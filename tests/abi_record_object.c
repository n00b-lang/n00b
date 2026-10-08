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
check_flagset(const core_object_section_t *metadata,
              const n00bb_abi_directory_entry_t *entries, size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *items = entry_for(entries, count,
        N00B_ABI_TABLE_ENUM_ITEM);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *locations = entry_for(entries, count,
        N00B_ABI_TABLE_SOURCE_LOCATION);
    assert(types && items && stream && symbols && locations);
    assert(items->record_count == 5);

    uint32_t type_index = 0;
    for (uint32_t i = 1; i < symbols->record_count; i++) {
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols, i);
        if (symbol->kind == N00B_ABI_SYMBOL_TYPE
            && short_name_is(&symbol->name, "FilePerms")) {
            assert(!type_index);
            type_index = symbol->type_index;
        }
    }
    assert(type_index && type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types, type_index);
    assert(type->kind == N00B_ABI_TYPE_FLAGSET);
    assert(type->owner_module_index == 1);
    assert(type->semantic_width_bits == 2);
    assert(type->unpacked_storage_bits == 8);
    assert(type->fields_or_arms.count == 4);
    assert(!(type->flags & N00B_ABI_TYPE_FLAG_SIGNED));

    const char *names[] = {"read", "write", "read_write", "none"};
    const uint64_t masks[] = {1, 2, 3, 0};
    for (uint32_t i = 0; i < 4; i++) {
        check_enum_item(metadata, stream, items, symbols, locations,
            type, type_index, i, names[i], masks[i], 0);
        uint32_t position = type->fields_or_arms.first + i;
        uint32_t item_index = *(const uint32_t *)row_at(metadata, stream,
                                                         position);
        const n00b_abi_enum_item_v1 *item = row_at(metadata, items,
                                                   item_index);
        assert(item->flags == (i < 2 ? N00B_ABI_ENUM_ITEM_FLAG_PRIMITIVE : 0));
    }
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
        && !function->keywords.count
        && function->variadic_type_index == N00B_ABI_V1_NO_TYPE_INDEX
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
    assert(types && requirements && requirements->record_count == 3);
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
    uint32_t imported_index = 0;
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
            imported_index = i;
        }
    }
    assert(local_records == 1 && imported_records == 1);
    const n00b_abi_requirement_v1 *type_requirement = row_at(metadata,
        requirements, 2);
    assert(type_requirement->symbol_kind == N00B_ABI_SYMBOL_TYPE
        && type_requirement->contract_type_index == imported_index
        && type_requirement->required_phase_mask
        && type_requirement->requesting_module_index == 1);
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
    assert(types && requirements && items && requirements->record_count == 3
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
    uint32_t imported_index = 0;
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
        imported_index = i;
    }
    assert(imported_enums == 1 && builtin_enums == 1);
    const n00b_abi_requirement_v1 *type_requirement = row_at(metadata,
        requirements, 2);
    assert(type_requirement->symbol_kind == N00B_ABI_SYMBOL_TYPE
        && type_requirement->contract_type_index == imported_index
        && type_requirement->required_phase_mask
        && type_requirement->requesting_module_index == 1);
    th_ctx_free(type_context);
}

static void
check_imported_flagset(const core_object_section_t *metadata,
                       const n00bb_abi_directory_entry_t *entries,
                       size_t count)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *requirements = entry_for(entries,
        count, N00B_ABI_TABLE_REQUIREMENT);
    const n00bb_abi_directory_entry_t *items = entry_for(entries, count,
        N00B_ABI_TABLE_ENUM_ITEM);
    assert(types && requirements && items && items->record_count == 1);
    th_ctx *type_context = th_ctx_new();
    assert(type_context);
    uint64_t hash = th_hash(type_context,
        th_name(type_context, "flagset_archive_dep.impl.FilePerms"));
    uint32_t type_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(metadata, types, i);
        if (type->typehash != hash) continue;
        assert(!type_index);
        assert(type->kind == N00B_ABI_TYPE_FLAGSET
            && !type->owner_module_index && !type->layout_index
            && !type->fields_or_arms.count
            && type->semantic_width_bits == 2);
        type_index = i;
    }
    assert(type_index);
    bool found = false;
    for (uint32_t i = 1; i < requirements->record_count; i++) {
        const n00b_abi_requirement_v1 *requirement = row_at(metadata,
            requirements, i);
        if (requirement->symbol_kind != N00B_ABI_SYMBOL_TYPE
            || requirement->contract_type_index != type_index) continue;
        assert(!found && requirement->required_phase_mask
            && requirement->requesting_module_index == 1);
        found = true;
    }
    assert(found);
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
    const n00bb_abi_directory_entry_t *bindings = entry_for(entries, count,
        N00B_ABI_TABLE_NATIVE_BINDING);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(modules && modules->record_count == 2 && storage
        && storage->record_count == 4 && symbols && bindings
        && bindings->record_count == 4 && stream);
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
    const char *native_names[] = {"$storage$fixed_value",
        "_$storage$shared_value", "_$storage$thread_seed"};
    const char *mach_native_names[] = {"_$storage$fixed_value",
        "__$storage$shared_value", "__$storage$thread_seed"};
    core_object_section_t pool;
    assert(core_object_section_find(object, length, format,
        format == CORE_TARGET_OBJECT_MACH_O ? "__TEXT" : NULL,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_strings"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$S"
        : ".rodata.n00b_strings", &pool));
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
        assert(symbol->native_binding_index == i
            && !!(symbol->flags & N00B_ABI_SYMBOL_FLAG_EXPORTED)
                == (expected == 0));
        const n00b_abi_native_binding_v1 *binding = row_at(metadata,
            bindings, symbol->native_binding_index);
        const char *expected_native = format == CORE_TARGET_OBJECT_MACH_O
            ? mach_native_names[expected] : native_names[expected];
        size_t native_length = strlen(expected_native);
        size_t pool_offset = 0;
        while (pool_offset <= pool.size
            && native_length <= pool.size - pool_offset
            && memcmp(pool.bytes + pool_offset, expected_native,
                      native_length)) pool_offset++;
        assert(pool_offset <= pool.size
            && native_length <= pool.size - pool_offset);
        assert(binding->kind == N00B_ABI_NATIVE_SECTION_OFFSET
            && binding->section_index == row->segment_index
            && binding->offset == (int64_t)row->byte_offset
            && binding->native_name.long_value.selector
                == ((uint64_t)native_length
                    | ((uint64_t)N00B_ABI_STRING_UTF8
                        << N00B_ABI_STRING_ENCODING_SHIFT)
                    | ((uint64_t)N00B_ABI_STRING_LONG
                        << N00B_ABI_STRING_TAG_SHIFT)));
        uint64_t pointer_offset = bindings->file_offset
            + (uint64_t)symbol->native_binding_index * bindings->record_bytes
            + offsetof(n00b_abi_native_binding_v1, native_name);
        bool found_name = false;
        for (uint32_t r = 0; r < metadata->relocation_count; r++) {
            core_object_absolute64_relocation_t relocation;
            assert(core_object_absolute64_relocation_at(metadata, format,
                r, &relocation));
            if (relocation.section_offset != pointer_offset) continue;
            if (format == CORE_TARGET_OBJECT_MACH_O)
                assert(relocation.target_is_section
                    && relocation.native_symbol_index == pool.native_index);
            else assert(relocation.addend_bits == pool_offset);
            found_name = true;
        }
        assert(found_name);
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

static void
check_string_storage(const uint8_t *object, size_t length,
                     const core_object_section_t *metadata,
                     const n00bb_abi_directory_entry_t *entries,
                     size_t count, uint32_t target_id)
{
    const n00bb_abi_directory_entry_t *storage = entry_for(entries, count,
        N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *tls_relocations = entry_for(entries,
        count, N00B_ABI_TABLE_TLS_RELOCATION);
    const n00bb_abi_directory_entry_t *indexes = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(storage && storage->record_count == 10 && symbols
        && tls_relocations && tls_relocations->record_count == 3 && indexes);
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    core_object_section_t pool;
    assert(core_object_section_find(object, length, format,
        format == CORE_TARGET_OBJECT_MACH_O ? "__TEXT" : NULL,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_strings"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$S"
        : ".rodata.n00b_strings", &pool));
    const char *long_text = "a longer immutable string value";
    const char *other_text = "a different long TLS string value";
    size_t long_length = strlen(long_text);
    size_t backing_matches = 0;
    for (size_t i = 0; i <= pool.size
         && long_length <= pool.size - i; i++)
        backing_matches += !memcmp(pool.bytes + i, long_text, long_length);
    assert(backing_matches == 1);

    uint64_t long_addends[7] = {0};
    size_t long_count = 0;
    uint32_t pool_storage_indexes[2] = {0};
    uint32_t tls_storage_indexes[2] = {0};
    bool seen[7] = {0};
    const char *names[] = {"short_label", "long_label", "repeated_label",
                           "thread_label", "empty_label",
                           "tls_long_label", "tls_other"};
    for (uint32_t i = 1; i < storage->record_count; i++) {
        const n00b_abi_storage_v1 *row = row_at(metadata, storage, i);
        if (!row->symbol_index) {
            bool primary = row->byte_length == long_length
                && row->byte_offset <= pool.size
                && row->byte_length <= pool.size - row->byte_offset
                && !memcmp(pool.bytes + row->byte_offset,
                    long_text, long_length);
            bool other = row->byte_length == strlen(other_text)
                && row->byte_offset <= pool.size
                && row->byte_length <= pool.size - row->byte_offset
                && !memcmp(pool.bytes + row->byte_offset,
                    other_text, strlen(other_text));
            assert(primary != other && row->module_index == 1
                && row->segment_kind == N00B_ABI_STORAGE_READ_ONLY
                && row->segment_index == pool.native_index);
            size_t which_pool = other ? 1 : 0;
            assert(!pool_storage_indexes[which_pool]);
            pool_storage_indexes[which_pool] = i;
            continue;
        }
        assert(row->module_index == 1 && row->symbol_index
            && row->symbol_index < symbols->record_count
            && row->byte_length == sizeof(n00b_abi_string_v1));
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
            row->symbol_index);
        size_t kind = 0;
        while (kind < 7 && !short_name_is(&symbol->name, names[kind]))
            kind++;
        assert(kind < 7 && !seen[kind] && symbol->type_index == row->type_index);
        seen[kind] = true;
        bool tls = kind == 3 || kind == 5 || kind == 6;
        if (kind >= 5) tls_storage_indexes[kind - 5] = i;
        assert(row->segment_kind == (tls ? N00B_ABI_STORAGE_TLS
            : N00B_ABI_STORAGE_READ_ONLY));
        core_object_section_t section;
        assert(core_object_section_find(object, length, format,
            format == CORE_TARGET_OBJECT_MACH_O
                ? (tls ? "__DATA" : "__DATA_CONST") : NULL,
            format == CORE_TARGET_OBJECT_MACH_O
                ? (tls ? "__n00b_tls" : "__n00b_ro")
                : format == CORE_TARGET_OBJECT_COFF
                    ? (tls ? ".tls$N" : ".rdata$N")
                    : (tls ? ".tdata.n00b_static"
                           : ".rodata.n00b_static"), &section));
        assert(row->segment_index == section.native_index
            && row->byte_offset <= section.size
            && row->byte_length <= section.size - row->byte_offset);
        const n00b_abi_string_v1 *value =
            (const void *)(section.bytes + row->byte_offset);
        if (kind == 0 || kind == 3 || kind == 4) {
            const char *text = kind == 0 ? "hello" : kind == 3 ? "worker" : "";
            size_t text_length = strlen(text);
            assert(value->short_value.selector ==
                    (text_length | (N00B_ABI_STRING_ASCII << 4))
                && !memcmp(value->short_value.bytes, text, text_length));
            continue;
        }
        size_t expected_length = kind == 6 ? strlen(other_text) : long_length;
        assert(value->long_value.selector ==
            ((uint64_t)expected_length
                | ((uint64_t)N00B_ABI_STRING_ASCII
                    << N00B_ABI_STRING_ENCODING_SHIFT)
                | ((uint64_t)N00B_ABI_STRING_LONG
                    << N00B_ABI_STRING_TAG_SHIFT)));
        bool found_relocation = false;
        for (uint32_t r = 0; r < section.relocation_count; r++) {
            core_object_absolute64_relocation_t relocation;
            assert(core_object_absolute64_relocation_at(&section, format, r,
                &relocation));
            if (relocation.section_offset != row->byte_offset) continue;
            if (format == CORE_TARGET_OBJECT_MACH_O)
                assert(relocation.target_is_section
                    && relocation.native_symbol_index == pool.native_index);
            else {
                core_object_symbol_name_t target;
                assert(core_object_symbol_name_at(object, length, format,
                    &section, relocation.native_symbol_index, &target)
                    && target.length == strlen("n00b_string_pool")
                    && !memcmp(target.bytes, "n00b_string_pool",
                               target.length));
            }
            assert(!long_addends[kind]);
            long_addends[kind] = relocation.addend_bits;
            long_count++;
            found_relocation = true;
        }
        assert(found_relocation);
    }
    for (size_t i = 0; i < 7; i++) assert(seen[i]);
    assert(long_count == 4 && long_addends[1] == long_addends[2]
        && long_addends[2] == long_addends[5]
        && long_addends[6] != long_addends[5]);
    for (size_t i = 0; i < 2; i++) {
        assert(pool_storage_indexes[i] && tls_storage_indexes[i]);
        const n00b_abi_storage_v1 *tls_row = row_at(metadata, storage,
            tls_storage_indexes[i]);
        const n00b_abi_storage_v1 *pool_row = row_at(metadata, storage,
            pool_storage_indexes[i]);
        assert(tls_row->tls_relocations.count == 1);
        uint32_t relocation_index = *(const uint32_t *)row_at(metadata,
            indexes, tls_row->tls_relocations.first);
        const n00b_abi_tls_relocation_v1 *relocation = row_at(metadata,
            tls_relocations, relocation_index);
        assert(relocation->template_storage_index == tls_storage_indexes[i]
            && relocation->referenced_storage_index == pool_storage_indexes[i]
            && relocation->write_byte_offset == 0
            && relocation->addend_bytes == 0
            && pool_row->byte_offset == long_addends[i ? 6 : 5]
                - (format == CORE_TARGET_OBJECT_MACH_O
                    ? pool.virtual_address : 0)
            && relocation->target_kind == N00B_ABI_STORAGE_READ_ONLY);
    }
}

static void
check_array_storage(const uint8_t *object, size_t length,
                    const core_object_section_t *metadata,
                    const n00bb_abi_directory_entry_t *entries,
                    size_t count, uint32_t target_id)
{
    const n00bb_abi_directory_entry_t *storage = entry_for(entries, count,
        N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_directory_entry_t *shapes = entry_for(entries, count,
        N00B_ABI_TABLE_SHAPE);
    const n00bb_abi_directory_entry_t *extents = entry_for(entries, count,
        N00B_ABI_TABLE_U64_STREAM);
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    assert(storage && storage->record_count == 2
        && shapes && shapes->record_count == 2
        && extents && extents->record_count == 1 && types);
    const n00b_abi_storage_v1 *row = row_at(metadata, storage, 1);
    const n00b_abi_shape_v1 *shape = row_at(metadata, shapes,
        row->instance_shape_index);
    const n00b_abi_type_v1 *type = row_at(metadata, types,
        row->type_index);
    assert(row->instance_shape_index == 1 && row->byte_length == 16
        && row->segment_kind == N00B_ABI_STORAGE_READ_ONLY
        && type->kind == N00B_ABI_TYPE_ARRAY
        && shape->rank == 1 && shape->flat_rank == 1
        && shape->total_count == 2
        && shape->extents.first == 0 && shape->extents.count == 1
        && *(const uint64_t *)row_at(metadata, extents, 0) == 2);
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const char *segment = format == CORE_TARGET_OBJECT_MACH_O
        ? "__DATA_CONST" : nullptr;
    core_object_section_t backing, value;
    assert(core_object_section_find(object, length, format, segment,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
        : ".rodata.n00b_aggregate", &backing));
    assert(core_object_section_find(object, length, format, segment,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_ro"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$N"
        : ".rodata.n00b_static", &value));
    assert(backing.size == 48 && value.size == 16
        && row->segment_index == value.native_index
        && core_le_load_u64(backing.bytes) == 1
        && core_le_load_u64(backing.bytes + 8) == 2
        && core_le_load_u64(backing.bytes + 16) == 2
        && core_le_load_u32(backing.bytes + 24) == 1
        && core_le_load_u32(backing.bytes + 28) == 1
        && core_le_load_u64(backing.bytes + 40) == 2
        && value.relocation_count == 2);
    core_object_absolute64_relocation_t pointers[2];
    for (uint32_t i = 0; i < 2; i++) {
        assert(core_object_absolute64_relocation_at(&value, format, i,
            &pointers[i]));
        if (format == CORE_TARGET_OBJECT_MACH_O)
            assert(pointers[i].target_is_section
                && pointers[i].native_symbol_index == backing.native_index);
        else {
            core_object_symbol_name_t target;
            assert(core_object_symbol_name_at(object, length, format,
                &value, pointers[i].native_symbol_index, &target)
                && target.length == strlen("n00b_aggregate_pool")
                && !memcmp(target.bytes, "n00b_aggregate_pool",
                           target.length));
        }
    }
    if (pointers[0].section_offset > pointers[1].section_offset) {
        core_object_absolute64_relocation_t swap = pointers[0];
        pointers[0] = pointers[1];
        pointers[1] = swap;
    }
    assert(pointers[0].section_offset == 0
        && pointers[1].section_offset == 8
        && pointers[1].addend_bits - pointers[0].addend_bits == 16);
}

static uint64_t
aggregate_pointer_addend(const uint8_t *object, size_t length,
                         core_target_object_format_t format,
                         const core_object_section_t *source,
                         const core_object_section_t *backing,
                         uint64_t offset)
{
    for (uint32_t i = 0; i < source->relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        assert(core_object_absolute64_relocation_at(source, format, i,
            &relocation));
        if (relocation.section_offset != offset) continue;
        if (format == CORE_TARGET_OBJECT_MACH_O)
            assert(relocation.target_is_section
                && relocation.native_symbol_index == backing->native_index);
        else {
            core_object_symbol_name_t target;
            assert(core_object_symbol_name_at(object, length, format,
                source, relocation.native_symbol_index, &target)
                && target.length == strlen("n00b_aggregate_pool")
                && !memcmp(target.bytes, "n00b_aggregate_pool",
                           target.length));
        }
        assert(format != CORE_TARGET_OBJECT_MACH_O
            || relocation.addend_bits >= backing->virtual_address);
        return format == CORE_TARGET_OBJECT_MACH_O
            ? relocation.addend_bits - backing->virtual_address
            : relocation.addend_bits;
    }
    assert(false && "missing aggregate pointer relocation");
    return 0;
}

static void
check_nested_array_storage(const uint8_t *object, size_t length,
                           const core_object_section_t *metadata,
                           const n00bb_abi_directory_entry_t *entries,
                           size_t count, uint32_t target_id, bool ragged)
{
    const n00bb_abi_directory_entry_t *storage = entry_for(entries, count,
        N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_directory_entry_t *shapes = entry_for(entries, count,
        N00B_ABI_TABLE_SHAPE);
    const n00bb_abi_directory_entry_t *extents = entry_for(entries, count,
        N00B_ABI_TABLE_U64_STREAM);
    assert(storage && storage->record_count == 2
        && shapes && shapes->record_count == 2
        && extents && extents->record_count == 2);
    const n00b_abi_storage_v1 *row = row_at(metadata, storage, 1);
    const n00b_abi_shape_v1 *shape = row_at(metadata, shapes,
        row->instance_shape_index);
    assert(row->instance_shape_index == 1 && row->byte_length == 16
        && row->segment_kind == N00B_ABI_STORAGE_READ_ONLY
        && shape->rank == 2 && shape->flat_rank == (ragged ? 1u : 2u)
        && shape->total_count == (ragged ? 2u : 4u)
        && shape->extents.first == 0 && shape->extents.count == 2
        && *(const uint64_t *)row_at(metadata, extents, 0) == 2
        && *(const uint64_t *)row_at(metadata, extents, 1)
            == (ragged ? 0u : 2u));
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const char *segment = format == CORE_TARGET_OBJECT_MACH_O
        ? "__DATA_CONST" : nullptr;
    core_object_section_t backing, value;
    assert(core_object_section_find(object, length, format, segment,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
        : ".rodata.n00b_aggregate", &backing));
    assert(core_object_section_find(object, length, format, segment,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_ro"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$N"
        : ".rodata.n00b_static", &value));
    assert(row->segment_index == value.native_index
        && value.size == 16 && value.relocation_count == 2
        && backing.size == (ragged ? 160u : 72u)
        && backing.relocation_count == (ragged ? 4u : 0u));
    uint64_t base = aggregate_pointer_addend(object, length, format,
        &value, &backing, 0);
    assert(aggregate_pointer_addend(object, length, format,
        &value, &backing, 8) == base + 32);
    assert(core_le_load_u64(backing.bytes + 32)
            == (ragged ? 2u : 4u)
        && core_le_load_u32(backing.bytes + 40) == 2
        && core_le_load_u32(backing.bytes + 44)
            == (ragged ? 1u : 2u)
        && core_le_load_u64(backing.bytes + 56) == 2
        && core_le_load_u64(backing.bytes + 64)
            == (ragged ? 0u : 2u));
    if (!ragged) {
        for (uint64_t i = 0; i < 4; i++)
            assert(core_le_load_u64(backing.bytes + i * 8) == i + 1);
        return;
    }
    assert(aggregate_pointer_addend(object, length, format,
        &backing, &backing, 0) == base + 72
        && aggregate_pointer_addend(object, length, format,
            &backing, &backing, 8) == base + 88
        && aggregate_pointer_addend(object, length, format,
            &backing, &backing, 16) == base + 120
        && aggregate_pointer_addend(object, length, format,
            &backing, &backing, 24) == base + 128);
    assert(core_le_load_u64(backing.bytes + 72) == 1
        && core_le_load_u64(backing.bytes + 80) == 2
        && core_le_load_u64(backing.bytes + 88) == 2
        && core_le_load_u32(backing.bytes + 96) == 1
        && core_le_load_u32(backing.bytes + 100) == 1
        && core_le_load_u64(backing.bytes + 112) == 2
        && core_le_load_u64(backing.bytes + 120) == 3
        && core_le_load_u64(backing.bytes + 128) == 1
        && core_le_load_u32(backing.bytes + 136) == 1
        && core_le_load_u32(backing.bytes + 140) == 1
        && core_le_load_u64(backing.bytes + 152) == 1);
}

static const n00b_abi_storage_v1 *
storage_named(const core_object_section_t *metadata,
              const n00bb_abi_directory_entry_t *entries, size_t count,
              const char *name)
{
    const n00bb_abi_directory_entry_t *storages = entry_for(entries, count,
        N00B_ABI_TABLE_STORAGE);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries, count,
        N00B_ABI_TABLE_SYMBOL);
    assert(storages && symbols);
    for (uint32_t i = 1; i < storages->record_count; i++) {
        const n00b_abi_storage_v1 *storage = row_at(metadata, storages, i);
        assert(storage->symbol_index < symbols->record_count);
        const n00b_abi_symbol_v1 *symbol = row_at(metadata, symbols,
            storage->symbol_index);
        if (short_name_is(&symbol->name, name)) return storage;
    }
    assert(false && "missing record storage");
    return nullptr;
}

static const n00b_abi_field_v1 *
record_field_named(const core_object_section_t *metadata,
                   const n00bb_abi_directory_entry_t *entries, size_t count,
                   uint32_t type_index, const char *name)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *fields = entry_for(entries, count,
        N00B_ABI_TABLE_FIELD);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(types && fields && stream && type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types, type_index);
    assert((type->kind == N00B_ABI_TYPE_RECORD
            || type->kind == N00B_ABI_TYPE_TUPLE)
        && type->fields_or_arms.first <= stream->record_count
        && type->fields_or_arms.count
            <= stream->record_count - type->fields_or_arms.first);
    for (uint32_t i = 0; i < type->fields_or_arms.count; i++) {
        uint32_t field_index = core_le_load_u32(row_at(metadata, stream,
            type->fields_or_arms.first + i));
        assert(field_index && field_index < fields->record_count);
        const n00b_abi_field_v1 *field = row_at(metadata, fields,
            field_index);
        if (short_name_is(&field->name, name)) return field;
    }
    assert(false && "missing record field");
    return nullptr;
}

static const n00b_abi_trace_v1 *
trace_for_type(const core_object_section_t *metadata,
               const n00bb_abi_directory_entry_t *entries, size_t count,
               uint32_t type_index)
{
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *traces = entry_for(entries, count,
        N00B_ABI_TABLE_TRACE);
    assert(types && traces && type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types, type_index);
    assert(type->tracing_index && type->tracing_index < traces->record_count);
    const n00b_abi_trace_v1 *trace = row_at(metadata, traces,
        type->tracing_index);
    assert(trace->type_index == type_index);
    return trace;
}

static const n00b_abi_trace_child_v1 *
trace_child_at(const core_object_section_t *metadata,
               const n00bb_abi_directory_entry_t *entries, size_t count,
               n00b_abi_range_v1 range, uint32_t ordinal)
{
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    const n00bb_abi_directory_entry_t *children = entry_for(entries, count,
        N00B_ABI_TABLE_TRACE_CHILD);
    assert(stream && children && ordinal < range.count
        && range.first < stream->record_count
        && range.count <= stream->record_count - range.first);
    uint32_t index = core_le_load_u32(row_at(metadata, stream,
        range.first + ordinal));
    assert(index && index < children->record_count);
    return row_at(metadata, children, index);
}

static uint64_t
record_bits(const uint8_t *bytes, uint64_t offset, unsigned width)
{
    assert(width <= 64);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)((bytes[(offset + i) / 8]
            >> ((offset + i) % 8)) & 1u) << i;
    return value;
}

static void
check_record_string_relocation(const uint8_t *object, size_t length,
    core_target_object_format_t format, const core_object_section_t *source,
    uint64_t pointer_offset, const char *expected)
{
    core_object_section_t pool;
    assert(core_object_section_find(object, length, format,
        format == CORE_TARGET_OBJECT_MACH_O ? "__TEXT" : nullptr,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_strings"
        : format == CORE_TARGET_OBJECT_COFF ? ".rdata$S"
        : ".rodata.n00b_strings", &pool));
    size_t text_length = strlen(expected);
    bool found = false;
    for (uint32_t i = 0; i < source->relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        assert(core_object_absolute64_relocation_at(source, format, i,
            &relocation));
        if (relocation.section_offset != pointer_offset) continue;
        if (format == CORE_TARGET_OBJECT_MACH_O)
            assert(relocation.target_is_section
                && relocation.native_symbol_index == pool.native_index
                && relocation.addend_bits >= pool.virtual_address);
        else {
            core_object_symbol_name_t target;
            assert(core_object_symbol_name_at(object, length, format,
                source, relocation.native_symbol_index, &target)
                && target.length == strlen("n00b_string_pool")
                && !memcmp(target.bytes, "n00b_string_pool",
                    target.length));
        }
        uint64_t offset = format == CORE_TARGET_OBJECT_MACH_O
            ? relocation.addend_bits - pool.virtual_address
            : relocation.addend_bits;
        assert(offset <= pool.size
            && text_length <= pool.size - offset
            && !memcmp(pool.bytes + offset, expected, text_length));
        found = true;
    }
    assert(found);
}

static void
check_zero_element_arrays(const uint8_t *object, size_t length,
    const core_object_section_t *metadata,
    const n00bb_abi_directory_entry_t *entries, size_t count,
    uint32_t target_id)
{
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *shapes = entry_for(entries, count,
        N00B_ABI_TABLE_SHAPE);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    const n00bb_abi_directory_entry_t *extents = entry_for(entries, count,
        N00B_ABI_TABLE_U64_STREAM);
    core_object_section_t backing;
    assert(types && shapes && stream && extents
        && core_object_section_find(object, length, format,
            format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : nullptr,
            format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
            : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
            : ".rodata.n00b_aggregate", &backing));
    const char *names[] = {"elements", "nothing"};
    for (size_t i = 0; i < sizeof names / sizeof(*names); i++) {
        const n00b_abi_storage_v1 *storage = storage_named(metadata,
            entries, count, names[i]);
        const n00b_abi_type_v1 *type = row_at(metadata, types,
            storage->type_index);
        const n00b_abi_shape_v1 *shape = row_at(metadata, shapes,
            storage->instance_shape_index);
        assert(storage->segment_kind == N00B_ABI_STORAGE_READ_ONLY
            && storage->byte_length == 16
            && type->kind == N00B_ABI_TYPE_ARRAY
            && type->components.count == 1
            && type->components.first < stream->record_count
            && shape->rank == 1 && shape->flat_rank == 1
            && shape->total_count == 0 && shape->extents.count == 1
            && shape->extents.first < extents->record_count
            && core_le_load_u64(row_at(metadata, extents,
                shape->extents.first)) == 0);
        uint32_t child_index = core_le_load_u32(row_at(metadata, stream,
            type->components.first));
        const n00b_abi_type_v1 *child = row_at(metadata, types,
            child_index);
        assert(child->size_bits == 0 && child->alignment_bytes == 1);
        core_object_section_t source;
        assert(core_object_section_at(object, length, format,
            storage->segment_index, &source));
        uint64_t data = aggregate_pointer_addend(object, length, format,
            &source, &backing, storage->byte_offset);
        uint64_t descriptor = aggregate_pointer_addend(object, length,
            format, &source, &backing, storage->byte_offset + 8);
        assert(data < backing.size && descriptor <= backing.size - 32
            && core_le_load_u64(backing.bytes + descriptor) == 0
            && core_le_load_u32(backing.bytes + descriptor + 8) == 1
            && core_le_load_u32(backing.bytes + descriptor + 12) == 1
            && core_le_load_u64(backing.bytes + descriptor + 24) == 0);
    }
}

static void
check_record_aggregate_storage(const uint8_t *object, size_t length,
    const core_object_section_t *metadata,
    const n00bb_abi_directory_entry_t *entries, size_t count,
    uint32_t target_id, const char *mode)
{
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const n00b_abi_storage_v1 *storage = storage_named(metadata, entries,
        count, !strcmp(mode, "record-bits-storage") ? "flags"
            : !strcmp(mode, "record-nested-storage") ? "group" : "point");
    core_object_section_t section;
    assert(storage->segment_kind == N00B_ABI_STORAGE_READ_ONLY
        && core_object_section_at(object, length, format,
            storage->segment_index, &section)
        && storage->byte_offset <= section.size
        && storage->byte_length <= section.size - storage->byte_offset);
    const uint8_t *bytes = section.bytes + storage->byte_offset;
    if (!strcmp(mode, "record-bits-storage")) {
        const n00b_abi_field_v1 *first = record_field_named(metadata,
            entries, count, storage->type_index, "first");
        const n00b_abi_field_v1 *second = record_field_named(metadata,
            entries, count, storage->type_index, "second");
        const n00b_abi_field_v1 *code = record_field_named(metadata,
            entries, count, storage->type_index, "code");
        assert(storage->byte_length == 1
            && record_bits(bytes, first->logical_bit_offset, 1) == 1
            && record_bits(bytes, second->logical_bit_offset, 1) == 0
            && record_bits(bytes, code->logical_bit_offset, 3) == 5);
        return;
    }
    if (!strcmp(mode, "record-nested-storage")) {
        const n00b_abi_field_v1 *lead = record_field_named(metadata,
            entries, count, storage->type_index, "lead");
        const n00b_abi_field_v1 *members = record_field_named(metadata,
            entries, count, storage->type_index, "members");
        const n00b_abi_field_v1 *flag = record_field_named(metadata,
            entries, count, lead->child_type_index, "flag");
        const n00b_abi_field_v1 *name = record_field_named(metadata,
            entries, count, lead->child_type_index, "name");
        const n00b_abi_trace_v1 *group_trace = trace_for_type(metadata,
            entries, count, storage->type_index);
        const n00b_abi_trace_v1 *item_trace = trace_for_type(metadata,
            entries, count, lead->child_type_index);
        assert(group_trace->fixed_children.count == 2
            && !group_trace->arms.count
            && item_trace->fixed_children.count == 1
            && !item_trace->arms.count);
        const n00b_abi_trace_child_v1 *group_lead = trace_child_at(
            metadata, entries, count, group_trace->fixed_children, 0);
        const n00b_abi_trace_child_v1 *group_members = trace_child_at(
            metadata, entries, count, group_trace->fixed_children, 1);
        const n00b_abi_trace_child_v1 *item_name = trace_child_at(
            metadata, entries, count, item_trace->fixed_children, 0);
        assert(group_lead->child_type_index == lead->child_type_index
            && group_lead->byte_offset == lead->logical_bit_offset / 8
            && group_members->child_type_index == members->child_type_index
            && group_members->byte_offset == members->logical_bit_offset / 8
            && item_name->child_type_index == name->child_type_index
            && item_name->byte_offset == name->logical_bit_offset / 8);
        assert(lead->logical_bit_offset % 8 == 0
            && members->logical_bit_offset % 8 == 0
            && name->logical_bit_offset % 8 == 0
            && record_bits(bytes, lead->logical_bit_offset
                + flag->logical_bit_offset, 1) == 1);
        const n00b_abi_string_v1 *lead_name = (const void *)(bytes
            + (lead->logical_bit_offset + name->logical_bit_offset) / 8);
        assert(lead_name->short_value.selector ==
            (4 | (N00B_ABI_STRING_ASCII << 4)));
        assert(!memcmp(lead_name->short_value.bytes, "lead", 4));
        core_object_section_t backing;
        assert(core_object_section_find(object, length, format,
            format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : nullptr,
            format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
            : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
            : ".rodata.n00b_aggregate", &backing));
        uint64_t pointer_offset = storage->byte_offset
            + members->logical_bit_offset / 8;
        uint64_t data_offset = aggregate_pointer_addend(object, length,
            format, &section, &backing, pointer_offset);
        uint64_t descriptor_offset = aggregate_pointer_addend(object,
            length, format, &section, &backing, pointer_offset + 8);
        const n00bb_abi_directory_entry_t *types = entry_for(entries,
            count, N00B_ABI_TABLE_TYPE);
        const n00b_abi_type_v1 *item_type = row_at(metadata, types,
            lead->child_type_index);
        uint64_t extent = (item_type->size_bits + 7) / 8;
        uint64_t stride = (extent + item_type->alignment_bytes - 1)
            & ~(uint64_t)(item_type->alignment_bytes - 1);
        assert(data_offset <= backing.size
            && stride <= (backing.size - data_offset) / 2
            && descriptor_offset <= backing.size - 32
            && core_le_load_u64(backing.bytes + descriptor_offset) == 2
            && core_le_load_u32(backing.bytes + descriptor_offset + 8) == 1
            && core_le_load_u64(backing.bytes + descriptor_offset + 24) == 2);
        const uint8_t *first = backing.bytes + data_offset;
        const uint8_t *second = first + stride;
        assert(record_bits(first, flag->logical_bit_offset, 1) == 0
            && record_bits(second, flag->logical_bit_offset, 1) == 1);
        const n00b_abi_string_v1 *first_name = (const void *)(first
            + name->logical_bit_offset / 8);
        const n00b_abi_string_v1 *second_name = (const void *)(second
            + name->logical_bit_offset / 8);
        const char *long_text = "a long nested record string value";
        assert(first_name->short_value.selector
                == (5 | (N00B_ABI_STRING_ASCII << 4))
            && !memcmp(first_name->short_value.bytes, "first", 5)
            && second_name->long_value.selector
                == ((uint64_t)strlen(long_text)
                    | ((uint64_t)N00B_ABI_STRING_ASCII
                        << N00B_ABI_STRING_ENCODING_SHIFT)
                    | ((uint64_t)N00B_ABI_STRING_LONG
                        << N00B_ABI_STRING_TAG_SHIFT)));
        check_record_string_relocation(object, length, format, &backing,
            data_offset + stride + name->logical_bit_offset / 8,
            long_text);
        return;
    }
    const n00b_abi_field_v1 *label = record_field_named(metadata, entries,
        count, storage->type_index, "label");
    const n00b_abi_field_v1 *x = record_field_named(metadata, entries,
        count, storage->type_index, "x");
    assert(storage->byte_length >= 24
        && label->logical_bit_offset % 8 == 0
        && x->logical_bit_offset % 8 == 0
        && core_le_load_u64(bytes + x->logical_bit_offset / 8) == 7);
    const n00b_abi_string_v1 *text = (const void *)(bytes
        + label->logical_bit_offset / 8);
    const char *expected = "record storage has a long string value";
    size_t text_length = strlen(expected);
    assert(text->long_value.selector == ((uint64_t)text_length
        | ((uint64_t)N00B_ABI_STRING_ASCII
            << N00B_ABI_STRING_ENCODING_SHIFT)
        | ((uint64_t)N00B_ABI_STRING_LONG << N00B_ABI_STRING_TAG_SHIFT)));
    check_record_string_relocation(object, length, format, &section,
        storage->byte_offset + label->logical_bit_offset / 8, expected);
}

static void
check_variant_storage(const uint8_t *object, size_t length,
    const core_object_section_t *metadata,
    const n00bb_abi_directory_entry_t *entries, size_t count,
    uint32_t target_id)
{
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries, count,
        N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *extractors = entry_for(entries, count,
        N00B_ABI_TABLE_TAG_EXTRACTOR);
    const n00b_abi_storage_v1 *number = storage_named(metadata, entries,
        count, "number_choice");
    const n00b_abi_storage_v1 *text = storage_named(metadata, entries,
        count, "text_choice");
    const n00b_abi_storage_v1 *empty = storage_named(metadata, entries,
        count, "empty_choice");
    const n00b_abi_storage_v1 *choices = storage_named(metadata, entries,
        count, "choices");
    assert(types && layouts && extractors
        && number->type_index < types->record_count
        && text->type_index == number->type_index
        && empty->type_index == number->type_index);
    const n00b_abi_type_v1 *type = row_at(metadata, types, number->type_index);
    assert(type->kind == N00B_ABI_TYPE_VARIANT
        && type->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(metadata, layouts,
        type->layout_index);
    assert(layout->tag_extractor_index < extractors->record_count);
    const n00b_abi_tag_extractor_v1 *tag = row_at(metadata, extractors,
        layout->tag_extractor_index);
    assert(tag->kind == N00B_ABI_TAG_DIRECT_BITS
        && tag->tag_width_bits == 2
        && tag->unit_byte_offset == 16);
    const n00b_abi_trace_v1 *variant_trace = trace_for_type(metadata,
        entries, count, number->type_index);
    const n00bb_abi_directory_entry_t *trace_arms = entry_for(entries,
        count, N00B_ABI_TABLE_TRACE_ARM);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries, count,
        N00B_ABI_TABLE_U32_STREAM);
    assert(trace_arms && stream
        && variant_trace->tag_extractor_index == layout->tag_extractor_index
        && !variant_trace->fixed_children.count
        && variant_trace->arms.count == 3);
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t arm_index = core_le_load_u32(row_at(metadata, stream,
            variant_trace->arms.first + i));
        assert(arm_index && arm_index < trace_arms->record_count);
        const n00b_abi_trace_arm_v1 *arm = row_at(metadata, trace_arms,
            arm_index);
        assert(arm->tag_value == i + 1
            && arm->children.count == (i == 1 ? 1u : 0u));
        if (i == 1) {
            const n00b_abi_trace_child_v1 *child = trace_child_at(
                metadata, entries, count, arm->children, 0);
            const n00b_abi_type_v1 *child_type = row_at(metadata, types,
                child->child_type_index);
            assert(child->byte_offset == 0
                && child_type->kind == N00B_ABI_TYPE_STRING);
        }
    }
    const n00b_abi_storage_v1 *globals[] = {number, text, empty};
    for (size_t i = 0; i < 3; i++) {
        const n00b_abi_storage_v1 *global = globals[i];
        core_object_section_t section;
        assert(global->segment_kind == N00B_ABI_STORAGE_READ_ONLY
            && core_object_section_at(object, length, format,
                global->segment_index, &section)
            && global->byte_length >= 17
            && global->byte_offset <= section.size
            && global->byte_length <= section.size - global->byte_offset);
        const uint8_t *bytes = section.bytes + global->byte_offset;
        assert(record_bits(bytes, tag->unit_byte_offset * 8
            + tag->lsb_position, tag->tag_width_bits) == i + 1);
        if (i == 0) assert(core_le_load_u64(bytes) == 7);
        if (i == 1) {
            const char *expected = "a long variant string value";
            const n00b_abi_string_v1 *value = (const void *)bytes;
            assert(value->long_value.selector ==
                ((uint64_t)strlen(expected)
                    | ((uint64_t)N00B_ABI_STRING_ASCII
                        << N00B_ABI_STRING_ENCODING_SHIFT)
                    | ((uint64_t)N00B_ABI_STRING_LONG
                        << N00B_ABI_STRING_TAG_SHIFT)));
            check_record_string_relocation(object, length, format, &section,
                global->byte_offset, expected);
        }
        if (i == 2)
            for (size_t j = 0; j < tag->unit_byte_offset; j++)
                assert(bytes[j] == 0);
    }
    const n00b_abi_storage_v1 *holder = storage_named(metadata, entries,
        count, "holder");
    const n00b_abi_field_v1 *field = record_field_named(metadata, entries,
        count, holder->type_index, "choice");
    const n00b_abi_trace_v1 *holder_trace = trace_for_type(metadata,
        entries, count, holder->type_index);
    assert(holder_trace->fixed_children.count == 1
        && !holder_trace->arms.count);
    const n00b_abi_trace_child_v1 *holder_child = trace_child_at(
        metadata, entries, count, holder_trace->fixed_children, 0);
    assert(holder_child->child_type_index == number->type_index
        && holder_child->byte_offset == field->logical_bit_offset / 8);
    core_object_section_t holder_section;
    assert(field->child_type_index == number->type_index
        && field->logical_bit_offset % 8 == 0
        && holder->byte_length >= field->logical_bit_offset / 8 + 17
        && core_object_section_at(object, length, format,
            holder->segment_index, &holder_section));
    const uint8_t *nested = holder_section.bytes + holder->byte_offset
        + field->logical_bit_offset / 8;
    assert(record_bits(nested, tag->unit_byte_offset * 8
        + tag->lsb_position, tag->tag_width_bits) == 2);
    check_record_string_relocation(object, length, format, &holder_section,
        holder->byte_offset + field->logical_bit_offset / 8,
        "a long variant string value");
    core_object_section_t source, backing;
    assert(core_object_section_at(object, length, format,
        choices->segment_index, &source)
        && core_object_section_find(object, length, format,
            format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : nullptr,
            format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
            : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
            : ".rodata.n00b_aggregate", &backing));
    uint64_t data_offset = aggregate_pointer_addend(object, length, format,
        &source, &backing, choices->byte_offset);
    uint64_t descriptor_offset = aggregate_pointer_addend(object, length,
        format, &source, &backing, choices->byte_offset + 8);
    uint64_t extent = (type->size_bits + 7) / 8;
    uint64_t stride = (extent + type->alignment_bytes - 1)
        & ~(uint64_t)(type->alignment_bytes - 1);
    assert(data_offset <= backing.size
        && stride <= (backing.size - data_offset) / 3
        && descriptor_offset <= backing.size - 32
        && core_le_load_u64(backing.bytes + descriptor_offset) == 3
        && core_le_load_u64(backing.bytes + descriptor_offset + 24) == 3);
    const uint8_t *first = backing.bytes + data_offset;
    const uint8_t *second = first + stride;
    const uint8_t *third = second + stride;
    assert(core_le_load_u64(first) == 1
        && record_bits(first, tag->unit_byte_offset * 8
            + tag->lsb_position, tag->tag_width_bits) == 1
        && record_bits(second, tag->unit_byte_offset * 8
            + tag->lsb_position, tag->tag_width_bits) == 3
        && record_bits(third, tag->unit_byte_offset * 8
            + tag->lsb_position, tag->tag_width_bits) == 2);
    check_record_string_relocation(object, length, format, &backing,
        data_offset + 2 * stride, "a long variant string value");
}

static void
check_tuple_storage(const uint8_t *object, size_t length,
    const core_object_section_t *metadata,
    const n00bb_abi_directory_entry_t *entries, size_t count,
    uint32_t target_id)
{
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const n00b_abi_storage_v1 *pair = storage_named(metadata, entries,
        count, "pair");
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries, count,
        N00B_ABI_TABLE_LAYOUT);
    assert(types && layouts && pair->type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types, pair->type_index);
    assert(type->kind == N00B_ABI_TYPE_TUPLE && type->size_bits == 136
        && type->alignment_bytes == 8 && type->fields_or_arms.count == 2
        && type->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(metadata, layouts,
        type->layout_index);
    assert(layout->policy == N00B_ABI_LAYOUT_ORDERED);
    const n00b_abi_field_v1 *text = record_field_named(metadata, entries,
        count, pair->type_index, "0");
    const n00b_abi_field_v1 *flag = record_field_named(metadata, entries,
        count, pair->type_index, "1");
    assert(text->declaration_ordinal == 0 && text->logical_bit_offset == 0
        && flag->declaration_ordinal == 1 && flag->logical_bit_offset == 128);
    const n00b_abi_trace_v1 *pair_trace = trace_for_type(metadata, entries,
        count, pair->type_index);
    assert(pair_trace->fixed_children.count == 1
        && !pair_trace->arms.count);
    const n00b_abi_trace_child_v1 *text_trace = trace_child_at(metadata,
        entries, count, pair_trace->fixed_children, 0);
    assert(text_trace->byte_offset == 0
        && text_trace->child_type_index == text->child_type_index);
    core_object_section_t section;
    assert(pair->segment_kind == N00B_ABI_STORAGE_READ_ONLY
        && pair->byte_length == 17
        && core_object_section_at(object, length, format,
            pair->segment_index, &section)
        && pair->byte_offset <= section.size
        && pair->byte_length <= section.size - pair->byte_offset);
    assert(record_bits(section.bytes + pair->byte_offset,
                       flag->logical_bit_offset, 1) == 1);
    check_record_string_relocation(object, length, format, &section,
        pair->byte_offset, "a long ordered tuple string value");

    const n00b_abi_storage_v1 *holder = storage_named(metadata, entries,
        count, "holder");
    const n00b_abi_field_v1 *item = record_field_named(metadata, entries,
        count, holder->type_index, "item");
    core_object_section_t holder_section;
    assert(item->child_type_index == pair->type_index
        && item->logical_bit_offset % 8 == 0
        && core_object_section_at(object, length, format,
            holder->segment_index, &holder_section));
    const uint8_t *nested = holder_section.bytes + holder->byte_offset
        + item->logical_bit_offset / 8;
    assert(record_bits(nested, flag->logical_bit_offset, 1) == 0);
    check_record_string_relocation(object, length, format, &holder_section,
        holder->byte_offset + item->logical_bit_offset / 8,
        "a long nested tuple string value");

    const n00b_abi_storage_v1 *pairs = storage_named(metadata, entries,
        count, "pairs");
    core_object_section_t source, backing;
    assert(core_object_section_at(object, length, format,
        pairs->segment_index, &source)
        && core_object_section_find(object, length, format,
            format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : nullptr,
            format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_aggregate"
            : format == CORE_TARGET_OBJECT_COFF ? ".rdata$A"
            : ".rodata.n00b_aggregate", &backing));
    uint64_t data_offset = aggregate_pointer_addend(object, length,
        format, &source, &backing, pairs->byte_offset);
    uint64_t descriptor_offset = aggregate_pointer_addend(object, length,
        format, &source, &backing, pairs->byte_offset + 8);
    uint64_t stride = (pair->byte_length + type->alignment_bytes - 1)
        & ~(uint64_t)(type->alignment_bytes - 1);
    assert(stride == 24 && data_offset <= backing.size
        && stride <= (backing.size - data_offset) / 2
        && descriptor_offset <= backing.size - 32
        && core_le_load_u64(backing.bytes + descriptor_offset) == 2);
    assert(record_bits(backing.bytes + data_offset,
        flag->logical_bit_offset, 1) == 1
        && record_bits(backing.bytes + data_offset + stride,
            flag->logical_bit_offset, 1) == 0);
    check_record_string_relocation(object, length, format, &backing,
        data_offset, "a long first tuple array value");
    check_record_string_relocation(object, length, format, &backing,
        data_offset + stride, "a long second tuple array value");
}

static void
check_anonymous_record_storage(const uint8_t *object, size_t length,
    const core_object_section_t *metadata,
    const n00bb_abi_directory_entry_t *entries, size_t count,
    uint32_t target_id)
{
    core_target_object_format_t format = target_id == N00BB_ABI_TARGET_ARM64_MACOS
        ? CORE_TARGET_OBJECT_MACH_O
        : target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            ? CORE_TARGET_OBJECT_COFF : CORE_TARGET_OBJECT_ELF;
    const n00b_abi_storage_v1 *first = storage_named(metadata, entries,
        count, "first");
    const n00b_abi_storage_v1 *second = storage_named(metadata, entries,
        count, "second");
    const n00b_abi_storage_v1 *nested = storage_named(metadata, entries,
        count, "nested");
    const n00bb_abi_directory_entry_t *types = entry_for(entries, count,
        N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries, count,
        N00B_ABI_TABLE_LAYOUT);
    assert(types && layouts && first->type_index == second->type_index
        && first->type_index < types->record_count);
    const n00b_abi_type_v1 *type = row_at(metadata, types,
        first->type_index);
    assert(type->kind == N00B_ABI_TYPE_RECORD && !type->owner_module_index
        && type->fields_or_arms.count == 3
        && type->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(metadata, layouts,
        type->layout_index);
    assert(layout->policy == N00B_ABI_LAYOUT_DEFAULT);
    const n00b_abi_field_v1 *left = record_field_named(metadata, entries,
        count, first->type_index, "left");
    const n00b_abi_field_v1 *right = record_field_named(metadata, entries,
        count, first->type_index, "right");
    const n00b_abi_field_v1 *flag = record_field_named(metadata, entries,
        count, first->type_index, "flag");
    assert(right->logical_bit_offset % 8 == 0);
    const n00b_abi_storage_v1 *values[] = {first, second};
    const char *texts[] = {"a long anonymous record string",
                            "another anonymous record string"};
    for (size_t i = 0; i < 2; i++) {
        core_object_section_t section;
        const n00b_abi_storage_v1 *value = values[i];
        assert(value->segment_kind == N00B_ABI_STORAGE_READ_ONLY
            && value->byte_length == (type->size_bits + 7) / 8
            && core_object_section_at(object, length, format,
                value->segment_index, &section)
            && value->byte_offset <= section.size
            && value->byte_length <= section.size - value->byte_offset);
        const uint8_t *bytes = section.bytes + value->byte_offset;
        assert(record_bits(bytes, left->logical_bit_offset, 64) == 8 + i
            && record_bits(bytes, flag->logical_bit_offset, 1) == 1 - i);
        check_record_string_relocation(object, length, format, &section,
            value->byte_offset + right->logical_bit_offset / 8, texts[i]);
    }
    const n00b_abi_field_v1 *item = record_field_named(metadata, entries,
        count, nested->type_index, "item");
    assert(item->child_type_index == first->type_index
        && item->logical_bit_offset % 8 == 0);
    core_object_section_t section;
    assert(core_object_section_at(object, length, format,
        nested->segment_index, &section));
    const uint8_t *bytes = section.bytes + nested->byte_offset
        + item->logical_bit_offset / 8;
    assert(record_bits(bytes, left->logical_bit_offset, 64) == 10
        && record_bits(bytes, flag->logical_bit_offset, 1) == 1);
    check_record_string_relocation(object, length, format, &section,
        nested->byte_offset + item->logical_bit_offset / 8
            + right->logical_bit_offset / 8,
        "a nested anonymous record string");
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
            || !strcmp(argv[2], "import-flagset")
            || !strcmp(argv[2], "variant")
            || !strcmp(argv[2], "constraint")
            || !strcmp(argv[2], "trait-methods")
            || !strcmp(argv[2], "trait-dispatch")
            || !strcmp(argv[2], "callback")
            || !strcmp(argv[2], "storage")
            || !strcmp(argv[2], "string-storage")
            || !strcmp(argv[2], "array-storage")
            || !strcmp(argv[2], "zero-element-array-storage")
            || !strcmp(argv[2], "nested-array-storage")
            || !strcmp(argv[2], "ragged-array-storage")
            || !strcmp(argv[2], "record-storage")
            || !strcmp(argv[2], "record-bits-storage")
            || !strcmp(argv[2], "record-nested-storage")
            || !strcmp(argv[2], "variant-storage")
            || !strcmp(argv[2], "tuple-storage")
            || !strcmp(argv[2], "anonymous-record-storage")
            || !strcmp(argv[2], "alias")
            || !strcmp(argv[2], "flagset")
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

    if (argc == 3 && !strcmp(argv[2], "alias")) {
        const n00b_abi_symbol_v1 *alias = nullptr;
        const n00b_abi_symbol_v1 *answer = nullptr;
        for (uint32_t i = 1; i < symbols->record_count; i++) {
            const n00b_abi_symbol_v1 *symbol = row_at(&metadata, symbols, i);
            if (short_name_is(&symbol->name, "Number")) alias = symbol;
            if (short_name_is(&symbol->name, "answer")) answer = symbol;
        }
        assert(alias && answer
            && alias->kind == N00B_ABI_SYMBOL_TYPE
            && alias->type_index == answer->type_index
            && alias->type_index < types->record_count);
        const n00b_abi_type_v1 *type = row_at(&metadata, types,
                                               alias->type_index);
        assert(short_name_is(&type->canonical_name, "int")
            && !type->owner_module_index);
        free(entries);
        free(object);
        return 0;
    }

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
    if (argc == 3 && !strcmp(argv[2], "string-storage")) {
        check_string_storage(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "array-storage")) {
        check_array_storage(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "zero-element-array-storage")) {
        check_zero_element_arrays(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[2], "nested-array-storage")
                      || !strcmp(argv[2], "ragged-array-storage"))) {
        check_nested_array_storage(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id,
            !strcmp(argv[2], "ragged-array-storage"));
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[2], "record-storage")
                    || !strcmp(argv[2], "record-bits-storage")
                    || !strcmp(argv[2], "record-nested-storage"))) {
        check_record_aggregate_storage(object, (size_t)length, &metadata,
            entries, header.directory_count, header.key.target_id, argv[2]);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "variant-storage")) {
        check_variant_storage(object, (size_t)length, &metadata,
            entries, header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "tuple-storage")) {
        check_tuple_storage(object, (size_t)length, &metadata, entries,
            header.directory_count, header.key.target_id);
        free(entries);
        free(object);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "anonymous-record-storage")) {
        check_anonymous_record_storage(object, (size_t)length, &metadata,
            entries, header.directory_count, header.key.target_id);
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
    if (argc == 3 && !strcmp(argv[2], "import-flagset")) {
        check_imported_flagset(&metadata, entries, header.directory_count);
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

    if (argc == 3 && !strcmp(argv[2], "flagset")) {
        check_flagset(&metadata, entries, header.directory_count);
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
