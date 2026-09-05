#include "parse/tree_util.h"
#include "internal/parse/grammar_internal.h"

ncc_string_t
ncc_tree_extract_first_identifier(ncc_parse_tree_t *node)
{
    ncc_string_t none = {0};

    if (!node) {
        return none;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);

        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);

            if (val.u8_bytes > 0) {
                return val;
            }
        }

        return none;
    }

    size_t nc = ncc_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        ncc_string_t s = ncc_tree_extract_first_identifier(
            ncc_tree_child(node, i));

        if (s.u8_bytes > 0) {
            return s;
        }
    }

    return none;
}

ncc_token_info_t *
ncc_tree_find_first_terminal(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_tree_leaf_value(node);
    }

    size_t nc = ncc_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *tok
            = ncc_tree_find_first_terminal(ncc_tree_child(node, i));

        if (tok) {
            return tok;
        }
    }

    return nullptr;
}

ncc_parse_tree_t *
ncc_tree_find_child_by_nt_name(ncc_grammar_t *g, ncc_parse_tree_t *parent,
                               ncc_string_t name)
{
    if (!parent) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(parent, i);

        if (ncc_tree_is_leaf(child)) {
            continue;
        }

        ncc_nt_node_t *cpn = &ncc_tree_node_value(child);

        if (cpn->group_top) {
            ncc_parse_tree_t *found
                = ncc_tree_find_child_by_nt_name(g, child, name);

            if (found) {
                return found;
            }

            continue;
        }

        if (cpn->id >= 0) {
            ncc_nonterm_t *nt = ncc_get_nonterm(g, cpn->id);

            if (nt && ncc_string_eq(nt->name, name)) {
                return child;
            }
        }
    }

    return nullptr;
}

ncc_parse_tree_t *
ncc_tree_get_nth_nt_child(ncc_parse_tree_t *node, int32_t n)
{
    if (!node) {
        return nullptr;
    }

    size_t  nc   = ncc_tree_num_children(node);
    int32_t seen = 0;

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);

        if (ncc_tree_is_leaf(child)) {
            continue;
        }

        ncc_nt_node_t *cpn = &ncc_tree_node_value(child);

        if (cpn->group_top) {
            size_t gnc = ncc_tree_num_children(child);

            for (size_t j = 0; j < gnc; j++) {
                ncc_parse_tree_t *gchild = ncc_tree_child(child, j);

                if (!ncc_tree_is_leaf(gchild)) {
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

    return nullptr;
}

ncc_parse_tree_t *
ncc_tree_resolve_child_ref(ncc_grammar_t *g, ncc_parse_tree_t *parent,
                           ncc_child_ref_t ref)
{
    if (!parent) {
        return nullptr;
    }

    switch (ref.kind) {
    case NCC_ROLE_BY_INDEX:
        if (ref.index < 0
            || (size_t)ref.index >= ncc_tree_num_children(parent)) {
            return nullptr;
        }

        return ncc_tree_child(parent, ref.index);

    case NCC_ROLE_BY_NAME:
        return ncc_tree_find_child_by_nt_name(g, parent, ref.name);
    }

    return nullptr;
}
