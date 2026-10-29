#ifndef N00BB_BUILTIN_ABI_H
#define N00BB_BUILTIN_ABI_H

#include "n00b-abi-v1.h"

const n00b_abi_type_v1 *n00b_builtin_abi_type(uint32_t index);
const n00b_abi_type_v1 *n00b_builtin_abi_type_for(uint32_t descriptor);
const n00b_abi_field_v1 *n00b_builtin_abi_field(uint32_t index);
const n00b_abi_arm_v1 *n00b_builtin_abi_arm(uint32_t index);
const n00b_abi_physical_run_v1 *n00b_builtin_abi_physical_run(uint32_t index);
const n00b_abi_trace_v1 *n00b_builtin_abi_trace(uint32_t index);
const n00b_abi_trace_child_v1 *n00b_builtin_abi_trace_child(uint32_t index);
const n00b_abi_trace_arm_v1 *n00b_builtin_abi_trace_arm(uint32_t index);
const n00b_abi_tag_extractor_v1 *n00b_builtin_abi_tag_extractor(uint32_t index);
const n00b_abi_layout_v1 *n00b_builtin_abi_layout(uint32_t index);
const n00b_abi_frame_layout_v1 *n00b_builtin_abi_frame_layout(uint32_t index);
uint32_t n00b_builtin_abi_storage_count(void);
const n00b_abi_storage_v1 *n00b_builtin_abi_storage(uint32_t index);
const n00b_abi_tls_relocation_v1 *n00b_builtin_abi_tls_relocation(
    uint32_t index);
const n00b_abi_symbol_v1 *n00b_builtin_abi_symbol(uint32_t index);
const n00b_abi_module_v1 *n00b_builtin_abi_module(uint32_t index);
const n00b_abi_function_v1 *n00b_builtin_abi_function(uint32_t index);
const uint8_t *n00b_builtin_abi_storage_address(uint32_t index);
uint32_t n00b_builtin_abi_baked_node_count(void);
const n00b_abi_baked_node_v1 *n00b_builtin_abi_baked_node(uint32_t index);
const uint8_t *n00b_builtin_abi_baked_node_address(uint32_t index);
const uint8_t *n00b_builtin_abi_string_backing_at(const void *address);
const uint8_t *n00b_builtin_abi_bytes(uint32_t first, uint32_t count);
int64_t n00b_builtin_abi_range_index(uint32_t first, uint32_t count,
                                      uint32_t ordinal);
const uint8_t *n00b_builtin_abi_table_base(uint32_t kind);
uint32_t n00b_builtin_abi_table_count(uint32_t kind);
uint32_t n00b_builtin_abi_table_record_bytes(uint32_t kind);
uint32_t n00b_builtin_abi_unary_type(uint32_t kind, uint32_t element);
int32_t n00b_builtin_abi_type_specializes(uint32_t type, uint32_t general);
uint32_t n00b_builtin_abi_type_argument_type(uint32_t index);
uint32_t n00b_builtin_abi_named_type(const uint8_t *name, uint64_t name_bytes,
                                     const uint32_t *arguments,
                                     uint32_t count);
uint64_t n00b_builtin_abi_ref_address(const void *value);
uint32_t n00b_builtin_dispatch_arm(uint64_t site, uint32_t type);
void n00b_builtin_dispatch_record(uint64_t site, uint32_t type, uint32_t arm);
const uint8_t *n00b_builtin_native_symbol_name(uint64_t address);

#endif
