#ifndef IR_TEXT_H
#define IR_TEXT_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "lib/string.h"

core_string_t ir_module_print(ir_module_t *m);
core_string_t ir_func_print(ir_func_t *f);
core_string_t ir_type_text(ir_module_t *m, th_type *t);
core_string_t ir_types_text(th_ctx *types, th_type *t);

core_string_t ir_origin_kind_name(ir_origin_kind_t k);

#endif
