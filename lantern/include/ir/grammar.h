#pragma once

#include "ir/ir.h"
#include "lib/string.h"
#include "parse/grammar.h"
#include "scanner/token_stream.h"

parse_grammar_t *ir_grammar(void);

int64_t ir_grammar_nt(core_string_t name);
int64_t ir_grammar_tid(core_string_t name);
int32_t ir_grammar_unit(void);

void ir_grammar_shutdown(void);

parse_token_stream_t *ir_lex(core_string_t src);
