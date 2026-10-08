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
#error "runtime ABI trace test needs a supported target"
#endif

typedef struct {
    n00b_abi_header_v1 header;
    uint8_t directory[10 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[4];
    n00b_abi_layout_v1 layouts[4];
    n00b_abi_field_v1 fields[2];
    n00b_abi_arm_v1 arms[2];
    n00b_abi_physical_run_v1 runs[3];
    n00b_abi_trace_v1 traces[3];
    n00b_abi_trace_child_v1 children[3];
    n00b_abi_trace_arm_v1 trace_arms[2];
    n00b_abi_tag_extractor_v1 tags[2];
    uint32_t indexes[10];
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
    image->header.directory_count = 10;
    image->header.directory_offset = offsetof(image_t, directory);
    image->header.total_extent = sizeof(*image);
    image->header.payload_checksum = UINT64_MAX;

    image->types[0].kind = N00B_ABI_TYPE_NIL;
    image->types[0].typehash = UINT64_C(0x0ea862bddb8ce94c);
    image->types[1] = (n00b_abi_type_v1){
        .typehash = 1, .kind = N00B_ABI_TYPE_RECORD,
        .size_bits = 128, .alignment_bytes = 8,
        .layout_index = 1, .tracing_index = 1,
        .fields_or_arms = {0, 1},
    };
    image->types[2] = (n00b_abi_type_v1){
        .typehash = 2, .kind = N00B_ABI_TYPE_STRING,
        .size_bits = 128, .alignment_bytes = 8, .layout_index = 2,
    };
    image->types[3] = (n00b_abi_type_v1){
        .typehash = 3, .kind = N00B_ABI_TYPE_VARIANT,
        .size_bits = 129, .alignment_bytes = 8,
        .layout_index = 3, .tracing_index = 2,
        .fields_or_arms = {4, 1},
    };
    image->layouts[1] = (n00b_abi_layout_v1){
        .type_index = 1, .occupied_runs = {1, 1},
    };
    image->layouts[2] = (n00b_abi_layout_v1){
        .type_index = 2, .occupied_runs = {2, 1},
    };
    image->layouts[3] = (n00b_abi_layout_v1){
        .type_index = 3, .occupied_runs = {5, 2},
        .tag_extractor_index = 1,
    };
    image->fields[1] = (n00b_abi_field_v1){
        .parent_type_index = 1, .child_type_index = 2,
        .visibility = N00B_ABI_VISIBILITY_PRIVATE,
    };
    image->arms[1] = (n00b_abi_arm_v1){
        .field = {
            .parent_type_index = 3, .child_type_index = 2,
            .visibility = N00B_ABI_VISIBILITY_PRIVATE,
            .physical_runs = {7, 1},
        },
        .tag_value = 1,
    };
    image->runs[1] = (n00b_abi_physical_run_v1){
        .width_bits = 128,
    };
    image->runs[2] = (n00b_abi_physical_run_v1){
        .byte_offset = 16, .width_bits = 1,
    };
    image->traces[1] = (n00b_abi_trace_v1){
        .type_index = 1, .fixed_children = {3, 1},
    };
    image->traces[2] = (n00b_abi_trace_v1){
        .type_index = 3, .tag_extractor_index = 1,
        .arms = {8, 1},
    };
    image->children[1].child_type_index = 2;
    image->children[2].child_type_index = 2;
    image->trace_arms[1] = (n00b_abi_trace_arm_v1){
        .tag_value = 1, .children = {9, 1},
    };
    image->tags[1] = (n00b_abi_tag_extractor_v1){
        .unit_byte_offset = 16, .mask = 1,
        .unit_width_bits = 8, .tag_width_bits = 1,
        .kind = N00B_ABI_TAG_DIRECT_BITS,
    };
    image->indexes[0] = 1;
    image->indexes[1] = 1;
    image->indexes[2] = 1;
    image->indexes[3] = 1;
    image->indexes[4] = 1;
    image->indexes[5] = 1;
    image->indexes[6] = 2;
    image->indexes[7] = 1;
    image->indexes[8] = 1;
    image->indexes[9] = 2;

    entry(image, 0, N00B_ABI_TABLE_TYPE, sizeof(image->types[0]),
          4, offsetof(image_t, types));
    entry(image, 1, N00B_ABI_TABLE_LAYOUT, sizeof(image->layouts[0]),
          4, offsetof(image_t, layouts));
    entry(image, 2, N00B_ABI_TABLE_FIELD, sizeof(image->fields[0]),
          2, offsetof(image_t, fields));
    entry(image, 3, N00B_ABI_TABLE_ARM, sizeof(image->arms[0]),
          2, offsetof(image_t, arms));
    entry(image, 4, N00B_ABI_TABLE_PHYSICAL_RUN, sizeof(image->runs[0]),
          3, offsetof(image_t, runs));
    entry(image, 5, N00B_ABI_TABLE_TRACE, sizeof(image->traces[0]),
          3, offsetof(image_t, traces));
    entry(image, 6, N00B_ABI_TABLE_TRACE_CHILD, sizeof(image->children[0]),
          3, offsetof(image_t, children));
    entry(image, 7, N00B_ABI_TABLE_TRACE_ARM, sizeof(image->trace_arms[0]),
          2, offsetof(image_t, trace_arms));
    entry(image, 8, N00B_ABI_TABLE_TAG_EXTRACTOR, sizeof(image->tags[0]),
          2, offsetof(image_t, tags));
    entry(image, 9, N00B_ABI_TABLE_U32_STREAM, sizeof(image->indexes[0]),
          10, offsetof(image_t, indexes));
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
        == N00B_ABI_BAD_METADATA
        && error.table_kind == table_kind
        && !generation && !n00b_abi_v1_current_snapshot());
}

int
main(void)
{
    _Alignas(16) image_t image = {0};
    initialize(&image);
    image.traces[1].reserved_zero = 1;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[1].reserved_zero = 0;
    image.types[1].tracing_index = 0;
    rejected(&image, N00B_ABI_TABLE_TYPE);
    image.types[1].tracing_index = 1;
    image.traces[1].fixed_children = (n00b_abi_range_v1){0};
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[1].fixed_children = (n00b_abi_range_v1){3, 2};
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.traces[1].fixed_children.count = 1;
    image.children[1].child_type_index = 1;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.children[1].child_type_index = 2;
    image.children[1].byte_offset = 1;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.children[1].byte_offset = 0;
    image.children[1].child_type_index = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.children[1].child_type_index = 2;
    image.types[2].flags = N00B_ABI_TYPE_FLAG_NON_CONCRETE;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.types[2].flags = 0;
    image.indexes[1] = 2;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.indexes[1] = 1;
    image.traces[2].tag_extractor_index = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.traces[2].tag_extractor_index = 1;
    image.tags[1].mask = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.tags[1].mask = 1;
    image.tags[1].unit_width_bits = 64;
    rejected(&image, N00B_ABI_TABLE_TRACE);
    image.tags[1].unit_width_bits = 8;
    image.tags[1].unit_byte_offset = 0;
    rejected(&image, N00B_ABI_TABLE_TRACE_ARM);
    image.tags[1].unit_byte_offset = 16;
    image.trace_arms[1].tag_value = 2;
    rejected(&image, N00B_ABI_TABLE_TRACE_ARM);
    image.trace_arms[1].tag_value = 1;
    image.trace_arms[1].children = (n00b_abi_range_v1){0};
    rejected(&image, N00B_ABI_TABLE_TRACE_ARM);
    image.trace_arms[1].children = (n00b_abi_range_v1){9, 1};
    image.arms[1].field.child_type_index = 1;
    image.arms[1].field.logical_bit_offset = 8;
    image.trace_arms[1].child_trace_index = 1;
    image.trace_arms[1].children = (n00b_abi_range_v1){0};
    rejected(&image, N00B_ABI_TABLE_TRACE_ARM);
    image.arms[1].field.child_type_index = 2;
    image.arms[1].field.logical_bit_offset = 0;
    image.trace_arms[1].child_trace_index = 0;
    image.trace_arms[1].children = (n00b_abi_range_v1){9, 1};
    image.arms[1].field.physical_runs = (n00b_abi_range_v1){6, 1};
    rejected(&image, N00B_ABI_TABLE_TRACE_ARM);
    image.arms[1].field.physical_runs = (n00b_abi_range_v1){7, 1};
    image.children[2].byte_offset = 8;
    rejected(&image, N00B_ABI_TABLE_TRACE_CHILD);
    image.children[2].byte_offset = 0;

    n00b_abi_image_descriptor_v1 descriptor = {
        .descriptor_version = 1,
        .metadata = &image.header,
        .metadata_bytes = sizeof(image),
    };
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && generation == 1 && !error.status);
    const n00b_abi_snapshot_v1 *snapshot =
        n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_type(snapshot, 1)->tracing_index == 1
        && n00b_abi_v1_trace(snapshot, 2)->type_index == 3);
    assert(n00b_abi_v1_layout(snapshot, 1)->type_index == 1
        && n00b_abi_v1_field(snapshot, 1)->parent_type_index == 1
        && n00b_abi_v1_trace(snapshot, 1)->type_index == 1
        && !n00b_abi_v1_layout(snapshot, 4)
        && !n00b_abi_v1_field(snapshot, 2)
        && !n00b_abi_v1_trace(snapshot, 3));
    return 0;
}
