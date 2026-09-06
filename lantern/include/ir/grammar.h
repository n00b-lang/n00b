#pragma once

#include "ir/ir.h"

typedef struct ncc_grammar_t    ncc_grammar_t;
typedef struct ncc_token_info_t ncc_token_info_t;

ncc_grammar_t *ir_grammar(void);

int64_t ir_grammar_nt(const char *name);
int64_t ir_grammar_tid(const char *name);

void ir_grammar_shutdown(void);

typedef struct ncc_token_stream_t ncc_token_stream_t;

ncc_token_stream_t *ir_lex(const char *src, size_t len);
