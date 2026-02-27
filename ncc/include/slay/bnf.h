#pragma once

#include "slay/grammar.h"

extern bool n00b_bnf_load(n00b_string_t bnf_text, n00b_string_t start_symbol,
                           n00b_grammar_t *user_g);
extern n00b_string_t n00b_bnf_strip_comments(n00b_string_t input);
extern n00b_string_t n00b_bnf_trim_lines(n00b_string_t input);
