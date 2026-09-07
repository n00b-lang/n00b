#pragma once

#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/tree_visit.h"
#include "scanner/scanner.h"

parse_tree_t *parse_tree_clone(parse_tree_t *tree);

parse_tree_t *parse_tree_make_nt(parse_grammar_t *g,
                                   core_string_t   nt_name,
                                   int32_t        rule_index);

parse_tree_t *parse_tree_make_nt_with(parse_grammar_t     *g,
                                        core_string_t       nt_name,
                                        int32_t            rule_index,
                                        parse_tree_t **kids,
                                        size_t             n);

parse_tree_t *parse_tree_make_token(int64_t      tid,
                                      core_string_t text,
                                      uint32_t     line,
                                      uint32_t     column);

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
