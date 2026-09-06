#pragma once

#include "ir/ir.h"
#include "ir/diag.h"

ir_module_t *ir_module_build(ir_ctx_t *ctx, const char *text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d);
