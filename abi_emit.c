#include "abi_emit.h"

#include <limits.h>
#include <string.h>

#include "abi_container.h"
#include "abi_emit_strings.h"
#include "abi_type_index.h"
#include "abi_type_layout_rows.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/diagnostic.h"
#include "lib/hash.h"
#include "lib/object_section.h"

static bool
align_up(size_t value, size_t alignment, size_t *out)
{
    if (!alignment || (alignment & (alignment - 1))
        || value > SIZE_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static bool
image_key(const core_target_desc_t *target, n00bb_abi_image_key_t *out)
{
    if (!target || !out || !core_target_validate(target)
        || target->endian != CORE_TARGET_LITTLE_ENDIAN
        || target->pointer_bits != 64
        || (target->enabled_features
            & ~CORE_TARGET_FEATURE_NATIVE_ATOMIC128)) return false;
    uint32_t id;
    if (target->object_format == CORE_TARGET_OBJECT_ELF
        && target->call_abi == CORE_TARGET_CALL_SYSV_AMD64)
        id = N00BB_ABI_TARGET_X86_64_LINUX;
    else if (target->object_format == CORE_TARGET_OBJECT_MACH_O
             && target->call_abi == CORE_TARGET_CALL_DARWIN_AARCH64)
        id = N00BB_ABI_TARGET_ARM64_MACOS;
    else if (target->object_format == CORE_TARGET_OBJECT_COFF
             && target->call_abi == CORE_TARGET_CALL_WINDOWS_X64)
        id = N00BB_ABI_TARGET_X86_64_WINDOWS;
    else return false;
    *out = (n00bb_abi_image_key_t){
        .target_id = id,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .cpu_profile = target->enabled_features
            & CORE_TARGET_FEATURE_NATIVE_ATOMIC128,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    return true;
}

bool
n00bb_abi_pack_tables(const core_target_desc_t *target,
                      uint64_t artifact_capabilities,
                      const n00bb_abi_table_rows_t *tables,
                      size_t table_count, n00bb_abi_packed_tables_t *out)
{
    n00bb_abi_image_key_t key;
    uint16_t one = 1;
    if (!out || (table_count && !tables)
        || table_count > UINT32_MAX
        || table_count > (SIZE_MAX - N00BB_ABI_HEADER_BYTES)
                         / N00BB_ABI_DIRECTORY_BYTES
        || *(const uint8_t *)&one != 1
        || !image_key(target, &key)) return false;

    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, table_count);
    size_t cursor = N00BB_ABI_HEADER_BYTES
        + table_count * N00BB_ABI_DIRECTORY_BYTES;
    uint32_t alignment = _Alignof(n00b_abi_header_v1);
    bool valid = true;
    for (size_t i = 0; i < table_count; i++) {
        const n00bb_abi_table_schema_t *schema = nullptr;
        for (size_t j = 0; j < schema_count; j++)
            if (schemas[j].kind == tables[i].kind) {
                schema = &schemas[j];
                break;
            }
        if (!schema || !schema->record_bytes || !schema->alignment_bytes
            || (tables[i].record_count && !tables[i].records)
            || tables[i].record_count > UINT32_MAX
            || tables[i].record_count > SIZE_MAX / schema->record_bytes
            || !align_up(cursor, schema->alignment_bytes, &cursor)) {
            valid = false;
            break;
        }
        size_t bytes = tables[i].record_count * schema->record_bytes;
        if (bytes > SIZE_MAX - cursor) {
            valid = false;
            break;
        }
        entries[i] = (n00bb_abi_directory_entry_t){
            .kind = schema->kind,
            .schema_version = schema->schema_version,
            .record_bytes = schema->record_bytes,
            .record_count = (uint32_t)tables[i].record_count,
            .file_offset = cursor,
            .byte_length = bytes,
        };
        cursor += bytes;
        if (alignment < schema->alignment_bytes)
            alignment = schema->alignment_bytes;
    }
    if (!valid) {
        core_free(entries);
        return false;
    }

    uint8_t *image = core_alloc_size(1, cursor);
    for (size_t i = 0; i < table_count; i++)
        if (entries[i].byte_length)
            memcpy(image + entries[i].file_offset, tables[i].records,
                   (size_t)entries[i].byte_length);
    n00bb_abi_image_header_t header = {
        .key = key,
        .artifact_capabilities = artifact_capabilities,
        .required_alignment = alignment,
        .directory_count = (uint32_t)table_count,
        .directory_offset = N00BB_ABI_HEADER_BYTES,
        .total_extent = cursor,
    };
    valid = n00bb_abi_container_write(image, cursor, &header,
        entries, schemas, schema_count, false);
    core_free(entries);
    if (!valid) {
        core_free(image);
        return false;
    }
    *out = (n00bb_abi_packed_tables_t){
        .bytes = image, .byte_count = cursor, .alignment_bytes = alignment,
    };
    return true;
}

void
n00bb_abi_packed_tables_release(n00bb_abi_packed_tables_t *packed)
{
    if (!packed) return;
    core_free(packed->bytes);
    *packed = (n00bb_abi_packed_tables_t){0};
}

bool
n00bb_abi_write_object(
    const core_target_desc_t *target,
    const n00bb_abi_packed_tables_t *packed,
    const core_object_emit_section_t *extra_sections, size_t extra_count,
    const core_object_emit_symbol_t *symbols, size_t symbol_count,
    const core_object_emit_absolute64_t *relocations,
    size_t relocation_count, uint8_t **out_image, size_t *out_size)
{
    if (!target || !packed || !packed->bytes || !packed->alignment_bytes
        || packed->byte_count < N00BB_ABI_HEADER_BYTES
        || !out_image || !out_size || extra_count >= UINT32_MAX
        || extra_count > SIZE_MAX / sizeof(core_object_emit_section_t) - 1
        || (extra_count && !extra_sections)) return false;
    n00bb_abi_image_key_t key;
    if (!image_key(target, &key)) return false;
    uint32_t entry_count = core_le_load_u32(packed->bytes + 60);
    if (entry_count > (packed->byte_count - N00BB_ABI_HEADER_BYTES)
                      / N00BB_ABI_DIRECTORY_BYTES) return false;
    n00bb_abi_directory_entry_t *entries = core_alloc_array(
        n00bb_abi_directory_entry_t, entry_count);
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_image_header_t header;
    bool valid = n00bb_abi_container_read(packed->bytes, packed->byte_count,
        &key, schemas, schema_count, false, &header, entries, entry_count);
    if (!valid || header.required_alignment != packed->alignment_bytes) {
        core_free(entries);
        return false;
    }
    const char *name, *segment = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = ".data.rel.ro.n00b_meta";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        name = "__n00b_meta";
        segment = "__DATA_CONST";
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = ".n00b$M";
        break;
    default:
        return false;
    }
    size_t section_count = extra_count + 1;
    core_object_emit_section_t *sections = core_alloc_array(
        core_object_emit_section_t, section_count);
    sections[0] = (core_object_emit_section_t){
        .name = name, .segment = segment,
        .bytes = packed->bytes, .size = packed->byte_count,
        .alignment = packed->alignment_bytes,
    };
    if (extra_count)
        memcpy(sections + 1, extra_sections,
               extra_count * sizeof(*extra_sections));
    uint8_t *image = nullptr;
    size_t image_bytes = 0;
    bool written = core_object_emit(target, sections, section_count,
        symbols, symbol_count, relocations, relocation_count,
        &image, &image_bytes);
    core_free(sections);
    core_object_section_t metadata;
    if (written)
        written = core_object_section_find(image, image_bytes,
            target->object_format, segment, name, &metadata)
            && metadata.size == packed->byte_count
            && n00bb_abi_container_write((uint8_t *)metadata.bytes,
                metadata.size, &header, entries, schemas, schema_count,
                false);
    core_free(entries);
    if (!written) {
        core_free(image);
        return false;
    }
    *out_image = image;
    *out_size = image_bytes;
    return written;
}

static bool
empty_unit_error(core_translation_unit_t *unit, const char *message)
{
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
        core_string_from_cstr(message));
    return false;
}

static bool
phase_has_only_trivial_initializer(const ir_module_t *module)
{
    if (ir_global_count(module) || ir_func_count(module) > 1)
        return false;
    if (!ir_func_count(module)) return true;
    ir_func_t *function = ir_func_at((ir_module_t *)module, 0);
    if (!function || !ir_func_is_initializer(function)
        || function->n_params || function->n_locals
        || function->n_blocks != 1 || !function->blocks[0]
        || function->blocks[0]->n_instrs != 1) return false;
    ir_instr_t *instruction = function->blocks[0]->instrs[0];
    return instruction && instruction->op == IR_OP_RETURN
        && !instruction->n_operands;
}

bool
n00bb_abi_emit_empty_unit(
    core_translation_unit_t *unit, core_string_t package_id,
    const parse_symtab_t *symbols,
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    const core_target_desc_t *target, uint64_t artifact_capabilities,
    n00bb_abi_packed_tables_t *out_tables,
    uint8_t **out_object, size_t *out_object_bytes)
{
    if (!unit || !symbols || !comptime_ir || !runtime_ir || !target
        || !target->triple || !unit->module.identifier.data
        || !out_tables || !out_object || !out_object_bytes
        || !package_id.data || core_translation_unit_has_fatal_diagnostics(unit))
        return false;
    for (const parse_scope_t *scope = symbols->all_scopes;
         scope; scope = scope->all_next)
        for (const parse_sym_entry_t *symbol = scope->first_in_scope;
             symbol; symbol = symbol->next_in_scope)
            if (symbol->decl_node || symbol->definition_count)
                return empty_unit_error(unit,
                    "this ABI emitter path cannot omit local declarations");
    if (!core_translation_unit_imports_finalized(unit)
        || core_translation_unit_import_count(unit)
        || !phase_has_only_trivial_initializer(comptime_ir)
        || !phase_has_only_trivial_initializer(runtime_ir))
        return empty_unit_error(unit,
            "this ABI emitter path requires no imports, declarations, globals, or executable initializer body");
    const char *runtime_name = ir_module_name(runtime_ir);
    if (!runtime_name || strlen(runtime_name) != unit->module.identifier.u8_bytes
        || memcmp(runtime_name, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes)
        || !ir_module_target(runtime_ir) || !ir_module_target(comptime_ir)
        || strcmp(ir_module_target(runtime_ir), target->triple)
        || strcmp(ir_module_target(comptime_ir), target->triple))
        return empty_unit_error(unit,
            "ABI unit identity or target does not match its IR modules");

    n00b_abi_package_v1 packages[2] = {0};
    n00b_abi_module_v1 modules[2] = {0};
    n00b_abi_source_file_v1 sources[2] = {0};
    n00b_abi_runtime_config_v1 runtime_configs[2] = {0};
    uint32_t package_modules[1] = {1};
    packages[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    packages[1].modules = (n00b_abi_range_v1){.first = 0, .count = 1};
    modules[1].package_index = 1;
    modules[1].runtime_config_index = 1;
    modules[1].source_file_index = 1;
    const char *source_bytes = core_source_file_contents(&unit->source);
    size_t source_size = core_source_file_size(&unit->source);
    sources[1].source_identity = core_xxh3_64bits_raw(
        source_bytes ? source_bytes : "", source_size);

    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    n00bb_abi_type_index_t *type_index = n00bb_abi_type_index_new(
        (ir_module_t *)runtime_ir);
    n00bb_abi_type_identity_rows_t type_rows = {0};
    n00bb_abi_type_layout_rows_t layout_rows = {0};
    bool valid = pool && type_index
        && n00bb_abi_type_identity_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            symbols, pool, &type_rows)
        && type_rows.count == 1
        && n00bb_abi_type_layout_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir, 1,
            &type_rows, &layout_rows);
    n00bb_abi_type_index_free(type_index);
    valid = valid && n00bb_abi_name_pool_encode(pool, package_id,
        N00B_ABI_TABLE_PACKAGE, 1, offsetof(n00b_abi_package_v1, name),
        &packages[1].name)
        && n00bb_abi_name_pool_encode(pool, unit->module.identifier,
            N00B_ABI_TABLE_MODULE, 1,
            offsetof(n00b_abi_module_v1, full_path), &modules[1].full_path)
        && n00bb_abi_name_pool_encode(pool,
            core_source_file_path(&unit->source),
            N00B_ABI_TABLE_SOURCE_FILE, 1,
            offsetof(n00b_abi_source_file_v1, path), &sources[1].path);
    if (!valid) {
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        return empty_unit_error(unit, "could not build ABI identity and layout rows");
    }

    size_t stream_count = layout_rows.run_index_count + 1;
    uint32_t *stream = core_alloc_array(uint32_t, stream_count);
    stream[0] = package_modules[0];
    if (layout_rows.run_index_count)
        memcpy(stream + 1, layout_rows.run_indexes,
               layout_rows.run_index_count * sizeof(*stream));
    n00bb_abi_table_rows_t rows[] = {
        {N00B_ABI_TABLE_PACKAGE, packages, 2},
        {N00B_ABI_TABLE_MODULE, modules, 2},
        {N00B_ABI_TABLE_TYPE, type_rows.rows, type_rows.count},
        {N00B_ABI_TABLE_LAYOUT, layout_rows.layouts,
         layout_rows.layout_count},
        {N00B_ABI_TABLE_PHYSICAL_RUN, layout_rows.runs,
         layout_rows.run_count},
        {N00B_ABI_TABLE_SOURCE_FILE, sources, 2},
        {N00B_ABI_TABLE_RUNTIME_CONFIG, runtime_configs, 2},
        {N00B_ABI_TABLE_U32_STREAM, stream, stream_count},
    };
    n00bb_abi_packed_tables_t packed = {0};
    valid = n00bb_abi_pack_tables(target, artifact_capabilities,
        rows, sizeof(rows) / sizeof(*rows), &packed);
    core_free(stream);
    if (!valid) {
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        return empty_unit_error(unit, "could not pack ABI identity tables");
    }

    n00bb_abi_image_key_t key;
    n00bb_abi_directory_entry_t entries[sizeof(rows) / sizeof(*rows)];
    n00bb_abi_image_header_t header;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    valid = image_key(target, &key)
        && n00bb_abi_container_read(packed.bytes, packed.byte_count,
            &key, schemas, schema_count, false, &header, entries,
            sizeof(entries) / sizeof(*entries));
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t relocation_count = 0;
    if (valid)
        valid = n00bb_abi_name_pool_relocations(pool, entries,
            sizeof(entries) / sizeof(*entries), &relocations,
            &relocation_count);
    core_object_emit_section_t string_section = {0};
    core_object_emit_symbol_t string_symbol = {0};
    if (n00bb_abi_name_pool_size(pool)) {
        string_section = (core_object_emit_section_t){
            .name = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__n00b_strings" : target->object_format == CORE_TARGET_OBJECT_COFF
                    ? ".rdata$S" : ".rodata.n00b_strings",
            .segment = target->object_format == CORE_TARGET_OBJECT_MACH_O
                ? "__TEXT" : nullptr,
            .bytes = n00bb_abi_name_pool_bytes(pool),
            .size = n00bb_abi_name_pool_size(pool),
            .alignment = 1,
        };
        string_symbol = (core_object_emit_symbol_t){
            .name = "n00b_string_pool", .section_index = 2,
        };
    }
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    if (valid)
        valid = n00bb_abi_write_object(target, &packed,
            n00bb_abi_name_pool_size(pool) ? &string_section : nullptr,
            n00bb_abi_name_pool_size(pool) ? 1 : 0,
            n00bb_abi_name_pool_size(pool) ? &string_symbol : nullptr,
            n00bb_abi_name_pool_size(pool) ? 1 : 0,
            relocations, relocation_count,
            &object, &object_bytes);
    core_free(relocations);
    n00bb_abi_type_layout_rows_release(&layout_rows);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(pool);
    if (!valid) {
        n00bb_abi_packed_tables_release(&packed);
        return empty_unit_error(unit, "could not write ABI identity object");
    }
    *out_tables = packed;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}
