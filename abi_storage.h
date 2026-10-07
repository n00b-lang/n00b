#pragma once

#include "abi_decl_index.h"
#include "abi_function_rows.h"
#include "abi_type_index.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"
#include "lib/buffer.h"

typedef struct {
    n00b_abi_storage_v1 *rows;
    size_t row_count;
    uint32_t *stream_indexes;
    size_t stream_count;
    n00b_abi_range_v1 static_range;
    n00b_abi_range_v1 tls_range;
    core_buffer_t read_only;
    core_buffer_t writable;
    core_buffer_t tls;
    uint32_t read_only_alignment;
    uint32_t writable_alignment;
    uint32_t tls_alignment;
} n00bb_abi_storage_rows_t;

bool n00bb_abi_storage_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    ir_module_t *runtime, uint32_t stream_base,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_storage_rows_t *out);
void n00bb_abi_storage_rows_release(n00bb_abi_storage_rows_t *rows);
