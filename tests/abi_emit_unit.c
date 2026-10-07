#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "abi_container.h"
#include "abi_emit.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"
#include "ir/ir.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/module.h"
#include "lib/object_section.h"

static const n00bb_abi_directory_entry_t *
find_entry(const n00bb_abi_directory_entry_t *entries, size_t count,
           uint32_t kind)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].kind == kind) return &entries[i];
    return nullptr;
}

static void
check_empty_unit(const char *triple, uint32_t target_id,
                 const char *metadata_segment, const char *metadata_name,
                 const char *pool_segment, const char *pool_name)
{
    static const char module_name[] = "default.long_identity_module_name";
    static const char source_path[] = "/tmp/project/long_identity_module_name.n00b";
    static const char source_text[] = "# empty\n";
    core_target_desc_t target;
    assert(core_target_profile(triple, 0, &target));
    core_module_spec_t spec = {0};
    assert(core_module_spec_init(&spec, CORE_STRING_STATIC(module_name)));
    core_source_file_t source = {0};
    assert(core_source_file_init(&source, CORE_STRING_STATIC(source_path),
                                 source_text, sizeof(source_text) - 1));
    core_translation_unit_t unit = {0};
    assert(core_translation_unit_init(&unit, &spec, &source));
    core_module_spec_release(&spec);
    assert(core_translation_unit_finalize_imports(&unit));

    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "n00b"};
    ir_module_t *runtime = ir_module_new(context, module_name,
                                        &profile, target.triple);
    ir_module_t *comptime = ir_module_new(context,
        "default.long_identity_module_name$comptime", &profile, target.triple);
    assert(runtime && comptime);
    parse_symtab_t *symbols = parse_symtab_new();
    assert(symbols);
    n00bb_abi_packed_tables_t packed = {0};
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    assert(n00bb_abi_emit_empty_unit(&unit, CORE_STRING_STATIC("default"),
        symbols, comptime, runtime,
        &target, N00B_ABI_CAP_DEBUG,
        &packed, &object, &object_bytes));
    assert(!core_translation_unit_has_fatal_diagnostics(&unit));

    core_object_section_t metadata;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, metadata_segment, metadata_name, &metadata));
    n00bb_abi_image_key_t key = {
        .target_id = target_id,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t entries[8];
    size_t entry_count = sizeof(entries) / sizeof(*entries);
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size,
        &key, schemas, schema_count, false, &header, entries, entry_count));
    assert(header.directory_count == entry_count);
    const n00bb_abi_directory_entry_t *package_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PACKAGE);
    const n00bb_abi_directory_entry_t *module_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *source_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_SOURCE_FILE);
    const n00bb_abi_directory_entry_t *type_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layout_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *run_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_PHYSICAL_RUN);
    const n00bb_abi_directory_entry_t *config_entry = find_entry(
        entries, entry_count, N00B_ABI_TABLE_RUNTIME_CONFIG);
    assert(package_entry && module_entry && source_entry && type_entry
        && layout_entry && run_entry && config_entry);
    assert(layout_entry->record_count == 1 && run_entry->record_count == 1);
    const n00b_abi_package_v1 *package = (const void *)(metadata.bytes
        + package_entry->file_offset + package_entry->record_bytes);
    const n00b_abi_module_v1 *module = (const void *)(metadata.bytes
        + module_entry->file_offset + module_entry->record_bytes);
    const n00b_abi_source_file_v1 *file = (const void *)(metadata.bytes
        + source_entry->file_offset + source_entry->record_bytes);
    const n00b_abi_type_v1 *nil = (const void *)(metadata.bytes
        + type_entry->file_offset);
    n00bb_abi_string_header_t decoded;
    assert(type_entry->record_count == 1);
    assert(nil->kind == N00B_ABI_TYPE_NIL && nil->size_bits == 0
        && nil->alignment_bytes == 1
        && nil->typehash == th_hash(ir_ctx_types(context),
                                   th_name(ir_ctx_types(context), "nil")));
    assert(n00bb_abi_string_decode((const uint8_t *)&nil->canonical_name,
        sizeof(nil->canonical_name), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_SHORT && decoded.length == 3);
    assert(!memcmp(nil->canonical_name.short_value.bytes, "nil", 3));
    assert(package->modules.first == 0 && package->modules.count == 1);
    assert(module->package_index == 1 && module->source_file_index == 1
        && module->runtime_config_index == 1 && module->scc_index == 0);
    assert(file->source_identity == core_xxh3_64bits_raw(
        source_text, sizeof(source_text) - 1));
    assert(n00bb_abi_string_decode((const uint8_t *)&package->name,
        sizeof(package->name), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_SHORT && decoded.length == 7);
    assert(n00bb_abi_string_decode((const uint8_t *)&module->full_path,
        sizeof(module->full_path), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_LONG
        && decoded.length == sizeof(module_name) - 1);
    assert(n00bb_abi_string_decode((const uint8_t *)&file->path,
        sizeof(file->path), &decoded));
    assert(decoded.tag == N00BB_ABI_STRING_LONG
        && decoded.length == sizeof(source_path) - 1);
    assert(metadata.relocation_count == 2);

    core_object_section_t pool;
    assert(core_object_section_find(object, object_bytes,
        target.object_format, pool_segment, pool_name, &pool));
    assert(pool.size == sizeof(module_name) - 1 + sizeof(source_path) - 1);
    assert(!memcmp(pool.bytes, module_name, sizeof(module_name) - 1));
    assert(!memcmp(pool.bytes + sizeof(module_name) - 1,
                   source_path, sizeof(source_path) - 1));
    uint64_t module_pointer = module_entry->file_offset
        + module_entry->record_bytes + offsetof(n00b_abi_module_v1, full_path);
    uint64_t source_pointer = source_entry->file_offset
        + source_entry->record_bytes + offsetof(n00b_abi_source_file_v1, path);
    bool found_module = false, found_source = false;
    for (uint32_t i = 0; i < metadata.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        assert(core_object_absolute64_relocation_at(&metadata,
            target.object_format, i, &relocation));
        found_module |= relocation.section_offset == module_pointer;
        found_source |= relocation.section_offset == source_pointer;
    }
    assert(found_module && found_source);

    parse_tree_t declaration_node = {0};
    assert(parse_symtab_add(symbols, CORE_STRING_STATIC("value"),
        CORE_STRING_STATIC("declared"), PARSE_SYM_VARIABLE,
        &declaration_node));
    n00bb_abi_packed_tables_t rejected = {0};
    uint8_t *rejected_object = nullptr;
    size_t rejected_bytes = 0;
    assert(!n00bb_abi_emit_empty_unit(&unit, CORE_STRING_STATIC("default"),
        symbols, comptime, runtime, &target, N00B_ABI_CAP_DEBUG,
        &rejected, &rejected_object, &rejected_bytes));
    assert(!rejected.bytes && !rejected_object && !rejected_bytes);
    assert(core_translation_unit_has_fatal_diagnostics(&unit));

    n00bb_abi_packed_tables_release(&packed);
    core_free(object);
    parse_symtab_free(symbols);
    ir_ctx_free(context);
    core_translation_unit_release(&unit);
}

int
main(void)
{
    check_empty_unit("x86_64-linux", N00BB_ABI_TARGET_X86_64_LINUX,
        nullptr, ".data.rel.ro.n00b_meta", nullptr,
        ".rodata.n00b_strings");
    check_empty_unit("arm64-macos", N00BB_ABI_TARGET_ARM64_MACOS,
        "__DATA_CONST", "__n00b_meta", "__TEXT", "__n00b_strings");
    check_empty_unit("x86_64-windows", N00BB_ABI_TARGET_X86_64_WINDOWS,
        nullptr, ".n00b$M", nullptr, ".rdata$S");
    return 0;
}
