#pragma once

#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/tree_visit.h"
#include "scanner/scanner.h"

ncc_parse_tree_t *ncc_tree_clone(ncc_parse_tree_t *tree);

ncc_parse_tree_t *ncc_tree_make_nt(ncc_grammar_t *g,
                                   ncc_string_t   nt_name,
                                   int32_t        rule_index);

ncc_parse_tree_t *ncc_tree_make_nt_with(ncc_grammar_t     *g,
                                        ncc_string_t       nt_name,
                                        int32_t            rule_index,
                                        ncc_parse_tree_t **kids,
                                        size_t             n);

ncc_parse_tree_t *ncc_tree_make_token(int64_t      tid,
                                      ncc_string_t text,
                                      uint32_t     line,
                                      uint32_t     column);

typedef struct ncc_template_t ncc_template_t;

ncc_template_t *ncc_template_parse(ncc_grammar_t *g,
                                   ncc_string_t   start_nt,
                                   ncc_string_t   source,
                                   ncc_scan_cb_t  tokenize,
                                   char         **err);

ncc_parse_tree_t *ncc_template_instance(ncc_template_t *t);
void              ncc_template_free(ncc_template_t *t);

#define NCC_REWRITE_MAX_PASSES 100

ncc_parse_tree_t *ncc_tree_rewrite(ncc_tree_walk_t  *w,
                                   ncc_parse_tree_t *root,
                                   int32_t           max_passes,
                                   int32_t          *passes_out);
