
#include "parse/tree.h"
#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/list.h"

bool
parse_node_is_token(parse_tree_t *t)
{
    return t && core_tree_is_leaf(t);
}

parse_token_info_t *
parse_node_token(parse_tree_t *t)
{
    if (!t || !core_tree_is_leaf(t)) {
        return nullptr;
    }

    return core_tree_leaf_value(t);
}

core_option_t(core_string_t)
parse_node_name(parse_tree_t *t)
{
    if (!t || core_tree_is_leaf(t)) {
        return core_option_none(core_string_t);
    }

    return core_option_set(core_string_t, core_tree_node_value(t).name);
}

core_list_decl(parse_tree_ptr_t);

static void *
run_action(parse_reduce_t *r)
{
    if (core_tree_is_leaf(r->node)) {
        return core_tree_leaf_value(r->node);
    }

    parse_nt_node_t *pn = &core_tree_node_value(r->node);

    if (pn->id == PARSE_EMPTY_STRING) {
        return nullptr;
    }

    void **kids = nullptr;

    if (r->n_kids) {
        kids = core_alloc_array(void *, r->n_kids);

        for (size_t i = 0; i < r->n_kids; i++) {
            kids[i] = r->kids[i];
        }
    }

    if (pn->group_top) {
        return kids;
    }

    parse_nonterm_t    *nt     = parse_get_nonterm(r->grammar, pn->id);
    parse_walk_action_t action = (nt && nt->action) ? nt->action
                                                  : r->grammar->default_action;

    if (!action) {
        return kids;
    }

    return (*action)(pn, kids, r->thunk);
}

void *
parse_tree_walk(parse_grammar_t *g, parse_tree_t *node, void *thunk)
{
    if (!node) {
        return nullptr;
    }

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    parse_walk_reduce_default(w, run_action, thunk);
    parse_walk_reduce_leaf(w, run_action, thunk);

    void *out = parse_tree_walk_reduce(w, node);

    parse_tree_walk_free(w);

    return out;
}

void
parse_tree_free(parse_tree_t *t)
{
    if (!t) {
        return;
    }

    core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);

    core_list_push(stack, t);

    while (stack.len) {
        parse_tree_t *n = stack.data[--stack.len];

        if (!n->is_leaf) {
            for (size_t i = 0; i < n->node.num_children; i++) {
                core_list_push(stack, n->node.children[i]);
            }
        }
        else {
            parse_token_free_from_tree(core_tree_leaf_value(n));
        }

        parse_tree_free_node(n);
    }

    core_list_free(stack);
}

void
parse_tree_free_node(parse_tree_t *t)
{
    if (!t) return;
    parse_node_semantic_t *semantic = parse_tree_semantic(t);
    if (semantic) {
        core_free(semantic->facts);
        semantic->facts = nullptr;
    }
    core_tree_free_node(t);
}

parse_node_facts_t *
parse_tree_facts_for_update(parse_tree_t *node)
{
    parse_node_semantic_t *semantic = parse_tree_semantic(node);
    if (!semantic) return nullptr;
    if (!semantic->facts) semantic->facts = core_alloc(parse_node_facts_t);
    return semantic->facts;
}

void
parse_tree_copy_semantic(parse_tree_t *dst, const parse_tree_t *src)
{
    parse_node_semantic_t *to = parse_tree_semantic(dst);
    parse_node_semantic_t *from = parse_tree_semantic(src);
    if (!to || !from || to == from) return;
    parse_node_facts_t *facts = to->facts;
    *to = *from;
    to->facts = facts;
    if (!from->facts) {
        core_free(to->facts);
        to->facts = nullptr;
        return;
    }
    if (!to->facts) to->facts = core_alloc(parse_node_facts_t);
    *to->facts = *from->facts;
}
