#include <assert.h>
#include <string.h>

#include "abi_string.h"
#include "abi_type_signatures.h"
#include "ir/typelang.h"
#include "lib/alloc.h"

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = CORE_STRING_STATIC("n00b")};
    ir_module_t *module = ir_module_new(context,
                                        CORE_STRING_STATIC("example"),
                                        &profile,
                                        CORE_STRING_STATIC("x86_64-linux"));
    assert(module);
    th_ctx *types = ir_ctx_types(context);
    th_type *nil = th_name(types, CORE_STRING_STATIC("nil"));
    th_type *string = th_name(types, CORE_STRING_STATIC("string"));
    th_type *number = th_bitint(types, 32, true);
    th_param positional[] = {
        {.mode = TH_IN, .type = string},
        {.mode = TH_OUT, .type = number},
    };
    th_param rest = {.mode = TH_CONSUME, .type = string};
    const core_string_t keyword_name =
        CORE_STRING_STATIC("extraordinary_named_argument");
    th_kwparam keywords[] = {{
        .name = keyword_name, .mode = TH_INOUT, .type = number,
    }};
    th_type *failable = th_func_with_error(types, nil, string,
        positional, 2, &rest, keywords, 1);
    th_type *infallible = th_func_with_error(types, string, nil,
        nullptr, 0, nullptr, nullptr, 0);
    n00bb_abi_type_index_t *index = n00bb_abi_type_index_new(module);
    uint32_t failable_index = 0, infallible_index = 0;
    assert(failable && infallible && index
        && n00bb_abi_type_index_add(index, failable, &failable_index)
        && n00bb_abi_type_index_add(index, infallible,
                                      &infallible_index));
    n00bb_abi_name_pool_t *names = n00bb_abi_name_pool_new();
    n00bb_abi_type_identity_rows_t types_rows = {0};
    assert(names && n00bb_abi_type_identity_rows_build(index,
        module, false, nullptr, nullptr, nullptr, names, &types_rows));
    n00bb_abi_type_signatures_t rows = {0};
    assert(n00bb_abi_type_signatures_build(index, &types_rows, names, 7,
                                            &rows));
    assert(rows.signature_count == 3 && rows.parameter_count == 5
        && rows.parameter_index_count == 3);
    uint32_t signature_index = types_rows.rows[failable_index].call_signature_index;
    assert(signature_index && rows.signatures[signature_index].type_index
        == failable_index);
    const n00b_abi_call_signature_v1 *signature =
        &rows.signatures[signature_index];
    uint32_t string_index = 0, number_index = 0;
    assert(n00bb_abi_type_index_find(index, string, &string_index)
        && n00bb_abi_type_index_find(index, number, &number_index));
    assert(signature->success_type_index == 0
        && signature->failure_type_index == string_index
        && signature->positional_parameters.first == 7
        && signature->positional_parameters.count == 2
        && signature->keyword_parameters.first == 9
        && signature->keyword_parameters.count == 1);
    const n00b_abi_parameter_v1 *first =
        &rows.parameters[rows.parameter_indexes[0]];
    const n00b_abi_parameter_v1 *second =
        &rows.parameters[rows.parameter_indexes[1]];
    const n00b_abi_parameter_v1 *keyword =
        &rows.parameters[rows.parameter_indexes[2]];
    const n00b_abi_parameter_v1 *variadic =
        &rows.parameters[signature->rest_parameter_index];
    assert(first->type_index == string_index
        && first->passing_mode == N00B_ABI_PASS_IN
        && first->declaration_ordinal == 0
        && second->type_index == number_index
        && second->passing_mode == N00B_ABI_PASS_OUT
        && second->declaration_ordinal == 1
        && keyword->type_index == number_index
        && keyword->passing_mode == N00B_ABI_PASS_MODIFY
        && keyword->declaration_ordinal == 3
        && variadic->type_index == string_index
        && variadic->passing_mode == N00B_ABI_PASS_CONSUME
        && variadic->declaration_ordinal == 2
        && variadic->flags == N00B_ABI_PARAMETER_FLAG_VARIADIC
        && !variadic->keyword_block_bit_offset);
    n00bb_abi_string_header_t decoded;
    assert(n00bb_abi_string_decode((const uint8_t *)&keyword->name,
        sizeof(keyword->name), &decoded)
        && decoded.tag == N00BB_ABI_STRING_LONG
        && decoded.length == keyword_name.u8_bytes);
    n00bb_abi_directory_entry_t directory[] = {
        {.kind = N00B_ABI_TABLE_TYPE,
         .record_bytes = sizeof(n00b_abi_type_v1),
         .record_count = (uint32_t)types_rows.count, .file_offset = 64},
        {.kind = N00B_ABI_TABLE_PARAMETER,
         .record_bytes = sizeof(n00b_abi_parameter_v1),
         .record_count = (uint32_t)rows.parameter_count,
         .file_offset = 4096},
    };
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t relocation_count = 0;
    assert(n00bb_abi_name_pool_relocations(names, directory, 2,
        &relocations, &relocation_count));
    bool keyword_relocated = false;
    uint64_t keyword_offset = directory[1].file_offset
        + (uint64_t)rows.parameter_indexes[2] * sizeof(n00b_abi_parameter_v1)
        + offsetof(n00b_abi_parameter_v1, name);
    for (size_t i = 0; i < relocation_count; i++) {
        if (relocations[i].section_offset != keyword_offset) continue;
        keyword_relocated = relocations[i].addend_bits
            <= n00bb_abi_name_pool_size(names) - keyword_name.u8_bytes
            && !memcmp(n00bb_abi_name_pool_bytes(names)
                    + relocations[i].addend_bits,
                keyword_name.data, keyword_name.u8_bytes);
    }
    assert(keyword_relocated);
    core_free(relocations);
    const n00b_abi_call_signature_v1 *simple = &rows.signatures[
        types_rows.rows[infallible_index].call_signature_index];
    assert(simple->success_type_index == string_index
        && simple->failure_type_index == 0
        && !simple->positional_parameters.count
        && !simple->keyword_parameters.count
        && !simple->rest_parameter_index);
    n00bb_abi_type_signatures_release(&rows);
    n00bb_abi_type_identity_rows_release(&types_rows);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(index);

    th_type *missing_failure = th_func(types, nil, nullptr, 0,
                                       nullptr, nullptr, 0);
    n00bb_abi_type_index_t *invalid = n00bb_abi_type_index_new(module);
    uint32_t invalid_index = 0;
    assert(missing_failure && invalid
        && n00bb_abi_type_index_add(invalid, missing_failure,
                                      &invalid_index));
    names = n00bb_abi_name_pool_new();
    types_rows = (n00bb_abi_type_identity_rows_t){0};
    rows = (n00bb_abi_type_signatures_t){0};
    assert(n00bb_abi_type_identity_rows_build(invalid,
        module, false, nullptr, nullptr, nullptr, names, &types_rows));
    assert(!n00bb_abi_type_signatures_build(invalid, &types_rows,
                                            names, 7, &rows));
    assert(types_rows.rows[invalid_index].call_signature_index == 0
        && !rows.signatures && !rows.parameters && !rows.parameter_indexes);
    n00bb_abi_type_identity_rows_release(&types_rows);
    n00bb_abi_name_pool_free(names);
    n00bb_abi_type_index_free(invalid);
    ir_ctx_free(context);
    return 0;
}
