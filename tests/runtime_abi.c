#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"

#if defined(__linux__) && defined(__x86_64__)
#define HOST_TARGET N00B_ABI_TARGET_X86_64_LINUX
#elif defined(__APPLE__) && defined(__aarch64__)
#define HOST_TARGET N00B_ABI_TARGET_ARM64_MACOS
#elif defined(_WIN32) && defined(_M_X64)
#define HOST_TARGET N00B_ABI_TARGET_X86_64_WINDOWS
#else
#error "runtime ABI test needs a supported target"
#endif

typedef struct {
    n00b_abi_header_v1 header;
    uint8_t directory[4 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[1];
    uint32_t indexes[2];
    uint64_t values[1];
    uint8_t bytes[3];
} image_t;

static void
directory_entry(image_t *image, uint32_t ordinal, uint32_t kind,
                uint32_t width, uint32_t count, uint64_t offset)
{
    n00bb_abi_directory_entry_t entry = {
        .kind = kind,
        .schema_version = N00B_ABI_V1_TABLE_SCHEMA_VERSION,
        .record_bytes = width,
        .record_count = count,
        .file_offset = offset,
        .byte_length = (uint64_t)width * count,
    };
    n00bb_abi_directory_encode_entry(image->directory
        + (size_t)ordinal * N00BB_ABI_DIRECTORY_BYTES, &entry);
}

int
main(void)
{
    assert(!n00b_abi_v1_current_snapshot());
    _Alignas(16) image_t image = {0};
    memcpy(image.header.magic, N00B_ABI_V1_MAGIC, 8);
    image.header.container_version = N00B_ABI_V1_CONTAINER_VERSION;
    image.header.header_bytes = sizeof(image.header);
    image.header.target_id = HOST_TARGET;
    image.header.abi_profile_id = N00B_ABI_V1_PROFILE_ID;
    image.header.data_layout_profile = N00B_ABI_DATA_LAYOUT_PROFILE_V1;
    image.header.required_alignment = 8;
    image.header.directory_count = 4;
    image.header.directory_offset = offsetof(image_t, directory);
    image.header.total_extent = sizeof(image);
    image.header.payload_checksum = UINT64_MAX;
    image.types[0].kind = N00B_ABI_TYPE_NIL;
    image.types[0].typehash = UINT64_C(0x0ea862bddb8ce94c);
    image.indexes[0] = 3;
    image.indexes[1] = 5;
    image.values[0] = 123;
    memcpy(image.bytes, "abc", 3);
    directory_entry(&image, 0, N00B_ABI_TABLE_TYPE,
        sizeof(image.types[0]), 1, offsetof(image_t, types));
    directory_entry(&image, 1, N00B_ABI_TABLE_U32_STREAM,
        sizeof(image.indexes[0]), 2, offsetof(image_t, indexes));
    directory_entry(&image, 2, N00B_ABI_TABLE_U64_STREAM,
        sizeof(image.values[0]), 1, offsetof(image_t, values));
    directory_entry(&image, 3, N00B_ABI_TABLE_BYTE_STREAM,
        sizeof(image.bytes[0]), 3, offsetof(image_t, bytes));

    n00b_abi_image_descriptor_v1 descriptor = {
        .descriptor_version = 1,
        .metadata = &image.header,
        .metadata_bytes = sizeof(image),
    };
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    descriptor.metadata = (const void *)((const uint8_t *)&image.header + 1);
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_ARGUMENT && !n00b_abi_v1_current_snapshot());
    descriptor.metadata = &image.header;
    image.header.target_id = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && error.status == N00B_ABI_BAD_METADATA
        && !n00b_abi_v1_current_snapshot());
    image.header.target_id = HOST_TARGET;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && !error.status && generation == 1);
    const n00b_abi_snapshot_v1 *snapshot =
        n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_generation(snapshot) == 1
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE) == 1
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_MODULE) == 0);
    uint32_t width = 0;
    const n00b_abi_type_v1 *nil = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TYPE, 0, &width);
    assert(nil == n00b_abi_v1_type(snapshot, 0)
        && width == sizeof(*nil) && nil->kind == N00B_ABI_TYPE_NIL
        && !n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TYPE, 1, &width)
        && !width && !n00b_abi_v1_module(snapshot, 1));
    uint32_t index = 0;
    uint64_t value = 0;
    const uint8_t *bytes = nullptr;
    assert(n00b_abi_v1_index_at(snapshot, (n00b_abi_range_v1){0, 2},
        1, &index) == 1 && index == 5);
    assert(n00b_abi_v1_index_at(snapshot, (n00b_abi_range_v1){0, 2},
        2, &index) == 0);
    assert(n00b_abi_v1_u64_at(snapshot, (n00b_abi_range_v1){0, 1},
        0, &value) == 1 && value == 123);
    assert(n00b_abi_v1_bytes(snapshot, (n00b_abi_range_v1){0, 3},
        &bytes) == 1 && !memcmp(bytes, "abc", 3));
    assert(n00b_abi_v1_bytes(snapshot, (n00b_abi_range_v1){1, 0},
        &bytes) == -1);
    n00b_abi_string_v1 text = {0};
    memcpy(text.short_value.bytes, "abc", 3);
    text.short_value.selector = 3;
    uint64_t length = 0;
    assert(n00b_abi_v1_string_bytes(snapshot, &text, &bytes, &length) == 1
        && length == 3 && !memcmp(bytes, "abc", 3));
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_UNRESOLVED
        && n00b_abi_v1_current_snapshot() == snapshot
        && n00b_abi_v1_generation(snapshot) == 1);
    return 0;
}
