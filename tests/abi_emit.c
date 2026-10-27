#include <assert.h>
#include <limits.h>
#include <string.h>

#include "abi_container.h"
#include "abi_emit.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/object_section.h"

#define S(x) CORE_STRING_STATIC(x)

static void
check_target(core_string_t triple, uint32_t target_id,
             core_string_t metadata_name, core_string_t metadata_segment,
             core_string_t data_name, core_string_t data_segment)
{
    core_target_desc_t target;
    assert(core_target_profile(triple, 0, &target));
    n00b_abi_package_v1 package = {0};
    memcpy(package.name.short_value.bytes, "demo", 4);
    package.name.short_value.selector =
        4 | (N00B_ABI_STRING_ASCII << 4);
    n00b_abi_type_v1 nil = {.kind = N00B_ABI_TYPE_NIL};
    n00bb_abi_table_rows_t tables[] = {
        {N00B_ABI_TABLE_PACKAGE, &package, 1},
        {N00B_ABI_TABLE_TYPE, &nil, 1},
    };
    n00bb_abi_packed_tables_t packed = {0};
    assert(n00bb_abi_pack_tables(&target, N00B_ABI_CAP_DEBUG,
                                  tables, 2, &packed));
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_image_key_t key = {
        .target_id = target_id,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    n00bb_abi_directory_entry_t entries[2];
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(packed.bytes, packed.byte_count,
        &key, schemas, schema_count, false, &header, entries, 2));
    core_target_desc_t other_target;
    assert(core_target_profile(
        target_id == N00BB_ABI_TARGET_X86_64_LINUX
            ? S("arm64-macos") : S("x86_64-linux"), 0, &other_target));
    uint8_t *wrong_object = nullptr;
    size_t wrong_bytes = 0;
    assert(!n00bb_abi_write_object(&other_target, &packed,
        nullptr, 0, nullptr, 0, nullptr, 0,
        &wrong_object, &wrong_bytes));
    assert(!wrong_object && !wrong_bytes);
    assert(header.artifact_capabilities == N00B_ABI_CAP_DEBUG);
    assert(header.required_alignment == packed.alignment_bytes);
    assert(entries[0].record_count == 1
        && entries[0].kind == N00B_ABI_TABLE_PACKAGE);
    assert(entries[1].record_count == 1
        && entries[1].kind == N00B_ABI_TABLE_TYPE);
    assert(!memcmp(packed.bytes + entries[0].file_offset,
                   &package, sizeof(package)));
    assert(!memcmp(packed.bytes + entries[1].file_offset,
                   &nil, sizeof(nil)));

    uint8_t data[8] = {0};
    core_object_emit_section_t extra = {
        .name = data_name, .segment = data_segment,
        .bytes = data, .size = sizeof(data),
        .alignment = _Alignof(uint64_t), .writable = true,
    };
    core_object_emit_symbol_t external = {
        .name = S("external"), .section_index = 0, .exported = true,
    };
    core_object_emit_absolute64_t relocation = {
        .section_index = 2, .symbol_index = 0,
    };
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    assert(n00bb_abi_write_object(&target, &packed, &extra, 1,
        &external, 1, &relocation, 1, &object, &object_bytes));
    core_object_section_t section;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, metadata_segment, metadata_name, &section));
    assert(section.size == packed.byte_count);
    assert(section.alignment == packed.alignment_bytes);
    assert(!memcmp(section.bytes, packed.bytes, packed.byte_count));
    assert(core_object_section_find(object, object_bytes,
        target.object_format, data_segment, data_name, &section));
    assert(section.relocation_count == 1);
    core_object_absolute64_relocation_t native_relocation;
    assert(core_object_absolute64_relocation_at(&section,
        target.object_format, 0, &native_relocation));
    assert(native_relocation.section_offset == 0);
    core_free(object);

    n00bb_abi_table_rows_t duplicate[] = {tables[0], tables[0]};
    n00bb_abi_packed_tables_t rejected = {0};
    assert(!n00bb_abi_pack_tables(&target, 0, duplicate, 2, &rejected));
    assert(!rejected.bytes);
    assert(!n00bb_abi_pack_tables(&target, UINT64_MAX,
                                  tables, 2, &rejected));
    assert(!rejected.bytes);
    n00bb_abi_table_rows_t too_many = {
        N00B_ABI_TABLE_TYPE, &nil, SIZE_MAX,
    };
    assert(!n00bb_abi_pack_tables(&target, 0, &too_many, 1, &rejected));
    assert(!rejected.bytes);
    n00bb_abi_packed_tables_release(&packed);
}

int
main(void)
{
    check_target(S("x86_64-linux"), N00BB_ABI_TARGET_X86_64_LINUX,
                 S(".data.rel.ro.n00b_meta"), (core_string_t){}, S(".data"),
                 (core_string_t){});
    check_target(S("arm64-macos"), N00BB_ABI_TARGET_ARM64_MACOS,
                 S("__n00b_meta"), S("__DATA_CONST"), S("__data"),
                 S("__DATA"));
    check_target(S("x86_64-windows"), N00BB_ABI_TARGET_X86_64_WINDOWS,
                 S(".n00b$M"), (core_string_t){}, S(".data"),
                 (core_string_t){});
    check_target(S("arm64-linux"), N00BB_ABI_TARGET_ARM64_LINUX,
                 S(".data.rel.ro.n00b_meta"), (core_string_t){}, S(".data"),
                 (core_string_t){});
    return 0;
}
