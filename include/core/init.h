#pragma once

#if !defined(N00B_THREADS_MAX)
#define N00B_THREADS_MAX 4096
#endif

#include <assert.h>

#include "n00b.h"
#include "core/array.h"
#include "core/option.h"
#include "core/mmaps.h"

typedef struct n00b_runtime_t n00b_runtime_t;

n00b_array_decl(char *);
n00b_array_decl(n00b_thread_t *);
n00b_array_decl(uint32_t);
n00b_option_decl(n00b_runtime_t *);

struct n00b_gc_root_t {
    void    *ptr;
    uint32_t num_words;
    char    *location;
};

// 1st arg is the allocator object.
// 1st char * is the base type.
// 2nd char * is the location string.
// Both of these can be null.
// void * is an opaque pointer specific to the allocator; untyped.
typedef void *(*n00b_calloc_fn)(void *, size_t, size_t, const char *, const char *, void *);
typedef void (*n00b_free_fn)(void *);
typedef void (*n00b_allocator_destroy_fn)(n00b_allocator_t *);

struct n00b_base_allocator_t {
    n00b_calloc_fn            zero_alloc;
    n00b_free_fn              free;
    char                     *debug_name;
    n00b_allocator_destroy_fn destroy;
};

struct n00b_runtime_t {
    n00b_array_t(char *) argv;
    n00b_array_t(char *) envp;
    n00b_mmap_ctx_t             mmaps;
    _Atomic uint32_t            next_thread_slot;
    _Atomic uint32_t            live_threads;
    _Atomic bool                startup_complete;
    _Atomic(n00b_allocator_t *) default_allocator;
    // n00b_linked_list(n00b_gc_root_t) roots;
    _Atomic(n00b_thread_t *)    thread_list[N00B_THREADS_MAX];
    uint32_t                    thread_generations[N00B_THREADS_MAX];
    n00b_base_allocator_t       slab_allocator;
};

extern void
n00b_init(n00b_runtime_t *, int argc, char *argv[]) _kargs
{
    char       **envp           = nullptr;
    char        *numeric_locale = "";
    int          fd_limit       = 0; // Less than 0 = "don't set"
    unsigned int max_threads    = N00B_THREADS_MAX;
};

static inline n00b_runtime_t *
n00b_get_runtime(void)
{
    return n00b_option_get(default_runtime);
}

static inline n00b_allocator_t *
n00b_default_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    assert(rt->default_allocator);

    return rt->default_allocator;
}

static inline n00b_allocator_t *
n00b_slab_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    return &rt->slab_allocator;
}

extern uint64_t n00b_page_size;
