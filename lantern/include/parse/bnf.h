#pragma once

#include "parse/grammar.h"
#include "lib/string.h"

typedef struct {
    core_string_t message;
    size_t        start_byte;
    size_t        end_byte;
    uint32_t      start_line;
    uint32_t      start_col;
    uint32_t      end_line;
    uint32_t      end_col;
} parse_bnf_error_info_t;

extern bool parse_bnf_load(core_string_t bnf_text, core_string_t start_symbol,
                           parse_grammar_t *user_g);
extern bool parse_bnf_load_with_origin(core_string_t bnf_text,
                                       core_string_t origin,
                                       core_string_t start_symbol,
                                       parse_grammar_t *user_g);
extern bool parse_bnf_load_file(core_string_t path, core_string_t start_symbol,
                                parse_grammar_t *user_g);
extern core_string_t parse_bnf_error(void);
extern bool parse_bnf_error_info(parse_bnf_error_info_t *out);
extern size_t       parse_bnf_declare_terminals(parse_grammar_t *g,
                                              core_string_t bnf_text);
extern int32_t      parse_bnf_last_unit(void);
extern core_string_t parse_bnf_print(parse_grammar_t *g, int32_t unit);
extern core_string_t parse_bnf_strip_comments(core_string_t input);
extern core_string_t parse_bnf_trim_lines(core_string_t input);
