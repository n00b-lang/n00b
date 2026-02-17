#pragma once

#include "n00b.h"
#include "core/option.h"
#include "core/alloc_mdata.h"
#include "core/runtime.h"
#include "core/atomic.h"
#include "core/macros.h"

#include <sys/mman.h>
#include <assert.h>

#define N00B_MPROT (PROT_READ | PROT_WRITE)
#define N00B_MFLAG (MAP_PRIVATE | MAP_ANON)
#define N00B_ALIGN 16

enum n00b_mmap_perms_t : uint8_t {
    n00b_mmap_perms_data_not_addr = 0,
    n00b_mmap_perms_ro            = 1,
    n00b_mmap_perms_no_access     = 2,
    n00b_mmap_perms_rw            = 4,
};

typedef struct n00b_mmap_node_t   n00b_mmap_node_t;
typedef enum n00b_mmap_rec_kind_t n00b_mmap_rec_kind_t;
typedef struct n00b_mmap_free_t   n00b_mmap_free_t;
typedef enum n00b_mmap_perms_t    n00b_mmap_perms_t;

struct n00b_mmap_free_t {
    _Atomic(n00b_mmap_free_t *) next;
};

// Not internal for inlining, but please don't call this directly.
extern n00b_mmap_node_t *n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr);

extern n00b_mmap_info_t *
n00b_mmap_register(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_runtime_t   *runtime           = n00b_get_runtime();
    const char       *file              = nullptr;
    n00b_allocator_t *allocator         = nullptr;
    uint64_t          binary_offset     = 0;
    intptr_t          slide             = 0;
    uint64_t          order_id          = 0;
    bool              definitely_unique = true;
};

extern void
n00b_mmap_unregister(void *start) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

extern void *
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    n00b_mmap_rec_kind_t kind      = n00b_mmap_api_mmap;
    char                *name      = nullptr;
};

#define n00b_mmap(sz, ...) _n00b_mmap(sz, N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

extern void
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

n00b_option_decl(n00b_mmap_info_t *);

typedef n00b_option_t(n00b_mmap_info_t *) n00b_mmap_opt_t;

static inline bool
n00b_mmap_is_arena_segment(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
        return true;
    default:
        return false;
    }
}

static inline bool
n00b_mmap_is_arena(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_arena:
        return true;
    default:
        return false;
    }
}

static inline n00b_mmap_rec_kind_t
n00b_mmap_get_kind(n00b_mmap_info_t *map)
{
    return map->kind;
}

extern n00b_mmap_info_t *
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

typedef n00b_option_decl(n00b_allocator_t *) n00b_allocator_opt_t;

extern n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

// Used by the GC.
static inline bool
n00b_mmap_is_managed(n00b_mmap_info_t *map)
{
    if (!map) {
        return false;
    }
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
    case n00b_mmap_pool:
        return true;
    default:
        return false;
    }
}

#define n00b_global_mem_map(rt) (&(rt->mmaps))
