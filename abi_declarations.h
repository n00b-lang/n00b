#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"
#include "lib/module.h"
#include "scanner/token_stream.h"

typedef struct {
    n00b_abi_scope_v1 *scopes;
    size_t scope_count;
    n00b_abi_symbol_v1 *symbols;
    size_t symbol_count;
    n00b_abi_source_location_v1 *locations;
    size_t location_count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
} n00bb_abi_declarations_t;

bool n00bb_abi_declarations_build(
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_type_index_t *type_index,
    n00bb_abi_type_identity_rows_t *type_rows,
    const core_translation_unit_t *unit,
    parse_token_stream_t *tokens, n00bb_abi_name_pool_t *names,
    uint32_t stream_base, n00bb_abi_declarations_t *out);
void n00bb_abi_declarations_release(n00bb_abi_declarations_t *rows);
