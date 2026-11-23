#pragma once

#include "parse/grammar.h"
#include "lib/string.h"

extern bool parse_bnf_load(core_string_t bnf_text, core_string_t start_symbol,
                           parse_grammar_t *user_g);
extern bool parse_bnf_load_with_origin(core_string_t bnf_text,
                                       core_string_t origin,
                                       core_string_t start_symbol,
                                       parse_grammar_t *user_g);
extern bool parse_bnf_load_file(core_string_t path, core_string_t start_symbol,
                                parse_grammar_t *user_g);
extern core_string_t parse_bnf_error(void);
extern size_t       parse_bnf_declare_terminals(parse_grammar_t *g,
                                              core_string_t bnf_text);
extern core_string_t parse_bnf_strip_comments(core_string_t input);
extern core_string_t parse_bnf_trim_lines(core_string_t input);
