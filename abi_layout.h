#pragma once

#include "abi_record_layout.h"
#include "ir/layout.h"
#include "lib/target.h"

typedef struct {
    const char *name;
    th_type *type;
    uint32_t ordinal;
    bool c_compatible;
} n00bb_abi_record_member_t;

bool n00bb_abi_publish_direct_layout(ir_module_t *module, th_type *type,
                                     const core_target_desc_t *target,
                                     bool *handled);
bool n00bb_abi_publish_interned_direct_layouts(
    ir_module_t *module, const core_target_desc_t *target,
    th_type **failed_type);
bool n00bb_abi_publish_integer_like_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    uint32_t semantic_bits);
bool n00bb_abi_publish_c_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_record_member_t *members, size_t count);
