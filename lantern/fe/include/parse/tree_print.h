#pragma once

#include "parse/grammar.h"
#include "parse/parse_tree.h"

typedef struct {
    ncc_string_t source;
    int32_t      width;
    int32_t      indent;
} ncc_print_opts_t;

char *ncc_tree_print(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                     const ncc_print_opts_t *opts);
