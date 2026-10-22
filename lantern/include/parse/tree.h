#pragma once

#include "parse/types.h"
#include "lib/tree.h"

core_tree_decl(parse_nt_node_t, parse_token_info_ptr_t);

typedef core_tree_t(parse_nt_node_t, parse_token_info_ptr_t) parse_tree_t;

typedef parse_tree_t *parse_tree_ptr_t;

core_array_decl(parse_tree_ptr_t);

typedef core_array_t(parse_tree_ptr_t) parse_tree_array_t;

bool parse_node_is_token(parse_tree_t *t);

parse_token_info_t *parse_node_token(parse_tree_t *t);

core_option_t(core_string_t) parse_node_name(parse_tree_t *t);

void *parse_tree_walk(parse_grammar_t *g, parse_tree_t *node, void *thunk);

void parse_tree_free(parse_tree_t *t);

void parse_tree_free_node(parse_tree_t *t);

parse_node_facts_t *parse_tree_facts_for_update(parse_tree_t *node);

void parse_tree_copy_semantic(parse_tree_t *dst, const parse_tree_t *src);

static inline parse_node_semantic_t *
parse_tree_semantic(const parse_tree_t *node)
{
    return node && !node->is_leaf
        ? &((parse_tree_t *)node)->node.value.semantic : nullptr;
}

static inline bool
parse_tree_is_group(const parse_tree_t *node)
{
    return node && !node->is_leaf && node->node.value.group_top;
}

static inline const parse_node_facts_t *
parse_tree_facts(const parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    return semantic ? semantic->facts : nullptr;
}

static inline parse_tree_t *
parse_tree_parent(const parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    return semantic ? semantic->parent : nullptr;
}

static inline bool
parse_tree_set_parent(parse_tree_t *node, parse_tree_t *parent)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    if (!semantic) return false;
    semantic->parent = parent;
    return true;
}

static inline parse_scope_t *
parse_tree_scope(const parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    return semantic ? semantic->scope : nullptr;
}

static inline bool
parse_tree_set_scope(parse_tree_t *node, parse_scope_t *scope)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    if (!semantic) return false;
    semantic->scope = scope;
    return true;
}

static inline th_type *
parse_tree_type(const parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    return semantic ? semantic->type : nullptr;
}

static inline bool
parse_tree_set_type(parse_tree_t *node, th_type *type)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    if (!semantic) return false;
    semantic->type = type;
    return true;
}

static inline void *
parse_tree_frontend_data(const parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    return semantic ? semantic->frontend_data : nullptr;
}

static inline bool
parse_tree_set_frontend_data(parse_tree_t *node, void *data)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    if (!semantic) return false;
    semantic->frontend_data = data;
    return true;
}

static inline parse_sym_entry_t *
parse_tree_sym(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->sym : nullptr;
}

static inline bool
parse_tree_set_sym(parse_tree_t *node, parse_sym_entry_t *sym)
{
    if (!sym && !parse_tree_facts(node)) return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->sym = sym;
    return true;
}

static inline th_type *
parse_tree_argument_type(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->argument_type : nullptr;
}

static inline bool
parse_tree_set_argument_type(parse_tree_t *node, th_type *type)
{
    if (!type && !parse_tree_facts(node)) return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->argument_type = type;
    return true;
}

static inline th_type *
parse_tree_error_type(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->error_type : nullptr;
}

static inline bool
parse_tree_set_error_type(parse_tree_t *node, th_type *error_type)
{
    if (!error_type && !parse_tree_facts(node))
        return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->error_type = error_type;
    return true;
}

static inline core_string_t
parse_tree_unresolved_name(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->unresolved_name : (core_string_t){0};
}

static inline bool
parse_tree_set_unresolved_name(parse_tree_t *node, core_string_t name)
{
    if (!name.data && !parse_tree_facts(node))
        return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->unresolved_name = name;
    return true;
}

static inline parse_node_resolution_t
parse_tree_resolution(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->resolution : (parse_node_resolution_t){0};
}

static inline bool
parse_tree_set_resolution_scope(parse_tree_t *node, parse_scope_t *scope)
{
    if (!scope) return false;
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->resolution = (parse_node_resolution_t){
        .kind = PARSE_NODE_RESOLUTION_SCOPE,
        .target.scope = scope,
    };
    facts->sym = nullptr;
    return true;
}

static inline bool
parse_tree_set_resolution_symbol(parse_tree_t *node,
                                 parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->resolution = (parse_node_resolution_t){
        .kind = PARSE_NODE_RESOLUTION_SYMBOL,
        .target.symbol = symbol,
    };
    facts->sym = symbol;
    return true;
}

static inline parse_sym_entry_t *
parse_tree_reference_target(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->reference_target : nullptr;
}

static inline bool
parse_tree_set_reference_target(parse_tree_t *node,
                                parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->reference_target = symbol;
    return true;
}

static inline bool
parse_tree_is_direct_symbol_reference(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts && facts->direct_symbol_reference;
}

static inline bool
parse_tree_mark_direct_symbol_reference(parse_tree_t *node)
{
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->direct_symbol_reference = true;
    return true;
}

static inline bool
parse_tree_is_index_address_access(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts && facts->index_address_access;
}

static inline bool
parse_tree_set_index_address_access(parse_tree_t *node, bool access)
{
    if (!access && !parse_tree_facts(node)) return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->index_address_access = access;
    return true;
}

static inline void *
parse_tree_integer_literal_origin(const parse_tree_t *node)
{
    const parse_node_facts_t *facts = parse_tree_facts(node);
    return facts ? facts->integer_literal_origin : nullptr;
}

static inline bool
parse_tree_set_integer_literal_origin(parse_tree_t *node, void *origin)
{
    if (!origin && !parse_tree_facts(node)) return parse_tree_semantic(node);
    parse_node_facts_t *facts = parse_tree_facts_for_update(node);
    if (!facts) return false;
    facts->integer_literal_origin = origin;
    return true;
}
