#pragma once

#include "ir/ir.h"

th_type *ir_typelang_parse(th_ctx *ctx, const char *src, char **err);

th_type *ir_typelang_parse_prefix(th_ctx *ctx, const char *src, size_t len,
                                  size_t *consumed, char **err);

void ir_typelang_shutdown(void);

typedef struct ncc_tree_walk_t     ncc_tree_walk_t;
typedef struct ir_typelang_lower_t ir_typelang_lower_t;

ir_typelang_lower_t *ir_typelang_lower_new(th_ctx *ctx);
void ir_typelang_install(ncc_tree_walk_t *w, ir_typelang_lower_t *l);
th_type *ir_typelang_type_of(void *reduced);
void ir_typelang_lower_free(ir_typelang_lower_t *l);
