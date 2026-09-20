#include "parse/rewrite.h"
#include "parse/pwz.h"
#include "private/parse/grammar.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <string.h>

parse_tree_t *
parse_tree_clone(const parse_tree_t *tree)
{
    if (!tree) {
        return nullptr;
    }

    if (core_tree_is_leaf(tree)) {
        parse_token_info_t *src = core_tree_leaf_value(tree);
        parse_token_info_t *tok = parse_token_clone_for_tree(src);

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
parse_tree_make_synthetic_token(int64_t tid, core_string_t text)
{
    parse_token_info_t *tok = core_alloc(parse_token_info_t);

    memset(tok, 0, sizeof(*tok));

    tok->tid    = (int32_t)tid;
    tok->origin = PARSE_TOKEN_SYNTHETIC;
    tok->tree_owned = true;

    if (text.data) {
        tok->value = core_option_set(core_string_t, core_string_dup(text));
        tok->owns_value = true;
    }

    return core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t, tok);
}

typedef struct {
    parse_grammar_t                         *grammar;
    th_ctx                                  *types;
    const parse_type_materialization_rule_t *rules;
    size_t                                   rule_count;
    parse_type_syntax_builder_fn             build_syntax;
    void                                    *state;
    char                                   **err;
    bool                                     failed;
} type_materialization_t;

static void
materialization_error(type_materialization_t *m, const char *message)
{
    if (m->err && !*m->err) {
        *m->err = core_strdup(message);
    }

    m->failed = true;
}

static bool
materialization_rules_valid(type_materialization_t *m)
{
    for (size_t i = 0; i < m->rule_count; i++) {
        const parse_type_materialization_rule_t *rule = &m->rules[i];
        parse_nonterm_t *nt = parse_find_nonterm(m->grammar, rule->node_name);

        if (!nt || rule->implicit_rule_index < 0
            || rule->explicit_rule_index < 0
            || (size_t)rule->implicit_rule_index >= nt->rule_ids.len
            || (size_t)rule->explicit_rule_index >= nt->rule_ids.len
            || rule->implicit_rule_index == rule->explicit_rule_index) {
            materialization_error(m, "invalid type materialization rule");

            return false;
        }

        for (size_t j = 0; j < i; j++) {
            const parse_type_materialization_rule_t *prior = &m->rules[j];

            if (core_string_eq(rule->node_name, prior->node_name)
                && rule->implicit_rule_index == prior->implicit_rule_index) {
                materialization_error(m,
                                      "duplicate type materialization rule");

                return false;
            }
        }
    }

    return true;
}

static const parse_type_materialization_rule_t *
materialization_rule_for(type_materialization_t *m, parse_nt_node_t *pn)
{
    for (size_t i = 0; i < m->rule_count; i++) {
        const parse_type_materialization_rule_t *rule = &m->rules[i];

        if (pn->rule_index == rule->implicit_rule_index
            && core_string_eq(pn->name, rule->node_name)) {
            return rule;
        }
    }

    return nullptr;
}

static void
mark_synthetic(parse_tree_t *tree)
{
    if (core_tree_is_leaf(tree)) {
        parse_token_info_t *token = core_tree_leaf_value(tree);

        if (token) {
            token->origin          = PARSE_TOKEN_SYNTHETIC;
            token->line            = 0;
            token->column          = 0;
            token->endline         = 0;
            token->endcol          = 0;
            token->start_byte      = 0;
            token->end_byte        = 0;
            token->system_header   = false;
        }

        return;
    }

    size_t child_count = core_tree_num_children(tree);

    for (size_t i = 0; i < child_count; i++) {
        mark_synthetic(core_tree_child(tree, i));
    }
}

static void
free_syntax(parse_tree_array_t *syntax)
{
    for (size_t i = 0; i < syntax->len; i++) {
        parse_tree_free(syntax->data[i]);
    }

    core_array_free(*syntax);
}

static bool
materialize_node(type_materialization_t *m, parse_tree_t *node)
{
    if (m->failed || !node) {
        return false;
    }

    if (core_tree_is_leaf(node)) {
        return true;
    }

    size_t child_count = core_tree_num_children(node);

    for (size_t i = 0; i < child_count; i++) {
        if (!materialize_node(m, core_tree_child(node, i))) {
            return false;
        }
    }

    parse_nt_node_t *pn = &core_tree_node_value(node);
    const parse_type_materialization_rule_t *rule
        = materialization_rule_for(m, pn);

    if (!rule) {
        return true;
    }

    if (rule->insert_before_child > child_count) {
        materialization_error(m,
                              "type insertion is outside the node's children");

        return false;
    }

    th_type *type = pn->type ? th_resolve(pn->type) : nullptr;

    if (!type || !th_is_closed(m->types, type)) {
        materialization_error(m,
                              "type materialization requires a resolved type");

        return false;
    }

    parse_tree_array_t syntax = m->build_syntax(m->grammar,
                                                rule,
                                                node,
                                                type,
                                                m->state,
                                                m->err);

    if (m->err && *m->err) {
        free_syntax(&syntax);
        m->failed = true;

        return false;
    }

    if (!syntax.data || !syntax.len) {
        core_array_free(syntax);

        if (!m->err || !*m->err) {
            materialization_error(m,
                                  "type syntax builder returned no syntax");
        }
        else {
            m->failed = true;
        }

        return false;
    }

    for (size_t i = 0; i < syntax.len; i++) {
        if (!syntax.data[i]) {
            free_syntax(&syntax);
            materialization_error(m,
                                  "type syntax builder returned a null node");

            return false;
        }
    }

    for (size_t i = 0; i < syntax.len; i++) {
        mark_synthetic(syntax.data[i]);
        core_tree_insert_child_at(node,
                                  rule->insert_before_child + i,
                                  syntax.data[i]);
    }

    pn->rule_index = rule->explicit_rule_index;
    core_array_free(syntax);

    return true;
}

parse_tree_t *
parse_tree_materialize_types(
    parse_grammar_t                         *grammar,
    th_ctx                                  *types,
    const parse_tree_t                      *source,
    const parse_type_materialization_rule_t *rules,
    size_t                                   rule_count,
    parse_type_syntax_builder_fn             build_syntax,
    void                                    *state,
    char                                   **err)
{
    if (err) {
        *err = nullptr;
    }

    type_materialization_t m = {
        .grammar      = grammar,
        .types        = types,
        .rules        = rules,
        .rule_count   = rule_count,
        .build_syntax = build_syntax,
        .state        = state,
        .err          = err,
    };

    if (!grammar || !types || !source || !rules || !rule_count
        || !build_syntax) {
        materialization_error(&m,
                              "type materialization requires all arguments");

        return nullptr;
    }

    if (!materialization_rules_valid(&m)) {
        return nullptr;
    }

    parse_tree_t *result = parse_tree_clone(source);

    if (!materialize_node(&m, result)) {
        parse_tree_free(result);

        return nullptr;
    }

    return result;
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

    t->parser = parse_pwz_new_for_start(g, parse_nonterm_id(start));

    bool ok = parse_pwz_parse(t->parser, t->stream);

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
