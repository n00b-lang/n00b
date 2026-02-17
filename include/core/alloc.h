#pragma once
#include <alloca.h>

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/macros.h"
#include "core/align.h"
#include "core/atomic.h"
#include "runtime.h"

struct n00b_allocator_t {
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
    void                     *opaque[];
};

typedef uint64_t (*n00b_obj_size_helper)(void *);

extern uint64_t n00b_gc_guard;

#define n00b_ensure_allocator(allocator_var)                                                   \
    if (!(allocator_var)) {                                                                    \
        allocator_var = n00b_atomic_load(&n00b_get_runtime()->default_allocator);              \
        assert(allocator_var);                                                                 \
    }

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

extern void n00b_free(void *ptr);
extern void n00b_allocator_destroy(n00b_allocator_t *allocator);

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

extern void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    char                     *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    // RISKY for custom allocators. Hides from GC.
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      __is_md_arena     = false;
};

static inline n00b_inline_hdr_t *
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

    n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)(uptr - N00B_ALLOC_HDR_SZ);

    if (hdr->guard == n00b_gc_guard) {
        return hdr;
    }

    return nullptr;
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

extern n00b_inline_hdr_opt_t n00b_object_header(void *p);
