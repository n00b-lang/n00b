#pragma once
#include <alloca.h>

#include "n00b.h"
#include "core/allocator.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/macros.h"
#include "core/align.h"
#include "core/atomic.h"

typedef uint64_t (*n00b_obj_size_helper)(void *);

extern uint64_t n00b_gc_guard;

#define n00b_ensure_allocator(allocator_var)                                                   \
    if (!(allocator_var)) {                                                                    \
        allocator_var = n00b_atomic_load(&n00b_get_runtime()->default_allocator);              \
        assert(allocator_var);                                                                 \
    }
static inline void *
_n00b_alloc_raw(size_t n, size_t sz, const char *base_type, const char *location) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    void             *aparams   = nullptr; // Marking for debug, cleanup, etc.
    void             *iparams   = nullptr; // To be sent to an object instance (TODO)
}
{
    void *r;

    n00b_ensure_allocator(allocator);
    r = (*allocator->zero_alloc)(allocator, n, n00b_align(sz), base_type, location, aparams);
    // TODO: Add object info lookup, iff instance_params.

    return r;
}

static inline void
n00b_free(void *ptr)
{
    n00b_allocator_t *allocator = n00b_mem_get_allocator(ptr);
    assert(allocator);

    if (allocator->free) {
        (*allocator->free)(allocator, ptr);
    }
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

extern uint64_t n00b_gc_guard;

static inline n00b_alloc_info_t *
n00b_inline_alloc_header(void *p)
{
    if (!p) {
        return nullptr;
    }

    uintptr_t uptr = (uintptr_t)p;

    if (uptr < N00B_ALLOC_HDR_SZ) {
        return nullptr;
    }

    if ((uptr & (N00B_ALIGN - 1)) != 0) {
        return nullptr;
    }

    n00b_alloc_info_t *hdr = (n00b_alloc_info_t *)(uptr - N00B_ALLOC_HDR_SZ);

    if (hdr->guard == n00b_gc_guard) {
        return hdr;
    }

    return nullptr;
}

extern void *
n00b_find_alloc_info(void *addr) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    bool              scan_for_header = false;
};

static inline n00b_alloc_info_t *
n00b_get_object_header(void *p)
{
    return n00b_find_alloc_info(p, nullptr);
}

static inline n00b_alloc_info_t *
n00b_get_unsafe_alloc_header(n00b_alloc_info_t *h)
{
    // Given a header that may or may not be the inline one, get the
    // inline one, if available.
    if (!h || h->guard == n00b_gc_guard || h->guard == N00B_STATIC_MAGIC) {
        return h;
    }

    return ((n00b_alloc_metadata_t *)h)->hcur;
}

static inline bool
n00b_is_unsafe_alloc_header(n00b_alloc_info_t *h)
{
    if (!h || h->guard == n00b_gc_guard || h->guard == N00B_STATIC_MAGIC) {
        return true;
    }
    return false;
}

static inline n00b_alloc_info_t *
n00b_get_unsafe_header_for_addr(void *p)
{
    // Only return the header if it's inline.
    n00b_alloc_info_t *hdr = n00b_get_object_header(p);
    if (hdr) {
        hdr = n00b_get_unsafe_alloc_header(hdr);
        if (hdr->guard != n00b_gc_guard) {
            return nullptr;
        }
    }

    return hdr;
}
