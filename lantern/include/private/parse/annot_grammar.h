#pragma once

#include "parse/annotation.h"
#include "parse/grammar.h"
#include "scanner/token_stream.h"

bool          parse_annot_parse_args(parse_annotation_t *a, core_string_t text);
core_string_t parse_annot_arg_grammar(parse_annot_kind_t kind);

parse_grammar_t      *parse_annot_grammar(void);
bool                parse_annot_building(void);
parse_token_stream_t *parse_annot_lex(core_string_t text, core_buffer_t **buf_out,
                                  parse_scanner_t **sc_out);
void                parse_annot_lex_free(parse_token_stream_t *ts,
                                       core_buffer_t       *buf,
                                       parse_scanner_t      *sc);
