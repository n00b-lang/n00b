#include "abi_semantics.h"

#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/list.h"

typedef struct {
    th_type *type;
    uint32_t index;
} receiver_t;
typedef struct {
    uint32_t child;
    uint32_t parent;
} specialization_t;
core_list_decl(receiver_t);
core_list_decl(specialization_t);
core_list_decl(n00b_abi_dispatch_v1);
core_list_decl(n00b_abi_dispatch_entry_v1);

static n00bb_abi_merged_table_t *
table(n00bb_abi_merged_rows_t *rows, uint32_t kind)
{
    return (n00bb_abi_merged_table_t *)n00bb_abi_merged_table(rows, kind);
}

static uint32_t
type_index(const n00bb_abi_merged_table_t *types, uint64_t hash)
{
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *row = (const void *)(types->records
            + (size_t)i * types->record_bytes);
        if (row->typehash == hash) return i;
    }
    return 0;
}

static bool
same_string(n00bb_abi_string_view_t value, const char *text)
{
    return text && value.length == strlen(text)
        && !memcmp(value.bytes, text, value.length);
}

static bool
add_receiver(th_ctx *ctx, const n00bb_abi_merged_table_t *types,
             core_list_t(receiver_t) *receivers, th_type *type)
{
    if (!type || !th_is_closed(ctx, type)) return true;
    uint32_t index = type_index(types, th_hash(ctx, type));
    if (!index) return true;
    const n00b_abi_type_v1 *row = (const void *)(types->records
        + (size_t)index * types->record_bytes);
    if (!row->owner_module_index
        || (row->flags & N00B_ABI_TYPE_FLAG_NON_CONCRETE)
        || row->kind == N00B_ABI_TYPE_TRAIT) return true;
    for (size_t i = 0; i < receivers->len; i++)
        if (receivers->data[i].index == index) return true;
    th_type *copy = th_type_clone(ctx, type);
    if (!copy) return false;
    core_list_push(*receivers, ((receiver_t){copy, index}));
    return true;
}

static bool
function_index_for_method(th_ctx *ctx,
                          const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_index_plan_t *plan,
                          const th_env_method_t *method,
                          uint32_t *out)
{
    uint64_t hash = th_hash(ctx, method->contract);
    uint32_t found = 0;
    for (size_t input = 0; input < selected->count; input++) {
        const n00bb_abi_object_view_t *view = &selected->views[input];
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, 1);
        n00bb_abi_string_view_t module_name;
        if (!module || !n00bb_abi_object_string(view, &module->full_path,
                                                 &module_name)) return false;
        if (!same_string(module_name, method->module)) continue;
        const n00bb_abi_directory_entry_t *functions = n00bb_abi_object_table(
            view, N00B_ABI_TABLE_FUNCTION);
        if (!functions) return false;
        for (uint32_t local = 1; local < functions->record_count; local++) {
            const n00b_abi_function_v1 *function = n00bb_abi_object_row(
                view, N00B_ABI_TABLE_FUNCTION, local);
            if (!function || !function->symbol_index) continue;
            n00bb_abi_string_view_t name;
            if (!n00bb_abi_object_string(view, &function->name, &name))
                return false;
            if (!same_string(name, method->root_name)) continue;
            const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
                N00B_ABI_TABLE_SYMBOL, function->symbol_index);
            const n00b_abi_type_v1 *signature = symbol
                ? n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE,
                                       symbol->type_index) : nullptr;
            if (!signature || signature->typehash != hash) continue;
            uint32_t mapped;
            if (found || !n00bb_abi_index_plan_map(plan, input,
                    N00B_ABI_TABLE_FUNCTION, local, &mapped) || !mapped)
                return false;
            found = mapped;
        }
    }
    *out = found;
    return found != 0;
}

static int
compare_specializations(const void *left, const void *right)
{
    const specialization_t *a = left, *b = right;
    if (a->child != b->child) return a->child < b->child ? -1 : 1;
    return (a->parent > b->parent) - (a->parent < b->parent);
}

static bool
add_specializations(n00bb_abi_merged_rows_t *merged,
                    core_list_t(specialization_t) *added)
{
    if (!added->len) return true;
    n00bb_abi_merged_table_t *types = table(merged, N00B_ABI_TABLE_TYPE);
    n00bb_abi_merged_table_t *links = table(merged,
        N00B_ABI_TABLE_SPECIALIZATION_LINK);
    if (!types || !links || types->record_bytes != sizeof(n00b_abi_type_v1)
        || links->record_bytes != sizeof(uint32_t)
        || !links->record_count || !links->records
        || added->len > UINT32_MAX - links->record_count) return false;
    core_list_t(specialization_t) edges = {0};
    for (uint32_t child = 1; child < types->record_count; child++) {
        const n00b_abi_type_v1 *type = (const void *)(types->records
            + (size_t)child * types->record_bytes);
        for (uint32_t j = type->specialization_start_ix;
             j < type->specialization_end_ix; j++) {
            if (j >= links->record_count) { core_list_free(edges); return false; }
            uint32_t parent = ((uint32_t *)links->records)[j];
            core_list_push(edges, ((specialization_t){child, parent}));
        }
    }
    for (size_t i = 0; i < added->len; i++)
        core_list_push(edges, added->data[i]);
    qsort(edges.data, edges.len, sizeof(*edges.data),
          compare_specializations);
    size_t unique = 0;
    for (size_t i = 0; i < edges.len; i++)
        if (!unique || compare_specializations(&edges.data[i],
                                                &edges.data[unique - 1]))
            edges.data[unique++] = edges.data[i];
    bool valid = unique < UINT32_MAX;
    uint32_t *incoming = valid
        ? core_alloc_array(uint32_t, types->record_count) : nullptr;
    uint32_t *queue = valid
        ? core_alloc_array(uint32_t, types->record_count) : nullptr;
    n00b_abi_range_v1 *spans = valid
        ? core_alloc_array(n00b_abi_range_v1, types->record_count) : nullptr;
    uint32_t *rebuilt = valid
        ? core_alloc_array(uint32_t, unique + 1) : nullptr;
    valid = valid && incoming && queue && spans && rebuilt;
    if (valid) {
        memset(incoming, 0, types->record_count * sizeof(*incoming));
        memset(spans, 0, types->record_count * sizeof(*spans));
        rebuilt[0] = 0;
        for (size_t i = 0; valid && i < unique; i++) {
            specialization_t edge = edges.data[i];
            if (!edge.child || !edge.parent || edge.child == edge.parent
                || edge.child >= types->record_count
                || edge.parent >= types->record_count) { valid = false; break; }
            n00b_abi_range_v1 *span = &spans[edge.child];
            if (!span->count) span->first = (uint32_t)i + 1;
            span->count++;
            rebuilt[i + 1] = edge.parent;
            incoming[edge.parent]++;
        }
        size_t head = 0, tail = 0;
        for (uint32_t i = 0; valid && i < types->record_count; i++)
            if (!incoming[i]) queue[tail++] = i;
        while (valid && head < tail) {
            uint32_t child = queue[head++];
            n00b_abi_range_v1 span = spans[child];
            for (uint32_t j = 0; j < span.count; j++) {
                uint32_t parent = rebuilt[span.first + j];
                if (!--incoming[parent]) queue[tail++] = parent;
            }
        }
        valid = valid && tail == types->record_count;
    }
    if (valid) {
        for (uint32_t i = 0; i < types->record_count; i++) {
            n00b_abi_type_v1 *type = (void *)(types->records
                + (size_t)i * types->record_bytes);
            type->specialization_start_ix = spans[i].first;
            type->specialization_end_ix = spans[i].count
                ? spans[i].first + spans[i].count : 0;
        }
        core_free(links->records);
        links->records = (uint8_t *)rebuilt;
        links->record_count = (uint32_t)unique + 1;
        rebuilt = nullptr;
    }
    core_free(rebuilt);
    core_free(spans);
    core_free(queue);
    core_free(incoming);
    core_list_free(edges);
    return valid;
}

static bool
add_dispatch(n00bb_abi_merged_rows_t *merged,
             core_list_t(n00b_abi_dispatch_v1) *dispatch,
             core_list_t(n00b_abi_dispatch_entry_v1) *entries)
{
    if (dispatch->len == 1) return true;
    n00bb_abi_merged_table_t *rows = table(merged, N00B_ABI_TABLE_DISPATCH);
    n00bb_abi_merged_table_t *items = table(merged,
        N00B_ABI_TABLE_DISPATCH_ENTRY);
    n00bb_abi_merged_table_t *stream = table(merged, N00B_ABI_TABLE_U32_STREAM);
    if (!rows || !items || !stream || rows->record_count > 1
        || items->record_count > 1
        || rows->record_bytes != sizeof(n00b_abi_dispatch_v1)
        || items->record_bytes != sizeof(n00b_abi_dispatch_entry_v1)
        || stream->record_bytes != sizeof(uint32_t)
        || dispatch->len > UINT32_MAX || entries->len > UINT32_MAX
        || entries->len - 1 > UINT32_MAX - stream->record_count)
        return false;
    size_t old_count = stream->record_count;
    uint32_t *rebuilt = core_realloc(stream->records,
        (old_count + entries->len - 1) * sizeof(uint32_t));
    if (!rebuilt) return false;
    stream->records = (uint8_t *)rebuilt;
    size_t next = old_count;
    for (size_t i = 1; i < dispatch->len; i++) {
        n00b_abi_dispatch_v1 *row = &dispatch->data[i];
        uint32_t first_entry = row->entries.first;
        if (!row->entries.count) { row->entries.first = 0; continue; }
        row->entries.first = (uint32_t)next;
        for (uint32_t j = 0; j < row->entries.count; j++)
            rebuilt[next++] = first_entry + j;
    }
    if (next != old_count + entries->len - 1) return false;
    stream->record_count = (uint32_t)next;
    core_free(rows->records);
    core_free(items->records);
    rows->records = (uint8_t *)dispatch->data;
    rows->record_count = (uint32_t)dispatch->len;
    items->records = (uint8_t *)entries->data;
    items->record_count = (uint32_t)entries->len;
    dispatch->data = nullptr;
    entries->data = nullptr;
    return true;
}

bool
n00bb_abi_link_semantic_rows(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    n00bb_abi_merged_rows_t *merged, void *context)
{
    const n00bb_abi_semantics_t *facts = context;
    n00bb_abi_merged_table_t *types = table(merged, N00B_ABI_TABLE_TYPE);
    if (!facts || !facts->environment || !selected || !plan || !types
        || types->record_bytes != sizeof(n00b_abi_type_v1)) return false;
    th_ctx *ctx = th_ctx_new();
    core_list_t(receiver_t) receivers = {0};
    core_list_t(specialization_t) specializations = {0};
    core_list_t(n00b_abi_dispatch_v1) dispatch = {0};
    core_list_t(n00b_abi_dispatch_entry_v1) entries = {0};
    core_list_push(dispatch, ((n00b_abi_dispatch_v1){0}));
    core_list_push(entries, ((n00b_abi_dispatch_entry_v1){0}));
    bool valid = ctx != nullptr;
    size_t declarations = th_env_declaration_count(facts->environment);
    const char *initial_module = "", *initial_package = "";
    for (size_t i = 0; i < declarations; i++) {
        th_env_declaration_view_t declaration;
        if (!th_env_declaration_at(facts->environment, i, &declaration)) {
            valid = false; break;
        }
        if (declaration.kind == TH_ENV_DECLARATION_TRAIT) {
            initial_module = declaration.module;
            initial_package = declaration.package;
            break;
        }
    }
    if (valid) valid = th_ctx_set_environment(ctx, facts->environment,
        initial_module, initial_package);
    for (size_t i = 0; valid && i < declarations; i++) {
        th_env_declaration_view_t declaration;
        valid = th_env_declaration_at(facts->environment, i, &declaration);
        if (!valid || declaration.kind != TH_ENV_DECLARATION_TYPE) continue;
        valid = add_receiver(ctx, types, &receivers,
            th_name(ctx, declaration.qualified_name));
    }
    for (size_t m = 0; valid && m < facts->module_count; m++) {
        ir_module_t *module = (ir_module_t *)facts->modules[m];
        for (size_t i = 0; valid && i < ir_type_count(module); i++)
            valid = add_receiver(ctx, types, &receivers,
                ir_type_at(module, i));
    }
    for (size_t i = 0; valid && i < declarations; i++) {
        th_env_declaration_view_t trait;
        valid = th_env_declaration_at(facts->environment, i, &trait);
        if (!valid || trait.kind != TH_ENV_DECLARATION_TRAIT) continue;
        if (!trait.contract || th_kind_of(trait.contract) != TH_TRAIT)
            continue;
        uint32_t trait_index = type_index(types,
            th_hash(ctx, trait.contract));
        if (!trait_index) continue;
        const n00b_abi_type_v1 *trait_row = (const void *)(types->records
            + (size_t)trait_index * types->record_bytes);
        size_t methods = th_trait_len(trait.contract);
        if (trait_row->kind != N00B_ABI_TYPE_TRAIT
            || trait_row->fields_or_arms.count != methods
            || !th_ctx_set_environment_scope(ctx, trait.module,
                                               trait.package)) {
            valid = false; break;
        }
        for (size_t r = 0; valid && r < receivers.len; r++) {
            receiver_t receiver = receivers.data[r];
            if (th_env_implements(ctx, receiver.type, trait.qualified_name))
                core_list_push(specializations, ((specialization_t){
                    receiver.index, trait_index,
                }));
        }
        for (size_t ordinal = 0; valid && ordinal < methods; ordinal++) {
            const th_member *method = th_trait_method(trait.contract,
                                                      ordinal);
            if (!method || !method->name || dispatch.len >= UINT32_MAX) {
                valid = false; break;
            }
            n00b_abi_dispatch_v1 row = {
                .trait_type_index = trait_index,
                .method_ordinal = (uint32_t)ordinal,
                .entries.first = (uint32_t)entries.len,
            };
            for (size_t r = 0; valid && r < receivers.len; r++) {
                receiver_t receiver = receivers.data[r];
                if (!th_env_implements(ctx, receiver.type,
                                       trait.qualified_name)) continue;
                th_env_method_t chosen = {0};
                uint32_t function_index = 0;
                valid = th_env_method_resolve(ctx, receiver.type,
                    trait.qualified_name, method->name, &chosen)
                        == TH_ENV_METHOD_FOUND
                    && function_index_for_method(ctx, selected, plan,
                        &chosen, &function_index)
                    && entries.len < UINT32_MAX;
                if (valid) core_list_push(entries,
                    ((n00b_abi_dispatch_entry_v1){
                        receiver.index, function_index,
                    }));
            }
            if (valid && entries.len > row.entries.first) {
                row.entries.count = (uint32_t)entries.len - row.entries.first;
                core_list_push(dispatch, row);
            }
        }
    }
    if (valid) valid = add_specializations(merged, &specializations)
        && add_dispatch(merged, &dispatch, &entries);
    core_list_free(entries);
    core_list_free(dispatch);
    core_list_free(specializations);
    core_list_free(receivers);
    th_ctx_free(ctx);
    return valid;
}
