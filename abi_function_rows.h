#pragma once

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_type_signatures.h"
#include "lib/link_unit.h"

typedef core_link_declaration_t *(*n00bb_abi_declaration_lookup_fn)(
    void *context, parse_sym_entry_t *symbol);

typedef struct {
    n00b_abi_function_v1 *functions;
    ir_func_t **ir_functions;
    size_t function_count;
    uint32_t *stream_indexes;
    size_t stream_index_count;
    uint32_t comptime_initializer_index;
    uint32_t runtime_initializer_index;
} n00bb_abi_function_rows_t;

bool n00bb_abi_function_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_type_signatures_t *signatures,
    uint32_t signature_stream_base, uint32_t stream_base,
    ir_module_t *comptime, ir_module_t *runtime,
    n00bb_abi_name_pool_t *names,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    n00bb_abi_function_rows_t *out);
void n00bb_abi_function_rows_release(n00bb_abi_function_rows_t *rows);
