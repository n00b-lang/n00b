#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_emit_strings.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"

typedef struct {
    n00b_abi_call_signature_v1 *signatures;
    size_t signature_count;
    n00b_abi_parameter_v1 *parameters;
    size_t parameter_count;
    uint32_t *parameter_indexes;
    size_t parameter_index_count;
} n00bb_abi_type_signatures_t;

bool n00bb_abi_type_signatures_build(
    n00bb_abi_type_index_t *index, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    n00bb_abi_type_signatures_t *out);
void n00bb_abi_type_signatures_release(n00bb_abi_type_signatures_t *rows);
