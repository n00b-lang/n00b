#include "abi_type_signatures.h"

#include <limits.h>
#include <string.h>

#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(n00b_abi_call_signature_v1);
core_list_decl(n00b_abi_parameter_v1);

static bool
passing_mode(th_mode mode, uint32_t *out)
{
    switch (mode) {
    case TH_IN: *out = N00B_ABI_PASS_IN; return true;
    case TH_OUT: *out = N00B_ABI_PASS_OUT; return true;
    case TH_INOUT: *out = N00B_ABI_PASS_MODIFY; return true;
    case TH_CONSUME: *out = N00B_ABI_PASS_CONSUME; return true;
    case TH_VIEW: *out = N00B_ABI_PASS_VIEW; return true;
    }
    return false;
}

static bool
add_parameter(n00bb_abi_type_index_t *index, n00bb_abi_name_pool_t *names,
              core_list_t(n00b_abi_parameter_v1) *parameters,
              th_type *type, th_mode mode, core_string_t name, uint32_t flags,
              uint32_t formal_ordinal,
              uint32_t *out_index)
{
    if (!type || !out_index || parameters->len >= UINT32_MAX) return false;
    n00b_abi_parameter_v1 row = {.flags = flags};
    if (!n00bb_abi_type_index_find(index, type, &row.type_index)
        || !passing_mode(mode, &row.passing_mode)) return false;
    uint32_t row_index = (uint32_t)parameters->len;
    row.declaration_ordinal = formal_ordinal;
    if (name.data && (!name.u8_bytes
        || !n00bb_abi_name_pool_encode(names, name,
            N00B_ABI_TABLE_PARAMETER, row_index,
            offsetof(n00b_abi_parameter_v1, name), &row.name))) return false;
    core_list_push(*parameters, row);
    *out_index = row_index;
    return true;
}

static bool
add_stream_index(core_list_t(uint32_t) *stream, uint32_t stream_base,
                 uint32_t index)
{
    if (stream->len >= UINT32_MAX - stream_base) return false;
    core_list_push(*stream, index);
    return true;
}

static bool
add_signature(n00bb_abi_type_index_t *index, th_type *type,
              n00bb_abi_name_pool_t *names, uint32_t stream_base,
              uint32_t type_index,
              core_list_t(n00b_abi_call_signature_v1) *signatures,
              core_list_t(n00b_abi_parameter_v1) *parameters,
              core_list_t(uint32_t) *stream,
              uint32_t *out_signature_index)
{
    if (!type || th_kind_of(type) != TH_FUNCTION
        || !th_func_ret(type) || !th_func_error(type)
        || signatures->len >= UINT32_MAX) return false;
    n00b_abi_call_signature_v1 row = {.type_index = type_index};
    if (!n00bb_abi_type_index_find(index, th_func_ret(type),
                                    &row.success_type_index)
        || !n00bb_abi_type_index_find(index, th_func_error(type),
                                       &row.failure_type_index)) return false;
    size_t npos = th_func_nparams(type);
    size_t nkw = th_func_nkw(type);
    const th_param *rest = th_func_rest(type);
    if (npos > UINT32_MAX || nkw > UINT32_MAX - npos
        || (rest && nkw == UINT32_MAX - npos)) return false;
    size_t first = stream->len;
    for (size_t i = 0; i < npos; i++) {
        const th_param *param = th_func_param(type, i);
        uint32_t param_index = 0;
        if (!param || !add_parameter(index, names, parameters,
                param->type, param->mode, (core_string_t){}, 0, (uint32_t)i,
                &param_index)
            || !add_stream_index(stream, stream_base, param_index))
            return false;
    }
    if (stream->len != first)
        row.positional_parameters = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)first,
            .count = (uint32_t)(stream->len - first),
        };
    first = stream->len;
    for (size_t i = 0; i < nkw; i++) {
        const th_kwparam *param = th_func_kw(type, i);
        uint32_t param_index = 0;
        if (!param || !param->name.data
            || !add_parameter(index, names, parameters,
                param->type, param->mode, param->name, 0,
                (uint32_t)(npos + (rest ? 1 : 0) + i), &param_index)
            || !add_stream_index(stream, stream_base, param_index))
            return false;
    }
    if (stream->len != first)
        row.keyword_parameters = (n00b_abi_range_v1){
            .first = stream_base + (uint32_t)first,
            .count = (uint32_t)(stream->len - first),
        };
    if (rest && !add_parameter(index, names, parameters,
            rest->type, rest->mode, (core_string_t){},
            N00B_ABI_PARAMETER_FLAG_VARIADIC, (uint32_t)npos,
            &row.rest_parameter_index))
        return false;
    *out_signature_index = (uint32_t)signatures->len;
    core_list_push(*signatures, row);
    return true;
}

bool
n00bb_abi_type_signatures_build(
    n00bb_abi_type_index_t *index, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    n00bb_abi_type_signatures_t *out)
{
    if (!index || !types || !types->rows || !names || !out
        || types->count != n00bb_abi_type_index_count(index)) return false;
    core_list_t(n00b_abi_call_signature_v1) signatures = {0};
    core_list_t(n00b_abi_parameter_v1) parameters = {0};
    core_list_t(uint32_t) stream = {0};
    uint32_t *type_signatures = core_alloc_array(uint32_t, types->count);
    core_list_push(signatures, ((n00b_abi_call_signature_v1){0}));
    core_list_push(parameters, ((n00b_abi_parameter_v1){0}));
    bool valid = true;
    for (size_t i = 0; valid && i < types->count; i++) {
        th_type *type = n00bb_abi_type_index_at(index, (uint32_t)i);
        if (!type) {
            valid = false;
            break;
        }
        if (types->rows[i].kind != N00B_ABI_TYPE_FUNCTION) {
            if (th_kind_of(type) == TH_FUNCTION) valid = false;
            continue;
        }
        valid = add_signature(index, type, names, stream_base,
            (uint32_t)i, &signatures, &parameters, &stream,
            &type_signatures[i]);
    }
    if (valid) {
        for (size_t i = 0; i < types->count; i++)
            types->rows[i].call_signature_index = type_signatures[i];
        *out = (n00bb_abi_type_signatures_t){
            .signatures = signatures.data,
            .signature_count = signatures.len,
            .parameters = parameters.data,
            .parameter_count = parameters.len,
            .parameter_indexes = stream.data,
            .parameter_index_count = stream.len,
        };
    }
    else {
        core_list_free(signatures);
        core_list_free(parameters);
        core_list_free(stream);
    }
    core_free(type_signatures);
    return valid;
}

void
n00bb_abi_type_signatures_release(n00bb_abi_type_signatures_t *rows)
{
    if (!rows) return;
    core_free(rows->signatures);
    core_free(rows->parameters);
    core_free(rows->parameter_indexes);
    *rows = (n00bb_abi_type_signatures_t){0};
}
