#pragma once

#include "parse/grammar.h"
#include "parse/forest.h"
#include "parse/token.h"

typedef struct parse_token_stream_t parse_token_stream_t;

typedef bool (*parse_pwz_disambig_fn)(void             *ud,
                                    core_string_t      predicate,
                                    parse_token_info_t *tok);

typedef struct parse_pwz_parser_t parse_pwz_parser_t;

parse_pwz_parser_t *parse_pwz_new_for_start(parse_grammar_t *g,
                                            parse_nt_id_t start);

void parse_pwz_free(parse_pwz_parser_t *p);

void parse_pwz_reset(parse_pwz_parser_t *p);

void parse_pwz_release_parse_state(parse_pwz_parser_t *p);

bool parse_pwz_parse(parse_pwz_parser_t   *p,
                     parse_token_stream_t *ts);

bool parse_pwz_parse_prefix(parse_pwz_parser_t   *p,
                          parse_token_stream_t *ts,
                          int32_t            *ntokens);

parse_tree_t *parse_pwz_get_tree(parse_pwz_parser_t *p);

int32_t parse_pwz_error_pos(parse_pwz_parser_t *p);

void parse_pwz_set_disambiguator(parse_pwz_parser_t    *p,
                               parse_pwz_disambig_fn  cb,
                               void                *ud);

parse_tree_array_t parse_pwz_get_trees(parse_pwz_parser_t *p);

parse_forest_t parse_pwz_get_forest(parse_pwz_parser_t *p);

parse_forest_t parse_pwz_parse_grammar(parse_grammar_t      *g,
                                       parse_nt_id_t         start,
                                       parse_token_stream_t *ts);
parse_forest_t parse_pwz_parse_grammar_at(parse_grammar_t *g,
                                          parse_nt_id_t start,
                                          parse_token_stream_t *ts,
                                          int32_t *error_pos);
