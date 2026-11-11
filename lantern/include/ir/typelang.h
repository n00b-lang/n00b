#pragma once

#include "ir/ir.h"
#include "lib/string.h"
#include "parse/tree_visit.h"
#include "parse/symtab.h"

th_type *ir_typelang_parse(th_ctx *ctx, core_string_t src,
                           core_string_t *err);

th_type *ir_typelang_parse_prefix(th_ctx *ctx, core_string_t src,
                                  size_t *consumed, core_string_t *err);

void ir_typelang_shutdown(void);

typedef struct ir_typelang_lower_t ir_typelang_lower_t;

ir_typelang_lower_t *ir_typelang_lower_new(th_ctx *ctx);

bool ir_typelang_install(parse_tree_walk_t *w, parse_grammar_t *g,
                         ir_typelang_lower_t *l);
th_type *ir_typelang_type_of(void *reduced);
void ir_typelang_lower_free(ir_typelang_lower_t *l);

th_type *ir_type_from_text(th_ctx *types, core_string_t spec);

th_ctx           *ir_module_types(ir_module_t *m);
ir_ctx_t         *ir_module_ctx(ir_module_t *m);
