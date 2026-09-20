#pragma once

#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/tree_visit.h"
#include "scanner/scanner.h"

parse_tree_t *parse_tree_clone(const parse_tree_t *tree);

parse_tree_t *parse_tree_make_nt(parse_grammar_t *g,
                                   core_string_t   nt_name,
                                   int32_t        rule_index);

parse_tree_t *parse_tree_make_nt_with(parse_grammar_t     *g,
                                        core_string_t       nt_name,
                                        int32_t            rule_index,
                                        parse_tree_t **kids,
                                        size_t             n);

parse_tree_t *parse_tree_make_synthetic_token(int64_t tid, core_string_t text);

typedef struct {
    core_string_t node_name;
    int32_t       implicit_rule_index;
    int32_t       explicit_rule_index;
    size_t        insert_before_child;
} parse_type_materialization_rule_t;

typedef parse_tree_array_t (*parse_type_syntax_builder_fn)(
    parse_grammar_t                         *grammar,
    const parse_type_materialization_rule_t *rule,
    const parse_tree_t                      *node,
    th_type                                 *type,
    void                                    *state,
    char                                   **err);

parse_tree_t *parse_tree_materialize_types(
    parse_grammar_t                         *grammar,
    th_ctx                                  *types,
    const parse_tree_t                      *source,
    const parse_type_materialization_rule_t *rules,
    size_t                                   rule_count,
    parse_type_syntax_builder_fn             build_syntax,
    void                                    *state,
    char                                   **err);

typedef struct parse_template_t parse_template_t;

parse_template_t *parse_template_parse(parse_grammar_t *g,
                                   core_string_t   start_nt,
                                   core_string_t   source,
                                   parse_scan_cb_t  tokenize,
                                   char         **err);

parse_tree_t *parse_template_instance(parse_template_t *t);
void              parse_template_free(parse_template_t *t);

#define PARSE_REWRITE_MAX_PASSES 100

parse_tree_t *parse_tree_rewrite(parse_tree_walk_t  *w,
                                   parse_tree_t *root,
                                   int32_t           max_passes,
                                   int32_t          *passes_out);
