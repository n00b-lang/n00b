#pragma once

#include "ir/layout.h"
#include "lib/target.h"

bool n00bb_abi_publish_scalar_layout(ir_module_t *module, th_type *type,
                                     const core_target_desc_t *target,
                                     bool *handled);
bool n00bb_abi_publish_interned_scalar_layouts(
    ir_module_t *module, const core_target_desc_t *target,
    th_type **failed_type);
bool n00bb_abi_publish_integer_like_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    uint32_t semantic_bits);
