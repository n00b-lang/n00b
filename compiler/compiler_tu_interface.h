#pragma once

#include "compiler_tu.h"

typedef struct {
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    ir_ctx_t *ir_context;
} n00bb_interface_context_t;

const char *interface_package_for_module(
    const n00bb_interface_context_t *context, core_string_t module);
const uint8_t *interface_owned_metadata(
    const n00bb_interface_context_t *context,
    th_env_metadata_owner_t owner_kind, const char *owner_name,
    const char *key, uint32_t version, size_t *size);
bool interface_restore_package_declarations(
    const n00bb_interface_context_t *context, core_translation_unit_t *unit);
bool finish_semantic_unit(void *context, core_translation_unit_t *unit);
ir_shape_t *function_parameter_shape(parse_sym_entry_t *symbol,
                                     size_t index);
bool interface_context_hook_symbol(parse_sym_entry_t *symbol);
