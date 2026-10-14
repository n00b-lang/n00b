#pragma once

#include "../core/model/model.h"
#include "writ/gen.h"

enum {
    WRIT_C_MAX_ARITY = 8,
};

typedef struct {
    const writ_model_t *model;
    writ_str_t          prefix;
    writ_str_t          upper;
} writ_c_context_t;

typedef struct {
    writ_str_t type;
    writ_str_t name;
} writ_c_param_t;

writ_str_t writ_c_type(const writ_c_context_t *c, const writ_type_view_t *type, bool constant);
bool       writ_c_is_text(const writ_type_view_t *type);
bool       writ_c_returns_directly(const writ_plan_view_t *plan);
writ_step_kind_t writ_c_lift_kind(const writ_plan_view_t *plan, uint32_t item);
bool       writ_c_offset_result(const writ_plan_view_t *plan, uint32_t item);
size_t     writ_c_item_params(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t item,
                              writ_c_param_t params[2]);
writ_str_t writ_c_return_type(const writ_c_context_t *c, const writ_plan_view_t *plan);
bool       writ_c_is_input(const writ_plan_item_view_t *item);
size_t     writ_c_param_order(const writ_plan_view_t *plan, uint32_t *order);
void       writ_c_emit_signature(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan);
void       writ_c_emit_real_prototype(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn,
                                      writ_str_t name, bool variadic_tail, const char *terminator);
writ_str_t writ_c_callee(const writ_c_context_t *c, const writ_fn_view_t *fn);
writ_status_t writ_gen_conformance(const writ_c_context_t *c, const writ_output_t *output);
