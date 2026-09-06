#include "parse/tree_util.h"
#include "internal/parse/grammar_internal.h"
#include "lib/list.h"

bool
ncc_alt_leads_with(ncc_grammar_t *g, ncc_string_t nt_name, int32_t rule_ix,
                   ncc_string_t word)
{
    ncc_nonterm_t *nt = ncc_find_nonterm(g, nt_name);

    if (!nt || rule_ix < 0 || (size_t)rule_ix >= ncc_list_len(nt->rule_ids)) {
        return false;
    }

    ncc_parse_rule_t *rule = ncc_get_rule(g, nt->rule_ids.data[rule_ix]);

    if (!rule || !ncc_list_len(rule->contents)
        || rule->contents.data[0].kind != NCC_MATCH_TERMINAL) {
        return false;
    }

    return rule->contents.data[0].terminal_id
        == ncc_lookup_terminal(g, word);
}

ncc_list_decl(ncc_parse_tree_ptr_t);

ncc_parse_tree_t *
ncc_tree_scan_leaves(ncc_parse_tree_t *root, bool reverse, ncc_leaf_fn fn,
                     void *arg)
{
    if (!root) {
        return nullptr;
    }

    ncc_list_t(ncc_parse_tree_ptr_t) stack = ncc_list_new(ncc_parse_tree_ptr_t);
    ncc_parse_tree_t *hit = nullptr;

    ncc_list_push(stack, root);

    while (stack.len) {
        ncc_parse_tree_t *n = stack.data[--stack.len];

        if (ncc_tree_is_leaf(n)) {
            if (!fn || fn(n, arg)) {
                hit = n;
                break;
            }

            continue;
        }

        size_t nc = ncc_tree_num_children(n);

        for (size_t i = 0; i < nc; i++) {
            ncc_list_push(stack, ncc_tree_child(n, reverse ? i : nc - 1 - i));
        }
    }

    ncc_list_free(stack);

    return hit;
}

static bool has_text(ncc_parse_tree_t *leaf, void *arg)
{
    (void)arg;

    ncc_token_info_t *tok = ncc_tree_leaf_value(leaf);

    return tok && ncc_option_is_set(tok->value)
        && ncc_option_get(tok->value).u8_bytes > 0;
}

ncc_string_t
ncc_tree_extract_first_identifier(ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *hit = ncc_tree_scan_leaves(node, false, has_text,
                                                 nullptr);

    if (!hit) {
        return (ncc_string_t){0};
    }

    return ncc_option_get(ncc_tree_leaf_value(hit)->value);
}

ncc_token_info_t *
ncc_tree_find_first_terminal(ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *hit = ncc_tree_scan_leaves(node, false, nullptr,
                                                 nullptr);

    return hit ? ncc_tree_leaf_value(hit) : nullptr;
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

ncc_string_t ncc_tree_nt_name(ncc_grammar_t *g, ncc_parse_tree_t *n)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return (ncc_string_t){0};
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(n);

    if (pn->id < 0 || pn->group_top) {
        return (ncc_string_t){0};
    }

    ncc_nonterm_t *nt = ncc_get_nonterm(g, pn->id);

    return nt ? nt->name : (ncc_string_t){0};
}

bool ncc_tree_is_nt(ncc_grammar_t *g, ncc_parse_tree_t *n, ncc_string_t name)
{
    return ncc_string_eq(ncc_tree_nt_name(g, n), name);
}

ncc_string_t ncc_tree_leaf_text(ncc_parse_tree_t *n)
{
    ncc_string_t none = {0};

    if (!n || !ncc_tree_is_leaf(n)) {
        return none;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(n);

    if (!tok || !ncc_option_is_set(tok->value)) {
        return none;
    }

    return ncc_option_get(tok->value);
}

int64_t ncc_tree_leaf_tid(ncc_parse_tree_t *n)
{
    if (!n || !ncc_tree_is_leaf(n)) {
        return 0;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(n);

    return tok ? tok->tid : 0;
}

bool ncc_tree_leaf_is(ncc_parse_tree_t *n, ncc_string_t text)
{
    return ncc_string_eq(ncc_tree_leaf_text(n), text);
}
