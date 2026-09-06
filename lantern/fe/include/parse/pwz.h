#pragma once

#include "parse/grammar.h"
#include "parse/parse_forest.h"
#include "parse/token.h"

typedef struct ncc_token_stream_t ncc_token_stream_t;

typedef bool (*ncc_pwz_disambig_fn)(void             *ud,
                                    const char       *predicate,
                                    ncc_token_info_t *tok);

typedef struct ncc_pwz_parser_t ncc_pwz_parser_t;

ncc_pwz_parser_t *ncc_pwz_new(ncc_grammar_t *g);

void ncc_pwz_free(ncc_pwz_parser_t *p);

void ncc_pwz_reset(ncc_pwz_parser_t *p);

void ncc_pwz_release_parse_state(ncc_pwz_parser_t *p);

bool ncc_pwz_parse(ncc_pwz_parser_t   *p,
                     ncc_token_stream_t *ts);

bool ncc_pwz_parse_prefix(ncc_pwz_parser_t   *p,
                          ncc_token_stream_t *ts,
                          int32_t            *ntokens);

ncc_parse_tree_t *ncc_pwz_get_tree(ncc_pwz_parser_t *p);

int32_t ncc_pwz_error_pos(ncc_pwz_parser_t *p);

void ncc_pwz_set_disambiguator(ncc_pwz_parser_t    *p,
                               ncc_pwz_disambig_fn  cb,
                               void                *ud);

ncc_parse_tree_t *ncc_pwz_reextract(ncc_pwz_parser_t *p);

ncc_parse_tree_array_t ncc_pwz_get_trees(ncc_pwz_parser_t *p);

ncc_parse_forest_t ncc_pwz_get_forest(ncc_pwz_parser_t *p);

ncc_parse_forest_t ncc_pwz_parse_grammar(ncc_grammar_t      *g,
                                             ncc_token_stream_t *ts);

#ifdef NCC_MEM_DEBUG
void ncc_pwz_report_stats(void);
#endif
