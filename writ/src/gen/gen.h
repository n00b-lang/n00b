#pragma once

#include "../core/model/model.h"
#include "writ/gen.h"

typedef struct {
    const writ_model_t *model;
    writ_str_t          prefix;
} writ_c_context_t;

writ_str_t writ_c_type(const writ_c_context_t *c, const writ_type_view_t *type, bool constant);
writ_str_t writ_c_element(const writ_c_context_t *c, const writ_type_view_t *array);
bool       writ_c_returns_directly(const writ_plan_view_t *plan);
void       writ_c_emit_signature(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan);
void       writ_c_emit_real_prototype(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn,
                                      const char *terminator);
writ_status_t writ_gen_conformance(const writ_c_context_t *c, const writ_output_t *output);
