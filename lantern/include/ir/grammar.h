#pragma once

#include "ir/ir.h"

typedef struct ncc_grammar_t    ncc_grammar_t;
typedef struct ncc_token_info_t ncc_token_info_t;

ncc_grammar_t *ir_grammar(void);

int64_t ir_grammar_nt(const char *name);

void ir_grammar_shutdown(void);

typedef struct {
    ncc_token_info_t **v;
    size_t            *at;
    size_t            *end;
    char             **note;
    int32_t            n;
    int32_t            cap;
} ir_tokens_t;

bool ir_tokenize(const char *src, size_t len, ir_tokens_t *out);
void ir_tokens_free(ir_tokens_t *t);
