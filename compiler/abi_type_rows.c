#include "abi_type_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "th/environment.h"
#include "abi_layout.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "signature_typevars.h"

core_list_decl(uint32_t);
core_list_decl(n00b_abi_type_argument_v1);

static bool
kind_from_owner(const parse_sym_entry_t *owner, uint32_t *kind)
{
    if (!owner) return false;
    if (owner->member_scope
        && core_string_eq(owner->member_scope->scope_tag,
                          CORE_STRING_STATIC("flagset"))) {
        *kind = N00B_ABI_TYPE_FLAGSET;
        return true;
    }
    if (!owner->adt_kind.u8_bytes) return false;
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
        bool variant = false;
        if (!th_named_info(type, nullptr, nullptr, &variant, nullptr))
            return false;
        th_builtin_kind_t builtin = th_builtin_kind_of(type);
        if (builtin == TH_BUILTIN_NIL)
            row->kind = N00B_ABI_TYPE_NIL;
        else if (builtin == TH_BUILTIN_STRING)
            row->kind = N00B_ABI_TYPE_STRING;
        else if (builtin == TH_BUILTIN_ANY)
            row->kind = N00B_ABI_TYPE_BOX;
        else if (builtin == TH_BUILTIN_ARRAY)
            row->kind = N00B_ABI_TYPE_ARRAY;
        else if (builtin == TH_BUILTIN_OPTION)
            row->kind = N00B_ABI_TYPE_OPTION;
        else if (builtin == TH_BUILTIN_ATOMIC)
            row->kind = N00B_ABI_TYPE_ATOMIC;
        else if (builtin == TH_BUILTIN_ALLOCATOR
                 || builtin == TH_BUILTIN_TYPESPEC) {
            row->kind = N00B_ABI_TYPE_PRIMITIVE;
            row->semantic_width_bits = 32;
            if (builtin == TH_BUILTIN_TYPESPEC)
                row->flags |= N00B_ABI_TYPE_FLAG_TYPE_INDEX;
        }
        else if (builtin == TH_BUILTIN_TUPLE)
            row->kind = N00B_ABI_TYPE_TUPLE;
        else if (variant) row->kind = N00B_ABI_TYPE_VARIANT;
        else {
            th_type *definition = ir_type_definition(module, type);
            if (definition && th_kind_of(definition) == TH_RECORD) {
                row->kind = N00B_ABI_TYPE_RECORD;
                return true;
            }
            if (th_named_nargs(type)) {
                row->kind = N00B_ABI_TYPE_NAMED;
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
    case TH_INDIRECTION: {
        uint8_t attrs = 0;
        if (!th_ind_attrs(type, &attrs)) return false;
        row->kind = attrs & TH_FOREIGN ? N00B_ABI_TYPE_FOREIGN
                                       : N00B_ABI_TYPE_REFERENCE;
        if (!attrs) row->flags |= N00B_ABI_TYPE_FLAG_PLAIN_REF;
        return true;
    }
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
    n00bb_abi_type_index_t *index, ir_module_t *runtime,
    bool include_external_definitions, const parse_symtab_t *symbols,
    n00bb_abi_type_owner_lookup_fn lookup, void *lookup_context,
    n00bb_abi_name_pool_t *names, n00bb_abi_type_identity_rows_t *out)
{
    if (!index || !runtime || !ir_module_target(runtime) || !names || !out)
        return false;
    size_t count = n00bb_abi_type_index_count(index);
    if (!count || count > (UINT32_MAX >> 1)) return false;
    n00b_abi_type_v1 *rows = core_alloc_array(n00b_abi_type_v1, count);
    parse_sym_entry_t **owners = core_alloc_array(parse_sym_entry_t *, count);
    core_list_t(n00b_abi_type_argument_v1) arguments = {0};
    core_list_push(arguments, ((n00b_abi_type_argument_v1){0}));
    for (parse_scope_t *scope = symbols ? symbols->all_scopes : nullptr;
         scope; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                || !symbol->decl_node || parse_sym_binding_origin(symbol)
                || core_string_eq(symbol->adt_kind,
                    CORE_STRING_STATIC("alias"))) continue;
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
    size_t failing_index = SIZE_MAX;
    const char *failed_reason = "type category";
    for (size_t i = 0; valid && i < count; i++) {
        failing_index = i;
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        parse_sym_entry_t *owner = owners[i];
        parse_sym_entry_t *category_owner = owner ? owner
            : lookup ? lookup(lookup_context, type) : nullptr;
        n00b_abi_type_v1 *row = &rows[i];
        valid = type && type_category(runtime, type, category_owner, row);
        if (!valid) break;
        if (row->kind == N00B_ABI_TYPE_VARIABLE) continue;
        failed_reason = "canonical type name";
        th_print_options_t print_options = {.omit_constraints = true};
        const char *spelling = th_print_with_options(types, type,
                                                     &print_options);
        valid = spelling && n00bb_abi_name_pool_encode(names,
                core_string_borrow(spelling, strlen(spelling)),
                N00B_ABI_TABLE_TYPE,
                (uint32_t)i, offsetof(n00b_abi_type_v1, canonical_name),
                &row->canonical_name);
        if (!valid) break;
        row->typehash = th_hash(types, type);
        if (th_kind_of(type) == TH_NAMED) {
            failed_reason = "named constructor";
            const char *constructor = nullptr;
            size_t constructor_bytes = 0;
            valid = th_named_info(type, &constructor, &constructor_bytes,
                                  nullptr, nullptr)
                && constructor && constructor_bytes
                && n00bb_abi_name_pool_encode(names,
                    core_string_borrow(constructor, constructor_bytes),
                    N00B_ABI_TABLE_TYPE, (uint32_t)i,
                    offsetof(n00b_abi_type_v1, named_constructor),
                    &row->named_constructor);
            if (!valid) break;
            size_t n = th_named_nargs(type);
            if (n && (arguments.len > UINT32_MAX
                      || n > UINT32_MAX - arguments.len)) {
                valid = false;
                break;
            }
            if (n) row->named_arguments = (n00b_abi_range_v1){
                .first = (uint32_t)arguments.len, .count = (uint32_t)n,
            };
            failed_reason = "named type argument";
            for (size_t ordinal = 0; valid && ordinal < n; ordinal++) {
                const th_arg *arg = th_named_arg(type, ordinal);
                n00b_abi_type_argument_v1 value = {
                    .owner_type_index = (uint32_t)i,
                    .ordinal = (uint32_t)ordinal,
                };
                if (!arg) { valid = false; break; }
                switch (arg->kind) {
                case TH_ARG_TYPE:
                    value.kind = N00B_ABI_TYPE_ARG_TYPE;
                    valid = arg->type && n00bb_abi_type_index_find(index,
                        arg->type, &value.type_index);
                    break;
                case TH_ARG_UINT:
                    value.kind = N00B_ABI_TYPE_ARG_UINT;
                    value.scalar_bits = arg->u;
                    break;
                case TH_ARG_SINT:
                    value.kind = N00B_ABI_TYPE_ARG_SINT;
                    value.scalar_bits = (uint64_t)arg->s;
                    break;
                case TH_ARG_STR:
                    value.kind = N00B_ABI_TYPE_ARG_STRING;
                    valid = arg->str.p && n00bb_abi_name_pool_encode(names,
                        core_string_borrow(arg->str.p, arg->str.n),
                        N00B_ABI_TABLE_TYPE_ARGUMENT,
                        (uint32_t)arguments.len,
                        offsetof(n00b_abi_type_argument_v1, string_value),
                        &value.string_value);
                    break;
                default:
                    valid = false;
                    break;
                }
                if (valid) core_list_push(arguments, value);
            }
            if (!valid) break;
        }
        if (category_owner && row->kind == N00B_ABI_TYPE_ENUM) {
            failed_reason = "enum range";
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
        if (category_owner && row->kind == N00B_ABI_TYPE_FLAGSET) {
            failed_reason = "flagset information";
            const n00bb_flagset_info_t *info = parse_sym_binding(
                category_owner, CORE_STRING_STATIC("n00bb.flagset-info"));
            if (!info || !info->atomic_count || info->atomic_count > 128) {
                valid = false;
                break;
            }
            row->semantic_width_bits = info->atomic_count;
        }
        parse_sym_entry_t *declaration_owner = owner;
        if (!declaration_owner && category_owner && category_owner->type
            && th_hash(types, type) ==
                th_hash(types, category_owner->type))
            declaration_owner = category_owner;
        n00bb_signature_typevars_t *type_parameters = declaration_owner
            && declaration_owner->kind == PARSE_SYM_TYPEDEF
            ? parse_sym_binding(declaration_owner, N00BB_SIGNATURE_TYPEVARS_KEY)
            : nullptr;
        th_type *definition = th_type_definition(types, type);
        if (!th_is_closed(types, type)
            || (type_parameters && type_parameters->len)
            || (definition && !th_is_closed(types, definition)))
            row->flags |= N00B_ABI_TYPE_FLAG_NON_CONCRETE;
        ir_module_t *definition_owner = ir_type_definition_owner(runtime,
            type);
        size_t named_arguments = 0;
        bool specialization = th_named_info(th_resolve(type), nullptr,
            nullptr, nullptr, &named_arguments) && named_arguments != 0;
        bool external = !include_external_definitions && !owner
            && definition_owner && definition_owner != runtime
            && row->kind != N00B_ABI_TYPE_ATOMIC;
        const ir_final_layout_t *final = ir_layout_final(runtime, type,
            ir_module_target(runtime));
        if (external && !specialization) final = nullptr;
        if (final) {
            row->size_bits = final->intrinsic_bits;
            row->alignment_bytes = final->alignment_bytes;
            if (n00bb_abi_layout_packable(final)
                && (row->kind == N00B_ABI_TYPE_PRIMITIVE
                    || row->kind == N00B_ABI_TYPE_ENUM
                    || row->kind == N00B_ABI_TYPE_FLAGSET
                    || row->kind == N00B_ABI_TYPE_WRAPPER))
                row->flags |= N00B_ABI_TYPE_FLAG_PACKABLE;
            if (row->kind == N00B_ABI_TYPE_PRIMITIVE
                || row->kind == N00B_ABI_TYPE_ENUM
                || row->kind == N00B_ABI_TYPE_FLAGSET
                || row->kind == N00B_ABI_TYPE_ATOMIC) {
                if (final->intrinsic_bits > UINT32_MAX) {
                    failed_reason = "primitive storage width";
                    valid = false;
                    break;
                }
                row->unpacked_storage_bits = (uint32_t)final->intrinsic_bits;
            }
            if (row->kind == N00B_ABI_TYPE_ATOMIC) {
                uint64_t semantic_bits = 0;
                for (size_t j = 0; j < final->n_occupied; j++) {
                    ir_layout_bit_run_t run = final->occupied[j];
                    if (run.offset > final->intrinsic_bits
                        || run.width > final->intrinsic_bits - run.offset) {
                        failed_reason = "atomic occupied range";
                        valid = false;
                        break;
                    }
                    if (run.offset + run.width > semantic_bits)
                        semantic_bits = run.offset + run.width;
                }
                if (!valid || !semantic_bits || semantic_bits > UINT32_MAX) {
                    failed_reason = "atomic semantic width";
                    valid = false;
                    break;
                }
                row->semantic_width_bits = (uint32_t)semantic_bits;
            }
        }
        else if (row->kind == N00B_ABI_TYPE_NIL)
            row->alignment_bytes = 1;
    }
    if (!valid || rows[0].kind != N00B_ABI_TYPE_NIL) {
        out->failed_type_index = failing_index < count
            ? (uint32_t)failing_index : 0;
        out->failed_reason = failed_reason;
        core_free(owners);
        core_free(rows);
        core_list_free(arguments);
        return false;
    }
    *out = (n00bb_abi_type_identity_rows_t){
        .rows = rows, .owners = owners, .count = count,
        .owner_lookup = lookup, .owner_lookup_context = lookup_context,
        .arguments = arguments.data, .argument_count = arguments.len,
    };
    return true;
}

bool
n00bb_abi_type_variables_bind_import(n00bb_abi_type_index_t *index,
    th_ctx *types, const parse_sym_entry_t *symbol, th_type *signature,
    uint32_t requirement_index, n00bb_abi_type_identity_rows_t *rows,
    const char **reason)
{
    if (reason) *reason = "invalid import type-variable binding";
    if (!index || !types || !symbol || !signature || !requirement_index
        || !rows || !rows->rows
        || rows->count != n00bb_abi_type_index_count(index)) return false;
    n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
        N00BB_SIGNATURE_TYPEVARS_KEY);
    if (variables && (variables->len > UINT32_MAX
        || (variables->len && !variables->data))) return false;
    for (size_t ordinal = 0; variables && ordinal < variables->len;
         ordinal++) {
        th_type *variable = variables->data[ordinal].type;
        if (!variable) {
            if (reason) *reason = "missing declared type variable";
            return false;
        }
        variable = th_resolve(variable);
        if (th_kind_of(variable) != TH_VAR) continue;
        uint32_t type_index = 0;
        if (!n00bb_abi_type_index_find(index, variable, &type_index))
            continue;
        n00b_abi_type_v1 *row = &rows->rows[type_index];
        if (row->kind != N00B_ABI_TYPE_VARIABLE) {
            if (reason) *reason = "declared type variable resolves to a non-variable type";
            return false;
        }
        if (row->typevar_owner_type_index) continue;
        if (row->typevar_owner_scope_index) {
            if (reason) *reason = "declared type variable has a local declaration owner";
            return false;
        }
        if (row->typevar_import_requirement_index
            && (row->typevar_import_requirement_index != requirement_index
                || row->typevar_ordinal != ordinal)) {
            if (reason) *reason = th_resolve(variable)
                == th_resolve(n00bb_abi_type_index_at(index, type_index))
                ? "declared type variable belongs to another import"
                : "distinct imported type variables share a catalog row";
            return false;
        }
        row->typevar_import_requirement_index = requirement_index;
        row->typevar_ordinal = (uint32_t)ordinal;
    }
    th_type **free_vars = nullptr;
    size_t free_count = th_free_vars(types, signature, &free_vars);
    size_t next_ordinal = variables ? variables->len : 0;
    for (size_t i = 0; i < free_count; i++) {
        uint32_t type_index = 0;
        if (!n00bb_abi_type_index_find(index, free_vars[i], &type_index)) {
            if (symbol->kind == PARSE_SYM_TYPEDEF || symbol->owner_type)
                continue;
            if (reason) *reason = "free type variable missing from catalog";
            return false;
        }
        n00b_abi_type_v1 *row = &rows->rows[type_index];
        if (row->kind != N00B_ABI_TYPE_VARIABLE
            || row->typevar_owner_scope_index) {
            if (reason) *reason = "free type variable has a declaration owner";
            return false;
        }
        if (row->typevar_owner_type_index) continue;
        if (row->typevar_import_requirement_index) {
            if (row->typevar_import_requirement_index != requirement_index) {
                if (reason) *reason = "free type variable belongs to another import";
                return false;
            }
            continue;
        }
        if (next_ordinal > UINT32_MAX) {
            if (reason) *reason = "too many free type variables";
            return false;
        }
        row->typevar_import_requirement_index = requirement_index;
        row->typevar_ordinal = (uint32_t)next_ordinal++;
    }
    return true;
}

bool
n00bb_abi_type_variables_bind(n00bb_abi_type_index_t *index, th_ctx *types,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_type_identity_rows_t *rows, const char **reason)
{
    if (reason) *reason = "invalid local type-variable binding";
    if (!index || !types || !declarations || !rows || !rows->rows
        || rows->count != n00bb_abi_type_index_count(index)) return false;
    const th_env *environment = th_ctx_environment(types);
    for (size_t i = 0; environment
         && i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration;
        if (!th_env_declaration_at(environment, i, &declaration)) return false;
        if (declaration.kind != TH_ENV_DECLARATION_TRAIT
            || !declaration.module
            || !declaration.contract
            || th_kind_of(th_resolve(declaration.contract)) != TH_TRAIT)
            continue;
        bool builtin = !strcmp(declaration.module, "builtins");
        uint32_t owner_index = 0;
        if (!n00bb_abi_type_index_find(index, declaration.contract,
                                       &owner_index)) {
            if (builtin) return false;
            continue;
        }
        th_type **variables = nullptr;
        size_t count = th_free_vars(types, declaration.contract, &variables);
        if (count > UINT32_MAX) return false;
        for (size_t ordinal = 0; ordinal < count; ordinal++) {
            uint32_t type_index = 0;
            if (!n00bb_abi_type_index_find(index, variables[ordinal],
                                           &type_index)) {
                if (builtin) return false;
                continue;
            }
            n00b_abi_type_v1 *row = &rows->rows[type_index];
            if (row->kind != N00B_ABI_TYPE_VARIABLE) return false;
            if (row->typevar_owner_scope_index
                || row->typevar_import_requirement_index) {
                if (builtin) return false;
                continue;
            }
            if (row->typevar_owner_type_index) continue;
            row->typevar_owner_type_index = owner_index;
            row->typevar_ordinal = (uint32_t)ordinal;
        }
    }
    for (size_t i = 1; i < rows->count; i++) {
        const n00b_abi_type_v1 *type_row = &rows->rows[i];
        if ((type_row->kind != N00B_ABI_TYPE_RECORD
                && type_row->kind != N00B_ABI_TYPE_VARIANT
                && type_row->kind != N00B_ABI_TYPE_ENUM
                && type_row->kind != N00B_ABI_TYPE_FLAGSET
                && type_row->kind != N00B_ABI_TYPE_CALLBACK
                && type_row->kind != N00B_ABI_TYPE_WRAPPER)
            || type_row->named_arguments.count) continue;
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        parse_sym_entry_t *owner = rows->owners[i] ? rows->owners[i]
            : rows->owner_lookup
                ? rows->owner_lookup(rows->owner_lookup_context, type)
                : nullptr;
        if (!owner || owner->kind != PARSE_SYM_TYPEDEF || !owner->type
            || core_string_eq(owner->adt_kind, CORE_STRING_STATIC("alias")))
            continue;
        uint32_t owner_index = 0;
        if (!n00bb_abi_type_index_find(index, owner->type, &owner_index)
            || owner_index != i) continue;
        th_type *definition = th_type_definition(types, owner->type);
        th_type **variables = nullptr;
        size_t count = th_free_vars(types,
            definition ? definition : owner->type, &variables);
        if (count > UINT32_MAX) return false;
        for (size_t ordinal = 0; ordinal < count; ordinal++) {
            uint32_t variable_index = 0;
            if (!n00bb_abi_type_index_find(index, variables[ordinal],
                    &variable_index)) continue;
            n00b_abi_type_v1 *row = &rows->rows[variable_index];
            if (row->kind != N00B_ABI_TYPE_VARIABLE
                || row->typevar_import_requirement_index
                || row->typevar_owner_scope_index
                || (row->typevar_owner_type_index
                    && (row->typevar_owner_type_index != owner_index
                        || row->typevar_ordinal != ordinal))) {
                if (reason) *reason = "named type variable has a conflicting owner";
                return false;
            }
            row->typevar_owner_type_index = owner_index;
            row->typevar_ordinal = (uint32_t)ordinal;
        }
    }
    for (size_t i = 1; i < n00bb_abi_decl_symbol_count(declarations); i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)i);
        if (!symbol) return false;
        n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
            N00BB_SIGNATURE_TYPEVARS_KEY);
        if (!variables || !variables->len) continue;
        if (!symbol->decl_node || !variables->data
            || variables->len > UINT32_MAX) return false;
        parse_scope_t *scope = parse_tree_scope(symbol->decl_node);
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
            if (row->typevar_owner_type_index) continue;
            if (row->kind != N00B_ABI_TYPE_VARIABLE
                || row->typevar_import_requirement_index
                || (row->typevar_owner_scope_index
                    && (row->typevar_owner_scope_index != scope_index
                        || row->typevar_ordinal != ordinal))) {
                if (reason) *reason = row->typevar_import_requirement_index
                    ? "local type variable is owned by an imported function"
                    : "local type variable has a conflicting declaration owner";
                return false;
            }
            row->typevar_owner_scope_index = scope_index;
            row->typevar_ordinal = (uint32_t)ordinal;
        }
    }
    for (size_t symbol_index = 1;
         symbol_index < n00bb_abi_decl_symbol_count(declarations);
         symbol_index++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)symbol_index);
        if (!symbol || symbol->kind != PARSE_SYM_FUNCTION
            || !symbol->decl_node) continue;
        n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
            N00BB_SIGNATURE_TYPEVARS_KEY);
        th_type *signature = n00bb_abi_type_index_symbol_type(index, symbol);
        if (!signature) return false;
        th_type **free_vars = nullptr;
        size_t free_count = th_free_vars(types, signature, &free_vars);
        size_t next_ordinal = variables ? variables->len : 0;
        uint32_t scope_index = 0;
        for (size_t i = 0; i < free_count; i++) {
            uint32_t type_index = 0;
            if (!n00bb_abi_type_index_find(index, free_vars[i], &type_index)) {
                if (reason) *reason = "local function type variable missing from catalog";
                return false;
            }
            n00b_abi_type_v1 *row = &rows->rows[type_index];
            if (row->kind != N00B_ABI_TYPE_VARIABLE) {
                if (reason) *reason = "local function variable has non-variable type";
                return false;
            }
            if (row->typevar_owner_scope_index
                || row->typevar_owner_type_index
                || row->typevar_import_requirement_index) continue;
            if (!scope_index) {
                parse_scope_t *scope = parse_tree_scope(symbol->decl_node);
                if (!n00bb_abi_decl_find_scope(declarations, scope,
                        &scope_index) || !scope_index) return false;
            }
            if (next_ordinal > UINT32_MAX) return false;
            row->typevar_owner_scope_index = scope_index;
            row->typevar_ordinal = (uint32_t)next_ordinal++;
        }
    }
    /* A body-local binding can retain an open type copied while resolving a
     * generic call. Give that copy a stable local owner as well. */
    for (size_t symbol_index = 1;
         symbol_index < n00bb_abi_decl_symbol_count(declarations);
         symbol_index++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            (uint32_t)symbol_index);
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
            || !symbol->decl_node || !symbol->type) continue;
        th_type **free_vars = nullptr;
        size_t free_count = th_free_vars(types, symbol->type, &free_vars);
        for (size_t i = 0; i < free_count; i++) {
            uint32_t type_index = 0;
            if (!n00bb_abi_type_index_find(index, free_vars[i], &type_index))
                continue;
            n00b_abi_type_v1 *row = &rows->rows[type_index];
            if (row->kind != N00B_ABI_TYPE_VARIABLE
                || row->typevar_owner_scope_index
                || row->typevar_owner_type_index
                || row->typevar_import_requirement_index) continue;
            uint32_t scope_index = 0;
            if (!n00bb_abi_decl_find_scope(declarations, symbol->scope,
                    &scope_index) || !scope_index) return false;
            uint32_t ordinal = 0;
            for (size_t j = 1; j < rows->count; j++) {
                const n00b_abi_type_v1 *other = &rows->rows[j];
                if (other->kind != N00B_ABI_TYPE_VARIABLE
                    || other->typevar_owner_scope_index != scope_index)
                    continue;
                if (other->typevar_ordinal == UINT32_MAX) return false;
                if (ordinal <= other->typevar_ordinal)
                    ordinal = other->typevar_ordinal + 1;
            }
            row->typevar_owner_scope_index = scope_index;
            row->typevar_ordinal = ordinal;
        }
    }
    return true;
}

bool
n00bb_abi_type_variables_validate(const n00bb_abi_type_identity_rows_t *rows,
                                  const char **reason)
{
    if (!rows || !rows->rows) return false;
    for (size_t i = 1; i < rows->count; i++)
        if (rows->rows[i].kind == N00B_ABI_TYPE_VARIABLE
            && !rows->rows[i].typevar_owner_scope_index
            && !rows->rows[i].typevar_owner_type_index
            && !rows->rows[i].typevar_import_requirement_index) {
            if (reason) *reason = "catalog has an unowned type variable";
            return false;
        }
    return true;
}

void
n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->owners);
    core_free(rows->arguments);
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
                       th_type *type, const parse_sym_entry_t *owner,
                       uint32_t kind,
                       core_list_t(uint32_t) *indexes)
{
    th_kind source_kind = th_kind_of(type);
    if (kind == N00B_ABI_TYPE_CALLBACK) {
        if (!owner) return true;
        if (!owner->member_scope) return false;
        const char *roles[] = {"call", "handler", "setup"};
        for (size_t i = 0; i < 3; i++) {
            parse_sym_entry_t *role = parse_symtab_lookup_in(
                owner->member_scope, core_string_from_cstr(roles[i]));
            if (!role) {
                if (i == 2) break;
                return false;
            }
            th_type *signature = n00bb_abi_type_index_symbol_type(index, role);
            if (!signature || th_kind_of(th_resolve(signature)) != TH_FUNCTION
                || !th_func_error(th_resolve(signature))
                || !append_component(index, signature, indexes)) return false;
        }
        return true;
    }
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
    if ((kind == N00B_ABI_TYPE_NAMED
         || kind == N00B_ABI_TYPE_RECORD)
        && source_kind == TH_NAMED) {
        for (size_t i = 0; i < th_named_nargs(type); i++) {
            const th_arg *arg = th_named_arg(type, i);
            if (!arg || (arg->kind == TH_ARG_TYPE
                && !append_component(index, arg->type, indexes)))
                return false;
        }
        return true;
    }
    if (kind == N00B_ABI_TYPE_VARIANT && source_kind == TH_NAMED) {
        for (size_t i = 0; i < th_named_nargs(type); i++) {
            const th_arg *arg = th_named_arg(type, i);
            if (!arg || arg->kind != TH_ARG_TYPE
                || !append_component(index, arg->type, indexes)) return false;
        }
        return true;
    }
    if (kind == N00B_ABI_TYPE_TUPLE && source_kind == TH_NAMED) {
        for (size_t i = 0; i < th_named_nargs(type); i++) {
            const th_arg *arg = th_named_arg(type, i);
            if (!arg || arg->kind != TH_ARG_TYPE
                || !append_component(index, arg->type, indexes)) return false;
        }
        return true;
    }
    if (kind == N00B_ABI_TYPE_REFERENCE
        || kind == N00B_ABI_TYPE_FOREIGN
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
                 && (kind == N00B_ABI_TYPE_REFERENCE
                     || kind == N00B_ABI_TYPE_FOREIGN))
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
            types->owners ? types->owners[i] : nullptr,
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
