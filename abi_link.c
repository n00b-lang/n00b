#include "abi_link.h"

#include "abi_string.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/indexed_table.h"
#include <string.h>

bool
n00bb_abi_object_open(const core_target_desc_t *target,
                      const uint8_t *object, size_t object_bytes,
                      n00bb_abi_object_view_t *out)
{
    n00bb_abi_image_key_t key;
    if (!out || !object || !object_bytes
        || !n00bb_abi_image_key_for_target(target, &key)) return false;
    const char *segment = nullptr, *name = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = ".data.rel.ro.n00b_meta";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        segment = "__DATA_CONST";
        name = "__n00b_meta";
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = ".n00b$M";
        break;
    default:
        return false;
    }
    core_object_section_t metadata = {0};
    if (!core_object_section_find(object, object_bytes,
            target->object_format, segment, name, &metadata)
        || metadata.size < N00BB_ABI_HEADER_BYTES) return false;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    uint32_t count = core_le_load_u32(metadata.bytes + 60);
    if (!schemas || count > schema_count
        || count > (metadata.size - N00BB_ABI_HEADER_BYTES)
                     / N00BB_ABI_DIRECTORY_BYTES) return false;
    n00bb_abi_directory_entry_t *directory = core_alloc_array(
        n00bb_abi_directory_entry_t, count);
    n00bb_abi_image_header_t header;
    if (!n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
            schemas, schema_count, false, &header, directory, count)) {
        core_free(directory);
        return false;
    }
    *out = (n00bb_abi_object_view_t){
        .object = object, .object_bytes = object_bytes,
        .metadata = metadata, .format = target->object_format,
        .header = header,
        .directory = directory, .directory_count = count,
    };
    return true;
}

void
n00bb_abi_object_close(n00bb_abi_object_view_t *view)
{
    if (!view) return;
    core_free(view->directory);
    *view = (n00bb_abi_object_view_t){0};
}

const n00bb_abi_directory_entry_t *
n00bb_abi_object_table(const n00bb_abi_object_view_t *view, uint32_t kind)
{
    if (!view || !view->directory) return nullptr;
    for (size_t i = 0; i < view->directory_count; i++)
        if (view->directory[i].kind == kind) return &view->directory[i];
    return nullptr;
}

const void *
n00bb_abi_object_row(const n00bb_abi_object_view_t *view,
                     uint32_t kind, uint32_t index)
{
    const n00bb_abi_directory_entry_t *table =
        n00bb_abi_object_table(view, kind);
    if (!table || index >= table->record_count) return nullptr;
    return view->metadata.bytes + table->file_offset
        + (size_t)index * table->record_bytes;
}

bool
n00bb_abi_object_string(const n00bb_abi_object_view_t *view,
                        const n00b_abi_string_v1 *value,
                        n00bb_abi_string_view_t *out)
{
    if (!view || !view->directory || !value || !out) return false;
    uintptr_t base = (uintptr_t)view->metadata.bytes;
    uintptr_t address = (uintptr_t)value;
    if (address < base || address - base > view->metadata.size
        || sizeof(*value) > view->metadata.size - (address - base))
        return false;

    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)value, sizeof(*value),
                                 &header)) return false;
    if (header.tag == N00BB_ABI_STRING_SHORT) {
        *out = (n00bb_abi_string_view_t){
            .bytes = value->short_value.bytes,
            .length = (size_t)header.length,
        };
        return true;
    }
    if (header.tag != N00BB_ABI_STRING_LONG) return false;

    const char *segment = view->format == CORE_TARGET_OBJECT_MACH_O
        ? "__TEXT" : nullptr;
    const char *name = view->format == CORE_TARGET_OBJECT_MACH_O
        ? "__n00b_strings" : view->format == CORE_TARGET_OBJECT_COFF
            ? ".rdata$S" : ".rodata.n00b_strings";
    core_object_section_t pool;
    if (!core_object_section_find(view->object, view->object_bytes,
            view->format, segment, name, &pool)) return false;

    bool found = false;
    uint64_t pool_offset = 0;
    uint64_t field_offset = address - base;
    for (uint32_t i = 0; i < view->metadata.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        if (!core_object_absolute64_relocation_at(&view->metadata,
                view->format, i, &relocation)) return false;
        if (relocation.section_offset != field_offset) continue;
        if (found) return false;
        if (view->format == CORE_TARGET_OBJECT_MACH_O) {
            if (!relocation.target_is_section
                || relocation.native_symbol_index != pool.native_index
                || relocation.addend_bits < pool.virtual_address)
                return false;
            pool_offset = relocation.addend_bits - pool.virtual_address;
        }
        else {
            core_object_symbol_name_t symbol;
            if (relocation.target_is_section
                || !core_object_symbol_name_at(view->object,
                    view->object_bytes, view->format, &view->metadata,
                    relocation.native_symbol_index, &symbol)
                || symbol.length != sizeof("n00b_string_pool") - 1
                || memcmp(symbol.bytes, "n00b_string_pool",
                          symbol.length)) return false;
            pool_offset = relocation.addend_bits;
        }
        found = true;
    }
    if (!found || pool_offset > pool.size
        || header.length > pool.size - pool_offset) return false;
    *out = (n00bb_abi_string_view_t){
        .bytes = pool.bytes + pool_offset,
        .length = (size_t)header.length,
    };
    return true;
}

static core_hash_value_t
module_key_hash(void *key)
{
    return core_string_hash(*(const core_string_t *)key);
}

static bool
module_key_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a,
                          *(const core_string_t *)b);
}

static bool
string_matches_module(const n00bb_abi_object_view_t *view,
                      const n00b_abi_string_v1 *value, core_string_t module)
{
    n00bb_abi_string_view_t decoded;
    return n00bb_abi_object_string(view, value, &decoded)
        && decoded.length == module.u8_bytes
        && !memcmp(decoded.bytes, module.data, decoded.length);
}

static bool
selected_imports_match(const n00bb_abi_object_view_t *view,
                       const core_translation_unit_t *unit)
{
    const n00bb_abi_directory_entry_t *modules = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_MODULE);
    const n00bb_abi_directory_entry_t *requirements =
        n00bb_abi_object_table(view, N00B_ABI_TABLE_REQUIREMENT);
    if (!modules || modules->record_count != 2 || !requirements
        || !requirements->record_count) return false;
    const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_MODULE, 1);
    if (!module || !string_matches_module(view, &module->full_path,
            unit->module.identifier)) return false;

    size_t matched = 0;
    size_t expected = core_translation_unit_import_count(unit);
    for (uint32_t i = 1; i < requirements->record_count; i++) {
        const n00b_abi_requirement_v1 *requirement =
            n00bb_abi_object_row(view, N00B_ABI_TABLE_REQUIREMENT, i);
        if (!requirement || requirement->requesting_module_index != 1)
            return false;
        if (requirement->symbol_kind != N00B_ABI_SYMBOL_MODULE) continue;
        core_translation_unit_t *imported = matched < expected
            ? core_translation_unit_import(unit, matched) : nullptr;
        if (!imported || !string_matches_module(view,
                &requirement->full_path, imported->module.identifier))
            return false;
        matched++;
    }
    return matched == expected;
}

static bool
selected_sccs_match(core_link_unit_t *link,
                    const core_indexed_table_t *module_indexes,
                    const n00bb_abi_selected_objects_t *selected)
{
    for (size_t group = 0; group < core_link_unit_scc_count(link); group++) {
        size_t count = core_link_unit_scc_member_count(link, group);
        if (!count) return false;
        core_translation_unit_t *only = count == 1
            ? core_link_unit_scc_member(link, group, 0) : nullptr;
        bool recursive = count > 1;
        if (only) for (size_t i = 0;
            i < core_translation_unit_import_count(only); i++)
            recursive |= core_translation_unit_import(only, i) == only;
        bool *ordinals = recursive ? core_alloc_array(bool, count) : nullptr;
        if (recursive && !ordinals) return false;
        bool valid = true;
        uint64_t interface_identity = 0, freshness_identity = 0;
        for (size_t i = 0; valid && i < count; i++) {
            core_translation_unit_t *unit = core_link_unit_scc_member(link,
                group, i);
            const core_indexed_entry_t *entry = unit
                ? core_indexed_table_find(module_indexes,
                    &unit->module.identifier) : nullptr;
            if (!entry || entry->index >= selected->count) {
                valid = false;
                break;
            }
            const n00bb_abi_object_view_t *view =
                &selected->views[entry->index];
            const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_MODULE, 1);
            const n00bb_abi_directory_entry_t *scc_table =
                n00bb_abi_object_table(view, N00B_ABI_TABLE_SCC);
            if (!module || !scc_table || scc_table->record_count
                != (recursive ? 2u : 1u)
                || module->scc_index != (recursive ? 1u : 0u)) {
                valid = false;
                break;
            }
            if (!recursive) continue;
            const n00b_abi_scc_v1 *scc = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SCC, 1);
            const n00bb_abi_directory_entry_t *stream =
                n00bb_abi_object_table(view, N00B_ABI_TABLE_U32_STREAM);
            if (!scc || !stream || scc->members.count != 1
                || scc->lexical_order.count != 1
                || scc->members.first >= stream->record_count
                || scc->lexical_order.first >= stream->record_count) {
                valid = false;
                break;
            }
            const uint8_t *member = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_U32_STREAM, scc->members.first);
            const uint8_t *ordinal = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_U32_STREAM, scc->lexical_order.first);
            uint32_t position = core_le_load_u32(ordinal);
            if (core_le_load_u32(member) != 1 || position >= count
                || ordinals[position]
                || (i && (scc->interface_identity != interface_identity
                    || scc->freshness_identity != freshness_identity))) {
                valid = false;
                break;
            }
            ordinals[position] = true;
            interface_identity = scc->interface_identity;
            freshness_identity = scc->freshness_identity;
        }
        for (size_t i = 0; valid && recursive && i < count; i++)
            valid = ordinals[i];
        core_free(ordinals);
        if (!valid) return false;
    }
    return true;
}

void
n00bb_abi_selected_objects_release(n00bb_abi_selected_objects_t *selected)
{
    if (!selected) return;
    for (size_t i = 0; i < selected->count; i++)
        n00bb_abi_object_close(&selected->views[i]);
    core_free(selected->views);
    *selected = (n00bb_abi_selected_objects_t){0};
}

bool
n00bb_abi_select_objects(core_link_unit_t *link,
                         const core_target_desc_t *target,
                         const n00bb_abi_object_input_t *candidates,
                         size_t candidate_count,
                         n00bb_abi_selected_objects_t *out)
{
    if (!link || !target || !out || (candidate_count && !candidates)
        || !core_link_unit_graph_is_finalized(link)) return false;
    size_t count = core_link_unit_translation_unit_count(link);
    if (!count || count > UINT32_MAX) return false;
    core_indexed_table_t *modules = core_indexed_table_new(0, UINT32_MAX,
        module_key_hash, module_key_eq);
    n00bb_abi_selected_objects_t selected = {
        .views = core_alloc_array(n00bb_abi_object_view_t, count),
        .count = count,
    };
    bool valid = modules && selected.views;
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        const core_indexed_entry_t *entry = unit
            ? core_indexed_table_intern(modules, &unit->module.identifier,
                unit, nullptr) : nullptr;
        valid = entry && entry->index == i && entry->value == unit;
    }
    for (size_t i = 0; valid && i < candidate_count; i++) {
        const n00bb_abi_object_input_t *candidate = &candidates[i];
        const core_indexed_entry_t *entry = candidate->module.data
            ? core_indexed_table_find(modules, &candidate->module) : nullptr;
        if (!entry) continue;
        n00bb_abi_object_view_t *view = &selected.views[entry->index];
        core_translation_unit_t *unit = entry->value;
        valid = !view->directory
            && n00bb_abi_object_open(target, candidate->object,
                candidate->object_bytes, view)
            && selected_imports_match(view, unit);
    }
    for (size_t i = 0; valid && i < count; i++)
        valid = selected.views[i].directory != nullptr;
    if (valid) valid = selected_sccs_match(link, modules, &selected);
    core_indexed_table_free(modules);
    if (!valid) {
        n00bb_abi_selected_objects_release(&selected);
        return false;
    }
    *out = selected;
    return true;
}
