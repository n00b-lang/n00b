#pragma once

#include "parse/parse_tree.h"

typedef struct ncc_token_stream_t ncc_token_stream_t;

typedef struct ncc_parse_forest_t {
    ncc_parse_tree_array_t  trees;
    ncc_grammar_t          *grammar;
} ncc_parse_forest_t;

ncc_parse_forest_t ncc_parse_forest_empty(ncc_grammar_t *g);

ncc_parse_forest_t ncc_parse_forest_new(ncc_grammar_t          *g,
                                           ncc_parse_tree_array_t  trees);

int32_t ncc_parse_forest_count(ncc_parse_forest_t *f);

bool ncc_parse_forest_is_ambiguous(ncc_parse_forest_t *f);

ncc_parse_tree_t *ncc_parse_forest_tree(ncc_parse_forest_t *f, int32_t ix);

ncc_parse_tree_t *ncc_parse_forest_best(ncc_parse_forest_t *f);

void *ncc_parse_forest_walk(ncc_parse_forest_t *f,
                              int32_t              ix,
                              void                *thunk);

void *ncc_parse_forest_walk_best(ncc_parse_forest_t *f,
                                   void               *thunk);

void ncc_parse_forest_free(ncc_parse_forest_t *f);

typedef ncc_parse_forest_t (*ncc_parse_fn_t)(ncc_grammar_t      *g,
                                                ncc_token_stream_t *ts);
