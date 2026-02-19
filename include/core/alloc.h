/**
 * @file alloc.h
 * @brief Memory allocation interface.
 *
 * Provides the allocator vtable structure, raw allocation/free entry
 * points, allocation info lookup, allocator setup, and convenience
 * macros (n00b_alloc, n00b_alloc_array, n00b_alloc_flex).
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/macros.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/rt_access.h"

struct n00b_allocator_t {
    n00b_calloc_fn            zero_alloc;
    n00b_free_fn              free;
    n00b_allocator_destroy_fn destroy;
    const char               *debug_name;
    uint8_t                   add_inline_header : 1;
    uint8_t                   __system          : 1; // no STW check
    uint8_t                   __md_pool         : 1; // This IS a metadata pool.
    uint8_t                   hidden            : 1; // GC-invisible; see below.
    n00b_allocator_t         *metadata_pool;
    n00b_dict_untyped_t      *metadata;
    void                     *opaque[];
};

typedef uint64_t (*n00b_obj_size_helper)(void *);

/** @brief Magic guard word placed before every managed allocation. */
extern uint64_t n00b_gc_guard;

/**
 * @brief Check whether the GC should scan a mapped region.
 *
 * Returns false for allocator-internal memory that must not be
 * traced during garbage collection.  Hidden allocators' pages are
 * the primary case: they are never registered in the mmap tree
 * (so the GC normally cannot find them), but this function provides
 * a defence-in-depth check for any path that has an mmap record.
 *
 * This is the canonical GC-visibility gate for the allocator
 * abstraction.  Allocator authors only need to set `hidden = true`
 * in their allocator setup; the rest is automatic.
 *
 * @param map Mmap info to check (must not be nullptr).
 * @return    true if the region should be scanned by the GC.
 */
static inline bool
n00b_mmap_is_gc_scannable(n00b_mmap_info_t *map)
{
    if (map->allocator && map->allocator->hidden) {
        return false;
    }
    return true;
}

#define n00b_ensure_allocator(allocator_var)                                                   \
    if (!(allocator_var)) {                                                                    \
        allocator_var = n00b_atomic_load(&n00b_get_runtime()->default_allocator);              \
        assert(allocator_var);                                                                 \
    }

/**
 * @brief Low-level allocation.  Prefer the n00b_alloc() macro family.
 * @param n         Number of elements.
 * @param sz        Size of each element in bytes.
 * @param base_type Stringified C type name (for debugging).
 * @param location  Source location string (auto-filled by macro).
 *
 * @kw allocator   Allocator to use (nullptr = runtime default).
 * @kw aparams     Opaque allocator params for debug/cleanup marking.
 * @kw iparams     Opaque params to send to the object instance (TODO).
 * @kw no_scan     If true, GC will not scan this allocation for pointers.
 * @kw mem_debug   Enable memory debugging for this allocation.
 * @kw debug_taint Taint freed memory with a debug pattern.
 *
 * @pre Runtime must be initialized (or an explicit allocator must be provided).
 * @post Returned pointer is zero-filled and aligned to `N00B_ALIGN`.
 */
extern void *
_n00b_alloc_raw(size_t n, size_t sz, char *base_type, const char *location) _kargs
{
    n00b_allocator_t *allocator   = nullptr;
    void             *aparams     = nullptr; // Marking for debug, cleanup, etc.
    void             *iparams     = nullptr; // To be sent to an object instance (TODO)
    bool              no_scan     = false;
    bool              mem_debug   = false;
    bool              debug_taint = false;
};

/**
 * @brief Free a managed allocation.
 * @param ptr Pointer returned by a prior n00b_alloc call.
 * @pre @p ptr was returned by an n00b_alloc family macro, or is nullptr.
 */
extern void n00b_free(void *ptr);

/**
 * @brief Tear down an allocator, releasing all its resources.
 * @param allocator Allocator to destroy.
 * @pre No outstanding allocations should be in use from @p allocator.
 * @post @p allocator is invalid and must not be used.
 */
extern void n00b_allocator_destroy(n00b_allocator_t *allocator);

/**
 * @brief Look up allocation metadata for an address.
 * @param addr   Address to look up.
 * @param result Output structure to fill.
 *
 * @kw allocator       Allocator to search (nullptr = search all).
 * @kw scan_for_header If true, scan backward for an inline header.
 */
extern void
_n00b_find_alloc_info(void *addr, n00b_alloc_info_t *result) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    bool              scan_for_header = false;
};

// Get this in the caller's frame.
#define n00b_find_alloc_info(addr, ...)                                                        \
    ({                                                                                         \
        n00b_alloc_info_t _info;                                                               \
        _n00b_find_alloc_info((addr), &_info __VA_OPT__(, __VA_ARGS__));                       \
        _info;                                                                                 \
    })

/**
 * @brief Configure an allocator's vtable and options.
 * @param allocator Allocator to set up.
 * @param alloc     Zero-fill allocation function.
 *
 * @kw free              Free function (nullptr = no-op).
 * @kw destroy           Allocator destroy function (nullptr = no-op).
 * @kw name              Debug name for the allocator.
 * @kw inline_headers    Prepend inline headers to allocations.
 * @kw external_metadata Keep OOB metadata in a separate pool.
 * @kw hidden            Make GC-invisible. Pages allocated through this
 *                       allocator are not registered in the mmap tree,
 *                       so the GC can never discover or scan them.
 *                       Allocator cleanup must use n00b_safe_munmap()
 *                       (which falls back to raw munmap for unregistered
 *                       pages).  See n00b_mmap_is_gc_scannable().
 * @kw __nomap           Skip mmap registration (internal only).
 * @kw __system          Skip STW checks (internal only).
 * @kw __is_md_pool      Mark as a metadata pool (internal only).
 */
extern void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    char                     *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      __is_md_pool      = false;
};

/**
 * @brief Try to retrieve the inline allocation header for a pointer.
 * @param p Pointer to a managed allocation.
 * @return  Optional inline header (none if @p p is not managed).
 */
static inline n00b_inline_hdr_opt_t
n00b_inline_alloc_header(void *p)
{
    if (!p) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    uintptr_t uptr = (uintptr_t)p;

    if (uptr < N00B_ALLOC_HDR_SZ) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    if ((uptr & (N00B_ALIGN - 1)) != 0) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)(uptr - N00B_ALLOC_HDR_SZ);

    if (hdr->guard == n00b_gc_guard) {
        return n00b_option_set(n00b_inline_hdr_t *, hdr);
    }

    return n00b_option_none(n00b_inline_hdr_t *);
}

#define n00b_alloc_size(n, sz, ...)                                                            \
    _n00b_alloc_raw(n, sz, nullptr, N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))
#define n00b_alloc(T, ...)                                                                     \
    _n00b_alloc_raw(1,                                                                         \
                    sizeof(T),                                                                 \
                    N00B_TO_STRING(T),                                                         \
                    N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))
#define n00b_alloc_array(T, N, ...)                                                            \
    _n00b_alloc_raw((N),                                                                       \
                    sizeof(T),                                                                 \
                    N00B_TO_STRING(T),                                                         \
                    N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))
#define n00b_alloc_flex(T1, T2, N2, ...)                                                       \
    _n00b_alloc_raw(1,                                                                         \
                    (sizeof(T1) + sizeof(T2) * (N2)),                                          \
                    N00B_TO_STRING(T1),                                                        \
                    N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Get the inline header for a managed object.
 * @param p Pointer to a managed object.
 * @return  Optional inline header.
 */
extern n00b_inline_hdr_opt_t n00b_object_header(void *p);
