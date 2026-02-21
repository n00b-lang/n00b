/**
 * @file mmaps.h
 * @brief Memory-mapping registry and helpers.
 *
 * Wraps mmap/munmap with a global interval-tree registry that tracks
 * every mapped region, its kind, and its owning allocator.  Also
 * provides sub-range registration for individual allocations within
 * larger mappings.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "core/rt_access.h"
#include "core/option.h"
#include "core/result.h"
#include "core/alloc_mdata.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/macros.h"

#include <assert.h>

#if defined(_WIN32)
#include "n00b_windows_compat.h"
#else
#include <sys/mman.h>
#endif

#if defined(__has_include)
#if __has_include("n00b_build_config.h")
#include "n00b_build_config.h"
#endif
#endif

#if defined(_WIN32)
#define N00B_MPROT 0
#define N00B_MFLAG 0

static inline n00b_result_t(void *)
n00b_platform_map_anon(size_t sz)
{
    void *p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!p) {
        return n00b_result_err(void *, (int)GetLastError());
    }

    return n00b_result_ok(void *, p);
}

static inline n00b_result_t(int)
n00b_platform_unmap(void *addr, size_t size)
{
    (void)size;

    if (!VirtualFree(addr, 0, MEM_RELEASE)) {
        return n00b_result_err(int, (int)GetLastError());
    }

    return n00b_result_ok(int, 0);
}
#else
#define N00B_MPROT (PROT_READ | PROT_WRITE)

#if defined(N00B_HAVE_MAP_ANONYMOUS) && N00B_HAVE_MAP_ANONYMOUS
#define N00B_MAP_ANON_FLAG MAP_ANONYMOUS
#elif defined(N00B_HAVE_MAP_ANON) && N00B_HAVE_MAP_ANON
#define N00B_MAP_ANON_FLAG MAP_ANON
#elif defined(MAP_ANONYMOUS)
#define N00B_MAP_ANON_FLAG MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define N00B_MAP_ANON_FLAG MAP_ANON
#else
#error "No anonymous mmap flag was detected."
#endif

#define N00B_MFLAG (MAP_PRIVATE | N00B_MAP_ANON_FLAG)

static inline n00b_result_t(void *)
n00b_platform_map_anon(size_t sz)
{
    return n00b_check_mmap(nullptr, sz, N00B_MPROT, N00B_MFLAG, -1, 0);
}

static inline n00b_result_t(int)
n00b_platform_unmap(void *addr, size_t size)
{
    return munmap(addr, size) ? n00b_result_err(int, errno) : n00b_result_ok(int, 0);
}
#endif

enum n00b_mmap_perms_t : uint8_t {
    n00b_mmap_perms_data_not_addr = 0,
    n00b_mmap_perms_ro            = 1,
    n00b_mmap_perms_no_access     = 2,
    n00b_mmap_perms_rw            = 4,
};

typedef enum n00b_mmap_rec_kind_t n00b_mmap_rec_kind_t;
typedef enum n00b_mmap_perms_t    n00b_mmap_perms_t;

/**
 * @brief Look up an mmap record by address (internal — prefer n00b_mmap_by_address).
 * @param ctx  Mmap context to search.
 * @param addr Address to look up.
 * @return     Optional mmap info.
 */
extern n00b_option_t(n00b_mmap_info_t *) n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr);

/**
 * @brief Register an mmap'd region in the global registry.
 * @param startp Start address of the mapping.
 * @param endp   End address (exclusive).
 * @param kind   Kind of mapping (arena, pool, etc.).
 *
 * @kw runtime          Runtime whose mmap context to use.
 * @kw file             Debug file name for this mapping.
 * @kw allocator        Allocator owning the mapping.
 * @kw binary_offset    Offset within a mapped binary file.
 * @kw slide            ASLR slide for the mapping.
 * @kw order_id         Insertion order identifier.
 * @kw definitely_unique If true, skip duplicate checks on insert.
 */
extern n00b_option_t(n00b_mmap_info_t *)
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

/**
 * @brief Unregister an mmap'd region.
 * @param start Start address of the mapping to remove.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern void
n00b_mmap_unregister(void *start) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

/**
 * @brief Allocate memory via mmap.  Use the n00b_mmap() macro.
 * @param sz  Number of bytes to map.
 * @param loc Source location string (auto-filled by macro).
 *
 * @kw allocator Allocator to associate with the mapping.
 * @kw kind      Mmap record kind (default n00b_mmap_api_mmap).
 * @kw name      Debug name for the mapping.
 *
 * @pre @p sz > 0.
 * @post On success, the mapped region is registered in the global mmap registry.
 */
// clang-format off
extern n00b_result_t(void *)
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    n00b_mmap_rec_kind_t kind      = n00b_mmap_api_mmap;
    char                *name      = nullptr;
};
// clang-format on

#define n00b_mmap(sz, ...) _n00b_mmap(sz, N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Unmap a previously mmap'd region.
 * @param addr Start address to unmap.
 *
 * @kw runtime Runtime whose mmap context to use.
 *
 * @pre @p addr was returned by a prior n00b_mmap() call.
 * @post The mapping is removed from the global registry and memory is released.
 */
// clang-format off
extern n00b_result_t(int)
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};
// clang-format on

/**
 * @brief Check whether a mapping is an arena segment.
 * @param map Mmap info to check.
 * @return    true if managed or system segment.
 */
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

/**
 * @brief Check whether a mapping is an arena root (not a segment).
 * @param map Mmap info to check.
 * @return    true if arena root.
 */
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

/**
 * @brief Look up mmap info by address using the global registry.
 * @param addr Address to look up.
 * @return     Optional mmap info.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

typedef n00b_option_t(n00b_allocator_t *) n00b_allocator_opt_t;

/**
 * @brief Find the allocator owning an address.
 * @param addr Address to look up.
 * @return     Optional allocator pointer.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

/**
 * @brief Check whether a mapping is GC-managed.
 * @param map Mmap info to check.
 * @return    true if managed segment, system segment, or pool.
 */
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

/**
 * @brief Unmap a region, handling both registered and hidden pages.
 *
 * Tries n00b_munmap() first (removes the mmap record and unmaps).
 * If the region is not registered (e.g. hidden allocator pages),
 * falls back to raw munmap with the provided size.
 *
 * This is the canonical cleanup path for allocator implementations
 * that may operate in hidden mode.
 *
 * @param addr Address to unmap.
 * @param size Size of the mapping (used as fallback for hidden pages).
 */
static inline void
n00b_safe_munmap(void *addr, size_t size)
{
    auto r = n00b_munmap(addr);

    if (n00b_result_is_err(r)) {
        (void)n00b_platform_unmap(addr, size);
    }
}

/**
 * @brief Register a sub-range (individual allocation) within an existing mmap.
 * @param start Start address of the sub-range.
 * @param end   End address (exclusive) of the sub-range.
 * @param kind  Kind of sub-range.
 *
 * @kw allocator Allocator owning the sub-range.
 * @kw file      Debug file name.
 */
extern n00b_mmap_info_t *
n00b_mmap_register_range(void *start, void *end, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    const char       *file      = nullptr;
};

/**
 * @brief Delete all sub-ranges overlapping [start, end).
 * @param ctx   Mmap context.
 * @param start Start of the range to clear.
 * @param end   End of the range to clear (exclusive).
 */
extern void n00b_mmap_delete_ranges(n00b_mmap_ctx_t *ctx, uint64_t start, uint64_t end);

#define n00b_global_mem_map(rt) (&(rt->mmaps))
