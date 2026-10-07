#include "abi_link.h"

#include "abi_string.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/hash.h"
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
    if (header.tag != N00BB_ABI_STRING_LONG
        && header.tag != N00BB_ABI_STRING_SPAN) return false;

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

struct n00bb_abi_index_plan_t {
    core_index_remap_t **maps;
    uint32_t *counts;
    size_t input_count;
    size_t kind_count;
};

typedef struct {
    const n00bb_abi_object_view_t *view;
    const void *row;
    const uint8_t *bytes;
    size_t length;
    uint64_t scalar;
    uint64_t auxiliary;
    size_t input_index;
    uint32_t local_index;
    bool unique;
} abi_link_key_t;

static core_hash_value_t
link_key_hash(void *value)
{
    const abi_link_key_t *key = value;
    if (key->unique)
        return core_hash_raw(&key->input_index, sizeof(key->input_index))
            ^ core_hash_raw(&key->local_index, sizeof(key->local_index));
    return core_hash_raw(key->bytes, key->length)
        ^ core_hash_raw(&key->auxiliary, sizeof(key->auxiliary));
}

static bool
link_key_eq(const void *a, const void *b)
{
    const abi_link_key_t *left = a, *right = b;
    if (left->unique || right->unique)
        return left->unique && right->unique
            && left->input_index == right->input_index
            && left->local_index == right->local_index;
    return left->auxiliary == right->auxiliary
        && left->length == right->length
        && !memcmp(left->bytes, right->bytes, left->length);
}

static bool
link_key_string(abi_link_key_t *key, const n00b_abi_string_v1 *value)
{
    n00bb_abi_string_view_t string;
    if (!n00bb_abi_object_string(key->view, value, &string)) return false;
    key->bytes = string.bytes;
    key->length = string.length;
    return true;
}

static bool
link_key_init(abi_link_key_t *key, uint32_t kind)
{
    switch (kind) {
    case N00B_ABI_TABLE_TYPE: {
        const n00b_abi_type_v1 *type = key->row;
        if (type->kind == N00B_ABI_TYPE_VARIABLE) {
            if (type->typehash || !type->typevar_owner_scope_index)
                return false;
            key->scalar = ((uint64_t)type->typevar_owner_scope_index << 32)
                | type->typevar_ordinal;
            key->auxiliary = key->input_index + 1;
        }
        else {
            if (!type->typehash) return false;
            key->scalar = type->typehash;
        }
        key->bytes = (const uint8_t *)&key->scalar;
        key->length = sizeof(key->scalar);
        return true;
    }
    case N00B_ABI_TABLE_PACKAGE:
        return link_key_string(key,
            &((const n00b_abi_package_v1 *)key->row)->name);
    case N00B_ABI_TABLE_MODULE:
        return link_key_string(key,
            &((const n00b_abi_module_v1 *)key->row)->full_path);
    case N00B_ABI_TABLE_SCC:
        key->bytes = key->row;
        key->length = 2 * sizeof(uint64_t);
        return true;
    case N00B_ABI_TABLE_SOURCE_FILE: {
        const n00b_abi_source_file_v1 *file = key->row;
        key->auxiliary = file->source_identity;
        return link_key_string(key, &file->path);
    }
    case N00B_ABI_TABLE_RUNTIME_CONFIG:
        return link_key_string(key,
            &((const n00b_abi_runtime_config_v1 *)key->row)
                ->memory_manager_name);
    case N00B_ABI_TABLE_STATIC_STRING:
        return link_key_string(key,
            &((const n00b_abi_static_string_v1 *)key->row)->value);
    default:
        key->unique = true;
        return true;
    }
}

static bool
compatible_type_identity(const abi_link_key_t *first,
                         const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a = first->row, *b = candidate->row;
    n00bb_abi_string_view_t a_name, b_name;
    return a->kind == b->kind
        && n00bb_abi_object_string(first->view, &a->canonical_name, &a_name)
        && n00bb_abi_object_string(candidate->view, &b->canonical_name,
                                   &b_name)
        && a_name.length == b_name.length
        && !memcmp(a_name.bytes, b_name.bytes, a_name.length);
}

static bool
reserved_row_is_zero(const void *row, size_t bytes)
{
    const uint8_t *data = row;
    for (size_t i = 0; i < bytes; i++)
        if (data[i]) return false;
    return true;
}

static bool
link_kind_is_stream(uint32_t kind)
{
    return kind == N00B_ABI_TABLE_U32_STREAM
        || kind == N00B_ABI_TABLE_BYTE_STREAM
        || kind == N00B_ABI_TABLE_U64_STREAM;
}

void
n00bb_abi_index_plan_free(n00bb_abi_index_plan_t *plan)
{
    if (!plan) return;
    for (size_t i = 0; i < plan->input_count * plan->kind_count; i++)
        core_index_remap_free(plan->maps[i]);
    core_free(plan->maps);
    core_free(plan->counts);
    core_free(plan);
}

bool
n00bb_abi_index_plan_count(const n00bb_abi_index_plan_t *plan,
                           uint32_t kind, uint32_t *out_count)
{
    if (!plan || !out_count || !kind || kind >= plan->kind_count)
        return false;
    *out_count = plan->counts[kind];
    return true;
}

bool
n00bb_abi_index_plan_map(const n00bb_abi_index_plan_t *plan,
                         size_t input_index, uint32_t kind,
                         uint32_t local_index, uint32_t *out_index)
{
    if (!plan || !out_index || input_index >= plan->input_count
        || !kind || kind >= plan->kind_count) return false;
    return core_index_remap_get(
        plan->maps[input_index * plan->kind_count + kind],
        local_index, out_index);
}

n00bb_abi_index_plan_t *
n00bb_abi_index_plan_build(const n00bb_abi_selected_objects_t *selected)
{
    if (!selected || !selected->views || !selected->count) return nullptr;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    if (!schemas || !schema_count) return nullptr;
    uint32_t max_kind = 0;
    for (size_t i = 0; i < schema_count; i++)
        if (schemas[i].kind > max_kind) max_kind = schemas[i].kind;
    size_t kind_count = (size_t)max_kind + 1;
    if (selected->count > SIZE_MAX / kind_count) return nullptr;

    size_t total_rows = 0;
    for (size_t i = 0; i < selected->count; i++) {
        const n00bb_abi_object_view_t *view = &selected->views[i];
        if (!view->directory) return nullptr;
        for (size_t j = 0; j < view->directory_count; j++) {
            size_t rows = view->directory[j].record_count;
            if (rows > SIZE_MAX - total_rows) return nullptr;
            total_rows += rows;
        }
    }
    abi_link_key_t *keys = core_alloc_array(abi_link_key_t, total_rows);
    core_indexed_table_t **tables = core_alloc_array(
        core_indexed_table_t *, kind_count);
    n00bb_abi_index_plan_t *plan = core_alloc(n00bb_abi_index_plan_t);
    *plan = (n00bb_abi_index_plan_t){
        .maps = core_alloc_array(core_index_remap_t *,
                                 selected->count * kind_count),
        .counts = core_alloc_array(uint32_t, kind_count),
        .input_count = selected->count,
        .kind_count = kind_count,
    };
    bool valid = true;
    const abi_link_key_t *first_nil = nullptr;
    size_t next_key = 0;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        for (size_t j = 0; valid && j < view->directory_count; j++) {
            const n00bb_abi_directory_entry_t *directory =
                &view->directory[j];
            uint32_t kind = directory->kind;
            bool stream = link_kind_is_stream(kind);
            if (!kind || kind >= kind_count || (!stream
                && !directory->record_count)) {
                valid = false;
                break;
            }
            uint32_t max_index = kind == N00B_ABI_TABLE_TYPE
                ? INT32_MAX : UINT32_MAX - 1;
            if (!tables[kind])
                tables[kind] = core_indexed_table_new(stream ? 0 : 1,
                    max_index, link_key_hash, link_key_eq);
            core_index_remap_t *remap = core_index_remap_new(
                directory->record_count, max_index);
            plan->maps[input * kind_count + kind] = remap;
            valid = tables[kind] && remap;
            if (!valid) break;
            uint32_t first = 0;
            if (!stream) {
                const void *row = view->metadata.bytes
                    + directory->file_offset;
                valid = core_index_remap_set(remap, 0, 0);
                if (kind == N00B_ABI_TABLE_TYPE) {
                    abi_link_key_t *nil = &keys[next_key++];
                    *nil = (abi_link_key_t){.view = view, .row = row};
                    const n00b_abi_type_v1 *type = row;
                    valid = valid && type->kind == N00B_ABI_TYPE_NIL
                        && type->typehash && (!first_nil
                            || (type->typehash
                                == ((const n00b_abi_type_v1 *)
                                    first_nil->row)->typehash
                                && compatible_type_identity(first_nil, nil)));
                    if (!first_nil) first_nil = nil;
                }
                else valid = valid && reserved_row_is_zero(row,
                    directory->record_bytes);
                first = 1;
            }
            for (uint32_t local = first; valid
                 && local < directory->record_count; local++) {
                const void *row = view->metadata.bytes
                    + directory->file_offset
                    + (size_t)local * directory->record_bytes;
                abi_link_key_t *key = &keys[next_key++];
                *key = (abi_link_key_t){
                    .view = view, .row = row,
                    .input_index = input, .local_index = local,
                };
                bool inserted = false;
                const core_indexed_entry_t *entry = link_key_init(key, kind)
                    ? core_indexed_table_intern(tables[kind], key, key,
                        &inserted) : nullptr;
                valid = entry && (inserted
                    || kind != N00B_ABI_TABLE_TYPE
                    || compatible_type_identity(entry->value, key))
                    && core_index_remap_set(remap, local, entry->index);
            }
        }
    }
    for (size_t kind = 1; valid && kind < kind_count; kind++) {
        if (!tables[kind]) continue;
        size_t rows = core_indexed_table_len(tables[kind]);
        valid = rows <= UINT32_MAX;
        if (valid) plan->counts[kind] = (uint32_t)rows;
    }
    for (size_t i = 0; i < kind_count; i++)
        core_indexed_table_free(tables[i]);
    core_free(tables);
    core_free(keys);
    if (!valid) {
        n00bb_abi_index_plan_free(plan);
        return nullptr;
    }
    return plan;
}

typedef struct {
    uint32_t row_kind;
    size_t offset;
    uint32_t target_kind;
} abi_ref_column_t;

#define ABI_REF(K, T, F, TARGET) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##TARGET}

static const abi_ref_column_t abi_index_columns[] = {
    ABI_REF(PACKAGE, package, parent_package_index, PACKAGE),
    ABI_REF(MODULE, module, package_index, PACKAGE),
    ABI_REF(MODULE, module, scc_index, SCC),
    ABI_REF(MODULE, module, comptime_initializer, FUNCTION),
    ABI_REF(MODULE, module, runtime_initializer, FUNCTION),
    ABI_REF(MODULE, module, runtime_config_index, RUNTIME_CONFIG),
    ABI_REF(MODULE, module, source_file_index, SOURCE_FILE),
    ABI_REF(TYPE, type, layout_index, LAYOUT),
    ABI_REF(TYPE, type, owner_module_index, MODULE),
    ABI_REF(TYPE, type, tracing_index, TRACE),
    ABI_REF(TYPE, type, call_signature_index, CALL_SIGNATURE),
    ABI_REF(TYPE, type, typevar_owner_scope_index, SCOPE),
    ABI_REF(ENUM_ITEM, enum_item, parent_type_index, TYPE),
    ABI_REF(ENUM_ITEM, enum_item, symbol_index, SYMBOL),
    ABI_REF(CONSTRAINT, constraint, target_type_index, TYPE),
    ABI_REF(CONSTRAINT, constraint, result_type_index, TYPE),
    ABI_REF(CONSTRAINT, constraint, call_type_index, TYPE),
    ABI_REF(LAYOUT, layout, type_index, TYPE),
    ABI_REF(LAYOUT, layout, tag_extractor_index, TAG_EXTRACTOR),
    ABI_REF(FIELD, field, parent_type_index, TYPE),
    ABI_REF(FIELD, field, child_type_index, TYPE),
    ABI_REF(FIELD, field, symbol_index, SYMBOL),
    ABI_REF(ARM, arm, field.parent_type_index, TYPE),
    ABI_REF(ARM, arm, field.child_type_index, TYPE),
    ABI_REF(ARM, arm, field.symbol_index, SYMBOL),
    ABI_REF(TRACE, trace, type_index, TYPE),
    ABI_REF(TRACE, trace, child_trace_index, TRACE),
    ABI_REF(TRACE, trace, tag_extractor_index, TAG_EXTRACTOR),
    ABI_REF(TRACE_CHILD, trace_child, child_type_index, TYPE),
    ABI_REF(TRACE_ARM, trace_arm, child_trace_index, TRACE),
    ABI_REF(FUNCTION, function, module_index, MODULE),
    ABI_REF(FUNCTION, function, scope_index, SCOPE),
    ABI_REF(FUNCTION, function, symbol_index, SYMBOL),
    ABI_REF(FUNCTION, function, success_type_index, TYPE),
    ABI_REF(FUNCTION, function, failure_type_index, TYPE),
    ABI_REF(FUNCTION, function, variadic_type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, success_type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, failure_type_index, TYPE),
    ABI_REF(CALL_SIGNATURE, call_signature, rest_parameter_index, PARAMETER),
    ABI_REF(PARAMETER, parameter, type_index, TYPE),
    ABI_REF(CODE_RANGE, code_range, function_index, FUNCTION),
    ABI_REF(SCOPE, scope, module_index, MODULE),
    ABI_REF(SCOPE, scope, function_index, FUNCTION),
    ABI_REF(SCOPE, scope, parent_scope_index, SCOPE),
    ABI_REF(SCOPE, scope, source_location_index, SOURCE_LOCATION),
    ABI_REF(SYMBOL, symbol, type_index, TYPE),
    ABI_REF(SYMBOL, symbol, module_index, MODULE),
    ABI_REF(SYMBOL, symbol, scope_index, SCOPE),
    ABI_REF(SYMBOL, symbol, declaration_location_index, SOURCE_LOCATION),
    ABI_REF(SYMBOL, symbol, native_binding_index, NATIVE_BINDING),
    ABI_REF(TRAIT_METHOD, trait_method, trait_type_index, TYPE),
    ABI_REF(TRAIT_METHOD, trait_method, signature_type_index, TYPE),
    ABI_REF(TRAIT_METHOD, trait_method, symbol_index, SYMBOL),
    ABI_REF(DISPATCH, dispatch, trait_type_index, TYPE),
    ABI_REF(DISPATCH_ENTRY, dispatch_entry, receiver_type_index, TYPE),
    ABI_REF(DISPATCH_ENTRY, dispatch_entry, function_index, FUNCTION),
    ABI_REF(FRAME_LAYOUT, frame_layout, function_index, FUNCTION),
    ABI_REF(FRAME_LAYOUT, frame_layout, shadow_frame_type_index, TYPE),
    ABI_REF(SOURCE_LOCATION, source_location, file_index, SOURCE_FILE),
    ABI_REF(STORAGE, storage, module_index, MODULE),
    ABI_REF(STORAGE, storage, symbol_index, SYMBOL),
    ABI_REF(STORAGE, storage, type_index, TYPE),
    ABI_REF(STORAGE, storage, instance_shape_index, SHAPE),
    ABI_REF(TLS_RELOCATION, tls_relocation, template_storage_index, STORAGE),
    ABI_REF(TLS_RELOCATION, tls_relocation, referenced_storage_index, STORAGE),
    ABI_REF(NAME_SLOT, name_slot, symbol_index, SYMBOL),
    ABI_REF(REQUIREMENT, requirement, contract_type_index, TYPE),
    ABI_REF(REQUIREMENT, requirement, required_layout_index, LAYOUT),
    ABI_REF(REQUIREMENT, requirement, requesting_module_index, MODULE),
};

#undef ABI_REF

typedef struct {
    uint32_t row_kind;
    size_t offset;
    uint32_t stream_kind;
    uint32_t target_kind;
} abi_range_column_t;

#define ABI_RANGE(K, T, F, STREAM, TARGET) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##STREAM, N00B_ABI_TABLE_##TARGET}
#define ABI_RAW_RANGE(K, T, F, STREAM) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F), \
     N00B_ABI_TABLE_##STREAM, 0}

static const abi_range_column_t abi_range_columns[] = {
    ABI_RANGE(PACKAGE, package, modules, U32_STREAM, MODULE),
    ABI_RANGE(MODULE, module, imports, U32_STREAM, MODULE),
    ABI_RANGE(MODULE, module, static_storage, U32_STREAM, STORAGE),
    ABI_RANGE(MODULE, module, tls_storage, U32_STREAM, STORAGE),
    ABI_RANGE(SCC, scc, members, U32_STREAM, MODULE),
    ABI_RAW_RANGE(SCC, scc, lexical_order, U32_STREAM),
    ABI_RANGE(TYPE, type, components, U32_STREAM, TYPE),
    ABI_RAW_RANGE(TYPE, type, fields_or_arms, U32_STREAM),
    ABI_RANGE(TYPE, type, constraints, U32_STREAM, CONSTRAINT),
    ABI_RANGE(CONSTRAINT, constraint, arguments, U32_STREAM, TYPE),
    ABI_RAW_RANGE(SHAPE, shape, extents, U64_STREAM),
    ABI_RANGE(LAYOUT, layout, occupied_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(LAYOUT, layout, reserved_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(FIELD, field, packed_runs, U32_STREAM, PACKED_RUN),
    ABI_RANGE(FIELD, field, physical_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(ARM, arm, field.packed_runs, U32_STREAM, PACKED_RUN),
    ABI_RANGE(ARM, arm, field.physical_runs, U32_STREAM, PHYSICAL_RUN),
    ABI_RANGE(TRACE, trace, fixed_children, U32_STREAM, TRACE_CHILD),
    ABI_RANGE(TRACE, trace, arms, U32_STREAM, TRACE_ARM),
    ABI_RANGE(TRACE_ARM, trace_arm, children, U32_STREAM, TRACE_CHILD),
    ABI_RANGE(FUNCTION, function, parameters, U32_STREAM, PARAMETER),
    ABI_RANGE(FUNCTION, function, keywords, U32_STREAM, PARAMETER),
    ABI_RANGE(FUNCTION, function, constraints, U32_STREAM, CONSTRAINT),
    ABI_RANGE(FUNCTION, function, code_ranges, U32_STREAM, CODE_RANGE),
    ABI_RANGE(FUNCTION, function, frame_layouts, U32_STREAM, FRAME_LAYOUT),
    ABI_RANGE(CALL_SIGNATURE, call_signature, positional_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(CALL_SIGNATURE, call_signature, keyword_parameters,
        U32_STREAM, PARAMETER),
    ABI_RANGE(SCOPE, scope, children, U32_STREAM, SCOPE),
    ABI_RANGE(SCOPE, scope, symbols, U32_STREAM, SYMBOL),
    ABI_RANGE(SYMBOL, symbol, definitions, U32_STREAM, SOURCE_LOCATION),
    ABI_RANGE(SYMBOL, symbol, uses, U32_STREAM, SOURCE_LOCATION),
    ABI_RANGE(DISPATCH, dispatch, entries, U32_STREAM, DISPATCH_ENTRY),
    ABI_RAW_RANGE(FRAME_LAYOUT, frame_layout, pointer_bitmap_bytes,
        BYTE_STREAM),
    ABI_RANGE(STORAGE, storage, tls_relocations, U32_STREAM, TLS_RELOCATION),
};

#undef ABI_RANGE
#undef ABI_RAW_RANGE

static bool
remap_index(const n00bb_abi_index_plan_t *plan, size_t input_index,
            uint32_t kind, uint32_t local, uint32_t *out)
{
    if (!local && kind != N00B_ABI_TABLE_TYPE) {
        *out = 0;
        return true;
    }
    return n00bb_abi_index_plan_map(plan, input_index, kind, local, out);
}

static uint32_t
type_member_kind(const n00b_abi_type_v1 *type)
{
    switch (type->kind) {
    case N00B_ABI_TYPE_RECORD: return N00B_ABI_TABLE_FIELD;
    case N00B_ABI_TYPE_VARIANT: return N00B_ABI_TABLE_ARM;
    case N00B_ABI_TYPE_ENUM:
    case N00B_ABI_TYPE_FLAGSET: return N00B_ABI_TABLE_ENUM_ITEM;
    default: return 0;
    }
}

static bool
remap_range(const n00bb_abi_object_view_t *view,
            const n00bb_abi_index_plan_t *plan, size_t input_index,
            n00b_abi_range_v1 source, uint32_t stream_kind,
            uint32_t target_kind, n00bb_abi_u32_stream_build_t *stream,
            uint32_t *out_first)
{
    if (!source.count) {
        *out_first = 0;
        return true;
    }
    const n00bb_abi_directory_entry_t *table =
        n00bb_abi_object_table(view, stream_kind);
    if (!table || source.first >= table->record_count
        || source.count > table->record_count - source.first) return false;
    uint32_t first, last;
    if (!n00bb_abi_index_plan_map(plan, input_index, stream_kind,
            source.first, &first)
        || !n00bb_abi_index_plan_map(plan, input_index, stream_kind,
            source.first + source.count - 1, &last)
        || last < first || last - first != source.count - 1) return false;
    if (stream_kind == N00B_ABI_TABLE_U32_STREAM) {
        if (!stream || !stream->values || !stream->kinds
            || last >= stream->count || !target_kind) return false;
        for (uint32_t i = 0; i < source.count; i++) {
            const uint8_t *slot = n00bb_abi_object_row(view,
                stream_kind, source.first + i);
            uint32_t local = core_le_load_u32(slot);
            uint32_t mapped = local;
            if (target_kind != UINT32_MAX
                && !remap_index(plan, input_index, target_kind,
                    local, &mapped)) return false;
            size_t at = (size_t)first + i;
            if (stream->kinds[at]
                && (stream->kinds[at] != target_kind
                    || stream->values[at] != mapped)) return false;
            stream->kinds[at] = target_kind;
            stream->values[at] = mapped;
        }
    }
    *out_first = first;
    return true;
}

bool
n00bb_abi_remap_row(const n00bb_abi_selected_objects_t *selected,
                    const n00bb_abi_index_plan_t *plan, size_t input_index,
                    uint32_t kind, uint32_t local_index, void *out,
                    size_t out_bytes, n00bb_abi_u32_stream_build_t *stream)
{
    if (!selected || !plan || !out || input_index >= selected->count)
        return false;
    const n00bb_abi_object_view_t *view = &selected->views[input_index];
    const n00bb_abi_directory_entry_t *table =
        n00bb_abi_object_table(view, kind);
    const void *source = n00bb_abi_object_row(view, kind, local_index);
    if (!table || !source || out_bytes < table->record_bytes
        || link_kind_is_stream(kind)) return false;
    memmove(out, source, table->record_bytes);
    uint8_t *bytes = out;
    for (size_t i = 0; i < sizeof(abi_index_columns)
         / sizeof(abi_index_columns[0]); i++) {
        const abi_ref_column_t *field = &abi_index_columns[i];
        if (field->row_kind != kind) continue;
        if (field->offset > table->record_bytes
            || sizeof(uint32_t) > table->record_bytes - field->offset)
            return false;
        uint32_t local = core_le_load_u32(bytes + field->offset), mapped;
        if (!remap_index(plan, input_index, field->target_kind,
                local, &mapped)) return false;
        core_le_store_u32(bytes + field->offset, mapped);
    }
    if (kind == N00B_ABI_TABLE_SPECIALIZATION_LINK) {
        uint32_t local = core_le_load_u32(bytes), mapped;
        if (!remap_index(plan, input_index, N00B_ABI_TABLE_TYPE,
                local, &mapped)) return false;
        core_le_store_u32(bytes, mapped);
    }
    for (size_t i = 0; i < sizeof(abi_range_columns)
         / sizeof(abi_range_columns[0]); i++) {
        const abi_range_column_t *field = &abi_range_columns[i];
        if (field->row_kind != kind) continue;
        if (field->offset > table->record_bytes
            || sizeof(n00b_abi_range_v1)
                > table->record_bytes - field->offset) return false;
        n00b_abi_range_v1 range;
        memcpy(&range, bytes + field->offset, sizeof(range));
        uint32_t target = field->target_kind;
        if (kind == N00B_ABI_TABLE_SCC
            && field->offset == offsetof(n00b_abi_scc_v1, lexical_order))
            target = UINT32_MAX;
        if (kind == N00B_ABI_TABLE_TYPE
            && field->offset == offsetof(n00b_abi_type_v1, fields_or_arms))
            target = type_member_kind((const n00b_abi_type_v1 *)source);
        uint32_t first;
        if (!remap_range(view, plan, input_index, range,
                field->stream_kind, target, stream, &first)) return false;
        core_le_store_u32(bytes + field->offset, first);
    }
    if (kind == N00B_ABI_TABLE_TYPE) {
        n00b_abi_type_v1 *type = out;
        uint32_t start = type->specialization_start_ix;
        uint32_t end = type->specialization_end_ix;
        if (start || end) {
            uint32_t mapped_start, mapped_last;
            if (!end || end <= start
                || !n00bb_abi_index_plan_map(plan, input_index,
                    N00B_ABI_TABLE_SPECIALIZATION_LINK, start,
                    &mapped_start)
                || !n00bb_abi_index_plan_map(plan, input_index,
                    N00B_ABI_TABLE_SPECIALIZATION_LINK, end - 1,
                    &mapped_last)
                || mapped_last - mapped_start != end - start - 1
                || mapped_last == UINT32_MAX) return false;
            type->specialization_start_ix = mapped_start;
            type->specialization_end_ix = mapped_last + 1;
        }
    }
    return true;
}

void
n00bb_abi_manifest_rows_release(n00bb_abi_manifest_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->packages);
    core_free(rows->sccs);
    core_free(rows->u32_stream);
    *rows = (n00bb_abi_manifest_rows_t){0};
}

bool
n00bb_abi_rebuild_manifests(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    n00bb_abi_manifest_rows_t *out)
{
    if (!selected || !selected->views || !selected->count || !plan || !out)
        return false;
    uint32_t packages, sccs, base_stream;
    if (!n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_PACKAGE, &packages)
        || !n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_SCC, &sccs)
        || !n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_U32_STREAM,
            &base_stream)
        || packages < 2 || !sccs
        || selected->count > (UINT32_MAX - base_stream) / 3)
        return false;
    uint32_t capacity = base_stream + 3 * (uint32_t)selected->count;
    n00bb_abi_manifest_rows_t built = {
        .packages = core_alloc_array(n00b_abi_package_v1, packages),
        .package_count = packages,
        .sccs = core_alloc_array(n00b_abi_scc_v1, sccs),
        .scc_count = sccs,
        .u32_stream = core_alloc_array(uint32_t, capacity),
    };
    uint32_t *kinds = core_alloc_array(uint32_t, base_stream);
    uint32_t *package_members = core_alloc_array(uint32_t, packages);
    uint32_t *package_written = core_alloc_array(uint32_t, packages);
    uint32_t *scc_members = core_alloc_array(uint32_t, sccs);
    uint32_t *scc_written = core_alloc_array(uint32_t, sccs);
    bool *package_seen = core_alloc_array(bool, packages);
    bool *scc_seen = core_alloc_array(bool, sccs);
    n00bb_abi_u32_stream_build_t stream = {
        .values = built.u32_stream, .kinds = kinds, .count = base_stream,
    };
    bool valid = true;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *package_table =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_PACKAGE);
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, 1);
        if (!package_table || !module || !module->package_index) {
            valid = false;
            break;
        }
        uint32_t package_index, module_index;
        valid = n00bb_abi_index_plan_map(plan, input,
            N00B_ABI_TABLE_PACKAGE, module->package_index, &package_index)
            && n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_MODULE, 1, &module_index)
            && package_index && package_index < packages && module_index;
        for (uint32_t local = 1; valid
             && local < package_table->record_count; local++) {
            uint32_t final;
            n00b_abi_package_v1 row;
            valid = n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_PACKAGE, local, &final)
                && final && final < packages
                && n00bb_abi_remap_row(selected, plan, input,
                    N00B_ABI_TABLE_PACKAGE, local, &row, sizeof(row),
                    &stream) && !row.reserved_zero;
            if (!valid) break;
            if (package_seen[final]) {
                const n00b_abi_package_v1 *first =
                    &built.packages[final];
                valid = first->parent_package_index
                            == row.parent_package_index
                    && first->visibility == row.visibility
                    && first->generation == row.generation;
            }
            else {
                row.modules = (n00b_abi_range_v1){0};
                built.packages[final] = row;
                package_seen[final] = true;
            }
        }
        if (!valid) break;
        const n00b_abi_package_v1 *original = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_PACKAGE, module->package_index);
        n00b_abi_package_v1 active;
        valid = original && original->modules.count == 1
            && n00bb_abi_remap_row(selected, plan, input,
                N00B_ABI_TABLE_PACKAGE, module->package_index, &active,
                sizeof(active), &stream)
            && active.modules.first < base_stream
            && built.u32_stream[active.modules.first] == module_index;
        if (!valid) break;
        package_members[package_index]++;

        if (!module->scc_index) continue;
        uint32_t scc_index;
        n00b_abi_scc_v1 row;
        valid = n00bb_abi_index_plan_map(plan, input, N00B_ABI_TABLE_SCC,
            module->scc_index, &scc_index)
            && scc_index && scc_index < sccs
            && n00bb_abi_remap_row(selected, plan, input,
                N00B_ABI_TABLE_SCC, module->scc_index, &row, sizeof(row),
                &stream)
            && row.members.count == 1 && row.lexical_order.count == 1
            && row.members.first < base_stream
            && row.lexical_order.first < base_stream
            && built.u32_stream[row.members.first] == module_index;
        if (!valid) break;
        if (scc_seen[scc_index]) {
            const n00b_abi_scc_v1 *first = &built.sccs[scc_index];
            valid = first->interface_identity == row.interface_identity
                && first->freshness_identity == row.freshness_identity;
        }
        else {
            row.members = (n00b_abi_range_v1){0};
            row.lexical_order = (n00b_abi_range_v1){0};
            built.sccs[scc_index] = row;
            scc_seen[scc_index] = true;
        }
        if (valid) scc_members[scc_index]++;
    }
    uint32_t cursor = base_stream;
    for (uint32_t i = 1; valid && i < packages; i++) {
        if (!package_seen[i]) { valid = false; break; }
        built.packages[i].modules = (n00b_abi_range_v1){
            .first = cursor, .count = package_members[i],
        };
        cursor += package_members[i];
    }
    for (uint32_t i = 1; valid && i < sccs; i++) {
        if (!scc_seen[i] || !scc_members[i]) { valid = false; break; }
        built.sccs[i].members = (n00b_abi_range_v1){
            .first = cursor, .count = scc_members[i],
        };
        cursor += scc_members[i];
        built.sccs[i].lexical_order = (n00b_abi_range_v1){
            .first = cursor, .count = scc_members[i],
        };
        cursor += scc_members[i];
    }
    valid = valid && cursor <= capacity;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, 1);
        uint32_t package_index, module_index;
        valid = module && n00bb_abi_index_plan_map(plan, input,
            N00B_ABI_TABLE_PACKAGE, module->package_index, &package_index)
            && n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_MODULE, 1, &module_index);
        if (!valid) break;
        n00b_abi_range_v1 range = built.packages[package_index].modules;
        built.u32_stream[range.first + package_written[package_index]++] =
            module_index;
        if (!module->scc_index) continue;
        uint32_t scc_index;
        valid = n00bb_abi_index_plan_map(plan, input, N00B_ABI_TABLE_SCC,
            module->scc_index, &scc_index);
        if (!valid) break;
        const n00b_abi_scc_v1 *source_scc = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SCC, module->scc_index);
        const uint8_t *ordinal_slot = source_scc
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_U32_STREAM,
                source_scc->lexical_order.first) : nullptr;
        if (!ordinal_slot) { valid = false; break; }
        uint32_t ordinal = core_le_load_u32(ordinal_slot);
        range = built.sccs[scc_index].members;
        if (ordinal >= range.count || scc_written[scc_index] >= range.count
            || built.u32_stream[built.sccs[scc_index].lexical_order.first
                                    + ordinal]) {
            valid = false;
            break;
        }
        built.u32_stream[range.first + scc_written[scc_index]++] =
            module_index;
        built.u32_stream[built.sccs[scc_index].lexical_order.first
                          + ordinal] = module_index;
    }
    for (uint32_t i = 1; valid && i < packages; i++)
        valid = package_written[i] == package_members[i];
    for (uint32_t i = 1; valid && i < sccs; i++) {
        valid = scc_written[i] == scc_members[i];
        n00b_abi_range_v1 order = built.sccs[i].lexical_order;
        for (uint32_t j = 0; valid && j < order.count; j++)
            valid = built.u32_stream[order.first + j] != 0;
    }
    built.u32_count = cursor;
    core_free(scc_seen);
    core_free(package_seen);
    core_free(scc_written);
    core_free(scc_members);
    core_free(package_written);
    core_free(package_members);
    core_free(kinds);
    if (!valid) {
        n00bb_abi_manifest_rows_release(&built);
        return false;
    }
    *out = built;
    return true;
}

typedef struct {
    uint32_t kind;
    size_t offset;
} abi_string_column_t;

#define ABI_STRING_FIELD(K, T, F) \
    {N00B_ABI_TABLE_##K, offsetof(n00b_abi_##T##_v1, F)}

static const abi_string_column_t abi_string_columns[] = {
    ABI_STRING_FIELD(PACKAGE, package, name),
    ABI_STRING_FIELD(MODULE, module, full_path),
    ABI_STRING_FIELD(TYPE, type, canonical_name),
    ABI_STRING_FIELD(ENUM_ITEM, enum_item, name),
    ABI_STRING_FIELD(CONSTRAINT, constraint, name),
    ABI_STRING_FIELD(CONSTRAINT, constraint, scope_module_name),
    ABI_STRING_FIELD(CONSTRAINT, constraint, scope_package_name),
    ABI_STRING_FIELD(FIELD, field, name),
    ABI_STRING_FIELD(ARM, arm, field.name),
    ABI_STRING_FIELD(FUNCTION, function, name),
    ABI_STRING_FIELD(PARAMETER, parameter, name),
    ABI_STRING_FIELD(SCOPE, scope, kind_name),
    ABI_STRING_FIELD(SYMBOL, symbol, name),
    ABI_STRING_FIELD(SYMBOL, symbol, storage_class_name),
    ABI_STRING_FIELD(NATIVE_BINDING, native_binding, native_name),
    ABI_STRING_FIELD(SOURCE_FILE, source_file, path),
    ABI_STRING_FIELD(STATIC_STRING, static_string, value),
    ABI_STRING_FIELD(RUNTIME_CONFIG, runtime_config, memory_manager_name),
    ABI_STRING_FIELD(REQUIREMENT, requirement, full_path),
};

#undef ABI_STRING_FIELD

bool
n00bb_abi_repool_row_strings(
    const n00bb_abi_object_view_t *view, uint32_t kind,
    uint32_t local_index, uint32_t final_index,
    void *out_row, size_t out_bytes, n00bb_abi_name_pool_t *pool)
{
    if (!view || !out_row || !pool) return false;
    const n00bb_abi_directory_entry_t *table =
        n00bb_abi_object_table(view, kind);
    const uint8_t *source = n00bb_abi_object_row(view, kind, local_index);
    if (!table || !source || out_row == (const void *)source
        || out_bytes < table->record_bytes) return false;
    uint8_t *destination = out_row;
    for (size_t i = 0; i < sizeof(abi_string_columns)
         / sizeof(abi_string_columns[0]); i++) {
        const abi_string_column_t *field = &abi_string_columns[i];
        if (field->kind != kind) continue;
        if (field->offset > table->record_bytes
            || sizeof(n00b_abi_string_v1)
                > table->record_bytes - field->offset) return false;
        const n00b_abi_string_v1 *value =
            (const void *)(source + field->offset);
        n00bb_abi_string_header_t header;
        n00bb_abi_string_view_t decoded;
        if (!n00bb_abi_string_decode((const uint8_t *)value,
                sizeof(*value), &header)
            || !n00bb_abi_object_string(view, value, &decoded)
            || !n00bb_abi_name_pool_encode_tagged(pool,
                core_string_borrow((char *)decoded.bytes, decoded.length),
                (n00b_abi_string_encoding_v1)header.encoding,
                header.tag == N00BB_ABI_STRING_SPAN
                    ? N00B_ABI_STRING_SPAN : N00B_ABI_STRING_LONG,
                kind, final_index, field->offset,
                (void *)(destination + field->offset))) return false;
    }
    return true;
}
