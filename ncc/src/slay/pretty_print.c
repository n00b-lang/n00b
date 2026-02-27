// pretty_print.c — Parse tree pretty printer.

#include "slay/pretty_print.h"
#include "internal/slay/grammar_internal.h"

static void
print_indent(FILE *out, int depth)
{
    for (int i = 0; i < depth; i++) {
        fprintf(out, "  ");
    }
}

static void
print_node(n00b_grammar_t *g, n00b_parse_tree_t *node, FILE *out,
           int depth, bool raw)
{
    if (!node) {
        return;
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (!tok) {
            return;
        }

        print_indent(out, depth);

        if (n00b_option_is_set(tok->value)) {
            n00b_string_t val = n00b_option_get(tok->value);

            if (val.data) {
                fprintf(out, "TOKEN[%d] \"%.*s\"\n",
                        tok->tid, (int)val.u8_bytes, val.data);
            }
            else {
                fprintf(out, "TOKEN[%d]\n", tok->tid);
            }
        }
        else {
            fprintf(out, "TOKEN[%d]\n", tok->tid);
        }

        return;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Skip group wrappers — just recurse into children at same depth.
    // In raw mode, show them explicitly instead.
    if (pn->group_top && !raw) {
        for (size_t i = 0; i < n00b_tree_num_children(node); i++) {
            print_node(g, n00b_tree_child(node, i), out, depth, raw);
        }
        return;
    }

    print_indent(out, depth);

    if (pn->group_top) {
        fprintf(out, "[GROUP_TOP]");
    }
    else if (pn->group_item) {
        fprintf(out, "[GROUP_ITEM]");
    }

    if (pn->name.data) {
        fprintf(out, "<%.*s>", (int)pn->name.u8_bytes, pn->name.data);
    }
    else {
        n00b_nonterm_t *nt = n00b_get_nonterm(g, pn->id);

        if (nt && nt->name.data) {
            fprintf(out, "<%.*s>", (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            fprintf(out, "<NT#%lld>", (long long)pn->id);
        }
    }

    fprintf(out, " rule=%d", pn->rule_index);

    if (pn->penalty > 0) {
        fprintf(out, " penalty=%u", pn->penalty);
    }

    fprintf(out, "\n");

    for (size_t i = 0; i < n00b_tree_num_children(node); i++) {
        print_node(g, n00b_tree_child(node, i), out, depth + 1, raw);
    }
}

void
n00b_parse_tree_print(n00b_grammar_t *g, n00b_parse_tree_t *tree,
                       FILE *out, bool raw)
{
    if (!tree || !out) {
        return;
    }

    print_node(g, tree, out, 0, raw);
}
