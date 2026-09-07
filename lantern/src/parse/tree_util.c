#include "parse/tree_util.h"
#include "private/parse/grammar.h"
#include "lib/list.h"

bool
parse_alt_leads_with(parse_grammar_t *g, core_string_t nt_name, int32_t rule_ix,
                   core_string_t word)
{
    parse_nonterm_t *nt = parse_find_nonterm(g, nt_name);

    if (!nt || rule_ix < 0 || (size_t)rule_ix >= core_list_len(nt->rule_ids)) {
        return false;
    }

    parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[rule_ix]);

    if (!rule || !core_list_len(rule->contents)
        || rule->contents.data[0].kind != PARSE_MATCH_TERMINAL) {
        return false;
    }

    return rule->contents.data[0].terminal_id
        == parse_lookup_terminal(g, word);
}

core_list_decl(parse_tree_ptr_t);

parse_tree_t *
parse_tree_scan_leaves(parse_tree_t *root, bool reverse, parse_leaf_fn fn,
                     void *arg)
{
    if (!root) {
        return nullptr;
    }

    core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
    parse_tree_t *hit = nullptr;

    core_list_push(stack, root);

    while (stack.len) {
        parse_tree_t *n = stack.data[--stack.len];

        if (core_tree_is_leaf(n)) {
            if (!fn || fn(n, arg)) {
                hit = n;
                break;
            }

            continue;
        }

        size_t nc = core_tree_num_children(n);

        for (size_t i = 0; i < nc; i++) {
            core_list_push(stack, core_tree_child(n, reverse ? i : nc - 1 - i));
        }
    }

    core_list_free(stack);

    return hit;
}

static bool has_text(parse_tree_t *leaf, void *arg)
{
    (void)arg;

    parse_token_info_t *tok = core_tree_leaf_value(leaf);

    return tok && core_option_is_set(tok->value)
        && core_option_get(tok->value).u8_bytes > 0;
}

core_string_t
parse_tree_extract_first_identifier(parse_tree_t *node)
{
    parse_tree_t *hit = parse_tree_scan_leaves(node, false, has_text,
                                                 nullptr);

    if (!hit) {
        return (core_string_t){0};
    }

    return core_option_get(core_tree_leaf_value(hit)->value);
}

parse_token_info_t *
parse_tree_find_first_terminal(parse_tree_t *node)
{
    parse_tree_t *hit = parse_tree_scan_leaves(node, false, nullptr,
                                                 nullptr);

    return hit ? core_tree_leaf_value(hit) : nullptr;
}

static parse_tree_t *
find_child_by_nt_id(parse_tree_t *parent, int64_t want)
{
    size_t nc = core_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *child = core_tree_child(parent, i);

        if (core_tree_is_leaf(child)) {
            continue;
        }

        parse_nt_node_t *cpn = &core_tree_node_value(child);

        if (cpn->group_top) {
            parse_tree_t *found = find_child_by_nt_id(child, want);

            if (found) {
                return found;
            }

            continue;
        }

        if (cpn->id == want) {
            return child;
        }
    }

    return nullptr;
}

parse_tree_t *
parse_tree_find_child_by_nt_name(parse_grammar_t *g, parse_tree_t *parent,
                               core_string_t name)
{
    if (!parent) {
        return nullptr;
    }

    parse_nonterm_t *want = parse_find_nonterm(g, name);

    return want ? find_child_by_nt_id(parent, want->id) : nullptr;
}

parse_tree_t *
parse_tree_get_nth_nt_child(parse_tree_t *node, int32_t n)
{
    if (!node) {
        return nullptr;
    }

    size_t  nc   = core_tree_num_children(node);
    int32_t seen = 0;

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *child = core_tree_child(node, i);

        if (core_tree_is_leaf(child)) {
            continue;
        }

        parse_nt_node_t *cpn = &core_tree_node_value(child);

        if (cpn->group_top) {
            size_t gnc = core_tree_num_children(child);

            for (size_t j = 0; j < gnc; j++) {
                parse_tree_t *gchild = core_tree_child(child, j);

                if (!core_tree_is_leaf(gchild)) {
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

parse_tree_t *
parse_tree_resolve_child_ref(parse_grammar_t *g, parse_tree_t *parent,
                           parse_child_ref_t ref)
{
    if (!parent) {
        return nullptr;
    }

    switch (ref.kind) {
    case PARSE_ROLE_BY_INDEX:
        if (ref.index < 0
            || (size_t)ref.index >= core_tree_num_children(parent)) {
            return nullptr;
        }

        return core_tree_child(parent, ref.index);

    case PARSE_ROLE_BY_NAME:
        return parse_tree_find_child_by_nt_name(g, parent, ref.name);
    }

    return nullptr;
}

core_string_t parse_tree_nt_name(parse_grammar_t *g, parse_tree_t *n)
{
    if (!n || core_tree_is_leaf(n)) {
        return (core_string_t){0};
    }

    parse_nt_node_t *pn = &core_tree_node_value(n);

    if (pn->id < 0 || pn->group_top) {
        return (core_string_t){0};
    }

    parse_nonterm_t *nt = parse_get_nonterm(g, pn->id);

    return nt ? nt->name : (core_string_t){0};
}

bool parse_tree_is_nt(parse_grammar_t *g, parse_tree_t *n, core_string_t name)
{
    return core_string_eq(parse_tree_nt_name(g, n), name);
}

core_string_t parse_tree_leaf_text(parse_tree_t *n)
{
    core_string_t none = {0};

    if (!n || !core_tree_is_leaf(n)) {
        return none;
    }

    parse_token_info_t *tok = core_tree_leaf_value(n);

    if (!tok || !core_option_is_set(tok->value)) {
        return none;
    }

    return core_option_get(tok->value);
}

int64_t parse_tree_leaf_tid(parse_tree_t *n)
{
    if (!n || !core_tree_is_leaf(n)) {
        return 0;
    }

    parse_token_info_t *tok = core_tree_leaf_value(n);

    return tok ? tok->tid : 0;
}

bool parse_tree_leaf_is(parse_tree_t *n, core_string_t text)
{
    return core_string_eq(parse_tree_leaf_text(n), text);
}
