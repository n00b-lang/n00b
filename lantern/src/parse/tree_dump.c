#include "parse/tree_dump.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

static void
print_indent(core_buffer_t *b, int depth)
{
    for (int i = 0; i < depth; i++) {
        core_buffer_puts(b, "  ");
    }
}

static void
print_node(parse_grammar_t *g, parse_tree_t *node, core_buffer_t *b,
           int depth, bool raw)
{
    if (!node) {
        return;
    }

    if (core_tree_is_leaf(node)) {
        parse_token_info_t *tok = core_tree_leaf_value(node);

        if (!tok) {
            return;
        }

        print_indent(b, depth);

        core_option_t(core_string_t) value = parse_token_value(tok);

        if (core_option_is_set(value)) {
            core_string_t val = core_option_get(value);

            if (val.data) {
                core_buffer_printf(b, "TOKEN[%d] \"%.*s\"\n",
                                  parse_token_tid(tok), (int)val.u8_bytes,
                                  val.data);

                return;
            }
        }

        core_buffer_printf(b, "TOKEN[%d]\n", parse_token_tid(tok));

        return;
    }

    parse_nt_node_t *pn = &core_tree_node_value(node);

    if (pn->group_top && !raw) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            print_node(g, core_tree_child(node, i), b, depth, raw);
        }

        return;
    }

    print_indent(b, depth);

    if (pn->group_top) {
        core_buffer_puts(b, "[GROUP_TOP]");
    }
    else if (pn->group_item) {
        core_buffer_puts(b, "[GROUP_ITEM]");
    }

    if (pn->name.data) {
        core_buffer_printf(b, "<%.*s>", (int)pn->name.u8_bytes, pn->name.data);
    }
    else {
        parse_nonterm_t *nt = parse_get_nonterm(g, pn->id);

        if (nt && nt->name.data) {
            core_buffer_printf(b, "<%.*s>",
                              (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            core_buffer_printf(b, "<NT#%lld>", (long long)pn->id);
        }
    }

    core_buffer_printf(b, " rule=%d", pn->rule_index);

    if (pn->penalty > 0) {
        core_buffer_printf(b, " penalty=%u", pn->penalty);
    }

    core_buffer_putc(b, '\n');

    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        print_node(g, core_tree_child(node, i), b, depth + 1, raw);
    }
}

char *
parse_tree_dump_str(parse_grammar_t *g, parse_tree_t *tree, bool raw)
{
    core_buffer_t *b = core_buffer_empty();

    if (tree) {
        print_node(g, tree, b, 0, raw);
    }

    return core_buffer_take(b);
}

void
parse_tree_dump(parse_grammar_t *g, parse_tree_t *tree,
              FILE *out, bool raw)
{
    if (!tree || !out) {
        return;
    }

    char *s = parse_tree_dump_str(g, tree, raw);

    fputs(s, out);
    core_free(s);
}
