#include "abi_trace_leaf.h"
#include "abi_link.h"
#include "abi_member_facts.h"
#include "abi_parameter_contract.h"
#include "abi_shape.h"
#include "abi_value_schema.h"
#include "abi_type_variable.h"

#include "abi_ref_schema.h"
#include "abi_string.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
#include <stdio.h>
#include <string.h>

bool
n00bb_abi_object_open(const core_target_desc_t *target,
                      const uint8_t *object, size_t object_bytes,
                      n00bb_abi_object_view_t *out)
{
    n00bb_abi_image_key_t key;
    if (!out || !object || !object_bytes
        || !n00bb_abi_image_key_for_target(target, &key)) return false;
    core_string_t segment = {}, name = {};
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = CORE_STRING_STATIC(".data.rel.ro.n00b_meta");
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        segment = CORE_STRING_STATIC("__DATA_CONST");
        name = CORE_STRING_STATIC("__n00b_meta");
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = CORE_STRING_STATIC(".n00b$M");
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

typedef struct {
    const core_target_desc_t *target;
    n00bb_abi_archive_inputs_t inputs;
    size_t capacity;
} archive_inputs_build_t;

static bool
archive_inputs_visit(const core_native_archive_member_t *member, void *user)
{
    archive_inputs_build_t *build = user;
    n00bb_abi_object_view_t view = {0};
    if (!n00bb_abi_object_open(build->target, member->bytes,
                               member->size, &view)) return true;
    const n00b_abi_module_v1 *module = n00bb_abi_object_row(&view,
        N00B_ABI_TABLE_MODULE, 1);
    n00bb_abi_string_view_t name;
    bool valid = module
        && n00bb_abi_object_string(&view, &module->full_path, &name)
        && name.length;
    if (valid) {
        if (build->inputs.count == build->capacity) {
            size_t next = build->capacity ? build->capacity * 2 : 4;
            if (next < build->capacity
                || next > SIZE_MAX / sizeof(*build->inputs.objects))
                valid = false;
            else {
                build->inputs.objects = core_realloc(build->inputs.objects,
                    next * sizeof(*build->inputs.objects));
                build->capacity = next;
            }
        }
        if (valid) build->inputs.objects[build->inputs.count++] =
            (n00bb_abi_object_input_t){
                .module = core_string_borrow((const char *)name.bytes,
                                             name.length),
                .object = member->bytes,
                .object_bytes = member->size,
            };
    }
    n00bb_abi_object_close(&view);
    return valid;
}

bool
n00bb_abi_archive_inputs_open(const core_target_desc_t *target,
                              const void *archive, size_t archive_bytes,
                              n00bb_abi_archive_inputs_t *out)
{
    if (!target || !out) return false;
    archive_inputs_build_t build = {.target = target};
    if (!core_native_archive_visit(archive, archive_bytes,
                                   archive_inputs_visit, &build)) {
        core_free(build.inputs.objects);
        return false;
    }
    *out = build.inputs;
    return true;
}

void
n00bb_abi_archive_inputs_release(n00bb_abi_archive_inputs_t *inputs)
{
    if (!inputs) return;
    core_free(inputs->objects);
    *inputs = (n00bb_abi_archive_inputs_t){0};
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

    core_string_t segment = view->format == CORE_TARGET_OBJECT_MACH_O
        ? CORE_STRING_STATIC("__TEXT") : (core_string_t){};
    core_string_t name = view->format == CORE_TARGET_OBJECT_MACH_O
        ? CORE_STRING_STATIC("__n00b_strings")
        : view->format == CORE_TARGET_OBJECT_COFF
            ? CORE_STRING_STATIC(".rdata$S")
            : CORE_STRING_STATIC(".rodata.n00b_strings");
    bool found = false;
    uint64_t pool_offset = 0;
    core_object_section_t pool = {0};
    uint64_t field_offset = address - base;
    for (uint32_t i = 0; i < view->metadata.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        if (!core_object_absolute64_relocation_at(&view->metadata,
                view->format, i, &relocation)) return false;
        if (relocation.section_offset != field_offset) continue;
        if (found) return false;
        if (view->format == CORE_TARGET_OBJECT_MACH_O) {
            if (!relocation.target_is_section
                || !core_object_section_at(view->object, view->object_bytes,
                    view->format, relocation.native_symbol_index, &pool)
                || relocation.addend_bits < pool.virtual_address)
                return false;
            pool_offset = relocation.addend_bits - pool.virtual_address;
        }
        else {
            core_object_symbol_t symbol;
            if (relocation.target_is_section
                || !core_object_symbol_at(view->object,
                    view->object_bytes, view->format, &view->metadata,
                    relocation.native_symbol_index, &symbol)
                || symbol.name.length != sizeof("n00b_string_pool") - 1
                || memcmp(symbol.name.bytes, "n00b_string_pool",
                          symbol.name.length)
                || !symbol.native_section_index
                || !core_object_section_at(view->object, view->object_bytes,
                    view->format, symbol.native_section_index, &pool))
                return false;
            pool_offset = relocation.addend_bits;
        }
        if (!core_string_eq(core_string_borrow((const char *)pool.name,
                                               pool.name_length), name)
            || (segment.data && !core_string_eq(core_string_borrow(
                    (const char *)pool.segment, pool.segment_length),
                    segment)))
            return false;
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

static bool
names_specialization_module(core_string_t module)
{
    static const char marker[] = "$spec$";
    size_t marker_length = sizeof(marker) - 1;
    for (size_t at = 0; module.data && at + marker_length <= module.u8_bytes;
         at++)
        if (!memcmp(module.data + at, marker, marker_length)) return true;
    return false;
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
        .views = core_alloc_array(n00bb_abi_object_view_t,
                                  count + candidate_count),
        .count = count,
    };
    const n00bb_abi_object_input_t **extra_candidates = core_alloc_array(
        const n00bb_abi_object_input_t *, candidate_count + 1);
    size_t extra_count = 0;
    bool valid = modules && selected.views && extra_candidates;
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
        if (!entry) {
            if (names_specialization_module(candidate->module))
                extra_candidates[extra_count++] = candidate;
            continue;
        }
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
    for (size_t i = 0; valid && i < extra_count; i++) {
        valid = n00bb_abi_object_open(target, extra_candidates[i]->object,
            extra_candidates[i]->object_bytes,
            &selected.views[selected.count]);
        if (valid) selected.count++;
    }
    core_free(extra_candidates);
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
    const n00bb_abi_selected_objects_t *selected;
    const n00bb_abi_object_view_t *view;
    const void *row;
    const uint8_t *bytes;
    size_t length;
    uint64_t scalar;
    uint64_t auxiliary;
    size_t input_index;
    uint32_t local_index;
    core_buffer_t *owned_bytes;
    bool unique;
} abi_link_key_t;

static bool nonconcrete_key_init(abi_link_key_t *key);
static bool link_key_from_owner_type(abi_link_key_t *key,
                                     uint32_t type_index, uint32_t kind);
static bool link_key_from_type_argument(abi_link_key_t *key);
static bool link_key_from_member(abi_link_key_t *key, uint32_t kind);

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
        if (!n00bb_abi_type_variable_fields_valid(type)) return false;
        if (type->kind == N00B_ABI_TYPE_VARIABLE) {
            return nonconcrete_key_init(key);
        }
        else {
            if (!type->typehash) return false;
            bool bare_nominal = !type->named_arguments.count
                && (type->kind == N00B_ABI_TYPE_RECORD
                    || type->kind == N00B_ABI_TYPE_VARIANT
                    || type->kind == N00B_ABI_TYPE_ENUM
                    || type->kind == N00B_ABI_TYPE_FLAGSET
                    || type->kind == N00B_ABI_TYPE_CALLBACK
                    || type->kind == N00B_ABI_TYPE_WRAPPER);
            if ((type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
                && type->kind != N00B_ABI_TYPE_TRAIT
                && !bare_nominal)
                return nonconcrete_key_init(key);
            key->scalar = type->typehash;
        }
        key->bytes = (const uint8_t *)&key->scalar;
        key->length = sizeof(key->scalar);
        return true;
    }
    case N00B_ABI_TABLE_LAYOUT: {
        const n00b_abi_layout_v1 *layout = key->row;
        return link_key_from_owner_type(key, layout->type_index, kind);
    }
    case N00B_ABI_TABLE_TRACE: {
        const n00b_abi_trace_v1 *trace = key->row;
        return link_key_from_owner_type(key, trace->type_index, kind);
    }
    case N00B_ABI_TABLE_CALL_SIGNATURE: {
        const n00b_abi_call_signature_v1 *signature = key->row;
        return link_key_from_owner_type(key, signature->type_index, kind);
    }
    case N00B_ABI_TABLE_TYPE_ARGUMENT:
        return link_key_from_type_argument(key);
    case N00B_ABI_TABLE_FIELD:
    case N00B_ABI_TABLE_ARM:
    case N00B_ABI_TABLE_ENUM_ITEM:
    case N00B_ABI_TABLE_TRAIT_METHOD:
        return link_key_from_member(key, kind);
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
    case N00B_ABI_TABLE_STATIC_STRING: {
        const n00b_abi_string_v1 *value =
            &((const n00b_abi_static_string_v1 *)key->row)->value;
        n00bb_abi_string_header_t header;
        if (!n00bb_abi_string_decode((const uint8_t *)value,
                sizeof(*value), &header)) return false;
        key->auxiliary = ((uint64_t)header.tag << 32) | header.encoding;
        return link_key_string(key, value);
    }
    default:
        key->unique = true;
        return true;
    }
}

static bool
link_key_from_owner_type(abi_link_key_t *key, uint32_t type_index,
                         uint32_t kind)
{
    const n00b_abi_type_v1 *type = n00bb_abi_object_row(key->view,
        N00B_ABI_TABLE_TYPE, type_index);
    if (!type) return false;
    uint32_t owner_index = kind == N00B_ABI_TABLE_LAYOUT
        ? type->layout_index : kind == N00B_ABI_TABLE_TRACE
            ? type->tracing_index : type->call_signature_index;
    if (owner_index != key->local_index) return false;
    abi_link_key_t owner = *key;
    owner.row = type;
    owner.local_index = type_index;
    owner.bytes = nullptr;
    owner.owned_bytes = nullptr;
    owner.unique = false;
    if (!link_key_init(&owner, N00B_ABI_TABLE_TYPE)) {
        core_buffer_free(owner.owned_bytes);
        return false;
    }
    key->scalar = owner.scalar;
    key->auxiliary = owner.auxiliary;
    key->owned_bytes = owner.owned_bytes;
    key->unique = owner.unique;
    key->bytes = owner.bytes == (const uint8_t *)&owner.scalar
        ? (const uint8_t *)&key->scalar : owner.bytes;
    key->length = owner.length;
    return true;
}

static bool
link_key_from_type_argument(abi_link_key_t *key)
{
    const n00b_abi_type_argument_v1 *argument = key->row;
    const n00b_abi_type_v1 *type = n00bb_abi_object_row(key->view,
        N00B_ABI_TABLE_TYPE, argument->owner_type_index);
    if (!type || argument->ordinal >= type->named_arguments.count
        || type->named_arguments.first + argument->ordinal
            != key->local_index) return false;
    abi_link_key_t owner = *key;
    owner.row = type;
    owner.local_index = argument->owner_type_index;
    owner.owned_bytes = nullptr;
    owner.unique = false;
    if (!link_key_init(&owner, N00B_ABI_TABLE_TYPE)) {
        core_buffer_free(owner.owned_bytes);
        return false;
    }
    key->owned_bytes = core_buffer_empty();
    core_buffer_append(key->owned_bytes, (const char *)owner.bytes,
                       owner.length);
    core_buffer_append(key->owned_bytes, (const char *)&argument->ordinal,
                       sizeof(argument->ordinal));
    core_buffer_free(owner.owned_bytes);
    key->bytes = (const uint8_t *)key->owned_bytes->data;
    key->length = key->owned_bytes->byte_len;
    return true;
}

static bool
link_key_from_member(abi_link_key_t *key, uint32_t kind)
{
    n00bb_abi_member_facts_t facts = n00bb_abi_member_facts(kind, key->row);
    const n00b_abi_type_v1 *type = facts.parent
        ? n00bb_abi_object_row(key->view, N00B_ABI_TABLE_TYPE,
                               facts.parent) : nullptr;
    if (!type) return false;
    abi_link_key_t owner = *key;
    owner.row = type;
    owner.local_index = facts.parent;
    owner.owned_bytes = nullptr;
    owner.unique = false;
    if (!link_key_init(&owner, N00B_ABI_TABLE_TYPE)) {
        core_buffer_free(owner.owned_bytes);
        return false;
    }
    key->unique = owner.unique;
    key->auxiliary = owner.auxiliary;
    if (!key->unique) {
        key->owned_bytes = core_buffer_empty();
        core_buffer_append(key->owned_bytes, (const char *)owner.bytes,
                           owner.length);
        core_buffer_append(key->owned_bytes, (const char *)&facts.ordinal,
                           sizeof(facts.ordinal));
        key->bytes = (const uint8_t *)key->owned_bytes->data;
        key->length = key->owned_bytes->byte_len;
    }
    core_buffer_free(owner.owned_bytes);
    return true;
}

static bool
valid_index_range(const n00bb_abi_object_view_t *view,
                  n00b_abi_range_v1 range)
{
    if (!range.count) return !range.first;
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_U32_STREAM);
    return table && range.first < table->record_count
        && range.count <= table->record_count - range.first;
}

static const void *
indexed_range_row(const n00bb_abi_object_view_t *view,
                  n00b_abi_range_v1 range, uint32_t ordinal,
                  uint32_t target_kind)
{
    if (ordinal >= range.count || !valid_index_range(view, range))
        return nullptr;
    const uint8_t *slot = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_U32_STREAM, range.first + ordinal);
    uint32_t index = core_le_load_u32(slot);
    if (!index && target_kind != N00B_ABI_TABLE_TYPE) return nullptr;
    return n00bb_abi_object_row(view, target_kind, index);
}

static void
key_append_u64(core_buffer_t *out, uint64_t value)
{
    core_buffer_append(out, (const char *)&value, sizeof(value));
}

static bool
key_append_string(const n00bb_abi_object_view_t *view,
                  const n00b_abi_string_v1 *value, core_buffer_t *out)
{
    n00bb_abi_string_view_t string;
    if (!n00bb_abi_object_string(view, value, &string)) return false;
    key_append_u64(out, string.length);
    core_buffer_append(out, (const char *)string.bytes, string.length);
    return true;
}

static bool
find_imported_variable(const n00bb_abi_selected_objects_t *selected,
                       const n00bb_abi_object_view_t *requester,
                       const n00b_abi_type_v1 *foreign,
                       const n00bb_abi_object_view_t **owner_view,
                       size_t *owner_input,
                       const n00b_abi_type_v1 **owner_type)
{
    const n00b_abi_requirement_v1 *requirement = n00bb_abi_object_row(
        requester, N00B_ABI_TABLE_REQUIREMENT,
        foreign->typevar_import_requirement_index);
    n00bb_abi_string_view_t path;
    if (!selected || !requirement
        || (requirement->symbol_kind != N00B_ABI_SYMBOL_FUNCTION
            && requirement->symbol_kind != N00B_ABI_SYMBOL_TYPE)
        || !n00bb_abi_object_string(requester, &requirement->full_path,
                                    &path)) return false;
    const n00b_abi_type_v1 *contract =
        requirement->contract_type_index
            ? n00bb_abi_object_row(requester, N00B_ABI_TABLE_TYPE,
                requirement->contract_type_index) : nullptr;
    if (requirement->contract_type_index && !contract) return false;
    size_t matches = 0;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *provider = &selected->views[input];
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(provider,
            N00B_ABI_TABLE_MODULE, 1);
        n00bb_abi_string_view_t module_name;
        if (!module || !n00bb_abi_object_string(provider,
                &module->full_path, &module_name)) return false;
        if (path.length <= module_name.length + 1
            || memcmp(path.bytes, module_name.bytes, module_name.length)
            || path.bytes[module_name.length] != '.') continue;
        const n00bb_abi_directory_entry_t *symbols = n00bb_abi_object_table(
            provider, N00B_ABI_TABLE_SYMBOL);
        const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
            provider, N00B_ABI_TABLE_TYPE);
        if (!symbols || !types) return false;
        for (uint32_t i = 1; i < symbols->record_count; i++) {
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(provider,
                N00B_ABI_TABLE_SYMBOL, i);
            if (!symbol || symbol->kind != requirement->symbol_kind
                || symbol->module_index != 1
                || !symbol->declaration_location_index) continue;
            n00bb_abi_string_view_t name;
            if (!n00bb_abi_object_string(provider, &symbol->name, &name))
                return false;
            size_t suffix = module_name.length + 1;
            if (name.length != path.length - suffix
                || memcmp(name.bytes, path.bytes + suffix, name.length))
                continue;
            if (contract) {
                const n00b_abi_type_v1 *signature = n00bb_abi_object_row(
                    provider, N00B_ABI_TABLE_TYPE, symbol->type_index);
                if (!signature || signature->typehash != contract->typehash)
                    continue;
            }
            for (uint32_t j = 1; j < types->record_count; j++) {
                const n00b_abi_type_v1 *candidate = n00bb_abi_object_row(
                    provider, N00B_ABI_TABLE_TYPE, j);
                if (!candidate || candidate->kind != N00B_ABI_TYPE_VARIABLE
                    || candidate->typevar_import_requirement_index
                    || candidate->typevar_owner_type_index
                    || candidate->typevar_ordinal != foreign->typevar_ordinal)
                    continue;
                const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(
                    provider, N00B_ABI_TABLE_SCOPE,
                    candidate->typevar_owner_scope_index);
                if (!scope || scope->module_index != 1
                    || scope->source_location_index
                        != symbol->declaration_location_index) continue;
                *owner_view = provider;
                *owner_input = input;
                *owner_type = candidate;
                matches++;
            }
        }
    }
    return matches == 1;
}

static bool
key_append_variable(const n00bb_abi_selected_objects_t *selected,
                    const n00bb_abi_object_view_t *view,
                    size_t input_index, const n00b_abi_type_v1 *type,
                    core_buffer_t *out)
{
    if (type->typevar_import_requirement_index) {
        const n00bb_abi_object_view_t *provider = nullptr;
        const n00b_abi_type_v1 *owned = nullptr;
        size_t provider_input = 0;
        return find_imported_variable(selected, view, type, &provider,
            &provider_input, &owned)
            && key_append_variable(selected, provider, provider_input,
                owned, out);
    }
    if (type->typevar_owner_type_index) {
        const n00b_abi_type_v1 *owner = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, type->typevar_owner_type_index);
        if (!owner || !owner->typehash) return false;
        core_buffer_putc(out, 'T');
        key_append_u64(out, owner->typehash);
        key_append_u64(out, type->typevar_ordinal);
        return true;
    }
    const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_SCOPE, type->typevar_owner_scope_index);
    if (!scope || !type->typevar_owner_scope_index) return false;
    core_buffer_putc(out, 'V');
    if (scope->source_location_index && scope->module_index) {
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, scope->module_index);
        const n00b_abi_source_location_v1 *location =
            n00bb_abi_object_row(view, N00B_ABI_TABLE_SOURCE_LOCATION,
                scope->source_location_index);
        const n00b_abi_source_file_v1 *file = location
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_SOURCE_FILE,
                location->file_index) : nullptr;
        if (!module || !location || !file) return false;
        core_buffer_putc(out, 'S');
        if (!key_append_string(view, &module->full_path, out)
            || !key_append_string(view, &file->path, out)
            || !key_append_string(view, &scope->kind_name, out)) return false;
        key_append_u64(out, file->source_identity);
        key_append_u64(out, location->start_byte_offset);
        key_append_u64(out, location->end_byte_offset);
    }
    else {
        core_buffer_putc(out, 'L');
        key_append_u64(out, input_index);
        key_append_u64(out, type->typevar_owner_scope_index);
    }
    key_append_u64(out, type->typevar_ordinal);
    return true;
}

static bool
key_append_type_variables(const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_object_view_t *view,
                          size_t input_index, uint32_t type_index,
                          uint8_t *active, core_buffer_t *out);

static bool
key_append_parameters(const n00bb_abi_selected_objects_t *selected,
                      const n00bb_abi_object_view_t *view,
                      size_t input_index, n00b_abi_range_v1 range,
                      uint8_t *active, core_buffer_t *out)
{
    if (!valid_index_range(view, range)) return false;
    for (uint32_t i = 0; i < range.count; i++) {
        const n00b_abi_parameter_v1 *parameter = indexed_range_row(view,
            range, i, N00B_ABI_TABLE_PARAMETER);
        if (!parameter || !key_append_type_variables(selected, view, input_index,
                parameter->type_index, active, out)) return false;
    }
    return true;
}

static bool
key_append_type_variables(const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_object_view_t *view,
                          size_t input_index, uint32_t type_index,
                          uint8_t *active, core_buffer_t *out)
{
    const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_TYPE, type_index);
    if (!type) return false;
    if (type->kind == N00B_ABI_TYPE_VARIABLE)
        return key_append_variable(selected, view, input_index, type, out);
    if (!(type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)) return true;
    if (active[type_index]) return true;
    active[type_index] = 1;
    bool valid = valid_index_range(view, type->components)
        && valid_index_range(view, type->fields_or_arms);
    for (uint32_t i = 0; valid && i < type->components.count; i++) {
        const uint8_t *slot = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_U32_STREAM, type->components.first + i);
        valid = slot && key_append_type_variables(selected, view, input_index,
            core_le_load_u32(slot), active, out);
    }
    if (valid && type->call_signature_index) {
        const n00b_abi_call_signature_v1 *signature =
            n00bb_abi_object_row(view, N00B_ABI_TABLE_CALL_SIGNATURE,
                type->call_signature_index);
        valid = signature && key_append_type_variables(selected, view, input_index,
                signature->success_type_index, active, out)
            && key_append_type_variables(selected, view, input_index,
                signature->failure_type_index, active, out)
            && key_append_parameters(selected, view, input_index,
                signature->positional_parameters, active, out)
            && key_append_parameters(selected, view, input_index,
                signature->keyword_parameters, active, out);
        if (valid && signature->rest_parameter_index) {
            const n00b_abi_parameter_v1 *rest = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_PARAMETER, signature->rest_parameter_index);
            valid = rest && key_append_type_variables(selected, view, input_index,
                rest->type_index, active, out);
        }
    }
    n00bb_abi_string_view_t constructor;
    if (valid && !n00bb_abi_object_string(view, &type->named_constructor,
                                           &constructor)) valid = false;
    uint32_t member_kind = type->kind == N00B_ABI_TYPE_RECORD
        || type->kind == N00B_ABI_TYPE_TUPLE
        ? N00B_ABI_TABLE_FIELD
        : type->kind == N00B_ABI_TYPE_VARIANT ? N00B_ABI_TABLE_ARM
        : type->kind == N00B_ABI_TYPE_TRAIT ? N00B_ABI_TABLE_TRAIT_METHOD
        : 0;
    if (valid && constructor.length) member_kind = 0;
    for (uint32_t i = 0; valid && member_kind
         && i < type->fields_or_arms.count; i++) {
        const void *member = indexed_range_row(view, type->fields_or_arms,
            i, member_kind);
        if (!member) { valid = false; break; }
        uint32_t child = member_kind == N00B_ABI_TABLE_TRAIT_METHOD
            ? ((const n00b_abi_trait_method_v1 *)member)->signature_type_index
            : ((const n00b_abi_field_v1 *)member)->child_type_index;
        valid = key_append_type_variables(selected, view, input_index, child,
            active, out);
    }
    active[type_index] = 0;
    return valid;
}

static bool
nonconcrete_key_init(abi_link_key_t *key)
{
    const n00b_abi_type_v1 *type = key->row;
    key->owned_bytes = core_buffer_empty();
    core_buffer_putc(key->owned_bytes,
        type->kind == N00B_ABI_TYPE_VARIABLE ? 'V' : 'N');
    if (type->kind == N00B_ABI_TYPE_VARIABLE) {
        if (!key_append_variable(key->selected, key->view, key->input_index, type,
                key->owned_bytes)) return false;
    }
    else {
        const n00bb_abi_directory_entry_t *types = n00bb_abi_object_table(
            key->view, N00B_ABI_TABLE_TYPE);
        if (!types) return false;
        key_append_u64(key->owned_bytes, type->typehash);
        uint8_t *active = core_alloc_array(uint8_t, types->record_count);
        bool valid = key_append_type_variables(key->selected, key->view,
            key->input_index,
            key->local_index, active, key->owned_bytes);
        core_free(active);
        if (!valid) return false;
    }
    key->bytes = (const uint8_t *)key->owned_bytes->data;
    key->length = key->owned_bytes->byte_len;
    return true;
}

static bool
compatible_record_runs(const n00bb_abi_object_view_t *a_view,
                       n00b_abi_range_v1 a_range,
                       const n00bb_abi_object_view_t *b_view,
                       n00b_abi_range_v1 b_range, uint32_t target_kind,
                       size_t row_bytes)
{
    if (a_range.count != b_range.count
        || !valid_index_range(a_view, a_range)
        || !valid_index_range(b_view, b_range)) return false;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const void *a = indexed_range_row(a_view, a_range, i, target_kind);
        const void *b = indexed_range_row(b_view, b_range, i, target_kind);
        if (!a || !b || memcmp(a, b, row_bytes)) return false;
    }
    return true;
}

static bool
compatible_type_layout(const abi_link_key_t *first,
                       const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a_type = first->row, *b_type = candidate->row;
    if (!a_type->layout_index || !b_type->layout_index) return true;
    const n00b_abi_layout_v1 *a = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_LAYOUT, a_type->layout_index);
    const n00b_abi_layout_v1 *b = n00bb_abi_object_row(candidate->view,
        N00B_ABI_TABLE_LAYOUT, b_type->layout_index);
    if (!a || !b || a->type_index != first->local_index
        || b->type_index != candidate->local_index
        || a->policy != b->policy) return false;
    if (!!a->tag_extractor_index != !!b->tag_extractor_index)
        return false;
    if (a->tag_extractor_index) {
        const n00b_abi_tag_extractor_v1 *a_tag = n00bb_abi_object_row(
            first->view, N00B_ABI_TABLE_TAG_EXTRACTOR,
            a->tag_extractor_index);
        const n00b_abi_tag_extractor_v1 *b_tag = n00bb_abi_object_row(
            candidate->view, N00B_ABI_TABLE_TAG_EXTRACTOR,
            b->tag_extractor_index);
        if (!a_tag || !b_tag || memcmp(a_tag, b_tag, sizeof(*a_tag)))
            return false;
    }
    return compatible_record_runs(first->view, a->occupied_runs,
            candidate->view, b->occupied_runs,
            N00B_ABI_TABLE_PHYSICAL_RUN, sizeof(n00b_abi_physical_run_v1))
        && compatible_record_runs(first->view, a->reserved_runs,
            candidate->view, b->reserved_runs,
            N00B_ABI_TABLE_PHYSICAL_RUN, sizeof(n00b_abi_physical_run_v1));
}

static bool
compatible_string_values(const n00bb_abi_object_view_t *a_view,
                         const n00b_abi_string_v1 *a,
                         const n00bb_abi_object_view_t *b_view,
                         const n00b_abi_string_v1 *b)
{
    if (!a || !b) return false;
    n00b_abi_string_v1 empty = {0};
    bool a_empty = !memcmp(a, &empty, sizeof(empty));
    bool b_empty = !memcmp(b, &empty, sizeof(empty));
    if (a_empty || b_empty) return a_empty == b_empty;
    n00bb_abi_string_view_t a_text, b_text;
    return n00bb_abi_object_string(a_view, a, &a_text)
        && n00bb_abi_object_string(b_view, b, &b_text)
        && a_text.length == b_text.length
        && !memcmp(a_text.bytes, b_text.bytes, a_text.length);
}

static bool
compatible_child_type(const n00bb_abi_object_view_t *a_view,
                      uint32_t a_index,
                      const n00bb_abi_object_view_t *b_view,
                      uint32_t b_index)
{
    const n00b_abi_type_v1 *a = n00bb_abi_object_row(a_view,
        N00B_ABI_TABLE_TYPE, a_index);
    const n00b_abi_type_v1 *b = n00bb_abi_object_row(b_view,
        N00B_ABI_TABLE_TYPE, b_index);
    if (!a || !b || a->kind != b->kind) return false;
    if (a->typehash || b->typehash)
        return a->typehash && a->typehash == b->typehash;
    return a->kind == N00B_ABI_TYPE_VARIABLE
        && a->typevar_ordinal == b->typevar_ordinal;
}

static bool
compatible_trace_link(const n00bb_abi_object_view_t *a_view,
                      uint32_t a_index,
                      const n00bb_abi_object_view_t *b_view,
                      uint32_t b_index)
{
    if (!a_index || !b_index) return a_index == b_index;
    const n00b_abi_trace_v1 *a = n00bb_abi_object_row(a_view,
        N00B_ABI_TABLE_TRACE, a_index);
    const n00b_abi_trace_v1 *b = n00bb_abi_object_row(b_view,
        N00B_ABI_TABLE_TRACE, b_index);
    return a && b && compatible_child_type(a_view, a->type_index,
                                           b_view, b->type_index);
}

static bool
compatible_trace_children(const n00bb_abi_object_view_t *a_view,
                          n00b_abi_range_v1 a_range,
                          const n00bb_abi_object_view_t *b_view,
                          n00b_abi_range_v1 b_range)
{
    if (a_range.count != b_range.count
        || !valid_index_range(a_view, a_range)
        || !valid_index_range(b_view, b_range)) return false;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const n00b_abi_trace_child_v1 *a = indexed_range_row(a_view,
            a_range, i, N00B_ABI_TABLE_TRACE_CHILD);
        const n00b_abi_trace_child_v1 *b = indexed_range_row(b_view,
            b_range, i, N00B_ABI_TABLE_TRACE_CHILD);
        if (!a || !b || a->byte_offset != b->byte_offset
            || a->repeat_count != b->repeat_count
            || a->stride_bits != b->stride_bits
            || a->reserved_zero || b->reserved_zero
            || !compatible_child_type(a_view, a->child_type_index,
                                      b_view, b->child_type_index)) return false;
    }
    return true;
}

static bool
compatible_trace_arms(const n00bb_abi_object_view_t *a_view,
                      n00b_abi_range_v1 a_range,
                      const n00bb_abi_object_view_t *b_view,
                      n00b_abi_range_v1 b_range)
{
    if (a_range.count != b_range.count
        || !valid_index_range(a_view, a_range)
        || !valid_index_range(b_view, b_range)) return false;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const n00b_abi_trace_arm_v1 *a = indexed_range_row(a_view,
            a_range, i, N00B_ABI_TABLE_TRACE_ARM);
        const n00b_abi_trace_arm_v1 *b = indexed_range_row(b_view,
            b_range, i, N00B_ABI_TABLE_TRACE_ARM);
        if (!a || !b || a->tag_value != b->tag_value
            || !compatible_trace_link(a_view, a->child_trace_index,
                                      b_view, b->child_trace_index)
            || !compatible_trace_children(a_view, a->children,
                                          b_view, b->children)) return false;
    }
    return true;
}

static bool
compatible_type_trace(const abi_link_key_t *first,
                      const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a_type = first->row, *b_type = candidate->row;
    if (!a_type->tracing_index || !b_type->tracing_index)
        return a_type->tracing_index == b_type->tracing_index
            || !a_type->layout_index || !b_type->layout_index
            || (a_type->kind == N00B_ABI_TYPE_OPTION
                && b_type->kind == N00B_ABI_TYPE_OPTION);
    const n00b_abi_trace_v1 *a = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_TRACE, a_type->tracing_index);
    const n00b_abi_trace_v1 *b = n00bb_abi_object_row(candidate->view,
        N00B_ABI_TABLE_TRACE, b_type->tracing_index);
    if (!a || !b || a->type_index != first->local_index
        || b->type_index != candidate->local_index
        || a->reserved_zero || b->reserved_zero
        || a->option_bit_offset != b->option_bit_offset
        || !!a->tag_extractor_index != !!b->tag_extractor_index
        || !compatible_trace_link(first->view, a->child_trace_index,
                                  candidate->view, b->child_trace_index))
        return false;
    if (a->tag_extractor_index) {
        const n00b_abi_tag_extractor_v1 *a_tag = n00bb_abi_object_row(
            first->view, N00B_ABI_TABLE_TAG_EXTRACTOR,
            a->tag_extractor_index);
        const n00b_abi_tag_extractor_v1 *b_tag = n00bb_abi_object_row(
            candidate->view, N00B_ABI_TABLE_TAG_EXTRACTOR,
            b->tag_extractor_index);
        if (!a_tag || !b_tag || memcmp(a_tag, b_tag, sizeof(*a_tag)))
            return false;
    }
    return compatible_trace_children(first->view, a->fixed_children,
            candidate->view, b->fixed_children)
        && compatible_trace_arms(first->view, a->arms,
                                 candidate->view, b->arms);
}

static bool
type_is_import_reference(const n00b_abi_type_v1 *type)
{
    return !type->owner_module_index && !type->layout_index
        && (type->kind == N00B_ABI_TYPE_RECORD
            || type->kind == N00B_ABI_TYPE_VARIANT
            || type->kind == N00B_ABI_TYPE_ENUM
            || type->kind == N00B_ABI_TYPE_FLAGSET
            || type->kind == N00B_ABI_TYPE_CALLBACK
            || type->kind == N00B_ABI_TYPE_TRAIT
            || type->kind == N00B_ABI_TYPE_WRAPPER);
}

static bool
compatible_type_components(const abi_link_key_t *first,
                           const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a = first->row, *b = candidate->row;
    if (a->components.count != b->components.count)
        return (!a->components.count && type_is_import_reference(a))
            || (!b->components.count && type_is_import_reference(b));
    if (!valid_index_range(first->view, a->components)
        || !valid_index_range(candidate->view, b->components)) return false;
    for (uint32_t i = 0; i < a->components.count; i++) {
        const uint8_t *a_slot = n00bb_abi_object_row(first->view,
            N00B_ABI_TABLE_U32_STREAM, a->components.first + i);
        const uint8_t *b_slot = n00bb_abi_object_row(candidate->view,
            N00B_ABI_TABLE_U32_STREAM, b->components.first + i);
        if (!compatible_child_type(first->view, core_le_load_u32(a_slot),
                candidate->view, core_le_load_u32(b_slot))) return false;
    }
    return true;
}

static bool
compatible_type_members(const abi_link_key_t *first,
                        const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a = first->row, *b = candidate->row;
    n00b_abi_range_v1 a_range = a->fields_or_arms;
    n00b_abi_range_v1 b_range = b->fields_or_arms;
    if (a_range.count != b_range.count)
        return (!a_range.count && type_is_import_reference(a))
            || (!b_range.count && type_is_import_reference(b));
    if (!valid_index_range(first->view, a_range)
        || !valid_index_range(candidate->view, b_range)) return false;
    uint32_t member_kind = a->kind == N00B_ABI_TYPE_RECORD
        || a->kind == N00B_ABI_TYPE_TUPLE
        ? N00B_ABI_TABLE_FIELD
        : a->kind == N00B_ABI_TYPE_VARIANT ? N00B_ABI_TABLE_ARM
        : a->kind == N00B_ABI_TYPE_ENUM
            || a->kind == N00B_ABI_TYPE_FLAGSET
            ? N00B_ABI_TABLE_ENUM_ITEM
        : a->kind == N00B_ABI_TYPE_TRAIT
            ? N00B_ABI_TABLE_TRAIT_METHOD : 0;
    if (!member_kind) return !a_range.count;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const void *a_row = indexed_range_row(first->view, a_range, i,
                                               member_kind);
        const void *b_row = indexed_range_row(candidate->view, b_range, i,
                                               member_kind);
        if (!a_row || !b_row) return false;
        if (member_kind == N00B_ABI_TABLE_TRAIT_METHOD) {
            const n00b_abi_trait_method_v1 *a_method = a_row;
            const n00b_abi_trait_method_v1 *b_method = b_row;
            const n00b_abi_symbol_v1 *a_symbol = n00bb_abi_object_row(
                first->view, N00B_ABI_TABLE_SYMBOL,
                a_method->symbol_index);
            const n00b_abi_symbol_v1 *b_symbol = n00bb_abi_object_row(
                candidate->view, N00B_ABI_TABLE_SYMBOL,
                b_method->symbol_index);
            if (a_method->trait_type_index != first->local_index
                || b_method->trait_type_index != candidate->local_index
                || a_method->declaration_ordinal
                    != b_method->declaration_ordinal
                || !compatible_child_type(first->view,
                    a_method->signature_type_index, candidate->view,
                    b_method->signature_type_index)
                || (!!a_symbol != !!b_symbol)
                || (a_symbol && !compatible_string_values(first->view,
                    &a_symbol->name, candidate->view,
                    &b_symbol->name))) return false;
            continue;
        }
        if (member_kind == N00B_ABI_TABLE_ENUM_ITEM) {
            const n00b_abi_enum_item_v1 *a_item = a_row, *b_item = b_row;
            if (a_item->parent_type_index != first->local_index
                || b_item->parent_type_index != candidate->local_index
                || a_item->declaration_ordinal
                    != b_item->declaration_ordinal
                || a_item->value_low != b_item->value_low
                || a_item->value_high != b_item->value_high
                || a_item->flags != b_item->flags
                || !compatible_string_values(first->view, &a_item->name,
                    candidate->view, &b_item->name)) return false;
            continue;
        }
        const n00b_abi_field_v1 *a_field = a_row, *b_field = b_row;
        if (a_field->parent_type_index != first->local_index
            || b_field->parent_type_index != candidate->local_index
            || a_field->visibility != b_field->visibility
            || a_field->logical_bit_offset != b_field->logical_bit_offset
            || a_field->inline_reserved_zero
            || b_field->inline_reserved_zero
            || a_field->inline_element_count
                != b_field->inline_element_count
            || a_field->inline_element_stride_bits
                != b_field->inline_element_stride_bits
            || (!!a_field->inline_element_type_index
                != !!b_field->inline_element_type_index)
            || (a_field->inline_element_type_index
                && !compatible_child_type(first->view,
                    a_field->inline_element_type_index,
                    candidate->view,
                    b_field->inline_element_type_index))
            || ((a->owner_module_index || b->owner_module_index)
                && a_field->declaration_ordinal
                    != b_field->declaration_ordinal)
            || !compatible_string_values(first->view, &a_field->name,
                candidate->view, &b_field->name)
            || !compatible_child_type(first->view,
                a_field->child_type_index, candidate->view,
                b_field->child_type_index)
            || !compatible_record_runs(first->view, a_field->packed_runs,
                candidate->view, b_field->packed_runs,
                N00B_ABI_TABLE_PACKED_RUN, sizeof(n00b_abi_packed_run_v1))
            || !compatible_record_runs(first->view, a_field->physical_runs,
                candidate->view, b_field->physical_runs,
                N00B_ABI_TABLE_PHYSICAL_RUN,
                sizeof(n00b_abi_physical_run_v1))) return false;
        if (member_kind == N00B_ABI_TABLE_ARM
            && ((const n00b_abi_arm_v1 *)a_row)->tag_value
                != ((const n00b_abi_arm_v1 *)b_row)->tag_value)
            return false;
    }
    return true;
}

static bool
compatible_parameter(const n00bb_abi_object_view_t *a_view,
                     const n00b_abi_parameter_v1 *a,
                     const n00bb_abi_object_view_t *b_view,
                     const n00b_abi_parameter_v1 *b)
{
    return a && b && a->passing_mode == b->passing_mode
        && a->declaration_ordinal == b->declaration_ordinal
        && a->flags == b->flags
        && a->keyword_block_bit_offset == b->keyword_block_bit_offset
        && compatible_child_type(a_view, a->type_index,
                                 b_view, b->type_index)
        && compatible_string_values(a_view, &a->name, b_view, &b->name);
}

static bool
compatible_parameter_range(const n00bb_abi_object_view_t *a_view,
                           n00b_abi_range_v1 a_range,
                           const n00bb_abi_object_view_t *b_view,
                           n00b_abi_range_v1 b_range)
{
    if (a_range.count != b_range.count
        || !valid_index_range(a_view, a_range)
        || !valid_index_range(b_view, b_range)) return false;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const n00b_abi_parameter_v1 *a = indexed_range_row(a_view,
            a_range, i, N00B_ABI_TABLE_PARAMETER);
        const n00b_abi_parameter_v1 *b = indexed_range_row(b_view,
            b_range, i, N00B_ABI_TABLE_PARAMETER);
        if (!compatible_parameter(a_view, a, b_view, b)) return false;
    }
    return true;
}

static bool
compatible_call_signature(const abi_link_key_t *first,
                          const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a_type = first->row, *b_type = candidate->row;
    if (a_type->kind != N00B_ABI_TYPE_FUNCTION)
        return !a_type->call_signature_index
            && !b_type->call_signature_index;
    if (!a_type->call_signature_index || !b_type->call_signature_index)
        return false;
    const n00b_abi_call_signature_v1 *a = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_CALL_SIGNATURE, a_type->call_signature_index);
    const n00b_abi_call_signature_v1 *b = n00bb_abi_object_row(
        candidate->view, N00B_ABI_TABLE_CALL_SIGNATURE,
        b_type->call_signature_index);
    if (!a || !b || a->type_index != first->local_index
        || b->type_index != candidate->local_index
        || !compatible_child_type(first->view, a->success_type_index,
            candidate->view, b->success_type_index)
        || !compatible_child_type(first->view, a->failure_type_index,
            candidate->view, b->failure_type_index)
        || !!a->rest_parameter_index != !!b->rest_parameter_index
        || !compatible_parameter_range(first->view,
            a->positional_parameters, candidate->view,
            b->positional_parameters)
        || !compatible_parameter_range(first->view,
            a->keyword_parameters, candidate->view,
            b->keyword_parameters)) return false;
    if (!a->rest_parameter_index) return true;
    const n00b_abi_parameter_v1 *a_rest = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_PARAMETER, a->rest_parameter_index);
    const n00b_abi_parameter_v1 *b_rest = n00bb_abi_object_row(
        candidate->view, N00B_ABI_TABLE_PARAMETER,
        b->rest_parameter_index);
    return compatible_parameter(first->view, a_rest,
                                candidate->view, b_rest);
}

static bool
compatible_constraint_type(const abi_link_key_t *first, uint32_t a_index,
                           const abi_link_key_t *candidate, uint32_t b_index)
{
    if (a_index == N00B_ABI_V1_NO_TYPE_INDEX
        || b_index == N00B_ABI_V1_NO_TYPE_INDEX)
        return a_index == b_index;
    const n00b_abi_type_v1 *a = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_TYPE, a_index);
    const n00b_abi_type_v1 *b = n00bb_abi_object_row(candidate->view,
        N00B_ABI_TABLE_TYPE, b_index);
    if (!a || !b || a->kind != b->kind) return false;
    abi_link_key_t a_key = {
        .view = first->view, .row = a,
        .input_index = first->input_index, .local_index = a_index,
    };
    abi_link_key_t b_key = {
        .view = candidate->view, .row = b,
        .input_index = candidate->input_index, .local_index = b_index,
    };
    bool valid = link_key_init(&a_key, N00B_ABI_TABLE_TYPE)
        && link_key_init(&b_key, N00B_ABI_TABLE_TYPE)
        && a_key.length == b_key.length
        && !memcmp(a_key.bytes, b_key.bytes, a_key.length);
    core_buffer_free(a_key.owned_bytes);
    core_buffer_free(b_key.owned_bytes);
    return valid;
}

static bool
compatible_constraint_types(const abi_link_key_t *first,
                            n00b_abi_range_v1 a_range,
                            const abi_link_key_t *candidate,
                            n00b_abi_range_v1 b_range)
{
    if (a_range.count != b_range.count
        || !valid_index_range(first->view, a_range)
        || !valid_index_range(candidate->view, b_range)) return false;
    for (uint32_t i = 0; i < a_range.count; i++) {
        const uint8_t *a = n00bb_abi_object_row(first->view,
            N00B_ABI_TABLE_U32_STREAM, a_range.first + i);
        const uint8_t *b = n00bb_abi_object_row(candidate->view,
            N00B_ABI_TABLE_U32_STREAM, b_range.first + i);
        if (!a || !b || !compatible_constraint_type(first,
                core_le_load_u32(a), candidate, core_le_load_u32(b)))
            return false;
    }
    return true;
}

static bool
compatible_call_shape(const abi_link_key_t *first, uint32_t a_index,
                      const abi_link_key_t *candidate, uint32_t b_index)
{
    if (!a_index || !b_index) return a_index == b_index;
    const n00b_abi_call_shape_v1 *a = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_CALL_SHAPE, a_index);
    const n00b_abi_call_shape_v1 *b = n00bb_abi_object_row(candidate->view,
        N00B_ABI_TABLE_CALL_SHAPE, b_index);
    if (!a || !b || a->reserved_zero || b->reserved_zero
        || !compatible_constraint_type(first, a->success_type_index,
                                       candidate, b->success_type_index)
        || !compatible_constraint_type(first, a->failure_type_index,
                                       candidate, b->failure_type_index)
        || !!a->rest_parameter_index != !!b->rest_parameter_index
        || !compatible_parameter_range(first->view,
            a->positional_parameters, candidate->view,
            b->positional_parameters)
        || !compatible_parameter_range(first->view,
            a->keyword_parameters, candidate->view,
            b->keyword_parameters)) return false;
    if (!a->rest_parameter_index) return true;
    const n00b_abi_parameter_v1 *a_rest = n00bb_abi_object_row(first->view,
        N00B_ABI_TABLE_PARAMETER, a->rest_parameter_index);
    const n00b_abi_parameter_v1 *b_rest = n00bb_abi_object_row(
        candidate->view, N00B_ABI_TABLE_PARAMETER,
        b->rest_parameter_index);
    return compatible_parameter(first->view, a_rest,
                                candidate->view, b_rest);
}

static bool
compatible_constraint_rows_scope(const abi_link_key_t *first,
                                 const n00b_abi_constraint_v1 *a,
                                 const abi_link_key_t *candidate,
                                 const n00b_abi_constraint_v1 *b,
                                 bool compare_scope)
{
    return a && b && a->kind == b->kind
        && (!compare_scope || a->flags == b->flags)
        && a->variable_ordinal == b->variable_ordinal
        && a->type_family == b->type_family
        && a->parameter_index == b->parameter_index
        && !a->reserved_zero && !b->reserved_zero
        && compatible_string_values(first->view, &a->name,
                                    candidate->view, &b->name)
        && (!compare_scope || (compatible_string_values(first->view,
            &a->scope_module_name, candidate->view, &b->scope_module_name)
            && compatible_string_values(first->view,
                &a->scope_package_name, candidate->view,
                &b->scope_package_name)))
        && compatible_constraint_type(first, a->target_type_index,
                                      candidate, b->target_type_index)
        && compatible_constraint_type(first, a->result_type_index,
                                      candidate, b->result_type_index)
        && compatible_call_shape(first, a->call_shape_index,
                                 candidate, b->call_shape_index)
        && compatible_constraint_types(first, a->arguments,
                                       candidate, b->arguments);
}

static bool
compatible_constraint_rows(const abi_link_key_t *first,
                           const n00b_abi_constraint_v1 *a,
                           const abi_link_key_t *candidate,
                           const n00b_abi_constraint_v1 *b)
{
    return compatible_constraint_rows_scope(first, a, candidate, b, true);
}

static bool
compatible_type_constraints(const abi_link_key_t *first,
                            const abi_link_key_t *candidate)
{
    const n00b_abi_type_v1 *a = first->row, *b = candidate->row;
    if ((a->typevar_import_requirement_index && !a->constraints.count)
        || (b->typevar_import_requirement_index && !b->constraints.count))
        return true;
    if (a->constraints.count != b->constraints.count
        || !valid_index_range(first->view, a->constraints)
        || !valid_index_range(candidate->view, b->constraints)) return false;
    bool *matched = core_alloc_array(bool, b->constraints.count);
    bool compatible = true;
    for (uint32_t i = 0; i < a->constraints.count; i++) {
        const n00b_abi_constraint_v1 *a_row = indexed_range_row(first->view,
            a->constraints, i, N00B_ABI_TABLE_CONSTRAINT);
        bool found = false;
        for (uint32_t j = 0; j < b->constraints.count; j++) {
            if (matched[j]) continue;
            const n00b_abi_constraint_v1 *b_row = indexed_range_row(
                candidate->view, b->constraints, j,
                N00B_ABI_TABLE_CONSTRAINT);
            if (!compatible_constraint_rows(first, a_row, candidate, b_row))
                continue;
            matched[j] = true;
            found = true;
            break;
        }
        if (!found) { compatible = false; break; }
    }
    core_free(matched);
    return compatible;
}

static bool
declared_constraint_matches_inferred(const abi_link_key_t *context,
                                     const n00b_abi_constraint_v1 *declared,
                                     const n00b_abi_constraint_v1 *inferred)
{
    if (!declared || !inferred || declared->flags
        || (inferred->flags & ~N00B_ABI_CONSTRAINT_FLAG_HAS_SCOPE)
        || (declared->kind != N00B_ABI_CONSTRAINT_IMPLEMENTS
            && declared->kind != N00B_ABI_CONSTRAINT_EXCLUDES)) return false;
    return compatible_constraint_rows_scope(context, declared,
                                            context, inferred, false);
}

static bool
declared_function_constraints_valid(
    const n00bb_abi_selected_objects_t *selected)
{
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *functions =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_FUNCTION);
        const n00bb_abi_directory_entry_t *types =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_TYPE);
        if (!functions) continue;
        if (!types) return false;
        abi_link_key_t context = {
            .selected = selected, .view = view, .input_index = input,
        };
        for (uint32_t i = 1; i < functions->record_count; i++) {
            const n00b_abi_function_v1 *function = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_FUNCTION, i);
            if (!function || !function->constraints.count) continue;
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SYMBOL, function->symbol_index);
            if (!symbol || symbol->kind != N00B_ABI_SYMBOL_FUNCTION
                || symbol->module_index != function->module_index
                || !symbol->declaration_location_index
                || !valid_index_range(view, function->constraints))
                return false;
            for (uint32_t j = 0; j < function->constraints.count; j++) {
                const n00b_abi_constraint_v1 *declared = indexed_range_row(
                    view, function->constraints, j,
                    N00B_ABI_TABLE_CONSTRAINT);
                if (!declared) return false;
                bool found = false;
                for (uint32_t k = 1; k < types->record_count; k++) {
                    const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
                        N00B_ABI_TABLE_TYPE, k);
                    if (!type || type->kind != N00B_ABI_TYPE_VARIABLE
                        || type->typevar_import_requirement_index
                        || type->typevar_ordinal != declared->variable_ordinal)
                        continue;
                    const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(
                        view, N00B_ABI_TABLE_SCOPE,
                        type->typevar_owner_scope_index);
                    if (!scope || scope->module_index != function->module_index
                        || scope->parent_scope_index != function->scope_index
                        || scope->source_location_index
                            != symbol->declaration_location_index)
                        continue;
                    if (!valid_index_range(view, type->constraints))
                        return false;
                    for (uint32_t n = 0; n < type->constraints.count; n++) {
                        const n00b_abi_constraint_v1 *inferred =
                            indexed_range_row(view, type->constraints, n,
                                N00B_ABI_TABLE_CONSTRAINT);
                        if (declared_constraint_matches_inferred(
                                &context, declared, inferred)) found = true;
                    }
                }
                if (!found) return false;
            }
        }
    }
    return true;
}

static bool
compatible_type_identity(const abi_link_key_t *first,
                         const abi_link_key_t *candidate,
                         core_string_t *reason)
{
#define TYPE_MISMATCH(message) do { if (reason) *reason = CORE_STRING_STATIC(message); return false; } while (0)
    const n00b_abi_type_v1 *a = first->row, *b = candidate->row;
    n00bb_abi_string_view_t a_name, b_name;
    bool a_import = type_is_import_reference(a);
    bool b_import = type_is_import_reference(b);
    uint32_t a_flags = a->flags, b_flags = b->flags;
    if (a_import || b_import) {
        if ((a_import && (a_flags & N00B_ABI_TYPE_FLAG_PACKABLE))
            || (b_import && (b_flags & N00B_ABI_TYPE_FLAG_PACKABLE)))
            TYPE_MISMATCH("imported type marked packable");
        a_flags &= ~N00B_ABI_TYPE_FLAG_PACKABLE;
        b_flags &= ~N00B_ABI_TYPE_FLAG_PACKABLE;
    }
    if (!a->size_bits || !b->size_bits) {
        a_flags &= ~N00B_ABI_TYPE_FLAG_PACKABLE;
        b_flags &= ~N00B_ABI_TYPE_FLAG_PACKABLE;
    }
    if (a->named_arguments.count != b->named_arguments.count
        || !compatible_string_values(first->view, &a->named_constructor,
            candidate->view, &b->named_constructor))
        TYPE_MISMATCH("named constructor mismatch");
    const n00bb_abi_directory_entry_t *a_args = n00bb_abi_object_table(
        first->view, N00B_ABI_TABLE_TYPE_ARGUMENT);
    const n00bb_abi_directory_entry_t *b_args = n00bb_abi_object_table(
        candidate->view, N00B_ABI_TABLE_TYPE_ARGUMENT);
    if (a->named_arguments.count && (!a_args || !b_args
        || a->named_arguments.first >= a_args->record_count
        || b->named_arguments.first >= b_args->record_count
        || a->named_arguments.count > a_args->record_count
            - a->named_arguments.first
        || b->named_arguments.count > b_args->record_count
            - b->named_arguments.first))
        TYPE_MISMATCH("invalid named argument range");
    for (uint32_t i = 0; i < a->named_arguments.count; i++) {
        const n00b_abi_type_argument_v1 *left = n00bb_abi_object_row(
            first->view, N00B_ABI_TABLE_TYPE_ARGUMENT,
            a->named_arguments.first + i);
        const n00b_abi_type_argument_v1 *right = n00bb_abi_object_row(
            candidate->view, N00B_ABI_TABLE_TYPE_ARGUMENT,
            b->named_arguments.first + i);
        if (!left || !right || left->owner_type_index != first->local_index
            || right->owner_type_index != candidate->local_index
            || left->ordinal != i || right->ordinal != i
            || left->kind != right->kind
            || left->scalar_bits != right->scalar_bits
            || !compatible_string_values(first->view, &left->string_value,
                candidate->view, &right->string_value)
            || (left->kind == N00B_ABI_TYPE_ARG_TYPE
                ? !compatible_child_type(first->view, left->type_index,
                    candidate->view, right->type_index)
                : left->type_index || right->type_index))
            TYPE_MISMATCH("named argument mismatch");
    }
    if (a->kind != b->kind || a->typehash != b->typehash
        || a_flags != b_flags)
        TYPE_MISMATCH("type kind, hash, or flags mismatch");
    if (a->size_bits && b->size_bits && a->size_bits != b->size_bits)
        TYPE_MISMATCH("type size mismatch");
    if (a->alignment_bytes && b->alignment_bytes
        && a->alignment_bytes != b->alignment_bytes)
        TYPE_MISMATCH("type alignment mismatch");
    if (a->semantic_width_bits && b->semantic_width_bits
        && a->semantic_width_bits != b->semantic_width_bits)
        TYPE_MISMATCH("semantic width mismatch");
    if (a->unpacked_storage_bits && b->unpacked_storage_bits
        && a->unpacked_storage_bits != b->unpacked_storage_bits)
        TYPE_MISMATCH("unpacked storage width mismatch");
    if (!n00bb_abi_object_string(first->view, &a->canonical_name, &a_name)
        || !n00bb_abi_object_string(candidate->view, &b->canonical_name,
                                   &b_name)
        || a_name.length != b_name.length
        || memcmp(a_name.bytes, b_name.bytes, a_name.length))
        TYPE_MISMATCH("canonical name mismatch");
    if (!compatible_type_layout(first, candidate))
        TYPE_MISMATCH("type layout mismatch");
    if (!compatible_type_trace(first, candidate))
        TYPE_MISMATCH("type trace mismatch");
    if (!compatible_type_components(first, candidate))
        TYPE_MISMATCH("type components mismatch");
    if (!compatible_type_members(first, candidate))
        TYPE_MISMATCH("type members mismatch");
    if (!compatible_call_signature(first, candidate))
        TYPE_MISMATCH("call signature mismatch");
    if (!compatible_type_constraints(first, candidate))
        TYPE_MISMATCH("type constraints mismatch");
#undef TYPE_MISMATCH
    return true;
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

const core_index_remap_t *
n00bb_abi_index_plan_remap(const n00bb_abi_index_plan_t *plan,
                           size_t input_index, uint32_t kind)
{
    if (!plan || input_index >= plan->input_count || !kind
        || kind >= plan->kind_count) return nullptr;
    return plan->maps[input_index * plan->kind_count + kind];
}

static bool
selected_call_shapes_valid(const n00bb_abi_selected_objects_t *selected)
{
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *shapes = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_CALL_SHAPE);
        const n00bb_abi_directory_entry_t *constraints =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_CONSTRAINT);
        uint32_t shape_count = shapes ? shapes->record_count : 0;
        uint8_t *seen = shape_count > 1
            ? core_alloc_array(uint8_t, shape_count) : nullptr;
        if (shape_count > 1 && !seen) return false;
        bool valid = true;
        for (uint32_t i = 1; valid && constraints
             && i < constraints->record_count; i++) {
            const n00b_abi_constraint_v1 *constraint =
                n00bb_abi_object_row(view, N00B_ABI_TABLE_CONSTRAINT, i);
            if (!constraint) { valid = false; break; }
            uint32_t shape_index = constraint->call_shape_index;
            if (!shape_index) continue;
            if (constraint->kind != N00B_ABI_CONSTRAINT_METHOD
                || shape_index >= shape_count || seen[shape_index])
                valid = false;
            else seen[shape_index] = 1;
        }
        for (uint32_t i = 1; valid && i < shape_count; i++)
            if (!seen[i]) valid = false;
        core_free(seen);
        if (!valid) return false;
    }
    return true;
}

n00bb_abi_index_plan_t *
n00bb_abi_index_plan_build_detailed(
    const n00bb_abi_selected_objects_t *selected,
    n00bb_abi_index_plan_failure_t *failure)
{
    if (failure) *failure = (n00bb_abi_index_plan_failure_t){0};
    if (!selected || !selected->views || !selected->count) return nullptr;
    if (!declared_function_constraints_valid(selected)) {
        if (failure) failure->reason =
            CORE_STRING_STATIC("invalid declared function constraints");
        return nullptr;
    }
    if (!selected_call_shapes_valid(selected)) {
        if (failure) failure->reason = CORE_STRING_STATIC("invalid call shapes");
        return nullptr;
    }
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
        if (!view->directory) {
            if (failure) *failure = (n00bb_abi_index_plan_failure_t){
                .input = i,
                .reason = CORE_STRING_STATIC("missing ABI directory"),
            };
            return nullptr;
        }
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
                if (failure) *failure = (n00bb_abi_index_plan_failure_t){
                    .input = input, .table_kind = kind,
                    .reason = CORE_STRING_STATIC(
                        "invalid table kind or row count"),
                };
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
                    *nil = (abi_link_key_t){
                        .selected = selected, .view = view, .row = row};
                    const n00b_abi_type_v1 *type = row;
                    valid = valid && type->kind == N00B_ABI_TYPE_NIL
                        && type->typehash && (!first_nil
                            || (type->typehash
                                == ((const n00b_abi_type_v1 *)
                                    first_nil->row)->typehash
                                && compatible_type_identity(first_nil, nil,
                                                            nullptr)));
                    if (!first_nil) first_nil = nil;
                }
                else valid = valid && reserved_row_is_zero(row,
                    directory->record_bytes);
                if (!valid && failure)
                    *failure = (n00bb_abi_index_plan_failure_t){
                        .input = input, .table_kind = kind,
                        .reason = CORE_STRING_STATIC("invalid reserved row"),
                    };
                first = 1;
            }
            for (uint32_t local = first; valid
                 && local < directory->record_count; local++) {
                const void *row = view->metadata.bytes
                    + directory->file_offset
                    + (size_t)local * directory->record_bytes;
                abi_link_key_t *key = &keys[next_key++];
                *key = (abi_link_key_t){
                    .selected = selected, .view = view, .row = row,
                    .input_index = input, .local_index = local,
                };
                bool inserted = false;
                bool key_valid = link_key_init(key, kind);
                const core_indexed_entry_t *entry = key_valid
                    ? core_indexed_table_intern(tables[kind], key, key,
                        &inserted) : nullptr;
                core_string_t identity_reason = {};
                bool compatible = entry && (inserted
                    || kind != N00B_ABI_TABLE_TYPE
                    || compatible_type_identity(entry->value, key,
                                                &identity_reason));
                valid = compatible && core_index_remap_set(remap, local,
                                                            entry->index);
                if (!valid && failure)
                    *failure = (n00bb_abi_index_plan_failure_t){
                        .input = input, .table_kind = kind,
                        .row_index = local,
                        .prior_input = entry && !inserted
                            ? ((const abi_link_key_t *)entry->value)->input_index
                            : 0,
                        .prior_row_index = entry && !inserted
                            ? ((const abi_link_key_t *)entry->value)->local_index
                            : 0,
                        .reason = !key_valid
                            ? CORE_STRING_STATIC("invalid row identity")
                            : !entry
                            ? CORE_STRING_STATIC("could not intern row identity")
                            : !compatible ? (identity_reason.data
                                ? identity_reason
                                : CORE_STRING_STATIC("conflicting type identity"))
                            : CORE_STRING_STATIC("could not remap row"),
                    };
                if (!valid && failure && kind == N00B_ABI_TABLE_LAYOUT) {
                    const n00b_abi_layout_v1 *layout = row;
                    const n00b_abi_type_v1 *owner = n00bb_abi_object_row(
                        view, N00B_ABI_TABLE_TYPE, layout->type_index);
                    failure->owner_type_index = layout->type_index;
                    failure->owner_row_index = owner ? owner->layout_index : 0;
                }
                if (!valid && failure && kind == N00B_ABI_TABLE_TYPE) {
                    const n00b_abi_type_v1 *type = row;
                    failure->type_kind = type->kind;
                    failure->type_flags = type->flags;
                    failure->typehash = type->typehash;
                    (void)n00bb_abi_object_string(view,
                        &type->canonical_name, &failure->name);
                }
                if (!valid && failure && kind == N00B_ABI_TABLE_TYPE
                    && entry && !inserted) {
                    const abi_link_key_t *prior = entry->value;
                    const n00b_abi_type_v1 *current_type = key->row;
                    const n00b_abi_type_v1 *prior_type = prior->row;
                    failure->constraint_count = current_type->constraints.count;
                    failure->prior_constraint_count = prior_type->constraints.count;
                    failure->imported_type_variable =
                        current_type->typevar_import_requirement_index != 0;
                    failure->prior_imported_type_variable =
                        prior_type->typevar_import_requirement_index != 0;
                    (void)n00bb_abi_object_string(key->view,
                        &current_type->canonical_name, &failure->name);
                    (void)n00bb_abi_object_string(prior->view,
                        &prior_type->canonical_name, &failure->prior_name);
                }
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
    for (size_t i = 0; i < next_key; i++)
        core_buffer_free(keys[i].owned_bytes);
    core_free(keys);
    if (!valid) {
        n00bb_abi_index_plan_free(plan);
        return nullptr;
    }
    return plan;
}

n00bb_abi_index_plan_t *
n00bb_abi_index_plan_build(const n00bb_abi_selected_objects_t *selected)
{
    return n00bb_abi_index_plan_build_detailed(selected, nullptr);
}

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
    case N00B_ABI_TYPE_TUPLE: return N00B_ABI_TABLE_FIELD;
    case N00B_ABI_TYPE_VARIANT: return N00B_ABI_TABLE_ARM;
    case N00B_ABI_TYPE_ENUM:
    case N00B_ABI_TYPE_FLAGSET: return N00B_ABI_TABLE_ENUM_ITEM;
    case N00B_ABI_TYPE_TRAIT: return N00B_ABI_TABLE_TRAIT_METHOD;
    default: return 0;
    }
}

static bool
type_backlinks_valid(const n00bb_abi_object_view_t *view,
                     const n00b_abi_type_v1 *type, uint32_t type_index)
{
    n00b_abi_string_v1 empty_constructor = {0};
    if ((type->kind == N00B_ABI_TYPE_NAMED
         || type->named_arguments.count)
        && !memcmp(&type->named_constructor, &empty_constructor,
                   sizeof(empty_constructor))) return false;
    const n00bb_abi_directory_entry_t *arguments = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_TYPE_ARGUMENT);
    if (!type->named_arguments.count) {
        if (type->named_arguments.first) return false;
    }
    else {
        if (!arguments || !type->named_arguments.first
            || type->named_arguments.first >= arguments->record_count
            || type->named_arguments.count > arguments->record_count
                - type->named_arguments.first) return false;
        for (uint32_t i = 0; i < type->named_arguments.count; i++) {
            const n00b_abi_type_argument_v1 *arg = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_TYPE_ARGUMENT,
                type->named_arguments.first + i);
            if (!arg || arg->owner_type_index != type_index
                || arg->ordinal != i) return false;
        }
    }
    if (type->layout_index) {
        const n00b_abi_layout_v1 *layout = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_LAYOUT, type->layout_index);
        if (!layout || layout->type_index != type_index) return false;
    }
    if (type->tracing_index) {
        const n00b_abi_trace_v1 *trace = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TRACE, type->tracing_index);
        if (!trace || trace->type_index != type_index) return false;
    }
    uint32_t kind = type_member_kind(type);
    if (!type->fields_or_arms.count) return !type->fields_or_arms.first;
    if (!kind || !valid_index_range(view, type->fields_or_arms)) return false;
    for (uint32_t i = 0; i < type->fields_or_arms.count; i++) {
        const void *member = indexed_range_row(view, type->fields_or_arms,
                                                i, kind);
        if (!member) return false;
        n00bb_abi_member_facts_t facts = n00bb_abi_member_facts(kind,
                                                                member);
        if (facts.parent != type_index) return false;
        if (facts.symbol) {
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SYMBOL, facts.symbol);
            if (!symbol || symbol->kind != facts.symbol_kind
                || symbol->type_index != facts.type
                || (facts.visibility
                    && symbol->visibility != facts.visibility)) return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            const void *previous = indexed_range_row(view,
                type->fields_or_arms, j, kind);
            if (!previous || n00bb_abi_member_facts(kind, previous).ordinal
                == facts.ordinal) return false;
        }
    }
    return true;
}

static bool
range_owns_row_once(const n00bb_abi_object_view_t *view,
                    n00b_abi_range_v1 range, uint32_t local_index)
{
    if (!valid_index_range(view, range)) return false;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < range.count; i++) {
        const uint8_t *slot = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_U32_STREAM, range.first + i);
        if (!slot) return false;
        if (core_le_load_u32(slot) == local_index) matches++;
    }
    return matches == 1;
}

static bool
module_initializer_valid(const n00bb_abi_object_view_t *view,
                         uint32_t module_index, uint32_t function_index)
{
    if (!function_index) return true;
    const n00b_abi_function_v1 *function = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_FUNCTION, function_index);
    if (!function || function->module_index != module_index
        || function->failure_type_index
        || (function->code_ranges.count
            && !valid_index_range(view, function->code_ranges))) return false;
    for (uint32_t i = 0; i < function->code_ranges.count; i++) {
        const n00b_abi_code_range_v1 *range = indexed_range_row(view,
            function->code_ranges, i, N00B_ABI_TABLE_CODE_RANGE);
        if (!range || range->function_index != function_index) return false;
    }
    return true;
}

static bool
object_shape_extent(const void *context, n00b_abi_range_v1 extents,
                    uint32_t ordinal, uint64_t *out)
{
    const n00bb_abi_object_view_t *view = context;
    if (ordinal >= extents.count || extents.first > UINT32_MAX - ordinal)
        return false;
    const uint8_t *slot = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_U64_STREAM, extents.first + ordinal);
    if (!slot) return false;
    *out = core_le_load_u64(slot);
    return true;
}

static bool
object_parameter_index_at(const void *context, n00b_abi_range_v1 range,
                          uint32_t ordinal, uint32_t *out)
{
    const n00bb_abi_object_view_t *view = context;
    if (ordinal >= range.count || range.first > UINT32_MAX - ordinal)
        return false;
    const uint8_t *slot = n00bb_abi_object_row(view,
        N00B_ABI_TABLE_U32_STREAM, range.first + ordinal);
    if (!slot) return false;
    *out = core_le_load_u32(slot);
    return true;
}

static const n00b_abi_parameter_v1 *
object_parameter_at(const void *context, uint32_t index)
{
    return n00bb_abi_object_row(context, N00B_ABI_TABLE_PARAMETER, index);
}

static bool
object_parameter_name_length(const void *context,
                             const n00b_abi_string_v1 *name,
                             uint64_t *length)
{
    n00bb_abi_string_view_t value;
    if (!n00bb_abi_object_string(context, name, &value)) return false;
    *length = value.length;
    return true;
}

static n00bb_abi_parameter_reader_t
object_parameter_reader(const n00bb_abi_object_view_t *view)
{
    return (n00bb_abi_parameter_reader_t){
        .context = view,
        .index_at = object_parameter_index_at,
        .parameter_at = object_parameter_at,
        .name_length = object_parameter_name_length,
    };
}

static bool
row_owner_valid(const n00bb_abi_object_view_t *view, uint32_t kind,
                uint32_t local_index, const void *row)
{
    if (!local_index) return true;
    if (kind == N00B_ABI_TABLE_SHAPE)
        return n00bb_abi_shape_valid(row, object_shape_extent, view);
    if (kind == N00B_ABI_TABLE_CALL_SHAPE) {
        const n00b_abi_call_shape_v1 *shape = row;
        n00bb_abi_parameter_reader_t reader = object_parameter_reader(view);
        return n00bb_abi_parameter_contract_valid(&reader,
            shape->positional_parameters, shape->rest_parameter_index,
            shape->keyword_parameters, false);
    }
    if (kind == N00B_ABI_TABLE_MODULE) {
        const n00b_abi_module_v1 *module = row;
        if (!module_initializer_valid(view, local_index,
                module->initializer))
            return false;
        n00b_abi_range_v1 ranges[] = {module->static_storage,
                                       module->tls_storage};
        for (size_t slot = 0; slot < 2; slot++) {
            if (!valid_index_range(view, ranges[slot])) return false;
            for (uint32_t i = 0; i < ranges[slot].count; i++) {
                const n00b_abi_storage_v1 *storage = indexed_range_row(view,
                    ranges[slot], i, N00B_ABI_TABLE_STORAGE);
                if (!storage || storage->module_index != local_index
                    || (slot ? storage->segment_kind != N00B_ABI_STORAGE_TLS
                        : storage->segment_kind != N00B_ABI_STORAGE_READ_ONLY
                          && storage->segment_kind
                              != N00B_ABI_STORAGE_WRITABLE))
                    return false;
            }
        }
        return true;
    }
    if (kind == N00B_ABI_TABLE_FUNCTION) {
        const n00b_abi_function_v1 *function = row;
        const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SCOPE, function->scope_index);
        const n00b_abi_symbol_v1 *symbol = function->symbol_index
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_SYMBOL,
                function->symbol_index) : nullptr;
        if (!function->module_index || !scope
            || scope->module_index != function->module_index
            || (function->symbol_index
                && (!symbol || symbol->kind != N00B_ABI_SYMBOL_FUNCTION
                    || symbol->module_index != function->module_index
                    || symbol->scope_index != function->scope_index)))
            return false;
        if (symbol) {
            const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_TYPE, symbol->type_index);
            const n00b_abi_call_signature_v1 *signature = type
                ? n00bb_abi_object_row(view, N00B_ABI_TABLE_CALL_SIGNATURE,
                    type->call_signature_index) : nullptr;
            if (!type || type->kind != N00B_ABI_TYPE_FUNCTION
                || !signature || signature->type_index != symbol->type_index
                || signature->success_type_index
                    != function->success_type_index
                || signature->failure_type_index
                    != function->failure_type_index) return false;
        }
        n00bb_abi_parameter_reader_t reader = object_parameter_reader(view);
        return n00bb_abi_function_parameters_valid(&reader, function);
    }
    if (kind == N00B_ABI_TABLE_SCOPE) {
        const n00b_abi_scope_v1 *scope = row;
        const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_SCOPE);
        if (!table || !valid_index_range(view, scope->children)
            || !valid_index_range(view, scope->symbols)) return false;
        uint32_t parent_index = scope->parent_scope_index;
        for (uint32_t steps = 0; parent_index; steps++) {
            const n00b_abi_scope_v1 *parent = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SCOPE, parent_index);
            if (!parent || parent->module_index != scope->module_index
                || parent_index == local_index
                || steps >= table->record_count) return false;
            if (!steps && !range_owns_row_once(view, parent->children,
                                               local_index)) return false;
            parent_index = parent->parent_scope_index;
        }
        for (uint32_t i = 0; i < scope->children.count; i++) {
            const n00b_abi_scope_v1 *child = indexed_range_row(view,
                scope->children, i, N00B_ABI_TABLE_SCOPE);
            if (!child || child->parent_scope_index != local_index
                || child->module_index != scope->module_index) return false;
        }
        for (uint32_t i = 0; i < scope->symbols.count; i++) {
            const n00b_abi_symbol_v1 *symbol = indexed_range_row(view,
                scope->symbols, i, N00B_ABI_TABLE_SYMBOL);
            if (!symbol || symbol->scope_index != local_index
                || symbol->module_index != scope->module_index) return false;
        }
        return true;
    }
    if (kind == N00B_ABI_TABLE_SYMBOL) {
        const n00b_abi_symbol_v1 *symbol = row;
        const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SCOPE, symbol->scope_index);
        return scope && scope->module_index == symbol->module_index
            && range_owns_row_once(view, scope->symbols, local_index);
    }
    if (kind == N00B_ABI_TABLE_TYPE_ARGUMENT) {
        const n00b_abi_type_argument_v1 *argument = row;
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, argument->owner_type_index);
        if (!type || argument->ordinal >= type->named_arguments.count
            || type->named_arguments.first + argument->ordinal
                != local_index) return false;
        n00b_abi_string_v1 empty = {0};
        if (argument->kind == N00B_ABI_TYPE_ARG_TYPE)
            return !argument->scalar_bits
                && !memcmp(&argument->string_value, &empty, sizeof(empty))
                && n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE,
                    argument->type_index);
        if (argument->type_index) return false;
        if (argument->kind == N00B_ABI_TYPE_ARG_STRING)
            return !argument->scalar_bits
                && n00bb_abi_object_string(view,
                    &argument->string_value, &(n00bb_abi_string_view_t){0});
        return (argument->kind == N00B_ABI_TYPE_ARG_UINT
                || argument->kind == N00B_ABI_TYPE_ARG_SINT)
            && !memcmp(&argument->string_value, &empty, sizeof(empty));
    }
    if (kind == N00B_ABI_TABLE_LAYOUT || kind == N00B_ABI_TABLE_TRACE
        || kind == N00B_ABI_TABLE_CALL_SIGNATURE) {
        uint32_t parent = kind == N00B_ABI_TABLE_LAYOUT
            ? ((const n00b_abi_layout_v1 *)row)->type_index
            : kind == N00B_ABI_TABLE_TRACE
                ? ((const n00b_abi_trace_v1 *)row)->type_index
                : ((const n00b_abi_call_signature_v1 *)row)->type_index;
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, parent);
        bool owner_valid = type && (kind == N00B_ABI_TABLE_LAYOUT
            ? type->layout_index : kind == N00B_ABI_TABLE_TRACE
                ? type->tracing_index : type->call_signature_index)
            == local_index;
        if (!owner_valid || kind != N00B_ABI_TABLE_CALL_SIGNATURE)
            return owner_valid;
        const n00b_abi_call_signature_v1 *signature = row;
        n00bb_abi_parameter_reader_t reader = object_parameter_reader(view);
        return n00bb_abi_parameter_contract_valid(&reader,
            signature->positional_parameters, signature->rest_parameter_index,
            signature->keyword_parameters, true);
    }
    if (kind == N00B_ABI_TABLE_CODE_RANGE
        || kind == N00B_ABI_TABLE_FRAME_LAYOUT) {
        uint32_t parent = kind == N00B_ABI_TABLE_CODE_RANGE
            ? ((const n00b_abi_code_range_v1 *)row)->function_index
            : ((const n00b_abi_frame_layout_v1 *)row)->function_index;
        const n00b_abi_function_v1 *function = parent
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_FUNCTION, parent)
            : nullptr;
        return function && range_owns_row_once(view,
            kind == N00B_ABI_TABLE_CODE_RANGE
                ? function->code_ranges : function->frame_layouts,
            local_index);
    }
    if (kind == N00B_ABI_TABLE_TLS_RELOCATION) {
        const n00b_abi_tls_relocation_v1 *relocation = row;
        const n00b_abi_storage_v1 *storage =
            relocation->template_storage_index
                ? n00bb_abi_object_row(view, N00B_ABI_TABLE_STORAGE,
                    relocation->template_storage_index)
                : nullptr;
        return storage && storage->segment_kind == N00B_ABI_STORAGE_TLS
            && range_owns_row_once(view, storage->tls_relocations,
                                   local_index);
    }
    if (kind == N00B_ABI_TABLE_STORAGE) {
        const n00b_abi_storage_v1 *storage = row;
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, storage->module_index);
        if (!module || !range_owns_row_once(view,
                storage->segment_kind == N00B_ABI_STORAGE_TLS
                    ? module->tls_storage : module->static_storage,
                local_index)) return false;
        if (storage->symbol_index) {
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SYMBOL, storage->symbol_index);
            const n00b_abi_native_binding_v1 *binding = symbol
                ? n00bb_abi_object_row(view, N00B_ABI_TABLE_NATIVE_BINDING,
                    symbol->native_binding_index) : nullptr;
            bool mach_tlv = view->format == CORE_TARGET_OBJECT_MACH_O
                && storage->segment_kind == N00B_ABI_STORAGE_TLS;
            bool binding_valid = false;
            if (binding && mach_tlv) {
                core_object_section_t descriptor;
                binding_valid = binding->offset >= 0
                    && core_object_section_at(view->object,
                        view->object_bytes, view->format,
                        binding->section_index, &descriptor)
                    && (descriptor.flags & 0xff) == 0x13
                    && (uint64_t)binding->offset <= descriptor.size
                    && descriptor.size - (uint64_t)binding->offset >= 24
                    && (binding->offset % 24) == 0;
            } else if (binding) {
                binding_valid = binding->section_index
                    == storage->segment_index
                    && storage->byte_offset <= INT64_MAX
                    && binding->offset == (int64_t)storage->byte_offset;
            }
            if (!symbol || symbol->module_index != storage->module_index
                || symbol->type_index != storage->type_index
                || !symbol->native_binding_index || !binding
                || binding->kind != N00B_ABI_NATIVE_SECTION_OFFSET
                || !storage->segment_index
                || !binding_valid)
                return false;
        }
        if (!storage->instance_shape_index) return true;
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, storage->type_index);
        return type && type->kind == N00B_ABI_TYPE_ARRAY;
    }
    if (kind != N00B_ABI_TABLE_FIELD && kind != N00B_ABI_TABLE_ARM
        && kind != N00B_ABI_TABLE_ENUM_ITEM
        && kind != N00B_ABI_TABLE_TRAIT_METHOD) return true;
    uint32_t parent = n00bb_abi_member_facts(kind, row).parent;
    const n00b_abi_type_v1 *type = parent
        ? n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE, parent) : nullptr;
    return type && type_member_kind(type) == kind
        && range_owns_row_once(view, type->fields_or_arms, local_index);
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
        return !source.first;
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
            if (!local && target_kind != N00B_ABI_TABLE_TYPE
                && target_kind != UINT32_MAX) return false;
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
    if ((local_index || kind == N00B_ABI_TABLE_TYPE)
        && !n00bb_abi_row_values_valid(kind, source,
            table->record_bytes, nullptr)) return false;
    if ((kind == N00B_ABI_TABLE_TYPE
         && !type_backlinks_valid(view, source, local_index))
        || !row_owner_valid(view, kind, local_index, source)) return false;
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
        if (local_index && field->required && !local) return false;
        if (field->optional_type
            && local == N00B_ABI_V1_NO_TYPE_INDEX) mapped = local;
        else if (!remap_index(plan, input_index, field->target_kind,
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
        if (type->kind == N00B_ABI_TYPE_VARIABLE
            && !type->typevar_owner_scope_index
            && !type->typevar_owner_type_index
            && !type->typevar_import_requirement_index) return false;
        uint32_t start = type->specialization_start_ix;
        uint32_t end = type->specialization_end_ix;
        if (start || end) {
            uint32_t mapped_start, mapped_last;
            if (!start || end <= start
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
    core_free(rows->u32_kinds);
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
        .u32_kinds = core_alloc_array(uint32_t, capacity),
    };
    uint32_t *package_members = core_alloc_array(uint32_t, packages);
    uint32_t *package_written = core_alloc_array(uint32_t, packages);
    uint32_t *scc_members = core_alloc_array(uint32_t, sccs);
    uint32_t *scc_written = core_alloc_array(uint32_t, sccs);
    bool *package_seen = core_alloc_array(bool, packages);
    bool *scc_seen = core_alloc_array(bool, sccs);
    n00bb_abi_u32_stream_build_t stream = {
        .values = built.u32_stream, .kinds = built.u32_kinds,
        .count = base_stream,
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
        built.u32_kinds[range.first + package_written[package_index] - 1] =
            N00B_ABI_TABLE_MODULE;
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
        built.u32_kinds[range.first + scc_written[scc_index] - 1] =
            N00B_ABI_TABLE_MODULE;
        built.u32_stream[built.sccs[scc_index].lexical_order.first
                          + ordinal] = module_index;
        built.u32_kinds[built.sccs[scc_index].lexical_order.first
                         + ordinal] = N00B_ABI_TABLE_MODULE;
    }
    for (uint32_t i = 1; valid && i < packages; i++)
        valid = package_written[i] == package_members[i];
    for (uint32_t i = 1; valid && i < packages; i++) {
        uint32_t parent = i;
        for (uint32_t depth = 0; parent; depth++) {
            if (depth >= packages || parent >= packages
                || !package_seen[parent]) {
                valid = false;
                break;
            }
            parent = built.packages[parent].parent_package_index;
        }
    }
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
    ABI_STRING_FIELD(TYPE, type, named_constructor),
    ABI_STRING_FIELD(TYPE_ARGUMENT, type_argument, string_value),
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
                (n00b_abi_string_tag_v1)header.tag,
                kind, final_index, field->offset,
                (void *)(destination + field->offset))) return false;
    }
    return true;
}

typedef struct {
    n00bb_abi_string_view_t name;
    uint32_t final_index;
} abi_module_target_t;

static core_hash_value_t
module_target_hash(void *value)
{
    const abi_module_target_t *target = value;
    return core_hash_raw(target->name.bytes, target->name.length);
}

static bool
module_target_eq(const void *left, const void *right)
{
    const abi_module_target_t *a = left, *b = right;
    return a->name.length == b->name.length
        && !memcmp(a->name.bytes, b->name.bytes, a->name.length);
}

static bool
module_import_count(const n00bb_abi_selected_objects_t *selected,
                    uint32_t *out_count)
{
    uint32_t count = 0;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_REQUIREMENT);
        if (!table || !table->record_count) return false;
        for (uint32_t local = 1; local < table->record_count; local++) {
            const n00b_abi_requirement_v1 *row = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_REQUIREMENT, local);
            if (row->symbol_kind != N00B_ABI_SYMBOL_MODULE) continue;
            if (row->requesting_module_index != 1 || count == UINT32_MAX)
                return false;
            count++;
        }
    }
    *out_count = count;
    return true;
}

static bool
resolve_module_imports(const n00bb_abi_selected_objects_t *selected,
                       const n00bb_abi_index_plan_t *plan,
                       n00bb_abi_merged_rows_t *merged,
                       uint32_t first_import)
{
    n00bb_abi_merged_table_t *modules = nullptr, *stream = nullptr;
    for (size_t i = 0; i < merged->table_count; i++) {
        if (merged->tables[i].kind == N00B_ABI_TABLE_MODULE)
            modules = &merged->tables[i];
        else if (merged->tables[i].kind == N00B_ABI_TABLE_U32_STREAM)
            stream = &merged->tables[i];
    }
    if (!modules || !stream || stream->record_bytes != sizeof(uint32_t))
        return false;

    abi_module_target_t *targets = core_alloc_array(
        abi_module_target_t, selected->count);
    core_indexed_table_t *by_name = core_indexed_table_new(0, UINT32_MAX,
        module_target_hash, module_target_eq);
    bool valid = by_name != nullptr;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00b_abi_module_v1 *source = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, 1);
        uint32_t final;
        valid = source && n00bb_abi_object_string(view,
            &source->full_path, &targets[input].name)
            && n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_MODULE, 1, &final)
            && final && final < modules->record_count;
        if (!valid) break;
        targets[input].final_index = final;
        bool inserted = false;
        valid = core_indexed_table_intern(by_name, &targets[input],
            &targets[input], &inserted) && inserted;
    }
    uint32_t cursor = first_import;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_REQUIREMENT);
        n00b_abi_module_v1 *module = (void *)(modules->records
            + (size_t)targets[input].final_index * modules->record_bytes);
        uint32_t start = cursor;
        for (uint32_t local = 1; valid && local < table->record_count;
             local++) {
            const n00b_abi_requirement_v1 *row = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_REQUIREMENT, local);
            if (row->symbol_kind != N00B_ABI_SYMBOL_MODULE) continue;
            n00bb_abi_string_view_t path;
            valid = n00bb_abi_object_string(view, &row->full_path, &path);
            if (!valid) break;
            abi_module_target_t lookup = {.name = path};
            const core_indexed_entry_t *found = core_indexed_table_find(
                by_name, &lookup);
            valid = found && cursor < stream->record_count;
            if (!valid) break;
            const abi_module_target_t *target = found->value;
            core_le_store_u32(stream->records
                + (size_t)cursor * sizeof(uint32_t), target->final_index);
            cursor++;
        }
        if (!valid) break;
        if (cursor != start) {
            valid = !module->imports.first && !module->imports.count;
            module->imports = (n00b_abi_range_v1){
                .first = start, .count = cursor - start,
            };
        }
    }
    valid = valid && cursor == stream->record_count;
    core_indexed_table_free(by_name);
    core_free(targets);
    return valid;
}

const n00bb_abi_merged_table_t *
n00bb_abi_merged_table(const n00bb_abi_merged_rows_t *merged, uint32_t kind)
{
    if (!merged || !merged->tables) return nullptr;
    for (size_t i = 0; i < merged->table_count; i++)
        if (merged->tables[i].kind == kind) return &merged->tables[i];
    return nullptr;
}

static n00bb_abi_merged_table_t *
merged_table_mut(n00bb_abi_merged_rows_t *merged, uint32_t kind)
{
    return (n00bb_abi_merged_table_t *)n00bb_abi_merged_table(merged, kind);
}

static bool
merged_row_append(n00bb_abi_merged_table_t *table, const void *row,
                  uint32_t *index)
{
    if (!table || !row || !table->record_bytes
        || table->record_count == UINT32_MAX
        || table->record_count >= SIZE_MAX / table->record_bytes)
        return false;
    uint32_t next = table->record_count;
    table->records = core_realloc(table->records,
        ((size_t)next + 1) * table->record_bytes);
    if (!table->records) return false;
    memcpy(table->records + (size_t)next * table->record_bytes,
           row, table->record_bytes);
    table->record_count++;
    if (index) *index = next;
    return true;
}

typedef enum {
    LINK_TRACE_UNSEEN,
    LINK_TRACE_ACTIVE,
    LINK_TRACE_NONE,
    LINK_TRACE_PRESENT,
    LINK_TRACE_INVALID,
} link_trace_state_t;

typedef struct {
    n00bb_abi_merged_rows_t *merged;
    uint8_t *states;
} link_trace_build_t;

static bool
merged_index_at(const n00bb_abi_merged_rows_t *merged,
                n00b_abi_range_v1 range, uint32_t ordinal,
                uint32_t *index)
{
    const n00bb_abi_merged_table_t *stream = n00bb_abi_merged_table(
        merged, N00B_ABI_TABLE_U32_STREAM);
    if (!stream || ordinal >= range.count
        || range.first >= stream->record_count
        || ordinal >= stream->record_count - range.first) return false;
    *index = ((const uint32_t *)stream->records)[range.first + ordinal];
    return true;
}

static const n00b_abi_field_v1 *
merged_trace_field(const n00bb_abi_merged_rows_t *merged,
                   const n00b_abi_type_v1 *type, uint32_t ordinal,
                   uint32_t *arm_index)
{
    uint32_t index = 0;
    if (!merged_index_at(merged, type->fields_or_arms, ordinal, &index)
        || !index) return nullptr;
    if (type->kind == N00B_ABI_TYPE_VARIANT) {
        const n00b_abi_arm_v1 *arm = n00bb_abi_merged_row(merged,
            N00B_ABI_TABLE_ARM, index);
        if (arm_index) *arm_index = index;
        return arm ? &arm->field : nullptr;
    }
    return n00bb_abi_merged_row(merged, N00B_ABI_TABLE_FIELD, index);
}

static uint32_t
merged_trace_member_type(const n00b_abi_field_v1 *field)
{
    return field->inline_element_count
        ? field->inline_element_type_index : field->child_type_index;
}

static bool
merged_trace_child(link_trace_build_t *build,
                   const n00b_abi_field_v1 *field,
                   n00b_abi_range_v1 *range)
{
    n00bb_abi_merged_table_t *children = merged_table_mut(build->merged,
        N00B_ABI_TABLE_TRACE_CHILD);
    n00bb_abi_merged_table_t *stream = merged_table_mut(build->merged,
        N00B_ABI_TABLE_U32_STREAM);
    n00b_abi_trace_child_v1 row = {
        .byte_offset = field->logical_bit_offset / 8,
        .child_type_index = merged_trace_member_type(field),
        .repeat_count = field->inline_element_count,
        .stride_bits = field->inline_element_stride_bits,
    };
    uint32_t index = 0;
    if (field->logical_bit_offset % 8 || range->count == UINT32_MAX
        || !merged_row_append(children, &row, &index)) return false;
    if (!range->count) range->first = stream->record_count;
    if (!merged_row_append(stream, &index, nullptr)) return false;
    range->count++;
    return true;
}

static link_trace_state_t
merged_trace_complete(link_trace_build_t *build, uint32_t type_index)
{
    n00bb_abi_merged_rows_t *merged = build->merged;
    n00bb_abi_merged_table_t *types = merged_table_mut(merged,
        N00B_ABI_TABLE_TYPE);
    if (!types || type_index >= types->record_count) return LINK_TRACE_INVALID;
    link_trace_state_t state = build->states[type_index];
    if (state) return state == LINK_TRACE_ACTIVE
        ? LINK_TRACE_INVALID : state;
    n00b_abi_type_v1 *type = (n00b_abi_type_v1 *)types->records + type_index;
    if (type->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
        return build->states[type_index] = LINK_TRACE_NONE;
    if (type->tracing_index)
        return build->states[type_index] = LINK_TRACE_PRESENT;
    if (!type->layout_index)
        return build->states[type_index] = LINK_TRACE_NONE;
    build->states[type_index] = LINK_TRACE_ACTIVE;
    n00b_abi_trace_v1 trace = {.type_index = type_index};
    state = LINK_TRACE_NONE;
    if (n00bb_abi_traced_leaf(type))
        state = LINK_TRACE_PRESENT;
    else if (type->kind == N00B_ABI_TYPE_OPTION
        || type->kind == N00B_ABI_TYPE_WRAPPER
        || type->kind == N00B_ABI_TYPE_ATOMIC) {
        uint32_t child = 0;
        if (type->components.count != 1
            || !merged_index_at(merged, type->components, 0, &child))
            state = LINK_TRACE_INVALID;
        else {
            state = merged_trace_complete(build, child);
            if (state == LINK_TRACE_PRESENT) {
                const n00b_abi_type_v1 *child_type =
                    (const n00b_abi_type_v1 *)types->records + child;
                trace.child_trace_index = child_type->tracing_index;
                if (type->kind == N00B_ABI_TYPE_OPTION) {
                    const n00b_abi_layout_v1 *layout = n00bb_abi_merged_row(
                        merged, N00B_ABI_TABLE_LAYOUT, type->layout_index);
                    const n00b_abi_tag_extractor_v1 *tag = layout
                        ? n00bb_abi_merged_row(merged,
                            N00B_ABI_TABLE_TAG_EXTRACTOR,
                            layout->tag_extractor_index) : nullptr;
                    if (!tag) state = LINK_TRACE_INVALID;
                    else {
                        trace.tag_extractor_index =
                            layout->tag_extractor_index;
                        if (tag->kind == N00B_ABI_TAG_DIRECT_BITS)
                            trace.option_bit_offset =
                                tag->unit_byte_offset * 8 + tag->lsb_position;
                    }
                }
            }
        }
    }
    else if (type->kind == N00B_ABI_TYPE_RECORD
             || type->kind == N00B_ABI_TYPE_TUPLE
             || type->kind == N00B_ABI_TYPE_VARIANT) {
        bool variant = type->kind == N00B_ABI_TYPE_VARIANT;
        if (variant) {
            const n00b_abi_layout_v1 *layout = n00bb_abi_merged_row(merged,
                N00B_ABI_TABLE_LAYOUT, type->layout_index);
            if (!layout || !layout->tag_extractor_index)
                state = LINK_TRACE_INVALID;
            else trace.tag_extractor_index = layout->tag_extractor_index;
        }
        for (uint32_t i = 0; state != LINK_TRACE_INVALID
             && i < type->fields_or_arms.count; i++) {
            const n00b_abi_field_v1 *field = merged_trace_field(merged,
                type, i, nullptr);
            if (!field) { state = LINK_TRACE_INVALID; break; }
            link_trace_state_t child = merged_trace_complete(build,
                merged_trace_member_type(field));
            if (child == LINK_TRACE_INVALID) {
                state = child;
                break;
            }
            if (child == LINK_TRACE_PRESENT) state = LINK_TRACE_PRESENT;
        }
        if (state == LINK_TRACE_PRESENT) {
            uint32_t arm_first = 0;
            for (uint32_t i = 0; i < type->fields_or_arms.count; i++) {
                uint32_t arm_index = 0;
                const n00b_abi_field_v1 *field = merged_trace_field(merged,
                    type, i, &arm_index);
                if (!field) { state = LINK_TRACE_INVALID; break; }
                uint32_t child_index = merged_trace_member_type(field);
                uint64_t offset = field->logical_bit_offset;
                n00b_abi_trace_arm_v1 arm = {0};
                n00b_abi_range_v1 *range = variant
                    ? &arm.children : &trace.fixed_children;
                if (build->states[child_index] == LINK_TRACE_PRESENT) {
                    const n00b_abi_type_v1 *child_type =
                        (const n00b_abi_type_v1 *)types->records + child_index;
                    uint64_t count = field->inline_element_count
                        ? field->inline_element_count : 1;
                    uint64_t stride = field->inline_element_stride_bits;
                    bool overflow = count > 1 && (!stride ||
                        (count - 1) > (UINT64_MAX - offset) / stride);
                    uint64_t last = overflow ? 0
                        : offset + (count - 1) * stride;
                    if (!child_type->alignment_bytes || offset % 8
                        || overflow || last > type->size_bits
                        || child_type->size_bits > type->size_bits - last
                        || (offset / 8) % child_type->alignment_bytes
                        || (stride && (stride % 8
                            || (stride / 8)
                                % child_type->alignment_bytes))
                        || !merged_trace_child(build, field, range)) {
                        state = LINK_TRACE_INVALID;
                        break;
                    }
                }
                if (variant) {
                    const n00b_abi_arm_v1 *source = n00bb_abi_merged_row(
                        merged, N00B_ABI_TABLE_ARM, arm_index);
                    n00bb_abi_merged_table_t *arms = merged_table_mut(merged,
                        N00B_ABI_TABLE_TRACE_ARM);
                    if (!source) { state = LINK_TRACE_INVALID; break; }
                    arm.tag_value = source->tag_value;
                    if (!merged_row_append(arms, &arm, &arm_first)) {
                        state = LINK_TRACE_INVALID;
                        break;
                    }
                    if (!trace.arms.count) trace.arms.first = arm_first;
                    trace.arms.count++;
                }
            }
            if (state == LINK_TRACE_PRESENT && variant) {
                n00bb_abi_merged_table_t *stream = merged_table_mut(merged,
                    N00B_ABI_TABLE_U32_STREAM);
                uint32_t first = trace.arms.first;
                trace.arms.first = stream->record_count;
                for (uint32_t i = 0; state != LINK_TRACE_INVALID
                     && i < trace.arms.count; i++) {
                    uint32_t arm_index = first + i;
                    if (!merged_row_append(stream, &arm_index, nullptr))
                        state = LINK_TRACE_INVALID;
                }
            }
        }
    }
    if (state == LINK_TRACE_PRESENT) {
        n00bb_abi_merged_table_t *traces = merged_table_mut(merged,
            N00B_ABI_TABLE_TRACE);
        uint32_t index = 0;
        if (!merged_row_append(traces, &trace, &index))
            state = LINK_TRACE_INVALID;
        else type->tracing_index = index;
    }
    return build->states[type_index] = state;
}

static bool
complete_merged_traces(n00bb_abi_merged_rows_t *merged)
{
    const n00bb_abi_merged_table_t *types = n00bb_abi_merged_table(merged,
        N00B_ABI_TABLE_TYPE);
    if (!types) return false;
    uint8_t *states = core_alloc_array(uint8_t, types->record_count);
    if (!states) return false;
    memset(states, 0, types->record_count);
    link_trace_build_t build = {.merged = merged, .states = states};
    bool valid = true;
    for (uint32_t i = 1; valid && i < types->record_count; i++)
        valid = merged_trace_complete(&build, i) != LINK_TRACE_INVALID;
    core_free(states);
    return valid;
}

const void *
n00bb_abi_merged_row(const n00bb_abi_merged_rows_t *merged,
                     uint32_t kind, uint32_t index)
{
    const n00bb_abi_merged_table_t *table = n00bb_abi_merged_table(
        merged, kind);
    return table && index < table->record_count
        ? table->records + (size_t)index * table->record_bytes : nullptr;
}

void
n00bb_abi_merged_rows_release(n00bb_abi_merged_rows_t *merged)
{
    if (!merged) return;
    for (size_t i = 0; i < merged->table_count; i++) {
        core_free(merged->tables[i].records);
        core_free(merged->tables[i].origins);
    }
    core_free(merged->tables);
    n00bb_abi_name_pool_free(merged->strings);
    *merged = (n00bb_abi_merged_rows_t){0};
}

void
n00bb_abi_native_sections_release(n00bb_abi_native_sections_t *sections)
{
    if (!sections) return;
    for (size_t i = 0; i < sections->count; i++) {
        core_free(sections->sections[i].name.data);
        core_free(sections->sections[i].segment.data);
        core_free((void *)sections->sections[i].bytes);
    }
    core_free(sections->sections);
    core_free(sections->final_indices);
    core_free(sections->maps);
    *sections = (n00bb_abi_native_sections_t){0};
}

static bool
same_native_name(core_string_t left, core_string_t right)
{
    return left.data && right.data ? core_string_eq(left, right)
                                   : left.data == right.data;
}

static bool
same_native_kind(const core_object_emit_section_t *left,
                 const core_object_emit_section_t *right)
{
    return same_native_name(left->name, right->name)
        && same_native_name(left->segment, right->segment)
        && left->writable == right->writable
        && left->executable == right->executable
        && left->is_thread_local == right->is_thread_local
        && left->is_thread_local_descriptor
            == right->is_thread_local_descriptor;
}

static bool
place_native_section(n00bb_abi_native_sections_t *built,
                     core_object_emit_section_t piece, size_t input,
                     uint32_t native)
{
    size_t section = 0;
    while (section < built->count
           && !same_native_kind(&built->sections[section], &piece))
        section++;
    uint64_t offset = 0;
    if (section == built->count) built->sections[built->count++] = piece;
    else {
        core_object_emit_section_t *merged = &built->sections[section];
        uint64_t align = piece.alignment;
        offset = (merged->size + align - 1) / align * align;
        if (offset < merged->size || piece.size > SIZE_MAX - offset)
            return false;
        uint8_t *bytes = offset + piece.size
            ? core_realloc((void *)merged->bytes, offset + piece.size)
            : nullptr;
        if (offset + piece.size && !bytes) return false;
        memset(bytes + merged->size, 0, offset - merged->size);
        if (piece.size) memcpy(bytes + offset, piece.bytes, piece.size);
        merged->bytes = bytes;
        merged->size = offset + piece.size;
        if (piece.alignment > merged->alignment)
            merged->alignment = piece.alignment;
        core_free(piece.name.data);
        core_free(piece.segment.data);
        core_free((void *)piece.bytes);
    }
    if (section > UINT32_MAX) return false;
    built->maps[built->map_count++] = (n00bb_abi_native_section_map_t){
        .input = input, .local_section_index = native,
        .section = (uint32_t)section, .base_offset = offset,
        .size = piece.size,
    };
    return true;
}

static core_string_t
copy_native_name(const uint8_t *data, size_t length)
{
    if (!data || !length || length >= INT64_MAX) return (core_string_t){};
    return core_string_from_raw((const char *)data, (int64_t)length);
}

bool
n00bb_abi_collect_native_sections(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    n00bb_abi_native_sections_t *out)
{
    n00bb_abi_image_key_t key;
    if (!selected || !selected->views || !selected->count || !out
        || !n00bb_abi_image_key_for_target(target, &key)) return false;
    size_t capacity = 0;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        uint32_t count;
        if (view->format != target->object_format
            || !core_object_section_count(view->object, view->object_bytes,
                                          view->format, &count)
            || count > UINT32_MAX - 1 - capacity) return false;
        capacity += count;
    }
    n00bb_abi_native_sections_t built = {
        .sections = core_alloc_array(core_object_emit_section_t, capacity),
        .maps = core_alloc_array(n00bb_abi_native_section_map_t, capacity),
    };
    bool valid = true;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        uint32_t count;
        valid = core_object_section_count(view->object, view->object_bytes,
                                          view->format, &count);
        for (uint32_t native = 1; valid && native <= count; native++) {
            if (native == view->metadata.native_index) continue;
            core_object_section_t source;
            valid = core_object_section_at(view->object, view->object_bytes,
                                           view->format, native, &source);
            if (!valid) break;
            if (view->format == CORE_TARGET_OBJECT_ELF
                && !(source.flags & UINT64_C(2))) continue;
            bool mach = view->format == CORE_TARGET_OBJECT_MACH_O;
            bool coff = view->format == CORE_TARGET_OBJECT_COFF;
            valid = source.name_length && (!mach || source.segment_length)
                && source.alignment
                && source.alignment <= UINT32_MAX
                && (source.alignment & (source.alignment - 1)) == 0;
            if (!valid) break;
            core_string_t name = copy_native_name(source.name,
                                                  source.name_length);
            core_string_t segment = source.segment_length
                ? copy_native_name(source.segment, source.segment_length)
                : (core_string_t){};
            uint8_t *bytes = source.size
                ? core_alloc_size(1, source.size) : nullptr;
            if (source.size) memcpy(bytes, source.bytes, source.size);
            bool tls = mach ? (source.flags & 0xff) == 0x11
                : coff ? source.name_length >= 5
                    && !memcmp(source.name, ".tls$", 5)
                : (source.flags & UINT64_C(0x400)) != 0;
            bool writable = mach
                ? !core_string_eq(segment, CORE_STRING_STATIC("__TEXT"))
                    && !core_string_eq(segment,
                                       CORE_STRING_STATIC("__DATA_CONST"))
                : coff ? (source.flags & UINT64_C(0x80000000)) != 0
                : (source.flags & UINT64_C(1)) != 0;
            bool executable = mach
                ? core_string_eq(segment, CORE_STRING_STATIC("__TEXT"))
                : coff ? (source.flags & UINT64_C(0x20000000)) != 0
                : (source.flags & UINT64_C(4)) != 0;
            core_object_emit_section_t piece = {
                .name = name, .segment = segment,
                .bytes = bytes, .size = source.size,
                .alignment = (uint32_t)source.alignment,
                .writable = writable, .executable = executable,
                .is_thread_local = tls,
                .is_thread_local_descriptor = mach
                    && (source.flags & 0xff) == 0x13,
            };
            valid = name.data && (!mach || segment.data)
                && place_native_section(&built, piece, input, native);
        }
    }
    built.final_indices = valid
        ? core_alloc_array(uint32_t, built.count + 1) : nullptr;
    valid = valid && built.final_indices;
    for (size_t i = 0; valid && i < built.count; i++) {
        valid = core_object_emit_native_section_index(target,
            1 + built.count, (uint32_t)(2 + i), &built.final_indices[i]);
    }
    for (size_t i = 0; valid && i < built.map_count; i++)
        built.maps[i].final_section_index =
            built.final_indices[built.maps[i].section];
    if (!valid) {
        n00bb_abi_native_sections_release(&built);
        return false;
    }
    *out = built;
    return true;
}

void
n00bb_abi_native_relocations_release(
    n00bb_abi_native_relocations_t *relocations)
{
    if (!relocations) return;
    for (size_t i = 0; i < relocations->symbol_count; i++)
        core_free(relocations->symbols[i].name.data);
    core_free(relocations->symbols);
    core_free(relocations->relocations);
    *relocations = (n00bb_abi_native_relocations_t){0};
}

typedef struct {
    size_t input;
    uint32_t native_index;
    uint32_t symbol_table_index;
    bool is_section;
} abi_native_symbol_key_t;

static core_hash_value_t
native_symbol_hash(void *value)
{
    const abi_native_symbol_key_t *key = value;
    uint64_t parts[] = {
        key->input,
        ((uint64_t)key->native_index << 32) | key->symbol_table_index,
        key->is_section,
    };
    return core_hash_raw(parts, sizeof(parts));
}

static bool
native_symbol_eq(const void *left, const void *right)
{
    const abi_native_symbol_key_t *a = left, *b = right;
    return a->input == b->input
        && a->native_index == b->native_index
        && a->symbol_table_index == b->symbol_table_index
        && a->is_section == b->is_section;
}

static const n00bb_abi_native_section_map_t *
native_section_piece(const n00bb_abi_native_sections_t *sections,
                     size_t input, uint32_t local_index)
{
    const n00bb_abi_native_section_map_t *found = nullptr;
    for (size_t i = 0; i < sections->map_count; i++) {
        if (sections->maps[i].input != input
            || sections->maps[i].local_section_index != local_index)
            continue;
        if (found || sections->maps[i].section > UINT32_MAX - 2)
            return nullptr;
        found = &sections->maps[i];
    }
    return found;
}

bool
n00bb_abi_collect_native_relocations(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *out)
{
    if (!selected || !selected->views || !selected->count || !sections
        || (sections->map_count && (!sections->sections || !sections->maps))
        || !out) return false;
    size_t capacity = 0;
    for (size_t i = 0; i < sections->map_count; i++) {
        const n00bb_abi_native_section_map_t *map = &sections->maps[i];
        if (map->input >= selected->count) return false;
        const n00bb_abi_object_view_t *view = &selected->views[map->input];
        core_object_section_t source;
        if (!core_object_section_at(view->object, view->object_bytes,
                                    view->format, map->local_section_index,
                                    &source)
            || source.relocation_count > UINT32_MAX - capacity)
            return false;
        capacity += source.relocation_count;
    }
    n00bb_abi_native_relocations_t built = {
        .symbols = core_alloc_array(core_object_emit_symbol_t, capacity),
        .relocations = core_alloc_array(core_object_emit_absolute64_t,
                                        capacity),
    };
    abi_native_symbol_key_t *keys = core_alloc_array(
        abi_native_symbol_key_t, capacity);
    core_indexed_table_t *seen = core_indexed_table_new(0, UINT32_MAX,
        native_symbol_hash, native_symbol_eq);
    bool valid = seen != nullptr;
    for (size_t i = 0; valid && i < sections->map_count; i++) {
        const n00bb_abi_native_section_map_t *map = &sections->maps[i];
        const n00bb_abi_object_view_t *view = &selected->views[map->input];
        core_object_section_t source;
        valid = core_object_section_at(view->object, view->object_bytes,
            view->format, map->local_section_index, &source);
        for (uint32_t j = 0; valid && j < source.relocation_count; j++) {
            core_object_absolute64_relocation_t relocation;
            valid = core_object_absolute64_relocation_at(&source,
                view->format, j, &relocation);
            if (!valid) break;
            abi_native_symbol_key_t key = {
                .input = map->input,
                .native_index = relocation.native_symbol_index,
                .symbol_table_index = view->format == CORE_TARGET_OBJECT_ELF
                    ? source.native_symbol_table_index : 0,
                .is_section = relocation.target_is_section,
            };
            if (key.is_section) {
                core_object_section_t target;
                valid = core_object_section_at(view->object,
                    view->object_bytes, view->format, key.native_index,
                    &target);
                if (!valid) break;
                relocation.addend_bits -= target.virtual_address;
            }
            const core_indexed_entry_t *entry =
                core_indexed_table_find(seen, &key);
            uint32_t symbol_index;
            if (entry) symbol_index = entry->index;
            else {
                core_object_emit_symbol_t symbol = {0};
                if (key.is_section) {
                    char name[sizeof(size_t) * 3 + sizeof(uint32_t) * 3
                              + sizeof("__n00bb_section__")];
                    int length = snprintf(name, sizeof(name),
                        "__n00bb_section_%zu_%u", key.input,
                        key.native_index);
                    const n00bb_abi_native_section_map_t *piece =
                        native_section_piece(sections, key.input,
                                             key.native_index);
                    valid = length > 0 && (size_t)length < sizeof(name)
                        && piece;
                    if (valid) {
                        symbol.section_index = piece->section + 2;
                        symbol.section_offset = piece->base_offset;
                        symbol.name = copy_native_name(
                            (const uint8_t *)name, (size_t)length);
                    }
                }
                else {
                    core_object_symbol_t source_symbol;
                    valid = core_object_symbol_at(view->object,
                        view->object_bytes, view->format, &source,
                        key.native_index, &source_symbol)
                        && source_symbol.name.length;
                    uint64_t base = 0;
                    if (valid && source_symbol.native_section_index) {
                        core_object_section_t target;
                        const n00bb_abi_native_section_map_t *piece =
                            nullptr;
                        valid = core_object_section_at(view->object,
                            view->object_bytes, view->format,
                            source_symbol.native_section_index, &target)
                            && source_symbol.section_offset <= target.size
                            && (piece = native_section_piece(sections,
                                key.input,
                                source_symbol.native_section_index));
                        if (valid) {
                            symbol.section_index = piece->section + 2;
                            base = piece->base_offset;
                        }
                    }
                    if (valid) {
                        symbol.name = copy_native_name(
                            source_symbol.name.bytes,
                            source_symbol.name.length);
                        symbol.section_offset = base
                            + source_symbol.section_offset;
                        symbol.exported = source_symbol.exported;
                        valid = symbol.section_index || symbol.exported;
                    }
                }
                if (!valid || !symbol.name.data) {
                    core_free(symbol.name.data);
                    break;
                }
                symbol_index = (uint32_t)built.symbol_count;
                keys[symbol_index] = key;
                entry = core_indexed_table_intern(seen, &keys[symbol_index],
                    &built.symbols[symbol_index], nullptr);
                if (!entry || entry->index != symbol_index) {
                    core_free(symbol.name.data);
                    valid = false;
                    break;
                }
                built.symbols[built.symbol_count++] = symbol;
            }
            if (built.relocation_count == capacity) {
                valid = false;
                break;
            }
            built.relocations[built.relocation_count++] =
                (core_object_emit_absolute64_t){
                    .section_index = map->section + 2,
                    .section_offset = map->base_offset
                        + relocation.section_offset,
                    .symbol_index = symbol_index,
                    .addend_bits = relocation.addend_bits,
                    .symbol_relocation = view->format
                        == CORE_TARGET_OBJECT_MACH_O
                        && !relocation.target_is_section,
                };
        }
    }
    core_indexed_table_free(seen);
    core_free(keys);
    if (!valid) {
        n00bb_abi_native_relocations_release(&built);
        return false;
    }
    *out = built;
    return true;
}

typedef struct {
    void *row;
    uint32_t kind;
    uint32_t section_index;
    uint64_t storage_offset;
    int64_t binding_offset;
} abi_native_rebase_t;

static size_t
rebased_row_bytes(uint32_t kind)
{
    return kind == N00B_ABI_TABLE_STORAGE ? sizeof(n00b_abi_storage_v1)
        : kind == N00B_ABI_TABLE_BAKED_NODE ? sizeof(n00b_abi_baked_node_v1)
        : kind == N00B_ABI_TABLE_NATIVE_BINDING
            ? sizeof(n00b_abi_native_binding_v1) : 0;
}

static uint32_t *
rebased_section(uint32_t kind, void *row)
{
    return kind == N00B_ABI_TABLE_STORAGE
        ? &((n00b_abi_storage_v1 *)row)->segment_index
        : kind == N00B_ABI_TABLE_BAKED_NODE
            ? &((n00b_abi_baked_node_v1 *)row)->segment_index
            : &((n00b_abi_native_binding_v1 *)row)->section_index;
}

static uint64_t *
rebased_offset(uint32_t kind, void *row)
{
    return kind == N00B_ABI_TABLE_STORAGE
        ? &((n00b_abi_storage_v1 *)row)->byte_offset
        : kind == N00B_ABI_TABLE_BAKED_NODE
            ? &((n00b_abi_baked_node_v1 *)row)->byte_offset : nullptr;
}

bool
n00bb_abi_rebase_native_references(
    n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_section_map_t *maps, size_t map_count,
    const n00bb_abi_string_storage_rebase_t *string_storage,
    n00bb_abi_native_rebase_failure_t *failure)
{
    if (failure) *failure = (n00bb_abi_native_rebase_failure_t){0};
    if (!merged || !merged->tables || (map_count && !maps)) return false;
    size_t capacity = 0;
    for (size_t ti = 0; ti < merged->table_count; ti++) {
        const n00bb_abi_merged_table_t *table = &merged->tables[ti];
        size_t row_bytes = rebased_row_bytes(table->kind);
        if (!row_bytes) continue;
        if ((table->record_count && (!table->records || !table->origins))
            || table->record_bytes != row_bytes
            || table->record_count > SIZE_MAX - capacity) {
            if (failure) *failure = (n00bb_abi_native_rebase_failure_t){
                .table_kind = table->kind,
                .reason = !table->records
                    ? CORE_STRING_STATIC("missing merged row bytes")
                    : !table->origins
                    ? CORE_STRING_STATIC("missing merged row origins")
                    : table->record_bytes != row_bytes
                        ? CORE_STRING_STATIC("merged row width mismatch")
                        : CORE_STRING_STATIC("merged row count overflow"),
            };
            return false;
        }
        capacity += table->record_count;
    }
    abi_native_rebase_t *changes = core_alloc_array(
        abi_native_rebase_t, capacity);
    size_t change_count = 0;
    bool valid = true;
    for (size_t ti = 0; valid && ti < merged->table_count; ti++) {
        n00bb_abi_merged_table_t *table = &merged->tables[ti];
        if (!rebased_row_bytes(table->kind)) continue;
        for (uint32_t row_index = 1; valid
             && row_index < table->record_count; row_index++) {
            void *row = table->records
                + (size_t)row_index * table->record_bytes;
            const n00bb_abi_row_origin_t *origin =
                &table->origins[row_index];
            if (table->kind == N00B_ABI_TABLE_NATIVE_BINDING
                && ((n00b_abi_native_binding_v1 *)row)->kind
                    == N00B_ABI_NATIVE_EXTERNAL_NAME) continue;
            uint32_t local_section = *rebased_section(table->kind, row);
            const n00bb_abi_native_section_map_t *match = nullptr;
            valid = origin->present && local_section != 0;
            core_string_t reason = valid
                ? CORE_STRING_STATIC("no matching native section")
                : CORE_STRING_STATIC("missing row origin or local section");
            for (size_t i = 0; valid && i < map_count; i++) {
                if (maps[i].input != origin->input
                    || maps[i].local_section_index != local_section)
                    continue;
                if (match) {
                    valid = false;
                    reason = CORE_STRING_STATIC(
                        "multiple native section matches");
                }
                else match = &maps[i];
            }
            if (valid && !match && table->kind == N00B_ABI_TABLE_STORAGE
                && string_storage && string_storage->selected
                && string_storage->pool
                && string_storage->final_section_index
                && origin->input < string_storage->selected->count) {
                const n00b_abi_storage_v1 *storage = row;
                const n00bb_abi_object_view_t *view =
                    &string_storage->selected->views[origin->input];
                core_object_section_t pool_section;
                core_string_t pool_name = view->format
                    == CORE_TARGET_OBJECT_MACH_O
                    ? CORE_STRING_STATIC("__n00b_strings")
                    : view->format == CORE_TARGET_OBJECT_COFF
                        ? CORE_STRING_STATIC(".rdata$S")
                        : CORE_STRING_STATIC(".rodata.n00b_strings");
                valid = !storage->symbol_index && !storage->type_index
                    && storage->segment_kind == N00B_ABI_STORAGE_READ_ONLY
                    && !storage->instance_shape_index
                    && !storage->tls_relocations.count
                    && core_object_section_find(view->object,
                        view->object_bytes, view->format,
                        view->format == CORE_TARGET_OBJECT_MACH_O
                            ? CORE_STRING_STATIC("__TEXT") : (core_string_t){},
                        pool_name, &pool_section)
                    && pool_section.native_index == local_section
                    && storage->byte_offset <= pool_section.size
                    && storage->byte_length <= pool_section.size
                        - storage->byte_offset;
                if (valid) {
                    size_t before = n00bb_abi_name_pool_size(
                        string_storage->pool);
                    size_t offset = 0;
                    n00b_abi_string_v1 ignored;
                    valid = n00bb_abi_name_pool_value_tagged(
                        string_storage->pool,
                        core_string_borrow((const char *)pool_section.bytes
                            + storage->byte_offset, storage->byte_length),
                        N00B_ABI_STRING_UTF8, N00B_ABI_STRING_LONG,
                        &ignored, &offset)
                        && n00bb_abi_name_pool_size(string_storage->pool)
                            == before;
                    if (valid) {
                        changes[change_count++] = (abi_native_rebase_t){
                            .row = row,
                            .kind = N00B_ABI_TABLE_STORAGE,
                            .section_index =
                                string_storage->final_section_index,
                            .storage_offset = offset,
                        };
                        continue;
                    }
                    reason = CORE_STRING_STATIC(
                        "string storage has no merged pool entry");
                }
            }
            valid = valid && match && match->final_section_index;
            if (!valid) {
                if (failure) *failure = (n00bb_abi_native_rebase_failure_t){
                    .table_kind = table->kind,
                    .row_index = row_index,
                    .input = origin->input,
                    .local_section = local_section,
                    .reason = reason,
                };
                break;
            }
            abi_native_rebase_t change = {
                .row = row, .kind = table->kind,
                .section_index = match->final_section_index,
            };
            uint64_t *storage_offset = rebased_offset(table->kind, row);
            if (storage_offset) {
                uint64_t offset = *storage_offset;
                valid = match->base_offset <= UINT64_MAX - offset;
                if (valid) change.storage_offset = offset + match->base_offset;
            }
            else {
                int64_t offset =
                    ((n00b_abi_native_binding_v1 *)row)->offset;
                valid = match->base_offset <= INT64_MAX
                    && offset <= INT64_MAX - (int64_t)match->base_offset;
                if (valid) change.binding_offset = offset
                    + (int64_t)match->base_offset;
            }
            if (valid) changes[change_count++] = change;
            else if (failure) *failure = (n00bb_abi_native_rebase_failure_t){
                .table_kind = table->kind,
                .row_index = row_index,
                .input = origin->input,
                .local_section = local_section,
                .reason = CORE_STRING_STATIC("native section offset overflow"),
            };
        }
    }
    if (valid) {
        for (size_t i = 0; i < change_count; i++) {
            const abi_native_rebase_t *change = &changes[i];
            *rebased_section(change->kind, change->row) =
                change->section_index;
            uint64_t *offset = rebased_offset(change->kind, change->row);
            if (offset) *offset = change->storage_offset;
            else ((n00b_abi_native_binding_v1 *)change->row)->offset =
                change->binding_offset;
        }
    }
    core_free(changes);
    return valid;
}

bool
n00bb_abi_add_native_bindings(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *symbols)
{
    if (!selected || !selected->views || !merged || !sections || !symbols
        || symbols->symbol_count > UINT32_MAX
        || (symbols->symbol_count && !symbols->symbols)
        || (sections->map_count && (!sections->sections || !sections->maps)))
        return false;
    for (size_t i = 0; i < symbols->symbol_count; i++)
        if (!symbols->symbols[i].name.data) return false;
    const n00bb_abi_merged_table_t *table = n00bb_abi_merged_table(
        merged, N00B_ABI_TABLE_NATIVE_BINDING);
    if (!table || table->record_bytes != sizeof(n00b_abi_native_binding_v1)
        || (table->record_count && (!table->records || !table->origins)))
        return false;
    size_t new_capacity = table->record_count
        ? table->record_count - 1 : 0;
    if (new_capacity > SIZE_MAX - symbols->symbol_count
        || new_capacity > UINT32_MAX - symbols->symbol_count) return false;
    new_capacity += symbols->symbol_count;
    core_object_emit_symbol_t *updated = core_alloc_array(
        core_object_emit_symbol_t, new_capacity);
    if (symbols->symbol_count)
        memcpy(updated, symbols->symbols,
               symbols->symbol_count * sizeof(*updated));
    size_t count = symbols->symbol_count;
    bool valid = true;
    for (uint32_t i = 1; valid && i < table->record_count; i++) {
        const n00bb_abi_row_origin_t *origin = &table->origins[i];
        const n00b_abi_native_binding_v1 *binding =
            (const void *)(table->records + (size_t)i * table->record_bytes);
        if (binding->kind == N00B_ABI_NATIVE_EXTERNAL_NAME) continue;
        if (!origin->present || origin->input >= selected->count
            || !origin->local
            || binding->kind != N00B_ABI_NATIVE_SECTION_OFFSET
            || binding->flags || binding->offset < 0) {
            valid = false;
            break;
        }
        const n00bb_abi_object_view_t *view =
            &selected->views[origin->input];
        const n00b_abi_native_binding_v1 *source = n00bb_abi_object_row(
            view, N00B_ABI_TABLE_NATIVE_BINDING, origin->local);
        n00bb_abi_string_view_t name;
        valid = source && n00bb_abi_object_string(view,
            &source->native_name, &name) && name.length
            && name.length < SIZE_MAX
            && !memchr(name.bytes, 0, name.length);
        if (!valid) break;
        uint32_t ordinal = 0;
        for (size_t j = 0; j < sections->count; j++) {
            if (sections->final_indices[j] != binding->section_index)
                continue;
            if (ordinal || j > UINT32_MAX - 2
                || (uint64_t)binding->offset > sections->sections[j].size) {
                valid = false;
                break;
            }
            ordinal = (uint32_t)(j + 2);
        }
        if (!valid || !ordinal) {
            valid = false;
            break;
        }
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (!core_string_eq(updated[j].name, core_string_borrow(
                    (const char *)name.bytes, name.length)))
                continue;
            if (updated[j].section_index
                && (updated[j].section_index != ordinal
                    || updated[j].section_offset
                        != (uint64_t)binding->offset)) {
                valid = false;
                break;
            }
            updated[j].section_index = ordinal;
            updated[j].section_offset = (uint64_t)binding->offset;
            updated[j].exported = true;
            found = true;
        }
        if (!valid || found) continue;
        updated[count++] = (core_object_emit_symbol_t){
            .name = copy_native_name(name.bytes, name.length),
            .section_index = ordinal,
            .section_offset = (uint64_t)binding->offset,
            .exported = true,
        };
    }
    if (!valid) {
        for (size_t i = symbols->symbol_count; i < count; i++)
            core_free(updated[i].name.data);
        core_free(updated);
        return false;
    }
    core_free(symbols->symbols);
    symbols->symbols = updated;
    symbols->symbol_count = count;
    return true;
}

bool
n00bb_abi_merge_rows(const n00bb_abi_selected_objects_t *selected,
                     const n00bb_abi_index_plan_t *plan,
                     n00bb_abi_merged_rows_t *out)
{
    if (!selected || !selected->views || !selected->count || !plan || !out)
        return false;
    n00bb_abi_manifest_rows_t manifests = {0};
    if (!n00bb_abi_rebuild_manifests(selected, plan, &manifests))
        return false;

    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_merged_rows_t built = {
        .tables = core_alloc_array(n00bb_abi_merged_table_t, schema_count),
        .table_count = schema_count,
        .strings = n00bb_abi_name_pool_new(),
    };
    n00bb_abi_row_origin_t **origins = core_alloc_array(
        n00bb_abi_row_origin_t *, schema_count);
    size_t max_width = 0;
    uint32_t base_u32_count = 0;
    uint32_t import_count = 0;
    bool valid = schemas && schema_count && built.strings
        && n00bb_abi_index_plan_count(plan, N00B_ABI_TABLE_U32_STREAM,
                                      &base_u32_count)
        && module_import_count(selected, &import_count)
        && import_count <= UINT32_MAX - manifests.u32_count;
    for (size_t i = 0; valid && i < schema_count; i++) {
        const n00bb_abi_table_schema_t *schema = &schemas[i];
        uint32_t count = 0;
        valid = schema->record_bytes
            && n00bb_abi_index_plan_count(plan, schema->kind, &count);
        if (!valid) break;
        if ((schema->kind == N00B_ABI_TABLE_PACKAGE
                && schema->record_bytes != sizeof(n00b_abi_package_v1))
            || (schema->kind == N00B_ABI_TABLE_SCC
                && schema->record_bytes != sizeof(n00b_abi_scc_v1))
            || (schema->kind == N00B_ABI_TABLE_U32_STREAM
                && schema->record_bytes != sizeof(uint32_t))) {
            valid = false;
            break;
        }
        if (schema->kind == N00B_ABI_TABLE_U32_STREAM)
            count = manifests.u32_count + import_count;
        if ((schema->kind == N00B_ABI_TABLE_PACKAGE
                && count != manifests.package_count)
            || (schema->kind == N00B_ABI_TABLE_SCC
                && count != manifests.scc_count)
            || count > SIZE_MAX / schema->record_bytes) {
            valid = false;
            break;
        }
        built.tables[i] = (n00bb_abi_merged_table_t){
            .kind = schema->kind, .record_count = count,
            .record_bytes = schema->record_bytes,
            .records = count ? core_alloc_size(count, schema->record_bytes)
                             : nullptr,
        };
        origins[i] = count ? core_alloc_array(n00bb_abi_row_origin_t, count)
                           : nullptr;
        if (schema->record_bytes > max_width)
            max_width = schema->record_bytes;
        if (schema->kind == N00B_ABI_TABLE_PACKAGE)
            memcpy(built.tables[i].records, manifests.packages,
                   (size_t)count * schema->record_bytes);
        else if (schema->kind == N00B_ABI_TABLE_SCC)
            memcpy(built.tables[i].records, manifests.sccs,
                   (size_t)count * schema->record_bytes);
        else if (schema->kind == N00B_ABI_TABLE_U32_STREAM)
            memcpy(built.tables[i].records, manifests.u32_stream,
                   (size_t)manifests.u32_count * schema->record_bytes);
    }

    uint8_t *scratch = valid ? core_alloc_size(1, max_width) : nullptr;
    n00bb_abi_u32_stream_build_t stream = {
        .values = manifests.u32_stream, .kinds = manifests.u32_kinds,
        .count = base_u32_count,
    };
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        for (size_t ti = 0; valid && ti < schema_count; ti++) {
            n00bb_abi_merged_table_t *table = &built.tables[ti];
            const n00bb_abi_directory_entry_t *source_table =
                n00bb_abi_object_table(view, table->kind);
            if (!source_table) continue;
            valid = source_table->record_bytes == table->record_bytes;
            for (uint32_t local = 0; valid
                 && local < source_table->record_count; local++) {
                uint32_t final;
                valid = n00bb_abi_index_plan_map(plan, input,
                    table->kind, local, &final)
                    && final < table->record_count;
                if (!valid) break;
                n00bb_abi_row_origin_t *origin = &origins[ti][final];
                uint8_t *destination = table->records
                    + (size_t)final * table->record_bytes;
                if (table->kind == N00B_ABI_TABLE_U32_STREAM) {
                    valid = final < base_u32_count;
                    if (!valid) break;
                    if (!origin->present)
                        *origin = (n00bb_abi_row_origin_t){input, local, true};
                    if (!manifests.u32_kinds[final])
                        manifests.u32_stream[final] = core_le_load_u32(
                            n00bb_abi_object_row(view,
                                N00B_ABI_TABLE_U32_STREAM, local));
                    continue;
                }
                if (table->kind == N00B_ABI_TABLE_PACKAGE
                    || table->kind == N00B_ABI_TABLE_SCC) {
                    if (!origin->present)
                        *origin = (n00bb_abi_row_origin_t){input, local, true};
                    continue;
                }
                if (link_kind_is_stream(table->kind)) {
                    valid = !origin->present;
                    if (!valid) break;
                    memcpy(destination, n00bb_abi_object_row(view,
                        table->kind, local), table->record_bytes);
                }
                else {
                    valid = n00bb_abi_remap_row(selected, plan, input,
                        table->kind, local, scratch, table->record_bytes,
                        &stream);
                    if (!valid) break;
                    bool replace = !origin->present;
                    if (table->kind == N00B_ABI_TABLE_TYPE
                        && origin->present) {
                        const n00b_abi_type_v1 *old = (const void *)destination;
                        const n00b_abi_type_v1 *next = (const void *)scratch;
                        if (old->owner_module_index
                            && next->owner_module_index
                            && old->owner_module_index
                                != next->owner_module_index) {
                            valid = false;
                            break;
                        }
                        replace = (!old->owner_module_index
                            && next->owner_module_index)
                            || (!old->layout_index && next->layout_index
                                && (!old->tracing_index
                                    || next->tracing_index)
                                && (!old->call_signature_index
                                    || next->call_signature_index)
                                && (!old->fields_or_arms.count
                                    || next->fields_or_arms.count)
                                && (!old->components.count
                                    || next->components.count))
                            || (old->kind == N00B_ABI_TYPE_OPTION
                                && !old->tracing_index
                                && next->tracing_index)
                            || (old->kind == N00B_ABI_TYPE_VARIABLE
                                && old->typevar_import_requirement_index
                                && next->typevar_owner_scope_index);
                    }
                    if (replace) memcpy(destination, scratch,
                                        table->record_bytes);
                    else continue;
                }
                *origin = (n00bb_abi_row_origin_t){input, local, true};
            }
        }
    }

    size_t type_table = schema_count;
    for (size_t ti = 0; ti < schema_count; ti++)
        if (built.tables[ti].kind == N00B_ABI_TABLE_TYPE) {
            type_table = ti;
            break;
        }
    if (type_table == schema_count) valid = false;
    for (uint32_t i = 1; valid
         && i < built.tables[type_table].record_count; i++) {
        n00bb_abi_row_origin_t source = origins[type_table][i];
        const n00bb_abi_object_view_t *view = source.present
            ? &selected->views[source.input] : nullptr;
        const n00b_abi_type_v1 *type = view
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE,
                                   source.local) : nullptr;
        if (!type) { valid = false; break; }
        const uint32_t kinds[] = {
            N00B_ABI_TABLE_LAYOUT, N00B_ABI_TABLE_TRACE,
        };
        const uint32_t local_indices[] = {
            type->layout_index, type->tracing_index,
        };
        for (size_t k = 0; valid && k < sizeof(kinds) / sizeof(*kinds); k++) {
            if (!local_indices[k]) continue;
            size_t ti = 0;
            while (ti < schema_count && built.tables[ti].kind != kinds[k])
                ti++;
            uint32_t final = 0;
            valid = ti < schema_count
                && n00bb_abi_index_plan_map(plan, source.input,
                    kinds[k], local_indices[k], &final)
                && final < built.tables[ti].record_count
                && n00bb_abi_remap_row(selected, plan, source.input,
                    kinds[k], local_indices[k], scratch,
                    built.tables[ti].record_bytes, &stream);
            if (!valid) break;
            memcpy(built.tables[ti].records
                + (size_t)final * built.tables[ti].record_bytes,
                scratch, built.tables[ti].record_bytes);
            origins[ti][final] = (n00bb_abi_row_origin_t){
                source.input, local_indices[k], true,
            };
        }
    }

    for (size_t ti = 0; valid && ti < schema_count; ti++) {
        const n00bb_abi_merged_table_t *table = &built.tables[ti];
        if (table->kind != N00B_ABI_TABLE_U64_STREAM
            && table->kind != N00B_ABI_TABLE_BYTE_STREAM
            && table->kind != N00B_ABI_TABLE_U32_STREAM) continue;
        uint32_t count = table->kind == N00B_ABI_TABLE_U32_STREAM
            ? base_u32_count : table->record_count;
        for (uint32_t i = 0; valid && i < count; i++)
            valid = origins[ti][i].present;
    }

    const n00bb_abi_merged_table_t *u32_table =
        n00bb_abi_merged_table(&built, N00B_ABI_TABLE_U32_STREAM);
    valid = valid && u32_table && u32_table->record_bytes == sizeof(uint32_t)
        && u32_table->record_count == manifests.u32_count + import_count;
    if (valid) memcpy(u32_table->records, manifests.u32_stream,
                      (size_t)manifests.u32_count * sizeof(uint32_t));
    if (valid) valid = resolve_module_imports(selected, plan, &built,
                                               manifests.u32_count);

    const n00bb_abi_merged_table_t *final_types = n00bb_abi_merged_table(
        &built, N00B_ABI_TABLE_TYPE);
    for (uint32_t i = 0; valid && final_types
         && i < final_types->record_count; i++) {
        const n00b_abi_type_v1 *type = (const void *)(final_types->records
            + (size_t)i * final_types->record_bytes);
        valid = n00bb_abi_type_variable_final_valid(type);
    }
    for (size_t ti = 0; valid && ti < schema_count; ti++) {
        n00bb_abi_merged_table_t *table = &built.tables[ti];
        if (link_kind_is_stream(table->kind)
            || table->kind == N00B_ABI_TABLE_REQUIREMENT
            || table->kind == N00B_ABI_TABLE_INDEX_PATCH) continue;
        for (uint32_t final = 0; valid && final < table->record_count;
             final++) {
            n00bb_abi_row_origin_t origin = origins[ti][final];
            valid = origin.present;
            if (!valid || (final == 0 && table->kind != N00B_ABI_TABLE_TYPE))
                continue;
            valid = n00bb_abi_repool_row_strings(
                &selected->views[origin.input], table->kind, origin.local,
                final, table->records + (size_t)final * table->record_bytes,
                table->record_bytes, built.strings);
        }
    }
    if (valid) valid = complete_merged_traces(&built);
    core_free(scratch);
    for (size_t i = 0; i < schema_count; i++) {
        if (rebased_row_bytes(built.tables[i].kind))
            built.tables[i].origins = origins[i];
        else core_free(origins[i]);
    }
    core_free(origins);
    n00bb_abi_manifest_rows_release(&manifests);
    if (!valid) {
        n00bb_abi_merged_rows_release(&built);
        return false;
    }
    *out = built;
    return true;
}
