#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"

#if defined(__linux__) && defined(__x86_64__)
#define HOST_TARGET N00B_ABI_TARGET_X86_64_LINUX
#elif defined(__linux__) && defined(__aarch64__)
#define HOST_TARGET N00B_ABI_TARGET_ARM64_LINUX
#elif defined(__APPLE__) && defined(__aarch64__)
#define HOST_TARGET N00B_ABI_TARGET_ARM64_MACOS
#elif defined(_WIN32) && defined(_M_X64)
#define HOST_TARGET N00B_ABI_TARGET_X86_64_WINDOWS
#else
#error "runtime ABI option test needs a supported target"
#endif

typedef struct {
    n00b_abi_header_v1 header;
    uint8_t directory[6 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[8];
    n00b_abi_layout_v1 layouts[8];
    n00b_abi_physical_run_v1 runs[4];
    n00b_abi_trace_v1 traces[5];
    n00b_abi_tag_extractor_v1 tags[3];
    uint32_t indexes[13];
} image_t;

static void
entry(image_t *image, uint32_t ordinal, uint32_t kind,
      uint32_t width, uint32_t count, uint64_t offset)
{
    n00bb_abi_directory_entry_t row = {
        .kind = kind,
        .schema_version = N00B_ABI_V1_TABLE_SCHEMA_VERSION,
        .record_bytes = width,
        .record_count = count,
        .file_offset = offset,
        .byte_length = (uint64_t)width * count,
    };
    n00bb_abi_directory_encode_entry(image->directory
        + (size_t)ordinal * N00BB_ABI_DIRECTORY_BYTES, &row);
}

static void
initialize(image_t *image)
{
    memcpy(image->header.magic, N00B_ABI_V1_MAGIC, 8);
    image->header.container_version = N00B_ABI_V1_CONTAINER_VERSION;
    image->header.header_bytes = sizeof(image->header);
    image->header.target_id = HOST_TARGET;
    image->header.abi_profile_id = N00B_ABI_V1_PROFILE_ID;
    image->header.data_layout_profile = N00B_ABI_DATA_LAYOUT_PROFILE_V1;
    image->header.required_alignment = 8;
    image->header.directory_count = 6;
    image->header.directory_offset = offsetof(image_t, directory);
    image->header.total_extent = sizeof(*image);
    image->header.payload_checksum = UINT64_MAX;

    image->types[0].kind = N00B_ABI_TYPE_NIL;
    image->types[0].typehash = UINT64_C(0x0ea862bddb8ce94c);
    image->types[1] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_REFERENCE, .typehash = 1,
        .size_bits = 64, .alignment_bytes = 8,
        .layout_index = 1, .tracing_index = 1,
        .components = {0, 1},
    };
    image->types[2] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_OPTION, .typehash = 2,
        .size_bits = 64, .alignment_bytes = 8,
        .layout_index = 2, .tracing_index = 2,
        .components = {1, 1},
    };
    image->types[3] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_PRIMITIVE, .typehash = 3,
        .size_bits = 64, .alignment_bytes = 8,
        .semantic_width_bits = 64, .unpacked_storage_bits = 64,
        .layout_index = 3,
    };
    image->types[4] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_PRIMITIVE, .typehash = 4,
        .size_bits = 8, .alignment_bytes = 1,
        .semantic_width_bits = 8, .unpacked_storage_bits = 8,
        .layout_index = 4,
    };
    image->types[5] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_OPTION, .typehash = 5,
        .size_bits = 9, .alignment_bytes = 1,
        .layout_index = 5, .components = {8, 1},
    };
    image->types[6] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_WRAPPER, .typehash = 6,
        .size_bits = 64, .alignment_bytes = 8,
        .layout_index = 6, .tracing_index = 3,
        .components = {10, 1},
    };
    image->types[7] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_ATOMIC, .typehash = 7,
        .size_bits = 64, .alignment_bytes = 8,
        .semantic_width_bits = 64, .unpacked_storage_bits = 64,
        .layout_index = 7, .tracing_index = 4,
        .components = {12, 1},
    };
    image->layouts[1] = (n00b_abi_layout_v1){
        .type_index = 1, .occupied_runs = {2, 1},
    };
    image->layouts[2] = (n00b_abi_layout_v1){
        .type_index = 2, .occupied_runs = {3, 1},
        .tag_extractor_index = 1,
    };
    image->layouts[3] = (n00b_abi_layout_v1){
        .type_index = 3, .occupied_runs = {4, 1},
    };
    image->layouts[4] = (n00b_abi_layout_v1){
        .type_index = 4, .occupied_runs = {5, 1},
    };
    image->layouts[5] = (n00b_abi_layout_v1){
        .type_index = 5, .occupied_runs = {6, 2},
        .tag_extractor_index = 2,
    };
    image->layouts[6] = (n00b_abi_layout_v1){
        .type_index = 6, .occupied_runs = {9, 1},
    };
    image->layouts[7] = (n00b_abi_layout_v1){
        .type_index = 7, .occupied_runs = {11, 1},
    };
    image->runs[1].width_bits = 64;
    image->runs[2].width_bits = 8;
    image->runs[3] = (n00b_abi_physical_run_v1){
        .byte_offset = 1, .width_bits = 1,
    };
    image->traces[1].type_index = 1;
    image->traces[2] = (n00b_abi_trace_v1){
        .type_index = 2, .child_trace_index = 1,
        .tag_extractor_index = 1,
    };
    image->traces[3] = (n00b_abi_trace_v1){
        .type_index = 6, .child_trace_index = 1,
    };
    image->traces[4] = (n00b_abi_trace_v1){
        .type_index = 7, .child_trace_index = 1,
    };
    image->tags[1] = (n00b_abi_tag_extractor_v1){
        .mask = UINT64_MAX, .unit_width_bits = 64,
        .tag_width_bits = 1, .kind = N00B_ABI_TAG_POINTER_NULL,
    };
    image->tags[2] = (n00b_abi_tag_extractor_v1){
        .unit_byte_offset = 1, .mask = 1, .unit_width_bits = 8,
        .tag_width_bits = 1, .kind = N00B_ABI_TAG_DIRECT_BITS,
    };
    image->indexes[0] = 3;
    image->indexes[1] = 1;
    image->indexes[2] = 1;
    image->indexes[3] = 1;
    image->indexes[4] = 1;
    image->indexes[5] = 2;
    image->indexes[6] = 2;
    image->indexes[7] = 3;
    image->indexes[8] = 4;
    image->indexes[9] = 1;
    image->indexes[10] = 1;
    image->indexes[11] = 1;
    image->indexes[12] = 1;

    entry(image, 0, N00B_ABI_TABLE_TYPE, sizeof(image->types[0]),
          8, offsetof(image_t, types));
    entry(image, 1, N00B_ABI_TABLE_LAYOUT, sizeof(image->layouts[0]),
          8, offsetof(image_t, layouts));
    entry(image, 2, N00B_ABI_TABLE_PHYSICAL_RUN, sizeof(image->runs[0]),
          4, offsetof(image_t, runs));
    entry(image, 3, N00B_ABI_TABLE_TRACE, sizeof(image->traces[0]),
          5, offsetof(image_t, traces));
    entry(image, 4, N00B_ABI_TABLE_TAG_EXTRACTOR, sizeof(image->tags[0]),
          3, offsetof(image_t, tags));
    entry(image, 5, N00B_ABI_TABLE_U32_STREAM, sizeof(image->indexes[0]),
          13, offsetof(image_t, indexes));
}

static void
rejected(image_t *image, uint32_t table_kind)
{
    n00b_abi_image_descriptor_v1 descriptor = {
        .descriptor_version = 1,
        .metadata = &image->header,
        .metadata_bytes = sizeof(*image),
    };
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && error.table_kind == table_kind
        && !generation && !n00b_abi_v1_current_snapshot());
}

int
main(void)
{
    _Alignas(16) image_t image = {0};
    initialize(&image);
    image.types[2].tracing_index = 0;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.types[2].tracing_index = 2;
    image.traces[2].child_trace_index = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[2].child_trace_index = 1;
    image.traces[2].option_bit_offset = 1;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[2].option_bit_offset = 0;
    image.tags[1].mask = 0;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.tags[1].mask = UINT64_MAX;
    image.tags[2].lsb_position = 1;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.tags[2].lsb_position = 0;
    image.traces[1].child_trace_index = 2;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[1].child_trace_index = 0;
    image.types[6].tracing_index = 0;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.types[6].tracing_index = 3;
    image.traces[3].child_trace_index = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[3].child_trace_index = 1;
    image.types[7].tracing_index = 0;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.types[7].tracing_index = 4;
    image.traces[4].child_trace_index = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[4].child_trace_index = 1;

    n00b_abi_image_descriptor_v1 descriptor = {
        .descriptor_version = 1,
        .metadata = &image.header,
        .metadata_bytes = sizeof(image),
    };
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && generation == 1 && !error.status);
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_type(snapshot, 2)->tracing_index == 2
        && n00b_abi_v1_layout(snapshot, 5)->tag_extractor_index == 2);
    return 0;
}
