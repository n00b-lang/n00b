#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "abi_emit_strings.h"
#include "abi_decl_index.h"
#include "abi_type_index.h"
#include "n00b-abi-v1.h"
#include "parse/symtab.h"

typedef struct {
    n00b_abi_type_v1 *rows;
    parse_sym_entry_t **owners;
    parse_sym_entry_t *(*owner_lookup)(void *context, th_type *type);
    void *owner_lookup_context;
    size_t count;
    n00b_abi_type_argument_v1 *arguments;
    size_t argument_count;
    uint32_t failed_type_index;
    const char *failed_reason;
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
    n00bb_abi_type_index_t *index, ir_module_t *runtime,
    bool include_external_definitions,
    const parse_symtab_t *symbols,
    n00bb_abi_type_owner_lookup_fn lookup, void *lookup_context,
    n00bb_abi_name_pool_t *names,
    n00bb_abi_type_identity_rows_t *out);
void n00bb_abi_type_identity_rows_release(
    n00bb_abi_type_identity_rows_t *rows);

/* Assigns variable identities from cached declaration signatures. */
bool n00bb_abi_type_variables_bind(
    n00bb_abi_type_index_t *index, th_ctx *types,
    const n00bb_abi_decl_index_t *declarations,
    n00bb_abi_type_identity_rows_t *rows, const char **reason);

bool n00bb_abi_type_variables_bind_import(
    n00bb_abi_type_index_t *index, th_ctx *types,
    const parse_sym_entry_t *symbol, th_type *signature,
    uint32_t requirement_index, n00bb_abi_type_identity_rows_t *rows,
    const char **reason);

/* Run after both local and imported identities have been assigned. */
bool n00bb_abi_type_variables_validate(
    const n00bb_abi_type_identity_rows_t *rows, const char **reason);

/* Adds TYPE component ranges to the shared U32 stream at stream_base. */
bool n00bb_abi_type_components_build(
    n00bb_abi_type_index_t *index, ir_module_t *module,
    uint32_t stream_base, n00bb_abi_type_identity_rows_t *types,
    n00bb_abi_type_components_t *out);
void n00bb_abi_type_components_release(n00bb_abi_type_components_t *rows);
