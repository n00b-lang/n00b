#include "abi_function_rows.h"

#include <limits.h>
#include <string.h>

#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(n00b_abi_function_v1);
core_list_decl(uint32_t);
typedef ir_func_t *ir_func_ptr_t;
core_list_decl(ir_func_ptr_t);

static bool
function_symbol_needs_qualified_name(th_ctx *types, parse_sym_entry_t *symbol)
{
    uint64_t signature = types && symbol && symbol->type
        ? th_hash(types, symbol->type) : 0;
    if (!signature) return false;
    for (parse_sym_entry_t *candidate = symbol && symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         candidate; candidate = candidate->next_in_scope)
        if (candidate != symbol && candidate->kind == PARSE_SYM_FUNCTION
            && core_string_eq(candidate->name, symbol->name)
            && candidate->type
            && th_hash(types, candidate->type) == signature) return true;
    return false;
}

static bool
append_module_initializer(n00bb_abi_type_index_t *index,
                          const n00bb_abi_type_identity_rows_t *types,
                          const n00bb_abi_decl_index_t *declarations,
                          ir_module_t *module, n00bb_abi_name_pool_t *names,
                          core_list_t(n00b_abi_function_v1) *rows,
                          core_list_t(ir_func_ptr_t) *bindings,
                          uint32_t *initializer_index)
{
    uint32_t scope_index = n00bb_abi_decl_module_scope_index(declarations);
    for (size_t i = 0; i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at(module, i);
        if (!function) return false;
        if (!ir_func_is_initializer(function)) continue;
        if (!scope_index || *initializer_index || !function->name.data
            || !function->type
            || function->owner != module || function->n_params
            || th_func_nparams(function->type)
            || th_func_nkw(function->type)
            || th_func_rest(function->type) || rows->len >= UINT32_MAX)
            return false;
        th_type *success = th_func_ret(function->type);
        th_type *failure = th_func_error(function->type);
        uint32_t success_index, failure_index;
        if (!success || !failure
            || !n00bb_abi_type_index_find(index, success, &success_index)
            || !n00bb_abi_type_index_find(index, failure, &failure_index)
            || success_index >= types->count || failure_index >= types->count)
            return false;
        n00b_abi_function_v1 row = {
            .module_index = 1,
            .scope_index = scope_index,
            .success_type_index = success_index,
            .failure_type_index = failure_index,
            .variadic_type_index = N00B_ABI_V1_NO_TYPE_INDEX,
        };
        uint32_t next = (uint32_t)rows->len;
        if (!n00bb_abi_name_pool_encode(names, function->name,
                N00B_ABI_TABLE_FUNCTION, next,
                offsetof(n00b_abi_function_v1, name), &row.name))
            return false;
        core_list_push(*rows, row);
        core_list_push(*bindings, function);
        *initializer_index = next;
    }
    return true;
}

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
    ir_module_t *module, n00bb_abi_name_pool_t *names,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_function_rows_t *out)
{
    if (!index || !declarations || !types || !types->rows || !signatures
        || !signatures->signatures || !signatures->parameters
        || !module || !names || !out) return false;
    core_list_t(n00b_abi_function_v1) rows = {0};
    core_list_t(ir_func_ptr_t) bindings = {0};
    core_list_t(uint32_t) stream = {0};
    core_list_push(rows, ((n00b_abi_function_v1){0}));
    core_list_push(bindings, nullptr);
    bool valid = true;
    uint32_t initializer_index = 0;
    for (size_t i = 1; valid && i < n00bb_abi_decl_symbol_count(declarations);
         i++) {
        parse_sym_entry_t *symbol = n00bb_abi_decl_symbol_at(declarations,
                                                              (uint32_t)i);
        if (!symbol || symbol->kind != PARSE_SYM_FUNCTION
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.generated-extern"))) continue;
        if (symbol->owner_type
            && (core_string_eq(symbol->owner_type->adt_kind,
                               CORE_STRING_STATIC("trait"))
                || core_string_eq(symbol->owner_type->adt_kind,
                                  CORE_STRING_STATIC("callback"))
                || (symbol->owner_type->member_scope
                    && core_string_eq(
                        symbol->owner_type->member_scope->scope_tag,
                        CORE_STRING_STATIC("trait"))))) continue;
        core_link_declaration_t *declaration = lookup
            ? lookup(lookup_context, symbol) : nullptr;
        ir_func_t *function = declaration ? core_link_declaration_binding(
            declaration, CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
        uint32_t type_index = 0, scope_index = 0;
        uint64_t ir_signature_hash = function && function->type
            ? th_hash(ir_module_types(module), function->type) : 0;
        uint64_t source_signature_hash = symbol->type
            ? th_hash(ir_module_types(module), symbol->type) : 0;
        if (!function || function->owner != module
            || !ir_signature_hash
            || ir_signature_hash != source_signature_hash
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
            .variadic_type_index = N00B_ABI_V1_NO_TYPE_INDEX,
        };
        core_string_t row_name = (parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.litmod.declaration"))
            || function_symbol_needs_qualified_name(
                ir_module_types(module), symbol))
            ? function->name : symbol->name;
        valid = n00bb_abi_name_pool_encode(names, row_name,
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
        if (valid) {
            core_list_push(rows, row);
            core_list_push(bindings, function);
        }
    }
    if (valid) valid = append_module_initializer(index, types, declarations,
        module, names, &rows, &bindings, &initializer_index);
    if (valid) *out = (n00bb_abi_function_rows_t){
        .functions = rows.data, .ir_functions = bindings.data,
        .function_count = rows.len,
        .stream_indexes = stream.data, .stream_index_count = stream.len,
        .initializer_index = initializer_index,
    };
    else {
        core_list_free(rows);
        core_list_free(bindings);
        core_list_free(stream);
    }
    return valid;
}

void
n00bb_abi_function_rows_release(n00bb_abi_function_rows_t *rows)
{
    if (!rows) return;
    core_free(rows->functions);
    core_free(rows->ir_functions);
    core_free(rows->stream_indexes);
    *rows = (n00bb_abi_function_rows_t){0};
}
