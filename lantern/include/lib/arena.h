#pragma once

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/alloc.h"

#define CORE_ARENA_DEFAULT_BLOCK_SIZE (256 * 1024)
#define CORE_ARENA_ALIGNMENT          16

typedef struct core_arena_block_t {
    struct core_arena_block_t *next;
    size_t                    capacity;
    size_t                    used;
    alignas(CORE_ARENA_ALIGNMENT) char data[];
} core_arena_block_t;

typedef struct {
    core_arena_block_t *current;
    core_arena_block_t *first;
    size_t             default_block_size;
} core_arena_t;

static inline void
core_arena_init(core_arena_t *a, size_t block_size)
{
    *a = (core_arena_t){
        .default_block_size = block_size ? block_size
                                         : CORE_ARENA_DEFAULT_BLOCK_SIZE,
    };
}

static inline size_t
_core_arena_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

static inline bool
core_arena_extend(core_arena_t *a, void *p, size_t oldsz, size_t newsz)
{
    core_arena_block_t *b = a->current;

    oldsz = _core_arena_align_up(oldsz, CORE_ARENA_ALIGNMENT);
    newsz = _core_arena_align_up(newsz, CORE_ARENA_ALIGNMENT);

    if (!b || !p || oldsz > b->used) {
        return false;
    }

    if ((char *)p + oldsz != b->data + b->used) {
        return false;
    }

    if (b->used - oldsz + newsz > b->capacity) {
        return false;
    }

    b->used = b->used - oldsz + newsz;

    memset((char *)p + oldsz, 0, newsz - oldsz);

    return true;
}

static inline void *
core_arena_alloc(core_arena_t *a, size_t size)
{
    size = _core_arena_align_up(size, CORE_ARENA_ALIGNMENT);

    if (a->current && a->current->used + size <= a->current->capacity) {
        void *ptr = a->current->data + a->current->used;
        a->current->used += size;
        return memset(ptr, 0, size);
    }

    if (a->current && a->current->next
        && a->current->next->capacity >= size) {
        a->current = a->current->next;
        a->current->used = size;
        return memset(a->current->data, 0, size);
    }

    size_t block_cap = a->default_block_size;
    if (size > block_cap) {
        block_cap = size;
    }

    core_arena_block_t *block = (core_arena_block_t *)core_alloc_size(
        1, sizeof(core_arena_block_t) + block_cap);

    block->capacity = block_cap;
    block->used     = size;
    block->next     = NULL;

    if (a->current) {
        block->next      = a->current->next;
        a->current->next = block;
    }
    else {
        a->first = block;
    }

    a->current = block;

    return memset(block->data, 0, size);
}

static inline bool
core_arena_try_reserve_raw(core_arena_t *arena, void **data,
                           size_t *capacity, size_t needed,
                           size_t element_size)
{
    if (needed <= *capacity) return true;
    if (!element_size) return false;
    size_t limit = (SIZE_MAX - sizeof(core_arena_block_t)
                    - (CORE_ARENA_ALIGNMENT - 1)) / element_size;
    if (needed > limit) return false;
    size_t next = *capacity ? *capacity : 1;
    while (next < needed) {
        if (next > limit / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }
    size_t old_bytes = *capacity * element_size;
    size_t new_bytes = next * element_size;
    if (!*data || !core_arena_extend(arena, *data, old_bytes, new_bytes)) {
        void *grown = core_arena_alloc(arena, new_bytes);
        if (!grown) return false;
        if (*data) memcpy(grown, *data, old_bytes);
        *data = grown;
    }
    *capacity = next;
    return true;
}

static inline void
core_arena_reset(core_arena_t *a)
{
    for (core_arena_block_t *b = a->first; b; b = b->next) {
        b->used = 0;
    }

    a->current = a->first;
}

typedef struct {
    core_arena_block_t *block;
    size_t             used;
} core_arena_mark_t;

static inline core_arena_mark_t
core_arena_mark(const core_arena_t *a)
{
    return (core_arena_mark_t){
        .block = a->current,
        .used  = a->current ? a->current->used : 0,
    };
}

static inline void
core_arena_rewind(core_arena_t *a, core_arena_mark_t mark)
{
    if (!mark.block) {
        core_arena_reset(a);
        return;
    }

    for (core_arena_block_t *b = mark.block->next; b; b = b->next) {
        b->used = 0;
    }

    mark.block->used = mark.used;
    a->current       = mark.block;
}

static inline void
core_arena_free(core_arena_t *a)
{
    core_arena_block_t *b = a->first;

    while (b) {
        core_arena_block_t *next = b->next;
        core_free(b);
        b = next;
    }

    *a = (core_arena_t){0};
}
