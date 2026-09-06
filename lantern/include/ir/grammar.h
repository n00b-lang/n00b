#pragma once

#include "ir/ir.h"
#include "parse/grammar.h"
#include "scanner/token_stream.h"

ncc_grammar_t *ir_grammar(void);

int64_t ir_grammar_nt(const char *name);
int64_t ir_grammar_tid(const char *name);
int32_t ir_grammar_unit(void);

void ir_grammar_shutdown(void);

ncc_token_stream_t *ir_lex(const char *src, size_t len);
