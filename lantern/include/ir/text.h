#ifndef IR_TEXT_H
#define IR_TEXT_H

#include "ir/diag.h"
#include "ir/ir.h"

char *ir_module_print(ir_module_t *m);
char *ir_text_format(const char *text, int32_t width);
const char *ir_type_text(ir_module_t *m, th_type *t);

ir_module_t *ir_module_parse(ir_ctx_t *ctx, const char *text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d);

bool        ir_origin_kind_from_name(const char *s, ir_origin_kind_t *out);
const char *ir_origin_kind_name(ir_origin_kind_t k);

#endif
