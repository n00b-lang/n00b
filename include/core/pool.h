#pragma once

#include "n00b.h"
#include "core/arena.h"
#include "core/stack.h"
#include "core/align.h"

#define N00B_POST_ROUND_SHIFT 6
#define N00B_NUM_FREE_LISTS   4

typedef struct n00b_pool_page_t {
    struct n00b_pool_page_t *prev;
    struct n00b_pool_page_t *next;
} n00b_pool_page_t;

typedef struct {
    unsigned int list_index;
} n00b_pool_entry_t;

static_assert(sizeof(n00b_pool_entry_t) <= N00B_ALIGN);

struct n00b_pool_t {
    n00b_base_allocator_t vtable;
    n00b_stack_t          free_lists[N00B_NUM_FREE_LISTS];
    n00b_pool_page_t     *page_table;
    _Atomic uint32_t      lock;
};

typedef struct n00b_pool_t n00b_pool_t;

extern n00b_allocator_t *
n00b_pool_init(n00b_pool_t *pool) _kargs
{
    uint32_t    headers           = true;
    bool        __system          = false;
    bool        inline_headers    = false;
    bool        external_metadata = false;
    bool        hidden            = false;
    const char *name              = "pool";
};
