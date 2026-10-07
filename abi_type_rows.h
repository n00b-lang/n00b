#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_emit_strings.h"
#include "abi_type_index.h"
#include "n00b-abi-v1.h"
#include "parse/symtab.h"

typedef struct {
    n00b_abi_type_v1 *rows;
    size_t count;
} n00bb_abi_type_identity_rows_t;

/* Populates identity, category and finalized extent facts. Relationship
 * tables (fields, constraints, signatures and layout runs) are deliberately
 * left for the complete type emitter; these rows are not standalone output. */
bool n00bb_abi_type_identity_rows_build(
    n00bb_abi_type_index_t *index, ir_module_t *comptime,
    ir_module_t *runtime,
    const parse_symtab_t *symbols, n00bb_abi_name_pool_t *names,
    n00bb_abi_type_identity_rows_t *out);
void n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows);
