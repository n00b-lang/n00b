#pragma once

#include "parse/tree.h"

typedef struct parse_token_stream_t parse_token_stream_t;

typedef struct parse_forest_t {
    parse_tree_array_t  trees;
    parse_grammar_t          *grammar;
} parse_forest_t;

parse_forest_t parse_forest_empty(parse_grammar_t *g);

parse_forest_t parse_forest_new(parse_grammar_t          *g,
                                           parse_tree_array_t  trees);

int32_t parse_forest_count(parse_forest_t *f);

bool parse_forest_is_ambiguous(parse_forest_t *f);

parse_tree_t *parse_forest_tree(parse_forest_t *f, int32_t ix);

parse_tree_t *parse_forest_best(parse_forest_t *f);

void parse_forest_free(parse_forest_t *f);

typedef parse_forest_t (*parse_fn_t)(parse_grammar_t      *g,
                                     parse_nt_id_t         start,
                                     parse_token_stream_t *ts);
