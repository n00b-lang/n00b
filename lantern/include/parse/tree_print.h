#pragma once

#include "parse/grammar.h"
#include "parse/tree.h"

typedef struct {
    core_string_t source;
    int32_t      width;
    int32_t      indent;
} parse_print_opts_t;

char *parse_tree_print(parse_grammar_t *g, parse_tree_t *tree,
                     const parse_print_opts_t *opts);
