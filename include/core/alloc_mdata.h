/**
 * @file alloc_mdata.h
 * @brief Allocation metadata structures.
 *
 * Common memory layout for managed memory and descriptor-backed static
 * objects. Includes inline headers, out-of-band headers, and allocation info
 * queries.
 */
#pragma once
#include "n00b.h"
#include "core/align.h"
#include "adt/option.h"

#define N00B_FORCED_ALIGNMENT N00B_ALIGN

typedef uint64_t n00b_alloc_type_info_t;

#define n00b_core_alloc_info_fields                                                            \
    n00b_alloc_type_info_t tinfo;                                                              \
    uint32_t               alloc_len;                                                          \
    uint32_t               ptr_words;                                                          \
    uint32_t               ptr_words_known : 1;                                                \
    uint32_t               is_array        : 1;                                                \
    uint32_t               no_scan         : 1;                                                \
    uint32_t               mem_debug       : 1;                                                \
    uint32_t               mem_debug_taint : 1;                                                \
    uint32_t               moved           : 1;                                                \
    /* Per-allocation GC scan shape; see core/gc_map.h.                                        \
     * 3 bits covers DEFAULT (0) through CALLBACK (4) with room. */                            \
    uint32_t               scan_kind       : 3;                                                \
    n00b_gc_scan_cb_t      scan_cb;                                                            \
    void                  *scan_user;                                                          \
    n00b_uint128_t         cached_hash

// The header is inline with allocations, and is used when we don't
// have a dynamic record, such as when unmarshaling or for static
// data.
struct n00b_inline_hdr_t {
    uint64_t guard;
    n00b_core_alloc_info_fields;
    alignas(N00B_FORCED_ALIGNMENT) uint64_t data[0];
};

// The dynamic record, which is kept out of band for safety, and thus
// is the prefered source of info when available.
struct n00b_oob_hdr_t {
    void *user_ptr;               // Pointer to the user-returned value.
    n00b_core_alloc_info_fields;  // Authoritative data.
    n00b_inline_hdr_t *hcur;      // Pointer to the guard / inline info.
    const char        *file_name; // file and line info.
};

// n00b_alloc_err means we couldn't find a record, but did find the allocator.
typedef enum {
    n00b_alloc_none,
    n00b_alloc_inline,
    n00b_alloc_oob,
    n00b_alloc_static_range,
    n00b_alloc_err,
} n00b_alloc_info_kind_t;

typedef struct n00b_alloc_info_t {
    n00b_alloc_info_kind_t kind;
    union {
        n00b_oob_hdr_t    *oob;
        n00b_inline_hdr_t *in_line;
        n00b_alloc_range_t *range;
    } hdr;
} n00b_alloc_info_t;

/**
 * @brief Extract the out-of-band header from an alloc info, if present.
 * @param info Allocation info to query.
 * @return     Optional OOB header pointer.
 */
static inline n00b_option_t(n00b_oob_hdr_t *)
n00b_alloc_info_oob(n00b_alloc_info_t info)
{
    if (info.kind != n00b_alloc_oob) {
        return n00b_option_none(n00b_oob_hdr_t *);
    }
    return n00b_option_set(n00b_oob_hdr_t *, info.hdr.oob);
}

/**
 * @brief Extract the inline header from an alloc info, if present.
 * @param info Allocation info to query.
 * @return     Optional inline header pointer.
 */
static inline n00b_option_t(n00b_inline_hdr_t *)
n00b_alloc_info_inline(n00b_alloc_info_t info)
{
    if (info.kind != n00b_alloc_inline) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }
    return n00b_option_set(n00b_inline_hdr_t *, info.hdr.in_line);
}

static inline bool
n00b_alloc_info_is_oob(n00b_alloc_info_t info)
{
    return info.kind == n00b_alloc_oob;
}

static inline bool
n00b_alloc_info_is_inline(n00b_alloc_info_t info)
{
    return info.kind == n00b_alloc_inline;
}

static inline bool
n00b_alloc_info_is_heap(n00b_alloc_info_t info)
{
    return info.kind == n00b_alloc_oob || info.kind == n00b_alloc_inline;
}

static inline bool
n00b_alloc_info_is_static_range(n00b_alloc_info_t info)
{
    return info.kind == n00b_alloc_static_range;
}

static inline n00b_option_t(n00b_alloc_range_t *)
n00b_alloc_info_static_range(n00b_alloc_info_t info)
{
    if (info.kind != n00b_alloc_static_range) {
        return n00b_option_none(n00b_alloc_range_t *);
    }
    return n00b_option_set(n00b_alloc_range_t *, info.hdr.range);
}

static inline bool
n00b_alloc_info_exists(n00b_alloc_info_t info)
{
    return info.kind != n00b_alloc_none;
}

#define N00B_ALLOC_HDR_SZ ((sizeof(n00b_inline_hdr_t) + N00B_ALIGN - 1) & ~(N00B_ALIGN - 1))
