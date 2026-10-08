#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_function_rows.h"
#include "abi_type_index.h"
#include "abi_type_rows.h"

typedef struct {
    n00b_abi_constraint_v1 *rows;
    size_t count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
    n00b_abi_call_shape_v1 *call_shapes;
    size_t call_shape_count;
    n00b_abi_parameter_v1 *call_parameters;
    size_t call_parameter_count;
} n00bb_abi_constraint_rows_t;

bool n00bb_abi_constraint_rows_build(
    n00bb_abi_type_index_t *index,
    n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_function_rows_t *functions,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    uint32_t parameter_base,
    n00bb_abi_constraint_rows_t *out);
void n00bb_abi_constraint_rows_release(n00bb_abi_constraint_rows_t *rows);
