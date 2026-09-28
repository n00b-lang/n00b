#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <string.h>

typedef struct {
    int32_t      scope_id;
    core_string_t name;
} symkey_t;

static core_hash_value_t
symkey_hash(void *k)
{
    symkey_t *key = (symkey_t *)k;

    return core_hash_raw(key->name.data, key->name.u8_bytes)
         ^ core_hash_raw(&key->scope_id, sizeof(key->scope_id));
}

static bool
symkey_eq(const void *a, const void *b)
{
    const symkey_t *ka = (const symkey_t *)a;
    const symkey_t *kb = (const symkey_t *)b;

    return ka->scope_id == kb->scope_id && core_string_eq(ka->name, kb->name);
}

static core_hash_value_t
name_key_hash(void *k)
{
    return core_string_hash(*(core_string_t *)k);
}

static bool
name_key_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a, *(const core_string_t *)b);
}

static core_dict_t *
ht_new(core_hash_fn hash, core_dict_key_eq_fn eq)
{
    core_dict_t *d = core_alloc(core_dict_t);

    core_dict_init(d, hash, eq);

    return d;
}

static parse_namespace_t *
find_ns(parse_symtab_t *st, core_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    bool  found = false;
    void *val   = _core_dict_get(st->ns_map, &ns_name, &found);

    return found ? (parse_namespace_t *)val : nullptr;
}

static parse_scope_t *
push_scope_in(parse_symtab_t *st, parse_namespace_t *ns, core_string_t scope_name)
{
    parse_scope_t *scope = core_alloc(parse_scope_t);

    memset(scope, 0, sizeof(*scope));

    scope->parent = ns->current;
    scope->ns     = ns;
    scope->name   = core_string_dup(scope_name);
    scope->depth  = ns->depth;
    scope->id     = st->next_scope_id++;

    scope->all_next = st->all_scopes;
    st->all_scopes  = scope;

    ns->current = scope;
    ns->depth++;

    return scope;
}

parse_symtab_t *
parse_symtab_new(void)
{
    parse_symtab_t *st = core_alloc(parse_symtab_t);

    memset(st, 0, sizeof(*st));
    st->scope_by_node = ht_new(core_hash_word, nullptr);
    st->ns_map        = ht_new(name_key_hash, name_key_eq);

    return st;
}

void
parse_symtab_free(parse_symtab_t *st)
{
    if (!st) {
        return;
    }

    parse_scope_t *scope = st->all_scopes;

    while (scope) {
        parse_scope_t     *next  = scope->all_next;
        parse_sym_entry_t *entry = scope->first_in_scope;

        while (entry) {
            parse_sym_entry_t *enext = entry->next_in_scope;

            core_free(entry->implements.data);
            core_free(entry->name.data);
            core_free(entry);
            entry = enext;
        }

        core_free(scope->name.data);
        core_free(scope);
        scope = next;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        core_dict_t *d = st->namespaces[i]->symbols;

        if (d) {
            core_dict_foreach (d, e) {
                core_free(e->key);
            }

            core_dict_free(d);
            core_free(d);
        }

        core_free(st->namespaces[i]->ns_name.data);
        core_free(st->namespaces[i]);
    }

    core_free(st->namespaces);
    core_dict_free(st->scope_by_node);
    core_free(st->scope_by_node);
    core_dict_free(st->ns_map);
    core_free(st->ns_map);
    core_free(st);
}

parse_namespace_t *
parse_symtab_ns(parse_symtab_t *st, core_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    parse_namespace_t *existing = find_ns(st, ns_name);

    if (existing) {
        return existing;
    }

    if (st->ns_count >= st->ns_cap) {
        int32_t           cap = st->ns_cap ? st->ns_cap * 2 : 4;
        parse_namespace_t **arr = core_alloc_array(parse_namespace_t *, cap);

        if (st->ns_count > 0) {
            memcpy(arr, st->namespaces,
                   (size_t)st->ns_count * sizeof(parse_namespace_t *));
        }

        core_free(st->namespaces);
        st->namespaces = arr;
        st->ns_cap     = cap;
    }

    parse_namespace_t *ns = core_alloc(parse_namespace_t);

    memset(ns, 0, sizeof(*ns));
    ns->ns_name = core_string_dup(ns_name);
    ns->symbols = ht_new(symkey_hash, symkey_eq);

    _core_dict_put(st->ns_map, &ns->ns_name, ns);

    st->namespaces[st->ns_count++] = ns;

    ns->root = push_scope_in(st, ns, (core_string_t){.data = ""});

    return ns;
}

void
parse_symtab_push_scope(parse_symtab_t *st, core_string_t ns_name,
                      core_string_t scope_name)
{
    parse_namespace_t *ns = parse_symtab_ns(st, ns_name);

    if (ns) {
        push_scope_in(st, ns, scope_name);
    }
}

void
parse_symtab_pop_scope(parse_symtab_t *st, core_string_t ns_name)
{
    parse_namespace_t *ns = find_ns(st, ns_name);

    if (!ns || !ns->current || !ns->current->parent) {
        return;
    }

    ns->current = ns->current->parent;
    ns->depth--;
}

void
parse_symtab_enter_scope(parse_symtab_t *st, core_string_t ns_name,
                       parse_scope_t *scope)
{
    parse_namespace_t *ns = find_ns(st, ns_name);

    if (!ns || !scope) {
        return;
    }

    ns->current = scope;
    ns->depth   = scope->depth;
}

void
parse_symtab_leave_scope(parse_symtab_t *st, core_string_t ns_name)
{
    parse_symtab_pop_scope(st, ns_name);
}

parse_sym_entry_t *
parse_symtab_lookup_in(parse_scope_t *scope, core_string_t name)
{
    if (!scope || !scope->ns || !name.data) {
        return nullptr;
    }

    symkey_t key   = {.scope_id = scope->id, .name = name};
    bool     found = false;
    void    *val   = _core_dict_get(scope->ns->symbols, &key, &found);

    return found ? (parse_sym_entry_t *)val : nullptr;
}

static parse_sym_entry_t *
lookup_chain(parse_scope_t *scope, core_string_t name)
{
    for (parse_scope_t *s = scope; s; s = s->parent) {
        parse_sym_entry_t *e = parse_symtab_lookup_in(s, name);

        if (e) {
            return e;
        }
    }

    return nullptr;
}

parse_sym_entry_t *
parse_symtab_add(parse_symtab_t *st, core_string_t ns_name, core_string_t name,
               parse_sym_kind_t kind, parse_tree_t *decl_node)
{
    parse_namespace_t *ns = parse_symtab_ns(st, ns_name);

    if (!ns || !name.data) {
        return nullptr;
    }

    parse_scope_t     *scope = ns->current;
    parse_sym_entry_t *entry = core_alloc(parse_sym_entry_t);

    memset(entry, 0, sizeof(*entry));

    entry->name        = core_string_dup(name);
    entry->kind        = kind;
    entry->scope_depth = scope->depth;
    entry->decl_node   = decl_node;
    entry->scope       = scope;
    entry->shadowed    = lookup_chain(scope, name);

    symkey_t *key = core_alloc(symkey_t);

    key->scope_id = scope->id;
    key->name     = entry->name;

    void *prior = _core_dict_put(ns->symbols, key, entry);

    if (prior) {
        core_free(key);
    }

    entry->next_in_scope = scope->first_in_scope;
    scope->first_in_scope = entry;

    return entry;
}

parse_sym_entry_t *
parse_symtab_lookup(parse_symtab_t *st, core_string_t ns_name, core_string_t name)
{
    parse_namespace_t *ns = find_ns(st, ns_name);

    return ns ? lookup_chain(ns->current, name) : nullptr;
}

parse_sym_entry_t *
parse_symtab_lookup_scoped(parse_symtab_t *st, parse_scope_t *scope,
                         core_string_t ns_name, core_string_t name)
{
    if (scope) {
        return lookup_chain(scope, name);
    }

    parse_namespace_t *ns = find_ns(st, ns_name);

    return ns ? parse_symtab_lookup_in(ns->root, name) : nullptr;
}

parse_sym_entry_t *
parse_symtab_resolve_node(parse_symtab_t *st, parse_tree_t *node,
                          core_string_t ns_name)
{
    if (!st || !node || core_tree_is_leaf(node)) return nullptr;

    parse_nt_node_t *pn = &core_tree_node_value(node);
    if (pn->sym) return pn->sym;

    core_string_t name = parse_tree_extract_first_identifier(node);
    if (!name.data || !name.u8_bytes) return nullptr;

    parse_scope_t *scope = pn->scope;
    if (!scope) scope = parse_symtab_scope_for_node(st, node);
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        st, scope, ns_name, name);
    if (symbol) {
        pn->scope = scope;
        pn->sym = symbol;
    }
    return symbol;
}

void
parse_symtab_set_scope_node(parse_symtab_t *st, core_string_t ns_name,
                          parse_tree_t *node)
{
    parse_namespace_t *ns = parse_symtab_ns(st, ns_name);

    if (!ns || !ns->current || !node) {
        return;
    }

    ns->current->node = node;
    core_dict_put(st->scope_by_node, node, ns->current);
}

parse_scope_t *
parse_symtab_scope_for_node(parse_symtab_t *st, parse_tree_t *node)
{
    if (!st || !node) {
        return nullptr;
    }

    bool  found = false;
    void *val   = core_dict_get(st->scope_by_node, node, &found);

    return found ? (parse_scope_t *)val : nullptr;
}

bool
parse_symtab_is_typedef(parse_symtab_t *st, core_string_t name)
{
    parse_sym_entry_t *e = parse_symtab_lookup(st, core_string_empty(), name);

    return e && e->kind == PARSE_SYM_TYPEDEF;
}

int32_t
parse_symtab_depth(parse_symtab_t *st, core_string_t ns_name)
{
    parse_namespace_t *ns = find_ns(st, ns_name);

    return ns ? ns->current->depth : -1;
}

parse_scope_t *
parse_symtab_current_scope(parse_symtab_t *st, core_string_t ns_name)
{
    parse_namespace_t *ns = find_ns(st, ns_name);

    return ns ? ns->current : nullptr;
}
