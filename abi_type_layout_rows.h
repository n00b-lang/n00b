#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi_type_index.h"
#include "abi_type_rows.h"

typedef struct {
    n00b_abi_layout_v1 *layouts;
    size_t layout_count;
    n00b_abi_physical_run_v1 *runs;
    size_t run_count;
    uint32_t *run_indexes;
    size_t run_index_count;
} n00bb_abi_type_layout_rows_t;

/* Appends finalized physical layouts to type identity rows. `stream_base`
 * reserves earlier U32-stream entries such as package membership. Field,
 * tag, and tracing tables are supplied by the complete emitter. */
bool n00bb_abi_type_layout_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime, uint32_t stream_base,
    n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_layout_rows_t *out);
void n00bb_abi_type_layout_rows_release(
    n00bb_abi_type_layout_rows_t *rows);
