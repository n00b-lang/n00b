#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "parse/symtab.h"

typedef struct n00bb_abi_decl_index_t n00bb_abi_decl_index_t;

/* Indexes only declarations owned by this TU. Row zero is reserved. */
n00bb_abi_decl_index_t *n00bb_abi_decl_index_new(
    const parse_symtab_t *symbols);
bool n00bb_abi_decl_generated_symbol(const parse_sym_entry_t *symbol);
bool n00bb_abi_decl_owned_symbol(const parse_sym_entry_t *symbol);
void n00bb_abi_decl_index_free(n00bb_abi_decl_index_t *index);
size_t n00bb_abi_decl_scope_count(const n00bb_abi_decl_index_t *index);
size_t n00bb_abi_decl_symbol_count(const n00bb_abi_decl_index_t *index);
uint32_t n00bb_abi_decl_module_scope_index(
    const n00bb_abi_decl_index_t *index);
parse_scope_t *n00bb_abi_decl_scope_at(
    const n00bb_abi_decl_index_t *index, uint32_t row);
parse_sym_entry_t *n00bb_abi_decl_symbol_at(
    const n00bb_abi_decl_index_t *index, uint32_t row);
bool n00bb_abi_decl_find_scope(const n00bb_abi_decl_index_t *index,
                               const parse_scope_t *scope, uint32_t *out);
bool n00bb_abi_decl_find_symbol(const n00bb_abi_decl_index_t *index,
                                const parse_sym_entry_t *symbol,
                                uint32_t *out);
