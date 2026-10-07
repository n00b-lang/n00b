#include "abi_decl_index.h"

#include <limits.h>

#include "lib/alloc.h"
#include "lib/indexed_table.h"
#include "lib/list.h"

typedef parse_scope_t *abi_scope_ptr_t;
core_list_decl(abi_scope_ptr_t);

struct n00bb_abi_decl_index_t {
    core_indexed_table_t *scopes;
    core_indexed_table_t *symbols;
};

static core_hash_value_t
pointer_hash(void *value)
{
    return (core_hash_value_t)(uintptr_t)value;
}

static bool
pointer_equal(const void *left, const void *right)
{
    return left == right;
}

static bool
add_scope_chain(n00bb_abi_decl_index_t *index, parse_scope_t *scope)
{
    core_list_t(abi_scope_ptr_t) chain = {0};
    bool module_found = false;
    for (parse_scope_t *cursor = scope; cursor; cursor = cursor->parent) {
        core_list_push(chain, cursor);
        if (core_string_eq(cursor->scope_tag,
                           CORE_STRING_STATIC("module"))) {
            module_found = true;
            break;
        }
    }
    if (module_found)
        for (size_t i = chain.len; i; i--)
            if (!core_indexed_table_intern(index->scopes,
                    chain.data[i - 1], chain.data[i - 1], nullptr)) {
                module_found = false;
                break;
            }
    core_list_free(chain);
    return module_found;
}

n00bb_abi_decl_index_t *
n00bb_abi_decl_index_new(const parse_symtab_t *symbols)
{
    if (!symbols) return nullptr;
    n00bb_abi_decl_index_t *index = core_alloc(n00bb_abi_decl_index_t);
    index->scopes = core_indexed_table_new(1, UINT32_MAX,
                                            pointer_hash, pointer_equal);
    index->symbols = core_indexed_table_new(1, UINT32_MAX,
                                             pointer_hash, pointer_equal);
    if (!index->scopes || !index->symbols) {
        n00bb_abi_decl_index_free(index);
        return nullptr;
    }
    for (parse_scope_t *scope = symbols->all_scopes; scope;
         scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->is_synthetic
                || (!symbol->decl_node && !symbol->definition_count))
                continue;
            if (symbol->scope != scope || !add_scope_chain(index, scope)
                || !core_indexed_table_intern(index->symbols, symbol,
                                               symbol, nullptr)) {
                n00bb_abi_decl_index_free(index);
                return nullptr;
            }
        }
    }
    return index;
}

void
n00bb_abi_decl_index_free(n00bb_abi_decl_index_t *index)
{
    if (!index) return;
    core_indexed_table_free(index->symbols);
    core_indexed_table_free(index->scopes);
    core_free(index);
}

size_t
n00bb_abi_decl_scope_count(const n00bb_abi_decl_index_t *index)
{
    return index ? core_indexed_table_len(index->scopes) : 0;
}

size_t
n00bb_abi_decl_symbol_count(const n00bb_abi_decl_index_t *index)
{
    return index ? core_indexed_table_len(index->symbols) : 0;
}

parse_scope_t *
n00bb_abi_decl_scope_at(const n00bb_abi_decl_index_t *index,
                         uint32_t row)
{
    const core_indexed_entry_t *entry = index
        ? core_indexed_table_at(index->scopes, row) : nullptr;
    return entry ? entry->value : nullptr;
}

parse_sym_entry_t *
n00bb_abi_decl_symbol_at(const n00bb_abi_decl_index_t *index,
                          uint32_t row)
{
    const core_indexed_entry_t *entry = index
        ? core_indexed_table_at(index->symbols, row) : nullptr;
    return entry ? entry->value : nullptr;
}

bool
n00bb_abi_decl_find_scope(const n00bb_abi_decl_index_t *index,
                           const parse_scope_t *scope, uint32_t *out)
{
    if (!index || !scope || !out) return false;
    const core_indexed_entry_t *entry = core_indexed_table_find(
        index->scopes, scope);
    if (!entry) return false;
    *out = entry->index;
    return true;
}

bool
n00bb_abi_decl_find_symbol(const n00bb_abi_decl_index_t *index,
                            const parse_sym_entry_t *symbol, uint32_t *out)
{
    if (!index || !symbol || !out) return false;
    const core_indexed_entry_t *entry = core_indexed_table_find(
        index->symbols, symbol);
    if (!entry) return false;
    *out = entry->index;
    return true;
}
