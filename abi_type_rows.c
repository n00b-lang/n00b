#include "abi_type_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "enum_value.h"
#include "signature_typevars.h"

core_list_decl(uint32_t);

static bool
name_is(const char *name, size_t length, const char *expected)
{
    return length == strlen(expected) && !memcmp(name, expected, length);
}

static bool
kind_from_owner(const parse_sym_entry_t *owner, uint32_t *kind)
{
    if (!owner || !owner->adt_kind.u8_bytes) return false;
    core_string_t adt = owner->adt_kind;
    if (core_string_eq(adt, CORE_STRING_STATIC("record"))
        || core_string_eq(adt, CORE_STRING_STATIC("context")))
        *kind = N00B_ABI_TYPE_RECORD;
    else if (core_string_eq(adt, CORE_STRING_STATIC("variant")))
        *kind = N00B_ABI_TYPE_VARIANT;
    else if (core_string_eq(adt, CORE_STRING_STATIC("enum")))
        *kind = N00B_ABI_TYPE_ENUM;
    else if (core_string_eq(adt, CORE_STRING_STATIC("flagset")))
        *kind = N00B_ABI_TYPE_FLAGSET;
    else if (core_string_eq(adt, CORE_STRING_STATIC("trait")))
        *kind = N00B_ABI_TYPE_TRAIT;
    else if (core_string_eq(adt, CORE_STRING_STATIC("callback")))
        *kind = N00B_ABI_TYPE_CALLBACK;
    else if (core_string_eq(adt, CORE_STRING_STATIC("nominal")))
        *kind = N00B_ABI_TYPE_WRAPPER;
    else return false;
    return true;
}

static bool
type_category(ir_module_t *module, th_type *type,
              const parse_sym_entry_t *owner, n00b_abi_type_v1 *row)
{
    if (kind_from_owner(owner, &row->kind)) return true;
    switch (th_kind_of(type)) {
    case TH_BITINT: {
        uint64_t width = 0;
        th_bitint_flavor flavor;
        if (!th_bitint_info(type, &width, &flavor) || width > UINT32_MAX)
            return false;
        row->kind = N00B_ABI_TYPE_PRIMITIVE;
        row->semantic_width_bits = (uint32_t)width;
        if (flavor == TH_BITINT_SIGNED)
            row->flags |= N00B_ABI_TYPE_FLAG_SIGNED;
        if (flavor == TH_BITINT_BITS)
            row->flags |= N00B_ABI_TYPE_FLAG_BITS;
        return true;
    }
    case TH_NAMED: {
        const char *name = nullptr;
        size_t name_len = 0;
        bool variant = false;
        if (!th_named_info(type, &name, &name_len, &variant, nullptr)
            || !name) return false;
        if (name_is(name, name_len, "nil"))
            row->kind = N00B_ABI_TYPE_NIL;
        else if (name_is(name, name_len, "string"))
            row->kind = N00B_ABI_TYPE_STRING;
        else if (name_is(name, name_len, "any"))
            row->kind = N00B_ABI_TYPE_BOX;
        else if (name_is(name, name_len, "array")
                 || name_is(name, name_len, "Array"))
            row->kind = N00B_ABI_TYPE_ARRAY;
        else if (name_is(name, name_len, "view")
                 || name_is(name, name_len, "View"))
            row->kind = N00B_ABI_TYPE_VIEW;
        else if (name_is(name, name_len, "option")
                 || name_is(name, name_len, "Option"))
            row->kind = N00B_ABI_TYPE_OPTION;
        else if (name_is(name, name_len, "ref")
                 || name_is(name, name_len, "Ref"))
            row->kind = N00B_ABI_TYPE_REFERENCE;
        else if (name_is(name, name_len, "atomic")
                 || name_is(name, name_len, "Atomic"))
            row->kind = N00B_ABI_TYPE_ATOMIC;
        else if (name_is(name, name_len, "Allocator")) {
            row->kind = N00B_ABI_TYPE_PRIMITIVE;
            row->semantic_width_bits = 32;
        }
        else if (variant) row->kind = N00B_ABI_TYPE_VARIANT;
        else {
            th_type *definition = ir_type_definition(module, type);
            if (definition && th_kind_of(definition) == TH_RECORD) {
                row->kind = N00B_ABI_TYPE_RECORD;
                return true;
            }
            ir_machine_t machine = ir_machine_of(module, type);
            if (machine.class != IR_CLASS_BOOL
                && machine.class != IR_CLASS_SINT
                && machine.class != IR_CLASS_UINT
                && machine.class != IR_CLASS_FLOAT)
                return false;
            row->kind = N00B_ABI_TYPE_PRIMITIVE;
            row->semantic_width_bits = machine.width;
            if (machine.class == IR_CLASS_SINT)
                row->flags |= N00B_ABI_TYPE_FLAG_SIGNED;
        }
        return true;
    }
    case TH_INDIRECTION:
        row->kind = N00B_ABI_TYPE_REFERENCE;
        return true;
    case TH_SEQUENCE:
        row->kind = N00B_ABI_TYPE_ARRAY;
        return true;
    case TH_RECORD:
        row->kind = N00B_ABI_TYPE_RECORD;
        return true;
    case TH_FUNCTION:
        row->kind = N00B_ABI_TYPE_FUNCTION;
        return true;
    case TH_BINDER:
        return th_binder_body(type)
            && type_category(module, th_binder_body(type), owner, row);
    case TH_SUM: {
        th_comb combination;
        if (!th_sum_comb(type, &combination)
            || combination != TH_UNION) return false;
        row->kind = N00B_ABI_TYPE_UNION;
        return true;
    }
    case TH_VAR:
    case TH_BOUND:
        row->kind = N00B_ABI_TYPE_VARIABLE;
        row->flags |= N00B_ABI_TYPE_FLAG_NON_CONCRETE;
        return true;
    case TH_TRAIT:
        row->kind = N00B_ABI_TYPE_TRAIT;
        return true;
    }
    return false;
}

bool
n00bb_abi_type_identity_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime, const parse_symtab_t *symbols,
    n00bb_abi_type_owner_lookup_fn lookup, void *lookup_context,
    n00bb_abi_name_pool_t *names, n00bb_abi_type_identity_rows_t *out)
{
    if (!index || !runtime || !ir_module_target(runtime) || !names || !out
        || (comptime && (ir_module_types(comptime) != ir_module_types(runtime)
                         || !ir_module_target(comptime)
                         || strcmp(ir_module_target(comptime),
                                   ir_module_target(runtime))))) return false;
    size_t count = n00bb_abi_type_index_count(index);
    if (!count || count > (UINT32_MAX >> 1)) return false;
    n00b_abi_type_v1 *rows = core_alloc_array(n00b_abi_type_v1, count);
    parse_sym_entry_t **owners = core_alloc_array(parse_sym_entry_t *, count);
    for (parse_scope_t *scope = symbols ? symbols->all_scopes : nullptr;
         scope; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                || !symbol->decl_node) continue;
            uint32_t row = 0;
            if (!n00bb_abi_type_index_find(index, symbol->type, &row))
                continue;
            if (!owners[row] || (!owners[row]->adt_kind.u8_bytes
                                 && symbol->adt_kind.u8_bytes))
                owners[row] = symbol;
        }
    }
    th_ctx *types = ir_module_types(runtime);
    bool valid = types != nullptr;
    for (size_t i = 0; valid && i < count; i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        parse_sym_entry_t *owner = owners[i];
        parse_sym_entry_t *category_owner = owner ? owner
            : lookup ? lookup(lookup_context, type) : nullptr;
        n00b_abi_type_v1 *row = &rows[i];
        valid = type && type_category(runtime, type, category_owner, row);
        if (!valid) break;
        if (row->kind == N00B_ABI_TYPE_VARIABLE) continue;
        const char *spelling = th_print(types, type);
        valid = spelling && n00bb_abi_name_pool_encode(names,
                core_string_borrow(spelling, strlen(spelling)),
                N00B_ABI_TABLE_TYPE,
                (uint32_t)i, offsetof(n00b_abi_type_v1, canonical_name),
                &row->canonical_name);
        if (!valid) break;
        row->typehash = th_hash(types, type);
        if (category_owner && row->kind == N00B_ABI_TYPE_ENUM) {
            const n00bb_enum_range_t *range = parse_sym_binding(category_owner,
                CORE_STRING_STATIC("n00bb.enum-range"));
            uint64_t width = range
                ? range->magnitude_bits + (uint64_t)range->is_signed : 0;
            if (!range || width > UINT32_MAX) {
                valid = false;
                break;
            }
            row->semantic_width_bits = width ? (uint32_t)width : 1;
            if (range->is_signed)
                row->flags |= N00B_ABI_TYPE_FLAG_SIGNED;
        }
        if (!th_is_closed(types, type))
            row->flags |= N00B_ABI_TYPE_FLAG_NON_CONCRETE;
        const ir_final_layout_t *final = ir_layout_final(
            runtime, type, ir_module_target(runtime));
        const ir_final_layout_t *compile_layout = comptime
            ? ir_layout_final(comptime, type, ir_module_target(runtime))
            : nullptr;
        if (final && compile_layout
            && (final->intrinsic_bits != compile_layout->intrinsic_bits
                || final->alignment_bytes != compile_layout->alignment_bytes)) {
            valid = false;
            break;
        }
        if (!final) final = compile_layout;
        if (final) {
            row->size_bits = final->intrinsic_bits;
            row->alignment_bytes = final->alignment_bytes;
            if (row->kind == N00B_ABI_TYPE_PRIMITIVE
                || row->kind == N00B_ABI_TYPE_ENUM
                || row->kind == N00B_ABI_TYPE_FLAGSET) {
                if (final->intrinsic_bits > UINT32_MAX) {
                    valid = false;
                    break;
                }
                row->unpacked_storage_bits = (uint32_t)final->intrinsic_bits;
            }
        }
        else if (row->kind == N00B_ABI_TYPE_NIL)
            row->alignment_bytes = 1;
    }
    if (!valid || rows[0].kind != N00B_ABI_TYPE_NIL) {
        core_free(owners);
        core_free(rows);
        return false;
    }
    *out = (n00bb_abi_type_identity_rows_t){
        .rows = rows, .owners = owners, .count = count,
    };
    return true;
}

bool
n00bb_abi_type_variables_bind(n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_type_identity_rows_t *rows)
{
    if (!index || !declarations || !rows || !rows->rows
        || rows->count != n00bb_abi_type_index_count(index)) return false;
    for (size_t i = 1; i < n00bb_abi_decl_symbol_count(declarations); i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)i);
        n00bb_signature_typevars_t *variables = symbol
            ? parse_sym_binding(symbol, N00BB_SIGNATURE_TYPEVARS_KEY)
            : nullptr;
        if (!variables || !variables->len) continue;
        if (!symbol->decl_node || !variables->data
            || variables->len > UINT32_MAX) return false;
        parse_scope_t *scope = core_tree_node_value(symbol->decl_node).scope;
        uint32_t scope_index = 0;
        if (!n00bb_abi_decl_find_scope(declarations, scope, &scope_index)
            || !scope_index) return false;
        for (size_t ordinal = 0; ordinal < variables->len; ordinal++) {
            th_type *variable = variables->data[ordinal].type;
            if (!variable) return false;
            variable = th_resolve(variable);
            if (th_kind_of(variable) != TH_VAR) continue;
            uint32_t type_index = 0;
            if (!n00bb_abi_type_index_find(index, variable, &type_index))
                continue;
            n00b_abi_type_v1 *row = &rows->rows[type_index];
            if (row->kind != N00B_ABI_TYPE_VARIABLE
                || (row->typevar_owner_scope_index
                    && (row->typevar_owner_scope_index != scope_index
                        || row->typevar_ordinal != ordinal))) return false;
            row->typevar_owner_scope_index = scope_index;
            row->typevar_ordinal = (uint32_t)ordinal;
        }
    }
    for (size_t i = 1; i < rows->count; i++)
        if (rows->rows[i].kind == N00B_ABI_TYPE_VARIABLE
            && !rows->rows[i].typevar_owner_scope_index) return false;
    return true;
}

void
n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->owners);
    *rows = (n00bb_abi_type_identity_rows_t){0};
}

static bool
append_component(n00bb_abi_type_index_t *index, th_type *component,
                 core_list_t(uint32_t) *indexes)
{
    uint32_t child = 0;
    if (!component || !n00bb_abi_type_index_find(index, component, &child))
        return false;
    core_list_push(*indexes, child);
    return true;
}

static bool
append_type_components(n00bb_abi_type_index_t *index, ir_module_t *module,
                       th_type *type, uint32_t kind,
                       core_list_t(uint32_t) *indexes)
{
    th_kind source_kind = th_kind_of(type);
    if (kind == N00B_ABI_TYPE_UNION) {
        if (source_kind != TH_SUM) return false;
        th_comb combination;
        if (!th_sum_comb(type, &combination) || combination != TH_UNION)
            return false;
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (!append_component(index, th_sum_arm(type, i), indexes))
                return false;
        return true;
    }
    if (kind == N00B_ABI_TYPE_REFERENCE
        || kind == N00B_ABI_TYPE_OPTION
        || kind == N00B_ABI_TYPE_ARRAY
        || kind == N00B_ABI_TYPE_VIEW
        || kind == N00B_ABI_TYPE_WRAPPER
        || kind == N00B_ABI_TYPE_ATOMIC) {
        th_type *component = nullptr;
        if (source_kind == TH_NAMED && kind == N00B_ABI_TYPE_WRAPPER)
            component = ir_type_definition(module, type);
        else if (source_kind == TH_NAMED) {
            const th_arg *arg = th_named_nargs(type) == 1
                ? th_named_arg(type, 0) : nullptr;
            if (arg && arg->kind == TH_ARG_TYPE) component = arg->type;
        }
        else if (source_kind == TH_INDIRECTION
                 && kind == N00B_ABI_TYPE_REFERENCE)
            component = th_ind_referent(type);
        else if (source_kind == TH_SEQUENCE
                 && kind == N00B_ABI_TYPE_ARRAY)
            component = th_seq_elem(type);
        return append_component(index, component, indexes);
    }
    if (source_kind == TH_NAMED && th_named_nargs(type)) return false;
    return kind != N00B_ABI_TYPE_CALLBACK;
}

bool
n00bb_abi_type_components_build(
    n00bb_abi_type_index_t *index, ir_module_t *module,
    uint32_t stream_base, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_components_t *out)
{
    if (!index || !module || !types || !types->rows || !out
        || types->count != n00bb_abi_type_index_count(index)) return false;
    core_list_t(uint32_t) indexes = {0};
    n00b_abi_range_v1 *ranges = core_alloc_array(n00b_abi_range_v1,
                                                  types->count);
    bool valid = true;
    for (size_t i = 0; valid && i < types->count; i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        if (!type || indexes.len > UINT32_MAX - stream_base) {
            valid = false;
            break;
        }
        size_t first = indexes.len;
        valid = append_type_components(index, module, type,
            types->rows[i].kind, &indexes)
            && indexes.len <= UINT32_MAX - stream_base;
        if (valid && indexes.len != first)
            ranges[i] = (n00b_abi_range_v1){
                .first = stream_base + (uint32_t)first,
                .count = (uint32_t)(indexes.len - first),
            };
    }
    if (valid) {
        for (size_t i = 0; i < types->count; i++)
            types->rows[i].components = ranges[i];
        *out = (n00bb_abi_type_components_t){
            .indexes = indexes.data, .count = indexes.len,
        };
    }
    else core_list_free(indexes);
    core_free(ranges);
    return valid;
}

void
n00bb_abi_type_components_release(n00bb_abi_type_components_t *rows)
{
    if (!rows) return;
    core_free(rows->indexes);
    *rows = (n00bb_abi_type_components_t){0};
}
