#include "abi_trait_method_rows.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/list.h"
#include "parse/tree.h"
#include "th/environment.h"

core_list_decl(n00b_abi_trait_method_v1);

typedef struct {
    parse_sym_entry_t *symbol;
    int32_t start;
    uint32_t symbol_index;
    uint32_t signature_type_index;
} trait_method_symbol_t;
core_list_decl(trait_method_symbol_t);

static int
compare_source_order(const void *left, const void *right)
{
    const trait_method_symbol_t *a = left;
    const trait_method_symbol_t *b = right;
    return (a->start > b->start) - (a->start < b->start);
}

bool
n00bb_abi_trait_method_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations,
    const th_env *environment,
    n00bb_abi_trait_method_rows_t *out)
{
    if (!index || !types || !types->rows || !types->owners
        || !declarations || !out
        || types->count != n00bb_abi_type_index_count(index)) return false;
    core_list_t(n00b_abi_trait_method_v1) rows = {0};
    core_list_push(rows, ((n00b_abi_trait_method_v1){0}));
    bool valid = true;
    for (size_t i = 1; valid && i < types->count; i++) {
        if (types->rows[i].kind != N00B_ABI_TYPE_TRAIT) continue;
        if (!types->owners[i]) {
            const th_type *trait = n00bb_abi_type_index_at(index, (uint32_t)i);
            bool builtin = false;
            for (size_t j = 0; environment
                 && j < th_env_declaration_count(environment); j++) {
                th_env_declaration_view_t declaration;
                uint32_t declaration_index = 0;
                if (!th_env_declaration_at(environment, j, &declaration)) {
                    valid = false;
                    break;
                }
                if (declaration.kind == TH_ENV_DECLARATION_TRAIT
                    && declaration.module.data
                    && core_string_eq(declaration.module,
                                      CORE_STRING_STATIC("builtins"))
                    && declaration.contract
                    && n00bb_abi_type_index_find(index,
                        declaration.contract, &declaration_index)
                    && declaration_index == i) builtin = true;
            }
            if (!valid) break;
            if (!builtin) continue;
            if (!trait || th_kind_of((th_type *)trait) != TH_TRAIT
                || th_trait_len(trait) > UINT32_MAX) {
                valid = false;
                break;
            }
            for (size_t ordinal = 0; ordinal < th_trait_len(trait);
                 ordinal++) {
                const th_member *method = th_trait_method(trait, ordinal);
                uint32_t signature_index = 0;
                if (!method || !method->type
                    || !n00bb_abi_type_index_find(index, method->type,
                                                   &signature_index)
                    || signature_index >= types->count
                    || types->rows[signature_index].kind
                        != N00B_ABI_TYPE_FUNCTION
                    || rows.len >= UINT32_MAX) {
                    valid = false;
                    break;
                }
                core_list_push(rows, ((n00b_abi_trait_method_v1){
                    .trait_type_index = (uint32_t)i,
                    .declaration_ordinal = (uint32_t)ordinal,
                    .signature_type_index = signature_index,
                }));
            }
            continue;
        }
        parse_sym_entry_t *owner = types->owners[i];
        th_type *trait = n00bb_abi_type_index_at(index, (uint32_t)i);
        if (!trait || th_kind_of(trait) != TH_TRAIT
            || !owner->member_scope
            || th_trait_len(trait) > UINT32_MAX) {
            valid = false;
            break;
        }
        core_list_t(trait_method_symbol_t) methods = {0};
        for (parse_sym_entry_t *symbol = owner->member_scope->first_in_scope;
             valid && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION
                || symbol->owner_type != owner) continue;
            uint32_t symbol_index = 0, signature_type_index = 0;
            int32_t start = symbol->decl_node
                && !parse_node_is_token(symbol->decl_node)
                ? core_tree_node_value(symbol->decl_node).start : -1;
            if (start < 0 || !symbol->type
                || !n00bb_abi_decl_find_symbol(declarations, symbol,
                                                &symbol_index)
                || !n00bb_abi_type_index_find(index, symbol->type,
                                               &signature_type_index)
                || signature_type_index >= types->count
                || types->rows[signature_type_index].kind
                    != N00B_ABI_TYPE_FUNCTION) {
                valid = false;
                break;
            }
            core_list_push(methods, ((trait_method_symbol_t){
                .symbol = symbol, .start = start,
                .symbol_index = symbol_index,
                .signature_type_index = signature_type_index,
            }));
        }
        if (valid && methods.len != th_trait_len(trait)) valid = false;
        if (valid && methods.len > 1) qsort(methods.data, methods.len,
                         sizeof(*methods.data), compare_source_order);
        for (size_t ordinal = 0; valid && ordinal < methods.len; ordinal++) {
            trait_method_symbol_t *source = &methods.data[ordinal];
            if ((ordinal && source->start == methods.data[ordinal - 1].start)
                || rows.len >= UINT32_MAX) {
                valid = false;
                break;
            }
            size_t matches = 0;
            for (size_t j = 0; j < th_trait_len(trait); j++) {
                const th_member *method = th_trait_method(trait, j);
                uint32_t signature_index = 0;
                if (!method || !method->name.data || !method->type
                    || !n00bb_abi_type_index_find(index, method->type,
                                                   &signature_index)) {
                    valid = false;
                    break;
                }
                if (signature_index == source->signature_type_index
                    && core_string_eq(source->symbol->name, method->name))
                    matches++;
            }
            if (!valid || matches != 1) {
                valid = false;
                break;
            }
            core_list_push(rows, ((n00b_abi_trait_method_v1){
                .trait_type_index = (uint32_t)i,
                .declaration_ordinal = (uint32_t)ordinal,
                .signature_type_index = source->signature_type_index,
                .symbol_index = source->symbol_index,
            }));
        }
        core_list_free(methods);
    }
    if (valid) *out = (n00bb_abi_trait_method_rows_t){
        .rows = rows.data, .count = rows.len,
    };
    else core_list_free(rows);
    return valid;
}

void
n00bb_abi_trait_method_rows_release(n00bb_abi_trait_method_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_trait_method_rows_t){0};
}

bool
n00bb_abi_trait_method_rows_attach(
    n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_trait_method_rows_t *methods, uint32_t stream_base)
{
    if (!types || !types->rows || !methods || !methods->rows
        || methods->stream_indexes || !methods->count
        || methods->count - 1 > UINT32_MAX - stream_base) return false;
    size_t count = methods->count - 1;
    uint32_t *indexes = core_alloc_array(uint32_t, count);
    if (!indexes) return false;
    uint32_t previous = 0;
    for (size_t i = 1; i < methods->count;) {
        uint32_t owner = methods->rows[i].trait_type_index;
        if (!owner || owner <= previous || owner >= types->count
            || types->rows[owner].kind != N00B_ABI_TYPE_TRAIT
            || types->rows[owner].fields_or_arms.count) {
            core_free(indexes);
            return false;
        }
        size_t first = i;
        do {
            if (methods->rows[i].declaration_ordinal != i - first) {
                core_free(indexes);
                return false;
            }
            indexes[i - 1] = (uint32_t)i;
            i++;
        } while (i < methods->count
                 && methods->rows[i].trait_type_index == owner);
        types->rows[owner].fields_or_arms = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)(first - 1),
            .count = (uint32_t)(i - first),
        };
        previous = owner;
    }
    methods->stream_indexes = indexes;
    methods->stream_index_count = count;
    return true;
}
