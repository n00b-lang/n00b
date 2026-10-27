#include "abi_constraint_rows.h"

#include <limits.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/list.h"
#include "signature_typevars.h"

core_list_decl(n00b_abi_constraint_v1);
core_list_decl(n00b_abi_call_shape_v1);
core_list_decl(n00b_abi_parameter_v1);

static bool
encode_name(n00bb_abi_name_pool_t *names, core_string_t text,
            uint32_t row_index, size_t offset, n00b_abi_string_v1 *out)
{
    if (!text.data) return false;
    return n00bb_abi_name_pool_encode(names, text, N00B_ABI_TABLE_CONSTRAINT,
        row_index, offset, out);
}

static bool
type_index(n00bb_abi_type_index_t *index, th_type *type, uint32_t *out)
{
    return !type || n00bb_abi_type_index_find(index, type, out);
}

static bool
shape_parameter(n00bb_abi_type_index_t *index,
                n00bb_abi_name_pool_t *names,
                core_list_t(n00b_abi_parameter_v1) *parameters,
                uint32_t parameter_base, th_type *type, th_mode mode,
                core_string_t name, uint32_t flags, uint32_t ordinal,
                uint32_t *out)
{
    if (!type || !out || parameters->len >= UINT32_MAX - parameter_base)
        return false;
    n00b_abi_parameter_v1 row = {
        .declaration_ordinal = ordinal,
        .flags = flags,
    };
    if (!n00bb_abi_type_index_find(index, type, &row.type_index))
        return false;
    switch (mode) {
    case TH_IN: row.passing_mode = N00B_ABI_PASS_IN; break;
    case TH_OUT: row.passing_mode = N00B_ABI_PASS_OUT; break;
    case TH_INOUT: row.passing_mode = N00B_ABI_PASS_MODIFY; break;
    case TH_CONSUME: row.passing_mode = N00B_ABI_PASS_CONSUME; break;
    case TH_VIEW: row.passing_mode = N00B_ABI_PASS_VIEW; break;
    default: return false;
    }
    uint32_t row_index = parameter_base + (uint32_t)parameters->len;
    if (name.data && (!name.u8_bytes || !n00bb_abi_name_pool_encode(names,
            name, N00B_ABI_TABLE_PARAMETER, row_index,
            offsetof(n00b_abi_parameter_v1, name), &row.name))) return false;
    core_list_push(*parameters, row);
    *out = row_index;
    return true;
}

static bool
append_call_shape(n00bb_abi_type_index_t *index,
                  n00bb_abi_name_pool_t *names, th_type *call,
                  uint32_t stream_base, uint32_t parameter_base,
                  core_list_t(n00b_abi_call_shape_v1) *shapes,
                  core_list_t(n00b_abi_parameter_v1) *parameters,
                  core_list_t(uint32_t) *stream, uint32_t *out)
{
    call = th_resolve(call);
    if (!call || th_kind_of(call) != TH_FUNCTION
        || !th_func_ret(call) || shapes->len >= UINT32_MAX
        || stream->len > UINT32_MAX - stream_base) return false;
    n00b_abi_call_shape_v1 row = {
        .failure_type_index = N00B_ABI_V1_NO_TYPE_INDEX,
    };
    if (!n00bb_abi_type_index_find(index, th_func_ret(call),
                                    &row.success_type_index)
        || !type_index(index, th_func_error(call),
                       &row.failure_type_index)) return false;
    size_t positional = th_func_nparams(call), keywords = th_func_nkw(call);
    const th_param *rest = th_func_rest(call);
    if (positional > UINT32_MAX || keywords > UINT32_MAX - positional
        || (rest && keywords == UINT32_MAX - positional)) return false;
    size_t first = stream->len;
    for (size_t i = 0; i < positional; i++) {
        const th_param *param = th_func_param(call, i);
        uint32_t parameter = 0;
        if (!param || !shape_parameter(index, names, parameters,
                parameter_base, param->type, param->mode, (core_string_t){}, 0,
                (uint32_t)i, &parameter)
            || stream->len >= UINT32_MAX - stream_base) return false;
        core_list_push(*stream, parameter);
    }
    if (stream->len != first) row.positional_parameters =
        (n00b_abi_range_v1){stream_base + (uint32_t)first,
                            (uint32_t)(stream->len - first)};
    first = stream->len;
    for (size_t i = 0; i < keywords; i++) {
        const th_kwparam *param = th_func_kw(call, i);
        uint32_t parameter = 0;
        if (!param || !param->name.data
            || !shape_parameter(index, names, parameters, parameter_base,
                param->type, param->mode, param->name, 0,
                (uint32_t)(positional + (rest ? 1 : 0) + i), &parameter)
            || stream->len >= UINT32_MAX - stream_base) return false;
        core_list_push(*stream, parameter);
    }
    if (stream->len != first) row.keyword_parameters =
        (n00b_abi_range_v1){stream_base + (uint32_t)first,
                            (uint32_t)(stream->len - first)};
    if (rest && !shape_parameter(index, names, parameters, parameter_base,
            rest->type, rest->mode, (core_string_t){},
            N00B_ABI_PARAMETER_FLAG_VARIADIC, (uint32_t)positional,
            &row.rest_parameter_index)) return false;
    *out = (uint32_t)shapes->len;
    core_list_push(*shapes, row);
    return true;
}

static bool
append_constraint(n00bb_abi_type_index_t *index,
                  n00bb_abi_name_pool_t *names, th_constraint source,
                  uint32_t ordinal, uint32_t stream_base,
                  uint32_t parameter_base,
                  core_list_t(n00b_abi_constraint_v1) *rows,
                  core_list_t(n00b_abi_call_shape_v1) *shapes,
                  core_list_t(n00b_abi_parameter_v1) *parameters,
                  core_list_t(uint32_t) *stream, uint32_t *out_index)
{
    if (rows->len >= UINT32_MAX) return false;
    uint32_t row_index = (uint32_t)rows->len;
    n00b_abi_constraint_v1 row = {
        .variable_ordinal = ordinal,
        .target_type_index = N00B_ABI_V1_NO_TYPE_INDEX,
        .result_type_index = N00B_ABI_V1_NO_TYPE_INDEX,
        .call_shape_index = 0,
    };
    if (source.has_scope) {
        row.flags = N00B_ABI_CONSTRAINT_FLAG_HAS_SCOPE;
        if (!encode_name(names, source.scope_module.data
                    ? source.scope_module : CORE_STRING_STATIC(""),
                row_index, offsetof(n00b_abi_constraint_v1,
                                    scope_module_name),
                &row.scope_module_name)
            || !encode_name(names, source.scope_package.data
                    ? source.scope_package : CORE_STRING_STATIC(""),
                row_index, offsetof(n00b_abi_constraint_v1,
                                    scope_package_name),
                &row.scope_package_name)) return false;
    }
    switch (source.kind) {
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        row.kind = source.kind == TH_CON_UNIFIES
            ? N00B_ABI_CONSTRAINT_UNIFIES
            : source.kind == TH_CON_PROMOTES
                ? N00B_ABI_CONSTRAINT_PROMOTES
                : N00B_ABI_CONSTRAINT_EXCLUDES;
        if (!source.target
            || !type_index(index, source.target, &row.target_type_index))
            return false;
        break;
    case TH_CON_ONE_OF:
        row.kind = N00B_ABI_CONSTRAINT_ONE_OF;
        if (!source.one_of.n || !source.one_of.types
            || source.one_of.n > UINT32_MAX
            || stream->len > UINT32_MAX - stream_base
            || source.one_of.n > UINT32_MAX - stream_base - stream->len)
            return false;
        row.arguments = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)stream->len,
            .count = (uint32_t)source.one_of.n,
        };
        for (size_t i = 0; i < source.one_of.n; i++) {
            uint32_t child = 0;
            if (!source.one_of.types[i]
                || !type_index(index, source.one_of.types[i], &child))
                return false;
            core_list_push(*stream, child);
        }
        break;
    case TH_CON_IMPLEMENTS:
        row.kind = N00B_ABI_CONSTRAINT_IMPLEMENTS;
        if (!source.iface.data || !source.iface.u8_bytes
            || !encode_name(names, source.iface, row_index,
                offsetof(n00b_abi_constraint_v1, name), &row.name))
            return false;
        break;
    case TH_CON_HAS_FIELD:
        row.kind = N00B_ABI_CONSTRAINT_HAS_FIELD;
        if (!source.field.name.data || !source.field.name.u8_bytes
            || !encode_name(names, source.field.name, row_index,
                offsetof(n00b_abi_constraint_v1, name), &row.name)
            || !type_index(index, source.field.type,
                           &row.target_type_index)) return false;
        break;
    case TH_CON_HAS_PARAM:
        row.kind = N00B_ABI_CONSTRAINT_HAS_PARAM;
        row.parameter_index = source.param.index;
        if (!type_index(index, source.param.type,
                        &row.target_type_index)) return false;
        break;
    case TH_CON_METHOD:
        row.kind = N00B_ABI_CONSTRAINT_METHOD;
        if (!source.method.name.data || !source.method.name.u8_bytes
            || !source.method.result
            || !encode_name(names, source.method.name, row_index,
                offsetof(n00b_abi_constraint_v1, name), &row.name)
            || !type_index(index, source.method.result,
                           &row.result_type_index)
            || (source.method.call && !append_call_shape(index, names,
                source.method.call, stream_base, parameter_base, shapes,
                parameters, stream, &row.call_shape_index))) return false;
        break;
    case TH_CON_KIND:
        row.kind = N00B_ABI_CONSTRAINT_TYPE_FAMILY;
        if (source.type_kind > TH_TRAIT) return false;
        row.type_family = (uint32_t)source.type_kind + 1;
        break;
    case TH_CON_INTEGER:
        return false;
    }
    if (!row.kind) return false;
    *out_index = row_index;
    core_list_push(*rows, row);
    return true;
}

static bool
append_membership(core_list_t(uint32_t) *stream, uint32_t stream_base,
                  core_list_t(uint32_t) *members,
                  n00b_abi_range_v1 *range)
{
    if (!members->len) return true;
    if (stream->len > UINT32_MAX - stream_base
        || members->len > UINT32_MAX - stream_base - stream->len)
        return false;
    *range = (n00b_abi_range_v1){
        .first = stream_base + (uint32_t)stream->len,
        .count = (uint32_t)members->len,
    };
    for (size_t i = 0; i < members->len; i++)
        core_list_push(*stream, members->data[i]);
    return true;
}

bool
n00bb_abi_constraint_rows_build(
    n00bb_abi_type_index_t *index,
    n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_function_rows_t *functions,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    uint32_t parameter_base,
    n00bb_abi_constraint_rows_t *out)
{
    if (!index || !types || !types->rows || !declarations || !functions
        || !functions->functions || !names || !out
        || types->count != n00bb_abi_type_index_count(index)) return false;
    core_list_t(n00b_abi_constraint_v1) rows = {0};
    core_list_t(n00b_abi_call_shape_v1) shapes = {0};
    core_list_t(n00b_abi_parameter_v1) parameters = {0};
    core_list_t(uint32_t) stream = {0};
    core_list_push(rows, ((n00b_abi_constraint_v1){0}));
    core_list_push(shapes, ((n00b_abi_call_shape_v1){0}));
    bool valid = true;
    for (size_t i = 1; valid && i < types->count; i++) {
        n00b_abi_type_v1 *type_row = &types->rows[i];
        if (type_row->kind != N00B_ABI_TYPE_VARIABLE
            || type_row->typevar_import_requirement_index) continue;
        th_type *variable = n00bb_abi_type_index_at(index, (uint32_t)i);
        if (!variable || th_kind_of(th_resolve(variable)) != TH_VAR
            || (!type_row->typevar_owner_scope_index
                && !type_row->typevar_owner_type_index
                && !type_row->typevar_import_requirement_index)) {
            valid = false;
            break;
        }
        core_list_t(uint32_t) members = {0};
        for (size_t j = 0; valid && j < th_var_constraint_count(variable); j++) {
            th_constraint constraint;
            uint32_t row_index = 0;
            valid = th_var_constraint_at(variable, j, &constraint)
                && append_constraint(index, names, constraint,
                    type_row->typevar_ordinal, stream_base,
                    parameter_base, &rows, &shapes, &parameters,
                    &stream, &row_index);
            if (valid) core_list_push(members, row_index);
        }
        if (valid) valid = append_membership(&stream, stream_base,
            &members, &type_row->constraints);
        core_list_free(members);
    }
    for (size_t i = 1; valid && i < functions->function_count; i++) {
        n00b_abi_function_v1 *function = &functions->functions[i];
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
            function->symbol_index);
        n00bb_declared_where_t *declared = symbol
            ? parse_sym_binding(symbol, N00BB_DECLARED_WHERE_KEY) : nullptr;
        if (!declared || !declared->len) continue;
        n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
            N00BB_SIGNATURE_TYPEVARS_KEY);
        if (!variables || !variables->data || !declared->data) {
            valid = false;
            break;
        }
        core_list_t(uint32_t) members = {0};
        for (size_t j = 0; valid && j < declared->len; j++) {
            th_trait_requirement *requirement = &declared->data[j];
            if (th_kind_of(th_resolve(requirement->subject)) != TH_VAR)
                continue;
            size_t ordinal = variables->len;
            for (size_t k = 0; k < variables->len; k++)
                if (th_resolve(requirement->subject)
                    == th_resolve(variables->data[k].type)) {
                    ordinal = k;
                    break;
                }
            if (ordinal >= variables->len || ordinal > UINT32_MAX) {
                valid = false;
                break;
            }
            th_constraint constraint = {.kind = requirement->kind};
            if (constraint.kind == TH_CON_IMPLEMENTS)
                constraint.iface = requirement->iface;
            else if (constraint.kind == TH_CON_EXCLUDES)
                constraint.target = requirement->target;
            else { valid = false; break; }
            uint32_t row_index = 0;
            valid = append_constraint(index, names, constraint,
                (uint32_t)ordinal, stream_base, parameter_base,
                &rows, &shapes, &parameters, &stream, &row_index);
            if (valid) core_list_push(members, row_index);
        }
        if (valid) valid = append_membership(&stream, stream_base,
            &members, &function->constraints);
        core_list_free(members);
    }
    if (valid) *out = (n00bb_abi_constraint_rows_t){
        .rows = rows.data, .count = rows.len,
        .stream_indexes = stream.data, .stream_index_count = stream.len,
        .call_shapes = shapes.data, .call_shape_count = shapes.len,
        .call_parameters = parameters.data,
        .call_parameter_count = parameters.len,
    };
    else {
        core_list_free(rows);
        core_list_free(shapes);
        core_list_free(parameters);
        core_list_free(stream);
    }
    return valid;
}

void
n00bb_abi_constraint_rows_release(n00bb_abi_constraint_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->rows);
    core_free(rows->call_shapes);
    core_free(rows->call_parameters);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_constraint_rows_t){0};
}
