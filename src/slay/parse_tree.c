// parse_tree.c - Shared parse tree operations (engine-agnostic).

#include "slay/parse_tree.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"

// ============================================================================
// Node inspection
// ============================================================================

bool
n00b_parse_node_is_token(n00b_parse_tree_t *t)
{
    return t && n00b_tree_is_leaf(t);
}

n00b_token_info_t *
n00b_parse_node_token(n00b_parse_tree_t *t)
{
    if (!t || !n00b_tree_is_leaf(t)) {
        return NULL;
    }

    return n00b_tree_leaf_value(t);
}

n00b_option_t(n00b_string_t)
n00b_parse_node_name(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return n00b_option_none(n00b_string_t);
    }

    return n00b_option_set(n00b_string_t, n00b_tree_node_value(t).name);
}

// ============================================================================
// Tree walking
// ============================================================================

void *
n00b_parse_tree_walk(n00b_grammar_t *g, n00b_parse_tree_t *node, void *thunk)
{
    if (!node) {
        return NULL;
    }

    if (n00b_tree_is_leaf(node)) {
        return n00b_tree_leaf_value(node);
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    if (pn->id == N00B_EMPTY_STRING) {
        return NULL;
    }

    size_t nc = n00b_tree_num_children(node);

    void **sub_results = NULL;

    if (nc) {
        sub_results = n00b_alloc_array(void *, nc);

        for (size_t i = 0; i < nc; i++) {
            sub_results[i] = n00b_parse_tree_walk(g,
                                                   n00b_tree_child(node, i),
                                                   thunk);
        }
    }

    // Group wrapper nodes (from BNF ?, *, +) have pn->id = N00B_GROUP_ID
    // which won't resolve to a real nonterm. Skip the action lookup.
    if (pn->group_top) {
        return sub_results;
    }

    n00b_nonterm_t    *nt     = n00b_get_nonterm(g, pn->id);
    n00b_walk_action_t action = (nt && nt->action) ? nt->action
                                                    : g->default_action;

    if (!action) {
        return sub_results;
    }

    return (*action)(pn, sub_results, thunk);
}

// ============================================================================
// Tree free
// ============================================================================

void
n00b_parse_tree_free(n00b_parse_tree_t *t)
{
    if (!t) {
        return;
    }

    if (!t->is_leaf) {
        for (size_t i = 0; i < t->node.num_children; i++) {
            n00b_parse_tree_free(t->node.children[i]);
        }
    }

    n00b_tree_free_node(t);
}
