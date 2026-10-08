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
    uint8_t directory[16 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[5];
    uint32_t indexes[8];
    uint64_t values[1];
    uint8_t bytes[3];
    n00b_abi_specialization_link_v1 links[3];
    n00b_abi_trait_method_v1 methods[2];
    n00b_abi_function_v1 functions[3];
    n00b_abi_dispatch_v1 dispatches[2];
    n00b_abi_dispatch_entry_v1 entries[3];
    n00b_abi_package_v1 packages[2];
    n00b_abi_module_v1 modules[2];
    n00b_abi_scope_v1 scopes[2];
    n00b_abi_source_file_v1 source_files[2];
    n00b_abi_runtime_config_v1 configs[2];
    n00b_abi_storage_v1 storages[3];
    n00b_abi_tls_relocation_v1 tls_relocations[2];
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
    image.header.directory_count = 16;
    image.header.directory_offset = offsetof(image_t, directory);
    image.header.total_extent = sizeof(image);
    image.header.payload_checksum = UINT64_MAX;
    image.types[0].kind = N00B_ABI_TYPE_NIL;
    image.types[0].typehash = UINT64_C(0x0ea862bddb8ce94c);
    image.types[1].kind = N00B_ABI_TYPE_TRAIT;
    image.types[1].typehash = 11;
    image.types[1].fields_or_arms = (n00b_abi_range_v1){2, 1};
    image.types[2].kind = N00B_ABI_TYPE_RECORD;
    image.types[2].typehash = 12;
    image.types[3].kind = N00B_ABI_TYPE_RECORD;
    image.types[3].typehash = 13;
    image.types[3].specialization_start_ix = 1;
    image.types[3].specialization_end_ix = 2;
    image.types[4].kind = N00B_ABI_TYPE_RECORD;
    image.types[4].typehash = 14;
    image.types[4].specialization_start_ix = 2;
    image.types[4].specialization_end_ix = 3;
    image.indexes[0] = 3;
    image.indexes[1] = 5;
    image.indexes[2] = 1;
    image.indexes[3] = 1;
    image.indexes[4] = 2;
    image.indexes[5] = 1;
    image.indexes[6] = 2;
    image.indexes[7] = 1;
    image.values[0] = 123;
    memcpy(image.bytes, "abc", 3);
    image.links[1] = 2;
    image.links[2] = 3;
    image.methods[1].trait_type_index = 1;
    image.methods[1].signature_type_index = 4;
    image.functions[1].module_index = 1;
    image.functions[1].scope_index = 1;
    image.functions[2].module_index = 1;
    image.functions[2].scope_index = 1;
    image.modules[1].package_index = 1;
    image.modules[1].runtime_config_index = 1;
    image.modules[1].source_file_index = 1;
    image.modules[1].static_storage = (n00b_abi_range_v1){5, 1};
    image.modules[1].tls_storage = (n00b_abi_range_v1){6, 1};
    image.storages[1].module_index = 1;
    image.storages[1].type_index = 2;
    image.storages[1].segment_kind = N00B_ABI_STORAGE_READ_ONLY;
    image.storages[1].segment_index = 1;
    image.storages[1].byte_length = 16;
    image.storages[2].module_index = 1;
    image.storages[2].type_index = 2;
    image.storages[2].segment_kind = N00B_ABI_STORAGE_TLS;
    image.storages[2].segment_index = 2;
    image.storages[2].byte_length = 16;
    image.storages[2].tls_relocations = (n00b_abi_range_v1){7, 1};
    image.tls_relocations[1].template_storage_index = 2;
    image.tls_relocations[1].referenced_storage_index = 1;
    image.tls_relocations[1].write_byte_offset = 8;
    image.tls_relocations[1].target_kind = N00B_ABI_STORAGE_READ_ONLY;
    image.scopes[1].module_index = 1;
    image.dispatches[1].trait_type_index = 1;
    image.dispatches[1].entries = (n00b_abi_range_v1){3, 2};
    image.entries[1].receiver_type_index = 2;
    image.entries[1].function_index = 1;
    image.entries[2].receiver_type_index = 3;
    image.entries[2].function_index = 2;
    directory_entry(&image, 0, N00B_ABI_TABLE_TYPE,
        sizeof(image.types[0]), 5, offsetof(image_t, types));
    directory_entry(&image, 1, N00B_ABI_TABLE_U32_STREAM,
        sizeof(image.indexes[0]), 8, offsetof(image_t, indexes));
    directory_entry(&image, 2, N00B_ABI_TABLE_U64_STREAM,
        sizeof(image.values[0]), 1, offsetof(image_t, values));
    directory_entry(&image, 3, N00B_ABI_TABLE_BYTE_STREAM,
        sizeof(image.bytes[0]), 3, offsetof(image_t, bytes));
    directory_entry(&image, 4, N00B_ABI_TABLE_SPECIALIZATION_LINK,
        sizeof(image.links[0]), 3, offsetof(image_t, links));
    directory_entry(&image, 5, N00B_ABI_TABLE_TRAIT_METHOD,
        sizeof(image.methods[0]), 2, offsetof(image_t, methods));
    directory_entry(&image, 6, N00B_ABI_TABLE_FUNCTION,
        sizeof(image.functions[0]), 3, offsetof(image_t, functions));
    directory_entry(&image, 7, N00B_ABI_TABLE_DISPATCH,
        sizeof(image.dispatches[0]), 2, offsetof(image_t, dispatches));
    directory_entry(&image, 8, N00B_ABI_TABLE_DISPATCH_ENTRY,
        sizeof(image.entries[0]), 3, offsetof(image_t, entries));
    directory_entry(&image, 9, N00B_ABI_TABLE_PACKAGE,
        sizeof(image.packages[0]), 2, offsetof(image_t, packages));
    directory_entry(&image, 10, N00B_ABI_TABLE_MODULE,
        sizeof(image.modules[0]), 2, offsetof(image_t, modules));
    directory_entry(&image, 11, N00B_ABI_TABLE_SCOPE,
        sizeof(image.scopes[0]), 2, offsetof(image_t, scopes));
    directory_entry(&image, 12, N00B_ABI_TABLE_SOURCE_FILE,
        sizeof(image.source_files[0]), 2, offsetof(image_t, source_files));
    directory_entry(&image, 13, N00B_ABI_TABLE_RUNTIME_CONFIG,
        sizeof(image.configs[0]), 2, offsetof(image_t, configs));
    directory_entry(&image, 14, N00B_ABI_TABLE_STORAGE,
        sizeof(image.storages[0]), 3, offsetof(image_t, storages));
    directory_entry(&image, 15, N00B_ABI_TABLE_TLS_RELOCATION,
        sizeof(image.tls_relocations[0]), 2,
        offsetof(image_t, tls_relocations));

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
    image.packages[0].parent_package_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_PACKAGE
        && !n00b_abi_v1_current_snapshot());
    image.packages[0].parent_package_index = 0;
    image.methods[1].trait_type_index = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && !n00b_abi_v1_current_snapshot());
    image.methods[1].trait_type_index = 1;
    image.functions[1].module_index = 99;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FUNCTION
        && error.record_index == 1 && !n00b_abi_v1_current_snapshot());
    image.functions[1].module_index = 1;
    image.types[1].fields_or_arms.first = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && !n00b_abi_v1_current_snapshot());
    image.types[1].fields_or_arms.first = 2;
    image.modules[1].imports.first = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.modules[1].imports.first = 0;
    image.links[1] = 3;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && !n00b_abi_v1_current_snapshot());
    image.links[1] = 4;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && !n00b_abi_v1_current_snapshot());
    image.links[1] = 2;
    image.types[4].specialization_start_ix = 0;
    image.types[4].specialization_end_ix = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA && !n00b_abi_v1_current_snapshot());
    image.types[4].specialization_start_ix = 2;
    image.types[4].specialization_end_ix = 3;
    image.tls_relocations[1].target_kind = N00B_ABI_STORAGE_WRITABLE;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TLS_RELOCATION
        && !n00b_abi_v1_current_snapshot());
    image.tls_relocations[1].target_kind = N00B_ABI_STORAGE_READ_ONLY;
    image.tls_relocations[1].template_storage_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TLS_RELOCATION
        && !n00b_abi_v1_current_snapshot());
    image.tls_relocations[1].template_storage_index = 2;
    image.tls_relocations[1].write_byte_offset = 9;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TLS_RELOCATION
        && !n00b_abi_v1_current_snapshot());
    image.tls_relocations[1].write_byte_offset = 8;
    image.modules[1].static_storage.first = 6;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.modules[1].static_storage.first = 5;
    image.modules[1].tls_storage = (n00b_abi_range_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_STORAGE
        && !n00b_abi_v1_current_snapshot());
    image.modules[1].tls_storage = (n00b_abi_range_v1){6, 1};
    image.storages[1].byte_offset = UINT64_MAX;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_STORAGE
        && !n00b_abi_v1_current_snapshot());
    image.storages[1].byte_offset = 0;
    image.dispatches[1].method_ordinal = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_DISPATCH
        && !n00b_abi_v1_current_snapshot());
    image.dispatches[1].method_ordinal = 0;
    image.entries[2].receiver_type_index = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_DISPATCH_ENTRY
        && !n00b_abi_v1_current_snapshot());
    image.entries[2].receiver_type_index = 3;
    image.dispatches[1].entries.count = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_DISPATCH_ENTRY
        && !n00b_abi_v1_current_snapshot());
    image.dispatches[1].entries.count = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && !error.status && generation == 1);
    const n00b_abi_snapshot_v1 *snapshot =
        n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_generation(snapshot) == 1
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE) == 5
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_MODULE) == 2);
    uint32_t width = 0;
    const n00b_abi_type_v1 *nil = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TYPE, 0, &width);
    assert(nil == n00b_abi_v1_type(snapshot, 0)
        && width == sizeof(*nil) && nil->kind == N00B_ABI_TYPE_NIL
        && !n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TYPE, 5, &width)
        && !width && n00b_abi_v1_module(snapshot, 1)
        && !n00b_abi_v1_module(snapshot, 2));
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
    uint32_t result = 0;
    assert(n00b_abi_v1_find_typehash(snapshot, 13, &result) == 1
        && result == 3);
    assert(n00b_abi_v1_find_typehash(snapshot, 99, &result) == 0);
    assert(n00b_abi_v1_type_specializes(snapshot, 4, 2) == 1);
    assert(n00b_abi_v1_type_specializes(snapshot, 4, 4) == 0);
    assert(n00b_abi_v1_type_specializes(snapshot, 2, 3) == 0);
    assert(n00b_abi_v1_type_specializes(snapshot, 5, 2) == -1);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 4,
        &result) == 1 && result == 2);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 2,
        &result) == 1 && result == 1);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 0,
        &result) == 0);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 1, 4,
        &result) == -1);
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_UNRESOLVED
        && n00b_abi_v1_current_snapshot() == snapshot
        && n00b_abi_v1_generation(snapshot) == 1);
    return 0;
}
