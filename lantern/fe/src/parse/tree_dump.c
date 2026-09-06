#include "parse/tree_dump.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

static void
print_indent(ncc_buffer_t *b, int depth)
{
    for (int i = 0; i < depth; i++) {
        ncc_buffer_puts(b, "  ");
    }
}

static void
print_node(ncc_grammar_t *g, ncc_parse_tree_t *node, ncc_buffer_t *b,
           int depth, bool raw)
{
    if (!node) {
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);

        if (!tok) {
            return;
        }

        print_indent(b, depth);

        if (ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);

            if (val.data) {
                ncc_buffer_printf(b, "TOKEN[%d] \"%.*s\"\n",
                                  tok->tid, (int)val.u8_bytes, val.data);

                return;
            }
        }

        ncc_buffer_printf(b, "TOKEN[%d]\n", tok->tid);

        return;
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);

    if (pn->group_top && !raw) {
        for (size_t i = 0; i < ncc_tree_num_children(node); i++) {
            print_node(g, ncc_tree_child(node, i), b, depth, raw);
        }

        return;
    }

    print_indent(b, depth);

    if (pn->group_top) {
        ncc_buffer_puts(b, "[GROUP_TOP]");
    }
    else if (pn->group_item) {
        ncc_buffer_puts(b, "[GROUP_ITEM]");
    }

    if (pn->name.data) {
        ncc_buffer_printf(b, "<%.*s>", (int)pn->name.u8_bytes, pn->name.data);
    }
    else {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, pn->id);

        if (nt && nt->name.data) {
            ncc_buffer_printf(b, "<%.*s>",
                              (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            ncc_buffer_printf(b, "<NT#%lld>", (long long)pn->id);
        }
    }

    ncc_buffer_printf(b, " rule=%d", pn->rule_index);

    if (pn->penalty > 0) {
        ncc_buffer_printf(b, " penalty=%u", pn->penalty);
    }

    ncc_buffer_putc(b, '\n');

    for (size_t i = 0; i < ncc_tree_num_children(node); i++) {
        print_node(g, ncc_tree_child(node, i), b, depth + 1, raw);
    }
}

char *
ncc_tree_dump_str(ncc_grammar_t *g, ncc_parse_tree_t *tree, bool raw)
{
    ncc_buffer_t *b = ncc_buffer_empty();

    if (tree) {
        print_node(g, tree, b, 0, raw);
    }

    return ncc_buffer_take(b);
}

void
ncc_tree_dump(ncc_grammar_t *g, ncc_parse_tree_t *tree,
              FILE *out, bool raw)
{
    if (!tree || !out) {
        return;
    }

    char *s = ncc_tree_dump_str(g, tree, raw);

    fputs(s, out);
    ncc_free(s);
}
