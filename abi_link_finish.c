#include "abi_link.h"

#include <stdlib.h>
#include <string.h>

#include "abi_emit.h"
#include "abi_index_patch.h"
#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/string.h"

static bool
same_key(const n00bb_abi_image_key_t *left,
         const n00bb_abi_image_key_t *right)
{
    return left->target_id == right->target_id
        && left->abi_profile_id == right->abi_profile_id
        && left->feature_bits == right->feature_bits
        && left->cpu_profile == right->cpu_profile
        && left->data_layout_profile == right->data_layout_profile;
}

typedef struct {
    uint32_t child;
    uint32_t parent;
    size_t input;
} specialization_edge_t;

static int
compare_specialization_edges(const void *left, const void *right)
{
    const specialization_edge_t *a = left, *b = right;
    if (a->child != b->child) return a->child < b->child ? -1 : 1;
    if (a->parent != b->parent) return a->parent < b->parent ? -1 : 1;
    if (a->input != b->input) return a->input < b->input ? -1 : 1;
    return 0;
}

bool
n00bb_abi_rebuild_specialization_links(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan, n00bb_abi_merged_rows_t *merged)
{
    if (!selected || !selected->views || !plan || !merged
        || !merged->tables) return false;
    n00bb_abi_merged_table_t *types = nullptr, *links = nullptr;
    for (size_t i = 0; i < merged->table_count; i++) {
        n00bb_abi_merged_table_t *table = &merged->tables[i];
        if (table->kind == N00B_ABI_TABLE_TYPE) types = table;
        if (table->kind == N00B_ABI_TABLE_SPECIALIZATION_LINK) links = table;
    }
    if (!types || !types->records || !types->record_count
        || types->record_bytes != sizeof(n00b_abi_type_v1)
        || (links && links->record_bytes != sizeof(uint32_t))) return false;
    size_t capacity = 0;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_directory_entry_t *source = n00bb_abi_object_table(
            &selected->views[input], N00B_ABI_TABLE_SPECIALIZATION_LINK);
        if (!source) continue;
        if (!source->record_count || source->record_count - 1
            > SIZE_MAX - capacity) return false;
        capacity += source->record_count - 1;
    }
    if (capacity >= UINT32_MAX
        || capacity > SIZE_MAX / sizeof(specialization_edge_t)) return false;
    specialization_edge_t *edges = core_alloc_array(specialization_edge_t,
                                                     capacity);
    size_t edge_count = 0;
    bool valid = true;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *source_types =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_TYPE);
        const n00bb_abi_directory_entry_t *source_links =
            n00bb_abi_object_table(view,
                N00B_ABI_TABLE_SPECIALIZATION_LINK);
        if (!source_types) { valid = false; break; }
        uint8_t *owned = source_links
            ? core_alloc_array(uint8_t, source_links->record_count) : nullptr;
        if (owned) memset(owned, 0, source_links->record_count);
        for (uint32_t local = 0; valid
             && local < source_types->record_count; local++) {
            const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_TYPE, local);
            uint32_t child;
            if (!type || !n00bb_abi_index_plan_map(plan, input,
                    N00B_ABI_TABLE_TYPE, local, &child)
                || child >= types->record_count) {
                valid = false;
                break;
            }
            uint32_t start = type->specialization_start_ix;
            uint32_t end = type->specialization_end_ix;
            if ((!start && end) || (start && (!source_links || !owned
                    || end <= start || end > source_links->record_count))) {
                valid = false;
                break;
            }
            for (uint32_t j = start; valid && j < end; j++) {
                const uint32_t *source_parent = n00bb_abi_object_row(view,
                    N00B_ABI_TABLE_SPECIALIZATION_LINK, j);
                uint32_t parent;
                if (owned[j] || !source_parent
                    || !n00bb_abi_index_plan_map(plan, input,
                        N00B_ABI_TABLE_TYPE, *source_parent, &parent)
                    || parent >= types->record_count || child == parent
                    || edge_count >= capacity) {
                    valid = false;
                    break;
                }
                owned[j] = 1;
                edges[edge_count++] = (specialization_edge_t){
                    .child = child, .parent = parent, .input = input,
                };
            }
        }
        if (source_links)
            for (uint32_t i = 1; valid
                 && i < source_links->record_count; i++)
                if (!owned || !owned[i]) valid = false;
        core_free(owned);
    }
    if (valid && edge_count) qsort(edges, edge_count, sizeof(*edges),
                                   compare_specialization_edges);
    size_t unique = 0;
    for (size_t i = 0; valid && i < edge_count;) {
        size_t end = i + 1;
        while (end < edge_count && edges[end].child == edges[i].child
            && edges[end].parent == edges[i].parent) {
            if (edges[end].input == edges[end - 1].input) valid = false;
            end++;
        }
        edges[unique++] = edges[i];
        i = end;
    }
    uint32_t *rebuilt = valid
        ? core_alloc_array(uint32_t, unique + 1) : nullptr;
    n00b_abi_range_v1 *spans = valid
        ? core_alloc_array(n00b_abi_range_v1, types->record_count) : nullptr;
    uint32_t *incoming = valid
        ? core_alloc_array(uint32_t, types->record_count) : nullptr;
    uint32_t *queue = valid
        ? core_alloc_array(uint32_t, types->record_count) : nullptr;
    valid = valid && rebuilt && spans && incoming && queue
        && (!unique || links);
    if (valid) {
        memset(spans, 0, (size_t)types->record_count * sizeof(*spans));
        memset(incoming, 0, (size_t)types->record_count * sizeof(*incoming));
        rebuilt[0] = 0;
        for (size_t i = 0; i < unique; i++) {
            specialization_edge_t edge = edges[i];
            n00b_abi_range_v1 *span = &spans[edge.child];
            if (!span->count) span->first = (uint32_t)i + 1;
            span->count++;
            rebuilt[i + 1] = edge.parent;
            incoming[edge.parent]++;
        }
        size_t head = 0, tail = 0;
        for (uint32_t i = 0; i < types->record_count; i++)
            if (!incoming[i]) queue[tail++] = i;
        while (head < tail) {
            uint32_t child = queue[head++];
            n00b_abi_range_v1 span = spans[child];
            for (uint32_t j = 0; j < span.count; j++) {
                uint32_t parent = rebuilt[span.first + j];
                if (!--incoming[parent]) queue[tail++] = parent;
            }
        }
        valid = tail == types->record_count;
    }
    if (valid) {
        for (uint32_t i = 0; i < types->record_count; i++) {
            n00b_abi_type_v1 *type = (void *)(types->records
                + (size_t)i * types->record_bytes);
            type->specialization_start_ix = spans[i].first;
            type->specialization_end_ix = spans[i].count
                ? spans[i].first + spans[i].count : 0;
        }
        if (links) {
            core_free(links->records);
            links->records = (uint8_t *)rebuilt;
            links->record_count = (uint32_t)unique + 1;
            rebuilt = nullptr;
        }
    }
    core_free(rebuilt);
    core_free(spans);
    core_free(incoming);
    core_free(queue);
    core_free(edges);
    return valid;
}

static bool
link_only_inputs_resolved(const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_image_key_t *key,
                          uint64_t *capabilities)
{
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        if (!view->directory || !same_key(&view->header.key, key))
            return false;
        *capabilities |= view->header.artifact_capabilities;
        const n00bb_abi_directory_entry_t *requirements =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_REQUIREMENT);
        if (requirements) {
            for (uint32_t i = 1; i < requirements->record_count; i++) {
                const n00b_abi_requirement_v1 *row =
                    n00bb_abi_object_row(view,
                        N00B_ABI_TABLE_REQUIREMENT, i);
                if (!row || (row->symbol_kind != N00B_ABI_SYMBOL_MODULE
                        && row->symbol_kind != N00B_ABI_SYMBOL_TYPE
                        && row->symbol_kind != N00B_ABI_SYMBOL_FUNCTION
                        && row->symbol_kind != N00B_ABI_SYMBOL_VARIABLE))
                    return false;
            }
        }
        const uint32_t pending[] = {
            N00B_ABI_TABLE_DISPATCH,
            N00B_ABI_TABLE_DISPATCH_ENTRY,
            N00B_ABI_TABLE_NAME_SLOT,
        };
        for (size_t i = 0; i < sizeof(pending) / sizeof(*pending); i++) {
            const n00bb_abi_directory_entry_t *table =
                n00bb_abi_object_table(view, pending[i]);
            if (table && table->record_count > 1) return false;
        }
    }
    return true;
}

static bool
typed_requirements_resolved(const n00bb_abi_selected_objects_t *selected,
                            const n00bb_abi_index_plan_t *plan)
{
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *requester = &selected->views[input];
        const n00bb_abi_directory_entry_t *requirements =
            n00bb_abi_object_table(requester, N00B_ABI_TABLE_REQUIREMENT);
        if (!requirements) continue;
        const n00b_abi_module_v1 *requesting_module =
            n00bb_abi_object_row(requester, N00B_ABI_TABLE_MODULE, 1);
        uint32_t requesting_package;
        if (!requesting_module
            || !n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_PACKAGE, requesting_module->package_index,
                &requesting_package)) return false;
        for (uint32_t i = 1; i < requirements->record_count; i++) {
            const n00b_abi_requirement_v1 *request =
                n00bb_abi_object_row(requester,
                    N00B_ABI_TABLE_REQUIREMENT, i);
            if (!request) return false;
            if (request->symbol_kind == N00B_ABI_SYMBOL_MODULE) continue;
            if ((request->symbol_kind != N00B_ABI_SYMBOL_TYPE
                    && request->symbol_kind != N00B_ABI_SYMBOL_FUNCTION
                    && request->symbol_kind != N00B_ABI_SYMBOL_VARIABLE)
                || request->requesting_module_index != 1
                || !request->required_phase_mask
                || (request->required_phase_mask
                    & ~(N00B_ABI_PHASE_COMPTIME | N00B_ABI_PHASE_RUNTIME))
                || request->contract_type_index
                    == N00B_ABI_V1_NO_TYPE_INDEX
                || request->reserved_zero
                || (request->symbol_kind == N00B_ABI_SYMBOL_FUNCTION
                    && request->required_layout_index)) return false;
            n00bb_abi_string_view_t wanted;
            if (!n00bb_abi_object_string(requester, &request->full_path,
                    &wanted)) return false;
            uint32_t contract_index;
            if (!n00bb_abi_index_plan_map(plan, input,
                    N00B_ABI_TABLE_TYPE, request->contract_type_index,
                    &contract_index)) return false;
            const n00b_abi_type_v1 *contract = n00bb_abi_object_row(
                requester, N00B_ABI_TABLE_TYPE,
                request->contract_type_index);
            if (!contract || !contract->typehash) return false;
            size_t matches = 0;
            for (size_t provider_input = 0;
                 provider_input < selected->count; provider_input++) {
                const n00bb_abi_object_view_t *provider =
                    &selected->views[provider_input];
                const n00b_abi_module_v1 *module = n00bb_abi_object_row(
                    provider, N00B_ABI_TABLE_MODULE, 1);
                n00bb_abi_string_view_t module_name;
                if (!module || !n00bb_abi_object_string(provider,
                        &module->full_path, &module_name)) return false;
                uint32_t provider_package;
                if (!n00bb_abi_index_plan_map(plan, provider_input,
                        N00B_ABI_TABLE_PACKAGE, module->package_index,
                        &provider_package)) return false;
                if (wanted.length <= module_name.length + 1
                    || memcmp(wanted.bytes, module_name.bytes,
                        module_name.length)
                    || wanted.bytes[module_name.length] != '.') continue;
                const n00bb_abi_directory_entry_t *symbols =
                    n00bb_abi_object_table(provider,
                        N00B_ABI_TABLE_SYMBOL);
                if (!symbols) return false;
                for (uint32_t j = 1; j < symbols->record_count; j++) {
                    const n00b_abi_symbol_v1 *symbol =
                        n00bb_abi_object_row(provider,
                            N00B_ABI_TABLE_SYMBOL, j);
                    if (!symbol || symbol->kind != request->symbol_kind
                        || symbol->module_index != 1) continue;
                    const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(
                        provider, N00B_ABI_TABLE_SCOPE, symbol->scope_index);
                    if (!scope || scope->parent_scope_index
                        || scope->module_index != 1) continue;
                    if ((symbol->visibility == N00B_ABI_VISIBILITY_PRIVATE
                         && provider_input != input)
                        || (symbol->visibility == N00B_ABI_VISIBILITY_PACKAGE
                            && provider_package != requesting_package))
                        continue;
                    n00bb_abi_string_view_t name;
                    if (!n00bb_abi_object_string(provider, &symbol->name,
                            &name)) return false;
                    size_t suffix = module_name.length + 1;
                    if (name.length != wanted.length - suffix
                        || memcmp(name.bytes, wanted.bytes + suffix,
                            name.length)) continue;
                    const n00b_abi_type_v1 *definition =
                        n00bb_abi_object_row(provider,
                            N00B_ABI_TABLE_TYPE, symbol->type_index);
                    uint32_t definition_index;
                    if (!definition
                        || (request->symbol_kind == N00B_ABI_SYMBOL_TYPE
                            && definition->owner_module_index != 1)
                        || (request->symbol_kind == N00B_ABI_SYMBOL_FUNCTION
                            && definition->kind != N00B_ABI_TYPE_FUNCTION))
                        return false;
                    if (definition->kind != contract->kind
                        || definition->typehash != contract->typehash
                        || (symbol->phase_mask
                            & request->required_phase_mask)
                            != request->required_phase_mask
                        || (provider->header.artifact_capabilities
                            & request->required_capabilities)
                            != request->required_capabilities)
                        continue;
                    if (!n00bb_abi_index_plan_map(plan, provider_input,
                            N00B_ABI_TABLE_TYPE, symbol->type_index,
                            &definition_index)) return false;
                    if (definition_index != contract_index) continue;
                    if (request->symbol_kind == N00B_ABI_SYMBOL_FUNCTION) {
                        const n00bb_abi_directory_entry_t *functions =
                            n00bb_abi_object_table(provider,
                                N00B_ABI_TABLE_FUNCTION);
                        bool defined = false;
                        if (!functions) return false;
                        for (uint32_t k = 1; k < functions->record_count; k++) {
                            const n00b_abi_function_v1 *function =
                                n00bb_abi_object_row(provider,
                                    N00B_ABI_TABLE_FUNCTION, k);
                            if (function && function->symbol_index == j)
                                defined = true;
                        }
                        if (!defined) continue;
                    }
                    if (request->symbol_kind == N00B_ABI_SYMBOL_VARIABLE
                        && (request->required_phase_mask
                            & N00B_ABI_PHASE_RUNTIME)) {
                        const n00bb_abi_directory_entry_t *storages =
                            n00bb_abi_object_table(provider,
                                N00B_ABI_TABLE_STORAGE);
                        bool defined = false;
                        if (!storages) return false;
                        for (uint32_t k = 1; k < storages->record_count; k++) {
                            const n00b_abi_storage_v1 *storage =
                                n00bb_abi_object_row(provider,
                                    N00B_ABI_TABLE_STORAGE, k);
                            if (storage && storage->symbol_index == j
                                && storage->module_index == 1
                                && storage->type_index == symbol->type_index)
                                defined = true;
                        }
                        if (!defined) continue;
                    }
                    if (request->required_layout_index) {
                        if (request->required_layout_index
                                != contract->layout_index
                            || !definition->layout_index)
                            return false;
                    }
                    matches++;
                }
            }
            if (matches != 1) return false;
        }
    }
    return true;
}

typedef struct {
    n00bb_abi_string_view_t module;
    n00bb_abi_string_view_t symbol;
    bool present;
} name_candidate_t;

static bool
same_name(n00bb_abi_string_view_t left, n00bb_abi_string_view_t right)
{
    return left.length == right.length
        && (!left.length || !memcmp(left.bytes, right.bytes, left.length));
}

static bool
rebuild_name_slots(const n00bb_abi_selected_objects_t *selected,
                   const n00bb_abi_index_plan_t *plan,
                   n00bb_abi_merged_rows_t *merged)
{
    const n00bb_abi_merged_table_t *symbols = n00bb_abi_merged_table(
        merged, N00B_ABI_TABLE_SYMBOL);
    n00bb_abi_merged_table_t *names = nullptr;
    for (size_t i = 0; i < merged->table_count; i++)
        if (merged->tables[i].kind == N00B_ABI_TABLE_NAME_SLOT)
            names = &merged->tables[i];
    if (!symbols || !names || names->record_count > 1) return false;
    name_candidate_t *candidates = core_alloc_array(name_candidate_t,
                                                     symbols->record_count);
    uint32_t linkable = 0;
    bool valid = true;
    for (size_t input = 0; valid && input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, 1);
        n00bb_abi_string_view_t module_path;
        valid = module && n00bb_abi_object_string(view,
            &module->full_path, &module_path);
        const n00bb_abi_directory_entry_t *source_symbols =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_SYMBOL);
        if (!valid || !source_symbols) continue;
        for (uint32_t local = 1; valid
             && local < source_symbols->record_count; local++) {
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_SYMBOL, local);
            valid = symbol != nullptr;
            if (!valid) break;
            if (symbol->kind != N00B_ABI_SYMBOL_TYPE
                && symbol->kind != N00B_ABI_SYMBOL_FUNCTION
                && symbol->kind != N00B_ABI_SYMBOL_VARIABLE) continue;
            const n00b_abi_scope_v1 *scope = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SCOPE, symbol->scope_index);
            valid = scope && scope->module_index == 1
                && symbol->module_index == 1;
            if (!valid || scope->parent_scope_index) continue;
            uint32_t final;
            n00bb_abi_string_view_t symbol_name;
            valid = n00bb_abi_index_plan_map(plan, input,
                    N00B_ABI_TABLE_SYMBOL, local, &final)
                && final && final < symbols->record_count
                && n00bb_abi_object_string(view, &symbol->name,
                                            &symbol_name)
                && symbol_name.length;
            if (!valid) break;
            name_candidate_t *candidate = &candidates[final];
            if (candidate->present) {
                valid = same_name(candidate->module, module_path)
                    && same_name(candidate->symbol, symbol_name);
                continue;
            }
            *candidate = (name_candidate_t){module_path, symbol_name, true};
            linkable++;
        }
    }
    uint32_t capacity = 0;
    if (valid) valid = linkable <= (UINT32_MAX - 2) / 2;
    if (valid) capacity = 2 * linkable + 1;
    uint32_t row_count = capacity + 1;
    n00b_abi_name_slot_v1 *slots = valid
        ? core_alloc_array(n00b_abi_name_slot_v1, row_count) : nullptr;
    core_string_t *paths = valid
        ? core_alloc_array(core_string_t, row_count) : nullptr;
    for (uint32_t symbol_index = 1; valid
         && symbol_index < symbols->record_count; symbol_index++) {
        const name_candidate_t *candidate = &candidates[symbol_index];
        if (!candidate->present) continue;
        size_t module_bytes = candidate->module.length;
        size_t symbol_bytes = candidate->symbol.length;
        valid = module_bytes <= SIZE_MAX - 1 - symbol_bytes;
        if (!valid) break;
        size_t length = module_bytes + 1 + symbol_bytes;
        char *path = core_alloc_size(1, length);
        memcpy(path, candidate->module.bytes, module_bytes);
        path[module_bytes] = '.';
        memcpy(path + module_bytes + 1, candidate->symbol.bytes,
               symbol_bytes);
        core_string_t full_path = core_string_borrow(path, length);
        uint64_t hash = core_xxh3_64bits_raw(path, length);
        uint32_t slot = 1 + (uint32_t)(hash % capacity);
        while (slots[slot].symbol_index) {
            if (core_string_eq(paths[slot], full_path)) {
                if (slots[slot].symbol_index != symbol_index)
                    slots[slot].ambiguous = 1;
                break;
            }
            slot = slot == capacity ? 1 : slot + 1;
        }
        if (valid && !slots[slot].symbol_index) {
            slots[slot] = (n00b_abi_name_slot_v1){
                .name_hash = hash, .symbol_index = symbol_index,
            };
            paths[slot] = full_path;
        }
        else core_free(path);
    }
    if (valid) {
        core_free(names->records);
        names->records = (uint8_t *)slots;
        names->record_count = row_count;
        slots = nullptr;
    }
    for (uint32_t i = 1; paths && i < row_count; i++)
        core_free(paths[i].data);
    core_free(paths);
    core_free(slots);
    core_free(candidates);
    return valid;
}

static core_index_remap_t *
specialization_patch_remap(const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    const n00bb_abi_merged_rows_t *merged, size_t input)
{
    const n00bb_abi_object_view_t *view = &selected->views[input];
    const n00bb_abi_directory_entry_t *source_types =
        n00bb_abi_object_table(view, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *source_links =
        n00bb_abi_object_table(view, N00B_ABI_TABLE_SPECIALIZATION_LINK);
    const n00bb_abi_merged_table_t *final_links = n00bb_abi_merged_table(
        merged, N00B_ABI_TABLE_SPECIALIZATION_LINK);
    if (!source_types || !source_links || !source_links->record_count
        || !final_links || !final_links->record_count) return nullptr;
    core_index_remap_t *remap = core_index_remap_new(
        source_links->record_count, final_links->record_count - 1);
    bool valid = remap && core_index_remap_set(remap, 0, 0);
    for (uint32_t local = 0; valid
         && local < source_types->record_count; local++) {
        const n00b_abi_type_v1 *source = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, local);
        uint32_t child;
        if (!source || !n00bb_abi_index_plan_map(plan, input,
                N00B_ABI_TABLE_TYPE, local, &child)) {
            valid = false;
            break;
        }
        const n00b_abi_type_v1 *target = n00bb_abi_merged_row(merged,
            N00B_ABI_TABLE_TYPE, child);
        if (!target) { valid = false; break; }
        for (uint32_t j = source->specialization_start_ix;
             valid && j < source->specialization_end_ix; j++) {
            const uint32_t *source_parent = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SPECIALIZATION_LINK, j);
            uint32_t parent, found = 0;
            if (!source_parent || !n00bb_abi_index_plan_map(plan, input,
                    N00B_ABI_TABLE_TYPE, *source_parent, &parent)) {
                valid = false;
                break;
            }
            for (uint32_t k = target->specialization_start_ix;
                 k < target->specialization_end_ix; k++) {
                const uint32_t *candidate = n00bb_abi_merged_row(merged,
                    N00B_ABI_TABLE_SPECIALIZATION_LINK, k);
                if (candidate && *candidate == parent) {
                    if (found) { valid = false; break; }
                    found = k;
                }
            }
            if (!valid || !found || !core_index_remap_set(remap, j, found))
                valid = false;
        }
    }
    for (uint32_t i = 1; valid && i < source_links->record_count; i++) {
        uint32_t mapped;
        valid = core_index_remap_get(remap, i, &mapped);
    }
    if (!valid) {
        core_index_remap_free(remap);
        return nullptr;
    }
    return remap;
}

bool
n00bb_abi_apply_link_patches(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    const n00bb_abi_merged_rows_t *merged,
    n00bb_abi_native_sections_t *sections)
{
    if (!selected || !selected->views || !plan || !merged
        || !merged->tables || !sections
        || (sections->count && (!sections->sections || !sections->maps)))
        return false;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00bb_abi_directory_entry_t *patch_table =
            n00bb_abi_object_table(view, N00B_ABI_TABLE_INDEX_PATCH);
        if (!patch_table || patch_table->record_count <= 1) continue;
        if (patch_table->record_bytes != N00BB_ABI_INDEX_PATCH_BYTES
            || patch_table->byte_length !=
                (uint64_t)patch_table->record_count
                    * N00BB_ABI_INDEX_PATCH_BYTES
            || patch_table->file_offset > view->metadata.size
            || patch_table->byte_length
                > view->metadata.size - patch_table->file_offset
            || !view->metadata.bytes) return false;
        n00bb_abi_patch_section_t *patch_sections = core_alloc_array(
            n00bb_abi_patch_section_t, sections->count);
        n00bb_abi_patch_table_t *patch_tables = core_alloc_array(
            n00bb_abi_patch_table_t, merged->table_count);
        core_index_remap_t *specialization_remap =
            n00bb_abi_object_table(view,
                N00B_ABI_TABLE_SPECIALIZATION_LINK)
            ? specialization_patch_remap(selected, plan, merged, input)
            : nullptr;
        if (n00bb_abi_object_table(view,
                N00B_ABI_TABLE_SPECIALIZATION_LINK)
            && !specialization_remap) {
            core_free(patch_sections);
            core_free(patch_tables);
            return false;
        }
        size_t patch_section_count = 0, patch_table_count = 0;
        for (size_t i = 0; i < sections->count; i++) {
            if (sections->maps[i].input != input) continue;
            patch_sections[patch_section_count++] =
                (n00bb_abi_patch_section_t){
                    .native_index = sections->maps[i].local_section_index,
                    .bytes = (uint8_t *)sections->sections[i].bytes,
                    .byte_length = sections->sections[i].size,
                };
        }
        for (size_t i = 0; i < merged->table_count; i++) {
            const n00bb_abi_merged_table_t *table = &merged->tables[i];
            if (table->kind == N00B_ABI_TABLE_U32_STREAM
                || table->kind == N00B_ABI_TABLE_U64_STREAM
                || table->kind == N00B_ABI_TABLE_BYTE_STREAM
                || table->kind == N00B_ABI_TABLE_REQUIREMENT
                || table->kind == N00B_ABI_TABLE_INDEX_PATCH) continue;
            const core_index_remap_t *remap =
                table->kind == N00B_ABI_TABLE_SPECIALIZATION_LINK
                ? specialization_remap
                : n00bb_abi_index_plan_remap(plan, input, table->kind);
            if (!remap) continue;
            patch_tables[patch_table_count++] =
                (n00bb_abi_patch_table_t){
                    .kind = table->kind,
                    .remap = remap,
                    .final_row_count = table->record_count,
                };
        }
        const uint8_t *patch_bytes = view->metadata.bytes
            + patch_table->file_offset + patch_table->record_bytes;
        size_t patch_byte_count = (size_t)(patch_table->record_count - 1)
            * patch_table->record_bytes;
        bool valid = n00bb_abi_index_patches_apply(patch_bytes,
            patch_byte_count, patch_sections, patch_section_count,
            patch_tables, patch_table_count, nullptr) ==
                N00BB_ABI_PATCH_OK;
        core_free(patch_sections);
        core_free(patch_tables);
        core_index_remap_free(specialization_remap);
        if (!valid) return false;
    }
    return true;
}

static bool
link_selected_objects(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    n00bb_abi_link_semantics_fn semantics, void *context,
    uint8_t **out_object, size_t *out_object_bytes)
{
    n00bb_abi_image_key_t key;
    uint64_t capabilities = 0;
    if (!selected || !selected->views || !selected->count || !target
        || !out_object || !out_object_bytes
        || !n00bb_abi_image_key_for_target(target, &key)
        || !link_only_inputs_resolved(selected, &key, &capabilities))
        return false;

    n00bb_abi_index_plan_t *plan =
        n00bb_abi_index_plan_build(selected);
    n00bb_abi_merged_rows_t merged = {0};
    n00bb_abi_native_sections_t sections = {0};
    n00bb_abi_native_relocations_t native = {0};
    n00bb_abi_packed_tables_t packed = {0};
    n00bb_abi_table_rows_t *rows = nullptr;
    n00bb_abi_directory_entry_t *entries = nullptr;
    core_object_emit_absolute64_t *pool_relocations = nullptr;
    core_object_emit_absolute64_t *relocations = nullptr;
    core_object_emit_section_t *extra_sections = nullptr;
    core_object_emit_symbol_t *symbols = nullptr;
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    bool valid = plan
        && typed_requirements_resolved(selected, plan)
        && n00bb_abi_merge_rows(selected, plan, &merged)
        && n00bb_abi_rebuild_specialization_links(selected, plan, &merged)
        && rebuild_name_slots(selected, plan, &merged)
        && n00bb_abi_collect_native_sections(selected, target, &sections)
        && n00bb_abi_apply_link_patches(selected, plan, &merged, &sections)
        && n00bb_abi_collect_native_relocations(selected, &sections,
                                                &native)
        && n00bb_abi_consolidate_native_strings(target, merged.strings,
                                                &sections, &native);
    uint32_t pending_pool_symbol = valid
        ? (uint32_t)native.symbol_count : 0;
    n00bb_abi_string_storage_rebase_t string_storage = {
        .selected = selected,
        .pool = merged.strings,
    };
    bool has_pool = valid && n00bb_abi_name_pool_size(merged.strings) != 0;
    if (valid && has_pool)
        valid = sections.count < UINT32_MAX - 2
            && core_object_emit_native_section_index(target,
                2 + sections.count, (uint32_t)(2 + sections.count),
                &string_storage.final_section_index);
    valid = valid
        && n00bb_abi_rebase_native_references(&merged, sections.maps,
            sections.count, has_pool ? &string_storage : nullptr)
        && n00bb_abi_add_native_bindings(selected, &merged, &sections,
                                         &native);
    if (valid && semantics)
        valid = semantics(selected, plan, &merged, context);
    if (valid)
        for (size_t i = 0; i < native.relocation_count; i++)
            if (native.relocations[i].symbol_index == pending_pool_symbol)
                native.relocations[i].symbol_index =
                    (uint32_t)native.symbol_count;

    size_t row_count = 0;
    if (valid) {
        rows = core_alloc_array(n00bb_abi_table_rows_t,
                                merged.table_count);
        for (size_t i = 0; i < merged.table_count; i++) {
            const n00bb_abi_merged_table_t *table = &merged.tables[i];
            if (!table->record_count
                || table->kind == N00B_ABI_TABLE_REQUIREMENT
                || table->kind == N00B_ABI_TABLE_INDEX_PATCH) continue;
            rows[row_count++] = (n00bb_abi_table_rows_t){
                .kind = table->kind,
                .records = table->records,
                .record_count = table->record_count,
            };
        }
        valid = n00bb_abi_pack_tables(target, capabilities, rows,
                                      row_count, &packed);
    }

    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_image_header_t header;
    if (valid) {
        entries = core_alloc_array(n00bb_abi_directory_entry_t,
                                   row_count);
        valid = n00bb_abi_container_read(packed.bytes,
            packed.byte_count, &key, schemas, schema_count, false,
            &header, entries, row_count);
    }
    size_t pool_relocation_count = 0;
    if (valid)
        valid = n00bb_abi_name_pool_relocations(merged.strings, entries,
            row_count, &pool_relocations, &pool_relocation_count);

    size_t pool_size = n00bb_abi_name_pool_size(merged.strings);
    if (valid) {
        valid = (has_pool || !pool_relocation_count)
            && sections.count < UINT32_MAX - 2
            && native.symbol_count < UINT32_MAX - (size_t)has_pool
            && native.relocation_count <= SIZE_MAX - pool_relocation_count;
    }
    if (valid) {
        size_t extra_count = sections.count + (size_t)has_pool;
        size_t symbol_count = native.symbol_count + (size_t)has_pool;
        size_t relocation_count = native.relocation_count
            + pool_relocation_count;
        extra_sections = core_alloc_array(core_object_emit_section_t,
                                          extra_count);
        symbols = core_alloc_array(core_object_emit_symbol_t,
                                   symbol_count);
        relocations = core_alloc_array(core_object_emit_absolute64_t,
                                       relocation_count);
        if (sections.count)
            memcpy(extra_sections, sections.sections,
                   sections.count * sizeof(*extra_sections));
        if (native.symbol_count)
            memcpy(symbols, native.symbols,
                   native.symbol_count * sizeof(*symbols));
        if (native.relocation_count)
            memcpy(relocations, native.relocations,
                   native.relocation_count * sizeof(*relocations));
        if (has_pool) {
            extra_sections[sections.count] =
                (core_object_emit_section_t){
                    .name = target->object_format == CORE_TARGET_OBJECT_MACH_O
                        ? "__n00b_strings"
                        : target->object_format == CORE_TARGET_OBJECT_COFF
                            ? ".rdata$S" : ".rodata.n00b_strings",
                    .segment = target->object_format
                        == CORE_TARGET_OBJECT_MACH_O ? "__TEXT" : nullptr,
                    .bytes = n00bb_abi_name_pool_bytes(merged.strings),
                    .size = pool_size,
                    .alignment = 1,
                };
            symbols[native.symbol_count] = (core_object_emit_symbol_t){
                .name = "n00b_string_pool",
                .section_index = (uint32_t)(2 + sections.count),
            };
        }
        for (size_t i = 0; i < pool_relocation_count; i++) {
            relocations[native.relocation_count + i] = pool_relocations[i];
            relocations[native.relocation_count + i].symbol_index =
                (uint32_t)native.symbol_count;
        }
        valid = n00bb_abi_write_object(target, &packed,
            extra_sections, extra_count, symbols, symbol_count,
            relocations, relocation_count, &object, &object_bytes);
    }

    core_free(relocations);
    core_free(symbols);
    core_free(extra_sections);
    core_free(pool_relocations);
    core_free(entries);
    core_free(rows);
    n00bb_abi_packed_tables_release(&packed);
    n00bb_abi_native_relocations_release(&native);
    n00bb_abi_native_sections_release(&sections);
    n00bb_abi_merged_rows_release(&merged);
    n00bb_abi_index_plan_free(plan);
    if (!valid) return false;
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}

bool
n00bb_abi_link_selected_objects(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    uint8_t **out_object, size_t *out_object_bytes)
{
    return link_selected_objects(selected, target, nullptr, nullptr,
                                 out_object, out_object_bytes);
}

bool
n00bb_abi_link_objects_with_semantics(
    core_link_unit_t *link, const core_target_desc_t *target,
    const n00bb_abi_object_input_t *candidates, size_t candidate_count,
    n00bb_abi_link_semantics_fn semantics, void *context,
    uint8_t **out_object, size_t *out_object_bytes)
{
    if (!out_object || !out_object_bytes) return false;
    n00bb_abi_selected_objects_t selected = {0};
    if (!n00bb_abi_select_objects(link, target, candidates,
                                  candidate_count, &selected)) return false;
    bool valid = link_selected_objects(&selected, target, semantics, context,
                                       out_object, out_object_bytes);
    n00bb_abi_selected_objects_release(&selected);
    return valid;
}

bool
n00bb_abi_link_objects(
    core_link_unit_t *link, const core_target_desc_t *target,
    const n00bb_abi_object_input_t *candidates, size_t candidate_count,
    uint8_t **out_object, size_t *out_object_bytes)
{
    return n00bb_abi_link_objects_with_semantics(link, target, candidates,
        candidate_count, nullptr, nullptr, out_object, out_object_bytes);
}
