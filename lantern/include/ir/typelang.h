#pragma once

#include "ir/ir.h"

// Parse typelang surface syntax into a typehash type term, driven by
// th/typelang.bnf rather than by a hand-written parser. On failure returns
// nullptr and, when @p err is given, sets it to a message the caller frees.
th_type *ir_typelang_parse(th_ctx *ctx, const char *src, char **err);

// Reads the longest type at the front of @p src and reports how many bytes it
// used, for a type embedded in a larger text.
th_type *ir_typelang_parse_prefix(th_ctx *ctx, const char *src, size_t len,
                                  size_t *consumed, char **err);

// Release the cached grammar. Only needed by leak-checked runs.
void ir_typelang_shutdown(void);
