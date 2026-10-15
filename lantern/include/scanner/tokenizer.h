#pragma once

#include "scanner/scanner.h"
#include "parse/grammar.h"

#define SCAN_TOKENIZER_DEFAULT "text"


bool          scan_tokenizer_register(core_string_t name, parse_scan_cb_t fn);
parse_scan_cb_t scan_tokenizer_for(core_string_t name);

void         scan_grammar_set_tokenizer(parse_grammar_t *g, core_string_t name);
core_string_t scan_grammar_tokenizer(parse_grammar_t *g);

parse_scan_cb_t scan_grammar_tokenizer_fn(parse_grammar_t *g);

parse_token_stream_t *scan_tokenize(parse_grammar_t  *g,
                                 const char     *src,
                                 size_t          len,
                                 core_buffer_t  **buf_out,
                                 parse_scanner_t **sc_out);

parse_token_stream_t *scan_tokenize_incremental(parse_grammar_t  *g,
                                                core_buffer_t  **buf_out,
                                                parse_scanner_t **sc_out);

parse_token_stream_t *scan_tokenize_owned(parse_grammar_t *g,
                                           const char *src,
                                           size_t len);

parse_token_stream_t *scan_tokenize_incremental_owned(parse_grammar_t *g);
