#pragma once

#include "parse/grammar.h"

extern bool parse_bnf_load(core_string_t bnf_text, core_string_t start_symbol,
                           parse_grammar_t *user_g);
extern size_t       parse_bnf_declare_terminals(parse_grammar_t *g,
                                              core_string_t bnf_text);
extern int32_t      parse_bnf_last_unit(void);
extern char        *parse_bnf_print(parse_grammar_t *g, int32_t unit);
extern core_string_t parse_bnf_strip_comments(core_string_t input);
extern core_string_t parse_bnf_trim_lines(core_string_t input);
