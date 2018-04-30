/*
 * Memory Pool implementation logic.
 */

#include "cache.h"
#include "mem-pool.h"

#define BLOCK_GROWTH_SIZE 1024*1024 - sizeof(struct mp_block);

static struct mp_block *mem_pool_alloc_block(struct mem_pool *mem_pool, size_t block_alloc)
{
	struct mp_block *p;

	mem_pool->pool_alloc += sizeof(struct mp_block) + block_alloc;
	p = xmalloc(st_add(sizeof(struct mp_block), block_alloc));
	p->next_block = mem_pool->mp_block;
	p->next_free = (char *)p->space;
	p->end = p->next_free + block_alloc;
	mem_pool->mp_block = p;

	return p;
}

static void *mem_pool_alloc_custom(struct mem_pool *mem_pool, size_t block_alloc)
{
	char *p;
	ALLOC_GROW(mem_pool->custom, mem_pool->nr + 1, mem_pool->alloc);
	ALLOC_GROW(mem_pool->custom_end, mem_pool->nr_end + 1, mem_pool->alloc_end);

	p = xmalloc(block_alloc);
	mem_pool->custom[mem_pool->nr++] = p;
	mem_pool->custom_end[mem_pool->nr_end++] = p + block_alloc;

	mem_pool->pool_alloc += block_alloc;

	return mem_pool->custom[mem_pool->nr];
}

void mem_pool_init(struct mem_pool **mem_pool, size_t initial_size)
{
	if (!(*mem_pool))
	{
		*mem_pool = xmalloc(sizeof(struct mem_pool));
		(*mem_pool)->pool_alloc = 0;
		(*mem_pool)->mp_block = NULL;
		(*mem_pool)->block_alloc = BLOCK_GROWTH_SIZE;
		(*mem_pool)->custom = NULL;
		(*mem_pool)->nr = 0;
		(*mem_pool)->alloc = 0;
		(*mem_pool)->custom_end = NULL;
		(*mem_pool)->nr_end = 0;
		(*mem_pool)->alloc_end = 0;

		if (initial_size > 0)
			mem_pool_alloc_block(*mem_pool, initial_size);
	}
}

void mem_pool_discard(struct mem_pool *mem_pool)
{
	int i;
	struct mp_block *block, *block_to_free;
	for (block = mem_pool->mp_block; block;)
	{
		block_to_free = block;
		block = block->next_block;
		free(block_to_free);
	}

	for (i = 0; i < mem_pool->nr; i++)
		free(mem_pool->custom[i]);

	free(mem_pool->custom);
	free(mem_pool->custom_end);
	free(mem_pool);
}

void *mem_pool_alloc(struct mem_pool *mem_pool, size_t len)
{
	struct mp_block *p;
	void *r;

	/* round up to a 'uintmax_t' alignment */
	if (len & (sizeof(uintmax_t) - 1))
		len += sizeof(uintmax_t) - (len & (sizeof(uintmax_t) - 1));

	for (p = mem_pool->mp_block; p; p = p->next_block)
		if (p->end - p->next_free >= len)
			break;

	if (!p) {
		if (len >= (mem_pool->block_alloc / 2))
			return mem_pool_alloc_custom(mem_pool, len);

		p = mem_pool_alloc_block(mem_pool, mem_pool->block_alloc);
	}

	r = p->next_free;
	p->next_free += len;
	return r;
}

void *mem_pool_calloc(struct mem_pool *mem_pool, size_t count, size_t size)
{
	size_t len = st_mult(count, size);
	void *r = mem_pool_alloc(mem_pool, len);
	memset(r, 0, len);
	return r;
}

int mem_pool_contains(struct mem_pool *mem_pool, void *mem)
{
	int i;
	struct mp_block *p;

	/* Check if memory is allocated in a block */
	for (p = mem_pool->mp_block; p; p = p->next_block)
		if ((mem >= ((void *)p->space)) &&
		    (mem < ((void *)p->end)))
			return 1;

	/* Check if memory is allocated in custom block */
	for (i = 0; i < mem_pool->nr; i++)
		if ((mem >= mem_pool->custom[i]) &&
		    (mem < mem_pool->custom_end[i]))
			return 1;

	return 0;
}

void mem_pool_combine(struct mem_pool *dst, struct mem_pool *src)
{
	int i;
	struct mp_block **tail = &dst->mp_block;

	/* Find pointer of dst's last block (if any) */
	while (*tail)
		tail = &(*tail)->next_block;

	/* Append the blocks from src to dst */
	*tail = src->mp_block;

	/* Combine custom allocations */
	ALLOC_GROW(dst->custom, dst->nr + src->nr, dst->alloc);
	ALLOC_GROW(dst->custom_end, dst->nr_end + src->nr_end, dst->alloc_end);

	for (i = 0; i < src->nr; i++) {
		dst->custom[dst->nr++] = src->custom[i];
		dst->custom_end[dst->nr_end++] = src->custom_end[i];
	}

	dst->pool_alloc += src->pool_alloc;
	src->pool_alloc = 0;
	src->mp_block = NULL;
	src->custom = NULL;
	src->nr = 0;
	src->alloc = 0;
	src->custom_end = NULL;
	src->nr_end = 0;
	src->alloc_end = 0;
}
