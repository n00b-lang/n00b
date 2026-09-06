#pragma once

#include "ir/ir.h"

th_type *ir_typelang_parse(th_ctx *ctx, const char *src, char **err);

th_type *ir_typelang_parse_prefix(th_ctx *ctx, const char *src, size_t len,
                                  size_t *consumed, char **err);

void ir_typelang_shutdown(void);
