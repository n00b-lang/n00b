#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"
#include "th/typehash.h"

#include <string.h>

typedef struct {
    int32_t      scope_id;
    core_string_t name;
} symkey_t;

typedef parse_sym_entry_t *sym_entry_ptr_t;
core_list_decl(sym_entry_ptr_t);

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
    scope->owner_unit = st->owner_unit;
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

static void
map_scope(parse_namespace_t *ns, parse_scope_t *lexical,
          parse_scope_t *scope)
{
    uintptr_t key = (uintptr_t)lexical->id + 1;
    core_dict_put(ns->scopes_by_lexical_id, (void *)key, scope);
}

static parse_scope_t *
scope_for_lexical(parse_namespace_t *ns, parse_scope_t *lexical)
{
    if (!ns || !lexical) return nullptr;
    uintptr_t key = (uintptr_t)lexical->id + 1;
    bool found = false;
    void *value = core_dict_get(ns->scopes_by_lexical_id, (void *)key,
                                &found);
    return found ? (parse_scope_t *)value : nullptr;
}

static parse_scope_t *
clone_scope_for_namespace(parse_symtab_t *st, parse_namespace_t *ns,
                          parse_scope_t *lexical)
{
    parse_scope_t *existing = scope_for_lexical(ns, lexical);
    if (existing) return existing;

    parse_scope_t *parent = lexical->parent
        ? clone_scope_for_namespace(st, ns, lexical->parent) : nullptr;
    if (lexical->parent && !parent) return nullptr;

    parse_scope_t *saved = ns->current;
    int32_t saved_depth = ns->depth;
    ns->current = parent ? parent : ns->root;
    ns->depth = ns->current ? ns->current->depth + 1 : 0;
    parse_scope_t *clone = push_scope_in(st, ns, lexical->name);
    clone->lexical_scope = lexical;
    clone->scope_tag = core_string_dup(lexical->scope_tag);
    clone->adt_kind = core_string_dup(lexical->adt_kind);
    clone->node = lexical->node;
    map_scope(ns, lexical, clone);
    ns->current = saved;
    ns->depth = saved_depth;
    return clone;
}

parse_symtab_t *
parse_symtab_new(void)
{
    parse_symtab_t *st = core_alloc(parse_symtab_t);

    memset(st, 0, sizeof(*st));
    st->scope_by_node = core_alloc(core_dict_t);
    core_dict_init(st->scope_by_node, core_hash_word, nullptr);
    st->ns_map = core_alloc(core_dict_t);
    core_dict_init_string(st->ns_map);
    st->nominal_types = core_alloc(core_dict_t);
    core_dict_init_string(st->nominal_types);

    return st;
}

void
parse_symtab_set_owner_unit(parse_symtab_t *st, core_translation_unit_t *unit)
{
    if (!st) return;
    st->owner_unit = unit;
    for (parse_scope_t *scope = st->all_scopes; scope;
         scope = scope->all_next) scope->owner_unit = unit;
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
            core_free(entry->requested_storage_class.data);
            core_free(entry->selected_storage_class.data);
            core_free(entry->definition_nodes);
            core_free(entry->use_nodes);
            core_free(entry->function_bindings);
            core_free(entry->aliases);
            core_free(entry->aliased_by);
            core_free(entry->lifetime.starts);
            core_free(entry->lifetime.ranges);
            core_free(entry->lifetime.ends);
            core_free(entry->lifetime.cfg_edges);
            if (entry->namespaced_bindings) {
                core_dict_t *bindings = entry->namespaced_bindings;
                core_dict_foreach(bindings, item) {
                    core_string_t *key = item->key;
                    core_dict_string_key_free(key);
                }
                core_dict_free(bindings);
                core_free(bindings);
            }
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

        core_dict_free(st->namespaces[i]->scopes_by_lexical_id);
        core_free(st->namespaces[i]->scopes_by_lexical_id);

        core_free(st->namespaces[i]->ns_name.data);
        core_free(st->namespaces[i]);
    }

    core_free(st->namespaces);
    core_dict_free(st->scope_by_node);
    core_free(st->scope_by_node);
    core_dict_free(st->ns_map);
    core_free(st->ns_map);
    core_dict_foreach(st->nominal_types, item)
        core_dict_string_key_free(item->key);
    core_dict_free(st->nominal_types);
    core_free(st->nominal_types);
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

    if (st->ns_count == INT32_MAX) return nullptr;
    void *namespaces = st->namespaces;
    if (!core_list_reserve_raw(&namespaces, &st->ns_cap,
                               (size_t)st->ns_count + 1,
                               sizeof(*st->namespaces))) return nullptr;
    st->namespaces = namespaces;

    parse_namespace_t *ns = core_alloc(parse_namespace_t);

    memset(ns, 0, sizeof(*ns));
    ns->ns_name = core_string_dup(ns_name);
    ns->symbols = core_alloc(core_dict_t);
    core_dict_init(ns->symbols, symkey_hash, symkey_eq);
    ns->scopes_by_lexical_id = core_alloc(core_dict_t);
    core_dict_init(ns->scopes_by_lexical_id, core_hash_word, nullptr);

    _core_dict_put(st->ns_map, &ns->ns_name, ns);

    st->namespaces[st->ns_count++] = ns;

    ns->root = push_scope_in(st, ns, (core_string_t){.data = ""});
    ns->root->lexical_scope = ns->root;

    parse_namespace_t *default_ns = find_ns(st, core_string_empty());
    if (core_string_eq(ns_name, core_string_empty())) {
        map_scope(ns, ns->root, ns->root);
        for (int32_t i = 0; i < st->ns_count - 1; i++) {
            parse_namespace_t *other = st->namespaces[i];
            other->root->lexical_scope = ns->root;
            map_scope(other, ns->root, other->root);
            for (parse_scope_t *scope = st->all_scopes; scope;
                 scope = scope->all_next) {
                if (scope->ns == ns && scope != ns->root)
                    clone_scope_for_namespace(st, other, scope);
            }
        }
    }
    else if (default_ns) {
        ns->root->lexical_scope = default_ns->root;
        map_scope(ns, default_ns->root, ns->root);
        for (parse_scope_t *scope = st->all_scopes; scope;
             scope = scope->all_next) {
            if (scope->ns == default_ns && scope != default_ns->root)
                clone_scope_for_namespace(st, ns, scope);
        }
        ns->current = scope_for_lexical(ns, default_ns->current);
        ns->depth = ns->current ? ns->current->depth : 0;
    }
    else {
        map_scope(ns, ns->root, ns->root);
    }

    return ns;
}

bool
parse_symtab_visit_namespace_scopes(parse_symtab_t *st,
                                    core_string_t ns_name,
                                    parse_symtab_scope_visitor_fn visitor,
                                    void *context)
{
    parse_namespace_t *ns = find_ns(st, ns_name);
    if (!ns || !visitor) return false;

    for (parse_scope_t *scope = st->all_scopes; scope;
         scope = scope->all_next) {
        if (scope->ns == ns && !visitor(scope, context)) return false;
    }
    return true;
}

bool
parse_symtab_visit_name_across_namespaces(
    parse_symtab_t *st, parse_scope_t *scope, core_string_t name,
    parse_symtab_symbol_visitor_fn visitor, void *context)
{
    if (!st || !scope || !name.data || !visitor) return false;

    parse_scope_t *lexical = scope->lexical_scope
        ? scope->lexical_scope : scope;
    for (int32_t i = 0; i < st->ns_count; i++) {
        parse_namespace_t *ns = st->namespaces[i];
        parse_scope_t *peer = scope->ns == ns
            ? scope : scope_for_lexical(ns, lexical);
        parse_sym_entry_t *symbol = parse_symtab_lookup_in(peer, name);
        if (symbol && !visitor(symbol, context)) return false;
    }
    return true;
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
parse_symtab_push_scope_all(parse_symtab_t *st, core_string_t scope_name)
{
    parse_namespace_t *default_ns = parse_symtab_ns(st, core_string_empty());
    if (!default_ns) return;
    parse_scope_t *lexical = push_scope_in(st, default_ns, scope_name);
    lexical->lexical_scope = lexical;
    map_scope(default_ns, lexical, lexical);

    for (int32_t i = 0; i < st->ns_count; i++) {
        parse_namespace_t *ns = st->namespaces[i];
        if (ns == default_ns) continue;
        parse_scope_t *parent = scope_for_lexical(ns, lexical->parent);
        if (!parent) continue;
        ns->current = parent;
        ns->depth = parent->depth + 1;
        parse_scope_t *scope = push_scope_in(st, ns, scope_name);
        scope->lexical_scope = lexical;
        map_scope(ns, lexical, scope);
    }
}

void
parse_symtab_pop_scope_all(parse_symtab_t *st)
{
    if (!st) return;
    for (int32_t i = 0; i < st->ns_count; i++)
        parse_symtab_pop_scope(st, st->namespaces[i]->ns_name);
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

parse_scope_t *
parse_symtab_scope_in_namespace(parse_symtab_t *st, parse_scope_t *scope,
                                core_string_t ns_name)
{
    parse_namespace_t *ns = find_ns(st, ns_name);
    if (!ns || !scope) return nullptr;
    if (scope->ns == ns) return scope;
    parse_scope_t *lexical = scope->lexical_scope
        ? scope->lexical_scope : scope;
    return scope_for_lexical(ns, lexical);
}

void
parse_symtab_enter_scope_all(parse_symtab_t *st, parse_scope_t *scope)
{
    if (!st || !scope) return;
    parse_scope_t *lexical = scope->lexical_scope
        ? scope->lexical_scope : scope;
    for (int32_t i = 0; i < st->ns_count; i++) {
        parse_namespace_t *ns = st->namespaces[i];
        parse_scope_t *peer = scope_for_lexical(ns, lexical);
        if (!peer) continue;
        ns->current = peer;
        ns->depth = peer->depth;
    }
}

void
parse_symtab_leave_scope_all(parse_symtab_t *st)
{
    parse_symtab_pop_scope_all(st);
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

bool
parse_symtab_bind_nominal_type(parse_symtab_t *st, th_type *type,
                               parse_sym_entry_t *symbol)
{
    core_string_t key = {};
    if (!st || !type || !symbol || symbol->kind != PARSE_SYM_TYPEDEF
        || !th_named_info(th_resolve(type), &key, nullptr, nullptr))
        return false;
    parse_sym_entry_t *existing = core_dict_get(st->nominal_types, &key,
                                                nullptr);
    if (existing) return existing == symbol;
    core_string_t *owned = core_dict_string_key_copy(key);
    if (!owned) return false;
    if (core_dict_add(st->nominal_types, owned, symbol)) return true;
    core_dict_string_key_free(owned);
    return false;
}

parse_sym_entry_t *
parse_symtab_nominal_type(parse_symtab_t *st, th_type *type)
{
    core_string_t key = {};
    if (!st || !type || !th_named_info(th_resolve(type), &key, nullptr,
                                       nullptr)) return nullptr;
    return core_dict_get(st->nominal_types, &key, nullptr);
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
    entry->owner_unit  = st->owner_unit;
    entry->kind        = kind;
    entry->scope_depth = scope->depth;
    entry->decl_node   = decl_node;
    entry->scope       = scope;
    entry->shadowed    = lookup_chain(scope, name);
    entry->lifetime.region = (uint64_t)scope->id;
    entry->lifetime.has_region = true;

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

bool
parse_sym_set_binding(parse_sym_entry_t *symbol, core_string_t key,
                      void *value)
{
    if (!symbol || !key.data || !key.u8_bytes || !value) return false;
    core_dict_t *bindings = symbol->namespaced_bindings;
    if (!bindings) {
        bindings = core_alloc(core_dict_t);
        if (!bindings) return false;
        core_dict_init_string(bindings);
        symbol->namespaced_bindings = bindings;
    }
    bool found = false;
    (void)core_dict_get(bindings, &key, &found);
    if (found) {
        core_dict_put(bindings, &key, value);
        return true;
    }
    core_dict_put(bindings, core_dict_string_key_copy(key), value);
    return true;
}

void *
parse_sym_binding(const parse_sym_entry_t *symbol, core_string_t key)
{
    if (!symbol || !key.data || !key.u8_bytes || !symbol->namespaced_bindings)
        return nullptr;
    bool found = false;
    return core_dict_get(symbol->namespaced_bindings, &key, &found);
}

bool
parse_sym_set_binding_origin(parse_sym_entry_t *symbol,
                             parse_sym_entry_t *origin)
{
    if (!symbol || !origin || symbol == origin) return false;
    for (parse_sym_entry_t *at = origin; at; at = at->binding_origin)
        if (at == symbol) return false;
    symbol->binding_origin = origin;
    return true;
}

parse_sym_entry_t *
parse_sym_binding_origin(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->binding_origin : nullptr;
}

static bool
symbol_add_node(parse_tree_t ***nodes, size_t *count, size_t *capacity,
                parse_tree_t *node)
{
    if (!node) return false;
    for (size_t i = 0; i < *count; i++)
        if ((*nodes)[i] == node) return true;
    void *data = *nodes;
    if (!core_list_reserve_raw(&data, capacity, *count + 1,
                               sizeof(**nodes))) return false;
    *nodes = data;
    (*nodes)[(*count)++] = node;
    return true;
}

bool
parse_sym_add_definition(parse_sym_entry_t *symbol, parse_tree_t *node)
{
    return symbol && symbol_add_node(&symbol->definition_nodes,
        &symbol->definition_count, &symbol->definition_capacity, node);
}

bool
parse_sym_add_use(parse_sym_entry_t *symbol, parse_tree_t *node)
{
    if (!symbol || !node) return false;
    for (size_t i = 0; i < symbol->use_count; i++)
        if (symbol->use_nodes[i] == node) return true;
    if (!symbol_add_node(&symbol->use_nodes, &symbol->use_count,
                         &symbol->use_capacity, node)) return false;
    symbol->uses++;
    return true;
}

bool
parse_sym_remove_use(parse_sym_entry_t *symbol, parse_tree_t *node)
{
    if (!symbol || !node) return false;
    for (size_t i = 0; i < symbol->use_count; i++) {
        if (symbol->use_nodes[i] != node) continue;
        memmove(&symbol->use_nodes[i], &symbol->use_nodes[i + 1],
                (symbol->use_count - i - 1) * sizeof(*symbol->use_nodes));
        symbol->use_count--;
        if (symbol->uses) symbol->uses--;
        return true;
    }
    return false;
}

void
parse_sym_mark_type_inferred(parse_sym_entry_t *symbol)
{
    if (symbol) symbol->type_is_inferred = true;
}

static bool
valid_phase_mask(parse_sym_phase_t phase)
{
    return phase != PARSE_SYM_PHASE_NONE
        && (phase & ~PARSE_SYM_PHASE_BOTH) == 0;
}

void
parse_sym_set_phases(parse_sym_entry_t *symbol, parse_sym_phase_t available,
                     parse_sym_phase_t mutable_in,
                     parse_sym_phase_t initializable_in)
{
    if (!symbol || (available & ~PARSE_SYM_PHASE_BOTH)
        || (mutable_in & ~PARSE_SYM_PHASE_BOTH)
        || (initializable_in & ~PARSE_SYM_PHASE_BOTH)) {
        return;
    }

    symbol->phases = available;
    symbol->mutable_phases = mutable_in;
    symbol->initializer_phases = initializable_in;
}

bool
parse_sym_can_read(const parse_sym_entry_t *symbol, parse_sym_phase_t phase)
{
    return symbol && valid_phase_mask(phase)
        && (symbol->phases & phase) == phase;
}

bool
parse_sym_can_write(const parse_sym_entry_t *symbol, parse_sym_phase_t phase)
{
    return parse_sym_can_read(symbol, phase)
        && (symbol->mutable_phases & phase) == phase;
}

bool
parse_sym_can_initialize(const parse_sym_entry_t *symbol,
                         parse_sym_phase_t phase)
{
    return symbol && valid_phase_mask(phase)
        && (symbol->initializer_phases & phase) == phase;
}

void
parse_sym_set_phase_values(parse_sym_entry_t *symbol, void *comptime_value,
                           void *runtime_initializer)
{
    parse_sym_set_phase_values_with_presence(
        symbol, comptime_value != nullptr, comptime_value,
        runtime_initializer != nullptr, runtime_initializer);
}

void
parse_sym_set_phase_values_with_presence(
    parse_sym_entry_t *symbol, bool has_comptime_value,
    void *comptime_value, bool has_runtime_initializer,
    void *runtime_initializer)
{
    if (!symbol) {
        return;
    }

    symbol->comptime_value = comptime_value;
    symbol->runtime_initializer = runtime_initializer;
    symbol->has_comptime_value = has_comptime_value;
    symbol->has_runtime_initializer = has_runtime_initializer;
}

bool
parse_sym_get_comptime_value(const parse_sym_entry_t *symbol, void **value)
{
    bool present = symbol && symbol->has_comptime_value;
    if (value) *value = present ? symbol->comptime_value : nullptr;
    return present;
}

bool
parse_sym_get_runtime_initializer(const parse_sym_entry_t *symbol,
                                  void **value)
{
    bool present = symbol && symbol->has_runtime_initializer;
    if (value) *value = present ? symbol->runtime_initializer : nullptr;
    return present;
}

bool
parse_sym_set_phase_binding(parse_sym_entry_t *symbol, parse_sym_phase_t phase,
                            void *binding)
{
    if (!symbol || (phase != PARSE_SYM_PHASE_COMPTIME
                    && phase != PARSE_SYM_PHASE_RUNTIME)) return false;
    if (phase == PARSE_SYM_PHASE_COMPTIME) {
        symbol->comptime_binding = binding;
        symbol->has_comptime_binding = binding != nullptr;
    }
    else {
        symbol->runtime_binding = binding;
        symbol->has_runtime_binding = binding != nullptr;
    }
    return true;
}

void *
parse_sym_phase_binding(const parse_sym_entry_t *symbol,
                        parse_sym_phase_t phase)
{
    if (!symbol) return nullptr;
    if (phase == PARSE_SYM_PHASE_COMPTIME)
        return symbol->has_comptime_binding ? symbol->comptime_binding
                                            : nullptr;
    if (phase == PARSE_SYM_PHASE_RUNTIME)
        return symbol->has_runtime_binding ? symbol->runtime_binding
                                           : nullptr;
    return nullptr;
}

bool
parse_sym_set_function_binding(parse_sym_entry_t *symbol,
                               parse_sym_phase_t phase, void *function,
                               void *binding)
{
    if (!symbol || !function || !binding
        || (phase != PARSE_SYM_PHASE_COMPTIME
            && phase != PARSE_SYM_PHASE_RUNTIME)) return false;
    for (size_t i = 0; i < symbol->function_binding_count; i++) {
        parse_sym_function_binding_t *old = &symbol->function_bindings[i];
        if (old->phase == phase && old->function == function) {
            old->binding = binding;
            return true;
        }
    }
    void *bindings = symbol->function_bindings;
    if (!core_list_reserve_raw(&bindings, &symbol->function_binding_capacity,
                               symbol->function_binding_count + 1,
                               sizeof(*symbol->function_bindings))) return false;
    symbol->function_bindings = bindings;
    symbol->function_bindings[symbol->function_binding_count++] =
        (parse_sym_function_binding_t){
            .phase = phase,
            .function = function,
            .binding = binding,
        };
    return true;
}

size_t
parse_sym_function_binding_count(const parse_sym_entry_t *symbol,
                                 parse_sym_phase_t phase)
{
    if (!symbol || (phase != PARSE_SYM_PHASE_COMPTIME
                    && phase != PARSE_SYM_PHASE_RUNTIME)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < symbol->function_binding_count; i++)
        count += symbol->function_bindings[i].phase == phase;
    return count;
}

const parse_sym_function_binding_t *
parse_sym_function_binding_at(const parse_sym_entry_t *symbol,
                              parse_sym_phase_t phase, size_t index)
{
    if (!symbol || (phase != PARSE_SYM_PHASE_COMPTIME
                    && phase != PARSE_SYM_PHASE_RUNTIME)) return nullptr;
    for (size_t i = 0; i < symbol->function_binding_count; i++) {
        const parse_sym_function_binding_t *binding =
            &symbol->function_bindings[i];
        if (binding->phase != phase) continue;
        if (!index--) return binding;
    }
    return nullptr;
}

void
parse_sym_set_storage_class(parse_sym_entry_t *symbol,
                            core_string_t requested, bool required,
                            core_string_t selected)
{
    if (!symbol) {
        return;
    }

    core_string_t requested_copy = requested.data
        ? core_string_dup(requested) : core_string_empty();
    core_string_t selected_copy = selected.data
        ? core_string_dup(selected) : core_string_empty();
    core_free(symbol->requested_storage_class.data);
    core_free(symbol->selected_storage_class.data);
    symbol->requested_storage_class = requested_copy;
    symbol->selected_storage_class = selected_copy;
    symbol->storage_class_required = required;
}

void
parse_sym_set_storage_duration(parse_sym_entry_t *symbol,
                              parse_sym_storage_duration_t duration)
{
    if (symbol && duration >= PARSE_SYM_STORAGE_UNSPECIFIED
        && duration <= PARSE_SYM_STORAGE_DYNAMIC)
        symbol->storage_duration = duration;
}

core_string_t
parse_sym_storage_duration_name(parse_sym_storage_duration_t duration)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("unspecified"), CORE_STRING_STATIC("comptime"),
        CORE_STRING_STATIC("automatic"),   CORE_STRING_STATIC("module"),
        CORE_STRING_STATIC("thread"),      CORE_STRING_STATIC("dynamic"),
    };
    return duration >= PARSE_SYM_STORAGE_UNSPECIFIED
        && duration <= PARSE_SYM_STORAGE_DYNAMIC ? names[duration]
        : CORE_STRING_STATIC("invalid");
}

void
parse_sym_set_lifetime(parse_sym_entry_t *symbol,
                       parse_sym_lifetime_t lifetime)
{
    if (symbol) {
        lifetime.starts = symbol->lifetime.starts;
        lifetime.start_count = symbol->lifetime.start_count;
        lifetime.start_capacity = symbol->lifetime.start_capacity;
        lifetime.ranges = symbol->lifetime.ranges;
        lifetime.range_count = symbol->lifetime.range_count;
        lifetime.range_capacity = symbol->lifetime.range_capacity;
        lifetime.ends = symbol->lifetime.ends;
        lifetime.end_count = symbol->lifetime.end_count;
        lifetime.end_capacity = symbol->lifetime.end_capacity;
        lifetime.cfg_edges = symbol->lifetime.cfg_edges;
        lifetime.cfg_edge_count = symbol->lifetime.cfg_edge_count;
        lifetime.cfg_edge_capacity = symbol->lifetime.cfg_edge_capacity;
        symbol->lifetime = lifetime;
    }
}

static parse_sym_lifetime_range_t *
ensure_lifetime_range(parse_sym_entry_t *symbol, parse_sym_phase_t phase,
                      void *function, void *binding)
{
    if (!symbol || !function || !binding
        || (phase != PARSE_SYM_PHASE_COMPTIME
            && phase != PARSE_SYM_PHASE_RUNTIME)) return nullptr;
    parse_sym_lifetime_t *lifetime = &symbol->lifetime;
    for (size_t i = 0; i < lifetime->range_count; i++) {
        parse_sym_lifetime_range_t *range = &lifetime->ranges[i];
        if (range->phase == phase && range->function == function
            && range->binding == binding) return range;
    }
    void *ranges = lifetime->ranges;
    if (!core_list_reserve_raw(&ranges, &lifetime->range_capacity,
                               lifetime->range_count + 1,
                               sizeof(*lifetime->ranges))) return nullptr;
    lifetime->ranges = ranges;
    parse_sym_lifetime_range_t *range =
        &lifetime->ranges[lifetime->range_count++];
    *range = (parse_sym_lifetime_range_t){
        .phase = phase,
        .function = function,
        .binding = binding,
    };
    return range;
}

bool
parse_sym_begin_lifetime_range(parse_sym_entry_t *symbol,
                               parse_sym_phase_t phase, void *function,
                               void *binding)
{
    parse_sym_lifetime_range_t *range = ensure_lifetime_range(
        symbol, phase, function, binding);
    if (!range) return false;
    range->complete = false;
    parse_sym_lifetime_t *lifetime = &symbol->lifetime;
    for (size_t i = 0; i < lifetime->start_count;) {
        parse_sym_lifetime_start_t *start = &lifetime->starts[i];
        if (start->phase != phase || start->function != function
            || start->binding != binding) {
            i++;
            continue;
        }
        for (size_t j = i + 1; j < lifetime->start_count; j++)
            lifetime->starts[j - 1] = lifetime->starts[j];
        lifetime->start_count--;
    }
    for (size_t i = 0; i < lifetime->cfg_edge_count;) {
        parse_sym_lifetime_edge_t *edge = &lifetime->cfg_edges[i];
        if (edge->phase != phase || edge->function != function
            || edge->binding != binding) {
            i++;
            continue;
        }
        for (size_t j = i + 1; j < lifetime->cfg_edge_count; j++)
            lifetime->cfg_edges[j - 1] = lifetime->cfg_edges[j];
        lifetime->cfg_edge_count--;
    }
    for (size_t i = 0; i < lifetime->end_count;) {
        parse_sym_lifetime_end_t *end = &lifetime->ends[i];
        if (end->phase != phase || end->function != function
            || end->binding != binding) {
            i++;
            continue;
        }
        for (size_t j = i + 1; j < lifetime->end_count; j++)
            lifetime->ends[j - 1] = lifetime->ends[j];
        lifetime->end_count--;
    }
    range->start_count = 0;
    range->edge_count = 0;
    range->end_count = 0;
    return true;
}

bool
parse_sym_finish_lifetime_range(parse_sym_entry_t *symbol,
                                parse_sym_phase_t phase, void *function,
                                void *binding, bool complete)
{
    parse_sym_lifetime_range_t *range = ensure_lifetime_range(
        symbol, phase, function, binding);
    if (!range) return false;
    range->complete = complete && range->start_count != 0
        && range->edge_count != 0;
    return !complete || range->complete;
}

size_t
parse_sym_lifetime_range_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->lifetime.range_count : 0;
}

const parse_sym_lifetime_range_t *
parse_sym_lifetime_range_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->lifetime.range_count
        ? &symbol->lifetime.ranges[index] : nullptr;
}

bool
parse_sym_set_lifetime_end(parse_sym_entry_t *symbol,
                           parse_sym_lifetime_end_t end)
{
    if (!symbol || !end.function || !end.binding
        || (end.phase != PARSE_SYM_PHASE_COMPTIME
            && end.phase != PARSE_SYM_PHASE_RUNTIME)
        || (end.on_edge
            ? (end.point || !end.from || !end.to)
            : (!end.point || end.from || end.to))) return false;
    parse_sym_lifetime_range_t *range = ensure_lifetime_range(
        symbol, end.phase, end.function, end.binding);
    if (!range) return false;
    parse_sym_lifetime_t *lifetime = &symbol->lifetime;
    for (size_t i = 0; i < lifetime->end_count; i++) {
        parse_sym_lifetime_end_t *old = &lifetime->ends[i];
        if (old->phase == end.phase && old->function == end.function
            && old->binding == end.binding && old->on_edge == end.on_edge
            && old->point == end.point && old->from == end.from
            && old->to == end.to) return true;
    }
    void *ends = lifetime->ends;
    if (!core_list_reserve_raw(&ends, &lifetime->end_capacity,
                               lifetime->end_count + 1,
                               sizeof(*lifetime->ends))) return false;
    lifetime->ends = ends;
    lifetime->ends[lifetime->end_count++] = end;
    range->end_count++;
    return true;
}

size_t
parse_sym_lifetime_end_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->lifetime.end_count : 0;
}

const parse_sym_lifetime_end_t *
parse_sym_lifetime_end_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->lifetime.end_count
        ? &symbol->lifetime.ends[index] : nullptr;
}

bool
parse_sym_set_lifetime_start(parse_sym_entry_t *symbol,
                             parse_sym_lifetime_start_t start)
{
    if (!symbol || !start.function || !start.binding || !start.point
        || (start.phase != PARSE_SYM_PHASE_COMPTIME
            && start.phase != PARSE_SYM_PHASE_RUNTIME)) return false;
    parse_sym_lifetime_range_t *range = ensure_lifetime_range(
        symbol, start.phase, start.function, start.binding);
    if (!range) return false;
    parse_sym_lifetime_t *lifetime = &symbol->lifetime;
    for (size_t i = 0; i < lifetime->start_count; i++) {
        parse_sym_lifetime_start_t *old = &lifetime->starts[i];
        if (old->phase == start.phase && old->function == start.function
            && old->binding == start.binding && old->point == start.point
            && old->at_entry == start.at_entry) return true;
    }
    void *starts = lifetime->starts;
    if (!core_list_reserve_raw(&starts, &lifetime->start_capacity,
                               lifetime->start_count + 1,
                               sizeof(*lifetime->starts))) return false;
    lifetime->starts = starts;
    lifetime->starts[lifetime->start_count++] = start;
    range->start_count++;
    return true;
}

size_t
parse_sym_lifetime_start_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->lifetime.start_count : 0;
}

const parse_sym_lifetime_start_t *
parse_sym_lifetime_start_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->lifetime.start_count
        ? &symbol->lifetime.starts[index] : nullptr;
}

bool
parse_sym_set_lifetime_edge(parse_sym_entry_t *symbol,
                            parse_sym_lifetime_edge_t edge)
{
    if (!symbol || !edge.function || !edge.binding || !edge.from
        || (edge.phase != PARSE_SYM_PHASE_COMPTIME
            && edge.phase != PARSE_SYM_PHASE_RUNTIME)
        || edge.direct > PARSE_SYM_LIVENESS_LIVE
        || edge.aliases > PARSE_SYM_LIVENESS_LIVE
        || (edge.is_exit ? edge.to != nullptr : edge.to == nullptr))
        return false;
    parse_sym_lifetime_range_t *range = ensure_lifetime_range(
        symbol, edge.phase, edge.function, edge.binding);
    if (!range) return false;
    parse_sym_lifetime_t *lifetime = &symbol->lifetime;
    for (size_t i = 0; i < lifetime->cfg_edge_count; i++) {
        parse_sym_lifetime_edge_t *old = &lifetime->cfg_edges[i];
        if (old->function == edge.function && old->binding == edge.binding
            && old->from == edge.from && old->to == edge.to
            && old->phase == edge.phase && old->is_exit == edge.is_exit) {
            *old = edge;
            return true;
        }
    }
    void *edges = lifetime->cfg_edges;
    if (!core_list_reserve_raw(&edges, &lifetime->cfg_edge_capacity,
                               lifetime->cfg_edge_count + 1,
                               sizeof(*lifetime->cfg_edges))) return false;
    lifetime->cfg_edges = edges;
    lifetime->cfg_edges[lifetime->cfg_edge_count++] = edge;
    range->edge_count++;
    return true;
}

size_t
parse_sym_lifetime_edge_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->lifetime.cfg_edge_count : 0;
}

const parse_sym_lifetime_edge_t *
parse_sym_lifetime_edge_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->lifetime.cfg_edge_count
        ? &symbol->lifetime.cfg_edges[index] : nullptr;
}

static parse_sym_alias_t *
find_alias(parse_sym_alias_t *items, size_t count, parse_sym_alias_t link)
{
    for (size_t i = 0; i < count; i++) {
        parse_sym_alias_t *old = &items[i];
        if (old->other == link.other && old->kind == link.kind
            && old->derived_at == link.derived_at) return old;
    }
    return nullptr;
}

static bool
append_alias(parse_sym_alias_t **items, size_t *count, size_t *capacity,
             parse_sym_alias_t link)
{
    parse_sym_alias_t *old = find_alias(*items, *count, link);
    if (old) {
        old->escapes |= link.escapes;
        if (link.has_last_use
            && (!old->has_last_use || link.last_use > old->last_use)) {
            old->last_use = link.last_use;
            old->has_last_use = true;
        }
        return true;
    }

    void *data = *items;
    if (!core_list_reserve_raw(&data, capacity, *count + 1,
                               sizeof(**items))) return false;
    *items = data;

    (*items)[(*count)++] = link;
    return true;
}

bool
parse_symtab_add_alias(parse_sym_entry_t *alias, parse_sym_entry_t *target,
                       parse_sym_alias_kind_t kind, uint64_t derived_at,
                       bool escapes)
{
    if (!alias || !target || alias == target
        || kind < PARSE_SYM_ALIAS_MAY || kind > PARSE_SYM_ALIAS_DERIVED) {
        return false;
    }

    parse_sym_alias_t forward = {
        .other = target,
        .kind = kind,
        .derived_at = derived_at,
        .escapes = escapes,
    };
    parse_sym_alias_t reverse = {
        .other = alias,
        .kind = kind,
        .derived_at = derived_at,
        .escapes = escapes,
    };

    if (!find_alias(alias->aliases, alias->alias_count, forward)) {
        void *data = alias->aliases;
        if (!core_list_reserve_raw(&data, &alias->alias_capacity,
                                   alias->alias_count + 1,
                                   sizeof(*alias->aliases))) return false;
        alias->aliases = data;
    }
    if (!find_alias(target->aliased_by, target->aliased_by_count, reverse)) {
        void *data = target->aliased_by;
        if (!core_list_reserve_raw(&data, &target->aliased_by_capacity,
                                   target->aliased_by_count + 1,
                                   sizeof(*target->aliased_by))) return false;
        target->aliased_by = data;
    }

    alias->alias_analysis_complete = false;
    target->alias_analysis_complete = false;
    return append_alias(&alias->aliases, &alias->alias_count,
                        &alias->alias_capacity, forward)
        && append_alias(&target->aliased_by, &target->aliased_by_count,
                        &target->aliased_by_capacity, reverse);
}

bool
parse_symtab_update_alias_use(parse_sym_entry_t *alias,
                              parse_sym_entry_t *target,
                              parse_sym_alias_kind_t kind,
                              uint64_t derived_at, uint64_t last_use,
                              bool escapes)
{
    if (last_use < derived_at
        || !parse_symtab_add_alias(alias, target, kind, derived_at, escapes)) {
        return false;
    }

    parse_sym_alias_t use = {
        .other = target,
        .kind = kind,
        .derived_at = derived_at,
        .last_use = last_use,
        .has_last_use = true,
        .escapes = escapes,
    };
    parse_sym_alias_t reverse = use;
    reverse.other = alias;
    return append_alias(&alias->aliases, &alias->alias_count,
                        &alias->alias_capacity, use)
        && append_alias(&target->aliased_by, &target->aliased_by_count,
                        &target->aliased_by_capacity, reverse);
}

void
parse_sym_set_alias_analysis_complete(parse_sym_entry_t *symbol,
                                      bool complete)
{
    if (symbol) symbol->alias_analysis_complete = complete;
}

static bool
alias_set_add(core_list_t(sym_entry_ptr_t) *symbols, parse_sym_entry_t *symbol)
{
    return core_option_is_set(core_list_find(*symbols, symbol))
        || core_list_try_push(*symbols, symbol);
}

static bool
aliases_expire_before(const parse_sym_alias_t *aliases, size_t count,
                      uint64_t point,
                      core_list_t(sym_entry_ptr_t) *work)
{
    for (size_t i = 0; i < count; i++) {
        const parse_sym_alias_t *alias = &aliases[i];
        if (!alias->other || alias->escapes || !alias->has_last_use
            || alias->last_use >= point
            || !alias_set_add(work, alias->other)) {
            return false;
        }
    }
    return true;
}

bool
parse_sym_can_finalize_at(const parse_sym_entry_t *symbol, uint64_t point)
{
    if (!symbol) return false;

    core_list_t(sym_entry_ptr_t) work = {0};
    size_t next = 0;
    bool safe = alias_set_add(&work, (parse_sym_entry_t *)symbol);

    while (safe && next < work.len) {
        parse_sym_entry_t *at = work.data[next++];
        if (!at->alias_analysis_complete || at->alias_unresolved
            || !at->lifetime.has_end
            || (at->lifetime.has_start && point < at->lifetime.start_point)
            || point < at->lifetime.end_point) {
            safe = false;
            break;
        }
        safe = aliases_expire_before(at->aliases, at->alias_count, point, &work)
            && aliases_expire_before(at->aliased_by, at->aliased_by_count,
                                     point, &work);
    }

    core_list_free(work);
    return safe;
}

size_t
parse_sym_alias_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->alias_count : 0;
}

const parse_sym_alias_t *
parse_sym_alias_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->alias_count
        ? &symbol->aliases[index] : nullptr;
}

size_t
parse_sym_aliased_by_count(const parse_sym_entry_t *symbol)
{
    return symbol ? symbol->aliased_by_count : 0;
}

const parse_sym_alias_t *
parse_sym_aliased_by_at(const parse_sym_entry_t *symbol, size_t index)
{
    return symbol && index < symbol->aliased_by_count
        ? &symbol->aliased_by[index] : nullptr;
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
        parse_namespace_t *ns = find_ns(st, ns_name);
        parse_scope_t *peer = scope->ns == ns
            ? scope : parse_symtab_scope_in_namespace(st, scope, ns_name);
        return peer ? lookup_chain(peer, name) : nullptr;
    }

    parse_namespace_t *ns = find_ns(st, ns_name);

    return ns ? parse_symtab_lookup_in(ns->root, name) : nullptr;
}

bool
parse_sym_visible_at(const parse_sym_entry_t *symbol, parse_tree_t *use)
{
    if (!symbol || !symbol->initializer_node) return true;
    for (parse_tree_t *at = use; at && !core_tree_is_leaf(at);
         at = parse_tree_parent(at))
        if (at == symbol->initializer_node) return false;
    return true;
}

parse_sym_entry_t *
parse_symtab_lookup_visible(parse_symtab_t *st, parse_scope_t *scope,
                            core_string_t ns_name, core_string_t name,
                            parse_tree_t *use)
{
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(st, scope,
                                                           ns_name, name);
    while (symbol && !parse_sym_visible_at(symbol, use))
        symbol = symbol->shadowed;
    return symbol;
}

parse_sym_entry_t *
parse_symtab_resolve_node(parse_symtab_t *st, parse_tree_t *node,
                          core_string_t ns_name)
{
    if (!st || !node || core_tree_is_leaf(node)) return nullptr;

    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        return resolution.target.symbol;
    parse_sym_entry_t *bound = parse_tree_sym(node);
    if (resolution.kind != PARSE_NODE_RESOLUTION_SCOPE && bound) {
        parse_tree_set_resolution_symbol(node, bound);
        return bound;
    }

    core_string_t name = parse_tree_unresolved_name(node);
    if (!name.data || !name.u8_bytes) return nullptr;

    parse_scope_t *scope = resolution.kind == PARSE_NODE_RESOLUTION_SCOPE
        ? resolution.target.scope : parse_tree_scope(node);
    if (!scope) scope = parse_symtab_scope_for_node(st, node);
    parse_sym_entry_t *symbol = parse_symtab_lookup_visible(
        st, scope, ns_name, name, node);
    if (symbol) {
        parse_tree_set_resolution_symbol(node, symbol);
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

void
parse_symtab_set_scope_node_all(parse_symtab_t *st, parse_tree_t *node)
{
    if (!st || !node) return;
    parse_namespace_t *default_ns = find_ns(st, core_string_empty());
    if (!default_ns || !default_ns->current) return;
    parse_scope_t *lexical = default_ns->current;
    for (int32_t i = 0; i < st->ns_count; i++) {
        parse_scope_t *peer = scope_for_lexical(st->namespaces[i], lexical);
        if (peer) peer->node = node;
    }
    core_dict_put(st->scope_by_node, node, lexical);
}

parse_scope_t *
parse_symtab_scope_for_node(parse_symtab_t *st, parse_tree_t *node)
{
    if (!st || !node) {
        return nullptr;
    }

    parse_scope_t *scope = parse_tree_scope(node);
    if (scope) return scope;

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
