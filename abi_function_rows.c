#include "abi_function_rows.h"

#include <limits.h>

#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(n00b_abi_function_v1);
core_list_decl(uint32_t);

static bool
append_parameter_range(const n00bb_abi_type_signatures_t *signatures,
                       n00b_abi_range_v1 source, uint32_t source_base,
                       uint32_t stream_base, core_list_t(uint32_t) *stream,
                       n00b_abi_range_v1 *result)
{
    if (!source.count) return true;
    if (source.first < source_base
        || source.first - source_base > signatures->parameter_index_count
        || source.count > signatures->parameter_index_count
            - (source.first - source_base)
        || stream->len > UINT32_MAX - stream_base
        || source.count > UINT32_MAX - stream_base - stream->len)
        return false;
    *result = (n00b_abi_range_v1){
        .first = stream_base + (uint32_t)stream->len,
        .count = source.count,
    };
    for (uint32_t i = 0; i < source.count; i++) {
        uint32_t parameter = signatures->parameter_indexes[
            source.first - source_base + i];
        if (!parameter || parameter >= signatures->parameter_count)
            return false;
        core_list_push(*stream, parameter);
    }
    return true;
}

bool
n00bb_abi_function_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_type_signatures_t *signatures,
    uint32_t signature_stream_base, uint32_t stream_base,
    ir_module_t *comptime, ir_module_t *runtime,
    n00bb_abi_name_pool_t *names,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_function_rows_t *out)
{
    if (!index || !declarations || !types || !types->rows || !signatures
        || !signatures->signatures || !signatures->parameters
        || !comptime || !runtime || !names || !out) return false;
    core_list_t(n00b_abi_function_v1) rows = {0};
    core_list_t(uint32_t) stream = {0};
    core_list_push(rows, ((n00b_abi_function_v1){0}));
    bool valid = true;
    for (size_t i = 1; valid && i < n00bb_abi_decl_symbol_count(declarations);
         i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                              (uint32_t)i);
        if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) continue;
        core_link_declaration_t *declaration = lookup
            ? lookup(lookup_context, symbol) : nullptr;
        ir_func_t *function = declaration ? core_link_declaration_binding(
            declaration, CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
        ir_func_t *compile_function = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.ir.comptime.function")) : nullptr;
        uint32_t type_index = 0, scope_index = 0;
        if (!function || function->owner != runtime
            || !ir_type_eq(runtime, function->type, symbol->type)
            || (compile_function && compile_function->owner != comptime)
            || !n00bb_abi_type_index_find(index, symbol->type, &type_index)
            || type_index >= types->count
            || types->rows[type_index].kind != N00B_ABI_TYPE_FUNCTION
            || !n00bb_abi_decl_find_scope(declarations, symbol->scope,
                                           &scope_index)
            || rows.len >= UINT32_MAX) {
            valid = false;
            break;
        }
        uint32_t signature_index = types->rows[type_index].call_signature_index;
        if (!signature_index || signature_index >= signatures->signature_count) {
            valid = false;
            break;
        }
        const n00b_abi_call_signature_v1 *signature =
            &signatures->signatures[signature_index];
        if (signature->type_index != type_index
            || signature->keyword_parameters.count
                > sizeof(uint64_t) * CHAR_BIT) {
            valid = false;
            break;
        }
        n00b_abi_function_v1 row = {
            .module_index = 1,
            .scope_index = scope_index,
            .symbol_index = (uint32_t)i,
            .success_type_index = signature->success_type_index,
            .failure_type_index = signature->failure_type_index,
        };
        valid = n00bb_abi_name_pool_encode(names, symbol->name,
            N00B_ABI_TABLE_FUNCTION, (uint32_t)rows.len,
            offsetof(n00b_abi_function_v1, name), &row.name);
        size_t first = stream.len;
        n00b_abi_range_v1 positional = {0};
        if (valid) valid = append_parameter_range(signatures,
            signature->positional_parameters, signature_stream_base,
            stream_base, &stream, &positional);
        if (valid && signature->rest_parameter_index) {
            uint32_t rest = signature->rest_parameter_index;
            if (rest >= signatures->parameter_count
                || stream.len >= UINT32_MAX - stream_base)
                valid = false;
            else {
                row.variadic_type_index = signatures->parameters[rest].type_index;
                core_list_push(stream, rest);
            }
        }
        if (valid) valid = append_parameter_range(signatures,
            signature->keyword_parameters, signature_stream_base,
            stream_base, &stream, &row.keywords);
        if (valid && stream.len != first) row.parameters =
            (n00b_abi_range_v1){
                .first = stream_base + (uint32_t)first,
                .count = (uint32_t)(stream.len - first),
            };
        if (valid && row.keywords.count) row.valid_keyword_bits =
            row.keywords.count == sizeof(uint64_t) * CHAR_BIT
                ? UINT64_MAX : (UINT64_C(1) << row.keywords.count) - 1;
        if (valid) core_list_push(rows, row);
    }
    if (valid) *out = (n00bb_abi_function_rows_t){
        .functions = rows.data, .function_count = rows.len,
        .stream_indexes = stream.data, .stream_index_count = stream.len,
    };
    else {
        core_list_free(rows);
        core_list_free(stream);
    }
    return valid;
}

void
n00bb_abi_function_rows_release(n00bb_abi_function_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->functions);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_function_rows_t){0};
}
