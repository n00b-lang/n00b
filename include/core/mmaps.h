#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"

#include <sys/mman.h>
#include <assert.h>

#define N00B_MPROT (PROT_READ | PROT_WRITE)
#define N00B_MFLAG (MAP_PRIVATE | MAP_ANON)
#define N00B_ALIGN 16

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

enum n00b_mmap_perms_t : uint8_t {
    n00b_mmap_perms_data_not_addr = 0,
    n00b_mmap_perms_ro            = 1,
    n00b_mmap_perms_no_access     = 2,
    n00b_mmap_perms_rw            = 4,
};

typedef struct n00b_mmap_node_t   n00b_mmap_node_t;
typedef enum n00b_mmap_rec_kind_t n00b_mmap_rec_kind_t;
typedef struct n00b_mmap_pool_t   n00b_mmap_pool_t;
typedef struct n00b_mmap_free_t   n00b_mmap_free_t;
typedef enum n00b_mmap_perms_t    n00b_mmap_perms_t;

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
    char                       *file;
    n00b_mmap_rec_kind_t        kind;
};

struct n00b_mmap_free_t {
    _Atomic(n00b_mmap_free_t *) next;
};

struct n00b_mmap_pool_t {
    n00b_mmap_pool_t *prev_pool;
    n00b_mmap_node_t *next_node;
    n00b_mmap_node_t *end;
    n00b_mmap_node_t  nodes[];
};

struct n00b_mmap_ctx_t {
    _Atomic(n00b_mmap_pool_t *) initial_pool;
    _Atomic(n00b_mmap_pool_t *) current_pool;
    _Atomic(n00b_mmap_free_t *) free_nodes;
    n00b_mmap_node_t            root;
    _Atomic uint32_t            lock;
    uint32_t                    next_pool_sz; // In pages.
    uint32_t                    treap_seed;
};

extern void n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx);
extern void n00b_mmaps_destroy(n00b_mmap_ctx_t *ctx);

// Insert
extern n00b_mmap_node_t *n00b_mmaps_register_mem_map(n00b_mmap_ctx_t     *ctx,
                                                     void                *start,
                                                     uint64_t             blen,
                                                     uint64_t             binary_offset,
                                                     n00b_mmap_rec_kind_t kind,
                                                     bool                 definitely_unique);

extern n00b_mmap_node_t *n00b_mmaps_lookup(n00b_mmap_ctx_t *ctx, void *addr);

// Remove a segment by node.
extern void n00b_mmaps_remove(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node);

typedef struct n00b_mmap_node_t n00b_mmap_info_t;
extern n00b_mmap_ctx_t          n00b_global_mem_maps;

extern bool                 n00b_mmap_is_segment(n00b_mmap_info_t *map);
extern bool                 n00b_mmap_is_arena(n00b_mmap_info_t *map);
extern n00b_mmap_rec_kind_t n00b_mmap_get_kind(n00b_mmap_info_t *map);
extern n00b_mmap_info_t    *n00b_mmap_by_address(void *addr);
extern n00b_allocator_t    *n00b_mmap_get_allocator(n00b_mmap_info_t *);

static inline n00b_allocator_t *
n00b_mem_get_allocator(void *addr)
{
    return n00b_mmap_get_allocator(n00b_mmap_by_address(addr));
}

// These versions are wrappers that use n00b_global_mem_maps.
extern n00b_mmap_info_t *n00b_register_mmap(const void          *startp,
                                            const void          *endp,
                                            const char          *file,
                                            n00b_allocator_t    *allocator,
                                            uint64_t             binary_offset,
                                            intptr_t             slide,
                                            n00b_mmap_rec_kind_t kind,
                                            uint64_t             order_id,
                                            bool                 definitely_unique);

extern void n00b_unregister_mmap(void *start);
extern void n00b_init_memory_info(void);
extern void n00b_load_static_ranges(void);
extern void n00b_mmaps_slow_rm_arena_segments(n00b_arena_t *);

static inline void *
n00b_basic_mmap(size_t size)
{
    void *result = mmap(nullptr, size, N00B_MPROT, N00B_MFLAG, -1, 0);

    assert(result != MAP_FAILED);
    return result;
}

static inline bool
n00b_mmap_is_managed(n00b_mmap_info_t *map)
{
    if (!map) {
        return false;
    }
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
        return true;
    default:
        return false;
    }
}

extern void *
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_arena_t        *arena = nullptr;
    n00b_mmap_rec_kind_t kind  = n00b_mmap_api_mmap;
    char                *name  = nullptr;
};

extern void n00b_munmap(void *addr);
#define n00b_mmap(sz, ...) _n00b_mmap(sz, N00B_LOC_STRING() __VA_OPT__(, ) __VA_ARGS__)
