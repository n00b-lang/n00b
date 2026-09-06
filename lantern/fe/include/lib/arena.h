#pragma once

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "lib/alloc.h"

#define NCC_ARENA_DEFAULT_BLOCK_SIZE (256 * 1024)
#define NCC_ARENA_ALIGNMENT          16

typedef struct ncc_arena_block_t {
    struct ncc_arena_block_t *next;
    size_t                    capacity;
    size_t                    used;
    alignas(NCC_ARENA_ALIGNMENT) char data[];
} ncc_arena_block_t;

typedef struct {
    ncc_arena_block_t *current;
    ncc_arena_block_t *first;
    size_t             default_block_size;
} ncc_arena_t;

static inline void
ncc_arena_init(ncc_arena_t *a, size_t block_size)
{
    *a = (ncc_arena_t){
        .default_block_size = block_size ? block_size
                                         : NCC_ARENA_DEFAULT_BLOCK_SIZE,
    };
}

static inline size_t
_ncc_arena_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

static inline void *
ncc_arena_alloc(ncc_arena_t *a, size_t size)
{
    size = _ncc_arena_align_up(size, NCC_ARENA_ALIGNMENT);

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

    ncc_arena_block_t *block = (ncc_arena_block_t *)ncc_alloc_size(
        1, sizeof(ncc_arena_block_t) + block_cap);

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

static inline void
ncc_arena_reset(ncc_arena_t *a)
{
    for (ncc_arena_block_t *b = a->first; b; b = b->next) {
        b->used = 0;
    }

    a->current = a->first;
}

static inline void
ncc_arena_free(ncc_arena_t *a)
{
    ncc_arena_block_t *b = a->first;

    while (b) {
        ncc_arena_block_t *next = b->next;
        ncc_free(b);
        b = next;
    }

    *a = (ncc_arena_t){0};
}
