#pragma once

#include "compiler_tu.h"

typedef struct n00bb_cache_t n00bb_cache_t;

typedef struct {
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    ir_ctx_t *ir_context;
    const n00bb_cache_t *cache;
} n00bb_interface_context_t;

core_string_t interface_package_for_module(
    const n00bb_interface_context_t *context, core_string_t module);
const uint8_t *interface_owned_metadata(
    const n00bb_interface_context_t *context,
    th_env_metadata_owner_t owner_kind, core_string_t owner_name,
    core_string_t key, uint32_t version, size_t *size);
bool interface_restore_package_declarations(
    const n00bb_interface_context_t *context, core_translation_unit_t *unit);
bool interface_may_declare_package(const n00bb_interface_context_t *context,
                                   core_string_t module, core_string_t name);
bool finish_semantic_unit(void *context, core_translation_unit_t *unit);
ir_shape_t *function_parameter_shape(parse_sym_entry_t *symbol,
                                     size_t index);
bool interface_context_hook_symbol(parse_sym_entry_t *symbol);
