#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_container.h"
#include "n00b-abi-v1.h"
#include "th/typehash.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

static const n00bb_abi_directory_entry_t *
entry_for(const n00bb_abi_directory_entry_t *entries, size_t count,
          uint32_t kind)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].kind == kind) return &entries[i];
    return nullptr;
}

static const void *
row_at(const core_object_section_t *section,
       const n00bb_abi_directory_entry_t *entry, uint32_t index)
{
    assert(entry && index < entry->record_count);
    return section->bytes + entry->file_offset
        + (size_t)index * entry->record_bytes;
}

static bool
short_name_is(const n00b_abi_string_v1 *value, const char *name)
{
    size_t length = strlen(name);
    return length <= 14
        && (value->short_value.selector & 15u) == length
        && !(value->short_value.selector & 0xc0u)
        && !memcmp(value->short_value.bytes, name, length);
}

static bool
find_metadata(const uint8_t *object, size_t length,
              core_object_section_t *metadata)
{
    struct {
        core_target_object_format_t format;
        const char *segment;
        const char *name;
    } locations[] = {
        {CORE_TARGET_OBJECT_ELF, nullptr, ".data.rel.ro.n00b_meta"},
        {CORE_TARGET_OBJECT_MACH_O, "__DATA_CONST", "__n00b_meta"},
        {CORE_TARGET_OBJECT_COFF, nullptr, ".n00b$M"},
    };
    for (size_t i = 0; i < sizeof(locations) / sizeof(*locations); i++)
        if (core_object_section_find(object, length, locations[i].format,
            locations[i].segment, locations[i].name, metadata)) return true;
    return false;
}

typedef struct {
    uint64_t interface_identity;
    uint64_t freshness_identity;
    uint32_t ordinal;
} cycle_fact_t;

static cycle_fact_t
cycle_fact(const core_object_section_t *metadata,
           const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *modules = entry_for(entries, count,
        N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *sccs = entry_for(entries, count,
        N00B_ABI_TABLE_SCC);
    const n00bb_abi_directory_entry_t *requirements = entry_for(entries,
        count, N00B_ABI_TABLE_REQUIREMENT);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(modules && modules->record_count == 2
        && sccs && sccs->record_count == 2
        && requirements && requirements->record_count == 2 && stream);
    const n00b_abi_module_v1 *module = row_at(metadata, modules, 1);
    const n00b_abi_scc_v1 *group = row_at(metadata, sccs, 1);
    const n00b_abi_requirement_v1 *import = row_at(metadata,
        requirements, 1);
    assert(module->scc_index == 1 && !module->imports.count
        && group->interface_identity && group->freshness_identity
        && group->members.count == 1 && group->lexical_order.count == 1
        && group->members.first < stream->record_count
        && group->lexical_order.first < stream->record_count
        && *(const uint32_t *)row_at(metadata, stream,
            group->members.first) == 1
        && import->symbol_kind == N00B_ABI_SYMBOL_MODULE
        && import->requesting_module_index == 1);
    return (cycle_fact_t){
        .interface_identity = group->interface_identity,
        .freshness_identity = group->freshness_identity,
        .ordinal = *(const uint32_t *)row_at(metadata, stream,
            group->lexical_order.first),
    };
}

static cycle_fact_t
cycle_fact_from_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *object = malloc((size_t)length);
    assert(object && fread(object, 1, (size_t)length, file)
        == (size_t)length && !fclose(file));
    core_object_section_t metadata = {0};
    assert(find_metadata(object, (size_t)length, &metadata));
    n00bb_abi_image_key_t key = {
        .target_id = core_le_load_u32(metadata.bytes + 16),
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .cpu_profile = core_le_load_u64(metadata.bytes + 40),
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = calloc(schema_count,
        sizeof(*entries));
    assert(entries);
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
        schemas, schema_count, false, &header, entries, schema_count));
    cycle_fact_t result = cycle_fact(&metadata, entries,
                                      header.directory_count);
    free(entries);
    free(object);
    return result;
}

static void
check_enum_item(const core_object_section_t *metadata,
                const n00bb_abi_directory_entry_t *stream,
                const n00bb_abi_directory_entry_t *items,
                const n00bb_abi_directory_entry_t *symbols,
                const n00bb_abi_directory_entry_t *locations,
                const n00b_abi_type_v1 *type, uint32_t type_index,
                uint32_t ordinal, const char *name,
                uint64_t low, uint64_t high)
{
    assert(ordinal < type->fields_or_arms.count);
    uint32_t position = type->fields_or_arms.first + ordinal;
    assert(position < stream->record_count);
    uint32_t item_index = *(const uint32_t *)row_at(metadata, stream,
                                                    position);
    assert(item_index && item_index < items->record_count);
    const n00b_abi_enum_item_v1 *item = row_at(metadata, items, item_index);
    assert(item->parent_type_index == type_index);
    assert(item->declaration_ordinal == ordinal);
    assert(item->value_low == low && item->value_high == high);
    assert(short_name_is(&item->name, name));
    assert(item->symbol_index && item->symbol_index < symbols->record_count);
    const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
                                               item->symbol_index);
    assert(short_name_is(&symbol->name, name));
    assert(symbol->kind == N00B_ABI_SYMBOL_ENUM_ITEM);
    assert(symbol->type_index == type_index);
    assert(symbol->declaration_location_index
        && symbol->declaration_location_index < locations->record_count);
    const n00b_abi_source_location_v1 *location = row_at(metadata, locations,
        symbol->declaration_location_index);
    assert(location->file_index == 1 && location->start_line > 0);
}

static void
check_function(const core_object_section_t *metadata,
               const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *functions = entry_for(entries,
        count, N00B_ABI_TABLE_FUNCTION);
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *signatures = entry_for(entries,
        count, N00B_ABI_TABLE_CALL_SIGNATURE);
    const n00bb_abi_directory_entry_t *parameters = entry_for(entries,
        count, N00B_ABI_TABLE_PARAMETER);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries,
        count, N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *locations = entry_for(entries,
        count, N00B_ABI_TABLE_SOURCE_LOCATION);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries,
        count, N00B_ABI_TABLE_U32_STREAM);
    assert(functions && functions->record_count == 2 && types
        && signatures && parameters && symbols && locations && stream);
    const n00b_abi_function_v1 *function = row_at(metadata, functions, 1);
    assert(short_name_is(&function->name, "add"));
    assert(function->module_index == 1 && function->scope_index
        && function->symbol_index && function->symbol_index
            < symbols->record_count);
    const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
        function->symbol_index);
    assert(symbol->kind == N00B_ABI_SYMBOL_FUNCTION
        && short_name_is(&symbol->name, "add")
        && symbol->declaration_location_index
        && symbol->declaration_location_index < locations->record_count);
    const n00b_abi_source_location_v1 *location = row_at(metadata, locations,
        symbol->declaration_location_index);
    assert(location->file_index == 1 && location->start_line == 1);
    assert(symbol->type_index && symbol->type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types,
        symbol->type_index);
    assert(type->kind == N00B_ABI_TYPE_FUNCTION
        && type->call_signature_index
        && type->call_signature_index < signatures->record_count);
    const n00b_abi_call_signature_v1 *signature = row_at(metadata,
        signatures, type->call_signature_index);
    assert(signature->type_index == symbol->type_index
        && function->success_type_index == signature->success_type_index
        && function->failure_type_index == signature->failure_type_index
        && function->parameters.count == 2
        && !function->keywords.count && !function->variadic_type_index
        && !function->code_ranges.count && !function->frame_layouts.count);
    assert(function->parameters.first + function->parameters.count
        <= stream->record_count);
    for (uint32_t i = 0; i < function->parameters.count; i++) {
        uint32_t index = *(const uint32_t *)row_at(metadata, stream,
            function->parameters.first + i);
        assert(index && index < parameters->record_count);
        const n00b_abi_parameter_v1 *parameter = row_at(metadata,
            parameters, index);
        assert(parameter->passing_mode == N00B_ABI_PASS_IN
            && parameter->declaration_ordinal == i
            && parameter->type_index < types->record_count);
    }
}

static void
check_import(const core_object_section_t *metadata,
             const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *modules = entry_for(entries,
        count, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *requirements = entry_for(entries,
        count, N00B_ABI_TABLE_REQUIREMENT);
    assert(modules && modules->record_count == 2
        && requirements && requirements->record_count == 2);
    const n00b_abi_module_v1 *module = row_at(metadata, modules, 1);
    const n00b_abi_requirement_v1 *import = row_at(metadata,
        requirements, 1);
    assert(short_name_is(&module->full_path, "import_root")
        && !module->imports.count
        && short_name_is(&import->full_path, "import_dep")
        && import->symbol_kind == N00B_ABI_SYMBOL_MODULE
        && import->requesting_module_index == 1
        && !import->required_layout_index
        && !import->required_capabilities);
}

static void
check_imported_record(const core_object_section_t *metadata,
                      const n00bb_abi_directory_entry_t *entries,
                      size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *requirements = entry_for(entries,
        count, N00B_ABI_TABLE_REQUIREMENT);
    assert(types && requirements && requirements->record_count == 2);
    const n00b_abi_requirement_v1 *import = row_at(metadata,
        requirements, 1);
    assert(short_name_is(&import->full_path, "record_dep")
        && import->symbol_kind == N00B_ABI_SYMBOL_MODULE);
    th_ctx *type_context = th_ctx_new();
    assert(type_context);
    uint64_t local_hash = th_hash(type_context,
        th_name(type_context, "record_root.SharedRecord"));
    uint64_t imported_hash = th_hash(type_context,
        th_name(type_context, "record_dep.SharedRecord"));
    uint32_t local_records = 0, imported_records = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->typehash == local_hash) {
            assert(type->kind == N00B_ABI_TYPE_RECORD
                && type->owner_module_index == 1);
            assert(type->layout_index && type->fields_or_arms.count == 1);
            local_records++;
        }
        else if (type->typehash == imported_hash) {
            assert(type->kind == N00B_ABI_TYPE_RECORD);
            assert(!type->owner_module_index && !type->layout_index
                && !type->fields_or_arms.count);
            imported_records++;
        }
    }
    assert(local_records == 1 && imported_records == 1);
    th_ctx_free(type_context);
}

static void
check_imported_enum(const core_object_section_t *metadata,
                    const n00bb_abi_directory_entry_t *entries,
                    size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *requirements = entry_for(entries,
        count, N00B_ABI_TABLE_REQUIREMENT);
    const n00bb_abi_directory_entry_t *items = entry_for(entries,
        count, N00B_ABI_TABLE_ENUM_ITEM);
    assert(types && requirements && items && requirements->record_count == 2
        && items->record_count == 1);
    const n00b_abi_requirement_v1 *import = row_at(metadata,
        requirements, 1);
    assert(short_name_is(&import->full_path, "foreign_enum")
        && import->symbol_kind == N00B_ABI_SYMBOL_MODULE);
    th_ctx *type_context = th_ctx_new();
    assert(type_context);
    uint64_t choice_hash = th_hash(type_context,
        th_name(type_context, "foreign_enum.Choice"));
    uint64_t comparison_hash = th_hash(type_context,
        th_name(type_context, "Comparison"));
    uint32_t imported_enums = 0, builtin_enums = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        assert(type->kind != N00B_ABI_TYPE_VARIABLE);
        if (type->typehash == comparison_hash) {
            assert(type->kind == N00B_ABI_TYPE_ENUM
                && !type->owner_module_index && !type->fields_or_arms.count
                && type->semantic_width_bits == 2);
            builtin_enums++;
        }
        if (type->typehash != choice_hash) continue;
        assert(type->kind == N00B_ABI_TYPE_ENUM);
        assert(!type->owner_module_index && !type->layout_index
            && !type->fields_or_arms.count
            && type->semantic_width_bits == 3
            && (type->flags & N00B_ABI_TYPE_FLAG_SIGNED));
        imported_enums++;
    }
    assert(imported_enums == 1 && builtin_enums == 1);
    th_ctx_free(type_context);
}

static void
check_variant(const core_object_section_t *metadata,
              const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries, count,
        N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *extractors = entry_for(entries, count,
        N00B_ABI_TABLE_TAG_EXTRACTOR);
    const n00bb_abi_directory_entry_t *arms = entry_for(entries, count,
        N00B_ABI_TABLE_ARM);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(types && layouts && extractors && arms && symbols && stream
        && arms->record_count == 3 && extractors->record_count == 2);
    uint32_t variant_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->kind != N00B_ABI_TYPE_VARIANT
            || !type->owner_module_index) continue;
        assert(!variant_index);
        variant_index = i;
    }
    assert(variant_index);
    const n00b_abi_type_v1 *variant = row_at(metadata, types,
                                             variant_index);
    assert(variant->size_bits == 66 && variant->alignment_bytes == 8
        && variant->fields_or_arms.count == 2
        && variant->layout_index && variant->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(metadata, layouts,
                                               variant->layout_index);
    assert(layout->type_index == variant_index
        && layout->tag_extractor_index == 1);
    const n00b_abi_tag_extractor_v1 *tag = row_at(metadata, extractors,
        layout->tag_extractor_index);
    assert(tag->kind == N00B_ABI_TAG_DIRECT_BITS
        && tag->unit_byte_offset == 8 && tag->unit_width_bits == 8
        && tag->lsb_position == 0 && tag->tag_width_bits == 2
        && tag->mask == 3);
    const char *names[] = {"number", "small"};
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t position = variant->fields_or_arms.first + i;
        assert(position < stream->record_count);
        uint32_t arm_index = *(const uint32_t *)row_at(metadata, stream,
                                                       position);
        assert(arm_index == i + 1);
        const n00b_abi_arm_v1 *arm = row_at(metadata, arms, arm_index);
        assert(short_name_is(&arm->field.name, names[i])
            && arm->field.parent_type_index == variant_index
            && arm->field.declaration_ordinal == i
            && arm->field.logical_bit_offset == 0
            && arm->field.child_type_index < types->record_count
            && arm->tag_value == i + 1
            && arm->field.symbol_index
            && arm->field.symbol_index < symbols->record_count);
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
            arm->field.symbol_index);
        assert(symbol->kind == N00B_ABI_SYMBOL_VARIANT_ARM
            && short_name_is(&symbol->name, names[i])
            && symbol->type_index == arm->field.child_type_index);
    }
}

static void
check_generic(const core_object_section_t *metadata,
              const n00bb_abi_directory_entry_t *entries, size_t count,
              const char *function_name, const char *parameter_name,
              const char *local_name, uint32_t passing_mode)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *scopes = entry_for(entries, count,
        N00B_ABI_TABLE_SCOPE);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *functions = entry_for(entries, count,
        N00B_ABI_TABLE_FUNCTION);
    const n00bb_abi_directory_entry_t *parameters = entry_for(entries,
        count, N00B_ABI_TABLE_PARAMETER);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(types && scopes && symbols && functions && parameters && stream);
    uint32_t variable_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->kind != N00B_ABI_TYPE_VARIABLE) continue;
        assert(!variable_index);
        assert(!type->typehash && !type->size_bits && !type->alignment_bytes
            && !type->layout_index && !type->canonical_name.words[0]
            && !type->canonical_name.words[1]
            && (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
            && type->typevar_owner_scope_index
            && type->typevar_owner_scope_index < scopes->record_count
            && !type->typevar_ordinal);
        const n00b_abi_scope_v1 *scope = row_at(metadata, scopes,
            type->typevar_owner_scope_index);
        assert(short_name_is(&scope->kind_name, "function"));
        variable_index = i;
    }
    assert(variable_index);
    uint32_t parameter_symbol = 0;
    uint32_t local_symbol = 0;
    for (uint32_t i = 1; i < symbols->record_count; i++) {
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols, i);
        if (local_name && short_name_is(&symbol->name, local_name)) {
            assert(!local_symbol && symbol->type_index == variable_index);
            local_symbol = i;
            continue;
        }
        if (!short_name_is(&symbol->name, parameter_name)) continue;
        assert(!parameter_symbol && symbol->type_index == variable_index);
        const n00b_abi_type_v1 *type = row_at(metadata, types,
            variable_index);
        assert(symbol->scope_index == type->typevar_owner_scope_index);
        parameter_symbol = i;
    }
    assert(parameter_symbol);
    assert(!local_name || local_symbol);
    uint32_t generic_function = 0;
    for (uint32_t i = 1; i < functions->record_count; i++) {
        const n00b_abi_function_v1 *function = row_at(metadata, functions, i);
        if (!short_name_is(&function->name, function_name)) continue;
        assert(!generic_function && function->parameters.count == 1);
        uint32_t parameter_index = *(const uint32_t *)row_at(metadata,
            stream, function->parameters.first);
        assert(parameter_index && parameter_index < parameters->record_count);
        const n00b_abi_parameter_v1 *parameter = row_at(metadata,
            parameters, parameter_index);
        assert(parameter->type_index == variable_index
            && parameter->passing_mode == passing_mode);
        generic_function = i;
    }
    assert(generic_function);
}

static void
check_constraint(const core_object_section_t *metadata,
                 const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *functions = entry_for(entries,
        count, N00B_ABI_TABLE_FUNCTION);
    const n00bb_abi_directory_entry_t *constraints = entry_for(entries,
        count, N00B_ABI_TABLE_CONSTRAINT);
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries,
        count, N00B_ABI_TABLE_U32_STREAM);
    assert(functions && functions->record_count == 2 && constraints
        && constraints->record_count > 1 && types && stream);
    const n00b_abi_function_v1 *function = row_at(metadata, functions, 1);
    assert(short_name_is(&function->name, "keep")
        && function->constraints.count == 1);
    uint32_t position = function->constraints.first;
    assert(position < stream->record_count);
    uint32_t index = *(const uint32_t *)row_at(metadata, stream, position);
    assert(index && index < constraints->record_count);
    const n00b_abi_constraint_v1 *constraint = row_at(metadata,
        constraints, index);
    assert(constraint->kind == N00B_ABI_CONSTRAINT_EXCLUDES
        && constraint->variable_ordinal == 0
        && constraint->target_type_index < types->record_count);
    const n00b_abi_type_v1 *target = row_at(metadata, types,
        constraint->target_type_index);
    assert(target->kind == N00B_ABI_TYPE_STRING);
    uint32_t variable_count = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->kind != N00B_ABI_TYPE_VARIABLE) continue;
        variable_count++;
        assert(type->typevar_owner_scope_index
            && type->typevar_ordinal == constraint->variable_ordinal
            && type->constraints.count >= 1);
        uint32_t first = type->constraints.first;
        assert(first < stream->record_count);
        uint32_t inferred_index = *(const uint32_t *)row_at(metadata,
            stream, first);
        assert(inferred_index && inferred_index < constraints->record_count);
        const n00b_abi_constraint_v1 *inferred = row_at(metadata,
            constraints, inferred_index);
        assert(inferred->kind == N00B_ABI_CONSTRAINT_EXCLUDES
            && inferred->target_type_index == constraint->target_type_index);
    }
    assert(variable_count == 1);
}

static void
check_trait_methods(const core_object_section_t *metadata,
                    const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *methods = entry_for(entries,
        count, N00B_ABI_TABLE_TRAIT_METHOD);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries,
        count, N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *signatures = entry_for(entries,
        count, N00B_ABI_TABLE_CALL_SIGNATURE);
    const n00bb_abi_directory_entry_t *functions = entry_for(entries,
        count, N00B_ABI_TABLE_FUNCTION);
    assert(types && methods && methods->record_count == 3 && symbols
        && signatures && functions && functions->record_count == 1);
    uint32_t trait_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->kind == N00B_ABI_TYPE_TRAIT
            && type->owner_module_index == 1) {
            assert(!trait_index);
            trait_index = i;
        }
    }
    assert(trait_index);
    bool found_owner = false;
    for (uint32_t i = 1; i < symbols->record_count; i++) {
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols, i);
        if (symbol->type_index == trait_index
            && short_name_is(&symbol->name, "Readable")) {
            assert(symbol->kind == N00B_ABI_SYMBOL_TYPE);
            found_owner = true;
        }
    }
    assert(found_owner);
    const char *names[] = {"read", "reset"};
    for (uint32_t i = 0; i < 2; i++) {
        const n00b_abi_trait_method_v1 *method = row_at(metadata, methods,
            i + 1);
        assert(method->trait_type_index == trait_index
            && method->declaration_ordinal == i
            && method->signature_type_index < types->record_count
            && method->symbol_index && method->symbol_index
                < symbols->record_count);
        const n00b_abi_type_v1 *type = row_at(metadata, types,
            method->signature_type_index);
        assert(type->kind == N00B_ABI_TYPE_FUNCTION
            && type->call_signature_index
            && type->call_signature_index < signatures->record_count);
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
            method->symbol_index);
        assert(symbol->kind == N00B_ABI_SYMBOL_FUNCTION
            && symbol->type_index == method->signature_type_index
            && short_name_is(&symbol->name, names[i]));
    }
}

static void
check_trait_dispatch(const core_object_section_t *metadata,
                     const n00bb_abi_directory_entry_t *entries,
                     size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *constraints = entry_for(entries,
        count, N00B_ABI_TABLE_CONSTRAINT);
    const n00bb_abi_directory_entry_t *signatures = entry_for(entries,
        count, N00B_ABI_TABLE_CALL_SIGNATURE);
    assert(types && constraints && signatures);
    bool found = false;
    for (uint32_t i = 1; i < constraints->record_count; i++) {
        const n00b_abi_constraint_v1 *constraint = row_at(metadata,
            constraints, i);
        if (constraint->kind != N00B_ABI_CONSTRAINT_METHOD) continue;
        assert(constraint->call_type_index
            && constraint->call_type_index < types->record_count);
        const n00b_abi_type_v1 *call = row_at(metadata, types,
            constraint->call_type_index);
        assert(call->kind == N00B_ABI_TYPE_FUNCTION
            && call->call_signature_index
            && call->call_signature_index < signatures->record_count);
        const n00b_abi_call_signature_v1 *signature = row_at(metadata,
            signatures, call->call_signature_index);
        assert(signature->failure_type_index < types->record_count);
        found = true;
    }
    assert(found);
}

static void
check_callbacks(const core_object_section_t *metadata,
                const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    const n00bb_abi_directory_entry_t *signatures = entry_for(entries, count,
        N00B_ABI_TABLE_CALL_SIGNATURE);
    const n00bb_abi_directory_entry_t *parameters = entry_for(entries, count,
        N00B_ABI_TABLE_PARAMETER);
    const n00bb_abi_directory_entry_t *functions = entry_for(entries, count,
        N00B_ABI_TABLE_FUNCTION);
    assert(types && symbols && stream && signatures && parameters
        && functions && functions->record_count == 1);
    const char *names[] = {"Event_Call", "State_Call", "Transform_Call"};
    const uint32_t role_counts[] = {2, 3, 2};
    const uint32_t arg_counts[][3] = {
        {1, 1, 0}, {1, 2, 2}, {1, 1, 0},
    };
    for (size_t kind = 0; kind < 3; kind++) {
        const n00b_abi_type_v1 *callback = nullptr;
        uint32_t generic_argument = 0, generic_success = 0,
                 generic_failure = 0;
        for (uint32_t i = 1; i < symbols->record_count; i++) {
            const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols, i);
            if (symbol->kind == N00B_ABI_SYMBOL_TYPE
                && short_name_is(&symbol->name, names[kind])) {
                assert(!callback && symbol->type_index < types->record_count);
                callback = row_at(metadata, types, symbol->type_index);
            }
        }
        assert(callback && callback->kind == N00B_ABI_TYPE_CALLBACK
            && callback->components.count == role_counts[kind]
            && callback->components.first < stream->record_count
            && callback->components.count <= stream->record_count
                - callback->components.first);
        for (uint32_t role = 0; role < role_counts[kind]; role++) {
            uint32_t type_index = *(const uint32_t *)row_at(metadata, stream,
                callback->components.first + role);
            assert(type_index && type_index < types->record_count);
            const n00b_abi_type_v1 *type = row_at(metadata, types, type_index);
            assert(type->kind == N00B_ABI_TYPE_FUNCTION
                && type->call_signature_index
                && type->call_signature_index < signatures->record_count);
            const n00b_abi_call_signature_v1 *signature = row_at(metadata,
                signatures, type->call_signature_index);
            assert(signature->positional_parameters.count
                    == arg_counts[kind][role]);
            if (kind != 2) assert(signature->failure_type_index == 0);
            if (kind == 1 && role == 1) {
                assert(signature->positional_parameters.first
                    < stream->record_count);
                uint32_t parameter_index = *(const uint32_t *)row_at(metadata,
                    stream, signature->positional_parameters.first);
                assert(parameter_index && parameter_index
                    < parameters->record_count);
                const n00b_abi_parameter_v1 *parameter = row_at(metadata,
                    parameters, parameter_index);
                assert(parameter->passing_mode == N00B_ABI_PASS_MODIFY);
            }
            if (kind == 2) {
                assert(signature->failure_type_index
                    && signature->failure_type_index < types->record_count
                    && signature->success_type_index
                    && signature->success_type_index < types->record_count
                    && signature->positional_parameters.first
                        < stream->record_count);
                uint32_t parameter_index = *(const uint32_t *)row_at(metadata,
                    stream, signature->positional_parameters.first);
                assert(parameter_index && parameter_index
                    < parameters->record_count);
                const n00b_abi_parameter_v1 *parameter = row_at(metadata,
                    parameters, parameter_index);
                assert(parameter->type_index
                    && parameter->type_index < types->record_count);
                if (!role) {
                    generic_argument = parameter->type_index;
                    generic_success = signature->success_type_index;
                    generic_failure = signature->failure_type_index;
                }
                else assert(parameter->type_index == generic_argument
                    && signature->success_type_index == generic_success
                    && signature->failure_type_index == generic_failure);
            }
        }
        if (kind == 2) {
            uint32_t variables[] = {
                generic_argument, generic_success, generic_failure,
            };
            for (size_t i = 0; i < 3; i++) {
                const n00b_abi_type_v1 *variable = row_at(metadata, types,
                    variables[i]);
                assert(variable->kind == N00B_ABI_TYPE_VARIABLE
                    && variable->typevar_owner_scope_index);
            }
            assert(generic_argument != generic_success
                && generic_argument != generic_failure
                && generic_success != generic_failure);
        }
    }
}

static void
check_storage(const uint8_t *object, size_t length,
              const core_object_section_t *metadata,
              const n00bb_abi_directory_entry_t *entries, size_t count,
              uint32_t target_id)
{
    const n00bb_abi_directory_entry_t *modules = entry_for(entries, count,
        N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *storage = entry_for(entries, count,
        N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(modules && modules->record_count == 2 && storage
        && storage->record_count == 4 && symbols && stream);
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const char *names[] = {"fixed_value", "shared_value", "thread_seed"};
    const uint32_t kinds[] = {N00B_ABI_STORAGE_READ_ONLY,
        N00B_ABI_STORAGE_WRITABLE, N00B_ABI_STORAGE_TLS};
    const uint64_t values[] = {42, 3, 7};
    const char *elf_names[] = {".rodata.n00b_static", ".data.n00b_static",
        ".tdata.n00b_static"};
    const char *mach_names[] = {"__n00b_ro", "__n00b_data", "__n00b_tls"};
    const char *coff_names[] = {".rdata$N", ".data$N", ".tls$N"};
    bool row_seen[4] = {0}, name_seen[3] = {0};
    for (uint32_t i = 1; i < storage->record_count; i++) {
        const n00b_abi_storage_v1 *row = row_at(metadata, storage, i);
        assert(row->module_index == 1 && row->symbol_index
            && row->symbol_index < symbols->record_count
            && row->byte_length == 8 && row->segment_kind >= 1
            && row->segment_kind <= 3);
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
            row->symbol_index);
        assert(symbol->type_index == row->type_index);
        size_t expected = 0;
        while (expected < 3 && !short_name_is(&symbol->name, names[expected]))
            expected++;
        assert(expected < 3 && row->segment_kind == kinds[expected]
            && !name_seen[expected]);
        name_seen[expected] = true;
        row_seen[i] = true;
        const char *name = format == CORE_TARGET_OBJECT_MACH_O
            ? mach_names[expected] : format == CORE_TARGET_OBJECT_COFF
                ? coff_names[expected] : elf_names[expected];
        const char *segment = format != CORE_TARGET_OBJECT_MACH_O
            ? NULL : expected ? "__DATA" : "__DATA_CONST";
        core_object_section_t section;
        assert(core_object_section_find(object, length, format, segment,
            name, &section));
        assert(row->segment_index == section.native_index
            && row->byte_offset <= section.size
            && row->byte_length <= section.size - row->byte_offset
            && core_le_load_u64(section.bytes + row->byte_offset)
                == values[expected]);
        if (expected == 2 && format == CORE_TARGET_OBJECT_MACH_O)
            assert((section.flags & 0xff) == 0x11);
        if (expected == 2 && format == CORE_TARGET_OBJECT_ELF)
            assert(section.flags & 0x400);
    }
    assert(name_seen[0] && name_seen[1] && name_seen[2]);
    const n00b_abi_module_v1 *module = row_at(metadata, modules, 1);
    assert(module->static_storage.count == 2
        && module->tls_storage.count == 1);
    n00b_abi_range_v1 ranges[] = {
        module->static_storage, module->tls_storage,
    };
    for (size_t r = 0; r < 2; r++) {
        assert(ranges[r].first < stream->record_count
            && ranges[r].count <= stream->record_count - ranges[r].first);
        for (uint32_t j = 0; j < ranges[r].count; j++) {
            uint32_t index = *(const uint32_t *)row_at(metadata, stream,
                ranges[r].first + j);
            assert(index && index < storage->record_count && row_seen[index]);
            const n00b_abi_storage_v1 *row = row_at(metadata, storage, index);
            assert((row->segment_kind == N00B_ABI_STORAGE_TLS) == (r == 1));
        }
    }
}

int
main(int argc, char **argv)
{
    bool cycle_mode = argc >= 3 && !strcmp(argv[2], "cycle");
    assert(argc == 2 || cycle_mode || (argc == 3
        && (!strcmp(argv[2], "enum") || !strcmp(argv[2], "function")
            || !strcmp(argv[2], "import")
            || !strcmp(argv[2], "import-record")
            || !strcmp(argv[2], "import-enum")
            || !strcmp(argv[2], "variant")
            || !strcmp(argv[2], "constraint")
            || !strcmp(argv[2], "trait-methods")
            || !strcmp(argv[2], "trait-dispatch")
            || !strcmp(argv[2], "callback")
            || !strcmp(argv[2], "storage")
            || !strcmp(argv[2], "generic")
            || !strcmp(argv[2], "generic-local"))));
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0);
    assert(!fseek(file, 0, SEEK_SET));
    uint8_t *object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));

    core_object_section_t metadata = {0};
    assert(find_metadata(object, (size_t)length, &metadata)
        && metadata.size >= N00BB_ABI_HEADER_BYTES);
    n00bb_abi_image_key_t key = {
        .target_id = core_le_load_u32(metadata.bytes + 16),
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .cpu_profile = core_le_load_u64(metadata.bytes + 40),
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t *entries = calloc(schema_count,
        sizeof(*entries));
    assert(entries);
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
        schemas, schema_count, false, &header, entries,
        schema_count));
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *fields = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_FIELD);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *runs = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_PHYSICAL_RUN);
    const n00bb_abi_directory_entry_t *locations_table = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_SOURCE_LOCATION);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_U32_STREAM);
    assert(types && fields && symbols && layouts && runs && locations_table
        && stream);

    if (cycle_mode) {
        size_t member_count = (size_t)argc - 2;
        bool *seen = calloc(member_count, sizeof(*seen));
        assert(seen);
        cycle_fact_t first = cycle_fact(&metadata, entries,
                                        header.directory_count);
        assert(first.ordinal < member_count);
        seen[first.ordinal] = true;
        for (int i = 3; i < argc; i++) {
            cycle_fact_t member = cycle_fact_from_file(argv[i]);
            assert(member.interface_identity == first.interface_identity
                && member.freshness_identity == first.freshness_identity
                && member.ordinal < member_count && !seen[member.ordinal]);
            seen[member.ordinal] = true;
        }
        for (size_t i = 0; i < member_count; i++) assert(seen[i]);
        free(seen);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "function")) {
        check_function(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "constraint")) {
        check_constraint(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "trait-methods")) {
        check_trait_methods(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "trait-dispatch")) {
        check_trait_dispatch(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "callback")) {
        check_callbacks(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "storage")) {
        check_storage(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "import")) {
        check_import(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "import-record")) {
        check_imported_record(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "import-enum")) {
        check_imported_enum(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "variant")) {
        check_variant(&metadata, entries, header.directory_count);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "generic")) {
        check_generic(&metadata, entries, header.directory_count,
            "assign_same", "item", nullptr, N00B_ABI_PASS_MODIFY);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[2], "generic-local")) {
        check_generic(&metadata, entries, header.directory_count,
            "hold_same", "item", "saved", N00B_ABI_PASS_IN);
        free(entries);
        free(object);
        return 0;
    }

    if (argc == 3) {
        const n00bb_abi_directory_entry_t *items = entry_for(entries,
            header.directory_count, N00B_ABI_TABLE_ENUM_ITEM);
        assert(items && items->record_count == 5);
        uint32_t zero_index = 0, signed_index = 0;
        for (uint32_t i = 1; i < types->record_count; i++) {
            const n00b_abi_type_v1 *type = row_at(&metadata, types, i);
            if (short_name_is(&type->canonical_name, "enum.Zero"))
                zero_index = i;
            if (short_name_is(&type->canonical_name, "enum.Signed"))
                signed_index = i;
        }
        assert(zero_index && signed_index && zero_index != signed_index);
        const n00b_abi_type_v1 *zero = row_at(&metadata, types, zero_index);
        const n00b_abi_type_v1 *signed_type = row_at(&metadata, types,
                                                     signed_index);
        assert(zero->kind == N00B_ABI_TYPE_ENUM
            && zero->owner_module_index == 1
            && zero->semantic_width_bits == 1
            && zero->unpacked_storage_bits == 8
            && zero->fields_or_arms.count == 1
            && zero->layout_index < layouts->record_count
            && !(zero->flags & N00B_ABI_TYPE_FLAG_SIGNED));
        assert(signed_type->kind == N00B_ABI_TYPE_ENUM
            && signed_type->owner_module_index == 1
            && signed_type->semantic_width_bits == 4
            && signed_type->unpacked_storage_bits == 8
            && signed_type->fields_or_arms.count == 3
            && signed_type->layout_index < layouts->record_count
            && (signed_type->flags & N00B_ABI_TYPE_FLAG_SIGNED));
        check_enum_item(&metadata, stream, items, symbols, locations_table,
            zero, zero_index, 0, "only", 0, 0);
        check_enum_item(&metadata, stream, items, symbols, locations_table,
            signed_type, signed_index, 0, "negative", UINT64_MAX - 6,
            UINT64_MAX);
        check_enum_item(&metadata, stream, items, symbols, locations_table,
            signed_type, signed_index, 1, "following", UINT64_MAX - 5,
            UINT64_MAX);
        check_enum_item(&metadata, stream, items, symbols, locations_table,
            signed_type, signed_index, 2, "positive", 7, 0);
        free(entries);
        free(object);
        return 0;
    }

    uint32_t record_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(&metadata, types, i);
        if (type->kind == N00B_ABI_TYPE_RECORD
            && type->owner_module_index == 1
            && type->fields_or_arms.count == 1) {
            assert(!record_index);
            record_index = i;
        }
    }
    assert(record_index);
    const n00b_abi_type_v1 *record = row_at(&metadata, types, record_index);
    assert(record->layout_index && record->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(&metadata, layouts,
                                               record->layout_index);
    assert(layout->type_index == record_index);
    assert(record->fields_or_arms.first < stream->record_count);
    uint32_t field_index = *(const uint32_t *)row_at(&metadata, stream,
                                                     record->fields_or_arms.first);
    assert(field_index && field_index < fields->record_count);
    const n00b_abi_field_v1 *field = row_at(&metadata, fields, field_index);
    assert(short_name_is(&field->name, "reading"));
    assert(field->parent_type_index == record_index);
    assert(field->child_type_index < types->record_count);
    assert(field->physical_runs.count);
    assert(field->physical_runs.first < stream->record_count);
    uint32_t run_index = *(const uint32_t *)row_at(&metadata, stream,
                                                   field->physical_runs.first);
    assert(run_index && run_index < runs->record_count);
    const n00b_abi_physical_run_v1 *run = row_at(&metadata, runs, run_index);
    assert(run->width_bits > 0);
    assert(field->symbol_index && field->symbol_index < symbols->record_count);
    const n00b_abi_symbol_v1 *field_symbol = row_at(&metadata, symbols,
                                                      field->symbol_index);
    assert(short_name_is(&field_symbol->name, "reading"));
    assert(field_symbol->type_index == field->child_type_index);
    assert(field_symbol->declaration_location_index
        && field_symbol->declaration_location_index
            < locations_table->record_count);
    const n00b_abi_source_location_v1 *location = row_at(&metadata,
        locations_table, field_symbol->declaration_location_index);
    assert(location->file_index == 1 && location->start_line > 0);

    free(entries);
    free(object);
    return 0;
}
