#pragma once

#include "parse/grammar.h"
#include "scanner/token_stream.h"

ncc_grammar_t      *ncc_annot_grammar(void);
bool                ncc_annot_building(void);
ncc_token_stream_t *ncc_annot_lex(ncc_string_t text, ncc_buffer_t **buf_out,
                                  ncc_scanner_t **sc_out);
void                ncc_annot_lex_free(ncc_token_stream_t *ts,
                                       ncc_buffer_t       *buf,
                                       ncc_scanner_t      *sc);
