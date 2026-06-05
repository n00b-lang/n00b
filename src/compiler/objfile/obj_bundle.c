#include "compiler/objfile/obj_bundle.h"

#include "adt/list.h"
#include "compiler/objfile/abstract.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf_rewrite.h"
#include "compiler/objfile/elf_types.h"
#include "compiler/objfile/writer.h"
#include "core/sha256.h"
#include "text/unicode/encoding.h"

const uint8_t N00B_OBJ_BUNDLE_MANIFEST_MAGIC[N00B_OBJ_BUNDLE_MANIFEST_MAGIC_LEN] = {
    'N', '0', '0', 'B', 'N', 'D', 'L', '1',
};

const uint8_t N00B_OBJ_BUNDLE_POLICY_MAGIC[N00B_OBJ_BUNDLE_POLICY_MAGIC_LEN] = {
    'N', '0', '0', 'B', 'P', 'O', 'L', '1',
};

#define N00B_OBJ_BUNDLE_HEADER_SIZE        208u
#define N00B_OBJ_BUNDLE_CONTENT_ID_OFF     32u
#define N00B_OBJ_BUNDLE_ARTIFACT_REC_SIZE  72u
#define N00B_OBJ_BUNDLE_PAYLOAD_REC_SIZE   64u
#define N00B_OBJ_BUNDLE_EXEC_REC_SIZE      24u
#define N00B_OBJ_BUNDLE_POLICY_REC_SIZE    88u
#define N00B_OBJ_BUNDLE_NO_PAYLOAD_INDEX   UINT32_MAX
#define N00B_OBJ_BUNDLE_EXEC_REC_DEFAULT   1u
#define N00B_OBJ_BUNDLE_EXEC_REC_SELECTOR  2u

#define N00B_OBJ_BUNDLE_DECL_POLICY_SIZE                 64u
#define N00B_OBJ_BUNDLE_DECL_POLICY_MAJOR                1u
#define N00B_OBJ_BUNDLE_DECL_POLICY_MINOR                0u
#define N00B_OBJ_BUNDLE_DECL_POLICY_DECL_FLAGS_OFF       16u
#define N00B_OBJ_BUNDLE_DECL_POLICY_PATH_FLAGS_OFF       24u
#define N00B_OBJ_BUNDLE_DECL_POLICY_ARTIFACT_MASK_OFF    32u
#define N00B_OBJ_BUNDLE_DECL_POLICY_EXEC_FLAGS_OFF       40u
#define N00B_OBJ_BUNDLE_DECL_POLICY_FALLBACK_ID_OFF      48u
#define N00B_OBJ_BUNDLE_DECL_POLICY_RESERVED1_OFF        56u
#define N00B_OBJ_BUNDLE_DECL_PATH_RELATIVE               (1ull << 0)
#define N00B_OBJ_BUNDLE_DECL_PATH_NO_EMPTY_COMPONENTS    (1ull << 1)
#define N00B_OBJ_BUNDLE_DECL_PATH_NO_PARENT_REFERENCES   (1ull << 2)
#define N00B_OBJ_BUNDLE_DECL_PATH_NO_ABSOLUTE_PATHS      (1ull << 3)
#define N00B_OBJ_BUNDLE_DECL_PATH_VALID_UTF8             (1ull << 4)
#define N00B_OBJ_BUNDLE_DECL_PATH_DEFAULT                \
    (N00B_OBJ_BUNDLE_DECL_PATH_RELATIVE                  \
     | N00B_OBJ_BUNDLE_DECL_PATH_NO_EMPTY_COMPONENTS     \
     | N00B_OBJ_BUNDLE_DECL_PATH_NO_PARENT_REFERENCES    \
     | N00B_OBJ_BUNDLE_DECL_PATH_NO_ABSOLUTE_PATHS       \
     | N00B_OBJ_BUNDLE_DECL_PATH_VALID_UTF8)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_FILE          \
    (1ull << N00B_OBJ_BUNDLE_ARTIFACT_FILE)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_EXECUTABLE    \
    (1ull << N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_DIRECTORY     \
    (1ull << N00B_OBJ_BUNDLE_ARTIFACT_DIRECTORY)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_METADATA      \
    (1ull << N00B_OBJ_BUNDLE_ARTIFACT_METADATA)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_OPAQUE        \
    (1ull << N00B_OBJ_BUNDLE_ARTIFACT_OPAQUE)
#define N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_DEFAULT       \
    (N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_FILE             \
     | N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_EXECUTABLE     \
     | N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_DIRECTORY      \
     | N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_METADATA       \
     | N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_OPAQUE)
#define N00B_OBJ_BUNDLE_DECL_EXEC_DEFAULT_EXEC           (1ull << 0)
#define N00B_OBJ_BUNDLE_DECL_EXEC_SELECTOR_MAPPING       (1ull << 1)
#define N00B_OBJ_BUNDLE_DECL_EXEC_DEFAULT                \
    (N00B_OBJ_BUNDLE_DECL_EXEC_DEFAULT_EXEC              \
     | N00B_OBJ_BUNDLE_DECL_EXEC_SELECTOR_MAPPING)
#define N00B_OBJ_BUNDLE_ELF_GUARD_SECTION_TYPE 0xc001u

typedef struct n00b_obj_bundle_exec_mapping {
    n00b_string_t *selector;
    uint64_t       target_artifact_id;
} n00b_obj_bundle_exec_mapping_t;

typedef struct n00b_obj_bundle_policy {
    uint64_t                      policy_id;
    n00b_obj_bundle_policy_kind_t kind;
    n00b_obj_bundle_policy_scope_t scope;
    uint64_t                      flags;
    uint64_t                      priority;
    const n00b_buffer_t          *payload;
    uint64_t                      fallback_policy_id;
} n00b_obj_bundle_policy_t;

struct n00b_obj_bundle {
    n00b_allocator_t                              *allocator;
    bool                                          is_mutable;
    uint64_t                                      next_artifact_id;
    n00b_list_t(n00b_obj_bundle_artifact_t *)     artifacts;
    bool                                          has_default_exec;
    uint64_t                                      default_exec_id;
    n00b_list_t(n00b_obj_bundle_exec_mapping_t *) exec_mappings;
    n00b_list_t(n00b_obj_bundle_policy_t *)       policies;
};

struct n00b_obj_bundle_artifact {
    uint64_t                        id;
    n00b_obj_bundle_artifact_kind_t kind;
    n00b_string_t                  *logical_path;
    const n00b_buffer_t            *payload;
    n00b_string_t                  *role;
    uint32_t                        mode;
    uint64_t                        flags;
};

struct n00b_obj_bundle_error {
    n00b_obj_bundle_error_code_t    code;
    n00b_string_t                  *message;
    n00b_format_t                   format;
    bool                            has_format;
    n00b_obj_bundle_carrier_t       carrier;
    bool                            has_carrier;
    n00b_string_t                  *logical_path;
    bool                            has_logical_path;
    uint64_t                        artifact_id;
    bool                            has_artifact_id;
    n00b_obj_bundle_policy_kind_t   policy_kind;
    bool                            has_policy_kind;
    n00b_obj_bundle_policy_scope_t  policy_scope;
    bool                            has_policy_scope;
    int64_t                         detail;
    bool                            has_detail;
};

typedef struct n00b_obj_bundle_manifest_strtab {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     error;
} n00b_obj_bundle_manifest_strtab_t;

typedef struct n00b_obj_bundle_encode_artifact {
    n00b_obj_bundle_artifact_t *artifact;
    uint64_t                    encoded_id;
    uint32_t                    path_off;
    uint32_t                    role_off;
    uint32_t                    payload_index;
    uint64_t                    payload_off;
    uint64_t                    payload_len;
    uint8_t                     digest[N00B_OBJ_BUNDLE_DIGEST_LEN];
} n00b_obj_bundle_encode_artifact_t;

typedef struct n00b_obj_bundle_encode_exec_mapping {
    n00b_obj_bundle_exec_mapping_t *mapping;
    uint32_t                        selector_off;
    uint64_t                        target_encoded_id;
} n00b_obj_bundle_encode_exec_mapping_t;

typedef struct n00b_obj_bundle_encode_policy {
    n00b_obj_bundle_policy_t *policy;
    uint64_t                  payload_off;
    uint64_t                  payload_len;
    uint8_t                   digest[N00B_OBJ_BUNDLE_DIGEST_LEN];
} n00b_obj_bundle_encode_policy_t;

typedef struct n00b_obj_bundle_manifest_range {
    uint64_t off;
    uint64_t len;
} n00b_obj_bundle_manifest_range_t;

typedef struct n00b_obj_bundle_decode_artifact {
    uint64_t                         id;
    n00b_obj_bundle_artifact_kind_t  kind;
    uint32_t                         record_flags;
    uint64_t                         flags;
    uint32_t                         path_off;
    uint32_t                         role_off;
    uint32_t                         mode;
    uint32_t                         payload_index;
    uint8_t                          digest[N00B_OBJ_BUNDLE_DIGEST_LEN];
    n00b_string_t                   *logical_path;
    n00b_string_t                   *role;
    const n00b_buffer_t             *payload;
} n00b_obj_bundle_decode_artifact_t;

typedef struct n00b_obj_bundle_decode_payload {
    uint64_t artifact_id;
    uint64_t flags;
    uint64_t payload_off;
    uint64_t payload_len;
    uint8_t  digest[N00B_OBJ_BUNDLE_DIGEST_LEN];
} n00b_obj_bundle_decode_payload_t;

typedef struct n00b_obj_bundle_decode_exec {
    uint32_t       kind;
    uint32_t       flags;
    uint32_t       selector_off;
    uint32_t       reserved;
    uint64_t       target_id;
    n00b_string_t *selector;
} n00b_obj_bundle_decode_exec_t;

typedef struct n00b_obj_bundle_decode_policy {
    uint64_t                      policy_id;
    n00b_obj_bundle_policy_kind_t kind;
    n00b_obj_bundle_policy_scope_t scope;
    uint64_t                      flags;
    uint64_t                      priority;
    uint64_t                      payload_off;
    uint64_t                      payload_len;
    uint64_t                      fallback_policy_id;
    uint8_t                       digest[N00B_OBJ_BUNDLE_DIGEST_LEN];
    const n00b_buffer_t          *payload;
} n00b_obj_bundle_decode_policy_t;

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_new(n00b_obj_bundle_error_code_t code,
                           n00b_string_t               *message,
                           n00b_allocator_t            *allocator)
{
    n00b_obj_bundle_error_t *error =
        n00b_alloc_with_opts(n00b_obj_bundle_error_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});

    error->code             = code;
    error->message          = message;
    error->format           = N00B_FMT_UNKNOWN;
    error->has_format       = false;
    error->carrier          = N00B_OBJ_BUNDLE_CARRIER_AUTO;
    error->has_carrier      = false;
    error->logical_path     = nullptr;
    error->has_logical_path = false;
    error->artifact_id      = N00B_OBJ_BUNDLE_ARTIFACT_ID_NONE;
    error->has_artifact_id  = false;
    error->policy_kind      = N00B_OBJ_BUNDLE_POLICY_KIND_NONE;
    error->has_policy_kind  = false;
    error->policy_scope     = N00B_OBJ_BUNDLE_POLICY_SCOPE_NONE;
    error->has_policy_scope = false;
    error->detail           = 0;
    error->has_detail       = false;

    return error;
}

#define OBJ_BUNDLE_ERR(T, code, message, allocator)                                           \
    n00b_result_err_payload(                                                                  \
        T,                                                                                    \
        n00b_obj_bundle_error_t *,                                                            \
        _n00b_obj_bundle_error_new((code), (message), (allocator)))

#define OBJ_BUNDLE_ERR_PAYLOAD(T, error)                                                      \
    n00b_result_err_payload(T, n00b_obj_bundle_error_t *, (error))

static n00b_allocator_t *
_n00b_obj_bundle_allocator(n00b_obj_bundle_t *bundle)
{
    return bundle == nullptr ? nullptr : bundle->allocator;
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_with_path(n00b_obj_bundle_error_code_t code,
                                 n00b_string_t               *message,
                                 n00b_string_t               *path,
                                 n00b_allocator_t            *allocator)
{
    n00b_obj_bundle_error_t *error =
        _n00b_obj_bundle_error_new(code, message, allocator);

    error->logical_path     = path;
    error->has_logical_path = path != nullptr;

    return error;
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_with_artifact(n00b_obj_bundle_error_code_t code,
                                     n00b_string_t               *message,
                                     uint64_t                     artifact_id,
                                     n00b_allocator_t            *allocator)
{
    n00b_obj_bundle_error_t *error =
        _n00b_obj_bundle_error_new(code, message, allocator);

    error->artifact_id     = artifact_id;
    error->has_artifact_id = true;

    return error;
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_with_policy(n00b_obj_bundle_error_code_t    code,
                                   n00b_string_t                  *message,
                                   n00b_obj_bundle_policy_kind_t   kind,
                                   n00b_obj_bundle_policy_scope_t  scope,
                                   uint64_t                        detail,
                                   n00b_allocator_t               *allocator)
{
    n00b_obj_bundle_error_t *error =
        _n00b_obj_bundle_error_new(code, message, allocator);

    error->policy_kind      = kind;
    error->has_policy_kind  = true;
    error->policy_scope     = scope;
    error->has_policy_scope = true;
    error->detail           = (int64_t)detail;
    error->has_detail       = true;

    return error;
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_with_format_carrier(
    n00b_obj_bundle_error_code_t code,
    n00b_string_t               *message,
    n00b_format_t                format,
    bool                         has_format,
    n00b_obj_bundle_carrier_t    carrier,
    bool                         has_carrier,
    n00b_allocator_t            *allocator)
{
    n00b_obj_bundle_error_t *error =
        _n00b_obj_bundle_error_new(code, message, allocator);

    error->format      = format;
    error->has_format  = has_format;
    error->carrier     = carrier;
    error->has_carrier = has_carrier;

    return error;
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_with_format_carrier_detail(
    n00b_obj_bundle_error_code_t code,
    n00b_string_t               *message,
    n00b_format_t                format,
    bool                         has_format,
    n00b_obj_bundle_carrier_t    carrier,
    bool                         has_carrier,
    int64_t                      detail,
    bool                         has_detail,
    n00b_allocator_t            *allocator)
{
    n00b_obj_bundle_error_t *error =
        _n00b_obj_bundle_error_with_format_carrier(code,
                                                   message,
                                                   format,
                                                   has_format,
                                                   carrier,
                                                   has_carrier,
                                                   allocator);

    error->detail     = detail;
    error->has_detail = has_detail;

    return error;
}

static bool
_n00b_obj_bundle_string_len_is_supported(n00b_string_t *s)
{
    return s != nullptr && s->u8_bytes <= UINT32_MAX && s->u8_bytes <= INT64_MAX;
}

static bool
_n00b_obj_bundle_string_bytes_eq(n00b_string_t *a, n00b_string_t *b)
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }

    if (a->u8_bytes != b->u8_bytes) {
        return false;
    }

    if (a->u8_bytes == 0) {
        return true;
    }

    return memcmp(a->data, b->data, a->u8_bytes) == 0;
}

static int
_n00b_obj_bundle_string_bytes_cmp(n00b_string_t *a, n00b_string_t *b)
{
    if (a == nullptr || b == nullptr) {
        return (a != nullptr) - (b != nullptr);
    }

    size_t min_len = a->u8_bytes < b->u8_bytes ? a->u8_bytes : b->u8_bytes;
    int    cmp     = min_len == 0 ? 0 : memcmp(a->data, b->data, min_len);

    if (cmp != 0) {
        return cmp;
    }

    return (a->u8_bytes > b->u8_bytes) - (a->u8_bytes < b->u8_bytes);
}

static int
_n00b_obj_bundle_buffer_bytes_cmp(const n00b_buffer_t *a,
                                  const n00b_buffer_t *b)
{
    if (a == nullptr || b == nullptr) {
        return (a != nullptr) - (b != nullptr);
    }

    size_t min_len = a->byte_len < b->byte_len ? a->byte_len : b->byte_len;
    int    cmp     = min_len == 0 ? 0 : memcmp(a->data, b->data, min_len);

    if (cmp != 0) {
        return cmp;
    }

    return (a->byte_len > b->byte_len) - (a->byte_len < b->byte_len);
}

static n00b_string_t *
_n00b_obj_bundle_copy_string(n00b_string_t *s, n00b_allocator_t *allocator)
{
    if (s == nullptr) {
        return nullptr;
    }

    return n00b_string_from_raw(s->data,
                                (int64_t)s->u8_bytes,
                                .allocator = allocator);
}

static n00b_buffer_t *
_n00b_obj_bundle_copy_buffer(const n00b_buffer_t *buffer,
                             n00b_allocator_t    *allocator)
{
    if (buffer == nullptr) {
        return nullptr;
    }

    return n00b_buffer_copy((n00b_buffer_t *)buffer, .allocator = allocator);
}

static bool
_n00b_obj_bundle_string_has_nul(n00b_string_t *s)
{
    if (s == nullptr || s->u8_bytes == 0) {
        return false;
    }

    return memchr(s->data, '\0', s->u8_bytes) != nullptr;
}

static uint64_t
_n00b_obj_bundle_align_u64(uint64_t value, uint64_t alignment)
{
    if (alignment <= 1) {
        return value;
    }

    uint64_t rem = value % alignment;

    return rem == 0 ? value : value + alignment - rem;
}

static void
_n00b_obj_bundle_sha256_bytes(const void *data,
                              size_t      len,
                              uint8_t     out[N00B_OBJ_BUNDLE_DIGEST_LEN])
{
    n00b_sha256_digest_t digest;

    n00b_sha256_hash(data, len, digest);

    for (size_t i = 0; i < N00B_SHA256_DIGEST_WORDS; i++) {
        uint32_t word = digest[i];

        out[i * 4]     = (uint8_t)(word >> 24);
        out[i * 4 + 1] = (uint8_t)(word >> 16);
        out[i * 4 + 2] = (uint8_t)(word >> 8);
        out[i * 4 + 3] = (uint8_t)word;
    }
}

static n00b_obj_bundle_manifest_strtab_t *
_n00b_obj_bundle_manifest_strtab_new(void)
{
    n00b_obj_bundle_manifest_strtab_t *tab =
        n00b_alloc(n00b_obj_bundle_manifest_strtab_t);

    tab->cap    = 256;
    tab->data   = n00b_alloc_array(uint8_t, tab->cap);
    tab->len    = 1;
    tab->error  = false;
    tab->data[0] = 0;

    return tab;
}

static void
_n00b_obj_bundle_manifest_strtab_ensure(
    n00b_obj_bundle_manifest_strtab_t *tab,
    size_t                             need)
{
    if (tab->error || need <= tab->cap) {
        return;
    }

    size_t new_cap = tab->cap;

    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
            tab->error = true;
            return;
        }

        new_cap *= 2;
    }

    uint8_t *new_data = n00b_alloc_array(uint8_t, new_cap);

    memcpy(new_data, tab->data, tab->len);
    tab->data = new_data;
    tab->cap  = new_cap;
}

static uint32_t
_n00b_obj_bundle_manifest_strtab_add(
    n00b_obj_bundle_manifest_strtab_t *tab,
    n00b_string_t                     *s)
{
    if (tab->error || s == nullptr || s->u8_bytes == 0) {
        return 0;
    }

    if (_n00b_obj_bundle_string_has_nul(s)) {
        tab->error = true;
        return 0;
    }

    for (size_t i = 1; i < tab->len; ) {
        size_t len = 0;

        while (i + len < tab->len && tab->data[i + len] != 0) {
            len++;
        }

        if (i + len >= tab->len) {
            tab->error = true;
            return 0;
        }

        if (len == s->u8_bytes
            && memcmp(tab->data + i, s->data, len) == 0) {
            return (uint32_t)i;
        }

        i += len + 1;
    }

    size_t need = tab->len + s->u8_bytes + 1;

    if (need > UINT32_MAX) {
        tab->error = true;
        return 0;
    }

    _n00b_obj_bundle_manifest_strtab_ensure(tab, need);

    if (tab->error) {
        return 0;
    }

    uint32_t off = (uint32_t)tab->len;

    memcpy(tab->data + tab->len, s->data, s->u8_bytes);
    tab->len += s->u8_bytes;
    tab->data[tab->len++] = 0;

    return off;
}

static n00b_result_t(n00b_string_t *)
_n00b_obj_bundle_normalize_logical_path(n00b_string_t     *path,
                                        n00b_allocator_t  *allocator)
{
    if (path == nullptr || !_n00b_obj_bundle_string_len_is_supported(path)) {
        return OBJ_BUNDLE_ERR(n00b_string_t *,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid logical path argument",
                              allocator);
    }

    if (path->u8_bytes == 0 || path->data == nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_string_t *,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH,
                r"object bundle: empty logical path",
                path,
                allocator));
    }

    if (!n00b_unicode_str_validate(path)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_string_t *,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH,
                r"object bundle: logical path is not valid UTF-8",
                path,
                allocator));
    }

    const uint8_t *data = (const uint8_t *)path->data;
    size_t         len  = path->u8_bytes;

    if (data[0] == '/' || data[len - 1] == '/') {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_string_t *,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH,
                r"object bundle: logical path has invalid slash syntax",
                path,
                allocator));
    }

    size_t component_start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i != len && data[i] != '/') {
            continue;
        }

        size_t component_len = i - component_start;

        if (component_len == 0
            || (component_len == 1 && data[component_start] == '.')
            || (component_len == 2
                && data[component_start] == '.'
                && data[component_start + 1] == '.')) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                n00b_string_t *,
                _n00b_obj_bundle_error_with_path(
                    N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH,
                    r"object bundle: logical path has invalid component",
                    path,
                    allocator));
        }

        component_start = i + 1;
    }

    return n00b_result_ok(n00b_string_t *,
                          _n00b_obj_bundle_copy_string(path, allocator));
}

static bool
_n00b_obj_bundle_artifact_kind_is_valid(n00b_obj_bundle_artifact_kind_t kind)
{
    switch (kind) {
    case N00B_OBJ_BUNDLE_ARTIFACT_FILE:
    case N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE:
    case N00B_OBJ_BUNDLE_ARTIFACT_DIRECTORY:
    case N00B_OBJ_BUNDLE_ARTIFACT_METADATA:
    case N00B_OBJ_BUNDLE_ARTIFACT_OPAQUE:
        return true;
    default:
        return false;
    }
}

static bool
_n00b_obj_bundle_artifact_kind_requires_payload(
    n00b_obj_bundle_artifact_kind_t kind)
{
    return kind == N00B_OBJ_BUNDLE_ARTIFACT_FILE
           || kind == N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE
           || kind == N00B_OBJ_BUNDLE_ARTIFACT_OPAQUE;
}

static bool
_n00b_obj_bundle_artifact_has_valid_payload(
    n00b_obj_bundle_artifact_kind_t kind,
    const n00b_buffer_t            *payload)
{
    if (kind == N00B_OBJ_BUNDLE_ARTIFACT_DIRECTORY) {
        return payload == nullptr;
    }

    if (_n00b_obj_bundle_artifact_kind_requires_payload(kind)) {
        return payload != nullptr;
    }

    return true;
}

static bool
_n00b_obj_bundle_artifact_is_executable(n00b_obj_bundle_artifact_t *artifact)
{
    if (artifact == nullptr) {
        return false;
    }

    if (artifact->kind == N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE) {
        return true;
    }

    return artifact->kind == N00B_OBJ_BUNDLE_ARTIFACT_FILE
           && (artifact->mode & 0111u) != 0;
}

static n00b_obj_bundle_artifact_t *
_n00b_obj_bundle_find_artifact_by_path(n00b_obj_bundle_t *bundle,
                                       n00b_string_t     *path)
{
    size_t n = n00b_list_len(bundle->artifacts);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_artifact_t *artifact =
            n00b_list_get(bundle->artifacts, i);

        if (_n00b_obj_bundle_string_bytes_eq(artifact->logical_path, path)) {
            return artifact;
        }
    }

    return nullptr;
}

static n00b_obj_bundle_artifact_t *
_n00b_obj_bundle_find_artifact_by_id(n00b_obj_bundle_t *bundle,
                                     uint64_t           artifact_id)
{
    size_t n = n00b_list_len(bundle->artifacts);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_artifact_t *artifact =
            n00b_list_get(bundle->artifacts, i);

        if (artifact->id == artifact_id) {
            return artifact;
        }
    }

    return nullptr;
}

static bool
_n00b_obj_bundle_selector_is_valid(n00b_string_t *selector)
{
    return _n00b_obj_bundle_string_len_is_supported(selector)
           && selector->u8_bytes != 0
           && selector->data != nullptr
           && n00b_unicode_str_validate(selector);
}

static n00b_obj_bundle_exec_mapping_t *
_n00b_obj_bundle_find_mapping_by_selector(n00b_obj_bundle_t *bundle,
                                          n00b_string_t     *selector)
{
    size_t n = n00b_list_len(bundle->exec_mappings);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_exec_mapping_t *mapping =
            n00b_list_get(bundle->exec_mappings, i);

        if (_n00b_obj_bundle_string_bytes_eq(mapping->selector, selector)) {
            return mapping;
        }
    }

    return nullptr;
}

static bool
_n00b_obj_bundle_policy_kind_is_supported(n00b_obj_bundle_policy_kind_t kind)
{
    return kind == N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT
           || kind == N00B_OBJ_BUNDLE_POLICY_KIND_DECLARATIVE_V1;
}

static bool
_n00b_obj_bundle_policy_scope_is_valid(n00b_obj_bundle_policy_scope_t scope)
{
    return scope != N00B_OBJ_BUNDLE_POLICY_SCOPE_NONE
           && (scope & ~N00B_OBJ_BUNDLE_POLICY_SCOPE_BOTH) == 0;
}

static bool
_n00b_obj_bundle_policy_flags_are_valid(uint64_t flags)
{
    uint64_t allowed = N00B_OBJ_BUNDLE_POLICY_F_REQUIRED
                       | N00B_OBJ_BUNDLE_POLICY_F_OPTIONAL
                       | N00B_OBJ_BUNDLE_POLICY_F_FALLBACK_ALLOWED;
    bool has_required = (flags & N00B_OBJ_BUNDLE_POLICY_F_REQUIRED) != 0;
    bool has_optional = (flags & N00B_OBJ_BUNDLE_POLICY_F_OPTIONAL) != 0;

    return (flags & ~allowed) == 0 && has_required != has_optional;
}

static n00b_obj_bundle_policy_t *
_n00b_obj_bundle_find_policy_by_id(n00b_obj_bundle_t *bundle,
                                   uint64_t           policy_id)
{
    size_t n = n00b_list_len(bundle->policies);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_policy_t *policy = n00b_list_get(bundle->policies, i);

        if (policy->policy_id == policy_id) {
            return policy;
        }
    }

    return nullptr;
}

static uint16_t
_n00b_obj_bundle_le16_at(const uint8_t *data, size_t off)
{
    return (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
}

static uint32_t
_n00b_obj_bundle_le32_at(const uint8_t *data, size_t off)
{
    return (uint32_t)data[off]
           | ((uint32_t)data[off + 1] << 8)
           | ((uint32_t)data[off + 2] << 16)
           | ((uint32_t)data[off + 3] << 24);
}

static uint64_t
_n00b_obj_bundle_le64_at(const uint8_t *data, size_t off)
{
    return (uint64_t)_n00b_obj_bundle_le32_at(data, off)
           | ((uint64_t)_n00b_obj_bundle_le32_at(data, off + 4) << 32);
}

static bool
_n00b_obj_bundle_declarative_policy_payload_is_valid(
    const n00b_buffer_t *payload,
    uint64_t             fallback_policy_id)
{
    if (payload == nullptr
        || payload->byte_len != N00B_OBJ_BUNDLE_DECL_POLICY_SIZE) {
        return false;
    }

    const uint8_t *data = (const uint8_t *)payload->data;

    if (memcmp(data,
               N00B_OBJ_BUNDLE_POLICY_MAGIC,
               N00B_OBJ_BUNDLE_POLICY_MAGIC_LEN) != 0) {
        return false;
    }

    uint16_t major = _n00b_obj_bundle_le16_at(data, 8);
    uint16_t minor = _n00b_obj_bundle_le16_at(data, 10);
    uint32_t reserved0 = _n00b_obj_bundle_le32_at(data, 12);
    uint64_t decl_flags =
        _n00b_obj_bundle_le64_at(data,
                                 N00B_OBJ_BUNDLE_DECL_POLICY_DECL_FLAGS_OFF);
    uint64_t path_flags =
        _n00b_obj_bundle_le64_at(data,
                                 N00B_OBJ_BUNDLE_DECL_POLICY_PATH_FLAGS_OFF);
    uint64_t artifact_kind_mask =
        _n00b_obj_bundle_le64_at(
            data,
            N00B_OBJ_BUNDLE_DECL_POLICY_ARTIFACT_MASK_OFF);
    uint64_t execution_flags =
        _n00b_obj_bundle_le64_at(data,
                                 N00B_OBJ_BUNDLE_DECL_POLICY_EXEC_FLAGS_OFF);
    uint64_t payload_fallback_id =
        _n00b_obj_bundle_le64_at(
            data,
            N00B_OBJ_BUNDLE_DECL_POLICY_FALLBACK_ID_OFF);
    uint64_t reserved1 =
        _n00b_obj_bundle_le64_at(data,
                                 N00B_OBJ_BUNDLE_DECL_POLICY_RESERVED1_OFF);

    return major == N00B_OBJ_BUNDLE_DECL_POLICY_MAJOR
           && minor == N00B_OBJ_BUNDLE_DECL_POLICY_MINOR
           && reserved0 == 0
           && decl_flags == 0
           && path_flags == N00B_OBJ_BUNDLE_DECL_PATH_DEFAULT
           && artifact_kind_mask
                  == N00B_OBJ_BUNDLE_DECL_ARTIFACT_KIND_DEFAULT
           && execution_flags == N00B_OBJ_BUNDLE_DECL_EXEC_DEFAULT
           && payload_fallback_id == fallback_policy_id
           && reserved1 == 0;
}

static bool
_n00b_obj_bundle_policy_payload_is_valid(n00b_obj_bundle_policy_kind_t kind,
                                         const n00b_buffer_t          *payload,
                                         uint64_t fallback_policy_id)
{
    if (kind == N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT) {
        return payload == nullptr;
    }

    if (kind == N00B_OBJ_BUNDLE_POLICY_KIND_DECLARATIVE_V1) {
        return _n00b_obj_bundle_declarative_policy_payload_is_valid(
            payload,
            fallback_policy_id);
    }

    return false;
}

static bool
_n00b_obj_bundle_format_request_is_valid(n00b_format_t format)
{
    switch (format) {
    case N00B_FMT_UNKNOWN:
    case N00B_FMT_ELF:
    case N00B_FMT_MACHO:
    case N00B_FMT_PE:
        return true;
    default:
        return false;
    }
}

static bool
_n00b_obj_bundle_carrier_request_is_valid(n00b_obj_bundle_carrier_t carrier)
{
    switch (carrier) {
    case N00B_OBJ_BUNDLE_CARRIER_AUTO:
    case N00B_OBJ_BUNDLE_CARRIER_METADATA:
    case N00B_OBJ_BUNDLE_CARRIER_LOADABLE:
    case N00B_OBJ_BUNDLE_CARRIER_SPLIT:
        return true;
    default:
        return false;
    }
}

static bool
_n00b_obj_bundle_replace_policy_is_valid(
    n00b_obj_bundle_replace_policy_t replace)
{
    switch (replace) {
    case N00B_OBJ_BUNDLE_REJECT_EXISTING:
    case N00B_OBJ_BUNDLE_REPLACE_EXISTING:
        return true;
    default:
        return false;
    }
}

static bool
_n00b_obj_bundle_object_bytes_arg_is_valid(n00b_buffer_t *object_bytes)
{
    return object_bytes != nullptr && object_bytes->data != nullptr;
}

static bool
_n00b_obj_bundle_policy_fallback_is_valid(n00b_obj_bundle_t        *bundle,
                                          n00b_obj_bundle_policy_t *policy)
{
    if (policy->fallback_policy_id == N00B_OBJ_BUNDLE_POLICY_ID_NONE) {
        return true;
    }

    if ((policy->flags & N00B_OBJ_BUNDLE_POLICY_F_FALLBACK_ALLOWED) == 0) {
        return false;
    }

    n00b_obj_bundle_policy_t *fallback =
        _n00b_obj_bundle_find_policy_by_id(bundle, policy->fallback_policy_id);

    if (fallback == nullptr
        || fallback->kind != N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT
        || fallback->priority >= policy->priority) {
        return false;
    }

    return (fallback->scope & policy->scope) == policy->scope;
}

static int
_n00b_obj_bundle_encode_artifact_cmp(const void *a, const void *b)
{
    const n00b_obj_bundle_encode_artifact_t *left  = a;
    const n00b_obj_bundle_encode_artifact_t *right = b;

    return _n00b_obj_bundle_string_bytes_cmp(left->artifact->logical_path,
                                             right->artifact->logical_path);
}

static int
_n00b_obj_bundle_encode_mapping_cmp(const void *a, const void *b)
{
    const n00b_obj_bundle_encode_exec_mapping_t *left  = a;
    const n00b_obj_bundle_encode_exec_mapping_t *right = b;

    return _n00b_obj_bundle_string_bytes_cmp(left->mapping->selector,
                                             right->mapping->selector);
}

static int
_n00b_obj_bundle_encode_policy_cmp(const void *a, const void *b)
{
    const n00b_obj_bundle_encode_policy_t *left  = a;
    const n00b_obj_bundle_encode_policy_t *right = b;
    n00b_obj_bundle_policy_t              *lp    = left->policy;
    n00b_obj_bundle_policy_t              *rp    = right->policy;

    if (lp->priority != rp->priority) {
        return (lp->priority > rp->priority) - (lp->priority < rp->priority);
    }

    if (lp->policy_id != rp->policy_id) {
        return (lp->policy_id > rp->policy_id)
               - (lp->policy_id < rp->policy_id);
    }

    if (lp->kind != rp->kind) {
        return (lp->kind > rp->kind) - (lp->kind < rp->kind);
    }

    if (lp->scope != rp->scope) {
        return (lp->scope > rp->scope) - (lp->scope < rp->scope);
    }

    if (lp->flags != rp->flags) {
        return (lp->flags > rp->flags) - (lp->flags < rp->flags);
    }

    int cmp = _n00b_obj_bundle_buffer_bytes_cmp(lp->payload, rp->payload);

    if (cmp != 0) {
        return cmp;
    }

    return (lp->fallback_policy_id > rp->fallback_policy_id)
           - (lp->fallback_policy_id < rp->fallback_policy_id);
}

static int
_n00b_obj_bundle_decode_policy_cmp(
    const n00b_obj_bundle_decode_policy_t *left,
    const n00b_obj_bundle_decode_policy_t *right)
{
    if (left->priority != right->priority) {
        return (left->priority > right->priority)
               - (left->priority < right->priority);
    }

    if (left->policy_id != right->policy_id) {
        return (left->policy_id > right->policy_id)
               - (left->policy_id < right->policy_id);
    }

    if (left->kind != right->kind) {
        return (left->kind > right->kind) - (left->kind < right->kind);
    }

    if (left->scope != right->scope) {
        return (left->scope > right->scope) - (left->scope < right->scope);
    }

    if (left->flags != right->flags) {
        return (left->flags > right->flags)
               - (left->flags < right->flags);
    }

    int cmp = _n00b_obj_bundle_buffer_bytes_cmp(left->payload,
                                                right->payload);

    if (cmp != 0) {
        return cmp;
    }

    return (left->fallback_policy_id > right->fallback_policy_id)
           - (left->fallback_policy_id < right->fallback_policy_id);
}

static bool
_n00b_obj_bundle_encoded_artifact_id(
    n00b_obj_bundle_encode_artifact_t *artifacts,
    size_t                             artifact_count,
    uint64_t                           internal_id,
    uint64_t                          *encoded_id)
{
    for (size_t i = 0; i < artifact_count; i++) {
        if (artifacts[i].artifact->id == internal_id) {
            *encoded_id = artifacts[i].encoded_id;
            return true;
        }
    }

    return false;
}

static bool
_n00b_obj_bundle_u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (UINT64_MAX - a < b) {
        return false;
    }

    *out = a + b;
    return true;
}

static bool
_n00b_obj_bundle_u64_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool
_n00b_obj_bundle_range_end(uint64_t off, uint64_t len, uint64_t *end)
{
    if (UINT64_MAX - off < len) {
        return false;
    }

    *end = off + len;
    return true;
}

static bool
_n00b_obj_bundle_range_within(uint64_t off, uint64_t len, uint64_t total)
{
    uint64_t end = 0;

    return _n00b_obj_bundle_range_end(off, len, &end) && end <= total;
}

static bool
_n00b_obj_bundle_is_aligned(uint64_t value, uint64_t alignment)
{
    return alignment != 0 && (value % alignment) == 0;
}

static bool
_n00b_obj_bundle_read_u16(n00b_bstream_t *stream, uint16_t *out)
{
    auto result = n00b_bstream_read_u16(stream);

    if (n00b_result_is_err(result)) {
        return false;
    }

    *out = n00b_result_get(result);
    return true;
}

static bool
_n00b_obj_bundle_read_u32(n00b_bstream_t *stream, uint32_t *out)
{
    auto result = n00b_bstream_read_u32(stream);

    if (n00b_result_is_err(result)) {
        return false;
    }

    *out = n00b_result_get(result);
    return true;
}

static bool
_n00b_obj_bundle_read_u64(n00b_bstream_t *stream, uint64_t *out)
{
    auto result = n00b_bstream_read_u64(stream);

    if (n00b_result_is_err(result)) {
        return false;
    }

    *out = n00b_result_get(result);
    return true;
}

static bool
_n00b_obj_bundle_read_digest(
    n00b_bstream_t *stream,
    uint8_t         out[N00B_OBJ_BUNDLE_DIGEST_LEN])
{
    if (!n00b_bstream_can_read(stream, N00B_OBJ_BUNDLE_DIGEST_LEN)) {
        return false;
    }

    memcpy(out, n00b_bstream_raw(stream), N00B_OBJ_BUNDLE_DIGEST_LEN);

    auto advance = n00b_bstream_advance(stream, N00B_OBJ_BUNDLE_DIGEST_LEN);

    return n00b_result_is_ok(advance);
}

static bool
_n00b_obj_bundle_setpos(n00b_bstream_t *stream, uint64_t off)
{
    if (off > SIZE_MAX) {
        return false;
    }

    auto result = n00b_bstream_setpos(stream, (size_t)off);

    return n00b_result_is_ok(result);
}

static bool
_n00b_obj_bundle_digest_is_zero(
    const uint8_t digest[N00B_OBJ_BUNDLE_DIGEST_LEN])
{
    for (size_t i = 0; i < N00B_OBJ_BUNDLE_DIGEST_LEN; i++) {
        if (digest[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool
_n00b_obj_bundle_decode_string(const uint8_t      *table,
                               uint64_t            table_len,
                               uint32_t            off,
                               bool                allow_empty,
                               n00b_allocator_t   *allocator,
                               n00b_string_t     **out)
{
    *out = nullptr;

    if (off == 0) {
        return allow_empty;
    }

    if (off >= table_len) {
        return false;
    }

    uint64_t len = 0;

    while ((uint64_t)off + len < table_len
           && table[(uint64_t)off + len] != 0) {
        len++;
    }

    if ((uint64_t)off + len >= table_len) {
        return false;
    }

    if (len == 0 || len > UINT32_MAX || len > INT64_MAX) {
        return false;
    }

    const void *raw = table + off;

    if (!n00b_unicode_utf8_validate(raw, (uint32_t)len)) {
        return false;
    }

    *out = n00b_string_from_raw(raw, (int64_t)len, .allocator = allocator);
    return true;
}

static n00b_buffer_t *
_n00b_obj_bundle_decode_payload_slice(const uint8_t    *payload_area,
                                      uint64_t          off,
                                      uint64_t          len,
                                      n00b_allocator_t *allocator)
{
    if (len > INT64_MAX) {
        return nullptr;
    }

    n00b_buffer_t *buffer = n00b_buffer_new((int64_t)len,
                                            .allocator = allocator);

    if (len != 0) {
        memcpy(buffer->data, payload_area + off, (size_t)len);
    }

    return buffer;
}

static void
_n00b_obj_bundle_write_digest(n00b_writer_t *writer,
                              const uint8_t digest[N00B_OBJ_BUNDLE_DIGEST_LEN])
{
    n00b_writer_write_bytes(writer, digest, N00B_OBJ_BUNDLE_DIGEST_LEN);
}

static void
_n00b_obj_bundle_write_zero_digest(n00b_writer_t *writer)
{
    n00b_writer_write_zeros(writer, N00B_OBJ_BUNDLE_DIGEST_LEN);
}

static void
_n00b_obj_bundle_write_artifact_record(
    n00b_writer_t                       *writer,
    n00b_obj_bundle_encode_artifact_t   *artifact)
{
    n00b_writer_write_u64(writer, artifact->encoded_id);
    n00b_writer_write_u32(writer, artifact->artifact->kind);
    n00b_writer_write_u32(writer, 0);
    n00b_writer_write_u64(writer, artifact->artifact->flags);
    n00b_writer_write_u32(writer, artifact->path_off);
    n00b_writer_write_u32(writer, artifact->role_off);
    n00b_writer_write_u32(writer, artifact->artifact->mode);
    n00b_writer_write_u32(writer, artifact->payload_index);

    if (artifact->artifact->payload == nullptr) {
        _n00b_obj_bundle_write_zero_digest(writer);
    }
    else {
        _n00b_obj_bundle_write_digest(writer, artifact->digest);
    }
}

static void
_n00b_obj_bundle_write_payload_record(
    n00b_writer_t                       *writer,
    n00b_obj_bundle_encode_artifact_t   *artifact)
{
    n00b_writer_write_u64(writer, artifact->encoded_id);
    n00b_writer_write_u64(writer, 0);
    n00b_writer_write_u64(writer, artifact->payload_off);
    n00b_writer_write_u64(writer, artifact->payload_len);
    _n00b_obj_bundle_write_digest(writer, artifact->digest);
}

static void
_n00b_obj_bundle_write_policy_record(n00b_writer_t                    *writer,
                                     n00b_obj_bundle_encode_policy_t  *policy)
{
    n00b_obj_bundle_policy_t *p = policy->policy;

    n00b_writer_write_u64(writer, p->policy_id);
    n00b_writer_write_u32(writer, p->kind);
    n00b_writer_write_u32(writer, p->scope);
    n00b_writer_write_u64(writer, p->flags);
    n00b_writer_write_u64(writer, p->priority);
    n00b_writer_write_u64(writer, policy->payload_off);
    n00b_writer_write_u64(writer, policy->payload_len);
    n00b_writer_write_u64(writer, p->fallback_policy_id);

    if (p->payload == nullptr) {
        _n00b_obj_bundle_write_zero_digest(writer);
    }
    else {
        _n00b_obj_bundle_write_digest(writer, policy->digest);
    }
}

n00b_result_t(n00b_obj_bundle_t *)
n00b_obj_bundle_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_obj_bundle_t *bundle =
        n00b_alloc_with_opts(n00b_obj_bundle_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});

    bundle->allocator        = allocator;
    bundle->is_mutable       = true;
    bundle->next_artifact_id = 0;
    bundle->artifacts =
        n00b_list_new_private(n00b_obj_bundle_artifact_t *,
                              .allocator = allocator,
                              .scan_kind = N00B_GC_SCAN_KIND_ALL);
    bundle->has_default_exec = false;
    bundle->default_exec_id  = N00B_OBJ_BUNDLE_ARTIFACT_ID_NONE;
    bundle->exec_mappings =
        n00b_list_new_private(n00b_obj_bundle_exec_mapping_t *,
                              .allocator = allocator,
                              .scan_kind = N00B_GC_SCAN_KIND_ALL);
    bundle->policies =
        n00b_list_new_private(n00b_obj_bundle_policy_t *,
                              .allocator = allocator,
                              .scan_kind = N00B_GC_SCAN_KIND_ALL);

    return n00b_result_ok(n00b_obj_bundle_t *, bundle);
}

n00b_result_t(bool)
n00b_obj_bundle_add_artifact(n00b_obj_bundle_t *bundle,
                             n00b_string_t     *logical_path,
                             const n00b_buffer_t *bytes) _kargs
{
    n00b_obj_bundle_artifact_kind_t kind  = N00B_OBJ_BUNDLE_ARTIFACT_FILE;
    n00b_string_t                  *role  = nullptr;
    uint32_t                        mode  = 0;
    uint64_t                        flags = 0;
}
{
    if (bundle == nullptr || logical_path == nullptr) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null artifact argument",
                              _n00b_obj_bundle_allocator(bundle));
    }

    n00b_allocator_t *allocator = bundle->allocator;

    if (!bundle->is_mutable
        || !_n00b_obj_bundle_artifact_kind_is_valid(kind)
        || !_n00b_obj_bundle_artifact_has_valid_payload(kind, bytes)) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid artifact argument",
                              allocator);
    }

    if (role != nullptr
        && (!_n00b_obj_bundle_string_len_is_supported(role)
            || !n00b_unicode_str_validate(role))) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid artifact role",
                              allocator);
    }

    auto normalized =
        _n00b_obj_bundle_normalize_logical_path(logical_path, allocator);

    if (n00b_result_is_err(normalized)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        normalized));
    }

    n00b_string_t *path = n00b_result_get(normalized);

    if (_n00b_obj_bundle_find_artifact_by_path(bundle, path) != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_DUPLICATE_LOGICAL_PATH,
                r"object bundle: duplicate logical path",
                path,
                allocator));
    }

    n00b_obj_bundle_artifact_t *artifact =
        n00b_alloc_with_opts(n00b_obj_bundle_artifact_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});

    artifact->id           = bundle->next_artifact_id++;
    artifact->kind         = kind;
    artifact->logical_path = path;
    artifact->payload      = _n00b_obj_bundle_copy_buffer(bytes, allocator);
    artifact->role         = _n00b_obj_bundle_copy_string(role, allocator);
    artifact->mode         = mode;
    artifact->flags        = flags;

    n00b_list_push(bundle->artifacts, artifact);

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_obj_bundle_set_default_exec(n00b_obj_bundle_t *bundle,
                                 n00b_string_t     *logical_path)
{
    if (bundle == nullptr || logical_path == nullptr) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null default executable argument",
                              _n00b_obj_bundle_allocator(bundle));
    }

    n00b_allocator_t *allocator = bundle->allocator;

    if (!bundle->is_mutable) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: immutable bundle",
                              allocator);
    }

    auto normalized =
        _n00b_obj_bundle_normalize_logical_path(logical_path, allocator);

    if (n00b_result_is_err(normalized)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        normalized));
    }

    n00b_string_t *path = n00b_result_get(normalized);

    if (bundle->has_default_exec) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_MULTIPLE_DEFAULT_EXEC,
                r"object bundle: default executable already exists",
                path,
                allocator));
    }

    n00b_obj_bundle_artifact_t *artifact =
        _n00b_obj_bundle_find_artifact_by_path(bundle, path);

    if (!_n00b_obj_bundle_artifact_is_executable(artifact)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                r"object bundle: default executable target is missing",
                path,
                allocator));
    }

    bundle->has_default_exec = true;
    bundle->default_exec_id  = artifact->id;

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_obj_bundle_add_exec_mapping(n00b_obj_bundle_t *bundle,
                                 n00b_string_t     *selector,
                                 n00b_string_t     *target_path)
{
    if (bundle == nullptr || selector == nullptr || target_path == nullptr) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null execution mapping argument",
                              _n00b_obj_bundle_allocator(bundle));
    }

    n00b_allocator_t *allocator = bundle->allocator;

    if (!bundle->is_mutable || !_n00b_obj_bundle_selector_is_valid(selector)) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid execution selector",
                              allocator);
    }

    if (_n00b_obj_bundle_find_mapping_by_selector(bundle, selector) != nullptr) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_DUPLICATE_SELECTOR,
                              r"object bundle: duplicate execution selector",
                              allocator);
    }

    auto normalized =
        _n00b_obj_bundle_normalize_logical_path(target_path, allocator);

    if (n00b_result_is_err(normalized)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        normalized));
    }

    n00b_string_t *path = n00b_result_get(normalized);

    n00b_obj_bundle_artifact_t *artifact =
        _n00b_obj_bundle_find_artifact_by_path(bundle, path);

    if (!_n00b_obj_bundle_artifact_is_executable(artifact)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_path(
                N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                r"object bundle: execution mapping target is missing",
                path,
                allocator));
    }

    n00b_obj_bundle_exec_mapping_t *mapping =
        n00b_alloc_with_opts(n00b_obj_bundle_exec_mapping_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});

    mapping->selector =
        _n00b_obj_bundle_copy_string(selector, allocator);
    mapping->target_artifact_id = artifact->id;

    n00b_list_push(bundle->exec_mappings, mapping);

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_obj_bundle_add_policy(n00b_obj_bundle_t             *bundle,
                           uint64_t                      policy_id,
                           n00b_obj_bundle_policy_kind_t  kind,
                           n00b_obj_bundle_policy_scope_t scope) _kargs
{
    uint64_t       flags              = N00B_OBJ_BUNDLE_POLICY_F_REQUIRED;
    uint64_t       priority           = 0;
    const n00b_buffer_t *payload      = nullptr;
    uint64_t       fallback_policy_id = N00B_OBJ_BUNDLE_POLICY_ID_NONE;
}
{
    if (bundle == nullptr
        || policy_id == N00B_OBJ_BUNDLE_POLICY_ID_NONE
        || !_n00b_obj_bundle_policy_kind_is_supported(kind)
        || !_n00b_obj_bundle_policy_scope_is_valid(scope)) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid policy argument",
                              _n00b_obj_bundle_allocator(bundle));
    }

    n00b_allocator_t *allocator = bundle->allocator;

    if (!bundle->is_mutable
        || !_n00b_obj_bundle_policy_flags_are_valid(flags)
        || !_n00b_obj_bundle_policy_payload_is_valid(kind,
                                                     payload,
                                                     fallback_policy_id)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_policy(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid policy record",
                kind,
                scope,
                policy_id,
                allocator));
    }

    if (_n00b_obj_bundle_find_policy_by_id(bundle, policy_id) != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_policy(
                N00B_OBJ_BUNDLE_ERR_DUPLICATE_POLICY_ID,
                r"object bundle: duplicate policy ID",
                kind,
                scope,
                policy_id,
                allocator));
    }

    n00b_obj_bundle_policy_t candidate = {
        .policy_id          = policy_id,
        .kind               = kind,
        .scope              = scope,
        .flags              = flags,
        .priority           = priority,
        .payload            = payload,
        .fallback_policy_id = fallback_policy_id,
    };

    if (!_n00b_obj_bundle_policy_fallback_is_valid(bundle, &candidate)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_error_with_policy(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid policy fallback",
                kind,
                scope,
                fallback_policy_id,
                allocator));
    }

    n00b_obj_bundle_policy_t *policy =
        n00b_alloc_with_opts(n00b_obj_bundle_policy_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});

    policy->policy_id = policy_id;
    policy->kind      = kind;
    policy->scope     = scope;
    policy->flags     = flags;
    policy->priority  = priority;
    policy->payload   = _n00b_obj_bundle_copy_buffer(payload, allocator);
    policy->fallback_policy_id = fallback_policy_id;

    n00b_list_push(bundle->policies, policy);

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
_n00b_obj_bundle_validate_artifacts(n00b_obj_bundle_t *bundle)
{
    size_t n = n00b_list_len(bundle->artifacts);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_artifact_t *artifact =
            n00b_list_get(bundle->artifacts, i);

        if (artifact == nullptr
            || !_n00b_obj_bundle_artifact_kind_is_valid(artifact->kind)
            || !_n00b_obj_bundle_artifact_has_valid_payload(artifact->kind,
                                                            artifact->payload)) {
            return OBJ_BUNDLE_ERR(bool,
                                  N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                                  r"object bundle: invalid artifact record",
                                  bundle->allocator);
        }

        auto path =
            _n00b_obj_bundle_normalize_logical_path(artifact->logical_path,
                                                   bundle->allocator);

        if (n00b_result_is_err(path)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                n00b_result_get_err_payload(n00b_obj_bundle_error_t *, path));
        }

        for (size_t j = i + 1; j < n; j++) {
            n00b_obj_bundle_artifact_t *other =
                n00b_list_get(bundle->artifacts, j);

            if (_n00b_obj_bundle_string_bytes_eq(artifact->logical_path,
                                                 other->logical_path)) {
                return OBJ_BUNDLE_ERR_PAYLOAD(
                    bool,
                    _n00b_obj_bundle_error_with_path(
                        N00B_OBJ_BUNDLE_ERR_DUPLICATE_LOGICAL_PATH,
                        r"object bundle: duplicate logical path",
                        artifact->logical_path,
                        bundle->allocator));
            }
        }
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
_n00b_obj_bundle_validate_exec_map(n00b_obj_bundle_t *bundle)
{
    if (bundle->has_default_exec) {
        n00b_obj_bundle_artifact_t *artifact =
            _n00b_obj_bundle_find_artifact_by_id(bundle,
                                                bundle->default_exec_id);

        if (!_n00b_obj_bundle_artifact_is_executable(artifact)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                _n00b_obj_bundle_error_with_artifact(
                    N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                    r"object bundle: invalid default executable target",
                    bundle->default_exec_id,
                    bundle->allocator));
        }
    }

    size_t n = n00b_list_len(bundle->exec_mappings);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_exec_mapping_t *mapping =
            n00b_list_get(bundle->exec_mappings, i);

        if (mapping == nullptr
            || !_n00b_obj_bundle_selector_is_valid(mapping->selector)) {
            return OBJ_BUNDLE_ERR(bool,
                                  N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                                  r"object bundle: invalid execution selector",
                                  bundle->allocator);
        }

        n00b_obj_bundle_artifact_t *artifact =
            _n00b_obj_bundle_find_artifact_by_id(bundle,
                                                mapping->target_artifact_id);

        if (!_n00b_obj_bundle_artifact_is_executable(artifact)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                _n00b_obj_bundle_error_with_artifact(
                    N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                    r"object bundle: invalid execution mapping target",
                    mapping->target_artifact_id,
                    bundle->allocator));
        }

        for (size_t j = i + 1; j < n; j++) {
            n00b_obj_bundle_exec_mapping_t *other =
                n00b_list_get(bundle->exec_mappings, j);

            if (_n00b_obj_bundle_string_bytes_eq(mapping->selector,
                                                 other->selector)) {
                return OBJ_BUNDLE_ERR(bool,
                                      N00B_OBJ_BUNDLE_ERR_DUPLICATE_SELECTOR,
                                      r"object bundle: duplicate selector",
                                      bundle->allocator);
            }
        }
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
_n00b_obj_bundle_validate_policies(n00b_obj_bundle_t *bundle)
{
    size_t n = n00b_list_len(bundle->policies);

    for (size_t i = 0; i < n; i++) {
        n00b_obj_bundle_policy_t *policy = n00b_list_get(bundle->policies, i);

        if (policy == nullptr
            || !_n00b_obj_bundle_policy_kind_is_supported(policy->kind)
            || !_n00b_obj_bundle_policy_scope_is_valid(policy->scope)
            || !_n00b_obj_bundle_policy_flags_are_valid(policy->flags)
            || !_n00b_obj_bundle_policy_payload_is_valid(policy->kind,
                                                         policy->payload,
                                                         policy->fallback_policy_id)) {
            return OBJ_BUNDLE_ERR(bool,
                                  N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                                  r"object bundle: invalid policy record",
                                  bundle->allocator);
        }

        if (!_n00b_obj_bundle_policy_fallback_is_valid(bundle, policy)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                _n00b_obj_bundle_error_with_policy(
                    N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                    r"object bundle: invalid policy fallback",
                    policy->kind,
                    policy->scope,
                    policy->fallback_policy_id,
                    bundle->allocator));
        }

        for (size_t j = i + 1; j < n; j++) {
            n00b_obj_bundle_policy_t *other = n00b_list_get(bundle->policies,
                                                            j);

            if (policy->policy_id == other->policy_id) {
                return OBJ_BUNDLE_ERR_PAYLOAD(
                    bool,
                    _n00b_obj_bundle_error_with_policy(
                        N00B_OBJ_BUNDLE_ERR_DUPLICATE_POLICY_ID,
                        r"object bundle: duplicate policy ID",
                        policy->kind,
                        policy->scope,
                        policy->policy_id,
                        bundle->allocator));
            }
        }
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_obj_bundle_validate(n00b_obj_bundle_t *bundle) _kargs
{
    bool strict = true;
}
{
    (void)strict;

    if (bundle == nullptr) {
        return OBJ_BUNDLE_ERR(bool,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null bundle",
                              nullptr);
    }

    auto artifacts = _n00b_obj_bundle_validate_artifacts(bundle);

    if (n00b_result_is_err(artifacts)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        artifacts));
    }

    auto exec_map = _n00b_obj_bundle_validate_exec_map(bundle);

    if (n00b_result_is_err(exec_map)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        exec_map));
    }

    auto policies = _n00b_obj_bundle_validate_policies(bundle);

    if (n00b_result_is_err(policies)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        policies));
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_buffer_t *)
n00b_obj_bundle_encode(n00b_obj_bundle_t *bundle) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bundle == nullptr) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null bundle",
                              allocator);
    }

    auto valid = n00b_obj_bundle_validate(bundle);

    if (n00b_result_is_err(valid)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, valid));
    }

    size_t artifact_count = n00b_list_len(bundle->artifacts);
    size_t mapping_count  = n00b_list_len(bundle->exec_mappings);
    size_t policy_count   = n00b_list_len(bundle->policies);
    size_t exec_count     = mapping_count + (bundle->has_default_exec ? 1 : 0);

    n00b_obj_bundle_encode_artifact_t *artifacts = nullptr;
    n00b_obj_bundle_encode_exec_mapping_t *mappings = nullptr;
    n00b_obj_bundle_encode_policy_t *policies = nullptr;

    if (artifact_count != 0) {
        artifacts = n00b_alloc_array(n00b_obj_bundle_encode_artifact_t,
                                     artifact_count);

        for (size_t i = 0; i < artifact_count; i++) {
            artifacts[i].artifact = n00b_list_get(bundle->artifacts, i);
        }

        qsort(artifacts,
              artifact_count,
              sizeof(artifacts[0]),
              _n00b_obj_bundle_encode_artifact_cmp);

        for (size_t i = 0; i < artifact_count; i++) {
            artifacts[i].encoded_id = i;
        }
    }

    if (mapping_count != 0) {
        mappings = n00b_alloc_array(n00b_obj_bundle_encode_exec_mapping_t,
                                    mapping_count);

        for (size_t i = 0; i < mapping_count; i++) {
            mappings[i].mapping = n00b_list_get(bundle->exec_mappings, i);
        }

        qsort(mappings,
              mapping_count,
              sizeof(mappings[0]),
              _n00b_obj_bundle_encode_mapping_cmp);
    }

    if (policy_count != 0) {
        policies = n00b_alloc_array(n00b_obj_bundle_encode_policy_t,
                                    policy_count);

        for (size_t i = 0; i < policy_count; i++) {
            policies[i].policy = n00b_list_get(bundle->policies, i);
        }

        qsort(policies,
              policy_count,
              sizeof(policies[0]),
              _n00b_obj_bundle_encode_policy_cmp);
    }

    n00b_obj_bundle_manifest_strtab_t *strings =
        _n00b_obj_bundle_manifest_strtab_new();
    uint32_t payload_index = 0;

    for (size_t i = 0; i < artifact_count; i++) {
        n00b_obj_bundle_artifact_t *artifact = artifacts[i].artifact;

        artifacts[i].path_off =
            _n00b_obj_bundle_manifest_strtab_add(strings,
                                                 artifact->logical_path);
        artifacts[i].role_off =
            _n00b_obj_bundle_manifest_strtab_add(strings, artifact->role);

        if (artifact->payload == nullptr) {
            artifacts[i].payload_index = N00B_OBJ_BUNDLE_NO_PAYLOAD_INDEX;
            continue;
        }

        artifacts[i].payload_index = payload_index++;
        artifacts[i].payload_len   = artifact->payload->byte_len;
        _n00b_obj_bundle_sha256_bytes(artifact->payload->data,
                                      artifact->payload->byte_len,
                                      artifacts[i].digest);
    }

    size_t payload_count = payload_index;

    for (size_t i = 0; i < mapping_count; i++) {
        mappings[i].selector_off =
            _n00b_obj_bundle_manifest_strtab_add(strings,
                                                 mappings[i].mapping->selector);

        if (!_n00b_obj_bundle_encoded_artifact_id(
                artifacts,
                artifact_count,
                mappings[i].mapping->target_artifact_id,
                &mappings[i].target_encoded_id)) {
            return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                                  N00B_OBJ_BUNDLE_ERR_BUILD,
                                  r"object bundle: missing encoded target",
                                  allocator);
        }
    }

    uint64_t default_encoded_id = N00B_OBJ_BUNDLE_ARTIFACT_ID_NONE;

    if (bundle->has_default_exec
        && !_n00b_obj_bundle_encoded_artifact_id(artifacts,
                                                artifact_count,
                                                bundle->default_exec_id,
                                                &default_encoded_id)) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_BUILD,
                              r"object bundle: missing encoded default target",
                              allocator);
    }

    if (strings->error) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_BUILD,
                              r"object bundle: string table build failed",
                              allocator);
    }

    uint64_t payload_cursor = 0;

    for (size_t i = 0; i < artifact_count; i++) {
        n00b_obj_bundle_artifact_t *artifact = artifacts[i].artifact;

        if (artifact->payload == nullptr) {
            continue;
        }

        artifacts[i].payload_off = payload_cursor;

        if (!_n00b_obj_bundle_u64_add(payload_cursor,
                                      artifact->payload->byte_len,
                                      &payload_cursor)) {
            return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                                  N00B_OBJ_BUNDLE_ERR_BUILD,
                                  r"object bundle: payload area too large",
                                  allocator);
        }
    }

    for (size_t i = 0; i < policy_count; i++) {
        n00b_obj_bundle_policy_t *policy = policies[i].policy;

        if (policy->payload == nullptr) {
            continue;
        }

        policies[i].payload_off = payload_cursor;
        policies[i].payload_len = policy->payload->byte_len;
        _n00b_obj_bundle_sha256_bytes(policy->payload->data,
                                      policy->payload->byte_len,
                                      policies[i].digest);

        if (!_n00b_obj_bundle_u64_add(payload_cursor,
                                      policy->payload->byte_len,
                                      &payload_cursor)) {
            return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                                  N00B_OBJ_BUNDLE_ERR_BUILD,
                                  r"object bundle: payload area too large",
                                  allocator);
        }
    }

    uint64_t artifact_table_len = 0;
    uint64_t payload_table_len  = 0;
    uint64_t exec_table_len     = 0;
    uint64_t policy_table_len   = 0;

    if (!_n00b_obj_bundle_u64_mul(artifact_count,
                                  N00B_OBJ_BUNDLE_ARTIFACT_REC_SIZE,
                                  &artifact_table_len)
        || !_n00b_obj_bundle_u64_mul(payload_count,
                                     N00B_OBJ_BUNDLE_PAYLOAD_REC_SIZE,
                                     &payload_table_len)
        || !_n00b_obj_bundle_u64_mul(exec_count,
                                     N00B_OBJ_BUNDLE_EXEC_REC_SIZE,
                                     &exec_table_len)
        || !_n00b_obj_bundle_u64_mul(policy_count,
                                     N00B_OBJ_BUNDLE_POLICY_REC_SIZE,
                                     &policy_table_len)) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_BUILD,
                              r"object bundle: table too large",
                              allocator);
    }

    uint64_t artifact_table_off = N00B_OBJ_BUNDLE_HEADER_SIZE;
    uint64_t payload_table_off =
        _n00b_obj_bundle_align_u64(artifact_table_off + artifact_table_len, 8);
    uint64_t exec_table_off =
        _n00b_obj_bundle_align_u64(payload_table_off + payload_table_len, 8);
    uint64_t policy_table_off =
        _n00b_obj_bundle_align_u64(exec_table_off + exec_table_len, 8);
    uint64_t string_table_off =
        _n00b_obj_bundle_align_u64(policy_table_off + policy_table_len, 8);
    uint64_t payload_area_off =
        _n00b_obj_bundle_align_u64(string_table_off + strings->len, 8);
    uint64_t extension_table_off = payload_area_off + payload_cursor;
    uint64_t total_len          = extension_table_off;

    if (total_len > INT64_MAX) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_BUILD,
                              r"object bundle: manifest too large",
                              allocator);
    }

    n00b_writer_t *writer = n00b_writer_new((size_t)total_len);
    n00b_writer_set_endian(writer, N00B_ENDIAN_LITTLE);

    n00b_writer_write_bytes(writer,
                            N00B_OBJ_BUNDLE_MANIFEST_MAGIC,
                            N00B_OBJ_BUNDLE_MANIFEST_MAGIC_LEN);
    n00b_writer_write_u16(writer, N00B_OBJ_BUNDLE_MANIFEST_MAJOR);
    n00b_writer_write_u16(writer, N00B_OBJ_BUNDLE_MANIFEST_MINOR);
    n00b_writer_write_u32(writer, N00B_OBJ_BUNDLE_HEADER_SIZE);
    n00b_writer_write_u64(writer, total_len);
    n00b_writer_write_u64(writer, 0);
    n00b_writer_write_zeros(writer, N00B_OBJ_BUNDLE_CONTENT_ID_LEN);
    n00b_writer_write_u64(writer, artifact_count);
    n00b_writer_write_u64(writer, payload_count);
    n00b_writer_write_u64(writer, exec_count);
    n00b_writer_write_u64(writer, policy_count);
    n00b_writer_write_u64(writer, artifact_table_off);
    n00b_writer_write_u64(writer, artifact_table_len);
    n00b_writer_write_u64(writer, payload_table_off);
    n00b_writer_write_u64(writer, payload_table_len);
    n00b_writer_write_u64(writer, exec_table_off);
    n00b_writer_write_u64(writer, exec_table_len);
    n00b_writer_write_u64(writer, policy_table_off);
    n00b_writer_write_u64(writer, policy_table_len);
    n00b_writer_write_u64(writer, string_table_off);
    n00b_writer_write_u64(writer, strings->len);
    n00b_writer_write_u64(writer, payload_area_off);
    n00b_writer_write_u64(writer, payload_cursor);
    n00b_writer_write_u64(writer, extension_table_off);
    n00b_writer_write_u64(writer, 0);

    n00b_writer_setpos(writer, artifact_table_off);

    for (size_t i = 0; i < artifact_count; i++) {
        _n00b_obj_bundle_write_artifact_record(writer, &artifacts[i]);
    }

    n00b_writer_setpos(writer, payload_table_off);

    for (size_t i = 0; i < artifact_count; i++) {
        if (artifacts[i].artifact->payload != nullptr) {
            _n00b_obj_bundle_write_payload_record(writer, &artifacts[i]);
        }
    }

    n00b_writer_setpos(writer, exec_table_off);

    if (bundle->has_default_exec) {
        n00b_writer_write_u32(writer, N00B_OBJ_BUNDLE_EXEC_REC_DEFAULT);
        n00b_writer_write_u32(writer, 0);
        n00b_writer_write_u32(writer, 0);
        n00b_writer_write_u32(writer, 0);
        n00b_writer_write_u64(writer, default_encoded_id);
    }

    for (size_t i = 0; i < mapping_count; i++) {
        n00b_writer_write_u32(writer, N00B_OBJ_BUNDLE_EXEC_REC_SELECTOR);
        n00b_writer_write_u32(writer, 0);
        n00b_writer_write_u32(writer, mappings[i].selector_off);
        n00b_writer_write_u32(writer, 0);
        n00b_writer_write_u64(writer, mappings[i].target_encoded_id);
    }

    n00b_writer_setpos(writer, policy_table_off);

    for (size_t i = 0; i < policy_count; i++) {
        _n00b_obj_bundle_write_policy_record(writer, &policies[i]);
    }

    n00b_writer_setpos(writer, string_table_off);
    n00b_writer_write_bytes(writer, strings->data, strings->len);
    n00b_writer_setpos(writer, payload_area_off);

    for (size_t i = 0; i < artifact_count; i++) {
        const n00b_buffer_t *payload = artifacts[i].artifact->payload;

        if (payload != nullptr && payload->byte_len != 0) {
            n00b_writer_write_bytes(writer, payload->data, payload->byte_len);
        }
    }

    for (size_t i = 0; i < policy_count; i++) {
        const n00b_buffer_t *payload = policies[i].policy->payload;

        if (payload != nullptr && payload->byte_len != 0) {
            n00b_writer_write_bytes(writer, payload->data, payload->byte_len);
        }
    }

    n00b_writer_setpos(writer, total_len);

    if (n00b_writer_has_error(writer)) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_BUILD,
                              r"object bundle: writer failed",
                              allocator);
    }

    n00b_buffer_t *encoded = n00b_writer_finalize(writer);
    uint8_t        content_id[N00B_OBJ_BUNDLE_CONTENT_ID_LEN];

    memset(encoded->data + N00B_OBJ_BUNDLE_CONTENT_ID_OFF,
           0,
           N00B_OBJ_BUNDLE_CONTENT_ID_LEN);
    _n00b_obj_bundle_sha256_bytes(encoded->data, encoded->byte_len, content_id);
    memcpy(encoded->data + N00B_OBJ_BUNDLE_CONTENT_ID_OFF,
           content_id,
           N00B_OBJ_BUNDLE_CONTENT_ID_LEN);

    if (allocator != nullptr) {
        encoded = n00b_buffer_copy(encoded, .allocator = allocator);
    }

    return n00b_result_ok(n00b_buffer_t *, encoded);
}

n00b_result_t(n00b_obj_bundle_t *)
n00b_obj_bundle_decode(n00b_buffer_t *bundle_bytes) _kargs
{
    bool              strict    = true;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bundle_bytes == nullptr || bundle_bytes->data == nullptr) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: null manifest bytes",
                              allocator);
    }

    if (bundle_bytes->byte_len < N00B_OBJ_BUNDLE_HEADER_SIZE
        || bundle_bytes->byte_len > INT64_MAX) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: manifest header is malformed",
                              allocator);
    }

    const uint8_t *data      = (const uint8_t *)bundle_bytes->data;
    uint64_t       total_len = (uint64_t)bundle_bytes->byte_len;

    if (memcmp(data,
               N00B_OBJ_BUNDLE_MANIFEST_MAGIC,
               N00B_OBJ_BUNDLE_MANIFEST_MAGIC_LEN) != 0) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: manifest magic mismatch",
                              allocator);
    }

    n00b_bstream_t *stream = n00b_bstream_new(bundle_bytes,
                                              .allocator = allocator);
    n00b_bstream_set_endian(stream, N00B_ENDIAN_LITTLE);

    uint16_t major       = 0;
    uint16_t minor       = 0;
    uint32_t header_size = 0;
    uint64_t manifest_len = 0;
    uint64_t manifest_flags = 0;
    uint64_t artifact_count = 0;
    uint64_t payload_count  = 0;
    uint64_t exec_count     = 0;
    uint64_t policy_count   = 0;
    uint8_t  content_id[N00B_OBJ_BUNDLE_CONTENT_ID_LEN];
    n00b_obj_bundle_manifest_range_t artifact_table = {};
    n00b_obj_bundle_manifest_range_t payload_table  = {};
    n00b_obj_bundle_manifest_range_t exec_table     = {};
    n00b_obj_bundle_manifest_range_t policy_table   = {};
    n00b_obj_bundle_manifest_range_t string_table   = {};
    n00b_obj_bundle_manifest_range_t payload_area   = {};
    n00b_obj_bundle_manifest_range_t extension_table = {};

    if (!_n00b_obj_bundle_setpos(stream, 8)
        || !_n00b_obj_bundle_read_u16(stream, &major)
        || !_n00b_obj_bundle_read_u16(stream, &minor)
        || !_n00b_obj_bundle_read_u32(stream, &header_size)
        || !_n00b_obj_bundle_read_u64(stream, &manifest_len)
        || !_n00b_obj_bundle_read_u64(stream, &manifest_flags)
        || !_n00b_obj_bundle_setpos(stream, 64)
        || !_n00b_obj_bundle_read_u64(stream, &artifact_count)
        || !_n00b_obj_bundle_read_u64(stream, &payload_count)
        || !_n00b_obj_bundle_read_u64(stream, &exec_count)
        || !_n00b_obj_bundle_read_u64(stream, &policy_count)
        || !_n00b_obj_bundle_read_u64(stream, &artifact_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &artifact_table.len)
        || !_n00b_obj_bundle_read_u64(stream, &payload_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &payload_table.len)
        || !_n00b_obj_bundle_read_u64(stream, &exec_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &exec_table.len)
        || !_n00b_obj_bundle_read_u64(stream, &policy_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &policy_table.len)
        || !_n00b_obj_bundle_read_u64(stream, &string_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &string_table.len)
        || !_n00b_obj_bundle_read_u64(stream, &payload_area.off)
        || !_n00b_obj_bundle_read_u64(stream, &payload_area.len)
        || !_n00b_obj_bundle_read_u64(stream, &extension_table.off)
        || !_n00b_obj_bundle_read_u64(stream, &extension_table.len)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: manifest header is truncated",
                              allocator);
    }

    memcpy(content_id,
           data + N00B_OBJ_BUNDLE_CONTENT_ID_OFF,
           N00B_OBJ_BUNDLE_CONTENT_ID_LEN);

    if (major != N00B_OBJ_BUNDLE_MANIFEST_MAJOR
        || minor != N00B_OBJ_BUNDLE_MANIFEST_MINOR) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_VERSION,
                              r"object bundle: unsupported manifest version",
                              allocator);
    }

    if (header_size != N00B_OBJ_BUNDLE_HEADER_SIZE) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: unsupported header size",
                              allocator);
    }

    if (manifest_len != total_len) {
        n00b_obj_bundle_error_code_t code =
            manifest_len > total_len
                ? N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS
                : N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST;
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              code,
                              r"object bundle: manifest length mismatch",
                              allocator);
    }

    if (manifest_flags != 0) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE,
                              r"object bundle: unsupported manifest flags",
                              allocator);
    }

    uint64_t artifact_table_len = 0;
    uint64_t payload_table_len  = 0;
    uint64_t exec_table_len     = 0;
    uint64_t policy_table_len   = 0;

    if (!_n00b_obj_bundle_u64_mul(artifact_count,
                                  N00B_OBJ_BUNDLE_ARTIFACT_REC_SIZE,
                                  &artifact_table_len)
        || !_n00b_obj_bundle_u64_mul(payload_count,
                                     N00B_OBJ_BUNDLE_PAYLOAD_REC_SIZE,
                                     &payload_table_len)
        || !_n00b_obj_bundle_u64_mul(exec_count,
                                     N00B_OBJ_BUNDLE_EXEC_REC_SIZE,
                                     &exec_table_len)
        || !_n00b_obj_bundle_u64_mul(policy_count,
                                     N00B_OBJ_BUNDLE_POLICY_REC_SIZE,
                                     &policy_table_len)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: manifest table is too large",
                              allocator);
    }

    if (artifact_table.len != artifact_table_len
        || payload_table.len != payload_table_len
        || exec_table.len != exec_table_len
        || policy_table.len != policy_table_len) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: manifest table length mismatch",
                              allocator);
    }

    if (!_n00b_obj_bundle_range_within(artifact_table.off,
                                       artifact_table.len,
                                       total_len)
        || !_n00b_obj_bundle_range_within(payload_table.off,
                                          payload_table.len,
                                          total_len)
        || !_n00b_obj_bundle_range_within(exec_table.off,
                                          exec_table.len,
                                          total_len)
        || !_n00b_obj_bundle_range_within(policy_table.off,
                                          policy_table.len,
                                          total_len)
        || !_n00b_obj_bundle_range_within(string_table.off,
                                          string_table.len,
                                          total_len)
        || !_n00b_obj_bundle_range_within(payload_area.off,
                                          payload_area.len,
                                          total_len)
        || !_n00b_obj_bundle_range_within(extension_table.off,
                                          extension_table.len,
                                          total_len)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: manifest table is out of bounds",
                              allocator);
    }

    uint64_t expected_artifact_off = N00B_OBJ_BUNDLE_HEADER_SIZE;
    uint64_t expected_payload_off =
        _n00b_obj_bundle_align_u64(expected_artifact_off
                                       + artifact_table.len,
                                   8);
    uint64_t expected_exec_off =
        _n00b_obj_bundle_align_u64(expected_payload_off + payload_table.len,
                                   8);
    uint64_t expected_policy_off =
        _n00b_obj_bundle_align_u64(expected_exec_off + exec_table.len, 8);
    uint64_t expected_string_off =
        _n00b_obj_bundle_align_u64(expected_policy_off + policy_table.len, 8);
    uint64_t expected_payload_area_off =
        _n00b_obj_bundle_align_u64(expected_string_off + string_table.len, 8);
    uint64_t expected_extension_off = expected_payload_area_off
                                      + payload_area.len;

    if (artifact_table.off != expected_artifact_off
        || payload_table.off != expected_payload_off
        || exec_table.off != expected_exec_off
        || policy_table.off != expected_policy_off
        || string_table.off != expected_string_off
        || payload_area.off != expected_payload_area_off
        || extension_table.off != expected_extension_off
        || extension_table.len != 0
        || extension_table.off != total_len
        || !_n00b_obj_bundle_is_aligned(artifact_table.off, 8)
        || !_n00b_obj_bundle_is_aligned(payload_table.off, 8)
        || !_n00b_obj_bundle_is_aligned(exec_table.off, 8)
        || !_n00b_obj_bundle_is_aligned(policy_table.off, 8)
        || !_n00b_obj_bundle_is_aligned(string_table.off, 8)
        || !_n00b_obj_bundle_is_aligned(payload_area.off, 8)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                              r"object bundle: manifest table order is non-canonical",
                              allocator);
    }

    if (string_table.len == 0 || data[string_table.off] != 0) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                              r"object bundle: string table is malformed",
                              allocator);
    }

    n00b_buffer_t *content_copy = n00b_buffer_copy(bundle_bytes);
    uint8_t        expected_content_id[N00B_OBJ_BUNDLE_CONTENT_ID_LEN];

    memset(content_copy->data + N00B_OBJ_BUNDLE_CONTENT_ID_OFF,
           0,
           N00B_OBJ_BUNDLE_CONTENT_ID_LEN);
    _n00b_obj_bundle_sha256_bytes(content_copy->data,
                                  content_copy->byte_len,
                                  expected_content_id);

    if (memcmp(content_id,
               expected_content_id,
               N00B_OBJ_BUNDLE_CONTENT_ID_LEN) != 0) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH,
                              r"object bundle: content ID mismatch",
                              allocator);
    }

    const uint8_t *string_base  = data + string_table.off;
    const uint8_t *payload_base = data + payload_area.off;

    n00b_obj_bundle_decode_artifact_t *artifacts = nullptr;
    n00b_obj_bundle_decode_payload_t  *payloads  = nullptr;
    n00b_obj_bundle_decode_exec_t     *execs     = nullptr;
    n00b_obj_bundle_decode_policy_t   *policies  = nullptr;

    if (artifact_count != 0) {
        artifacts = n00b_alloc_array(n00b_obj_bundle_decode_artifact_t,
                                     (size_t)artifact_count);
    }

    if (payload_count != 0) {
        payloads = n00b_alloc_array(n00b_obj_bundle_decode_payload_t,
                                    (size_t)payload_count);
    }

    if (exec_count != 0) {
        execs = n00b_alloc_array(n00b_obj_bundle_decode_exec_t,
                                 (size_t)exec_count);
    }

    if (policy_count != 0) {
        policies = n00b_alloc_array(n00b_obj_bundle_decode_policy_t,
                                    (size_t)policy_count);
    }

    if (!_n00b_obj_bundle_setpos(stream, artifact_table.off)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: artifact table is out of bounds",
                              allocator);
    }

    for (uint64_t i = 0; i < artifact_count; i++) {
        n00b_obj_bundle_decode_artifact_t *artifact = &artifacts[i];
        uint32_t kind = 0;

        if (!_n00b_obj_bundle_read_u64(stream, &artifact->id)
            || !_n00b_obj_bundle_read_u32(stream, &kind)
            || !_n00b_obj_bundle_read_u32(stream, &artifact->record_flags)
            || !_n00b_obj_bundle_read_u64(stream, &artifact->flags)
            || !_n00b_obj_bundle_read_u32(stream, &artifact->path_off)
            || !_n00b_obj_bundle_read_u32(stream, &artifact->role_off)
            || !_n00b_obj_bundle_read_u32(stream, &artifact->mode)
            || !_n00b_obj_bundle_read_u32(stream, &artifact->payload_index)
            || !_n00b_obj_bundle_read_digest(stream, artifact->digest)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: artifact record is truncated",
                                  allocator);
        }

        artifact->kind = (n00b_obj_bundle_artifact_kind_t)kind;

        if (artifact->id != i) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: artifact IDs are non-canonical",
                                  allocator);
        }

        if (!_n00b_obj_bundle_artifact_kind_is_valid(artifact->kind)
            || artifact->record_flags != 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE,
                                  r"object bundle: unsupported artifact record",
                                  allocator);
        }

        if (!_n00b_obj_bundle_decode_string(string_base,
                                            string_table.len,
                                            artifact->path_off,
                                            false,
                                            allocator,
                                            &artifact->logical_path)
            || !_n00b_obj_bundle_decode_string(string_base,
                                               string_table.len,
                                               artifact->role_off,
                                               true,
                                               allocator,
                                               &artifact->role)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                                  r"object bundle: artifact string reference is invalid",
                                  allocator);
        }

        if (i != 0) {
            int cmp = _n00b_obj_bundle_string_bytes_cmp(
                artifacts[i - 1].logical_path,
                artifact->logical_path);

            if (cmp == 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_DUPLICATE_LOGICAL_PATH,
                                      r"object bundle: duplicate artifact path",
                                      allocator);
            }

            if (cmp > 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                      r"object bundle: artifact order is non-canonical",
                                      allocator);
            }
        }

        if (artifact->payload_index == N00B_OBJ_BUNDLE_NO_PAYLOAD_INDEX) {
            if (!_n00b_obj_bundle_digest_is_zero(artifact->digest)) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH,
                                      r"object bundle: empty artifact digest is nonzero",
                                      allocator);
            }
        }
        else if (artifact->payload_index >= payload_count) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: artifact payload index is out of bounds",
                                  allocator);
        }
    }

    uint64_t payload_cursor = 0;

    if (!_n00b_obj_bundle_setpos(stream, payload_table.off)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: payload table is out of bounds",
                              allocator);
    }

    for (uint64_t i = 0; i < payload_count; i++) {
        n00b_obj_bundle_decode_payload_t *payload = &payloads[i];

        if (!_n00b_obj_bundle_read_u64(stream, &payload->artifact_id)
            || !_n00b_obj_bundle_read_u64(stream, &payload->flags)
            || !_n00b_obj_bundle_read_u64(stream, &payload->payload_off)
            || !_n00b_obj_bundle_read_u64(stream, &payload->payload_len)
            || !_n00b_obj_bundle_read_digest(stream, payload->digest)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: payload record is truncated",
                                  allocator);
        }

        if (payload->flags != 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE,
                                  r"object bundle: unsupported payload flags",
                                  allocator);
        }

        if (payload->artifact_id >= artifact_count) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                                  r"object bundle: payload artifact target is missing",
                                  allocator);
        }

        if (i != 0 && payload->artifact_id <= payloads[i - 1].artifact_id) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: payload order is non-canonical",
                                  allocator);
        }

        n00b_obj_bundle_decode_artifact_t *artifact =
            &artifacts[payload->artifact_id];

        if (artifact->payload_index != i) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: payload index is non-canonical",
                                  allocator);
        }

        if (!_n00b_obj_bundle_range_within(payload->payload_off,
                                           payload->payload_len,
                                           payload_area.len)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: payload bytes are out of bounds",
                                  allocator);
        }

        if (payload->payload_off != payload_cursor) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: payload placement is non-canonical",
                                  allocator);
        }

        artifact->payload =
            _n00b_obj_bundle_decode_payload_slice(payload_base,
                                                  payload->payload_off,
                                                  payload->payload_len,
                                                  allocator);

        if (artifact->payload == nullptr) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: payload is too large",
                                  allocator);
        }

        uint8_t digest[N00B_OBJ_BUNDLE_DIGEST_LEN];

        _n00b_obj_bundle_sha256_bytes(artifact->payload->data,
                                      artifact->payload->byte_len,
                                      digest);

        if (memcmp(digest, payload->digest, N00B_OBJ_BUNDLE_DIGEST_LEN) != 0
            || memcmp(digest,
                      artifact->digest,
                      N00B_OBJ_BUNDLE_DIGEST_LEN) != 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH,
                                  r"object bundle: payload digest mismatch",
                                  allocator);
        }

        if (!_n00b_obj_bundle_u64_add(payload_cursor,
                                      payload->payload_len,
                                      &payload_cursor)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: payload area is too large",
                                  allocator);
        }
    }

    for (uint64_t i = 0; i < artifact_count; i++) {
        if (!_n00b_obj_bundle_artifact_has_valid_payload(artifacts[i].kind,
                                                         artifacts[i].payload)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                                  r"object bundle: artifact payload is invalid",
                                  allocator);
        }
    }

    if (!_n00b_obj_bundle_setpos(stream, exec_table.off)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: exec table is out of bounds",
                              allocator);
    }

    bool has_default_exec = false;
    bool saw_selector     = false;

    for (uint64_t i = 0; i < exec_count; i++) {
        n00b_obj_bundle_decode_exec_t *exec = &execs[i];

        if (!_n00b_obj_bundle_read_u32(stream, &exec->kind)
            || !_n00b_obj_bundle_read_u32(stream, &exec->flags)
            || !_n00b_obj_bundle_read_u32(stream, &exec->selector_off)
            || !_n00b_obj_bundle_read_u32(stream, &exec->reserved)
            || !_n00b_obj_bundle_read_u64(stream, &exec->target_id)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: exec record is truncated",
                                  allocator);
        }

        if (exec->flags != 0 || exec->reserved != 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE,
                                  r"object bundle: unsupported exec record flags",
                                  allocator);
        }

        if (exec->target_id >= artifact_count) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                                  r"object bundle: exec target is missing",
                                  allocator);
        }

        n00b_obj_bundle_decode_artifact_t *target = &artifacts[exec->target_id];
        bool target_is_exec = target->kind == N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE
                              || (target->kind == N00B_OBJ_BUNDLE_ARTIFACT_FILE
                                  && (target->mode & 0111u) != 0);

        if (!target_is_exec) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MISSING_TARGET,
                                  r"object bundle: exec target is not executable",
                                  allocator);
        }

        if (exec->kind == N00B_OBJ_BUNDLE_EXEC_REC_DEFAULT) {
            if (has_default_exec) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_MULTIPLE_DEFAULT_EXEC,
                                      r"object bundle: multiple default exec records",
                                      allocator);
            }

            if (saw_selector || exec->selector_off != 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                      r"object bundle: default exec order is non-canonical",
                                      allocator);
            }

            has_default_exec = true;
            continue;
        }

        if (exec->kind != N00B_OBJ_BUNDLE_EXEC_REC_SELECTOR) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                                  r"object bundle: unknown exec record kind",
                                  allocator);
        }

        saw_selector = true;

        if (!_n00b_obj_bundle_decode_string(string_base,
                                            string_table.len,
                                            exec->selector_off,
                                            false,
                                            allocator,
                                            &exec->selector)
            || !_n00b_obj_bundle_selector_is_valid(exec->selector)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                                  r"object bundle: selector string is invalid",
                                  allocator);
        }

        for (uint64_t j = 0; j < i; j++) {
            if (execs[j].kind != N00B_OBJ_BUNDLE_EXEC_REC_SELECTOR) {
                continue;
            }

            int cmp = _n00b_obj_bundle_string_bytes_cmp(execs[j].selector,
                                                        exec->selector);

            if (cmp == 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_DUPLICATE_SELECTOR,
                                      r"object bundle: duplicate selector",
                                      allocator);
            }

            if (cmp > 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                      r"object bundle: selector order is non-canonical",
                                      allocator);
            }
        }
    }

    if (!_n00b_obj_bundle_setpos(stream, policy_table.off)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                              r"object bundle: policy table is out of bounds",
                              allocator);
    }

    for (uint64_t i = 0; i < policy_count; i++) {
        n00b_obj_bundle_decode_policy_t *policy = &policies[i];
        uint32_t kind = 0;
        uint32_t scope = 0;

        if (!_n00b_obj_bundle_read_u64(stream, &policy->policy_id)
            || !_n00b_obj_bundle_read_u32(stream, &kind)
            || !_n00b_obj_bundle_read_u32(stream, &scope)
            || !_n00b_obj_bundle_read_u64(stream, &policy->flags)
            || !_n00b_obj_bundle_read_u64(stream, &policy->priority)
            || !_n00b_obj_bundle_read_u64(stream, &policy->payload_off)
            || !_n00b_obj_bundle_read_u64(stream, &policy->payload_len)
            || !_n00b_obj_bundle_read_u64(stream, &policy->fallback_policy_id)
            || !_n00b_obj_bundle_read_digest(stream, policy->digest)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                  r"object bundle: policy record is truncated",
                                  allocator);
        }

        policy->kind  = (n00b_obj_bundle_policy_kind_t)kind;
        policy->scope = (n00b_obj_bundle_policy_scope_t)scope;

        if (!_n00b_obj_bundle_policy_kind_is_supported(policy->kind)
            || !_n00b_obj_bundle_policy_scope_is_valid(policy->scope)
            || !_n00b_obj_bundle_policy_flags_are_valid(policy->flags)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE,
                                  r"object bundle: unsupported policy record",
                                  allocator);
        }

        for (uint64_t j = 0; j < i; j++) {
            if (policies[j].policy_id == policy->policy_id) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_DUPLICATE_POLICY_ID,
                                      r"object bundle: duplicate policy ID",
                                      allocator);
            }
        }

        if (policy->payload_len == 0) {
            if (policy->payload_off != 0
                || !_n00b_obj_bundle_digest_is_zero(policy->digest)) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                      r"object bundle: empty policy payload is non-canonical",
                                      allocator);
            }
        }
        else {
            if (!_n00b_obj_bundle_range_within(policy->payload_off,
                                               policy->payload_len,
                                               payload_area.len)) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                      r"object bundle: policy payload is out of bounds",
                                      allocator);
            }

            if (policy->payload_off != payload_cursor) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                      r"object bundle: policy payload placement is non-canonical",
                                      allocator);
            }

            policy->payload =
                _n00b_obj_bundle_decode_payload_slice(payload_base,
                                                      policy->payload_off,
                                                      policy->payload_len,
                                                      allocator);

            if (policy->payload == nullptr) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                      r"object bundle: policy payload is too large",
                                      allocator);
            }

            uint8_t digest[N00B_OBJ_BUNDLE_DIGEST_LEN];

            _n00b_obj_bundle_sha256_bytes(policy->payload->data,
                                          policy->payload->byte_len,
                                          digest);

            if (memcmp(digest,
                       policy->digest,
                       N00B_OBJ_BUNDLE_DIGEST_LEN) != 0) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH,
                                      r"object bundle: policy digest mismatch",
                                      allocator);
            }

            if (!_n00b_obj_bundle_u64_add(payload_cursor,
                                          policy->payload_len,
                                          &payload_cursor)) {
                return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                      N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS,
                                      r"object bundle: payload area is too large",
                                      allocator);
            }
        }

        if (!_n00b_obj_bundle_policy_payload_is_valid(policy->kind,
                                                      policy->payload,
                                                      policy->fallback_policy_id)) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST,
                                  r"object bundle: policy payload is invalid",
                                  allocator);
        }

        if (i != 0
            && _n00b_obj_bundle_decode_policy_cmp(&policies[i - 1],
                                                  policy) > 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: policy order is non-canonical",
                                  allocator);
        }
    }

    if (payload_cursor != payload_area.len) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                              r"object bundle: payload area has non-canonical trailing bytes",
                              allocator);
    }

    auto create = n00b_obj_bundle_new(.allocator = allocator);

    if (n00b_result_is_err(create)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, create));
    }

    n00b_obj_bundle_t *bundle = n00b_result_get(create);

    for (uint64_t i = 0; i < artifact_count; i++) {
        n00b_obj_bundle_decode_artifact_t *artifact = &artifacts[i];
        auto add = n00b_obj_bundle_add_artifact(bundle,
                                                artifact->logical_path,
                                                artifact->payload,
                                                .kind = artifact->kind,
                                                .role = artifact->role,
                                                .mode = artifact->mode,
                                                .flags = artifact->flags);

        if (n00b_result_is_err(add)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                n00b_obj_bundle_t *,
                n00b_result_get_err_payload(n00b_obj_bundle_error_t *, add));
        }
    }

    for (uint64_t i = 0; i < exec_count; i++) {
        n00b_obj_bundle_decode_exec_t *exec = &execs[i];
        n00b_string_t *target_path = artifacts[exec->target_id].logical_path;

        if (exec->kind == N00B_OBJ_BUNDLE_EXEC_REC_DEFAULT) {
            auto set_default =
                n00b_obj_bundle_set_default_exec(bundle, target_path);

            if (n00b_result_is_err(set_default)) {
                return OBJ_BUNDLE_ERR_PAYLOAD(
                    n00b_obj_bundle_t *,
                    n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                                set_default));
            }

            continue;
        }

        auto add_mapping = n00b_obj_bundle_add_exec_mapping(bundle,
                                                            exec->selector,
                                                            target_path);

        if (n00b_result_is_err(add_mapping)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                n00b_obj_bundle_t *,
                n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                            add_mapping));
        }
    }

    for (uint64_t i = 0; i < policy_count; i++) {
        n00b_obj_bundle_decode_policy_t *policy = &policies[i];
        auto add_policy = n00b_obj_bundle_add_policy(
            bundle,
            policy->policy_id,
            policy->kind,
            policy->scope,
            .flags = policy->flags,
            .priority = policy->priority,
            .payload = policy->payload,
            .fallback_policy_id = policy->fallback_policy_id);

        if (n00b_result_is_err(add_policy)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                n00b_obj_bundle_t *,
                n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                            add_policy));
        }
    }

    auto valid = n00b_obj_bundle_validate(bundle);

    if (n00b_result_is_err(valid)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, valid));
    }

    if (strict) {
        auto encoded = n00b_obj_bundle_encode(bundle, .allocator = allocator);

        if (n00b_result_is_err(encoded)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                n00b_obj_bundle_t *,
                n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                            encoded));
        }

        n00b_buffer_t *canonical = n00b_result_get(encoded);

        if (canonical->byte_len != bundle_bytes->byte_len
            || memcmp(canonical->data,
                      bundle_bytes->data,
                      bundle_bytes->byte_len) != 0) {
            return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                                  N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST,
                                  r"object bundle: manifest is non-canonical",
                                  allocator);
        }
    }

    bundle->is_mutable = false;

    return n00b_result_ok(n00b_obj_bundle_t *, bundle);
}

static bool
_n00b_obj_bundle_elf_section_is_metadata_carrier(
    n00b_elf_section_t *section)
{
    return section != nullptr
           && _n00b_obj_bundle_string_bytes_eq(section->name,
                                               r".0c001.bundle");
}

static bool
_n00b_obj_bundle_string_starts_with(n00b_string_t *s, n00b_string_t *prefix)
{
    return s != nullptr
           && prefix != nullptr
           && s->u8_bytes >= prefix->u8_bytes
           && memcmp(s->data, prefix->data, prefix->u8_bytes) == 0;
}

static bool
_n00b_obj_bundle_elf_section_is_foreign_legacy(n00b_elf_section_t *section)
{
    return section != nullptr
           && _n00b_obj_bundle_string_bytes_eq(section->name,
                                               r".0c001.file");
}

static bool
_n00b_obj_bundle_elf_section_is_wrapped_or_reserved(
    n00b_elf_section_t *section)
{
    return section != nullptr
           && (_n00b_obj_bundle_string_bytes_eq(section->name,
                                                r".0c001.wrap")
               || _n00b_obj_bundle_string_bytes_eq(section->name,
                                                   r".0c001.code"));
}

static bool
_n00b_obj_bundle_elf_section_is_unknown_0c001(n00b_elf_section_t *section)
{
    return section != nullptr
           && section->name != nullptr
           && _n00b_obj_bundle_string_starts_with(section->name, r".0c001.")
           && !_n00b_obj_bundle_elf_section_is_metadata_carrier(section)
           && !_n00b_obj_bundle_elf_section_is_foreign_legacy(section)
           && !_n00b_obj_bundle_elf_section_is_wrapped_or_reserved(section);
}

static int64_t
_n00b_obj_bundle_elf_carrier_shape_detail(n00b_elf_section_t *section)
{
    if (section->type != SHT_PROGBITS) {
        return (int64_t)section->type;
    }

    if ((section->flags & SHF_ALLOC) != 0) {
        return section->flags > INT64_MAX ? INT64_MAX : (int64_t)section->flags;
    }

    if (section->size > INT64_MAX) {
        return INT64_MAX;
    }

    return (int64_t)section->size;
}

static bool
_n00b_obj_bundle_elf_carrier_shape_is_valid(n00b_elf_section_t *section)
{
    return section != nullptr
           && section->type == SHT_PROGBITS
           && (section->flags & SHF_ALLOC) == 0
           && section->content != nullptr
           && section->content->data != nullptr;
}

static int64_t
_n00b_obj_bundle_decode_failure_detail(
    n00b_result_t(n00b_obj_bundle_t *) decode)
{
    if (n00b_result_is_err_payload(n00b_obj_bundle_error_t *, decode)) {
        n00b_obj_bundle_error_t *lower =
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, decode);

        return lower == nullptr ? 0 : lower->code;
    }

    return n00b_result_get_err(decode);
}

static n00b_result_t(n00b_obj_bundle_t *)
_n00b_obj_bundle_read_elf_metadata_carrier(n00b_buffer_t     *object_bytes,
                                           bool               strict,
                                           n00b_allocator_t  *allocator)
{
    n00b_bstream_t *stream = n00b_bstream_new(object_bytes,
                                              .allocator = allocator);
    auto            parsed = n00b_elf_parse(stream, .allocator = allocator);

    if (n00b_result_is_err(parsed)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier_detail(
                N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                r"object bundle: ELF object is malformed",
                N00B_FMT_ELF,
                true,
                N00B_OBJ_BUNDLE_CARRIER_METADATA,
                true,
                n00b_result_get_err(parsed),
                true,
                allocator));
    }

    n00b_elf_binary_t  *elf     = n00b_result_get(parsed);
    n00b_elf_section_t *carrier = nullptr;
    size_t              count   = 0;

    for (uint32_t i = 0; i < elf->num_sections; i++) {
        n00b_elf_section_t *section = &elf->sections[i];

        if (!_n00b_obj_bundle_elf_section_is_metadata_carrier(section)) {
            continue;
        }

        count++;

        if (carrier == nullptr) {
            carrier = section;
        }
    }

    if (count == 0) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_BUNDLE_NOT_FOUND,
                r"object bundle: ELF metadata carrier not found",
                N00B_FMT_ELF,
                true,
                N00B_OBJ_BUNDLE_CARRIER_METADATA,
                true,
                allocator));
    }

    if (count > 1) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier_detail(
                N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER,
                r"object bundle: duplicate ELF metadata carriers",
                N00B_FMT_ELF,
                true,
                N00B_OBJ_BUNDLE_CARRIER_METADATA,
                true,
                (int64_t)count,
                true,
                allocator));
    }

    if (!_n00b_obj_bundle_elf_carrier_shape_is_valid(carrier)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier_detail(
                N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                r"object bundle: ELF metadata carrier has invalid shape",
                N00B_FMT_ELF,
                true,
                N00B_OBJ_BUNDLE_CARRIER_METADATA,
                true,
                _n00b_obj_bundle_elf_carrier_shape_detail(carrier),
                true,
                allocator));
    }

    auto decoded = n00b_obj_bundle_decode(carrier->content,
                                          .strict = strict,
                                          .allocator = allocator);

    if (n00b_result_is_err(decoded)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier_detail(
                N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                r"object bundle: ELF metadata carrier payload is malformed",
                N00B_FMT_ELF,
                true,
                N00B_OBJ_BUNDLE_CARRIER_METADATA,
                true,
                _n00b_obj_bundle_decode_failure_detail(decoded),
                true,
                allocator));
    }

    return n00b_result_ok(n00b_obj_bundle_t *, n00b_result_get(decoded));
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_elf_metadata_error(n00b_obj_bundle_error_code_t code,
                                    n00b_string_t               *message,
                                    int64_t                      detail,
                                    bool                         has_detail,
                                    n00b_allocator_t            *allocator)
{
    return _n00b_obj_bundle_error_with_format_carrier_detail(
        code,
        message,
        N00B_FMT_ELF,
        true,
        N00B_OBJ_BUNDLE_CARRIER_METADATA,
        true,
        detail,
        has_detail,
        allocator);
}

static n00b_result_t(n00b_elf_binary_t *)
_n00b_obj_bundle_parse_elf_for_write(n00b_buffer_t     *object_bytes,
                                     n00b_allocator_t  *allocator)
{
    n00b_bstream_t *stream = n00b_bstream_new(object_bytes,
                                              .allocator = allocator);
    auto            parsed = n00b_elf_parse(stream, .allocator = allocator);

    if (n00b_result_is_err(parsed)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_elf_binary_t *,
            _n00b_obj_bundle_elf_metadata_error(
                N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                r"object bundle: ELF object is malformed",
                n00b_result_get_err(parsed),
                true,
                allocator));
    }

    return n00b_result_ok(n00b_elf_binary_t *, n00b_result_get(parsed));
}

static n00b_result_t(bool)
_n00b_obj_bundle_validate_elf_write_environment(n00b_elf_binary_t *elf,
                                                n00b_obj_bundle_replace_policy_t replace,
                                                bool strict,
                                                n00b_allocator_t *allocator)
{
    n00b_elf_section_t *bundle_carrier = nullptr;
    size_t              bundle_count   = 0;
    n00b_obj_bundle_error_t *foreign_error = nullptr;
    n00b_obj_bundle_error_t *wrapped_error = nullptr;
    n00b_obj_bundle_error_t *reserved_error = nullptr;
    n00b_obj_bundle_error_t *guard_error = nullptr;

    for (uint32_t i = 0; i < elf->num_sections; i++) {
        n00b_elf_section_t *section = &elf->sections[i];
        int64_t             detail  = (int64_t)i;

        if (_n00b_obj_bundle_elf_section_is_metadata_carrier(section)) {
            bundle_count++;

            if (bundle_carrier == nullptr) {
                bundle_carrier = section;
            }

            continue;
        }

        if (section->type == N00B_OBJ_BUNDLE_ELF_GUARD_SECTION_TYPE) {
            if (guard_error == nullptr) {
                guard_error = _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_GUARD_SECTION_PRESENT,
                    r"object bundle: ELF guard section blocks carrier write",
                    section->type,
                    true,
                    allocator);
            }

            continue;
        }

        if (_n00b_obj_bundle_elf_section_is_foreign_legacy(section)) {
            if (foreign_error == nullptr) {
                foreign_error = _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_FOREIGN_LEGACY_BUNDLE,
                    r"object bundle: ELF foreign legacy carrier blocks write",
                    detail,
                    true,
                    allocator);
            }

            continue;
        }

        if (_n00b_obj_bundle_elf_section_is_wrapped_or_reserved(section)) {
            if (wrapped_error == nullptr) {
                wrapped_error = _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_ALREADY_WRAPPED_OR_RESERVED,
                    r"object bundle: ELF wrapped input blocks carrier write",
                    detail,
                    true,
                    allocator);
            }

            continue;
        }

        if (_n00b_obj_bundle_elf_section_is_unknown_0c001(section)) {
            if (reserved_error == nullptr) {
                reserved_error = _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_RESERVED_NAMESPACE_OCCUPIED,
                    r"object bundle: ELF reserved namespace blocks write",
                    detail,
                    true,
                    allocator);
            }
        }
    }

    if (bundle_count > 1) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_elf_metadata_error(
                N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER,
                r"object bundle: duplicate ELF metadata carriers",
                (int64_t)bundle_count,
                true,
                allocator));
    }

    if (bundle_count == 1) {
        if (!_n00b_obj_bundle_elf_carrier_shape_is_valid(bundle_carrier)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                    r"object bundle: ELF metadata carrier has invalid shape",
                    _n00b_obj_bundle_elf_carrier_shape_detail(bundle_carrier),
                    true,
                    allocator));
        }

        auto decoded = n00b_obj_bundle_decode(bundle_carrier->content,
                                              .strict = strict,
                                              .allocator = allocator);

        if (n00b_result_is_err(decoded)) {
            return OBJ_BUNDLE_ERR_PAYLOAD(
                bool,
                _n00b_obj_bundle_elf_metadata_error(
                    N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER,
                    r"object bundle: ELF metadata carrier payload is malformed",
                    _n00b_obj_bundle_decode_failure_detail(decoded),
                    true,
                    allocator));
        }
    }

    if (foreign_error != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(bool, foreign_error);
    }

    if (wrapped_error != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(bool, wrapped_error);
    }

    if (reserved_error != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(bool, reserved_error);
    }

    if (guard_error != nullptr) {
        return OBJ_BUNDLE_ERR_PAYLOAD(bool, guard_error);
    }

    if (bundle_count == 1 && replace != N00B_OBJ_BUNDLE_REPLACE_EXISTING) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            bool,
            _n00b_obj_bundle_elf_metadata_error(
                N00B_OBJ_BUNDLE_ERR_REPLACE_REQUIRED,
                r"object bundle: ELF metadata carrier replacement is explicit",
                1,
                true,
                allocator));
    }

    return n00b_result_ok(bool, bundle_count == 1);
}

static n00b_obj_bundle_error_t *
_n00b_obj_bundle_error_from_rewrite_plan(n00b_elf_rewrite_plan_t *plan,
                                         n00b_allocator_t        *allocator)
{
    if (plan == nullptr) {
        return _n00b_obj_bundle_elf_metadata_error(
            N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE,
            r"object bundle: ELF rewrite planning failed",
            0,
            false,
            allocator);
    }

    return _n00b_obj_bundle_elf_metadata_error(
        N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE,
        r"object bundle: ELF rewrite plan was rejected",
        plan->rejection_reason,
        true,
        allocator);
}

static n00b_result_t(n00b_buffer_t *)
_n00b_obj_bundle_write_elf_metadata_carrier(
    n00b_buffer_t                    *object_bytes,
    n00b_obj_bundle_t                *bundle,
    n00b_obj_bundle_replace_policy_t  replace,
    bool                              strict,
    n00b_allocator_t                 *allocator)
{
    auto encoded = n00b_obj_bundle_encode(bundle, .allocator = allocator);

    if (n00b_result_is_err(encoded)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, encoded));
    }

    auto parsed = _n00b_obj_bundle_parse_elf_for_write(object_bytes, allocator);

    if (n00b_result_is_err(parsed)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *, parsed));
    }

    n00b_elf_binary_t *elf = n00b_result_get(parsed);
    auto environment =
        _n00b_obj_bundle_validate_elf_write_environment(elf,
                                                        replace,
                                                        strict,
                                                        allocator);

    if (n00b_result_is_err(environment)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            n00b_result_get_err_payload(n00b_obj_bundle_error_t *,
                                        environment));
    }

    n00b_elf_rewrite_metadata_request_t request = {
        .section_name          = r".0c001.bundle",
        .payload               = n00b_result_get(encoded),
        .file_alignment        = 8,
        .section_type          = SHT_PROGBITS,
        .section_flags         = 0,
        .preferred_file_offset = n00b_option_none(uint64_t),
        .policy                = {
            .flags = N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION
                   | N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY,
        },
    };

    bool has_existing_bundle = n00b_result_get(environment);
    auto plan_result =
        has_existing_bundle && replace == N00B_OBJ_BUNDLE_REPLACE_EXISTING
            ? n00b_elf_rewrite_plan_object_bundle_replace(
                  elf,
                  &request,
                  .allocator = allocator)
            : n00b_elf_rewrite_plan_object_bundle_insert(
                  elf,
                  &request,
                  .allocator = allocator);

    if (n00b_result_is_err(plan_result)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_elf_metadata_error(
                N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE,
                r"object bundle: ELF rewrite planning failed",
                n00b_result_get_err(plan_result),
                true,
                allocator));
    }

    n00b_elf_rewrite_plan_t *plan = n00b_result_get(plan_result);

    if (plan->outcome != N00B_ELF_REWRITE_PLAN_ACCEPTED) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_error_from_rewrite_plan(plan, allocator));
    }

    auto applied = n00b_elf_rewrite_apply_object_bundle_plan(
        elf,
        plan,
        .allocator = allocator);

    if (n00b_result_is_err(applied)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_elf_metadata_error(
                N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE,
                r"object bundle: ELF rewrite apply failed",
                n00b_result_get_err(applied),
                true,
                allocator));
    }

    return n00b_result_ok(n00b_buffer_t *, n00b_result_get(applied));
}

n00b_result_t(n00b_obj_bundle_t *)
n00b_obj_bundle_read(n00b_buffer_t *object_bytes) _kargs
{
    n00b_format_t    format    = N00B_FMT_UNKNOWN;
    bool             strict    = true;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!_n00b_obj_bundle_object_bytes_arg_is_valid(object_bytes)) {
        return OBJ_BUNDLE_ERR(n00b_obj_bundle_t *,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid object bytes",
                              allocator);
    }

    if (!_n00b_obj_bundle_format_request_is_valid(format)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_obj_bundle_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid object format request",
                format,
                true,
                N00B_OBJ_BUNDLE_CARRIER_AUTO,
                false,
                allocator));
    }

    n00b_format_t effective_format = format;

    if (format == N00B_FMT_UNKNOWN) {
        n00b_bstream_t *stream = n00b_bstream_new(object_bytes,
                                                  .allocator = allocator);

        effective_format = n00b_detect_format(stream);
    }

    if (effective_format == N00B_FMT_ELF) {
        return _n00b_obj_bundle_read_elf_metadata_carrier(object_bytes,
                                                          strict,
                                                          allocator);
    }

    return OBJ_BUNDLE_ERR_PAYLOAD(
        n00b_obj_bundle_t *,
        _n00b_obj_bundle_error_with_format_carrier(
            N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER,
            r"object bundle: carrier read is unsupported for object format",
            effective_format,
            effective_format != N00B_FMT_UNKNOWN,
            N00B_OBJ_BUNDLE_CARRIER_AUTO,
            false,
            allocator));
}

n00b_result_t(n00b_buffer_t *)
n00b_obj_bundle_write(n00b_buffer_t     *object_bytes,
                      n00b_obj_bundle_t *bundle) _kargs
{
    n00b_format_t                    format    = N00B_FMT_UNKNOWN;
    n00b_obj_bundle_carrier_t        carrier   = N00B_OBJ_BUNDLE_CARRIER_AUTO;
    n00b_obj_bundle_replace_policy_t replace   = N00B_OBJ_BUNDLE_REJECT_EXISTING;
    bool                             strict    = true;
    n00b_allocator_t                *allocator = nullptr;
}
{
    if (!_n00b_obj_bundle_object_bytes_arg_is_valid(object_bytes)
        || bundle == nullptr) {
        return OBJ_BUNDLE_ERR(n00b_buffer_t *,
                              N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                              r"object bundle: invalid carrier write argument",
                              allocator);
    }

    if (!_n00b_obj_bundle_format_request_is_valid(format)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid object format request",
                format,
                true,
                carrier,
                _n00b_obj_bundle_carrier_request_is_valid(carrier),
                allocator));
    }

    if (!_n00b_obj_bundle_carrier_request_is_valid(carrier)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid carrier request",
                format,
                format != N00B_FMT_UNKNOWN,
                carrier,
                true,
                allocator));
    }

    if (!_n00b_obj_bundle_replace_policy_is_valid(replace)) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT,
                r"object bundle: invalid replacement policy",
                format,
                format != N00B_FMT_UNKNOWN,
                carrier,
                true,
                allocator));
    }

    n00b_format_t effective_format = format;

    if (format == N00B_FMT_UNKNOWN) {
        n00b_bstream_t *stream = n00b_bstream_new(object_bytes,
                                                  .allocator = allocator);

        effective_format = n00b_detect_format(stream);
    }

    if (carrier == N00B_OBJ_BUNDLE_CARRIER_LOADABLE
        || carrier == N00B_OBJ_BUNDLE_CARRIER_SPLIT) {
        return OBJ_BUNDLE_ERR_PAYLOAD(
            n00b_buffer_t *,
            _n00b_obj_bundle_error_with_format_carrier(
                N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER,
                r"object bundle: requested carrier is unsupported",
                effective_format,
                effective_format != N00B_FMT_UNKNOWN,
                carrier,
                true,
                allocator));
    }

    if (effective_format == N00B_FMT_ELF) {
        return _n00b_obj_bundle_write_elf_metadata_carrier(object_bytes,
                                                           bundle,
                                                           replace,
                                                           strict,
                                                           allocator);
    }

    return OBJ_BUNDLE_ERR_PAYLOAD(
        n00b_buffer_t *,
        _n00b_obj_bundle_error_with_format_carrier(
            N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER,
            r"object bundle: carrier write is unsupported for object format",
            effective_format,
            effective_format != N00B_FMT_UNKNOWN,
            carrier,
            true,
            allocator));
}

n00b_obj_bundle_error_code_t
n00b_obj_bundle_error_code(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    return error->code;
}

n00b_string_t *
n00b_obj_bundle_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_OBJ_BUNDLE_ERR_OK:
        return r"object bundle: ok";
    case N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT:
        return r"object bundle: invalid argument";
    case N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH:
        return r"object bundle: invalid logical path";
    case N00B_OBJ_BUNDLE_ERR_DUPLICATE_LOGICAL_PATH:
        return r"object bundle: duplicate logical path";
    case N00B_OBJ_BUNDLE_ERR_DUPLICATE_SELECTOR:
        return r"object bundle: duplicate execution selector";
    case N00B_OBJ_BUNDLE_ERR_MULTIPLE_DEFAULT_EXEC:
        return r"object bundle: multiple default executables";
    case N00B_OBJ_BUNDLE_ERR_MISSING_TARGET:
        return r"object bundle: missing execution target";
    case N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_VERSION:
        return r"object bundle: unsupported version";
    case N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE:
        return r"object bundle: unsupported feature";
    case N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST:
        return r"object bundle: malformed manifest";
    case N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST:
        return r"object bundle: non-canonical manifest";
    case N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS:
        return r"object bundle: table or payload out of bounds";
    case N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH:
        return r"object bundle: digest mismatch";
    case N00B_OBJ_BUNDLE_ERR_BUILD:
        return r"object bundle: build failure";
    case N00B_OBJ_BUNDLE_ERR_DUPLICATE_POLICY_ID:
        return r"object bundle: duplicate policy ID";
    case N00B_OBJ_BUNDLE_ERR_BUNDLE_NOT_FOUND:
        return r"object bundle: bundle carrier not found";
    case N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER:
        return r"object bundle: duplicate bundle carrier";
    case N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER:
        return r"object bundle: malformed bundle carrier";
    case N00B_OBJ_BUNDLE_ERR_REPLACE_REQUIRED:
        return r"object bundle: replacement required";
    case N00B_OBJ_BUNDLE_ERR_RESERVED_NAMESPACE_OCCUPIED:
        return r"object bundle: reserved namespace occupied";
    case N00B_OBJ_BUNDLE_ERR_FOREIGN_LEGACY_BUNDLE:
        return r"object bundle: foreign legacy bundle";
    case N00B_OBJ_BUNDLE_ERR_ALREADY_WRAPPED_OR_RESERVED:
        return r"object bundle: already wrapped or reserved";
    case N00B_OBJ_BUNDLE_ERR_GUARD_SECTION_PRESENT:
        return r"object bundle: guard section present";
    case N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER:
        return r"object bundle: unsupported carrier";
    case N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE:
        return r"object bundle: rewrite failure";
    default:
        return r"object bundle: unknown error code";
    }
}

n00b_option_t(n00b_string_t *)
n00b_obj_bundle_error_message(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    return n00b_option_from_nullable(n00b_string_t *, error->message);
}

n00b_option_t(n00b_format_t)
n00b_obj_bundle_error_format(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_format) {
        return n00b_option_none(n00b_format_t);
    }

    return n00b_option_set(n00b_format_t, error->format);
}

n00b_option_t(n00b_obj_bundle_carrier_t)
n00b_obj_bundle_error_carrier(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_carrier) {
        return n00b_option_none(n00b_obj_bundle_carrier_t);
    }

    return n00b_option_set(n00b_obj_bundle_carrier_t, error->carrier);
}

n00b_option_t(n00b_string_t *)
n00b_obj_bundle_error_logical_path(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_logical_path) {
        return n00b_option_none(n00b_string_t *);
    }

    return n00b_option_from_nullable(n00b_string_t *, error->logical_path);
}

n00b_option_t(uint64_t)
n00b_obj_bundle_error_artifact_id(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_artifact_id) {
        return n00b_option_none(uint64_t);
    }

    return n00b_option_set(uint64_t, error->artifact_id);
}

n00b_option_t(n00b_obj_bundle_policy_kind_t)
n00b_obj_bundle_error_policy_kind(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_policy_kind) {
        return n00b_option_none(n00b_obj_bundle_policy_kind_t);
    }

    return n00b_option_set(n00b_obj_bundle_policy_kind_t, error->policy_kind);
}

n00b_option_t(n00b_obj_bundle_policy_scope_t)
n00b_obj_bundle_error_policy_scope(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_policy_scope) {
        return n00b_option_none(n00b_obj_bundle_policy_scope_t);
    }

    return n00b_option_set(n00b_obj_bundle_policy_scope_t, error->policy_scope);
}

n00b_option_t(int64_t)
n00b_obj_bundle_error_detail(n00b_obj_bundle_error_t *error)
{
    n00b_require(error != nullptr, "object bundle error must not be null");

    if (!error->has_detail) {
        return n00b_option_none(int64_t);
    }

    return n00b_option_set(int64_t, error->detail);
}
