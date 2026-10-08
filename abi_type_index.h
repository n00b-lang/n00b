#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir/ir.h"
#include "parse/symtab.h"

typedef struct n00bb_abi_type_index_t n00bb_abi_type_index_t;

/* Index 0 is the builtin nil type. This catalog is local to one TU and its
 * shared type context; indexes are rebuilt and remapped when objects link. */
n00bb_abi_type_index_t *n00bb_abi_type_index_new(ir_module_t *module);
void n00bb_abi_type_index_free(n00bb_abi_type_index_t *index);
bool n00bb_abi_type_index_add(n00bb_abi_type_index_t *index,
                              th_type *type, uint32_t *out_index);
/* Retains an imported trait's identity without copying its provider-owned
 * method type variables into this TU's declaration universe. */
bool n00bb_abi_type_index_add_imported_trait(
    n00bb_abi_type_index_t *index, th_type *type);
bool n00bb_abi_type_index_collect_ir(n00bb_abi_type_index_t *index,
                                     ir_module_t *module);
/* Closed registered module types have stable identity. Open body-local copies
 * instead enter ABI metadata through their declaring symbols, if any. */
bool n00bb_abi_type_index_collect_module_types(
    n00bb_abi_type_index_t *index, ir_module_t *module);
typedef bool (*n00bb_abi_symbol_selector_fn)(
    void *context, const parse_sym_entry_t *symbol, bool *include);
bool n00bb_abi_type_index_collect_symbols(
    n00bb_abi_type_index_t *index, const parse_symtab_t *symbols,
    n00bb_abi_symbol_selector_fn select, void *context);
th_type *n00bb_abi_type_index_symbol_type(
    n00bb_abi_type_index_t *index, const parse_sym_entry_t *symbol);
size_t n00bb_abi_type_index_count(const n00bb_abi_type_index_t *index);
th_type *n00bb_abi_type_index_at(const n00bb_abi_type_index_t *index,
                                uint32_t row);
bool n00bb_abi_type_index_find(n00bb_abi_type_index_t *index,
                               th_type *type, uint32_t *out_index);
