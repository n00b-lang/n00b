#ifndef N00BB_ABI_PARAMETER_CONTRACT_H
#define N00BB_ABI_PARAMETER_CONTRACT_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

typedef struct {
    const void *context;
    bool (*index_at)(const void *, n00b_abi_range_v1, uint32_t, uint32_t *);
    const n00b_abi_parameter_v1 *(*parameter_at)(const void *, uint32_t);
    bool (*name_length)(const void *, const n00b_abi_string_v1 *, uint64_t *);
} n00bb_abi_parameter_reader_t;

static inline bool
n00bb_abi_parameter_contract_valid(
    const n00bb_abi_parameter_reader_t *reader,
    n00b_abi_range_v1 positional, uint32_t rest_index,
    n00b_abi_range_v1 keywords, bool allow_keyword_offsets)
{
    if (!reader || !reader->index_at || !reader->parameter_at
        || !reader->name_length
        || (uint64_t)positional.count + !!rest_index + keywords.count
            > UINT32_MAX) return false;
    for (uint32_t i = 0; i < positional.count; i++) {
        uint32_t index = 0;
        if (!reader->index_at(reader->context, positional, i, &index)
            || !index) return false;
        const n00b_abi_parameter_v1 *parameter =
            reader->parameter_at(reader->context, index);
        uint64_t length = 0;
        if (!parameter || parameter->flags
            || parameter->declaration_ordinal != i
            || parameter->keyword_block_bit_offset
            || !reader->name_length(reader->context, &parameter->name,
                                    &length) || length) return false;
    }
    if (rest_index) {
        const n00b_abi_parameter_v1 *rest = reader->parameter_at(
            reader->context, rest_index);
        uint64_t length = 0;
        if (!rest || rest->flags != N00B_ABI_PARAMETER_FLAG_VARIADIC
            || rest->declaration_ordinal != positional.count
            || rest->keyword_block_bit_offset
            || !reader->name_length(reader->context, &rest->name,
                                    &length) || length) return false;
    }
    for (uint32_t i = 0; i < keywords.count; i++) {
        uint32_t index = 0;
        if (!reader->index_at(reader->context, keywords, i, &index)
            || !index) return false;
        const n00b_abi_parameter_v1 *parameter =
            reader->parameter_at(reader->context, index);
        uint64_t length = 0;
        if (!parameter || parameter->flags
            || parameter->declaration_ordinal
                != positional.count + !!rest_index + i
            || (!allow_keyword_offsets
                && parameter->keyword_block_bit_offset)
            || !reader->name_length(reader->context, &parameter->name,
                                    &length) || !length) return false;
    }
    return true;
}

static inline bool
n00bb_abi_function_parameters_valid(
    const n00bb_abi_parameter_reader_t *reader,
    const n00b_abi_function_v1 *function)
{
    if (!reader || !reader->index_at || !reader->parameter_at || !function
        || function->keywords.count > sizeof(uint64_t) * CHAR_BIT)
        return false;
    uint64_t keyword_bits = function->keywords.count
        == sizeof(uint64_t) * CHAR_BIT
            ? UINT64_MAX : (UINT64_C(1) << function->keywords.count) - 1;
    if (function->valid_keyword_bits != keyword_bits) return false;
    uint32_t previous_ordinal = 0;
    for (uint32_t i = 0; i < function->parameters.count; i++) {
        uint32_t index = 0;
        if (!reader->index_at(reader->context, function->parameters,
                i, &index) || !index) return false;
        const n00b_abi_parameter_v1 *parameter =
            reader->parameter_at(reader->context, index);
        if (!parameter || (i && parameter->declaration_ordinal
            <= previous_ordinal)) return false;
        previous_ordinal = parameter->declaration_ordinal;
    }
    uint32_t next = 0;
    for (uint32_t i = 0; i < function->keywords.count; i++) {
        uint32_t keyword = 0, parameter = 0;
        if (!reader->index_at(reader->context, function->keywords,
                i, &keyword) || !keyword) return false;
        do {
            if (next == function->parameters.count
                || !reader->index_at(reader->context,
                    function->parameters, next++, &parameter)) return false;
        } while (parameter != keyword);
    }
    return true;
}

#endif
