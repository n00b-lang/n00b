#ifndef IR_TEXT_H
#define IR_TEXT_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "lib/string.h"

core_string_t ir_module_print(ir_module_t *m);
core_string_t ir_text_format(core_string_t text, int32_t width);
core_string_t ir_type_text(ir_module_t *m, th_type *t);
core_string_t ir_types_text(th_ctx *types, th_type *t);

ir_module_t *ir_module_parse(ir_ctx_t *ctx, core_string_t text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d);

bool          ir_origin_kind_from_name(core_string_t s, ir_origin_kind_t *out);
core_string_t ir_origin_kind_name(ir_origin_kind_t k);

#endif
