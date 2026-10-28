#include "parse/rewrite.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"


parse_tree_t *
parse_tree_clone(const parse_tree_t *tree)
{
    if (!tree) {
        return nullptr;
    }

    if (core_tree_is_leaf(tree)) {
        parse_token_info_t *src = core_tree_leaf_value(tree);

        return parse_tree_new_owned_leaf(parse_token_copy(src, true));
    }

    parse_nt_node_t     pn  = core_tree_node_value(tree);
    pn.semantic.facts = nullptr;
    parse_tree_t *out = core_tree_node(parse_nt_node_t,
                                          parse_token_info_ptr_t,
                                          pn);
    parse_tree_copy_semantic(out, tree);
    size_t            nc  = core_tree_num_children(tree);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *kid = parse_tree_clone(core_tree_child(tree, i));

        if (kid) {
            core_tree_add_child(out, kid);
        }
    }

    return out;
}

parse_tree_t *
parse_tree_make_nt(parse_grammar_t *g, core_string_t nt_name, int32_t rule_index)
{
    parse_nonterm_t *nt = g ? parse_find_nonterm(g, nt_name) : nullptr;

    if (!nt) {
        return nullptr;
    }

    parse_nt_node_t pn = {0};

    pn.name       = nt->name;
    pn.id         = nt->id;
    pn.rule_index = rule_index;

    return core_tree_node(parse_nt_node_t, parse_token_info_ptr_t, pn);
}

parse_tree_t *
parse_tree_make_synthetic_token(int64_t tid, core_string_t text)
{
    parse_token_info_t *tok = parse_token_new();

    parse_token_set_tid(tok, (int32_t)tid);
    parse_token_set_origin(tok, PARSE_TOKEN_SYNTHETIC);

    if (text.data) {
        parse_token_set_value(tok,
                              core_option_set(core_string_t,
                                              core_string_dup(text)),
                              true);
    }

    return parse_tree_new_owned_leaf(tok);
}
