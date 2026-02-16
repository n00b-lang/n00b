#pragma once

#include "n00b.h"
#include "runtime.h"

struct n00b_allocator_t {
    n00b_calloc_fn            zero_alloc;
    n00b_free_fn              free;
    n00b_allocator_destroy_fn destroy;
    const char               *debug_name;
    void                     *opaque[];
};

static inline void
n00b_allocator_destroy(n00b_allocator_t *allocator)
{
    (*allocator->destroy)(allocator);
}
