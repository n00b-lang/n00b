#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_decl_index.h"
#include "abi_type_index.h"
#include "abi_type_rows.h"

typedef struct {
    n00b_abi_trait_method_v1 *rows;
    size_t count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
} n00bb_abi_trait_method_rows_t;

bool n00bb_abi_trait_method_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_trait_method_rows_t *out);
bool n00bb_abi_trait_method_rows_attach(
    n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_trait_method_rows_t *methods, uint32_t stream_base);
void n00bb_abi_trait_method_rows_release(
    n00bb_abi_trait_method_rows_t *rows);
