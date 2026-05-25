#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/data_lock.h"
#include "core/arena.h"
#include "core/static_image.h"
#include "util/defer.h"

// ============================================================================
// Hex map for encoding
// ============================================================================

const char n00b_hex_map_lower[16] = {
    '0',
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    'a',
    'b',
    'c',
    'd',
    'e',
    'f',
};

// ============================================================================
// Internal helpers
// ============================================================================

/*
 * Create a new buffer with the given byte length, using the specified
 * allocator (or runtime default if nullptr).
 */
static n00b_buffer_t *
_buffer_create(int64_t length, n00b_allocator_t *allocator)
{
    n00b_buffer_t *buf = n00b_alloc_with_opts(n00b_buffer_t, &(n00b_alloc_opts_t){.allocator = allocator});

    n00b_buffer_init(buf, .length = length, .allocator = allocator);
    return buf;
}

static bool
static_arg_is_named(const n00b_static_init_arg_t *arg, const char *name)
{
    return arg->name && strcmp(arg->name, name) == 0;
}

static void
static_image_append_bytes(n00b_static_image_builder_t *builder,
                          const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i != 0) {
            n00b_static_image_builder_append(builder, ",");
        }
        n00b_static_image_builder_append(builder, "0x%02x",
                                         (unsigned int)data[i]);
    }
}

static uint64_t
static_image_pow2_capacity(uint64_t len)
{
    if (len == 0) {
        return N00B_EMPTY_BUFFER_ALLOC;
    }

    uint64_t cap = 1;
    while (cap < len) {
        cap <<= 1;
    }
    return cap;
}

static uint64_t
static_image_hash_cstr(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static char *
static_image_c_string_literal(const char *s)
{
    size_t cap = 2;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '\\':
        case '"':
        case '\n':
        case '\r':
        case '\t':
            cap += 2;
            break;
        default:
            cap += (*p >= 0x20 && *p < 0x7f) ? 1 : 4;
            break;
        }
    }

    char *out = malloc(cap + 1);
    if (!out) {
        return nullptr;
    }

    char *w = out;
    *w++ = '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '\\':
            *w++ = '\\';
            *w++ = '\\';
            break;
        case '"':
            *w++ = '\\';
            *w++ = '"';
            break;
        case '\n':
            *w++ = '\\';
            *w++ = 'n';
            break;
        case '\r':
            *w++ = '\\';
            *w++ = 'r';
            break;
        case '\t':
            *w++ = '\\';
            *w++ = 't';
            break;
        default:
            if (*p >= 0x20 && *p < 0x7f) {
                *w++ = (char)*p;
            }
            else {
                sprintf(w, "\\%03o", (unsigned)*p);
                w += 4;
            }
            break;
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

static int
hex_digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static n00b_static_image_status_t
decode_hex_arg(n00b_static_image_builder_t *builder,
               const n00b_static_init_arg_t *arg,
               unsigned char **out,
               uint64_t *out_len)
{
    if (arg->bytes.len % 2 != 0) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "buffer .hex static initializer requires an even number of hex digits");
    }

    uint64_t len = arg->bytes.len / 2;
    unsigned char *data = calloc((size_t)(len ? len : 1), 1);
    if (!data) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_INITIALIZER,
            "out of memory while decoding buffer .hex argument");
    }

    const unsigned char *src = arg->bytes.data;
    for (uint64_t i = 0; i < len; i++) {
        int hi = hex_digit_value(src[i * 2]);
        int lo = hex_digit_value(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(data);
            return n00b_static_image_builder_fail(
                builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
                "buffer .hex static initializer contains a non-hex digit");
        }
        data[i] = (unsigned char)((hi << 4) | lo);
    }

    *out     = data;
    *out_len = len;
    return N00B_STATIC_IMAGE_OK;
}

n00b_static_image_status_t
n00b_buffer_static_init(n00b_static_image_builder_t *builder)
{
    const n00b_static_image_request_t *request = builder->request;

    if (!request->symbol_prefix || !request->symbol_prefix[0]) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "n00b_buffer_t static initializer requires a symbol prefix");
    }

    if ((request->object_flags & N00B_STATIC_OBJECT_F_READONLY) == 0) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY,
            "n00b_buffer_t static images are currently readonly only");
    }

    const unsigned char *raw = nullptr;
    uint64_t raw_len = 0;
    unsigned char *hex_decoded = nullptr;
    uint64_t hex_decoded_len = 0;
    bool have_raw = false;
    bool have_hex = false;
    bool have_length = false;
    uint64_t length = 0;
    // WP-011 Phase 3c.iii (D-066 buffer realization): dict-key-position
    // buffer literals carry a precomputed XXH3_128bits of the buffer's
    // payload bytes — the same value `n00b_buffer_hash` would compute
    // at runtime — split into a low/high uint64 pair on the request
    // wire (signed int64 underneath, see ncc's
    // `build_buffer_literal_helper_request` for the bit-pattern
    // reinterpretation contract).  When both halves are zero we leave
    // the obj descriptor's `.cached_hash` slot at zero so non-key
    // buffer literals stay bit-identical to pre-3c.iii behavior.
    uint64_t cached_hash_lo = 0;
    uint64_t cached_hash_hi = 0;

    for (uint64_t i = 0; i < request->arg_count; i++) {
        const n00b_static_init_arg_t *arg = &request->args[i];
        bool positional = arg->name == nullptr;

        if ((positional || static_arg_is_named(arg, "raw"))
            && arg->kind == N00B_STATIC_INIT_ARG_BYTES) {
            if (have_raw || have_hex) {
                return n00b_static_image_builder_fail(
                    builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
                    "buffer static initializer accepts only one raw or hex payload");
            }
            have_raw = true;
            raw = arg->bytes.data;
            raw_len = arg->bytes.len;
            continue;
        }

        if (static_arg_is_named(arg, "hex")
            && arg->kind == N00B_STATIC_INIT_ARG_BYTES) {
            if (have_raw || have_hex) {
                return n00b_static_image_builder_fail(
                    builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
                    "buffer static initializer accepts only one raw or hex payload");
            }
            n00b_static_image_status_t status = decode_hex_arg(
                builder, arg, &hex_decoded, &hex_decoded_len);
            if (status != N00B_STATIC_IMAGE_OK) {
                return status;
            }
            have_hex = true;
            raw = hex_decoded;
            raw_len = hex_decoded_len;
            continue;
        }

        if (static_arg_is_named(arg, "length")
            && arg->kind == N00B_STATIC_INIT_ARG_INT) {
            if (arg->integer < 0) {
                return n00b_static_image_builder_fail(
                    builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
                    "buffer .length static initializer must be non-negative");
            }
            have_length = true;
            length = (uint64_t)arg->integer;
            continue;
        }

        if (static_arg_is_named(arg, "no_lock")
            && arg->kind == N00B_STATIC_INIT_ARG_BOOL) {
            continue;
        }

        // WP-011 Phase 3c.iii: dict-key-position cached_hash inputs.
        // The request wire transmits int64 (signed); we reinterpret
        // the bit pattern as uint64 here.  ncc emits both halves
        // only when the precomputed XXH3_128bits is non-zero.
        if (static_arg_is_named(arg, "cached_hash_lo")
            && arg->kind == N00B_STATIC_INIT_ARG_INT) {
            cached_hash_lo = (uint64_t)arg->integer;
            continue;
        }

        if (static_arg_is_named(arg, "cached_hash_hi")
            && arg->kind == N00B_STATIC_INIT_ARG_INT) {
            cached_hash_hi = (uint64_t)arg->integer;
            continue;
        }

        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "unsupported n00b_buffer_t static initializer argument");
    }

    if (!have_raw && !have_hex) {
        raw_len = have_length ? length : 0;
    }
    else if (have_length && length != raw_len) {
        free(hex_decoded);
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "buffer .length must match the static raw/hex payload length");
    }

    uint64_t byte_len  = raw_len;
    uint64_t alloc_len = static_image_pow2_capacity(byte_len);
    uint64_t payload_len = alloc_len;
    unsigned char *payload = calloc((size_t)payload_len, 1);
    if (!payload) {
        free(hex_decoded);
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_INITIALIZER,
            "out of memory while building buffer static image");
    }
    if (raw && raw_len) {
        memcpy(payload, raw, (size_t)raw_len);
    }

    const char *prefix     = request->symbol_prefix;
    const char *entry_attr = request->entry_attr ? request->entry_attr : "";
    bool have_identity = request->identity_namespace
                      && request->identity_namespace[0]
                      && request->identity_object_key
                      && request->identity_object_key[0]
                      && request->identity_payload_key
                      && request->identity_payload_key[0];
    char *identity_namespace_lit = have_identity
                                 ? static_image_c_string_literal(
                                       request->identity_namespace)
                                 : nullptr;
    char *identity_object_key_lit = have_identity
                                  ? static_image_c_string_literal(
                                        request->identity_object_key)
                                  : nullptr;
    char *identity_payload_key_lit = have_identity
                                   ? static_image_c_string_literal(
                                         request->identity_payload_key)
                                   : nullptr;
    if (have_identity
        && (!identity_namespace_lit || !identity_object_key_lit
            || !identity_payload_key_lit)) {
        free(payload);
        free(hex_decoded);
        free(identity_namespace_lit);
        free(identity_object_key_lit);
        free(identity_payload_key_lit);
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_INITIALIZER,
            "out of memory while building buffer static image identities");
    }

    char payload_identity_ref[128];
    char object_identity_ref[128];
    snprintf(payload_identity_ref, sizeof(payload_identity_ref),
             "&%s_data_id", prefix);
    snprintf(object_identity_ref, sizeof(object_identity_ref),
             "&%s_obj_id", prefix);
    uint64_t payload_id = static_image_hash_cstr(prefix) ^ UINT64_C(0x7061796c6f6164);
    uint64_t object_id  = static_image_hash_cstr(prefix) ^ UINT64_C(0x627566666572);
    unsigned contract_version = (unsigned)N00B_STATIC_IMAGE_CONTRACT_VERSION;
    unsigned target_endian    = (unsigned)request->target_abi.endian;
    unsigned borrowed_flags   = (unsigned)N00B_BUF_F_BORROWED;

    n00b_static_image_builder_set_expr(builder, "&%s_obj", prefix);

    if (have_identity) {
        n00b_static_image_builder_append(
            builder,
            "static const n00b_static_identity_t %s_data_id={"
            ".version=1u,"
            ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_PAYLOAD,"
            ".namespace_id=%s,.object_key=%s};"
            "static const n00b_static_identity_t %s_obj_id={"
            ".version=1u,"
            ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_OBJECT,"
            ".namespace_id=%s,.object_key=%s};",
            prefix, identity_namespace_lit, identity_payload_key_lit,
            prefix, identity_namespace_lit, identity_object_key_lit);
    }

    n00b_static_image_builder_append(
        builder,
        "static const unsigned char %s_data[]={",
        prefix);
    static_image_append_bytes(builder, payload, (size_t)payload_len);
    n00b_static_image_builder_append(
        builder,
        "};"
        "static const n00b_static_object_desc_t %s_data_desc={"
        ".start=(const void*)%s_data,"
        ".len=(uint64_t)sizeof(%s_data),"
        ".tinfo=0,.scan_kind=N00B_GC_SCAN_KIND_NONE,"
        ".scan_cb=nullptr,.scan_user=nullptr,"
        ".object_id=%lluULL,.file=__FILE__,"
        ".identity=%s,"
        ".flags=N00B_STATIC_OBJECT_F_READONLY};"
        "static const n00b_static_object_desc_t * const %s_data_entry %s=&%s_data_desc;"
        "static const n00b_static_image_request_t %s_request={"
        ".version=%u,"
        ".type_hash=%lluULL,.type_name=\"n00b_buffer_t\","
        ".symbol_prefix=\"%s\",.entry_attr=\"\","
        ".payload_kind=N00B_STATIC_IMAGE_PAYLOAD_BYTES,"
        ".payload=(const void*)%s_data,.payload_len=(uint64_t)%lluULL,"
        ".args=nullptr,.arg_count=0,"
        ".target_abi={.version=%u,"
        ".pointer_bytes=(uint8_t)sizeof(void*),"
        ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,"
        ".endian=%u},"
        ".object_flags=N00B_STATIC_OBJECT_F_READONLY,"
        ".required_scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
        ".identity_namespace=%s,"
        ".identity_object_key=%s,"
        ".identity_payload_key=%s};"
        "_Static_assert((__builtin_offsetof(n00b_buffer_t,data)%%sizeof(void*))==0,"
        "\"buffer data pointer must be pointer-aligned\");"
        "_Static_assert((sizeof(n00b_buffer_t)%%sizeof(void*))==0,"
        "\"buffer static image object must be word-sized\");"
        "static const uint64_t %s_offsets[]={"
        "__builtin_offsetof(n00b_buffer_t,data)/sizeof(void*)};"
        "static n00b_gc_struct_layout_t %s_shape={"
        ".stride=(sizeof(n00b_buffer_t)/sizeof(void*)),.count=1,"
        ".offset_count=1,.offsets=%s_offsets};"
        "static const n00b_buffer_t %s_obj={"
        ".data=(char*)%s_data,.byte_len=%lluULL,.alloc_len=%lluULL,"
        ".lock=nullptr,.allocator=nullptr,.flags=%u,"
        ".scan_kind=N00B_GC_SCAN_KIND_NONE,.scan_cb=nullptr,.scan_user=nullptr};"
        "static const n00b_static_object_desc_t %s_obj_desc={"
        ".start=(const void*)&%s_obj,.len=(uint64_t)sizeof(%s_obj),"
        ".tinfo=%lluULL,.scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
        ".scan_cb=n00b_gc_scan_cb_struct_layout,.scan_user=&%s_shape,"
        ".object_id=%lluULL,.file=__FILE__,"
        ".identity=%s,"
        ".flags=N00B_STATIC_OBJECT_F_READONLY,"
        // WP-011 Phase 3c.iii: dict-key-position buffers get the
        // XXH3_128bits of their payload threaded through as a
        // 128-bit literal; the runtime probe loop in
        // n00b_register_static_object pre-populates the alloc
        // range's cached_hash so the first lookup short-circuits
        // n00b_buffer_hash.  Non-dict-key buffer call sites
        // default both halves to 0 and the slot stays zero.
        ".cached_hash=(((n00b_uint128_t)0x%016llxULL<<64)"
        "|(n00b_uint128_t)0x%016llxULL)};"
        "static const n00b_static_object_desc_t * const %s_obj_entry %s=&%s_obj_desc;"
        "static const n00b_static_image_dependency_t %s_deps[]={"
        "{.desc=&%s_data_desc,.relocation_offset=__builtin_offsetof(n00b_buffer_t,data),"
        ".role=\"payload\"}};"
        "static const n00b_static_image_response_t %s_response __attribute__((used))={"
        ".version=%u,.request=&%s_request,"
        ".object_start=(const void*)&%s_obj,.object_len=(uint64_t)sizeof(%s_obj),"
        ".scan_kind=N00B_GC_SCAN_KIND_CALLBACK,.scan_cb=n00b_gc_scan_cb_struct_layout,"
        ".scan_user=&%s_shape,.dependencies=%s_deps,.dependency_count=1};",
        prefix, prefix, prefix,
        (unsigned long long)payload_id,
        have_identity ? payload_identity_ref : "nullptr",
        prefix, entry_attr, prefix,
        prefix, contract_version,
        (unsigned long long)request->type_hash, prefix,
        prefix, (unsigned long long)byte_len,
        contract_version, target_endian,
        have_identity ? identity_namespace_lit : "nullptr",
        have_identity ? identity_object_key_lit : "nullptr",
        have_identity ? identity_payload_key_lit : "nullptr",
        prefix, prefix, prefix,
        prefix, prefix, (unsigned long long)byte_len,
        (unsigned long long)alloc_len, borrowed_flags,
        prefix, prefix, prefix, (unsigned long long)request->type_hash,
        prefix, (unsigned long long)object_id,
        have_identity ? object_identity_ref : "nullptr",
        // WP-011 Phase 3c.iii: cached_hash high/low halves for the
        // obj descriptor's `.cached_hash` slot.  Both default to 0
        // for non-dict-key buffer literals.
        (unsigned long long)cached_hash_hi,
        (unsigned long long)cached_hash_lo,
        prefix, entry_attr, prefix,
        prefix, prefix,
        prefix, contract_version,
        prefix, prefix, prefix, prefix, prefix, prefix);

    free(payload);
    free(hex_decoded);
    free(identity_namespace_lit);
    free(identity_object_key_lit);
    free(identity_payload_key_lit);
    return N00B_STATIC_IMAGE_OK;
}

// ============================================================================
// Construction
// ============================================================================

void
n00b_buffer_init(n00b_buffer_t *obj) _kargs
{
    int64_t              length    = -1;
    char                *raw       = nullptr;
    n00b_string_t       *hex       = nullptr;
    char                *ptr       = nullptr;
    n00b_allocator_t    *allocator = nullptr;
    bool                 no_lock   = false;
    n00b_gc_scan_kind_t  scan_kind = N00B_GC_SCAN_KIND_NONE;
    n00b_gc_scan_cb_t    scan_cb   = nullptr;
    void                *scan_user = nullptr;
}
{
    obj->lock      = no_lock ? nullptr : n00b_data_lock_new();
    obj->allocator = allocator;
    obj->scan_kind = scan_kind;
    obj->scan_cb   = scan_cb;
    obj->scan_user = scan_user;

    if (raw == nullptr && hex == nullptr && ptr == nullptr) {
        if (length < 0) {
            return;
        }
    }

    if (length < 0) {
        if (hex == nullptr) {
            return;
        }
        else {
            length = hex->u8_bytes >> 1;
        }
    }

    if (length == 0) {
        obj->alloc_len = N00B_EMPTY_BUFFER_ALLOC;
        obj->data      = n00b_alloc_array_with_opts(char, obj->alloc_len,
                                                    &(n00b_alloc_opts_t){
                                                        .allocator = obj->allocator,
                                                        .scan_kind = obj->scan_kind,
                                                        .scan_cb   = obj->scan_cb,
                                                        .scan_user = obj->scan_user,
                                                    });
        obj->byte_len  = 0;
        return;
    }

    if (length > 0 && ptr == nullptr) {
        int64_t alloc_len = n00b_align_closest_pow2_ceil(length);

        obj->data      = n00b_alloc_array_with_opts(char, alloc_len,
                                                    &(n00b_alloc_opts_t){
                                                        .allocator = obj->allocator,
                                                        .scan_kind = obj->scan_kind,
                                                        .scan_cb   = obj->scan_cb,
                                                        .scan_user = obj->scan_user,
                                                    });
        obj->alloc_len = alloc_len;
    }

    if (raw != nullptr) {
        if (hex != nullptr || ptr != nullptr) {
            return;
        }
        memcpy(obj->data, raw, length);
    }

    if (ptr != nullptr) {
        if (hex != nullptr) {
            return;
        }
        obj->data      = ptr;
        // .ptr transfers ownership: the buffer's free path will
        // n00b_free this pointer. Record alloc_len so resize-in-place
        // up to `length` doesn't pointlessly reallocate. Anything
        // larger than `length` still goes through the grow path. If
        // `length` is wrong (caller passed a smaller allocation),
        // grow-then-free will fault — same risk that existed before,
        // documented in the .ptr kwarg's @post.
        obj->alloc_len = length;
    }

    if (hex != nullptr) {
        uint8_t cur         = 0;
        int     valid_count = 0;

        for (size_t i = 0; i < hex->u8_bytes; i++) {
            uint8_t byte = (uint8_t)hex->data[i];

            if (byte >= '0' && byte <= '9') {
                if ((++valid_count) % 2 == 1) {
                    cur = (byte - '0') << 4;
                }
                else {
                    cur |= (byte - '0');
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
            if (byte >= 'a' && byte <= 'f') {
                if ((++valid_count) % 2 == 1) {
                    cur = ((byte - 'a') + 10) << 4;
                }
                else {
                    cur |= (byte - 'a') + 10;
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
            if (byte >= 'A' && byte <= 'F') {
                if ((++valid_count) % 2 == 1) {
                    cur = ((byte - 'A') + 10) << 4;
                }
                else {
                    cur |= (byte - 'A') + 10;
                    obj->data[obj->byte_len++] = cur;
                }
                continue;
            }
        }
    }
    else {
        obj->byte_len = length;
    }
}

// ============================================================================
// Len
// ============================================================================

n00b_size_t
n00b_buffer_len(n00b_buffer_t *buffer)
{
    return (n00b_size_t)buffer->byte_len;
}

// ============================================================================
// Resize
// ============================================================================

void
n00b_buffer_resize(n00b_buffer_t *buffer, uint64_t new_sz)
{
    defer_on();
    n00b_buffer_acquire_w(buffer);

    if ((int64_t)new_sz <= (int64_t)buffer->alloc_len) {
        buffer->byte_len = new_sz;
        Return;
    }

    uint64_t new_alloc_sz = n00b_align_closest_pow2_ceil(new_sz);
    char    *new_data = n00b_alloc_array_with_opts(char, new_alloc_sz,
                                                   &(n00b_alloc_opts_t){
                                                       .allocator = buffer->allocator,
                                                       .scan_kind = buffer->scan_kind,
                                                       .scan_cb   = buffer->scan_cb,
                                                       .scan_user = buffer->scan_user,
                                                   });

    memcpy(new_data, buffer->data, buffer->byte_len);

    if (buffer->data) {
        n00b_free(buffer->data);
    }

    buffer->data      = new_data;
    buffer->byte_len  = new_sz;
    buffer->alloc_len = new_alloc_sz;

    Return;
    defer_func_end();
}

// ============================================================================
// Add (concatenate)
// ============================================================================

n00b_buffer_t *
n00b_buffer_add(n00b_buffer_t *b1, n00b_buffer_t *b2) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!b1) {
        return b2;
    }
    if (!b2) {
        return b1;
    }

    defer_on();
    n00b_buffer_acquire_r(b1);
    n00b_buffer_acquire_r(b2);

    int64_t        l1     = n00b_max((int64_t)b1->byte_len, (int64_t)0);
    int64_t        l2     = n00b_max((int64_t)b2->byte_len, (int64_t)0);
    int64_t        lnew   = l1 + l2;
    n00b_buffer_t *result = _buffer_create(lnew, allocator);

    if (l1 > 0) {
        memcpy(result->data, b1->data, l1);
    }
    if (l2 > 0) {
        memcpy(result->data + l1, b2->data, l2);
    }

    Return result;
    defer_func_end();
}

// ============================================================================
// Concat (in-place append)
// ============================================================================

void
n00b_buffer_concat(n00b_buffer_t *dst, n00b_buffer_t *src) _kargs
{
    bool to_front = false;
}
{
    if (!dst || !src || src->byte_len == 0) {
        return;
    }

    defer_on();
    n00b_buffer_acquire_w(dst);
    n00b_buffer_acquire_r(src);

    size_t   old_len = dst->byte_len;
    uint64_t needed  = old_len + src->byte_len;

    if (needed > dst->alloc_len) {
        uint64_t new_alloc = n00b_align_closest_pow2_ceil(needed);
        char    *new_data  = n00b_alloc_array_with_opts(
            char, new_alloc,
            &(n00b_alloc_opts_t){.allocator = dst->allocator});

        if (to_front) {
            memcpy(new_data, src->data, src->byte_len);
            memcpy(new_data + src->byte_len, dst->data, old_len);
        }
        else {
            memcpy(new_data, dst->data, old_len);
            memcpy(new_data + old_len, src->data, src->byte_len);
        }

        if (dst->data) {
            n00b_free(dst->data);
        }

        dst->data      = new_data;
        dst->alloc_len = new_alloc;
    }
    else if (to_front) {
        memmove(dst->data + src->byte_len, dst->data, old_len);
        memcpy(dst->data, src->data, src->byte_len);
    }
    else {
        memcpy(dst->data + old_len, src->data, src->byte_len);
    }

    dst->byte_len = needed;

    Return;
    defer_func_end();
}

// ============================================================================
// Find
// ============================================================================

n00b_option_t(int64_t) n00b_buffer_find(n00b_buffer_t *b, n00b_buffer_t *sub) _kargs
{
    n00b_option_t(size_t) start = n00b_option_set(size_t, 0);
    n00b_option_t(size_t) end   = n00b_option_none(size_t);
}
{
    size_t blen   = b->byte_len;
    size_t sublen = sub->byte_len;
    size_t s      = n00b_option_get_or_else(start, 0);
    size_t e      = n00b_option_get_or_else(end, blen);

    if (s > blen) {
        return n00b_option_none(int64_t);
    }
    if (e > blen) {
        e = blen;
    }
    if (e <= s) {
        return n00b_option_none(int64_t);
    }
    if (sublen == 0) {
        return n00b_option_set(int64_t, (int64_t)s);
    }

    char *bp   = b->data + s;
    char *endp = b->data + e - sublen;

    while (bp <= endp) {
        char *p    = bp;
        char *subp = sub->data;
        bool  hit  = true;

        for (size_t i = 0; i < sublen; i++) {
            if (*p++ != *subp++) {
                hit = false;
                break;
            }
        }
        if (hit) {
            return n00b_option_set(int64_t, (int64_t)(bp - b->data));
        }
        bp++;
    }

    return n00b_option_none(int64_t);
}

// ============================================================================
// Index get / set
// ============================================================================

n00b_result_t(uint8_t) n00b_buffer_get_index(n00b_buffer_t *b, int64_t n)
{
    defer_on();
    n00b_buffer_acquire_r(b);

    if (n < 0) {
        n += (int64_t)b->byte_len;
        if (n < 0) {
            Return n00b_result_err(uint8_t, N00B_ERR_BUFFER_INDEX_OOB);
        }
    }
    if ((uint64_t)n >= b->byte_len) {
        Return n00b_result_err(uint8_t, N00B_ERR_BUFFER_INDEX_OOB);
    }

    uint8_t result = (uint8_t)b->data[n];
    Return  n00b_result_ok(uint8_t, result);
    defer_func_end();
}

n00b_result_t(bool) n00b_buffer_set_index(n00b_buffer_t *b, int64_t n, uint8_t c)
{
    defer_on();
    n00b_buffer_acquire_w(b);

    if (n < 0) {
        n += (int64_t)b->byte_len;
        if (n < 0) {
            Return n00b_result_err(bool, N00B_ERR_BUFFER_INDEX_OOB);
        }
    }
    if ((uint64_t)n >= b->byte_len) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_INDEX_OOB);
    }

    b->data[n] = (char)c;
    Return n00b_result_ok(bool, true);
    defer_func_end();
}

// ============================================================================
// Slice get / set
// ============================================================================

n00b_buffer_t *
n00b_buffer_get_slice(n00b_buffer_t *b, int64_t start, int64_t end) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    defer_on();
    n00b_buffer_acquire_r(b);

    int64_t len = (int64_t)b->byte_len;

    if (start < 0) {
        start += len;
    }
    else if (start >= len) {
        Return _buffer_create(0, allocator);
    }
    if (end < 0) {
        end += len + 1;
    }
    else if (end > len) {
        end = len;
    }
    if ((start | end) < 0 || start >= end) {
        Return _buffer_create(0, allocator);
    }

    int64_t        slice_len = end - start;
    n00b_buffer_t *result    = _buffer_create(slice_len, allocator);

    memcpy(result->data, b->data + start, slice_len);

    Return result;
    defer_func_end();
}

n00b_result_t(bool)
    n00b_buffer_set_slice(n00b_buffer_t *b, int64_t start, int64_t end) _kargs
{
    n00b_buffer_t *val = nullptr;
}
{
    defer_on();
    n00b_buffer_acquire_w(b);

    int64_t len = (int64_t)b->byte_len;

    if (start < 0) {
        start += len;
    }
    else if (start >= len) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_SLICE_OOB);
    }
    if (end < 0) {
        end += len + 1;
    }
    else if (end > len) {
        end = len;
    }
    if ((start | end) < 0 || start >= end) {
        Return n00b_result_err(bool, N00B_ERR_BUFFER_SLICE_OOB);
    }

    int64_t slice_len   = end - start;
    int64_t new_len     = (int64_t)b->byte_len - slice_len;
    int64_t replace_len = 0;

    if (val != nullptr) {
        replace_len = (int64_t)val->byte_len;
        new_len += replace_len;
    }

    if (new_len <= (int64_t)b->byte_len) {
        if (val != nullptr && val->byte_len > 0) {
            memcpy(b->data + start, val->data, replace_len);
        }
        if (end < (int64_t)b->byte_len) {
            memmove(b->data + start + replace_len, b->data + end, b->byte_len - end);
        }
    }
    else {
        char *new_buf = n00b_alloc_array_with_opts(char, new_len, &(n00b_alloc_opts_t){.allocator = b->allocator});
        if (start > 0) {
            memcpy(new_buf, b->data, start);
        }
        if (replace_len != 0) {
            memcpy(new_buf + start, val->data, replace_len);
        }
        if (end < (int64_t)b->byte_len) {
            memcpy(new_buf + start + replace_len, b->data + end, b->byte_len - end);
        }
        if (b->data) {
            n00b_free(b->data);
        }
        b->data      = new_buf;
        b->alloc_len = new_len;
    }

    b->byte_len = new_len;

    Return n00b_result_ok(bool, true);
    defer_func_end();
}

// ============================================================================
// Copy
// ============================================================================

n00b_buffer_t *
n00b_buffer_copy(n00b_buffer_t *inbuf) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    defer_on();
    n00b_buffer_acquire_r(inbuf);

    n00b_buffer_t *outbuf = _buffer_create((int64_t)inbuf->byte_len, allocator);

    memcpy(outbuf->data, inbuf->data, inbuf->byte_len);

    Return outbuf;
    defer_func_end();
}

// ============================================================================
// Raw C access
// ============================================================================

char *
n00b_buffer_to_c(n00b_buffer_t *b, int64_t *len_ptr)
{
    if (len_ptr) {
        *len_ptr = (int64_t)b->byte_len;
    }
    return b->data;
}

// ============================================================================
// To string
// ============================================================================

n00b_string_t *
n00b_buffer_to_string(n00b_buffer_t *buffer)
{
    defer_on();
    n00b_buffer_acquire_r(buffer);

    int64_t        nbytes = (int64_t)buffer->byte_len;
    n00b_string_t *result = n00b_string_from_raw(buffer->data, nbytes);

    Return result;
    defer_func_end();
}

// ============================================================================
// To hex string
// ============================================================================

n00b_string_t *
n00b_buffer_to_hex_str(n00b_buffer_t *buf)
{
    defer_on();
    n00b_buffer_acquire_r(buf);

    int64_t        hex_len = (int64_t)buf->byte_len * 2;
    n00b_string_t *result  = n00b_alloc(n00b_string_t);

    result->data = n00b_alloc_array(char, hex_len + 1);
    char *p      = result->data;

    for (size_t i = 0; i < buf->byte_len; i++) {
        uint8_t c = ((uint8_t *)buf->data)[i];
        *p++      = n00b_hex_map_lower[(c >> 4)];
        *p++      = n00b_hex_map_lower[c & 0x0f];
    }

    *p                 = '\0';
    result->codepoints = hex_len;
    result->u8_bytes   = hex_len;
    result->styling    = nullptr;

    Return result;
    defer_func_end();
}

// ============================================================================
// Join
// ============================================================================

n00b_buffer_t *
n00b_buffer_join(n00b_array_t(n00b_buffer_t *) items,
                 n00b_buffer_t *joiner) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    size_t count = items.len;

    if (count == 0) {
        return _buffer_create(0, allocator);
    }

    int64_t new_len = 0;
    int64_t jlen    = 0;

    for (size_t i = 0; i < count; i++) {
        new_len += (int64_t)items.data[i]->byte_len;
    }

    if (joiner != nullptr) {
        jlen = (int64_t)joiner->byte_len;
        new_len += jlen * ((int64_t)count - 1);
    }

    n00b_buffer_t *result = _buffer_create(new_len, allocator);
    char          *p      = result->data;

    int64_t clen = (int64_t)items.data[0]->byte_len;
    memcpy(p, items.data[0]->data, clen);

    for (size_t i = 1; i < count; i++) {
        p += clen;
        if (jlen > 0) {
            memcpy(p, joiner->data, jlen);
            p += jlen;
        }
        clen = (int64_t)items.data[i]->byte_len;
        memcpy(p, items.data[i]->data, clen);
    }

    return result;
}

// ============================================================================
// From codepoint
// ============================================================================

n00b_buffer_t *
n00b_buffer_from_codepoint(n00b_codepoint_t cp) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    uint8_t tmp[4];
    int     nbytes = n00b_utf8_encode_codepoint(cp, tmp);

    if (nbytes <= 0) {
        return _buffer_create(0, allocator);
    }

    n00b_buffer_t *result = _buffer_create(nbytes, allocator);
    memcpy(result->data, tmp, nbytes);

    return result;
}

// ============================================================================
// Free
// ============================================================================

void
n00b_buffer_free(n00b_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    if (buf->data) {
        if (buf->flags & N00B_BUF_F_MMAP) {
#ifndef _WIN32
            munmap(buf->data, buf->byte_len);
#endif
        }
        else if (buf->flags & N00B_BUF_F_BORROWED) {
            // Borrowed pointer — owner frees, we don't touch it.
        }
        else {
            n00b_free(buf->data);
        }
    }

    buf->data      = nullptr;
    buf->byte_len  = 0;
    buf->alloc_len = 0;
    buf->flags     = 0;
    buf->lock      = nullptr;
}
