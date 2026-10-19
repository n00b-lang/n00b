#pragma once

#include "abi_record_layout.h"
#include "ir/layout.h"
#include "lib/target.h"

typedef struct {
    core_string_t name;
    th_type *type;
    uint32_t ordinal;
    bool c_compatible;
    bool packed;
    th_type *inline_element_type;
    uint64_t inline_element_count;
    uint32_t union_group;
} n00bb_abi_record_member_t;

typedef struct {
    core_string_t name;
    th_type *type;
    uint32_t ordinal;
} n00bb_abi_variant_arm_t;

static inline bool
n00bb_abi_layout_packable(const ir_final_layout_t *layout)
{
    return layout && !layout->n_members && !layout->has_discriminant
        && layout->n_occupied == 1 && layout->occupied
        && !layout->occupied[0].offset
        && layout->occupied[0].width < layout->intrinsic_bits;
}

bool n00bb_abi_publish_direct_layout(ir_module_t *module, th_type *type,
                                     const core_target_desc_t *target,
                                     bool *handled);
bool n00bb_abi_publish_interned_direct_layouts(
    ir_module_t *module, const core_target_desc_t *target,
    th_type **failed_type);
bool n00bb_abi_publish_integer_like_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    uint32_t semantic_bits);
bool n00bb_abi_publish_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    n00bb_record_policy_t policy, bool structural,
    const n00bb_abi_record_member_t *members, size_t count);
bool n00bb_abi_publish_c_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_record_member_t *members, size_t count);
bool n00bb_abi_publish_variant_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_variant_arm_t *arms, size_t count);
