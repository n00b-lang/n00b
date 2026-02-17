#pragma once

#if !defined(N00B_THREADS_MAX)
#define N00B_THREADS_MAX 4096
#endif

#include <assert.h>

#include "n00b.h"
#include "core/array.h"
#include "core/option.h"

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

// void * is an opaque pointer specific to the allocator; untyped.
typedef void *(*n00b_calloc_fn)(n00b_allocator_t *, size_t, void *);
typedef void (*n00b_free_fn)(n00b_allocator_t *, void *);
typedef void (*n00b_allocator_destroy_fn)(n00b_allocator_t *);

struct n00b_base_allocator_t {
    n00b_calloc_fn            zero_alloc;
    n00b_free_fn              free;
    n00b_allocator_destroy_fn destroy;
    const char               *debug_name;
    uint8_t                   add_inline_header : 1;
    uint8_t                   __system          : 1; // no STW check
    uint8_t                   __md_arena        : 1; // Special for metadata arenas.
    uint8_t                   hidden            : 1; // GC must consider it data.
    n00b_allocator_t         *metadata_arena;
    n00b_dict_untyped_t      *metadata;
};

// Need to forward declare the mmaps data structures to be in the runtime.
typedef struct n00b_mmap_node_t   n00b_mmap_node_t;
typedef enum n00b_mmap_rec_kind_t n00b_mmap_rec_kind_t;

enum n00b_mmap_rec_kind_t {
    n00b_mmap_root            = 0,
    n00b_mmap_static          = 1,
    n00b_mmap_arena           = 2,
    n00b_mmap_managed_segment = 4,
    n00b_mmap_sys_segment     = 8,
    n00b_mmap_zero_page       = 16,
    n00b_mmap_unmanaged       = 32,
    n00b_mmap_stack           = 64,
    n00b_mmap_internal        = 128,
    n00b_mmap_pool            = 256,
    n00b_mmap_api_mmap        = 512,
    n00b_mmap_type_mask       = 1023,
};

struct n00b_mmap_node_t {
    uint64_t                    subtree_min;
    uint64_t                    subtree_max;
    uint64_t                    start;
    uint64_t                    end;
    uint64_t                    binary_offset;
    uint64_t                    order_id;
    uint32_t                    priority;
    _Atomic(n00b_mmap_node_t *) left;
    _Atomic(n00b_mmap_node_t *) right;
    _Atomic(n00b_mmap_node_t *) parent;
    _Atomic(n00b_allocator_t *) allocator;
    intptr_t                    slide; // The slide for ASLR
    const char                 *file;
    n00b_mmap_rec_kind_t        kind;
};

#include "core/pool.h"

struct n00b_mmap_ctx_t {
    n00b_mmap_node_t root;
    _Atomic int64_t  tid_lock;
    uint32_t         treap_seed;
    n00b_pool_t      pool;
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

extern n00b_option_t(n00b_runtime_t *) n00b_default_runtime;

static inline n00b_runtime_t *
n00b_get_runtime(void)
{
    return n00b_option_get(n00b_default_runtime);
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
    return (n00b_allocator_t *)&rt->slab_allocator;
}
