#pragma once

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_type_rows.h"

typedef struct {
    n00b_abi_enum_item_v1 *items;
    size_t item_count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
} n00bb_abi_enum_rows_t;

bool n00bb_abi_enum_rows_build(n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_name_pool_t *names, uint32_t stream_base,
    n00bb_abi_type_identity_rows_t *types, n00bb_abi_enum_rows_t *out);
void n00bb_abi_enum_rows_release(n00bb_abi_enum_rows_t *rows);
