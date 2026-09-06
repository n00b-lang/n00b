#include "parse/rewrite.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <string.h>

ncc_parse_tree_t *
ncc_tree_clone(ncc_parse_tree_t *tree)
{
    if (!tree) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(tree)) {
        ncc_token_info_t *src = ncc_tree_leaf_value(tree);
        ncc_token_info_t *tok = nullptr;

        if (src) {
            tok  = ncc_alloc(ncc_token_info_t);
            *tok = *src;

            tok->leading_trivia  = nullptr;
            tok->trailing_trivia = nullptr;
        }

        return ncc_tree_leaf(ncc_nt_node_t, ncc_token_info_ptr_t, tok);
    }

    ncc_nt_node_t     pn  = ncc_tree_node_value(tree);
    ncc_parse_tree_t *out = ncc_tree_node(ncc_nt_node_t,
                                          ncc_token_info_ptr_t,
                                          pn);
    size_t            nc  = ncc_tree_num_children(tree);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *kid = ncc_tree_clone(ncc_tree_child(tree, i));

        if (kid) {
            ncc_tree_add_child(out, kid);
        }
    }

    return out;
}

ncc_parse_tree_t *
ncc_tree_make_nt(ncc_grammar_t *g, ncc_string_t nt_name, int32_t rule_index)
{
    ncc_nonterm_t *nt = g ? ncc_find_nonterm(g, nt_name) : nullptr;

    if (!nt) {
        return nullptr;
    }

    ncc_nt_node_t pn = {0};

    pn.name       = nt->name;
    pn.id         = nt->id;
    pn.rule_index = rule_index;

    return ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
}

ncc_parse_tree_t *
ncc_tree_make_nt_with(ncc_grammar_t     *g,
                      ncc_string_t       nt_name,
                      int32_t            rule_index,
                      ncc_parse_tree_t **kids,
                      size_t             n)
{
    ncc_parse_tree_t *out = ncc_tree_make_nt(g, nt_name, rule_index);

    if (!out) {
        return nullptr;
    }

    for (size_t i = 0; i < n; i++) {
        if (kids[i]) {
            ncc_tree_add_child(out, kids[i]);
        }
    }

    return out;
}

ncc_parse_tree_t *
ncc_tree_make_token(int64_t      tid,
                    ncc_string_t text,
                    uint32_t     line,
                    uint32_t     column)
{
    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);

    memset(tok, 0, sizeof(*tok));

    tok->tid    = (int32_t)tid;
    tok->line   = line;
    tok->column = column;

    if (text.data) {
        tok->value = ncc_option_set(ncc_string_t, text);
    }

    return ncc_tree_leaf(ncc_nt_node_t, ncc_token_info_ptr_t, tok);
}

struct ncc_template_t {
    ncc_parse_tree_t   *tree;
    ncc_pwz_parser_t   *parser;
    ncc_token_stream_t *stream;
    ncc_scanner_t      *scanner;
    ncc_buffer_t       *buf;
};

ncc_template_t *
ncc_template_parse(ncc_grammar_t *g,
                   ncc_string_t   start_nt,
                   ncc_string_t   source,
                   ncc_scan_cb_t  tokenize,
                   char         **err)
{
    if (err) {
        *err = nullptr;
    }

    ncc_nonterm_t *start = g ? ncc_find_nonterm(g, start_nt) : nullptr;

    if (!start) {
        if (err) {
            *err = ncc_strdup("no such non-terminal to start from");
        }

        return nullptr;
    }

    if (!tokenize || !source.data) {
        if (err) {
            *err = ncc_strdup("a template needs a tokenizer and a source");
        }

        return nullptr;
    }

    ncc_template_t *t = ncc_alloc(ncc_template_t);

    memset(t, 0, sizeof(*t));

    t->buf     = ncc_buffer_from_bytes(source.data,
                                       (int64_t)source.u8_bytes);
    t->scanner = ncc_scanner_new(t->buf, tokenize, g,
                                 ncc_option_none(ncc_string_t), nullptr,
                                 nullptr);
    t->stream  = ncc_token_stream_new(t->scanner);

    int32_t saved = g->default_start;

    ncc_grammar_set_start(g, start);

    t->parser = ncc_pwz_new(g);

    bool ok = ncc_pwz_parse(t->parser, t->stream);

    ncc_grammar_set_start_id(g, saved);

    if (!ok) {
        if (err) {
            *err = ncc_strdup("the template does not parse");
        }

        ncc_template_free(t);

        return nullptr;
    }

    t->tree = ncc_pwz_get_tree(t->parser);

    return t;
}

ncc_parse_tree_t *
ncc_template_instance(ncc_template_t *t)
{
    return t ? ncc_tree_clone(t->tree) : nullptr;
}

void
ncc_template_free(ncc_template_t *t)
{
    if (!t) {
        return;
    }

    if (t->parser) {
        ncc_pwz_free(t->parser);
    }

    if (t->stream) {
        ncc_token_stream_free(t->stream);
    }

    if (t->scanner) {
        ncc_scanner_free(t->scanner);
    }

    if (t->buf) {
        ncc_buffer_free(t->buf);
    }

    ncc_free(t);
}

ncc_parse_tree_t *
ncc_tree_rewrite(ncc_tree_walk_t  *w,
                 ncc_parse_tree_t *root,
                 int32_t           max_passes,
                 int32_t          *passes_out)
{
    int32_t limit = max_passes > 0 ? max_passes : NCC_REWRITE_MAX_PASSES;
    int32_t n     = 0;

    if (passes_out) {
        *passes_out = 0;
    }

    if (!w || !root) {
        return root;
    }

    while (n < limit) {
        root = ncc_tree_walk_rewrite(w, root);
        n++;

        if (!ncc_tree_walk_replaced(w)) {
            break;
        }
    }

    if (passes_out) {
        *passes_out = n;
    }

    return root;
}
