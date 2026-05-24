#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "adt/dict_untyped.h"
#include "core/buffer.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/static_image.h"
#include "core/static_objects.h"
#include "core/type_info.h"

const n00b_buffer_t *readonly_buffer =
    ncc_static_image("phase-six");
const n00b_buffer_t *hex_buffer =
    ncc_static_image(.hex = "6869");
const n00b_buffer_t *sized_buffer =
    ncc_static_image(.length = 3);
const n00b_buffer_t *raw_buffer =
    ncc_static_image(.raw = "raw", .length = 3, .no_lock = true);
const n00b_buffer_t *literal_buffer =
    b"literal";

static n00b_alloc_range_t *
range_for_address(const void *addr)
{
    auto found = n00b_mmap_range_by_address((void *)addr);
    assert(n00b_option_is_set(found));
    return n00b_option_get(found);
}

static void
assert_static_image_identity(n00b_alloc_range_t *range,
                             n00b_static_identity_kind_t kind,
                             const char *key_prefix)
{
    assert(range->identity != nullptr);
    assert(range->identity->version == N00B_STATIC_IDENTITY_VERSION);
    assert(range->identity->kind == kind);
    assert(range->identity->namespace_id != nullptr);
    assert(strcmp(range->identity->namespace_id, "n00b.tests") == 0);
    assert(range->identity->object_key != nullptr);
    assert(strncmp(range->identity->object_key,
                   key_prefix,
                   strlen(key_prefix)) == 0);
    assert(strstr(range->identity->object_key,
                  "test_ncc_static_images.c:") != nullptr);

    n00b_static_identity_query_t query = {
        .checks      = N00B_STATIC_IDENTITY_CHECK_LEN
                     | N00B_STATIC_IDENTITY_CHECK_SCAN_KIND
                     | N00B_STATIC_IDENTITY_CHECK_FLAGS,
        .len         = range->len,
        .scan_kind   = range->scan_kind,
        .flags_mask  = range->flags,
        .flags_value = range->flags,
    };
    n00b_alloc_range_t *resolved = nullptr;
    assert(n00b_static_identity_lookup(range->identity, &query, &resolved)
           == N00B_STATIC_IDENTITY_OK);
    assert(resolved == range);
}

static n00b_static_layout_info_t *
require_layout(uint64_t type_hash)
{
    auto layout_opt = n00b_type_static_layout(type_hash);
    assert(n00b_option_is_set(layout_opt));
    return n00b_option_get(layout_opt);
}

static uint64_t
expected_capacity(uint64_t len)
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

static void
assert_buffer_payload(const n00b_buffer_t *buf, const char *expected,
                      uint64_t len)
{
    assert(buf != nullptr);
    assert(buf->byte_len == len);
    assert(buf->alloc_len == expected_capacity(len));
    assert(buf->lock == nullptr);
    assert(buf->allocator == nullptr);
    assert(buf->flags & N00B_BUF_F_BORROWED);
    assert(buf->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(buf->scan_cb == nullptr);
    assert(buf->scan_user == nullptr);

    if (len != 0) {
        assert(memcmp(buf->data, expected, (size_t)len) == 0);
    }
    for (uint64_t i = len; i < buf->alloc_len; i++) {
        assert(buf->data[i] == 0);
    }
}

static void
assert_static_buffer_range(const n00b_buffer_t *buf)
{
    n00b_alloc_range_t *object_range = range_for_address(buf);
    assert(object_range->kind == n00b_mmap_static);
    assert(object_range->len == sizeof(*buf));
    assert(object_range->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(object_range->scan_cb == n00b_gc_scan_cb_struct_layout);
    assert(object_range->flags & N00B_STATIC_OBJECT_F_READONLY);
    assert_static_image_identity(
        object_range,
        N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_OBJECT,
        "ncc-static-image-object:");

    n00b_gc_struct_layout_t *layout = object_range->scan_user;
    assert(layout != nullptr);
    assert(layout->count == 1);
    assert(layout->stride == sizeof(*buf) / sizeof(void *));
    assert(layout->offset_count == 1);
    assert(layout->offsets[0] == offsetof(n00b_buffer_t, data) / sizeof(void *));

    n00b_alloc_range_t *payload_range = range_for_address(buf->data);
    assert(payload_range->kind == n00b_mmap_static);
    assert(payload_range->len == buf->alloc_len);
    assert(payload_range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(payload_range->flags & N00B_STATIC_OBJECT_F_READONLY);
    assert_static_image_identity(
        payload_range,
        N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_PAYLOAD,
        "ncc-static-image-payload:");
}

static n00b_static_image_request_t
buffer_request(const n00b_static_init_arg_t *args, uint64_t arg_count)
{
    return (n00b_static_image_request_t){
        .version            = N00B_STATIC_IMAGE_CONTRACT_VERSION,
        .type_hash          = typehash(n00b_buffer_t *),
        .type_name          = "n00b_buffer_t",
        .symbol_prefix      = "__manual_buffer",
        .entry_attr         = "",
        .payload_kind       = N00B_STATIC_IMAGE_PAYLOAD_NONE,
        .payload            = nullptr,
        .payload_len        = 0,
        .args               = args,
        .arg_count          = arg_count,
        .target_abi         = N00B_STATIC_IMAGE_ABI_INIT,
        .object_flags       = N00B_STATIC_OBJECT_F_READONLY,
        .required_scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
    };
}

static void
test_static_image_request_validation(void)
{
    static const unsigned char payload[] = "manual";
    n00b_static_init_arg_t arg = {
        .kind  = N00B_STATIC_INIT_ARG_BYTES,
        .bytes = {.data = payload, .len = sizeof(payload) - 1},
    };
    n00b_static_image_request_t req = buffer_request(&arg, 1);

    assert(n00b_static_image_validate_request(nullptr)
           == N00B_STATIC_IMAGE_ERR_NULL_REQUEST);
    assert(n00b_static_image_validate_request(&req) == N00B_STATIC_IMAGE_OK);
    assert(strcmp(n00b_static_image_status_name(N00B_STATIC_IMAGE_OK), "ok")
           == 0);

    req.version = 0;
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_VERSION);
    req.version = N00B_STATIC_IMAGE_CONTRACT_VERSION;

    req.target_abi.pointer_bytes = 1;
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_ABI);
    req.target_abi = (n00b_static_image_abi_t)N00B_STATIC_IMAGE_ABI_INIT;

    req.payload_kind = N00B_STATIC_IMAGE_PAYLOAD_BYTES;
    req.payload = nullptr;
    req.payload_len = 1;
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_PAYLOAD);
    req.payload_kind = N00B_STATIC_IMAGE_PAYLOAD_NONE;
    req.payload_len = 0;

    req.required_scan_kind = N00B_GC_SCAN_KIND_NONE;
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_SCAN_KIND);
    req.required_scan_kind = N00B_GC_SCAN_KIND_CALLBACK;

    req.type_hash = UINT64_C(0xDEADBEEFCAFEBABE);
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE);

    req.type_hash = typehash(n00b_dict_untyped_t *);
    req.required_scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    assert(n00b_static_image_validate_request(&req)
           == N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY);

    printf("  [PASS] static image request validation\n");
}

static void
test_static_initializer_build_api(void)
{
    static const unsigned char payload[] = "manual";
    n00b_static_init_arg_t arg = {
        .kind  = N00B_STATIC_INIT_ARG_BYTES,
        .bytes = {.data = payload, .len = sizeof(payload) - 1},
    };
    n00b_static_image_request_t req = buffer_request(&arg, 1);

    n00b_static_image_builder_t builder;
    n00b_static_image_status_t status =
        n00b_static_image_build(&req, &builder);
    assert(status == N00B_STATIC_IMAGE_OK);
    assert(builder.expr != nullptr);
    assert(builder.decls != nullptr);
    assert(strcmp(builder.expr, "&__manual_buffer_obj") == 0);
    assert(strstr(builder.decls, "n00b_buffer_t __manual_buffer_obj") != nullptr);
    assert(strstr(builder.decls, ".byte_len=6ULL") != nullptr);
    n00b_static_image_builder_destroy(&builder);

    printf("  [PASS] static initializer build API\n");
}

static void
test_generated_static_image_registration(void)
{
    n00b_static_objects_register_all();

    assert_buffer_payload(readonly_buffer, "phase-six", 9);
    assert_buffer_payload(hex_buffer, "hi", 2);
    assert_buffer_payload(sized_buffer, "\0\0\0", 3);
    assert_buffer_payload(raw_buffer, "raw", 3);
    assert_buffer_payload(literal_buffer, "literal", 7);

    assert_static_buffer_range(readonly_buffer);
    assert_static_buffer_range(hex_buffer);
    assert_static_buffer_range(sized_buffer);
    assert_static_buffer_range(raw_buffer);
    assert_static_buffer_range(literal_buffer);

    n00b_buffer_t *heap_buffer = n00b_buffer_from_bytes("heap", 4);
    assert(heap_buffer->byte_len == 4);
    assert(memcmp(heap_buffer->data, "heap", 4) == 0);
    assert((heap_buffer->flags & N00B_BUF_F_BORROWED) == 0);

    printf("  [PASS] generated static image registration\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_static_layout_info_t *layout =
        require_layout(typehash(n00b_buffer_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);

    printf("Running ncc static image tests...\n");
    test_static_image_request_validation();
    test_static_initializer_build_api();
    test_generated_static_image_registration();
    printf("All ncc static image tests passed.\n");

    n00b_shutdown();
    return 0;
}
