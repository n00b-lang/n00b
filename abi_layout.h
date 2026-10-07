#pragma once

#include "ir/layout.h"
#include "lib/target.h"

bool n00bb_abi_publish_scalar_layout(ir_module_t *module, th_type *type,
                                     const core_target_desc_t *target,
                                     bool *handled);
