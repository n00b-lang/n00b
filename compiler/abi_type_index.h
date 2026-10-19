#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir/ir.h"
#include "parse/symtab.h"
#include "th/environment.h"

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
static inline th_type *
n00bb_generic_head(th_ctx *types, th_type *type)
{
    core_string_t constructor = {};
    size_t arguments = 0;
    bool variant = false;
    type = type ? th_resolve(type) : nullptr;
    if (!types || th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &constructor, &variant, &arguments)
        || !arguments || !constructor.data) return nullptr;
    th_env_type_shape_kind_t shape_kind = TH_ENV_TYPE_SHAPE_ALIAS;
    if (!th_env_type_shape(types, constructor, &shape_kind)
        || shape_kind == TH_ENV_TYPE_SHAPE_ALIAS) return nullptr;
    return variant ? th_variant(types, constructor, nullptr, 0)
                   : th_name(types, constructor);
}
