#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi_type_index.h"
#include "abi_type_rows.h"
#include "abi_decl_index.h"

typedef struct {
    n00b_abi_layout_v1 *layouts;
    size_t layout_count;
    n00b_abi_physical_run_v1 *runs;
    size_t run_count;
    n00b_abi_packed_run_v1 *packed_runs;
    size_t packed_run_count;
    n00b_abi_field_v1 *fields;
    size_t field_count;
    n00b_abi_arm_v1 *arms;
    size_t arm_count;
    n00b_abi_tag_extractor_v1 *tag_extractors;
    size_t tag_extractor_count;
    n00b_abi_trace_v1 *traces;
    size_t trace_count;
    n00b_abi_trace_child_v1 *trace_children;
    size_t trace_child_count;
    n00b_abi_trace_arm_v1 *trace_arms;
    size_t trace_arm_count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
} n00bb_abi_type_layout_rows_t;

/* Appends finalized physical layouts to type identity rows. `stream_base`
 * reserves earlier U32-stream entries. Record fields use the same finalized
 * layout and the TU's indexed symbol declarations. */
bool n00bb_abi_type_layout_rows_build(
    n00bb_abi_type_index_t *index,
    ir_module_t *runtime, bool include_external_definitions,
    uint32_t stream_base,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_name_pool_t *names,
    n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_layout_rows_t *out, uint32_t *failed_type_index);
void n00bb_abi_type_layout_rows_release(
    n00bb_abi_type_layout_rows_t *rows);
