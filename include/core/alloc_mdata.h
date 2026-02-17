// This is the common memory layout for managed memory, whether static
// or dynamic. It includes out-of-heap accounting.

#pragma once
#include "n00b.h"
#include "core/align.h"
#include "core/option.h"

#define N00B_FORCED_ALIGNMENT N00B_ALIGN

// We're going to use the C type for the type info, full stop.

typedef char *n00b_alloc_type_info_t;

#define n00b_core_alloc_info_fields                                                            \
    n00b_alloc_type_info_t tinfo;                                                              \
    uint32_t               alloc_len;                                                          \
    uint32_t               ptr_words       : 20;                                               \
    uint32_t               is_array        : 1;                                                \
    uint32_t               no_scan         : 1;                                                \
    uint32_t               mem_debug       : 1;                                                \
    uint32_t               mem_debug_taint : 1;                                                \
    uint32_t               moved           : 1;                                                \
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
    n00b_alloc_err,
} n00b_alloc_info_kind_t;

typedef struct n00b_alloc_info_t {
    n00b_alloc_info_kind_t kind;
    union {
        n00b_oob_hdr_t    *oob;
        n00b_inline_hdr_t *in_line;
    } hdr;
} n00b_alloc_info_t;

typedef n00b_option_decl(n00b_oob_hdr_t *) n00b_oob_hdr_opt_t;
typedef n00b_option_decl(n00b_inline_hdr_t *) n00b_inline_hdr_opt_t;

static inline n00b_oob_hdr_opt_t
n00b_alloc_info_oob(n00b_alloc_info_t info)
{
    if (info.kind != n00b_alloc_oob) {
        return n00b_option_none(n00b_oob_hdr_t *);
    }
    return n00b_option_set(n00b_oob_hdr_t *, info.hdr.oob);
}

static inline n00b_inline_hdr_opt_t
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
n00b_alloc_info_exists(n00b_alloc_info_t info)
{
    return info.kind != n00b_alloc_none;
}

// "\xcc400b1e\xcc" on little endian machines (byte swapped on on big endian)
#define N00B_STATIC_MAGIC 0xcc653162303034ccUL

struct n00b_static_header_t {
    // We need to have the static magic be the same as starting from the guard
    // of n00b_inline_hdr_t, so there will generally be an extra word of
    // padding after alloc_loc, since we align to 16 byte boundaries.
    char    *alloc_loc;
    void    *padding;
    uint64_t static_magic;
    n00b_core_alloc_info_fields;
    char data[0];
};

#define N00B_ALLOC_HDR_SZ ((sizeof(n00b_inline_hdr_t) + N00B_ALIGN - 1) & ~(N00B_ALIGN - 1))

#define N00B_STATIC_BASE(name, c_type, stored_type, n00b_type_val, ...)                        \
    struct {                                                                                   \
        n00b_static_header_t hdr;                                                              \
        c_type               obj;                                                              \
    } __##name = {                                                         \
        .hdr = {                                                           \
            .alloc_loc       = N00B_LOC_STRING(),                          \
            .static_magic    = N00B_STATIC_MAGIC,                          \
            .tinfo.n00b_type = stored_type,                                \
            .alloc_len       = sizeof(n00b_inline_hdr_t) + sizeof(c_type), \
            .ptr_words       = 0,                                          \
            .n00b_type       = n00b_type_val,                              \
            .no_scan         = false,                                      \
            .mem_debug       = false,                                      \
            .mem_debug_taint = false,                                      \
            .moved           = false,                                      \
            .cached_hash     = 0,                                          \
        },                                                                 \
        .obj = (c_type)__VA_ARGS__};                   \
    c_type *const name = &__##name.obj

#define N00B_STR_DECL(name, value)                                                             \
    N00B_STATIC_BASE(name,                                                                     \
                     n00b_string_t,                                                            \
                     N00B_T_STRING,                                                            \
                     true,                                                                     \
                     {                                                                         \
                         .data       = (value),                                                \
                         .u32_data   = nullptr,                                                \
                         .styling    = nullptr,                                                \
                         .codepoints = (sizeof(value) - 1),                                    \
                         .u8_bytes   = (sizeof(value) - 1),                                    \
                     })
