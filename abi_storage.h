#pragma once

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_function_rows.h"
#include "abi_type_index.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"
#include "lib/buffer.h"

typedef struct {
    uint32_t source_kind;
    uint64_t byte_offset;
    uint32_t target_kind;
    uint64_t target_offset;
} n00bb_abi_storage_relocation_t;

enum {
    N00BB_ABI_RELOC_AGGREGATE = 3,
    N00BB_ABI_RELOC_STRING_POOL = 4,
};

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
    core_buffer_t aggregates;
    uint32_t read_only_alignment;
    uint32_t writable_alignment;
    uint32_t tls_alignment;
    uint32_t aggregate_alignment;
    n00b_abi_shape_v1 *shapes;
    size_t shape_count;
    uint64_t *extents;
    size_t extent_count;
    n00bb_abi_storage_relocation_t *relocations;
    size_t relocation_count;
} n00bb_abi_storage_rows_t;

bool n00bb_abi_storage_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    ir_module_t *runtime, n00bb_abi_name_pool_t *pool,
    uint32_t stream_base,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_storage_rows_t *out);
void n00bb_abi_storage_rows_release(n00bb_abi_storage_rows_t *rows);
