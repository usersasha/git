#include "cache.h"
#include "builtin.h"
#include "parse-options.h"
#include "string-list.h"
#include "run-command.h"
#include "argv-array.h"
#include "hashmap.h"
#include "xdiff-interface.h"
#include "hungarian.h"
#include "diff.h"
#include "diffcore.h"
#include "commit.h"
#include "pretty.h"
#include "userdiff.h"

static const char * const builtin_branch_diff_usage[] = {
N_("git branch-diff [<options>] <old-base>..<old-tip> <new-base>..<new-tip>"),
N_("git branch-diff [<options>] <old-tip>...<new-tip>"),
N_("git branch-diff [<options>] <base> <old-tip> <new-tip>"),
NULL
};

static int parse_creation_weight(const struct option *opt, const char *arg,
				 int unset)
{
	double *d = opt->value;
	if (unset)
		*d = 0.6;
	else
		*d = atof(arg);
	return 0;
}

struct patch_util {
	/* For the search for an exact match */
	struct hashmap_entry e;
	const char *diff, *patch;

	int i, shown;
	int diffsize;
	size_t diff_offset;
	/* the index of the matching item in the other branch, or -1 */
	int matching;
	struct object_id oid;
};

/*
 * Reads the patches into a string list, with the `util` field being populated
 * as struct object_id (will need to be free()d).
 */
static int read_patches(const char *range, struct string_list *list)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	FILE *in;
	struct strbuf buf = STRBUF_INIT, line = STRBUF_INIT;
	struct patch_util *util = NULL;
	int in_header = 1;

	argv_array_pushl(&cp.args, "log", "--no-color", "-p", "--no-merges",
			"--reverse", "--date-order", "--decorate=no",
			"--no-abbrev-commit", range,
			NULL);
	cp.out = -1;
	cp.no_stdin = 1;
	cp.git_cmd = 1;

	if (start_command(&cp))
		return error_errno(_("could not start `log`"));
	in = fdopen(cp.out, "r");
	if (!in) {
		error_errno(_("could not read `log` output"));
		finish_command(&cp);
		return -1;
	}

	while (strbuf_getline(&line, in) != EOF) {
		const char *p;

		if (skip_prefix(line.buf, "commit ", &p)) {
			if (util) {
				string_list_append(list, buf.buf)->util = util;
				strbuf_reset(&buf);
			}
			util = xcalloc(sizeof(*util), 1);
			if (get_oid(p, &util->oid)) {
				error(_("could not parse commit '%s'"), p);
				free(util);
				string_list_clear(list, 1);
				strbuf_release(&buf);
				strbuf_release(&line);
				fclose(in);
				finish_command(&cp);
				return -1;
			}
			util->matching = -1;
			in_header = 1;
			continue;
		}

		if (starts_with(line.buf, "diff --git")) {
			in_header = 0;
			strbuf_addch(&buf, '\n');
			if (!util->diff_offset)
				util->diff_offset = buf.len;
			strbuf_addbuf(&buf, &line);
		} else if (in_header) {
			if (starts_with(line.buf, "Author: ")) {
				strbuf_addbuf(&buf, &line);
				strbuf_addstr(&buf, "\n\n");
			} else if (starts_with(line.buf, "    ")) {
				strbuf_rtrim(&line);
				strbuf_addbuf(&buf, &line);
				strbuf_addch(&buf, '\n');
			}
			continue;
		} else if (starts_with(line.buf, "@@ "))
			strbuf_addstr(&buf, "@@");
		else if (line.buf[0] && !starts_with(line.buf, "index "))
			/*
			 * A completely blank (not ' \n', which is context)
			 * line is not valid in a diff.  We skip it
			 * silently, because this neatly handles the blank
			 * separator line between commits in git-log
			 * output.
			 */
			strbuf_addbuf(&buf, &line);
		else
			continue;

		strbuf_addch(&buf, '\n');
		util->diffsize++;
	}
	fclose(in);
	strbuf_release(&line);

	if (util)
		string_list_append(list, buf.buf)->util = util;
	strbuf_release(&buf);

	if (finish_command(&cp))
		return -1;

	return 0;
}

static int patch_util_cmp(const void *dummy, const struct patch_util *a,
		     const struct patch_util *b, const char *keydata)
{
	return strcmp(a->diff, keydata ? keydata : b->diff);
}

static void find_exact_matches(struct string_list *a, struct string_list *b)
{
	struct hashmap map;
	int i;

	hashmap_init(&map, (hashmap_cmp_fn)patch_util_cmp, NULL, 0);

	/* First, add the patches of a to a hash map */
	for (i = 0; i < a->nr; i++) {
		struct patch_util *util = a->items[i].util;

		util->i = i;
		util->patch = a->items[i].string;
		util->diff = util->patch + util->diff_offset;
		hashmap_entry_init(util, strhash(util->diff));
		hashmap_add(&map, util);
	}

	/* Now try to find exact matches in b */
	for (i = 0; i < b->nr; i++) {
		struct patch_util *util = b->items[i].util, *other;

		util->i = i;
		util->patch = b->items[i].string;
		util->diff = util->patch + util->diff_offset;
		hashmap_entry_init(util, strhash(util->diff));
		other = hashmap_remove(&map, util, NULL);
		if (other) {
			if (other->matching >= 0)
				BUG("already assigned!");

			other->matching = i;
			util->matching = other->i;
		}
	}

	hashmap_free(&map, 0);
}

static void diffsize_consume(void *data, char *line, unsigned long len)
{
	(*(int *)data)++;
}

static int diffsize(const char *a, const char *b)
{
	xpparam_t pp = { 0 };
	xdemitconf_t cfg = { 0 };
	mmfile_t mf1, mf2;
	int count = 0;

	mf1.ptr = (char *)a;
	mf1.size = strlen(a);
	mf2.ptr = (char *)b;
	mf2.size = strlen(b);

	cfg.ctxlen = 3;
	if (!xdi_diff_outf(&mf1, &mf2, diffsize_consume, &count, &pp, &cfg))
		return count;

	error(_("failed to generate diff"));
	return INT_MAX;
}

static int get_correspondences(struct string_list *a, struct string_list *b,
			       double creation_weight)
{
	int n = a->nr + b->nr;
	double *cost = xmalloc(sizeof(double) * n * n), c;
	int *a2b = xmalloc(sizeof(int) * n), *b2a = xmalloc(sizeof(int) * n);
	int i, j, res;

	for (i = 0; i < a->nr; i++) {
		struct patch_util *a_util = a->items[i].util;

		for (j = 0; j < b->nr; j++) {
			struct patch_util *b_util = b->items[j].util;

			if (a_util->matching == j)
				c = 0;
			else if (a_util->matching < 0 && b_util->matching < 0)
				c = diffsize(a_util->diff, b_util->diff);
			else
				c = INT_MAX;
			cost[i + n * j] = c;
		}

		c = a_util->matching < 0 ?
			a_util->diffsize * creation_weight : INT_MAX;
		for (j = b->nr; j < n; j++)
			cost[i + n * j] = c;
	}

	for (j = 0; j < b->nr; j++) {
		struct patch_util *util = b->items[j].util;

		c = util->matching < 0 ?
			util->diffsize * creation_weight : INT_MAX;
		for (i = a->nr; i < n; i++)
			cost[i + n * j] = c;
	}

	for (i = a->nr; i < n; i++)
		for (j = b->nr; j < n; j++)
			cost[i + n * j] = 0;

	res = compute_assignment(n, n, cost, a2b, b2a);

	for (i = 0; i < a->nr; i++)
		if (a2b[i] >= 0 && a2b[i] < b->nr) {
			struct patch_util *a_util = a->items[i].util;
			struct patch_util *b_util = b->items[a2b[i]].util;

			a_util->matching = a2b[i];
			b_util->matching = i;
		}

	free(cost);
	free(a2b);
	free(b2a);

	return res;
}

static void output_pair_header(struct diff_options *diffopt, struct strbuf *buf,
			       int i, struct patch_util *a_util,
			       int j, struct patch_util *b_util)
{
	static char *dashes;
	struct object_id *oid = a_util ? &a_util->oid : &b_util->oid;
	struct commit *commit;
	char status;
	const char *color_reset = diff_get_color_opt(diffopt, DIFF_RESET);
	const char *color_old = diff_get_color_opt(diffopt, DIFF_FILE_OLD);
	const char *color_new = diff_get_color_opt(diffopt, DIFF_FILE_NEW);
	const char *color_commit = diff_get_color_opt(diffopt, DIFF_COMMIT);
	const char *color;

	if (!dashes) {
		char *p;

		dashes = xstrdup(find_unique_abbrev(oid, DEFAULT_ABBREV));
		for (p = dashes; *p; p++)
			*p = '-';
	}

	if (j < 0) {
		color = color_old;
		status = '<';
	} else if (i < 0) {
		color = color_new;
		status = '>';
	} else if (strcmp(a_util->patch, b_util->patch)) {
		color = color_commit;
		status = '!';
	} else {
		color = color_commit;
		status = '=';
	}

	strbuf_reset(buf);
	strbuf_addstr(buf, status == '!' ? color_old : color);
	if (i < 0)
		strbuf_addf(buf, "-:  %s ", dashes);
	else
		strbuf_addf(buf, "%d:  %s ", i + 1,
			    find_unique_abbrev(&a_util->oid, DEFAULT_ABBREV));

	if (status == '!')
		strbuf_addf(buf, "%s%s", color_reset, color);
	strbuf_addch(buf, status);
	if (status == '!')
		strbuf_addf(buf, "%s%s", color_reset, color_new);

	if (j < 0)
		strbuf_addf(buf, " -:  %s", dashes);
	else
		strbuf_addf(buf, " %d:  %s", j + 1,
			    find_unique_abbrev(&b_util->oid, DEFAULT_ABBREV));

	commit = lookup_commit_reference(oid);
	if (commit) {
		const char *commit_buffer = get_commit_buffer(commit, NULL);
		const char *subject;

		if (status == '!')
			strbuf_addf(buf, "%s%s", color_reset, color);

		find_commit_subject(commit_buffer, &subject);
		strbuf_addch(buf, ' ');
		format_subject(buf, subject, " ");
		unuse_commit_buffer(commit, commit_buffer);
	}
	strbuf_addf(buf, "%s\n", color_reset);

	fwrite(buf->buf, buf->len, 1, stdout);
}

static struct strbuf *output_prefix_cb(struct diff_options *opt, void *data)
{
	return data;
}

static struct userdiff_driver no_func_name = {
	.funcname = { "$^", 0 }
};

static struct diff_filespec *get_filespec(const char *name, const char *p)
{
	struct diff_filespec *spec = alloc_filespec(name);

	fill_filespec(spec, &null_oid, 0, 0644);
	spec->data = (char *)p;
	spec->size = strlen(p);
	spec->should_munmap = 0;
	spec->is_stdin = 1;
	spec->driver = &no_func_name;

	return spec;
}

static void patch_diff(const char *a, const char *b,
			      struct diff_options *diffopt)
{
	diff_queue(&diff_queued_diff,
		   get_filespec("a", a), get_filespec("b", b));

	diffcore_std(diffopt);
	diff_flush(diffopt);
}

static void output(struct string_list *a, struct string_list *b,
		   struct diff_options *diffopt)
{
	struct strbuf buf = STRBUF_INIT;
	int i = 0, j = 0;

	/*
	 * We assume the user is really more interested in the second argument
	 * ("newer" version). To that end, we print the output in the order of
	 * the RHS (the `b` parameter). To put the LHS (the `a` parameter)
	 * commits that are no longer in the RHS into a good place, we place
	 * them once we have shown all of their predecessors in the LHS.
	 */

	while (i < a->nr || j < b->nr) {
		struct patch_util *a_util, *b_util;
		a_util = i < a->nr ? a->items[i].util : NULL;
		b_util = j < b->nr ? b->items[j].util : NULL;

		/* Skip all the already-shown commits from the LHS. */
		while (i < a->nr && a_util->shown)
			a_util = ++i < a->nr ? a->items[i].util : NULL;

		/* Show unmatched LHS commit whose predecessors were shown. */
		if (i < a->nr && a_util->matching < 0) {
			output_pair_header(diffopt, &buf, i, a_util, -1, NULL);
			i++;
			continue;
		}

		/* Show unmatched RHS commits. */
		while (j < b->nr && b_util->matching < 0) {
			output_pair_header(diffopt, &buf, -1, NULL, j, b_util);
			b_util = ++j < b->nr ? b->items[j].util : NULL;
		}

		/* Show matching LHS/RHS pair. */
		if (j < b->nr) {
			a_util = a->items[b_util->matching].util;
			output_pair_header(diffopt, &buf,
					   b_util->matching, a_util, j, b_util);
			if (!(diffopt->output_format & DIFF_FORMAT_NO_OUTPUT))
				patch_diff(a->items[b_util->matching].string,
					   b->items[j].string, diffopt);
			a_util->shown = 1;
			j++;
		}
	}
	strbuf_release(&buf);
}

int cmd_branch_diff(int argc, const char **argv, const char *prefix)
{
	struct diff_options diffopt = { NULL };
	struct strbuf four_spaces = STRBUF_INIT;
	int dual_color = 0;
	double creation_weight = 0.6;
	struct option options[] = {
		OPT_BOOL(0, "dual-color", &dual_color,
			    N_("color both diff and diff-between-diffs")),
		OPT_SET_INT(0, "no-patches", &diffopt.output_format,
			    N_("short format (no diffs)"),
			    DIFF_FORMAT_NO_OUTPUT),
		{ OPTION_CALLBACK,
			0, "creation-weight", &creation_weight, N_("factor"),
			N_("Fudge factor by which creation is weighted [0.6]"),
			0, parse_creation_weight },
		OPT_END()
	};
	int i, j, res = 0;
	struct strbuf range1 = STRBUF_INIT, range2 = STRBUF_INIT;
	struct string_list branch1 = STRING_LIST_INIT_DUP;
	struct string_list branch2 = STRING_LIST_INIT_DUP;

	git_diff_basic_config("diff.color.frag", "magenta", NULL);

	diff_setup(&diffopt);
	diffopt.output_format = DIFF_FORMAT_PATCH;
	diffopt.flags.suppress_diff_headers = 1;
	diffopt.output_prefix = output_prefix_cb;
	strbuf_addstr(&four_spaces, "    ");
	diffopt.output_prefix_data = &four_spaces;

	argc = parse_options(argc, argv, NULL, options,
			builtin_branch_diff_usage, PARSE_OPT_KEEP_UNKNOWN);

	for (i = j = 0; i < argc; i++) {
		int c = diff_opt_parse(&diffopt, argv + i, argc - i, prefix);

		if (!c)
			argv[j++] = argv[i];
	}
	argc = j;
	diff_setup_done(&diffopt);

	if (dual_color) {
		diffopt.use_color = 1;
		diffopt.flags.dual_color_diffed_diffs = 1;
	}

	if (argc == 2) {
		if (!strstr(argv[0], ".."))
			warning(_("no .. in range: '%s'"), argv[0]);
		strbuf_addstr(&range1, argv[0]);

		if (!strstr(argv[1], ".."))
			warning(_("no .. in range: '%s'"), argv[1]);
		strbuf_addstr(&range2, argv[1]);
	} else if (argc == 3) {
		strbuf_addf(&range1, "%s..%s", argv[0], argv[1]);
		strbuf_addf(&range2, "%s..%s", argv[0], argv[2]);
	} else if (argc == 1) {
		const char *b = strstr(argv[0], "..."), *a = argv[0];
		int a_len;

		if (!b)
			die(_("single arg format requires a symmetric range"));

		a_len = (int)(b - a);
		if (!a_len) {
			a = "HEAD";
			a_len = strlen(a);
		}
		b += 3;
		if (!*b)
			b = "HEAD";
		strbuf_addf(&range1, "%s..%.*s", b, a_len, a);
		strbuf_addf(&range2, "%.*s..%s", a_len, a, b);
	} else {
		error(_("need two commit ranges"));
		usage_with_options(builtin_branch_diff_usage, options);
	}

	if (read_patches(range1.buf, &branch1))
		res = error(_("could not parse log for '%s'"), range1.buf);
	if (!res && read_patches(range2.buf, &branch2))
		res = error(_("could not parse log for '%s'"), range2.buf);

	if (!res) {
		find_exact_matches(&branch1, &branch2);
		res = get_correspondences(&branch1, &branch2, creation_weight);
		if (!res)
			output(&branch1, &branch2, &diffopt);
	}

	strbuf_release(&range1);
	strbuf_release(&range2);
	string_list_clear(&branch1, 1);
	string_list_clear(&branch2, 1);

	return !!res;
}
