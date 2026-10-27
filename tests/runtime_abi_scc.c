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
#error "runtime ABI SCC test needs a supported target"
#endif

typedef struct {
    n00b_abi_header_v1 header;
    uint8_t directory[7 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[1];
    uint32_t indexes[12];
    n00b_abi_package_v1 packages[2];
    n00b_abi_module_v1 modules[4];
    n00b_abi_scc_v1 sccs[2];
    n00b_abi_source_file_v1 files[2];
    n00b_abi_runtime_config_v1 configs[2];
} image_t;

static void
table(image_t *image, uint32_t ordinal, uint32_t kind,
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
    image->header.directory_count = 7;
    image->header.directory_offset = offsetof(image_t, directory);
    image->header.total_extent = sizeof(*image);
    image->header.payload_checksum = UINT64_MAX;
    image->types[0].kind = N00B_ABI_TYPE_NIL;
    image->types[0].typehash = UINT64_C(0x0ea862bddb8ce94c);
    const uint32_t indexes[] = {
        1, 2, 3, 2, 3, 1, 1, 2, 3, 1, 2, 3,
    };
    memcpy(image->indexes, indexes, sizeof(indexes));
    image->packages[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    image->packages[1].name.short_value.bytes[0] = 'p';
    image->packages[1].name.short_value.selector = 1;
    image->packages[1].modules = (n00b_abi_range_v1){0, 3};
    for (uint32_t i = 1; i <= 3; i++) {
        image->modules[i].package_index = 1;
        image->modules[i].scc_index = 1;
        image->modules[i].runtime_config_index = 1;
        image->modules[i].source_file_index = 1;
        image->modules[i].imports = (n00b_abi_range_v1){i + 2, 1};
        image->modules[i].full_path.short_value.bytes[0] = 'p';
        image->modules[i].full_path.short_value.bytes[1] = '.';
        image->modules[i].full_path.short_value.bytes[2] = (uint8_t)('a' + i - 1);
        image->modules[i].full_path.short_value.selector = 3;
    }
    image->sccs[1].interface_identity = 1;
    image->sccs[1].freshness_identity = 2;
    image->sccs[1].members = (n00b_abi_range_v1){6, 3};
    image->sccs[1].lexical_order = (n00b_abi_range_v1){9, 3};
    image->files[1].path.short_value.bytes[0] = 'x';
    image->files[1].path.short_value.selector = 1;
    table(image, 0, N00B_ABI_TABLE_TYPE, sizeof(image->types[0]), 1,
          offsetof(image_t, types));
    table(image, 1, N00B_ABI_TABLE_U32_STREAM, sizeof(image->indexes[0]), 12,
          offsetof(image_t, indexes));
    table(image, 2, N00B_ABI_TABLE_PACKAGE, sizeof(image->packages[0]), 2,
          offsetof(image_t, packages));
    table(image, 3, N00B_ABI_TABLE_MODULE, sizeof(image->modules[0]), 4,
          offsetof(image_t, modules));
    table(image, 4, N00B_ABI_TABLE_SCC, sizeof(image->sccs[0]), 2,
          offsetof(image_t, sccs));
    table(image, 5, N00B_ABI_TABLE_SOURCE_FILE, sizeof(image->files[0]), 2,
          offsetof(image_t, files));
    table(image, 6, N00B_ABI_TABLE_RUNTIME_CONFIG,
          sizeof(image->configs[0]), 2, offsetof(image_t, configs));
}

static void
reject(const n00b_abi_image_descriptor_v1 *descriptor, uint32_t kind)
{
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    assert(n00b_abi_v1_register_image(descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && error.table_kind == kind
        && !generation && !n00b_abi_v1_current_snapshot());
}

int
main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--acyclic")));
    _Alignas(16) image_t image = {0};
    initialize(&image);
    n00b_abi_image_descriptor_v1 descriptor = {
        .descriptor_version = 1,
        .metadata = &image.header,
        .metadata_bytes = sizeof(image),
    };
    image.modules[1].scc_index = 0;
    reject(&descriptor, N00B_ABI_TABLE_MODULE);
    image.modules[1].scc_index = 1;
    image.indexes[7] = 1;
    reject(&descriptor, N00B_ABI_TABLE_SCC);
    image.indexes[7] = 2;
    image.indexes[10] = 1;
    reject(&descriptor, N00B_ABI_TABLE_SCC);
    image.indexes[10] = 2;
    image.modules[3].imports = (n00b_abi_range_v1){0};
    reject(&descriptor, N00B_ABI_TABLE_MODULE);
    image.modules[3].imports = (n00b_abi_range_v1){5, 1};
    image.modules[1].imports.count = 2;
    image.indexes[4] = 2;
    reject(&descriptor, N00B_ABI_TABLE_MODULE);
    image.modules[1].imports.count = 1;
    image.indexes[4] = 3;
    image.indexes[3] = 1;
    image.indexes[5] = 2;
    reject(&descriptor, N00B_ABI_TABLE_MODULE);
    image.indexes[3] = 2;
    image.indexes[5] = 1;
    if (argc == 2) {
        image.modules[3].imports = (n00b_abi_range_v1){0};
        for (uint32_t i = 1; i <= 3; i++) image.modules[i].scc_index = 0;
        reject(&descriptor, N00B_ABI_TABLE_SCC);
        table(&image, 4, N00B_ABI_TABLE_SCC, sizeof(image.sccs[0]), 1,
              offsetof(image_t, sccs));
    }
    n00b_abi_error_v1 error = {0};
    uint64_t generation = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && generation == 1 && !error.status);
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_generation(snapshot) == 1);
    return 0;
}
