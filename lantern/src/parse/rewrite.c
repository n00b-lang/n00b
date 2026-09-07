#include "parse/rewrite.h"
#include "parse/pwz.h"
#include "private/parse/grammar.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <string.h>

parse_tree_t *
parse_tree_clone(parse_tree_t *tree)
{
    if (!tree) {
        return nullptr;
    }

    if (core_tree_is_leaf(tree)) {
        parse_token_info_t *src = core_tree_leaf_value(tree);
        parse_token_info_t *tok = nullptr;

        if (src) {
            tok  = core_alloc(parse_token_info_t);
            *tok = *src;

            tok->leading_trivia  = nullptr;
            tok->trailing_trivia = nullptr;
        }

        return core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t, tok);
    }

    parse_nt_node_t     pn  = core_tree_node_value(tree);
    parse_tree_t *out = core_tree_node(parse_nt_node_t,
                                          parse_token_info_ptr_t,
                                          pn);
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
parse_tree_make_nt_with(parse_grammar_t     *g,
                      core_string_t       nt_name,
                      int32_t            rule_index,
                      parse_tree_t **kids,
                      size_t             n)
{
    parse_tree_t *out = parse_tree_make_nt(g, nt_name, rule_index);

    if (!out) {
        return nullptr;
    }

    for (size_t i = 0; i < n; i++) {
        if (kids[i]) {
            core_tree_add_child(out, kids[i]);
        }
    }

    return out;
}

parse_tree_t *
parse_tree_make_token(int64_t      tid,
                    core_string_t text,
                    uint32_t     line,
                    uint32_t     column)
{
    parse_token_info_t *tok = core_alloc(parse_token_info_t);

    memset(tok, 0, sizeof(*tok));

    tok->tid    = (int32_t)tid;
    tok->line   = line;
    tok->column = column;

    if (text.data) {
        tok->value = core_option_set(core_string_t, text);
    }

    return core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t, tok);
}

struct parse_template_t {
    parse_tree_t   *tree;
    parse_pwz_parser_t   *parser;
    parse_token_stream_t *stream;
    parse_scanner_t      *scanner;
    core_buffer_t       *buf;
};

parse_template_t *
parse_template_parse(parse_grammar_t *g,
                   core_string_t   start_nt,
                   core_string_t   source,
                   parse_scan_cb_t  tokenize,
                   char         **err)
{
    if (err) {
        *err = nullptr;
    }

    parse_nonterm_t *start = g ? parse_find_nonterm(g, start_nt) : nullptr;

    if (!start) {
        if (err) {
            *err = core_strdup("no such non-terminal to start from");
        }

        return nullptr;
    }

    if (!tokenize || !source.data) {
        if (err) {
            *err = core_strdup("a template needs a tokenizer and a source");
        }

        return nullptr;
    }

    parse_template_t *t = core_alloc(parse_template_t);

    memset(t, 0, sizeof(*t));

    t->buf     = core_buffer_from_bytes(source.data,
                                       (int64_t)source.u8_bytes);
    t->scanner = scan_scanner_new(t->buf, tokenize, g,
                                 core_option_none(core_string_t), nullptr,
                                 nullptr);
    t->stream  = scan_token_stream_new(t->scanner);

    int32_t saved = g->default_start;

    parse_grammar_set_start(g, start);

    t->parser = parse_pwz_new(g);

    bool ok = parse_pwz_parse(t->parser, t->stream);

    parse_grammar_set_start_id(g, saved);

    if (!ok) {
        if (err) {
            *err = core_strdup("the template does not parse");
        }

        parse_template_free(t);

        return nullptr;
    }

    t->tree = parse_pwz_get_tree(t->parser);

    return t;
}

parse_tree_t *
parse_template_instance(parse_template_t *t)
{
    return t ? parse_tree_clone(t->tree) : nullptr;
}

void
parse_template_free(parse_template_t *t)
{
    if (!t) {
        return;
    }

    if (t->parser) {
        parse_pwz_free(t->parser);
    }

    if (t->stream) {
        scan_token_stream_free(t->stream);
    }

    if (t->scanner) {
        scan_scanner_free(t->scanner);
    }

    if (t->buf) {
        core_buffer_free(t->buf);
    }

    core_free(t);
}

parse_tree_t *
parse_tree_rewrite(parse_tree_walk_t  *w,
                 parse_tree_t *root,
                 int32_t           max_passes,
                 int32_t          *passes_out)
{
    int32_t limit = max_passes > 0 ? max_passes : PARSE_REWRITE_MAX_PASSES;
    int32_t n     = 0;

    if (passes_out) {
        *passes_out = 0;
    }

    if (!w || !root) {
        return root;
    }

    while (n < limit) {
        root = parse_tree_walk_rewrite(w, root);
        n++;

        if (!parse_tree_walk_replaced(w)) {
            break;
        }
    }

    if (passes_out) {
        *passes_out = n;
    }

    return root;
}
