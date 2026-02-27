// tree_util.c - Shared parse-tree helpers for annotation walk and type inference.

#include "slay/tree_util.h"
#include "internal/slay/grammar_internal.h"
#include "strings/string_ops.h"

// ============================================================================
// extract_first_identifier
// ============================================================================

n00b_string_t
n00b_tree_extract_first_identifier(n00b_parse_tree_t *node)
{
    if (!node) {
        return n00b_string_empty();
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t val = n00b_option_get(tok->value);

            if (val.u8_bytes > 0) {
                return val;
            }
        }

        return n00b_string_empty();
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_string_t s = n00b_tree_extract_first_identifier(
            n00b_tree_child(node, i));

        if (s.u8_bytes > 0) {
            return s;
        }
    }

    return n00b_string_empty();
}

// ============================================================================
// find_child_by_nt_name
// ============================================================================

n00b_parse_tree_t *
n00b_tree_find_child_by_nt_name(n00b_grammar_t *g, n00b_parse_tree_t *parent,
                                n00b_string_t name)
{
    size_t nc = n00b_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(parent, i);

        if (n00b_tree_is_leaf(child)) {
            continue;
        }

        n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

        if (cpn->group_top) {
            n00b_parse_tree_t *found
                = n00b_tree_find_child_by_nt_name(g, child, name);

            if (found) {
                return found;
            }

            continue;
        }

        if (cpn->id >= 0) {
            n00b_nonterm_t *nt = n00b_get_nonterm(g, cpn->id);

            if (nt && n00b_unicode_str_eq(nt->name, name)) {
                return child;
            }
        }
    }

    return NULL;
}

// ============================================================================
// find_first_terminal
// ============================================================================

n00b_token_info_t *
n00b_tree_find_first_terminal(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }

    if (n00b_tree_is_leaf(node)) {
        return n00b_tree_leaf_value(node);
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_token_info_t *tok
            = n00b_tree_find_first_terminal(n00b_tree_child(node, i));

        if (tok) {
            return tok;
        }
    }

    return NULL;
}

// ============================================================================
// resolve_child_ref
// ============================================================================

n00b_parse_tree_t *
n00b_tree_resolve_child_ref(n00b_grammar_t *g, n00b_parse_tree_t *parent,
                            n00b_child_ref_t ref)
{
    switch (ref.kind) {
    case N00B_ROLE_BY_INDEX:
        if (ref.index < 0) {
            return NULL;
        }

        if ((size_t)ref.index >= n00b_tree_num_children(parent)) {
            return NULL;
        }

        return n00b_tree_child(parent, ref.index);

    case N00B_ROLE_BY_NAME:
        return n00b_tree_find_child_by_nt_name(g, parent, ref.name);
    }

    return NULL;
}

// ============================================================================
// get_nth_nt_child
// ============================================================================

n00b_parse_tree_t *
n00b_tree_get_nth_nt_child(n00b_parse_tree_t *node, int32_t n)
{
    size_t  nc   = n00b_tree_num_children(node);
    int32_t seen = 0;

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_tree_is_leaf(child)) {
            continue;
        }

        n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

        if (cpn->group_top) {
            size_t gnc = n00b_tree_num_children(child);

            for (size_t j = 0; j < gnc; j++) {
                n00b_parse_tree_t *gchild = n00b_tree_child(child, j);

                if (!n00b_tree_is_leaf(gchild)) {
                    if (seen == n) {
                        return gchild;
                    }

                    seen++;
                }
            }

            continue;
        }

        if (seen == n) {
            return child;
        }

        seen++;
    }

    return NULL;
}
