#include "parse/symtab.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <string.h>

// Symbols are keyed by (owning scope, name) rather than by name alone. Scopes
// are retained after their pop, so a lookup walks the parent chain probing one
// hash per level: pop does no work, and a scoped lookup is still correct once
// the walk that built the table has finished.
typedef struct {
    int32_t      scope_id;
    ncc_string_t name;
} symkey_t;

static ncc_hash_value_t
symkey_hash(void *k)
{
    symkey_t *key = (symkey_t *)k;

    return ncc_hash_raw(key->name.data, key->name.u8_bytes)
         ^ ncc_hash_raw(&key->scope_id, sizeof(key->scope_id));
}

static bool
symkey_eq(const void *a, const void *b)
{
    const symkey_t *ka = (const symkey_t *)a;
    const symkey_t *kb = (const symkey_t *)b;

    return ka->scope_id == kb->scope_id && ncc_string_eq(ka->name, kb->name);
}

static ncc_string_t
dup_str(ncc_string_t s)
{
    if (!s.data) {
        return s;
    }

    // The default namespace is the empty string, which must stay distinct
    // from an absent one: ncc_string_from_raw hands back a null string for a
    // zero length.
    if (s.u8_bytes == 0) {
        return ncc_string_empty();
    }

    return ncc_string_from_raw(s.data, (int64_t)s.u8_bytes);
}

static ncc_dict_t *
ht_new(ncc_hash_fn hash, ncc_dict_key_eq_fn eq)
{
    ncc_dict_t *d = ncc_alloc(ncc_dict_t);

    ncc_dict_init(d, hash, eq);

    return d;
}

static ncc_namespace_t *
find_ns(ncc_symtab_t *st, ncc_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (ncc_string_eq(st->namespaces[i]->ns_name, ns_name)) {
            return st->namespaces[i];
        }
    }

    return nullptr;
}

static ncc_scope_t *
push_scope_in(ncc_symtab_t *st, ncc_namespace_t *ns, ncc_string_t scope_name)
{
    ncc_scope_t *scope = ncc_alloc(ncc_scope_t);

    memset(scope, 0, sizeof(*scope));

    scope->parent = ns->current;
    scope->ns     = ns;
    scope->name   = dup_str(scope_name);
    scope->depth  = ns->depth;
    scope->id     = st->next_scope_id++;

    scope->all_next = st->all_scopes;
    st->all_scopes  = scope;

    ns->current = scope;
    ns->depth++;

    return scope;
}

ncc_symtab_t *
ncc_symtab_new(void)
{
    ncc_symtab_t *st = ncc_alloc(ncc_symtab_t);

    memset(st, 0, sizeof(*st));
    st->scope_by_node = ht_new(ncc_hash_word, nullptr);

    return st;
}

void
ncc_symtab_free(ncc_symtab_t *st)
{
    if (!st) {
        return;
    }

    // The table owns the names it copied on add and push. Every other string
    // on an entry or scope is borrowed from the grammar's annotations or the
    // parse tree, which outlive it.
    ncc_scope_t *scope = st->all_scopes;

    while (scope) {
        ncc_scope_t     *next  = scope->all_next;
        ncc_sym_entry_t *entry = scope->first_in_scope;

        while (entry) {
            ncc_sym_entry_t *enext = entry->next_in_scope;

            ncc_free(entry->implements.data);
            ncc_free(entry->name.data);
            ncc_free(entry);
            entry = enext;
        }

        ncc_free(scope->name.data);
        ncc_free(scope);
        scope = next;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        ncc_dict_t *d = st->namespaces[i]->symbols;

        if (d) {
            for (size_t b = 0; b < d->capacity; b++) {
                if (d->buckets[b].state == _NCC_BUCKET_OCCUPIED) {
                    ncc_free(d->buckets[b].key);
                }
            }

            ncc_dict_free(d);
            ncc_free(d);
        }

        ncc_free(st->namespaces[i]->ns_name.data);
        ncc_free(st->namespaces[i]);
    }

    ncc_free(st->namespaces);
    ncc_dict_free(st->scope_by_node);
    ncc_free(st->scope_by_node);
    ncc_free(st);
}

ncc_namespace_t *
ncc_symtab_ns(ncc_symtab_t *st, ncc_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    ncc_namespace_t *existing = find_ns(st, ns_name);

    if (existing) {
        return existing;
    }

    if (st->ns_count >= st->ns_cap) {
        int32_t           cap = st->ns_cap ? st->ns_cap * 2 : 4;
        ncc_namespace_t **arr = ncc_alloc_array(ncc_namespace_t *, cap);

        if (st->ns_count > 0) {
            memcpy(arr, st->namespaces,
                   (size_t)st->ns_count * sizeof(ncc_namespace_t *));
        }

        ncc_free(st->namespaces);
        st->namespaces = arr;
        st->ns_cap     = cap;
    }

    ncc_namespace_t *ns = ncc_alloc(ncc_namespace_t);

    memset(ns, 0, sizeof(*ns));
    ns->ns_name = dup_str(ns_name);
    ns->symbols = ht_new(symkey_hash, symkey_eq);

    st->namespaces[st->ns_count++] = ns;

    // push_scope_in copies the name, so a borrowed empty string will do.
    ns->root = push_scope_in(st, ns, (ncc_string_t){.data = ""});

    return ns;
}

void
ncc_symtab_push_scope(ncc_symtab_t *st, ncc_string_t ns_name,
                      ncc_string_t scope_name)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (ns) {
        push_scope_in(st, ns, scope_name);
    }
}

void
ncc_symtab_pop_scope(ncc_symtab_t *st, ncc_string_t ns_name)
{
    ncc_namespace_t *ns = find_ns(st, ns_name);

    // The file scope is never popped; it is what a scoped lookup falls back
    // to once the walk has finished.
    if (!ns || !ns->current || !ns->current->parent) {
        return;
    }

    ns->current = ns->current->parent;
    ns->depth--;
}

ncc_sym_entry_t *
ncc_symtab_lookup_in(ncc_scope_t *scope, ncc_string_t name)
{
    if (!scope || !scope->ns || !name.data) {
        return nullptr;
    }

    symkey_t key   = {.scope_id = scope->id, .name = name};
    bool     found = false;
    void    *val   = _ncc_dict_get(scope->ns->symbols, &key, &found);

    return found ? (ncc_sym_entry_t *)val : nullptr;
}

static ncc_sym_entry_t *
lookup_chain(ncc_scope_t *scope, ncc_string_t name)
{
    for (ncc_scope_t *s = scope; s; s = s->parent) {
        ncc_sym_entry_t *e = ncc_symtab_lookup_in(s, name);

        if (e) {
            return e;
        }
    }

    return nullptr;
}

ncc_sym_entry_t *
ncc_symtab_add(ncc_symtab_t *st, ncc_string_t ns_name, ncc_string_t name,
               ncc_sym_kind_t kind, ncc_parse_tree_t *decl_node)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (!ns || !name.data) {
        return nullptr;
    }

    ncc_scope_t     *scope = ns->current;
    ncc_sym_entry_t *entry = ncc_alloc(ncc_sym_entry_t);

    memset(entry, 0, sizeof(*entry));

    entry->name        = dup_str(name);
    entry->kind        = kind;
    entry->scope_depth = scope->depth;
    entry->decl_node   = decl_node;
    entry->scope       = scope;
    entry->shadowed    = lookup_chain(scope, name);

    symkey_t *key = ncc_alloc(symkey_t);

    key->scope_id = scope->id;
    key->name     = entry->name;

    void *prior = _ncc_dict_put(ns->symbols, key, entry);

    if (prior) {
        // Redeclared in the same scope: the key was already present, so the
        // one just allocated is unused.
        ncc_free(key);
    }

    entry->next_in_scope = scope->first_in_scope;
    scope->first_in_scope = entry;

    return entry;
}

ncc_sym_entry_t *
ncc_symtab_lookup(ncc_symtab_t *st, ncc_string_t ns_name, ncc_string_t name)
{
    ncc_namespace_t *ns = find_ns(st, ns_name);

    return ns ? lookup_chain(ns->current, name) : nullptr;
}

ncc_sym_entry_t *
ncc_symtab_lookup_scoped(ncc_symtab_t *st, ncc_scope_t *scope,
                         ncc_string_t ns_name, ncc_string_t name)
{
    if (scope) {
        return lookup_chain(scope, name);
    }

    ncc_namespace_t *ns = find_ns(st, ns_name);

    return ns ? ncc_symtab_lookup_in(ns->root, name) : nullptr;
}

void
ncc_symtab_set_scope_node(ncc_symtab_t *st, ncc_string_t ns_name,
                          ncc_parse_tree_t *node)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (!ns || !ns->current || !node) {
        return;
    }

    ns->current->node = node;
    ncc_dict_put(st->scope_by_node, node, ns->current);
}

ncc_scope_t *
ncc_symtab_scope_for_node(ncc_symtab_t *st, ncc_parse_tree_t *node)
{
    if (!st || !node) {
        return nullptr;
    }

    bool  found = false;
    void *val   = ncc_dict_get(st->scope_by_node, node, &found);

    return found ? (ncc_scope_t *)val : nullptr;
}

bool
ncc_symtab_is_typedef(ncc_symtab_t *st, ncc_string_t name)
{
    ncc_sym_entry_t *e = ncc_symtab_lookup(st, ncc_string_empty(), name);

    return e && e->kind == NCC_SYM_TYPEDEF;
}

int32_t
ncc_symtab_depth(ncc_symtab_t *st, ncc_string_t ns_name)
{
    ncc_namespace_t *ns = find_ns(st, ns_name);

    return ns ? ns->current->depth : -1;
}

ncc_scope_t *
ncc_symtab_current_scope(ncc_symtab_t *st, ncc_string_t ns_name)
{
    ncc_namespace_t *ns = find_ns(st, ns_name);

    return ns ? ns->current : nullptr;
}
