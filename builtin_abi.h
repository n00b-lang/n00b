#ifndef N00BB_BUILTIN_ABI_H
#define N00BB_BUILTIN_ABI_H

#include "n00b-abi-v1.h"

const n00b_abi_type_v1 *n00b_builtin_abi_type(uint32_t index);
const n00b_abi_type_v1 *n00b_builtin_abi_type_for(uint32_t descriptor);
const n00b_abi_field_v1 *n00b_builtin_abi_field(uint32_t index);
const n00b_abi_trace_v1 *n00b_builtin_abi_trace(uint32_t index);
const n00b_abi_trace_child_v1 *n00b_builtin_abi_trace_child(uint32_t index);
const n00b_abi_trace_arm_v1 *n00b_builtin_abi_trace_arm(uint32_t index);
const n00b_abi_tag_extractor_v1 *n00b_builtin_abi_tag_extractor(uint32_t index);
const n00b_abi_frame_layout_v1 *n00b_builtin_abi_frame_layout(uint32_t index);
uint32_t n00b_builtin_abi_storage_count(void);
const n00b_abi_storage_v1 *n00b_builtin_abi_storage(uint32_t index);
const uint8_t *n00b_builtin_abi_storage_address(uint32_t index);
const uint8_t *n00b_builtin_abi_string_backing_at(const void *address);
const uint8_t *n00b_builtin_abi_bytes(uint32_t first, uint32_t count);
int64_t n00b_builtin_abi_range_index(uint32_t first, uint32_t count,
                                      uint32_t ordinal);
uint64_t n00b_builtin_abi_ref_address(const void *value);

#endif
