// symtab.c - Symbol table with named parallel namespaces.
//
// Each namespace has its own scope stack and hash table. Push/pop
// scope is per-namespace. Symbol lookup returns the innermost
// (most recent) definition. Shadowed entries are restored on pop.

#include "slay/symtab.h"
#include "core/alloc.h"
#include "core/dict_untyped.h"
#include "core/hash.h"
#include "strings/string_ops.h"

#include <string.h>

// ============================================================================
// Hash table helpers (thin wrappers around dict_untyped)
// ============================================================================

static n00b_dict_untyped_t *
ht_new(void)
{
    n00b_dict_untyped_t *d = n00b_alloc(n00b_dict_untyped_t);
    n00b_dict_untyped_init(d, .hash = n00b_hash_cstring);
    return d;
}

static void
ht_put(n00b_dict_untyped_t *d, n00b_string_t key, n00b_sym_entry_t *val)
{
    // Use the string data pointer as the key. For lookup consistency,
    // we rely on the dict using our string hash function.
    n00b_dict_untyped_put(d, key.data, val);
}

static n00b_sym_entry_t *
ht_get(n00b_dict_untyped_t *d, n00b_string_t key)
{
    if (!d || !key.data) {
        return NULL;
    }

    bool found = false;
    void *val  = n00b_dict_untyped_get(d, key.data, &found);

    return found ? (n00b_sym_entry_t *)val : NULL;
}

static void
ht_remove(n00b_dict_untyped_t *d, n00b_string_t key)
{
    if (d && key.data) {
        n00b_dict_untyped_remove(d, key.data);
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

n00b_symtab_t *
n00b_symtab_new(void)
{
    n00b_symtab_t *st = n00b_alloc(n00b_symtab_t);
    memset(st, 0, sizeof(*st));
    return st;
}

void
n00b_symtab_free(n00b_symtab_t *st)
{
    if (!st) {
        return;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        n00b_namespace_t *ns = &st->namespaces[i];

        // Free all tracked scopes and their entries.
        for (int32_t j = 0; j < ns->all_count; j++) {
            n00b_scope_t *scope = ns->all_scopes[j];
            n00b_sym_entry_t *entry = scope->first_in_scope;

            while (entry) {
                n00b_sym_entry_t *next = entry->next_in_scope;
                n00b_free(entry);
                entry = next;
            }

            n00b_free(scope);
        }

        if (ns->all_scopes) {
            n00b_free(ns->all_scopes);
        }

        if (ns->symbols) {
            n00b_free(ns->symbols);
        }
    }

    if (st->namespaces) {
        n00b_free(st->namespaces);
    }

    n00b_free(st);
}

// ============================================================================
// Namespace management
// ============================================================================

n00b_namespace_t *
n00b_symtab_ns(n00b_symtab_t *st, n00b_string_t ns_name)
{
    if (!st) {
        return NULL;
    }

    // Search for existing namespace.
    for (int32_t i = 0; i < st->ns_count; i++) {
        if (n00b_unicode_str_eq(st->namespaces[i].ns_name, ns_name)) {
            return &st->namespaces[i];
        }
    }

    // Create new namespace.
    if (st->ns_count >= st->ns_cap) {
        int32_t new_cap = st->ns_cap ? st->ns_cap * 2 : 4;
        n00b_namespace_t *new_ns = n00b_alloc_array(n00b_namespace_t, new_cap);

        if (st->ns_count > 0) {
            memcpy(new_ns, st->namespaces, st->ns_count * sizeof(n00b_namespace_t));
        }

        if (st->namespaces) {
            n00b_free(st->namespaces);
        }

        st->namespaces = new_ns;
        st->ns_cap = new_cap;
    }

    n00b_namespace_t *ns = &st->namespaces[st->ns_count++];
    memset(ns, 0, sizeof(*ns));
    ns->ns_name = ns_name;
    ns->symbols = ht_new();

    return ns;
}

// ============================================================================
// Scope management
// ============================================================================

void
n00b_symtab_push_scope(n00b_symtab_t *st, n00b_string_t ns_name,
                        n00b_string_t scope_name)
{
    n00b_namespace_t *ns = n00b_symtab_ns(st, ns_name);

    if (!ns) {
        return;
    }

    n00b_scope_t *scope = n00b_alloc(n00b_scope_t);
    memset(scope, 0, sizeof(*scope));
    scope->parent = ns->current;
    scope->name   = scope_name;
    scope->depth  = ns->depth;

    ns->current = scope;
    ns->depth++;

    // Track scope for cleanup.
    if (ns->all_count >= ns->all_cap) {
        int32_t new_cap = ns->all_cap ? ns->all_cap * 2 : 8;
        n00b_scope_t **new_arr = n00b_alloc_array(n00b_scope_t *, new_cap);

        if (ns->all_count > 0) {
            memcpy(new_arr, ns->all_scopes,
                   ns->all_count * sizeof(n00b_scope_t *));
        }

        if (ns->all_scopes) {
            n00b_free(ns->all_scopes);
        }

        ns->all_scopes = new_arr;
        ns->all_cap    = new_cap;
    }

    ns->all_scopes[ns->all_count++] = scope;
}

void
n00b_symtab_pop_scope(n00b_symtab_t *st, n00b_string_t ns_name)
{
    n00b_namespace_t *ns = n00b_symtab_ns(st, ns_name);

    if (!ns || !ns->current) {
        return;
    }

    n00b_scope_t *scope = ns->current;
    n00b_dict_untyped_t *ht = (n00b_dict_untyped_t *)ns->symbols;

    // Restore the hash table for correct lookup at the enclosing scope,
    // but keep the scope and its entries alive (attached to the tree node).
    n00b_sym_entry_t *entry = scope->first_in_scope;

    while (entry) {
        if (entry->shadowed) {
            ht_put(ht, entry->name, entry->shadowed);
        }
        else {
            ht_remove(ht, entry->name);
        }

        entry = entry->next_in_scope;
    }

    ns->current = scope->parent;
    ns->depth--;
}

// ============================================================================
// Symbol operations
// ============================================================================

n00b_sym_entry_t *
n00b_symtab_add(n00b_symtab_t *st, n00b_string_t ns_name,
                 n00b_string_t name, n00b_sym_kind_t kind,
                 n00b_parse_tree_t *decl_node)
{
    n00b_namespace_t *ns = n00b_symtab_ns(st, ns_name);

    if (!ns) {
        return NULL;
    }

    n00b_dict_untyped_t *ht = (n00b_dict_untyped_t *)ns->symbols;

    n00b_sym_entry_t *entry = n00b_alloc(n00b_sym_entry_t);
    memset(entry, 0, sizeof(*entry));
    entry->name        = name;
    entry->kind        = kind;
    entry->scope_depth = ns->depth;
    entry->decl_node   = decl_node;

    // Check for shadowing.
    n00b_sym_entry_t *existing = ht_get(ht, name);

    if (existing) {
        entry->shadowed = existing;
    }

    // Insert into hash table.
    ht_put(ht, name, entry);

    // Chain onto the current scope for cleanup.
    if (ns->current) {
        entry->next_in_scope = ns->current->first_in_scope;
        ns->current->first_in_scope = entry;
    }

    return entry;
}

n00b_sym_entry_t *
n00b_symtab_lookup(n00b_symtab_t *st, n00b_string_t ns_name,
                    n00b_string_t name)
{
    if (!st) {
        return NULL;
    }

    // Find the namespace (don't create it).
    for (int32_t i = 0; i < st->ns_count; i++) {
        if (n00b_unicode_str_eq(st->namespaces[i].ns_name, ns_name)) {
            return ht_get((n00b_dict_untyped_t *)st->namespaces[i].symbols, name);
        }
    }

    return NULL;
}

n00b_sym_entry_t *
n00b_symtab_lookup_all(n00b_symtab_t *st, n00b_string_t ns_name,
                        n00b_string_t name)
{
    if (!st || !name.data) {
        return NULL;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (!n00b_unicode_str_eq(st->namespaces[i].ns_name, ns_name)) {
            continue;
        }

        n00b_namespace_t *ns = &st->namespaces[i];

        // First try the live hash table (covers current scope).
        n00b_sym_entry_t *found = ht_get((n00b_dict_untyped_t *)ns->symbols, name);

        if (found) {
            return found;
        }

        // Walk all scopes (most recent first).
        for (int32_t j = ns->all_count - 1; j >= 0; j--) {
            n00b_sym_entry_t *entry = ns->all_scopes[j]->first_in_scope;

            while (entry) {
                if (n00b_unicode_str_eq(entry->name, name)) {
                    return entry;
                }

                entry = entry->next_in_scope;
            }
        }

        return NULL;
    }

    return NULL;
}

bool
n00b_symtab_is_typedef(n00b_symtab_t *st, n00b_string_t name)
{
    n00b_sym_entry_t *entry = n00b_symtab_lookup(st, n00b_string_empty(), name);

    return entry && entry->kind == N00B_SYM_TYPEDEF;
}

int32_t
n00b_symtab_depth(n00b_symtab_t *st, n00b_string_t ns_name)
{
    if (!st) {
        return -1;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (n00b_unicode_str_eq(st->namespaces[i].ns_name, ns_name)) {
            return st->namespaces[i].depth;
        }
    }

    return -1;
}

n00b_scope_t *
n00b_symtab_current_scope(n00b_symtab_t *st, n00b_string_t ns_name)
{
    if (!st) {
        return NULL;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (n00b_unicode_str_eq(st->namespaces[i].ns_name, ns_name)) {
            return st->namespaces[i].current;
        }
    }

    return NULL;
}
