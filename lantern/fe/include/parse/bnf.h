#pragma once

#include "parse/grammar.h"

extern bool ncc_bnf_load(ncc_string_t bnf_text, ncc_string_t start_symbol,
                           ncc_grammar_t *user_g);
extern int32_t      ncc_bnf_last_unit(void);
extern char        *ncc_bnf_print(ncc_grammar_t *g, int32_t unit);
extern ncc_string_t ncc_bnf_strip_comments(ncc_string_t input);
extern ncc_string_t ncc_bnf_trim_lines(ncc_string_t input);
