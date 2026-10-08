#include "abi_enum_rows.h"

#include <limits.h>

#include "enum_value.h"
#include "flagset_value.h"
#include "lib/alloc.h"
#include "lib/list.h"

typedef parse_sym_entry_t *enum_symbol_ptr_t;
core_list_decl(enum_symbol_ptr_t);
core_list_decl(n00b_abi_enum_item_v1);
core_list_decl(uint32_t);

bool
n00bb_abi_enum_rows_build(n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    n00bb_abi_type_identity_rows_t *types, n00bb_abi_enum_rows_t *out)
{
    if (!index || !declarations || !names || !types || !types->rows
        || !types->owners || !out
        || types->count != n00bb_abi_type_index_count(index)) return false;
    core_list_t(n00b_abi_enum_item_v1) rows = {0};
    core_list_t(uint32_t) stream = {0};
    n00b_abi_range_v1 *ranges = core_alloc_array(n00b_abi_range_v1,
                                                  types->count);
    core_list_push(rows, ((n00b_abi_enum_item_v1){0}));
    bool valid = true;
    for (size_t i = 1; valid && i < types->count; i++) {
        bool flagset = types->rows[i].kind == N00B_ABI_TYPE_FLAGSET;
        if (types->rows[i].kind != N00B_ABI_TYPE_ENUM && !flagset) continue;
        parse_sym_entry_t *owner = types->owners[i];
        if (!owner) continue;
        if (!owner->member_scope || !owner->decl_node) {
            valid = false;
            break;
        }
        core_list_t(enum_symbol_ptr_t) items = {0};
        for (parse_sym_entry_t *item = owner->member_scope->first_in_scope;
             item; item = item->next_in_scope) {
            if (item->kind != PARSE_SYM_ENUM_CONST
                || item->owner_type != owner || !item->decl_node
                || !item->has_comptime_value || !item->comptime_value) {
                valid = false;
                break;
            }
            int32_t start = core_tree_node_value(item->decl_node).start;
            if (start < 0) { valid = false; break; }
            size_t at = items.len;
            while (at && core_tree_node_value(
                    items.data[at - 1]->decl_node).start > start) at--;
            core_list_insert(items, at, item);
        }
        if (valid && (items.len > UINT32_MAX || rows.len > UINT32_MAX
            || stream.len > UINT32_MAX - stream_base
            || items.len > UINT32_MAX - stream_base - stream.len))
            valid = false;
        if (valid) ranges[i] = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)stream.len,
            .count = (uint32_t)items.len,
        };
        for (size_t j = 0; valid && j < items.len; j++) {
            parse_sym_entry_t *item = items.data[j];
            uint32_t type_index = 0, symbol_index = 0;
            if (rows.len >= UINT32_MAX
                || !n00bb_abi_type_index_find(index, item->type, &type_index)
                || type_index != i
                || !n00bb_abi_decl_find_symbol(declarations, item,
                                                 &symbol_index)) {
                valid = false;
                break;
            }
            ir_uint_t pattern;
            uint32_t flags = 0;
            if (flagset) {
                const n00bb_flagset_value_t *value = item->comptime_value;
                pattern = value->mask;
                if (value->atomic)
                    flags = N00B_ABI_ENUM_ITEM_FLAG_PRIMITIVE;
            }
            else {
                const n00bb_enum_value_t *value = item->comptime_value;
                pattern = value->negative
                    ? (ir_uint_t)0 - value->magnitude : value->magnitude;
            }
            n00b_abi_enum_item_v1 row = {
                .parent_type_index = (uint32_t)i,
                .declaration_ordinal = (uint32_t)j,
                .value_low = (uint64_t)pattern,
                .value_high = (uint64_t)(pattern >> 64),
                .symbol_index = symbol_index,
                .flags = flags,
            };
            valid = n00bb_abi_name_pool_encode(names, item->name,
                N00B_ABI_TABLE_ENUM_ITEM, (uint32_t)rows.len,
                offsetof(n00b_abi_enum_item_v1, name), &row.name);
            if (valid) {
                core_list_push(stream, (uint32_t)rows.len);
                core_list_push(rows, row);
            }
        }
        core_list_free(items);
    }
    if (valid) {
        for (size_t i = 1; i < types->count; i++)
            if ((types->rows[i].kind == N00B_ABI_TYPE_ENUM
                 || types->rows[i].kind == N00B_ABI_TYPE_FLAGSET)
                && types->owners[i])
                types->rows[i].fields_or_arms = ranges[i];
        *out = (n00bb_abi_enum_rows_t){
            .items = rows.data, .item_count = rows.len,
            .stream_indexes = stream.data, .stream_index_count = stream.len,
        };
    }
    else {
        core_list_free(rows);
        core_list_free(stream);
    }
    core_free(ranges);
    return valid;
}

void
n00bb_abi_enum_rows_release(n00bb_abi_enum_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->items);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_enum_rows_t){0};
}
