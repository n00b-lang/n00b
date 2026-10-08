#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abi_directory.h"
#include "n00b-abi-v1.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define XXH_INLINE_ALL
#define XXH_FORCE_MEMORY_ACCESS 3
#include <xxhash.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
    uint8_t directory[22 * N00BB_ABI_DIRECTORY_BYTES];
    n00b_abi_type_v1 types[6];
    uint32_t indexes[16];
    uint64_t values[1];
    uint8_t bytes[4];
    n00b_abi_specialization_link_v1 links[3];
    n00b_abi_trait_method_v1 methods[3];
    n00b_abi_function_v1 functions[4];
    n00b_abi_call_signature_v1 signatures[2];
    n00b_abi_code_range_v1 code_ranges[3];
    n00b_abi_frame_layout_v1 frame_layouts[2];
    n00b_abi_dispatch_v1 dispatches[2];
    n00b_abi_dispatch_entry_v1 entries[4];
    n00b_abi_package_v1 packages[2];
    n00b_abi_module_v1 modules[3];
    n00b_abi_scope_v1 scopes[2];
    n00b_abi_symbol_v1 symbols[2];
    n00b_abi_name_slot_v1 name_slots[4];
    n00b_abi_source_file_v1 source_files[2];
    n00b_abi_source_location_v1 locations[2];
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
    image.header.directory_count = 22;
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
    image.types[4].kind = N00B_ABI_TYPE_RECORD;
    image.types[4].typehash = 14;
    image.types[4].specialization_start_ix = 1;
    image.types[4].specialization_end_ix = 3;
    image.types[5].kind = N00B_ABI_TYPE_FUNCTION;
    image.types[5].typehash = 15;
    image.types[5].call_signature_index = 1;
    image.signatures[1].type_index = 5;
    image.locations[1].file_index = 1;
    image.indexes[0] = 3;
    image.indexes[1] = 5;
    image.indexes[2] = 1;
    image.indexes[3] = 1;
    image.indexes[4] = 2;
    image.indexes[5] = 1;
    image.indexes[6] = 2;
    image.indexes[7] = 1;
    image.indexes[8] = 1;
    image.indexes[9] = 2;
    image.indexes[10] = 3;
    image.indexes[11] = 1;
    image.indexes[12] = 2;
    image.indexes[13] = 1;
    image.indexes[14] = 1;
    image.indexes[15] = 1;
    image.values[0] = 123;
    memcpy(image.bytes, "abc", 3);
    image.bytes[3] = 1;
    image.links[1] = 2;
    image.links[2] = 3;
    image.methods[1].trait_type_index = 1;
    image.methods[1].signature_type_index = 4;
    image.packages[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    image.packages[1].modules = (n00b_abi_range_v1){14, 1};
    image.functions[1].module_index = 1;
    image.functions[1].scope_index = 1;
    image.functions[1].code_ranges = (n00b_abi_range_v1){11, 1};
    image.functions[1].frame_layouts = (n00b_abi_range_v1){13, 1};
    image.functions[2].module_index = 1;
    image.functions[2].scope_index = 1;
    image.functions[2].code_ranges = (n00b_abi_range_v1){12, 1};
    image.functions[3].module_index = 1;
    image.functions[3].scope_index = 1;
    image.code_ranges[1] = (n00b_abi_code_range_v1){
        .function_index = 1,
        .phase_mask = N00B_ABI_PHASE_RUNTIME,
        .byte_length = 1,
        .start = (const void *)(uintptr_t)main,
    };
    image.code_ranges[2] = (n00b_abi_code_range_v1){
        .function_index = 2,
        .phase_mask = N00B_ABI_PHASE_RUNTIME,
        .byte_length = 1,
        .start = (const void *)(uintptr_t)main,
    };
    image.frame_layouts[1] = (n00b_abi_frame_layout_v1){
        .function_index = 1,
        .pointer_bitmap_bytes = {3, 1},
        .frame_byte_extent = 16,
    };
    image.modules[1].package_index = 1;
    memcpy(image.modules[1].full_path.short_value.bytes, "p.m", 3);
    image.modules[1].full_path.short_value.selector = 3;
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
    image.symbols[1].kind = N00B_ABI_SYMBOL_VARIABLE;
    image.symbols[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    image.symbols[1].module_index = 1;
    image.symbols[1].scope_index = 1;
    image.symbols[1].type_index = 2;
    image.symbols[1].declaration_location_index = 1;
    memcpy(image.symbols[1].name.short_value.bytes, "foo", 3);
    image.symbols[1].name.short_value.selector = 3;
    uint64_t name_hash = XXH3_64bits("p.m.foo", 7);
    uint32_t name_slot_index = 1 + (uint32_t)(name_hash % 3);
    image.name_slots[name_slot_index].name_hash = name_hash;
    image.name_slots[name_slot_index].symbol_index = 1;
    image.dispatches[1].trait_type_index = 1;
    image.dispatches[1].entries = (n00b_abi_range_v1){8, 3};
    image.entries[1].receiver_type_index = 2;
    image.entries[1].function_index = 1;
    image.entries[2].receiver_type_index = 3;
    image.entries[2].function_index = 2;
    image.entries[3].receiver_type_index = 4;
    image.entries[3].function_index = 3;
    directory_entry(&image, 0, N00B_ABI_TABLE_TYPE,
        sizeof(image.types[0]), 6, offsetof(image_t, types));
    directory_entry(&image, 1, N00B_ABI_TABLE_U32_STREAM,
        sizeof(image.indexes[0]), 16, offsetof(image_t, indexes));
    directory_entry(&image, 2, N00B_ABI_TABLE_U64_STREAM,
        sizeof(image.values[0]), 1, offsetof(image_t, values));
    directory_entry(&image, 3, N00B_ABI_TABLE_BYTE_STREAM,
        sizeof(image.bytes[0]), 4, offsetof(image_t, bytes));
    directory_entry(&image, 4, N00B_ABI_TABLE_SPECIALIZATION_LINK,
        sizeof(image.links[0]), 3, offsetof(image_t, links));
    directory_entry(&image, 5, N00B_ABI_TABLE_TRAIT_METHOD,
        sizeof(image.methods[0]), 2, offsetof(image_t, methods));
    directory_entry(&image, 6, N00B_ABI_TABLE_FUNCTION,
        sizeof(image.functions[0]), 4, offsetof(image_t, functions));
    directory_entry(&image, 7, N00B_ABI_TABLE_DISPATCH,
        sizeof(image.dispatches[0]), 2, offsetof(image_t, dispatches));
    directory_entry(&image, 8, N00B_ABI_TABLE_DISPATCH_ENTRY,
        sizeof(image.entries[0]), 4, offsetof(image_t, entries));
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
    directory_entry(&image, 16, N00B_ABI_TABLE_SYMBOL,
        sizeof(image.symbols[0]), 2, offsetof(image_t, symbols));
    directory_entry(&image, 17, N00B_ABI_TABLE_NAME_SLOT,
        sizeof(image.name_slots[0]), 4, offsetof(image_t, name_slots));
    directory_entry(&image, 18, N00B_ABI_TABLE_CODE_RANGE,
        sizeof(image.code_ranges[0]), 3, offsetof(image_t, code_ranges));
    directory_entry(&image, 19, N00B_ABI_TABLE_FRAME_LAYOUT,
        sizeof(image.frame_layouts[0]), 2, offsetof(image_t, frame_layouts));
    directory_entry(&image, 20, N00B_ABI_TABLE_CALL_SIGNATURE,
        sizeof(image.signatures[0]), 2, offsetof(image_t, signatures));
    directory_entry(&image, 21, N00B_ABI_TABLE_SOURCE_LOCATION,
        sizeof(image.locations[0]), 2, offsetof(image_t, locations));

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
    image.methods[2] = image.methods[1];
    directory_entry(&image, 5, N00B_ABI_TABLE_TRAIT_METHOD,
        sizeof(image.methods[0]), 3, offsetof(image_t, methods));
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TRAIT_METHOD
        && error.record_index == 2 && !n00b_abi_v1_current_snapshot());
    directory_entry(&image, 5, N00B_ABI_TABLE_TRAIT_METHOD,
        sizeof(image.methods[0]), 2, offsetof(image_t, methods));
    image.types[1].fields_or_arms.count = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && !n00b_abi_v1_current_snapshot());
    image.types[1].fields_or_arms.count = 1;
    image.types[3].typevar_ordinal = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 3 && !n00b_abi_v1_current_snapshot());
    image.types[3].typevar_ordinal = 0;
    image.types[3].kind = N00B_ABI_TYPE_TUPLE + 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 3 && !n00b_abi_v1_current_snapshot());
    image.types[3].kind = N00B_ABI_TYPE_RECORD;
    image.types[3].flags = UINT32_MAX;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 3 && !n00b_abi_v1_current_snapshot());
    image.types[3].flags = 0;
    n00b_abi_type_v1 original_type = image.types[3];
    image.types[3] = (n00b_abi_type_v1){
        .kind = N00B_ABI_TYPE_VARIABLE,
        .flags = N00B_ABI_TYPE_FLAG_NON_CONCRETE,
        .typevar_owner_scope_index = 1,
    };
    image.types[3].semantic_width_bits = 8;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 3 && !n00b_abi_v1_current_snapshot());
    image.types[3] = original_type;
    image.signatures[1].type_index = 4;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 5 && !n00b_abi_v1_current_snapshot());
    image.signatures[1].type_index = 5;
    image.types[5].call_signature_index = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CALL_SIGNATURE
        && error.record_index == 1 && !n00b_abi_v1_current_snapshot());
    image.types[5].call_signature_index = 1;
    image.methods[1].symbol_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && !n00b_abi_v1_current_snapshot());
    image.symbols[1].kind = N00B_ABI_SYMBOL_FUNCTION;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && !n00b_abi_v1_current_snapshot());
    image.symbols[1].kind = N00B_ABI_SYMBOL_VARIABLE;
    image.symbols[1].kind = UINT32_MAX;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_SYMBOL
        && error.record_index == 1 && !n00b_abi_v1_current_snapshot());
    image.symbols[1].kind = N00B_ABI_SYMBOL_VARIABLE;
    image.methods[1].symbol_index = 0;
    image.functions[1].module_index = 99;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FUNCTION
        && error.record_index == 1 && !n00b_abi_v1_current_snapshot());
    image.functions[1].module_index = 1;
    image.code_ranges[1].byte_length = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.code_ranges[1].byte_length = 1;
    image.code_ranges[1].phase_mask = 0;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.code_ranges[1].phase_mask = N00B_ABI_PHASE_RUNTIME;
    image.functions[1].symbol_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.symbols[1].kind = N00B_ABI_SYMBOL_FUNCTION;
    image.symbols[1].phase_mask = N00B_ABI_PHASE_COMPTIME;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.symbols[1].kind = N00B_ABI_SYMBOL_VARIABLE;
    image.symbols[1].phase_mask = 0;
    image.functions[1].symbol_index = 0;
    image.code_ranges[2].function_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.code_ranges[2].function_index = 2;
    image.functions[2].code_ranges.first = 11;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.functions[2].code_ranges.first = 12;
    image.functions[2].code_ranges = (n00b_abi_range_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.functions[2].code_ranges = (n00b_abi_range_v1){12, 1};
    image.code_ranges[1].start = (const void *)(uintptr_t)UINTPTR_MAX;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_CODE_RANGE
        && !n00b_abi_v1_current_snapshot());
    image.code_ranges[1].start = (const void *)(uintptr_t)main;
    image.frame_layouts[1].frame_byte_extent = 15;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.frame_layouts[1].frame_byte_extent = 80;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.frame_layouts[1].frame_byte_extent = 16;
    image.bytes[3] = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.bytes[3] = 1;
    image.frame_layouts[1].function_index = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.frame_layouts[1].function_index = 1;
    image.functions[2].frame_layouts = (n00b_abi_range_v1){13, 1};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.functions[2].frame_layouts = (n00b_abi_range_v1){0};
    image.functions[1].frame_layouts = (n00b_abi_range_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_FRAME_LAYOUT
        && !n00b_abi_v1_current_snapshot());
    image.functions[1].frame_layouts = (n00b_abi_range_v1){13, 1};
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
    image.modules[2] = image.modules[1];
    image.modules[2].static_storage = (n00b_abi_range_v1){0};
    image.modules[2].tls_storage = (n00b_abi_range_v1){0};
    directory_entry(&image, 10, N00B_ABI_TABLE_MODULE,
        sizeof(image.modules[0]), 3, offsetof(image_t, modules));
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && error.record_index == 2 && !n00b_abi_v1_current_snapshot());
    directory_entry(&image, 10, N00B_ABI_TABLE_MODULE,
        sizeof(image.modules[0]), 2, offsetof(image_t, modules));
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
    image.types[4].specialization_start_ix = 1;
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
    directory_entry(&image, 8, N00B_ABI_TABLE_DISPATCH_ENTRY,
        sizeof(image.entries[0]), 3, offsetof(image_t, entries));
    image.dispatches[1].entries.count = 2;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_DISPATCH
        && !n00b_abi_v1_current_snapshot());
    directory_entry(&image, 8, N00B_ABI_TABLE_DISPATCH_ENTRY,
        sizeof(image.entries[0]), 4, offsetof(image_t, entries));
    image.dispatches[1].entries.count = 3;
    image.name_slots[name_slot_index].name_hash ^= 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_NAME_SLOT
        && !n00b_abi_v1_current_snapshot());
    image.name_slots[name_slot_index].name_hash = name_hash;
    uint32_t displaced_slot = name_slot_index == 3 ? 1 : name_slot_index + 1;
    image.name_slots[displaced_slot] = image.name_slots[name_slot_index];
    image.name_slots[name_slot_index] = (n00b_abi_name_slot_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_NAME_SLOT
        && !n00b_abi_v1_current_snapshot());
    image.name_slots[name_slot_index] = image.name_slots[displaced_slot];
    image.name_slots[displaced_slot] = (n00b_abi_name_slot_v1){0};
    image.name_slots[name_slot_index] = (n00b_abi_name_slot_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_SYMBOL
        && !n00b_abi_v1_current_snapshot());
    image.name_slots[name_slot_index] = (n00b_abi_name_slot_v1){
        .name_hash = name_hash, .symbol_index = 1,
    };
    image.types[4].typehash = image.types[3].typehash;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_TYPE
        && error.record_index == 4 && !n00b_abi_v1_current_snapshot());
    image.types[4].typehash = 14;
    image.packages[1].parent_package_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_PACKAGE
        && !n00b_abi_v1_current_snapshot());
    image.packages[1].parent_package_index = 0;
    image.packages[1].modules = (n00b_abi_range_v1){0};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.packages[1].modules = (n00b_abi_range_v1){14, 2};
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_PACKAGE
        && !n00b_abi_v1_current_snapshot());
    image.packages[1].modules.count = 1;
    image.modules[1].runtime_initializer = 1;
    image.code_ranges[1].phase_mask = N00B_ABI_PHASE_COMPTIME;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.code_ranges[1].phase_mask = N00B_ABI_PHASE_RUNTIME;
    image.functions[1].failure_type_index = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.functions[1].failure_type_index = 0;
    image.modules[1].comptime_initializer = 1;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_BAD_METADATA
        && error.table_kind == N00B_ABI_TABLE_MODULE
        && !n00b_abi_v1_current_snapshot());
    image.modules[1].comptime_initializer = 0;
    image.types[5].typehash = image.types[1].typehash + 16;
    assert(n00b_abi_v1_register_image(&descriptor, &generation, &error)
        == N00B_ABI_OK && !error.status && generation == 1);
    const n00b_abi_snapshot_v1 *snapshot =
        n00b_abi_v1_current_snapshot();
    assert(snapshot && n00b_abi_v1_generation(snapshot) == 1
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_TYPE) == 6
        && n00b_abi_v1_table_count(snapshot, N00B_ABI_TABLE_MODULE) == 2);
    uint32_t width = 0;
    const n00b_abi_type_v1 *nil = n00b_abi_v1_record(snapshot,
        N00B_ABI_TABLE_TYPE, 0, &width);
    assert(nil == n00b_abi_v1_type(snapshot, 0)
        && width == sizeof(*nil) && nil->kind == N00B_ABI_TYPE_NIL
        && !n00b_abi_v1_record(snapshot, N00B_ABI_TABLE_TYPE, 6, &width)
        && !width && n00b_abi_v1_module(snapshot, 1)
        && !n00b_abi_v1_module(snapshot, 2));
    assert(n00b_abi_v1_package(snapshot, 1)
        && n00b_abi_v1_runtime_config(snapshot, 1)
        && n00b_abi_v1_function(snapshot, 1)->module_index == 1
        && n00b_abi_v1_call_signature(snapshot, 1)->type_index == 5
        && n00b_abi_v1_scope(snapshot, 1)->module_index == 1
        && n00b_abi_v1_symbol(snapshot, 1)->type_index == 2
        && n00b_abi_v1_source_location(snapshot, 1)->file_index == 1
        && n00b_abi_v1_code_range(snapshot, 1)->function_index == 1
        && n00b_abi_v1_frame_layout(snapshot, 1)->function_index == 1
        && n00b_abi_v1_storage(snapshot, 1)->module_index == 1
        && !n00b_abi_v1_call_signature(snapshot, 2)
        && !n00b_abi_v1_source_location(snapshot, 2));
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
    assert(n00b_abi_v1_find_typehash(snapshot,
        image.types[5].typehash, &result) == 1 && result == 5);
    assert(n00b_abi_v1_find_typehash(snapshot, 99, &result) == 0);
    n00b_abi_string_v1 module_path = {0};
    memcpy(module_path.short_value.bytes, "p.m", 3);
    module_path.short_value.selector = 3;
    assert(n00b_abi_v1_find_module(snapshot, module_path, &result) == 1
        && result == 1);
    module_path.short_value.bytes[2] = 'x';
    result = 42;
    assert(n00b_abi_v1_find_module(snapshot, module_path, &result) == 0
        && result == 42);
    module_path.words[1] = UINT64_C(3) << N00B_ABI_STRING_TAG_SHIFT;
    assert(n00b_abi_v1_find_module(snapshot, module_path, &result) == -1);
    assert(n00b_abi_v1_find_module(snapshot, (n00b_abi_string_v1){0},
        nullptr) == -1);
    n00b_abi_string_v1 symbol_path = {0};
    memcpy(symbol_path.short_value.bytes, "p.m.foo", 7);
    symbol_path.short_value.selector = 7;
    assert(n00b_abi_v1_find_symbol(snapshot, symbol_path, &result) == 1
        && result == 1);
    symbol_path.short_value.bytes[6] = 'x';
    result = 42;
    assert(n00b_abi_v1_find_symbol(snapshot, symbol_path, &result) == 0
        && result == 42);
    assert(n00b_abi_v1_type_specializes(snapshot, 4, 2) == 1);
    assert(n00b_abi_v1_type_specializes(snapshot, 4, 4) == 0);
    assert(n00b_abi_v1_type_specializes(snapshot, 2, 3) == 0);
    assert(n00b_abi_v1_type_specializes(snapshot, 6, 2) == -1);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 4,
        &result) == 1 && result == 3);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 2,
        &result) == 1 && result == 1);
    assert(n00b_abi_v1_resolve_trait_method(snapshot, 1, 0, 3,
        &result) == 1 && result == 2);
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
