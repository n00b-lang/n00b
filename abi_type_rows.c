#include "abi_type_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

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
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type) continue;
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
        n00b_abi_type_v1 *row = &rows[i];
        const char *spelling = type ? th_print(types, type) : nullptr;
        valid = type && spelling
            && type_category(runtime, type, owner, row)
            && n00bb_abi_name_pool_encode(names,
                core_string_borrow(spelling, strlen(spelling)),
                N00B_ABI_TABLE_TYPE,
                (uint32_t)i, offsetof(n00b_abi_type_v1, canonical_name),
                &row->canonical_name);
        if (!valid) break;
        row->typehash = th_hash(types, type);
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
    core_free(owners);
    if (!valid || rows[0].kind != N00B_ABI_TYPE_NIL) {
        core_free(rows);
        return false;
    }
    *out = (n00bb_abi_type_identity_rows_t){.rows = rows, .count = count};
    return true;
}

void
n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    *rows = (n00bb_abi_type_identity_rows_t){0};
}
