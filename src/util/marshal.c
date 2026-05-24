#define N00B_USE_INTERNAL_API

#include "n00b.h"
#include "util/marshal.h"

#include "adt/dict_untyped.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/align.h"
#include "core/buffer.h"
#include "core/gc_map.h"
#include "core/hash.h"
#include "core/mmaps.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/static_objects.h"
#include "core/stw.h"

#include <stdint.h>

#define N00B_MARSHAL_OP_ALLOC  UINT32_C(0xe11cbab0)
#define N00B_MARSHAL_OP_CPATCH UINT32_C(0xe31cbab0)
#define N00B_MARSHAL_OP_SPATCH UINT32_C(0xe41cbab0)
#define N00B_MARSHAL_OP_STOP   UINT32_C(0xe51cbab0)
#define N00B_MARSHAL_OP_PSPATCH UINT32_C(0xe61cbab0)

#define N00B_MARSHAL_MIN_VERSION 1u
#define N00B_MARSHAL_STATIC_CHECK_MAX 16u

#define N00B_MARSHAL_ALLOC_F_SOURCE_INLINE     (1u << 0)
#define N00B_MARSHAL_ALLOC_F_SOURCE_OOB        (1u << 1)
#define N00B_MARSHAL_ALLOC_F_SOURCE_HEADERLESS (1u << 2)
#define N00B_MARSHAL_ALLOC_F_KNOWN             \
    (N00B_MARSHAL_ALLOC_F_SOURCE_INLINE        \
     | N00B_MARSHAL_ALLOC_F_SOURCE_OOB         \
     | N00B_MARSHAL_ALLOC_F_SOURCE_HEADERLESS)

typedef struct {
    uint64_t marshal_magic;
    uint32_t version;
    uint32_t base_address;
    uint32_t root_offset;
    uint32_t flags;
} n00b_marshal_stream_header_t;

typedef struct {
    uint32_t op;
    uint32_t flags;
    uint64_t vaddr;
    uint64_t user_len;
    uint64_t payload_len;
    uint64_t tinfo;
    uint32_t ptr_words;
    uint32_t scan_kind;
    uint32_t no_scan;
    uint32_t is_array;
} n00b_marshal_alloc_record_t;

typedef struct {
    uint32_t op;
    uint32_t reserved;
    uint64_t vaddr;
    uint64_t value;
} n00b_marshal_cpatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t check_len;
    uint64_t vaddr;
    uint64_t static_addr;
    uint64_t static_start;
    uint64_t static_len;
    uint64_t object_id;
    uint8_t  check[16];
} n00b_marshal_spatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t record_len;
    uint64_t vaddr;
    uint64_t object_offset;
    uint64_t object_len;
    uint64_t tinfo;
    uint32_t flags_mask;
    uint32_t flags_value;
    uint32_t scan_kind;
    uint32_t identity_version;
    uint32_t identity_kind;
    uint32_t namespace_len;
    uint32_t key_len;
    uint32_t check_len;
    uint32_t reserved;
} n00b_marshal_pspatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t end_of_stream;
} n00b_marshal_stop_record_t;

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} marshal_bytes_t;

typedef struct n00b_marshal_node_t {
    void                         *user_ptr;
    uint64_t                      vaddr;
    n00b_marshal_alloc_record_t   rec;
    char                         *payload;
    bool                          scanned;
} n00b_marshal_node_t;

typedef struct {
    uint64_t                    vaddr;
    uint64_t                    user_len;
    void                       *user_ptr;
    n00b_marshal_alloc_record_t rec;
} n00b_unmarshal_segment_t;

typedef struct {
    uint64_t                       start;
    uint64_t                       len;
    uint64_t                       object_id;
    n00b_alloc_type_info_t         tinfo;
    n00b_gc_scan_kind_t            scan_kind;
    uint32_t                       flags;
    const n00b_static_identity_t  *identity;
} n00b_marshal_static_ref_t;

struct n00b_marshal_ctx_t {
    n00b_pool_t             scratch;
    n00b_allocator_t       *scratch_alloc;
    n00b_dict_untyped_t     memos;
    n00b_marshal_node_t   **worklist;
    size_t                  work_len;
    size_t                  work_ix;
    size_t                  work_cap;
    marshal_bytes_t         out;
    marshal_bytes_t         patches;
    uint32_t                base_address;
    uint32_t                flags;
    uint32_t                next_offset;
    n00b_marshal_status_t   status;
    const char             *error;
    bool                    used;
};

struct n00b_unmarshal_ctx_t {
    n00b_pool_t               scratch;
    n00b_allocator_t         *scratch_alloc;
    marshal_bytes_t           in;
    n00b_unmarshal_segment_t **segments;
    size_t                    segment_len;
    size_t                    segment_cap;
    n00b_arena_t             *target_arena;
    n00b_marshal_status_t     status;
    const char               *error;
    bool                      closed;
};

static uint64_t
align8(uint64_t n)
{
    return (n + 7u) & ~UINT64_C(7);
}

static void
marshal_set_error(n00b_marshal_status_t *status,
                  const char           **error,
                  n00b_marshal_status_t  code,
                  const char            *msg)
{
    if (*status == N00B_MARSHAL_OK) {
        *status = code;
        *error  = msg;
    }
}

static void
marshal_init_scratch(n00b_pool_t       *pool,
                     n00b_allocator_t **alloc,
                     const char        *name)
{
    *alloc = n00b_pool_init(pool,
                            .__system = true,
                            .hidden   = true,
                            .name     = name);
}

static void *
marshal_scratch_alloc(n00b_allocator_t *alloc, size_t n)
{
    if (!n) {
        n = 1;
    }
    return n00b_alloc_size_with_opts(1,
                                     n00b_align(n),
                                     &(n00b_alloc_opts_t){
                                         .allocator = alloc,
                                         .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                     });
}

static void
bytes_reserve(marshal_bytes_t   *b,
              n00b_allocator_t *alloc,
              size_t            needed)
{
    if (needed <= b->cap) {
        return;
    }

    size_t new_cap = b->cap ? b->cap : 256;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap <<= 1;
    }

    char *new_data = marshal_scratch_alloc(alloc, new_cap);
    if (b->len) {
        memcpy(new_data, b->data, b->len);
    }
    b->data = new_data;
    b->cap  = new_cap;
}

static void
bytes_append(marshal_bytes_t   *b,
             n00b_allocator_t *alloc,
             const void       *data,
             size_t            len)
{
    bytes_reserve(b, alloc, b->len + len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void
bytes_append_zero(marshal_bytes_t   *b,
                  n00b_allocator_t *alloc,
                  size_t            len)
{
    bytes_reserve(b, alloc, b->len + len);
    memset(b->data + b->len, 0, len);
    b->len += len;
}

static void
work_push(n00b_marshal_ctx_t *ctx, n00b_marshal_node_t *node)
{
    if (ctx->work_len == ctx->work_cap) {
        size_t new_cap = ctx->work_cap ? ctx->work_cap << 1 : 32;
        n00b_marshal_node_t **new_items = marshal_scratch_alloc(
            ctx->scratch_alloc, new_cap * sizeof(*new_items));
        if (ctx->work_len) {
            memcpy(new_items, ctx->worklist, ctx->work_len * sizeof(*new_items));
        }
        ctx->worklist = new_items;
        ctx->work_cap = new_cap;
    }
    ctx->worklist[ctx->work_len++] = node;
}

static bool
scan_word_for_policy(n00b_marshal_alloc_record_t *rec, uint64_t i)
{
    switch ((n00b_gc_scan_kind_t)rec->scan_kind) {
    case N00B_GC_SCAN_KIND_NONE:
        return false;
    case N00B_GC_SCAN_KIND_ALL:
        return true;
    case N00B_GC_SCAN_KIND_EVERY_OTHER:
        return (i & 1u) == 0;
    case N00B_GC_SCAN_KIND_DEFAULT:
        return !rec->no_scan;
    case N00B_GC_SCAN_KIND_CALLBACK:
    default:
        return false;
    }
}

static uint64_t
marshal_scan_word_count(n00b_marshal_alloc_record_t *rec)
{
    uint64_t user_words = rec->user_len / sizeof(uint64_t);

    if (rec->ptr_words && rec->ptr_words < user_words) {
        return rec->ptr_words;
    }

    return user_words;
}

static n00b_marshal_status_t
marshal_status_from_static_identity(n00b_static_identity_status_t status)
{
    switch (status) {
    case N00B_STATIC_IDENTITY_OK:
        return N00B_MARSHAL_OK;
    case N00B_STATIC_IDENTITY_ERR_MISSING:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_MISSING;
    case N00B_STATIC_IDENTITY_ERR_DUPLICATE:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_DUPLICATE;
    case N00B_STATIC_IDENTITY_ERR_MUTABILITY:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_MUTABILITY;
    case N00B_STATIC_IDENTITY_ERR_TYPE:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_TYPE;
    case N00B_STATIC_IDENTITY_ERR_SCAN:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_SCAN;
    case N00B_STATIC_IDENTITY_ERR_LENGTH:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_LENGTH;
    case N00B_STATIC_IDENTITY_ERR_CHECK_BYTES:
        return N00B_MARSHAL_ERR_STATIC_IDENTITY_CHECK_BYTES;
    case N00B_STATIC_IDENTITY_ERR_NULL:
    case N00B_STATIC_IDENTITY_ERR_INVALID:
    default:
        return N00B_MARSHAL_ERR_BAD_STREAM;
    }
}

static bool
marshal_static_identity_valid(const n00b_static_identity_t *identity)
{
    return identity != nullptr
        && identity->version == N00B_STATIC_IDENTITY_VERSION
        && identity->kind != N00B_STATIC_IDENTITY_NONE
        && identity->kind <= N00B_STATIC_IDENTITY_MANUAL
        && identity->namespace_id != nullptr
        && identity->namespace_id[0] != '\0'
        && identity->object_key != nullptr
        && identity->object_key[0] != '\0';
}

static bool
pspatch_expected_len(uint32_t namespace_len,
                     uint32_t key_len,
                     uint32_t check_len,
                     uint32_t *out_len)
{
    if (namespace_len == 0 || key_len == 0
        || check_len == 0 || check_len > N00B_MARSHAL_STATIC_CHECK_MAX) {
        return false;
    }

    uint64_t len = sizeof(n00b_marshal_pspatch_record_t);
    len += namespace_len;
    len += key_len;
    len += check_len;
    len = align8(len);

    if (len > UINT32_MAX) {
        return false;
    }

    *out_len = (uint32_t)len;
    return true;
}

static bool
pspatch_lengths_for_identity(const n00b_static_identity_t *identity,
                             uint32_t check_len,
                             uint32_t *namespace_len,
                             uint32_t *key_len,
                             uint32_t *record_len)
{
    if (!marshal_static_identity_valid(identity)) {
        return false;
    }

    size_t ns_len = strlen(identity->namespace_id);
    size_t obj_len = strlen(identity->object_key);

    if (ns_len > UINT32_MAX || obj_len > UINT32_MAX) {
        return false;
    }

    *namespace_len = (uint32_t)ns_len;
    *key_len       = (uint32_t)obj_len;
    return pspatch_expected_len(*namespace_len, *key_len, check_len, record_len);
}

static const unsigned char *
pspatch_namespace_bytes(const n00b_marshal_pspatch_record_t *rec)
{
    return (const unsigned char *)rec + sizeof(*rec);
}

static const unsigned char *
pspatch_key_bytes(const n00b_marshal_pspatch_record_t *rec)
{
    return pspatch_namespace_bytes(rec) + rec->namespace_len;
}

static const unsigned char *
pspatch_check_bytes(const n00b_marshal_pspatch_record_t *rec)
{
    return pspatch_key_bytes(rec) + rec->key_len;
}

static bool
pspatch_payload_has_nul(const n00b_marshal_pspatch_record_t *rec)
{
    return memchr(pspatch_namespace_bytes(rec), '\0', rec->namespace_len) != nullptr
        || memchr(pspatch_key_bytes(rec), '\0', rec->key_len) != nullptr;
}

static bool
alloc_view(n00b_alloc_info_t              info,
           void                         **user_ptr,
           uint64_t                      *user_len,
           n00b_marshal_alloc_record_t   *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->op = N00B_MARSHAL_OP_ALLOC;

    if (info.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = info.hdr.oob;
        *user_ptr           = oob->user_ptr;
        *user_len           = oob->hcur ? oob->alloc_len - N00B_ALLOC_HDR_SZ
                                        : oob->alloc_len;
        rec->flags          = N00B_MARSHAL_ALLOC_F_SOURCE_OOB;
        if (oob->hcur) {
            rec->flags |= N00B_MARSHAL_ALLOC_F_SOURCE_INLINE;
        }
        else {
            rec->flags |= N00B_MARSHAL_ALLOC_F_SOURCE_HEADERLESS;
        }
        rec->tinfo     = oob->tinfo;
        rec->ptr_words = oob->ptr_words;
        rec->scan_kind = oob->scan_kind;
        rec->no_scan   = oob->no_scan;
        rec->is_array  = oob->is_array;
        return true;
    }

    if (info.kind == n00b_alloc_inline) {
        n00b_inline_hdr_t *hdr = info.hdr.in_line;
        *user_ptr             = ((char *)hdr) + N00B_ALLOC_HDR_SZ;
        *user_len             = hdr->alloc_len - N00B_ALLOC_HDR_SZ;
        rec->flags            = N00B_MARSHAL_ALLOC_F_SOURCE_INLINE;
        rec->tinfo            = hdr->tinfo;
        rec->ptr_words        = hdr->ptr_words;
        rec->scan_kind        = hdr->scan_kind;
        rec->no_scan          = hdr->no_scan;
        rec->is_array         = hdr->is_array;
        return true;
    }

    return false;
}

static n00b_marshal_node_t *
marshal_add_alloc(n00b_marshal_ctx_t *ctx, n00b_alloc_info_t info)
{
    void                       *user_ptr = nullptr;
    uint64_t                    user_len = 0;
    n00b_marshal_alloc_record_t rec;

    if (!alloc_view(info, &user_ptr, &user_len, &rec)) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_ALLOCATION,
                          "unsupported or missing allocation metadata");
        return nullptr;
    }

    if (rec.scan_kind == N00B_GC_SCAN_KIND_CALLBACK) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_SCAN_POLICY,
                          "callback scan policy is not yet marshalable");
        return nullptr;
    }

    if (rec.ptr_words > (user_len / sizeof(uint64_t))) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_ALLOCATION,
                          "allocation pointer-word metadata exceeds object size");
        return nullptr;
    }

    bool found = false;
    n00b_marshal_node_t *node = n00b_dict_untyped_get(&ctx->memos, user_ptr, &found);
    if (found) {
        return node;
    }

    uint64_t payload_len = align8(user_len);
    if (ctx->next_offset > UINT32_MAX || payload_len > UINT32_MAX
        || ctx->next_offset + payload_len > UINT32_MAX) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_LIMIT,
                          "marshaled graph exceeds 32-bit virtual heap limit");
        return nullptr;
    }

    node           = marshal_scratch_alloc(ctx->scratch_alloc, sizeof(*node));
    node->user_ptr = user_ptr;
    node->vaddr    = ((uint64_t)ctx->base_address << 32) | ctx->next_offset;
    node->rec      = rec;
    node->payload  = marshal_scratch_alloc(ctx->scratch_alloc, payload_len);
    node->scanned  = false;

    node->rec.vaddr       = node->vaddr;
    node->rec.user_len    = user_len;
    node->rec.payload_len = payload_len;

    memcpy(node->payload, user_ptr, user_len);
    if (payload_len > user_len) {
        memset(node->payload + user_len, 0, payload_len - user_len);
    }

    ctx->next_offset += (uint32_t)payload_len;
    n00b_dict_untyped_put(&ctx->memos, user_ptr, node);
    work_push(ctx, node);

    return node;
}

static void
emit_cpatch(n00b_marshal_ctx_t *ctx, uint64_t vaddr, uint64_t value)
{
    n00b_marshal_cpatch_record_t rec = {
        .op       = N00B_MARSHAL_OP_CPATCH,
        .reserved = 0,
        .vaddr    = vaddr,
        .value    = value,
    };

    bytes_append(&ctx->patches, ctx->scratch_alloc, &rec, sizeof(rec));
}

static bool
static_ref_view(void *addr, n00b_marshal_static_ref_t *out)
{
    memset(out, 0, sizeof(*out));

    auto range_opt = n00b_mmap_range_by_address(addr);
    if (n00b_option_is_set(range_opt)) {
        n00b_alloc_range_t *range = n00b_option_get(range_opt);
        uint64_t            base  = (uint64_t)(uintptr_t)range->start;
        uint64_t            end   = base + range->len;

        if (range->kind == n00b_mmap_static
            && (uint64_t)(uintptr_t)addr >= base
            && (uint64_t)(uintptr_t)addr < end) {
            *out = (n00b_marshal_static_ref_t){
                .start     = base,
                .len       = range->len,
                .object_id = range->object_id,
                .tinfo     = range->tinfo,
                .scan_kind = range->scan_kind,
                .flags     = range->flags,
                .identity  = range->identity,
            };
            return true;
        }
    }

    return false;
}

static bool
emit_spatch(n00b_marshal_ctx_t *ctx,
            uint64_t vaddr,
            uint64_t value,
            const n00b_marshal_static_ref_t *ref)
{
    uint64_t offset = value - ref->start;
    if (offset >= ref->len) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer lies outside its registered object");
        return false;
    }

    uint64_t remain    = ref->len - offset;
    uint32_t check_len = (uint32_t)(remain < sizeof(((n00b_marshal_spatch_record_t *)0)->check)
                                       ? remain
                                       : sizeof(((n00b_marshal_spatch_record_t *)0)->check));
    if (!check_len) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer has no bytes available for validation");
        return false;
    }

    n00b_marshal_spatch_record_t rec = {
        .op           = N00B_MARSHAL_OP_SPATCH,
        .check_len    = check_len,
        .vaddr        = vaddr,
        .static_addr  = value,
        .static_start = ref->start,
        .static_len   = ref->len,
        .object_id    = ref->object_id,
    };
    memcpy(rec.check, (void *)(uintptr_t)value, check_len);

    bytes_append(&ctx->patches, ctx->scratch_alloc, &rec, sizeof(rec));
    return true;
}

static bool
emit_pspatch(n00b_marshal_ctx_t *ctx,
             uint64_t vaddr,
             uint64_t value,
             const n00b_marshal_static_ref_t *ref)
{
    const n00b_static_identity_t *identity = ref->identity;
    if (!marshal_static_identity_valid(identity)) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer identity is malformed");
        return false;
    }

    uint64_t offset = value - ref->start;
    if (offset >= ref->len) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer lies outside its registered object");
        return false;
    }

    uint64_t remain    = ref->len - offset;
    uint32_t check_len = (uint32_t)(remain < N00B_MARSHAL_STATIC_CHECK_MAX
                                       ? remain
                                       : N00B_MARSHAL_STATIC_CHECK_MAX);
    if (!check_len) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer has no bytes available for validation");
        return false;
    }

    uint32_t namespace_len;
    uint32_t key_len;
    uint32_t record_len;
    if (!pspatch_lengths_for_identity(identity,
                                      check_len,
                                      &namespace_len,
                                      &key_len,
                                      &record_len)) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer identity cannot be encoded");
        return false;
    }

    const uint32_t flags_mask = N00B_STATIC_OBJECT_F_READONLY
                              | N00B_STATIC_OBJECT_F_MUTABLE;
    const uint32_t flags_value = ref->flags & flags_mask;
    const unsigned char *check = (const unsigned char *)(uintptr_t)value;

    n00b_static_identity_query_t query = {
        .checks = N00B_STATIC_IDENTITY_CHECK_LEN
                | N00B_STATIC_IDENTITY_CHECK_TINFO
                | N00B_STATIC_IDENTITY_CHECK_SCAN_KIND
                | N00B_STATIC_IDENTITY_CHECK_FLAGS
                | N00B_STATIC_IDENTITY_CHECK_BYTES,
        .len          = ref->len,
        .tinfo        = ref->tinfo,
        .scan_kind    = ref->scan_kind,
        .flags_mask   = flags_mask,
        .flags_value  = flags_value,
        .check_offset = offset,
        .check_len    = check_len,
        .check_bytes  = check,
    };

    n00b_alloc_range_t *range = nullptr;
    n00b_static_identity_status_t lookup = n00b_static_identity_lookup(identity,
                                                                       &query,
                                                                       &range);
    if (lookup != N00B_STATIC_IDENTITY_OK) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          marshal_status_from_static_identity(lookup),
                          "portable static pointer identity failed validation");
        return false;
    }
    if (range == nullptr || (uint64_t)(uintptr_t)range->start != ref->start) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_STATIC_IDENTITY_DUPLICATE,
                          "portable static pointer identity resolved to a different object");
        return false;
    }

    n00b_marshal_pspatch_record_t rec = {
        .op               = N00B_MARSHAL_OP_PSPATCH,
        .record_len       = record_len,
        .vaddr            = vaddr,
        .object_offset    = offset,
        .object_len       = ref->len,
        .tinfo            = ref->tinfo,
        .flags_mask       = flags_mask,
        .flags_value      = flags_value,
        .scan_kind        = ref->scan_kind,
        .identity_version = identity->version,
        .identity_kind    = identity->kind,
        .namespace_len    = namespace_len,
        .key_len          = key_len,
        .check_len        = check_len,
        .reserved         = 0,
    };

    bytes_append(&ctx->patches, ctx->scratch_alloc, &rec, sizeof(rec));
    bytes_append(&ctx->patches,
                 ctx->scratch_alloc,
                 identity->namespace_id,
                 namespace_len);
    bytes_append(&ctx->patches,
                 ctx->scratch_alloc,
                 identity->object_key,
                 key_len);
    bytes_append(&ctx->patches, ctx->scratch_alloc, check, check_len);

    size_t used = sizeof(rec) + namespace_len + key_len + check_len;
    bytes_append_zero(&ctx->patches, ctx->scratch_alloc, record_len - used);
    return true;
}

static bool
emit_static_patch(n00b_marshal_ctx_t *ctx, uint64_t vaddr, uint64_t value)
{
    n00b_marshal_static_ref_t ref;
    if (!static_ref_view((void *)(uintptr_t)value, &ref)) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER,
                          "static pointer is not a registered static object");
        return false;
    }

    if (ref.identity != nullptr) {
        return emit_pspatch(ctx, vaddr, value, &ref);
    }

    return emit_spatch(ctx, vaddr, value, &ref);
}

static void
emit_alloc(n00b_marshal_ctx_t *ctx, n00b_marshal_node_t *node)
{
    bytes_append(&ctx->out, ctx->scratch_alloc, &node->rec, sizeof(node->rec));
    bytes_append(&ctx->out,
                 ctx->scratch_alloc,
                 node->payload,
                 (size_t)node->rec.payload_len);
}

static void
scan_node(n00b_marshal_ctx_t *ctx, n00b_marshal_node_t *node)
{
    if (node->scanned || ctx->status != N00B_MARSHAL_OK) {
        return;
    }

    node->scanned = true;
    uint64_t nwords = marshal_scan_word_count(&node->rec);
    uint64_t *words = (uint64_t *)node->payload;

    for (uint64_t i = 0; i < nwords; i++) {
        uint64_t word      = words[i];
        bool     rewritten = false;

        if (scan_word_for_policy(&node->rec, i) && word) {
            auto mmap_opt = n00b_mmap_by_address((void *)(uintptr_t)word);
            if (n00b_option_is_set(mmap_opt)) {
                n00b_mmap_info_t *map = n00b_option_get(mmap_opt);
                switch (map->kind) {
                case n00b_mmap_managed_segment:
                case n00b_mmap_pool:
                case n00b_mmap_sys_segment: {
                    n00b_alloc_info_t info = n00b_find_alloc_info(
                        (void *)(uintptr_t)word, .scan_for_header = true);
                    n00b_marshal_node_t *target = marshal_add_alloc(ctx, info);
                    if (!target) {
                        return;
                    }
                    uint64_t interior = word - (uint64_t)(uintptr_t)target->user_ptr;
                    if (interior >= target->rec.user_len) {
                        marshal_set_error(&ctx->status,
                                          &ctx->error,
                                          N00B_MARSHAL_ERR_UNSUPPORTED_ALLOCATION,
                                          "pointer does not resolve inside target allocation");
                        return;
                    }
                    words[i]  = target->vaddr + interior;
                    rewritten = true;
                    break;
                }
                case n00b_mmap_static:
                    if (!emit_static_patch(ctx, node->vaddr + i * sizeof(uint64_t), word)) {
                        return;
                    }
                    words[i]  = 0;
                    rewritten = true;
                    break;
                default:
                    break;
                }
            }
        }

        if (!rewritten && (word >> 32) == ctx->base_address) {
            uint64_t vaddr = node->vaddr + i * sizeof(uint64_t);
            emit_cpatch(ctx, vaddr, word);
            words[i] = 0;
        }
    }

    emit_alloc(ctx, node);
}

static bool
marshal_process(n00b_marshal_ctx_t *ctx, void *addr)
{
    if (!addr) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_NULL_ARG,
                          "cannot marshal a null root");
        return false;
    }

    n00b_alloc_info_t info = n00b_find_alloc_info(addr, .scan_for_header = true);
    n00b_marshal_node_t *root = marshal_add_alloc(ctx, info);
    if (!root) {
        return false;
    }

    uint64_t interior = (uint64_t)(uintptr_t)addr - (uint64_t)(uintptr_t)root->user_ptr;
    if (interior >= root->rec.user_len) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_UNSUPPORTED_ALLOCATION,
                          "root pointer does not resolve inside allocation");
        return false;
    }

    n00b_marshal_stream_header_t hdr = {
        .marshal_magic = N00B_MARSHAL_MAGIC,
        .version       = N00B_MARSHAL_VERSION,
        .base_address  = ctx->base_address,
        .root_offset   = (uint32_t)((root->vaddr + interior) & UINT32_MAX),
        .flags         = ctx->flags,
    };

    bytes_append(&ctx->out, ctx->scratch_alloc, &hdr, sizeof(hdr));

    while (ctx->work_ix < ctx->work_len) {
        scan_node(ctx, ctx->worklist[ctx->work_ix++]);
        if (ctx->status != N00B_MARSHAL_OK) {
            return false;
        }
    }

    if (ctx->patches.len) {
        bytes_append(&ctx->out, ctx->scratch_alloc, ctx->patches.data, ctx->patches.len);
    }

    n00b_marshal_stop_record_t stop = {
        .op            = N00B_MARSHAL_OP_STOP,
        .end_of_stream = 1,
    };
    bytes_append(&ctx->out, ctx->scratch_alloc, &stop, sizeof(stop));

    return true;
}

n00b_marshal_ctx_t *
n00b_marshal_ctx_new() _kargs
{
    uint32_t flags        = N00B_MARSHAL_F_NONE;
    uint32_t base_address = 0;
}
{
    n00b_marshal_ctx_t *ctx = n00b_alloc(n00b_marshal_ctx_t);
    memset(ctx, 0, sizeof(*ctx));
    marshal_init_scratch(&ctx->scratch, &ctx->scratch_alloc, "marshal_scratch");
    n00b_dict_untyped_init(&ctx->memos,
                           .allocator     = ctx->scratch_alloc,
                           .hash          = n00b_hash_word,
                           .skip_obj_hash = true,
                           .scan_kind     = N00B_GC_SCAN_KIND_NONE);

    ctx->base_address = base_address ? base_address
                                     : (0x4d000000u
                                        | ((uint32_t)(uintptr_t)ctx & 0x00ffffffu));
    if (!ctx->base_address) {
        ctx->base_address = 0x4d1cbab0u;
    }
    ctx->flags  = flags;
    ctx->status = N00B_MARSHAL_OK;
    return ctx;
}

void
n00b_marshal_ctx_destroy(n00b_marshal_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    n00b_allocator_destroy((n00b_allocator_t *)&ctx->scratch);
    n00b_free(ctx);
}

n00b_marshal_status_t
n00b_marshal_ctx_status(n00b_marshal_ctx_t *ctx)
{
    return ctx ? ctx->status : N00B_MARSHAL_ERR_NULL_ARG;
}

const char *
n00b_marshal_ctx_error(n00b_marshal_ctx_t *ctx)
{
    if (!ctx) {
        return "null marshal context";
    }
    return ctx->error;
}

n00b_buffer_t *
n00b_marshal_incremental(n00b_marshal_ctx_t *ctx, void *addr) _kargs
{
    bool close = true;
}
{
    (void)close;
    if (!ctx) {
        return nullptr;
    }
    if (ctx->used) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_CONTEXT_CLOSED,
                          "marshal context already emitted a root");
        return nullptr;
    }
    ctx->used = true;

    bool hold_stw = (ctx->flags & N00B_MARSHAL_F_STW) != 0;
    if (hold_stw) {
        n00b_stop_the_world();
    }
    bool ok = marshal_process(ctx, addr);
    if (hold_stw) {
        n00b_restart_the_world();
    }
    if (!ok) {
        return nullptr;
    }

    return n00b_buffer_from_bytes(ctx->out.data, (int64_t)ctx->out.len);
}

n00b_buffer_t *
n00b_marshal(void *addr) _kargs
{
    uint32_t flags        = N00B_MARSHAL_F_NONE;
    uint32_t base_address = 0;
}
{
    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(.flags        = flags,
                                                   .base_address = base_address);
    n00b_buffer_t *result = n00b_marshal_incremental(ctx, addr, .close = true);
    n00b_marshal_ctx_destroy(ctx);
    return result;
}

static void
segments_push(n00b_unmarshal_ctx_t *ctx, n00b_unmarshal_segment_t *seg)
{
    if (ctx->segment_len == ctx->segment_cap) {
        size_t new_cap = ctx->segment_cap ? ctx->segment_cap << 1 : 32;
        n00b_unmarshal_segment_t **new_items = marshal_scratch_alloc(
            ctx->scratch_alloc, new_cap * sizeof(*new_items));
        if (ctx->segment_len) {
            memcpy(new_items, ctx->segments, ctx->segment_len * sizeof(*new_items));
        }
        ctx->segments    = new_items;
        ctx->segment_cap = new_cap;
    }
    ctx->segments[ctx->segment_len++] = seg;
}

static n00b_unmarshal_segment_t *
segment_for_vaddr(n00b_unmarshal_ctx_t *ctx, uint64_t vaddr)
{
    for (size_t i = 0; i < ctx->segment_len; i++) {
        n00b_unmarshal_segment_t *seg = ctx->segments[i];
        if (vaddr >= seg->vaddr && vaddr < seg->vaddr + seg->user_len) {
            return seg;
        }
    }
    return nullptr;
}

static void *
addr_for_vaddr_span(n00b_unmarshal_ctx_t *ctx, uint64_t vaddr, uint64_t len)
{
    n00b_unmarshal_segment_t *seg = segment_for_vaddr(ctx, vaddr);
    if (!seg) {
        return nullptr;
    }

    uint64_t offset = vaddr - seg->vaddr;
    if (len > seg->user_len || offset > seg->user_len - len) {
        return nullptr;
    }

    return ((char *)seg->user_ptr) + offset;
}

static void *
addr_for_vaddr(n00b_unmarshal_ctx_t *ctx, uint64_t vaddr)
{
    return addr_for_vaddr_span(ctx, vaddr, 1);
}

static uint64_t *
word_slot_for_vaddr(n00b_unmarshal_ctx_t *ctx, uint64_t vaddr)
{
    if (vaddr & (sizeof(uint64_t) - 1)) {
        return nullptr;
    }

    return addr_for_vaddr_span(ctx, vaddr, sizeof(uint64_t));
}

static bool
stream_complete(n00b_unmarshal_ctx_t *ctx)
{
    if (ctx->in.len < sizeof(n00b_marshal_stream_header_t)) {
        return false;
    }

    n00b_marshal_stream_header_t *hdr = (void *)ctx->in.data;
    if (hdr->marshal_magic != N00B_MARSHAL_MAGIC
        || hdr->version < N00B_MARSHAL_MIN_VERSION
        || hdr->version > N00B_MARSHAL_VERSION) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_BAD_STREAM,
                          "invalid marshal stream header");
        return false;
    }

    uint32_t expected_offset = 0;
    size_t   ix              = sizeof(*hdr);
    while (ix < ctx->in.len) {
        if (ctx->in.len - ix < sizeof(uint32_t)) {
            return false;
        }
        uint32_t op = *(uint32_t *)(ctx->in.data + ix);
        switch (op) {
        case N00B_MARSHAL_OP_ALLOC: {
            if (ctx->in.len - ix < sizeof(n00b_marshal_alloc_record_t)) {
                return false;
            }
            n00b_marshal_alloc_record_t *rec = (void *)(ctx->in.data + ix);
            if ((rec->vaddr >> 32) != hdr->base_address
                || (uint32_t)(rec->vaddr & UINT32_MAX) != expected_offset
                || rec->payload_len != align8(rec->user_len)
                || rec->payload_len < rec->user_len
                || rec->ptr_words > (rec->user_len / sizeof(uint64_t))
                || rec->scan_kind > N00B_GC_SCAN_KIND_CALLBACK
                || (rec->flags & ~N00B_MARSHAL_ALLOC_F_KNOWN) != 0) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "invalid allocation record shape");
                return false;
            }
            if (rec->scan_kind == N00B_GC_SCAN_KIND_CALLBACK) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_UNSUPPORTED_SCAN_POLICY,
                                  "callback scan policy is not yet unmarshalable");
                return false;
            }
            ix += sizeof(*rec);
            if (rec->payload_len > SIZE_MAX || ctx->in.len - ix < rec->payload_len) {
                return false;
            }
            ix += (size_t)rec->payload_len;
            if (rec->payload_len > UINT32_MAX
                || expected_offset + rec->payload_len > UINT32_MAX) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_LIMIT,
                                  "marshal stream exceeds 32-bit virtual heap limit");
                return false;
            }
            expected_offset += (uint32_t)rec->payload_len;
            break;
        }
        case N00B_MARSHAL_OP_CPATCH: {
            if (ctx->in.len - ix < sizeof(n00b_marshal_cpatch_record_t)) {
                return false;
            }
            n00b_marshal_cpatch_record_t *rec = (void *)(ctx->in.data + ix);
            if (rec->reserved != 0 || (rec->vaddr >> 32) != hdr->base_address) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "invalid collision patch record");
                return false;
            }
            ix += sizeof(*rec);
            break;
        }
        case N00B_MARSHAL_OP_SPATCH: {
            if (ctx->in.len - ix < sizeof(n00b_marshal_spatch_record_t)) {
                return false;
            }
            n00b_marshal_spatch_record_t *rec = (void *)(ctx->in.data + ix);
            if (rec->check_len == 0 || rec->check_len > sizeof(rec->check)
                || (rec->vaddr >> 32) != hdr->base_address
                || rec->static_addr < rec->static_start
                || rec->static_addr - rec->static_start >= rec->static_len
                || rec->check_len > rec->static_len - (rec->static_addr - rec->static_start)) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "invalid static patch record");
                return false;
            }
            ix += sizeof(*rec);
            break;
        }
        case N00B_MARSHAL_OP_PSPATCH: {
            if (ctx->in.len - ix < sizeof(n00b_marshal_pspatch_record_t)) {
                return false;
            }
            n00b_marshal_pspatch_record_t *rec = (void *)(ctx->in.data + ix);
            uint32_t expected_len;
            const uint32_t flags_mask = N00B_STATIC_OBJECT_F_READONLY
                                      | N00B_STATIC_OBJECT_F_MUTABLE;
            if (hdr->version < 2
                || !pspatch_expected_len(rec->namespace_len,
                                          rec->key_len,
                                          rec->check_len,
                                          &expected_len)
                || rec->record_len != expected_len
                || rec->reserved != 0
                || (rec->vaddr >> 32) != hdr->base_address
                || rec->object_len == 0
                || rec->object_offset >= rec->object_len
                || rec->check_len > rec->object_len - rec->object_offset
                || rec->identity_version != N00B_STATIC_IDENTITY_VERSION
                || rec->identity_kind == N00B_STATIC_IDENTITY_NONE
                || rec->identity_kind > N00B_STATIC_IDENTITY_MANUAL
                || rec->flags_mask != flags_mask
                || (rec->flags_value & ~rec->flags_mask) != 0
                || rec->scan_kind > N00B_GC_SCAN_KIND_CALLBACK) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "invalid portable static patch record");
                return false;
            }
            if (ctx->in.len - ix < rec->record_len) {
                return false;
            }
            if (pspatch_payload_has_nul(rec)) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "portable static patch identity contains nul bytes");
                return false;
            }
            ix += rec->record_len;
            break;
        }
        case N00B_MARSHAL_OP_STOP: {
            if (ctx->in.len - ix < sizeof(n00b_marshal_stop_record_t)) {
                return false;
            }
            n00b_marshal_stop_record_t *rec = (void *)(ctx->in.data + ix);
            if (rec->end_of_stream != 1 || ix + sizeof(*rec) != ctx->in.len) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "invalid marshal stream terminator");
                return false;
            }
            return true;
        }
        default:
            marshal_set_error(&ctx->status,
                              &ctx->error,
                              N00B_MARSHAL_ERR_BAD_STREAM,
                              "unknown marshal stream record");
            return false;
        }
    }

    return false;
}

static void
patch_alloc_metadata(void *user_ptr, n00b_marshal_alloc_record_t *rec)
{
    n00b_alloc_info_t info = n00b_find_alloc_info(user_ptr);
    if (info.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = info.hdr.oob;
        oob->tinfo          = rec->tinfo;
        oob->ptr_words      = rec->ptr_words;
        oob->is_array       = rec->is_array != 0;
        oob->no_scan        = rec->no_scan != 0;
        oob->scan_kind      = rec->scan_kind;
        oob->scan_cb        = nullptr;
        oob->scan_user      = nullptr;
        if (oob->hcur) {
            oob->hcur->tinfo     = rec->tinfo;
            oob->hcur->ptr_words = rec->ptr_words;
            oob->hcur->is_array  = rec->is_array != 0;
            oob->hcur->no_scan   = rec->no_scan != 0;
            oob->hcur->scan_kind = rec->scan_kind;
            oob->hcur->scan_cb   = nullptr;
            oob->hcur->scan_user = nullptr;
        }
        return;
    }
    if (info.kind == n00b_alloc_inline) {
        n00b_inline_hdr_t *hdr = info.hdr.in_line;
        hdr->tinfo            = rec->tinfo;
        hdr->ptr_words        = rec->ptr_words;
        hdr->is_array         = rec->is_array != 0;
        hdr->no_scan          = rec->no_scan != 0;
        hdr->scan_kind        = rec->scan_kind;
        hdr->scan_cb          = nullptr;
        hdr->scan_user        = nullptr;
    }
}

static bool
unmarshal_allocate_records(n00b_unmarshal_ctx_t *ctx)
{
    size_t ix = sizeof(n00b_marshal_stream_header_t);

    while (ix < ctx->in.len) {
        uint32_t op = *(uint32_t *)(ctx->in.data + ix);
        if (op == N00B_MARSHAL_OP_STOP) {
            return true;
        }
        if (op == N00B_MARSHAL_OP_CPATCH) {
            ix += sizeof(n00b_marshal_cpatch_record_t);
            continue;
        }
        if (op == N00B_MARSHAL_OP_SPATCH) {
            ix += sizeof(n00b_marshal_spatch_record_t);
            continue;
        }
        if (op == N00B_MARSHAL_OP_PSPATCH) {
            n00b_marshal_pspatch_record_t *rec = (void *)(ctx->in.data + ix);
            ix += rec->record_len;
            continue;
        }

        n00b_marshal_alloc_record_t *rec = (void *)(ctx->in.data + ix);
        ix += sizeof(*rec);

        n00b_alloc_opts_t opts = {
            .allocator = (n00b_allocator_t *)ctx->target_arena,
            .no_scan   = rec->no_scan != 0,
            .scan_kind = rec->scan_kind,
        };
        void *obj = _n00b_alloc_raw(1,
                                    rec->user_len ? rec->user_len : 1,
                                    0,
                                    "*unmarshal*",
                                    &opts);
        memcpy(obj, ctx->in.data + ix, (size_t)rec->user_len);
        patch_alloc_metadata(obj, rec);

        n00b_unmarshal_segment_t *seg = marshal_scratch_alloc(ctx->scratch_alloc,
                                                              sizeof(*seg));
        *seg = (n00b_unmarshal_segment_t){
            .vaddr    = rec->vaddr,
            .user_len = rec->user_len,
            .user_ptr = obj,
            .rec      = *rec,
        };
        segments_push(ctx, seg);

        ix += (size_t)rec->payload_len;
    }

    return false;
}

static bool
unmarshal_relink_records(n00b_unmarshal_ctx_t *ctx)
{
    size_t ix = sizeof(n00b_marshal_stream_header_t);

    while (ix < ctx->in.len) {
        uint32_t op = *(uint32_t *)(ctx->in.data + ix);
        if (op == N00B_MARSHAL_OP_STOP) {
            return true;
        }
        if (op == N00B_MARSHAL_OP_CPATCH) {
            n00b_marshal_cpatch_record_t *patch = (void *)(ctx->in.data + ix);
            uint64_t *slot = word_slot_for_vaddr(ctx, patch->vaddr);
            if (!slot) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "collision patch points outside a marshaled word slot");
                return false;
            }
            *slot = patch->value;
            ix += sizeof(*patch);
            continue;
        }
        if (op == N00B_MARSHAL_OP_SPATCH) {
            n00b_marshal_spatch_record_t *patch = (void *)(ctx->in.data + ix);
            uint64_t *slot = word_slot_for_vaddr(ctx, patch->vaddr);
            if (!slot) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "static patch points outside a marshaled word slot");
                return false;
            }

            auto mmap_opt = n00b_mmap_by_address((void *)(uintptr_t)patch->static_addr);
            if (!n00b_option_is_set(mmap_opt)
                || n00b_option_get(mmap_opt)->kind != n00b_mmap_static) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "static patch target is not mapped static memory");
                return false;
            }

            n00b_marshal_static_ref_t ref;
            if (!static_ref_view((void *)(uintptr_t)patch->static_addr, &ref)
                || ref.start != patch->static_start
                || ref.len != patch->static_len
                || (patch->object_id && ref.object_id != patch->object_id)
                || patch->check_len > ref.len - (patch->static_addr - ref.start)
                || memcmp((void *)(uintptr_t)patch->static_addr,
                          patch->check,
                          patch->check_len) != 0) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "static patch target failed validation");
                return false;
            }

            *slot = patch->static_addr;
            ix += sizeof(*patch);
            continue;
        }
        if (op == N00B_MARSHAL_OP_PSPATCH) {
            n00b_marshal_pspatch_record_t *patch = (void *)(ctx->in.data + ix);
            uint64_t *slot = word_slot_for_vaddr(ctx, patch->vaddr);
            if (!slot) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "portable static patch points outside a marshaled word slot");
                return false;
            }

            char *namespace_id = marshal_scratch_alloc(ctx->scratch_alloc,
                                                       patch->namespace_len + 1);
            memcpy(namespace_id, pspatch_namespace_bytes(patch), patch->namespace_len);
            namespace_id[patch->namespace_len] = '\0';

            char *object_key = marshal_scratch_alloc(ctx->scratch_alloc,
                                                     patch->key_len + 1);
            memcpy(object_key, pspatch_key_bytes(patch), patch->key_len);
            object_key[patch->key_len] = '\0';

            n00b_static_identity_t identity = {
                .version      = patch->identity_version,
                .kind         = (n00b_static_identity_kind_t)patch->identity_kind,
                .namespace_id = namespace_id,
                .object_key   = object_key,
            };
            n00b_static_identity_query_t query = {
                .checks = N00B_STATIC_IDENTITY_CHECK_LEN
                        | N00B_STATIC_IDENTITY_CHECK_TINFO
                        | N00B_STATIC_IDENTITY_CHECK_SCAN_KIND
                        | N00B_STATIC_IDENTITY_CHECK_FLAGS
                        | N00B_STATIC_IDENTITY_CHECK_BYTES,
                .len          = patch->object_len,
                .tinfo        = patch->tinfo,
                .scan_kind    = (n00b_gc_scan_kind_t)patch->scan_kind,
                .flags_mask   = patch->flags_mask,
                .flags_value  = patch->flags_value,
                .check_offset = patch->object_offset,
                .check_len    = patch->check_len,
                .check_bytes  = pspatch_check_bytes(patch),
            };

            n00b_alloc_range_t *range = nullptr;
            n00b_static_identity_status_t lookup = n00b_static_identity_lookup(&identity,
                                                                               &query,
                                                                               &range);
            if (lookup != N00B_STATIC_IDENTITY_OK) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  marshal_status_from_static_identity(lookup),
                                  "portable static patch target failed validation");
                return false;
            }
            if (range == nullptr) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_STATIC_IDENTITY_MISSING,
                                  "portable static patch target resolved to no object");
                return false;
            }

            *slot = (uint64_t)(uintptr_t)range->start + patch->object_offset;
            ix += patch->record_len;
            continue;
        }

        n00b_marshal_alloc_record_t *rec = (void *)(ctx->in.data + ix);
        n00b_unmarshal_segment_t *seg = segment_for_vaddr(ctx, rec->vaddr);
        if (!seg) {
            marshal_set_error(&ctx->status,
                              &ctx->error,
                              N00B_MARSHAL_ERR_BAD_STREAM,
                              "allocation record missing virtual segment");
            return false;
        }

        uint64_t nwords = marshal_scan_word_count(rec);
        uint64_t *words = (uint64_t *)seg->user_ptr;
        for (uint64_t i = 0; i < nwords; i++) {
            if (!scan_word_for_policy(rec, i)) {
                continue;
            }
            uint64_t word = words[i];
            if ((word >> 32) != ((n00b_marshal_stream_header_t *)ctx->in.data)->base_address) {
                continue;
            }
            void *target = addr_for_vaddr(ctx, word);
            if (!target) {
                marshal_set_error(&ctx->status,
                                  &ctx->error,
                                  N00B_MARSHAL_ERR_BAD_STREAM,
                                  "virtual pointer target missing");
                return false;
            }
            words[i] = (uint64_t)(uintptr_t)target;
        }

        ix += sizeof(*rec) + (size_t)rec->payload_len;
    }

    return false;
}

static bool
unmarshal_process(n00b_unmarshal_ctx_t *ctx, n00b_list_t(void *) *roots)
{
    n00b_marshal_stream_header_t *hdr = (void *)ctx->in.data;

    n00b_stop_the_world();
    bool saved_collection = ctx->target_arena->collection_enabled;
    ctx->target_arena->collection_enabled = false;

    bool ok = unmarshal_allocate_records(ctx) && unmarshal_relink_records(ctx);

    if (ok) {
        uint64_t root_vaddr = ((uint64_t)hdr->base_address << 32) | hdr->root_offset;
        void *root = addr_for_vaddr(ctx, root_vaddr);
        if (!root) {
            marshal_set_error(&ctx->status,
                              &ctx->error,
                              N00B_MARSHAL_ERR_BAD_STREAM,
                              "root virtual address missing");
            ok = false;
        }
        else {
            n00b_list_push(*roots, root);
        }
    }

    ctx->target_arena->collection_enabled = saved_collection;
    n00b_restart_the_world();
    return ok;
}

n00b_unmarshal_ctx_t *
n00b_unmarshal_ctx_new() _kargs
{
    n00b_arena_t *target_arena = nullptr;
}
{
    n00b_unmarshal_ctx_t *ctx = n00b_alloc(n00b_unmarshal_ctx_t);
    memset(ctx, 0, sizeof(*ctx));
    marshal_init_scratch(&ctx->scratch, &ctx->scratch_alloc, "unmarshal_scratch");
    ctx->target_arena = target_arena ? target_arena : n00b_get_runtime()->default_arena;
    ctx->status       = N00B_MARSHAL_OK;
    return ctx;
}

void
n00b_unmarshal_ctx_destroy(n00b_unmarshal_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    n00b_allocator_destroy((n00b_allocator_t *)&ctx->scratch);
    n00b_free(ctx);
}

n00b_marshal_status_t
n00b_unmarshal_ctx_status(n00b_unmarshal_ctx_t *ctx)
{
    return ctx ? ctx->status : N00B_MARSHAL_ERR_NULL_ARG;
}

const char *
n00b_unmarshal_ctx_error(n00b_unmarshal_ctx_t *ctx)
{
    if (!ctx) {
        return "null unmarshal context";
    }
    return ctx->error;
}

n00b_list_t(void *)
n00b_unmarshal_incremental(n00b_unmarshal_ctx_t *ctx, n00b_buffer_t *chunk)
{
    n00b_list_t(void *) roots = n00b_list_new(void *);
    if (!ctx || !chunk) {
        if (ctx) {
            marshal_set_error(&ctx->status,
                              &ctx->error,
                              N00B_MARSHAL_ERR_NULL_ARG,
                              "null unmarshal argument");
        }
        return roots;
    }
    if (ctx->closed) {
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_CONTEXT_CLOSED,
                          "unmarshal context already closed");
        return roots;
    }

    _n00b_buffer_rlock(chunk);
    if (chunk->byte_len > SIZE_MAX - ctx->in.len) {
        _n00b_buffer_unlock(chunk);
        marshal_set_error(&ctx->status,
                          &ctx->error,
                          N00B_MARSHAL_ERR_LIMIT,
                          "marshal stream input exceeds host size limit");
        return roots;
    }
    bytes_append(&ctx->in, ctx->scratch_alloc, chunk->data, chunk->byte_len);
    _n00b_buffer_unlock(chunk);

    if (ctx->status == N00B_MARSHAL_ERR_INCOMPLETE_STREAM) {
        ctx->status = N00B_MARSHAL_OK;
        ctx->error  = nullptr;
    }

    if (!stream_complete(ctx)) {
        if (ctx->status == N00B_MARSHAL_OK) {
            ctx->status = N00B_MARSHAL_ERR_INCOMPLETE_STREAM;
        }
        return roots;
    }

    ctx->status = N00B_MARSHAL_OK;
    if (unmarshal_process(ctx, &roots)) {
        ctx->closed = true;
    }
    return roots;
}

n00b_list_t(void *)
n00b_unmarshal(n00b_buffer_t *buf) _kargs
{
    n00b_arena_t *target_arena = nullptr;
}
{
    n00b_unmarshal_ctx_t *ctx = n00b_unmarshal_ctx_new(.target_arena = target_arena);
    n00b_list_t(void *) roots = n00b_unmarshal_incremental(ctx, buf);
    n00b_unmarshal_ctx_destroy(ctx);
    return roots;
}

void *
n00b_unmarshal_one(n00b_buffer_t *buf) _kargs
{
    n00b_arena_t *target_arena = nullptr;
}
{
    n00b_list_t(void *) roots = n00b_unmarshal(buf, .target_arena = target_arena);
    if (n00b_list_len(roots) == 0) {
        return nullptr;
    }
    return n00b_list_get(roots, 0);
}
