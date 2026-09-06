
#include "parse/parse_tree.h"
#include "parse/tree_visit.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/list.h"

bool
ncc_parse_node_is_token(ncc_parse_tree_t *t)
{
    return t && ncc_tree_is_leaf(t);
}

ncc_token_info_t *
ncc_parse_node_token(ncc_parse_tree_t *t)
{
    if (!t || !ncc_tree_is_leaf(t)) {
        return nullptr;
    }

    return ncc_tree_leaf_value(t);
}

ncc_option_t(ncc_string_t)
ncc_parse_node_name(ncc_parse_tree_t *t)
{
    if (!t || ncc_tree_is_leaf(t)) {
        return ncc_option_none(ncc_string_t);
    }

    return ncc_option_set(ncc_string_t, ncc_tree_node_value(t).name);
}

ncc_list_decl(ncc_parse_tree_ptr_t);

static void *
run_action(ncc_reduce_t *r)
{
    if (ncc_tree_is_leaf(r->node)) {
        return ncc_tree_leaf_value(r->node);
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(r->node);

    if (pn->id == NCC_EMPTY_STRING) {
        return nullptr;
    }

    void **kids = nullptr;

    if (r->n_kids) {
        kids = ncc_alloc_array(void *, r->n_kids);

        for (size_t i = 0; i < r->n_kids; i++) {
            kids[i] = r->kids[i];
        }
    }

    if (pn->group_top) {
        return kids;
    }

    ncc_nonterm_t    *nt     = ncc_get_nonterm(r->grammar, pn->id);
    ncc_walk_action_t action = (nt && nt->action) ? nt->action
                                                  : r->grammar->default_action;

    if (!action) {
        return kids;
    }

    return (*action)(pn, kids, r->thunk);
}

void *
ncc_parse_tree_walk(ncc_grammar_t *g, ncc_parse_tree_t *node, void *thunk)
{
    if (!node) {
        return nullptr;
    }

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, nullptr);

    ncc_walk_reduce_default(w, run_action, thunk);
    ncc_walk_reduce_leaf(w, run_action, thunk);

    void *out = ncc_tree_walk_reduce(w, node);

    ncc_tree_walk_free(w);

    return out;
}

void
ncc_parse_tree_free(ncc_parse_tree_t *t)
{
    if (!t) {
        return;
    }

    ncc_list_t(ncc_parse_tree_ptr_t) stack = ncc_list_new(ncc_parse_tree_ptr_t);

    ncc_list_push(stack, t);

    while (stack.len) {
        ncc_parse_tree_t *n = stack.data[--stack.len];

        if (!n->is_leaf) {
            for (size_t i = 0; i < n->node.num_children; i++) {
                ncc_list_push(stack, n->node.children[i]);
            }
        }

        ncc_tree_free_node(n);
    }

    ncc_list_free(stack);
}
