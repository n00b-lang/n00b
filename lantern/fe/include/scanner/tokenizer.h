#pragma once

#include "scanner/scanner.h"
#include "parse/grammar.h"

#define NCC_TOKENIZER_DEFAULT "text"

void          ncc_tokenizers_register_builtin(void);

bool          ncc_tokenizer_register(ncc_string_t name, ncc_scan_cb_t fn);
ncc_scan_cb_t ncc_tokenizer_for(ncc_string_t name);
size_t        ncc_tokenizer_count(void);
ncc_string_t  ncc_tokenizer_name_at(size_t i);

void         ncc_grammar_set_tokenizer(ncc_grammar_t *g, ncc_string_t name);
ncc_string_t ncc_grammar_tokenizer(ncc_grammar_t *g);

ncc_scan_cb_t ncc_grammar_tokenizer_fn(ncc_grammar_t *g);

ncc_token_stream_t *ncc_tokenize(ncc_grammar_t  *g,
                                 const char     *src,
                                 size_t          len,
                                 ncc_buffer_t  **buf_out,
                                 ncc_scanner_t **sc_out);
