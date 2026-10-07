#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_emit_strings.h"
#include "abi_type_index.h"
#include "n00b-abi-v1.h"
#include "parse/symtab.h"

typedef struct {
    n00b_abi_type_v1 *rows;
    parse_sym_entry_t **owners;
    size_t count;
} n00bb_abi_type_identity_rows_t;

typedef struct {
    uint32_t *indexes;
    size_t count;
} n00bb_abi_type_components_t;

typedef parse_sym_entry_t *(*n00bb_abi_type_owner_lookup_fn)(
    void *context, th_type *type);

/* Populates identity, category and finalized extent facts. Relationship
 * tables (fields, constraints, signatures and layout runs) are deliberately
 * left for the complete type emitter; these rows are not standalone output. */
bool n00bb_abi_type_identity_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime,
    const parse_symtab_t *symbols,
    n00bb_abi_type_owner_lookup_fn lookup, void *lookup_context,
    n00bb_abi_name_pool_t *names,
    n00bb_abi_type_identity_rows_t *out);
void n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows);

/* Adds TYPE component ranges to the shared U32 stream at stream_base. */
bool n00bb_abi_type_components_build(
    n00bb_abi_type_index_t *index, ir_module_t *module,
    uint32_t stream_base, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_components_t *out);
void n00bb_abi_type_components_release(n00bb_abi_type_components_t *rows);
