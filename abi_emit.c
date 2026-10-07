#include "abi_emit.h"

#include <limits.h>
#include <string.h>

#include "abi_container.h"
#include "abi_constraint_rows.h"
#include "abi_decl_index.h"
#include "abi_declarations.h"
#include "abi_enum_rows.h"
#include "abi_function_rows.h"
#include "abi_emit_strings.h"
#include "abi_type_index.h"
#include "abi_type_layout_rows.h"
#include "abi_type_rows.h"
#include "abi_type_signatures.h"
#include "abi_trait_method_rows.h"
#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
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
abi_unit_error(core_translation_unit_t *unit, const char *message)
{
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0007"),
        core_string_from_cstr(message));
    return false;
}

static bool
phase_has_only_trivial_initializer(const ir_module_t *module)
{
    if (ir_global_count(module)) return false;
    for (size_t i = 0; i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)module, i);
        if (!function) return false;
        if (!ir_func_is_initializer(function)) continue;
        if (function->n_params || function->n_locals
            || function->n_blocks != 1 || !function->blocks[0]
            || function->blocks[0]->n_instrs != 1) return false;
        ir_instr_t *instruction = function->blocks[0]->instrs[0];
        if (!instruction || instruction->op != IR_OP_RETURN
            || instruction->n_operands) return false;
    }
    return true;
}

typedef struct {
    core_translation_unit_t **members;
    size_t count;
    bool *visited;
    size_t *order;
    size_t ordered;
} abi_scc_order_t;

static bool
module_name_less(core_string_t left, core_string_t right)
{
    size_t common = left.u8_bytes < right.u8_bytes
        ? left.u8_bytes : right.u8_bytes;
    int comparison = memcmp(left.data, right.data, common);
    return comparison < 0 || (!comparison && left.u8_bytes < right.u8_bytes);
}

static bool
append_scc_input(core_buffer_t *buffer, const char *bytes, size_t count)
{
    if (count && !bytes) return false;
    uint8_t length[8];
    core_le_store_u64(length, (uint64_t)count);
    core_buffer_append(buffer, (const char *)length, sizeof(length));
    core_buffer_append(buffer, bytes, count);
    return true;
}

static bool
append_scc_source(core_buffer_t *buffer, const core_source_file_t *source)
{
    const char *contents = core_source_file_contents(source);
    size_t size = core_source_file_size(source);
    if (size && !contents) return false;
    uint8_t digest[8];
    core_le_store_u64(digest, core_hash_raw(contents ? contents : "", size));
    core_buffer_append(buffer, (const char *)digest, sizeof(digest));
    return true;
}

static bool
visit_scc_member(abi_scc_order_t *group, size_t at)
{
    group->visited[at] = true;
    group->order[group->ordered++] = at;
    core_translation_unit_t *unit = group->members[at];
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            unit, i);
        for (size_t j = 0; j < group->count; j++) {
            if (group->members[j] != imported) continue;
            if (!group->visited[j] && !visit_scc_member(group, j))
                return false;
            break;
        }
    }
    return true;
}

static bool
recursive_scc_fragment(core_link_unit_t *link,
                       core_translation_unit_t *unit,
                       n00b_abi_scc_v1 *manifest, uint32_t *ordinal,
                       bool *recursive)
{
    *recursive = false;
    if (!link) return true;
    size_t group_index = core_link_unit_scc_count(link);
    size_t count = 0;
    for (size_t i = 0; i < group_index; i++) {
        size_t members = core_link_unit_scc_member_count(link, i);
        for (size_t j = 0; j < members; j++) {
            if (core_link_unit_scc_member(link, i, j) != unit) continue;
            group_index = i;
            count = members;
            break;
        }
        if (count) break;
    }
    if (!count || count >= UINT32_MAX) return false;
    if (count == 1) {
        bool self_import = false;
        for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
            self_import |= core_translation_unit_import(unit, i) == unit;
        if (!self_import) return true;
    }
    core_translation_unit_t **members = core_alloc_array(
        core_translation_unit_t *, count);
    bool *visited = core_alloc_array(bool, count);
    size_t *order = core_alloc_array(size_t, count);
    bool valid = members && visited && order;
    size_t first = 0;
    for (size_t i = 0; valid && i < count; i++) {
        members[i] = core_link_unit_scc_member(link, group_index, i);
        if (!members[i] || !members[i]->module.identifier.data
            || !core_translation_unit_imports_finalized(members[i])) {
            valid = false;
            break;
        }
        if (module_name_less(members[i]->module.identifier,
                             members[first]->module.identifier)) first = i;
    }
    abi_scc_order_t group = {
        .members = members, .count = count,
        .visited = visited, .order = order,
    };
    if (valid) valid = visit_scc_member(&group, first)
        && group.ordered == count;
    core_buffer_t input = {0};
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *member = members[order[i]];
        core_string_t name = member->module.identifier;
        /* A source edit conservatively invalidates the entire cycle. */
        valid = append_scc_input(&input, name.data, name.u8_bytes)
            && append_scc_source(&input, &member->source);
        for (size_t j = 0; valid
             && j < core_translation_unit_import_count(member); j++) {
            core_translation_unit_t *imported = core_translation_unit_import(
                member, j);
            core_string_t import_name = imported->module.identifier;
            valid = append_scc_input(&input, import_name.data,
                                     import_name.u8_bytes);
        }
        if (member == unit) *ordinal = (uint32_t)i;
    }
    if (valid) {
        core_buffer_t identity = {0};
        core_buffer_puts(&identity, "n00b.scc.interface.v1");
        core_buffer_append(&identity, input.data, input.byte_len);
        manifest->interface_identity = core_hash_raw(identity.data,
                                                     identity.byte_len);
        core_buffer_release(&identity);
        core_buffer_puts(&identity, "n00b.scc.freshness.v1");
        core_buffer_append(&identity, input.data, input.byte_len);
        manifest->freshness_identity = core_hash_raw(identity.data,
                                                     identity.byte_len);
        core_buffer_release(&identity);
        *recursive = true;
    }
    core_buffer_release(&input);
    core_free(order);
    core_free(visited);
    core_free(members);
    return valid;
}

bool
n00bb_abi_emit_unit(
    core_link_unit_t *link, core_translation_unit_t *unit,
    core_string_t package_id,
    const parse_symtab_t *symbols, parse_token_stream_t *tokens,
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    const core_target_desc_t *target, uint64_t artifact_capabilities,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_node_source_fn node_source, void *source_context,
    n00bb_abi_type_owner_lookup_fn type_owner, void *type_owner_context,
    n00bb_abi_packed_tables_t *out_tables,
    uint8_t **out_object, size_t *out_object_bytes)
{
    if (!unit || !symbols || !tokens || !comptime_ir || !runtime_ir || !target
        || !target->triple || !unit->module.identifier.data
        || !out_tables || !out_object || !out_object_bytes
        || !package_id.data || core_translation_unit_has_fatal_diagnostics(unit))
        return false;
    if (link && !node_source)
        return abi_unit_error(unit,
            "linked ABI emission requires source ownership for syntax nodes");
    n00bb_abi_decl_index_t *declarations = n00bb_abi_decl_index_new(symbols);
    if (!declarations)
        return abi_unit_error(unit, "could not index local ABI declarations");
    for (size_t i = 1; i < n00bb_abi_decl_symbol_count(declarations); i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                               (uint32_t)i);
        bool record = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("record"));
        bool enumeration = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && core_string_eq(symbol->adt_kind,
                              CORE_STRING_STATIC("enum"));
        bool trait = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && (core_string_eq(symbol->adt_kind,
                               CORE_STRING_STATIC("trait"))
                || (symbol->member_scope
                    && core_string_eq(symbol->member_scope->scope_tag,
                                      CORE_STRING_STATIC("trait"))));
        bool variant = symbol && symbol->kind == PARSE_SYM_TYPEDEF
            && ((symbol->member_scope
                 && core_string_eq(symbol->member_scope->scope_tag,
                                   CORE_STRING_STATIC("variant")))
                || core_string_eq(symbol->adt_kind,
                                  CORE_STRING_STATIC("variant")));
        bool arm = symbol && (symbol->kind == PARSE_SYM_TAG
                || symbol->kind == PARSE_SYM_VARIABLE)
            && symbol->owner_type
            && ((symbol->owner_type->member_scope
                 && core_string_eq(symbol->owner_type->member_scope->scope_tag,
                                   CORE_STRING_STATIC("variant")))
                || core_string_eq(symbol->owner_type->adt_kind,
                                  CORE_STRING_STATIC("variant")));
        bool field = symbol && symbol->kind == PARSE_SYM_VARIABLE
            && symbol->is_field && symbol->owner_type
            && core_string_eq(symbol->owner_type->adt_kind,
                              CORE_STRING_STATIC("record"));
        bool enum_item = symbol && symbol->kind == PARSE_SYM_ENUM_CONST
            && symbol->owner_type
            && core_string_eq(symbol->owner_type->adt_kind,
                              CORE_STRING_STATIC("enum"));
        bool function = symbol && symbol->kind == PARSE_SYM_FUNCTION;
        bool parameter = symbol && symbol->kind == PARSE_SYM_PARAM;
        bool local = symbol && symbol->kind == PARSE_SYM_VARIABLE
            && !symbol->is_field;
        if (!record && !field && !enumeration && !enum_item && !trait
            && !variant && !arm
            && !function && !parameter && !local) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "ABI emission does not support declaration '%.*s'",
                (int)symbol->name.u8_bytes, symbol->name.data);
            n00bb_abi_decl_index_free(declarations);
            bool result = abi_unit_error(unit, message.data);
            core_buffer_release(&message);
            return result;
        }
    }
    if (!core_translation_unit_imports_finalized(unit)
        || !phase_has_only_trivial_initializer(comptime_ir)
        || !phase_has_only_trivial_initializer(runtime_ir)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "this ABI emitter path requires no globals or executable initializer body");
    }
    size_t import_count = core_translation_unit_import_count(unit);
    if (import_count && (!link || !core_link_unit_graph_is_finalized(link)
        || import_count >= UINT32_MAX)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "ABI emission for imports requires a finalized link graph");
    }
    n00b_abi_scc_v1 sccs[2] = {0};
    uint32_t scc_ordinal = 0;
    bool recursive = false;
    if (link && !recursive_scc_fragment(link, unit, &sccs[1],
                                        &scc_ordinal, &recursive)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "could not establish recursive import group identity");
    }
    const char *runtime_name = ir_module_name(runtime_ir);
    if (!runtime_name || strlen(runtime_name) != unit->module.identifier.u8_bytes
        || memcmp(runtime_name, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes)
        || !ir_module_target(runtime_ir) || !ir_module_target(comptime_ir)
        || strcmp(ir_module_target(runtime_ir), target->triple)
        || strcmp(ir_module_target(comptime_ir), target->triple)) {
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit,
            "ABI unit identity or target does not match its IR modules");
    }

    n00b_abi_package_v1 packages[2] = {0};
    n00b_abi_module_v1 modules[2] = {0};
    n00b_abi_runtime_config_v1 runtime_configs[2] = {0};
    n00b_abi_requirement_v1 *requirements = core_alloc_array(
        n00b_abi_requirement_v1, import_count + 1);
    for (size_t i = 0; i < import_count; i++) {
        requirements[i + 1].symbol_kind = N00B_ABI_SYMBOL_MODULE;
        requirements[i + 1].requesting_module_index = 1;
    }
    uint32_t package_modules[1] = {1};
    uint32_t prefix_stream_count = 1 + (recursive ? 2 : 0);
    if (recursive) {
        sccs[1].members = (n00b_abi_range_v1){.first = 1, .count = 1};
        sccs[1].lexical_order = (n00b_abi_range_v1){.first = 2, .count = 1};
    }
    packages[1].visibility = N00B_ABI_VISIBILITY_PRIVATE;
    packages[1].modules = (n00b_abi_range_v1){.first = 0, .count = 1};
    modules[1].package_index = 1;
    modules[1].scc_index = recursive ? 1 : 0;
    modules[1].runtime_config_index = 1;
    modules[1].source_file_index = 1;

    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    n00bb_abi_type_index_t *type_index = n00bb_abi_type_index_new(
        (ir_module_t *)runtime_ir);
    n00bb_abi_type_identity_rows_t type_rows = {0};
    n00bb_abi_type_components_t components = {0};
    n00bb_abi_type_layout_rows_t layout_rows = {0};
    n00bb_abi_enum_rows_t enum_rows = {0};
    n00bb_abi_type_signatures_t signature_rows = {0};
    n00bb_abi_declarations_t declaration_rows = {0};
    n00bb_abi_function_rows_t function_rows = {0};
    n00bb_abi_trait_method_rows_t trait_method_rows = {0};
    n00bb_abi_constraint_rows_t constraint_rows = {0};
    bool valid = pool && type_index
        && n00bb_abi_type_index_collect_module_types(type_index,
            (ir_module_t *)comptime_ir)
        && n00bb_abi_type_index_collect_module_types(type_index,
            (ir_module_t *)runtime_ir)
        && n00bb_abi_type_index_collect_symbols(type_index, symbols)
        && n00bb_abi_type_identity_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            symbols, type_owner, type_owner_context, pool, &type_rows)
        && n00bb_abi_type_variables_bind(type_index, declarations,
            &type_rows)
        && n00bb_abi_type_components_build(type_index,
            (ir_module_t *)runtime_ir, prefix_stream_count,
            &type_rows, &components)
        && n00bb_abi_type_layout_rows_build(type_index,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            prefix_stream_count + (uint32_t)components.count,
            declarations, pool,
            &type_rows, &layout_rows)
        && n00bb_abi_enum_rows_build(type_index, declarations, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count,
            &type_rows, &enum_rows)
        && n00bb_abi_type_signatures_build(type_index, &type_rows, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count,
            &signature_rows)
        && n00bb_abi_declarations_build(declarations, type_index,
            &type_rows, unit, tokens, pool, node_source, source_context,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count,
            &declaration_rows)
        && n00bb_abi_function_rows_build(type_index, declarations,
            &type_rows, &signature_rows,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count
                + (uint32_t)declaration_rows.stream_index_count,
            (ir_module_t *)comptime_ir, (ir_module_t *)runtime_ir,
            pool, lookup, lookup_context, &function_rows)
        && n00bb_abi_trait_method_rows_build(type_index, &type_rows,
            declarations, &trait_method_rows)
        && n00bb_abi_constraint_rows_build(type_index, &type_rows,
            declarations, &function_rows, pool,
            prefix_stream_count + (uint32_t)components.count
                + (uint32_t)layout_rows.stream_index_count
                + (uint32_t)enum_rows.stream_index_count
                + (uint32_t)signature_rows.parameter_index_count
                + (uint32_t)declaration_rows.stream_index_count
                + (uint32_t)function_rows.stream_index_count,
            &constraint_rows);
    n00bb_abi_type_index_free(type_index);
    valid = valid && n00bb_abi_name_pool_encode(pool, package_id,
        N00B_ABI_TABLE_PACKAGE, 1, offsetof(n00b_abi_package_v1, name),
        &packages[1].name)
        && n00bb_abi_name_pool_encode(pool, unit->module.identifier,
            N00B_ABI_TABLE_MODULE, 1,
            offsetof(n00b_abi_module_v1, full_path), &modules[1].full_path);
    for (size_t i = 0; valid && i < import_count; i++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            unit, i);
        valid = imported && imported->module.identifier.data
            && n00bb_abi_name_pool_encode(pool,
                imported->module.identifier, N00B_ABI_TABLE_REQUIREMENT,
                (uint32_t)i + 1,
                offsetof(n00b_abi_requirement_v1, full_path),
                &requirements[i + 1].full_path);
    }
    if (!valid) {
        core_free(requirements);
        n00bb_abi_trait_method_rows_release(&trait_method_rows);
        n00bb_abi_constraint_rows_release(&constraint_rows);
        n00bb_abi_function_rows_release(&function_rows);
        n00bb_abi_declarations_release(&declaration_rows);
        n00bb_abi_type_signatures_release(&signature_rows);
        n00bb_abi_enum_rows_release(&enum_rows);
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_components_release(&components);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit, "could not build ABI type and declaration rows");
    }

    size_t stream_count = prefix_stream_count + components.count
        + layout_rows.stream_index_count
        + enum_rows.stream_index_count
        + signature_rows.parameter_index_count
        + declaration_rows.stream_index_count
        + function_rows.stream_index_count
        + constraint_rows.stream_index_count;
    uint32_t *stream = core_alloc_array(uint32_t, stream_count);
    stream[0] = package_modules[0];
    if (recursive) {
        stream[1] = 1;
        stream[2] = scc_ordinal;
    }
    if (components.count)
        memcpy(stream + prefix_stream_count, components.indexes,
               components.count * sizeof(*stream));
    if (layout_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count,
               layout_rows.stream_indexes,
               layout_rows.stream_index_count * sizeof(*stream));
    if (enum_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count,
               enum_rows.stream_indexes,
               enum_rows.stream_index_count * sizeof(*stream));
    if (signature_rows.parameter_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count,
               signature_rows.parameter_indexes,
               signature_rows.parameter_index_count * sizeof(*stream));
    if (declaration_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count,
               declaration_rows.stream_indexes,
               declaration_rows.stream_index_count * sizeof(*stream));
    if (function_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count
                   + declaration_rows.stream_index_count,
               function_rows.stream_indexes,
               function_rows.stream_index_count * sizeof(*stream));
    if (constraint_rows.stream_index_count)
        memcpy(stream + prefix_stream_count + components.count
                   + layout_rows.stream_index_count
                   + enum_rows.stream_index_count
                   + signature_rows.parameter_index_count
                   + declaration_rows.stream_index_count
                   + function_rows.stream_index_count,
               constraint_rows.stream_indexes,
               constraint_rows.stream_index_count * sizeof(*stream));
    n00bb_abi_table_rows_t rows[] = {
        {N00B_ABI_TABLE_PACKAGE, packages, 2},
        {N00B_ABI_TABLE_MODULE, modules, 2},
        {N00B_ABI_TABLE_SCC, sccs, recursive ? 2 : 1},
        {N00B_ABI_TABLE_TYPE, type_rows.rows, type_rows.count},
        {N00B_ABI_TABLE_LAYOUT, layout_rows.layouts,
         layout_rows.layout_count},
        {N00B_ABI_TABLE_PHYSICAL_RUN, layout_rows.runs,
         layout_rows.run_count},
        {N00B_ABI_TABLE_PACKED_RUN, layout_rows.packed_runs,
         layout_rows.packed_run_count},
        {N00B_ABI_TABLE_FIELD, layout_rows.fields,
         layout_rows.field_count},
        {N00B_ABI_TABLE_ARM, layout_rows.arms,
         layout_rows.arm_count},
        {N00B_ABI_TABLE_TAG_EXTRACTOR, layout_rows.tag_extractors,
         layout_rows.tag_extractor_count},
        {N00B_ABI_TABLE_ENUM_ITEM, enum_rows.items,
         enum_rows.item_count},
        {N00B_ABI_TABLE_FUNCTION, function_rows.functions,
         function_rows.function_count},
        {N00B_ABI_TABLE_TRAIT_METHOD, trait_method_rows.rows,
         trait_method_rows.count},
        {N00B_ABI_TABLE_CONSTRAINT, constraint_rows.rows,
         constraint_rows.count},
        {N00B_ABI_TABLE_CALL_SIGNATURE, signature_rows.signatures,
         signature_rows.signature_count},
        {N00B_ABI_TABLE_PARAMETER, signature_rows.parameters,
         signature_rows.parameter_count},
        {N00B_ABI_TABLE_SCOPE, declaration_rows.scopes,
         declaration_rows.scope_count},
        {N00B_ABI_TABLE_SYMBOL, declaration_rows.symbols,
         declaration_rows.symbol_count},
        {N00B_ABI_TABLE_SOURCE_LOCATION, declaration_rows.locations,
         declaration_rows.location_count},
        {N00B_ABI_TABLE_SOURCE_FILE, declaration_rows.source_files,
         declaration_rows.source_file_count},
        {N00B_ABI_TABLE_RUNTIME_CONFIG, runtime_configs, 2},
        {N00B_ABI_TABLE_REQUIREMENT, requirements, import_count + 1},
        {N00B_ABI_TABLE_U32_STREAM, stream, stream_count},
    };
    n00bb_abi_packed_tables_t packed = {0};
    valid = n00bb_abi_pack_tables(target, artifact_capabilities,
        rows, sizeof(rows) / sizeof(*rows), &packed);
    core_free(stream);
    if (!valid) {
        core_free(requirements);
        n00bb_abi_trait_method_rows_release(&trait_method_rows);
        n00bb_abi_constraint_rows_release(&constraint_rows);
        n00bb_abi_function_rows_release(&function_rows);
        n00bb_abi_declarations_release(&declaration_rows);
        n00bb_abi_type_signatures_release(&signature_rows);
        n00bb_abi_enum_rows_release(&enum_rows);
        n00bb_abi_type_layout_rows_release(&layout_rows);
        n00bb_abi_type_components_release(&components);
        n00bb_abi_type_identity_rows_release(&type_rows);
        n00bb_abi_name_pool_free(pool);
        n00bb_abi_decl_index_free(declarations);
        return abi_unit_error(unit, "could not pack ABI tables");
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
    core_free(requirements);
    n00bb_abi_trait_method_rows_release(&trait_method_rows);
    n00bb_abi_constraint_rows_release(&constraint_rows);
    n00bb_abi_function_rows_release(&function_rows);
    n00bb_abi_declarations_release(&declaration_rows);
    n00bb_abi_type_signatures_release(&signature_rows);
    n00bb_abi_enum_rows_release(&enum_rows);
    n00bb_abi_type_layout_rows_release(&layout_rows);
    n00bb_abi_type_components_release(&components);
    n00bb_abi_type_identity_rows_release(&type_rows);
    n00bb_abi_name_pool_free(pool);
    n00bb_abi_decl_index_free(declarations);
    if (!valid) {
        n00bb_abi_packed_tables_release(&packed);
        return abi_unit_error(unit, "could not write ABI object");
    }
    *out_tables = packed;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}
