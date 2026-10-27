#ifndef IR_TRAIT_H
#define IR_TRAIT_H

#include "ir/ir.h"
#include "lib/string.h"

#define IR_TRAIT_VALUE_DATA_FIELD "data"
#define IR_TRAIT_VALUE_VTABLE_FIELD "vtable"
#define IR_TRAIT_VTABLE_TYPE_INFO_FIELD "type_info"

th_type *ir_trait_value_type(th_ctx *types);

ir_value_t *ir_trait_value_make(th_ctx *types, ir_block_t *block,
                                core_string_t name, ir_value_t *data,
                                ir_value_t *vtable);
ir_value_t *ir_trait_value_data(ir_block_t *block, core_string_t name,
                                ir_value_t *trait_value);
ir_value_t *ir_trait_value_vtable(ir_block_t *block, core_string_t name,
                                  ir_value_t *trait_value);

th_type *ir_trait_vtable_type(th_ctx *types,
                              const core_string_t *method_names,
                              th_type *const *lowered_signatures,
                              size_t method_count);

ir_global_t *ir_trait_vtable_global_add(
    ir_module_t *module, core_string_t global_name, uint32_t type_info_index,
    const core_string_t *method_names, ir_func_t *const *adapters,
    size_t method_count);

ir_func_t *ir_trait_receiver_adapter_add(ir_module_t *module,
                                        core_string_t adapter_name,
                                        ir_func_t *implementation,
                                        th_type *slot_signature);

typedef struct {
  core_string_t name;
  ir_value_t *value;
} ir_trait_named_arg_t;

ir_value_t *ir_trait_method_call_with_keywords(
    th_ctx *types, ir_block_t *block, core_string_t result_name,
    ir_value_t *trait_value, th_type *vtable_type, core_string_t method_name,
    ir_value_t *const *arguments, size_t argument_count,
    const ir_trait_named_arg_t *keywords, size_t keyword_count);

ir_value_t *ir_trait_method_call_with_rest(
    th_ctx *types, ir_block_t *block, core_string_t result_name,
    ir_value_t *trait_value, th_type *vtable_type, core_string_t method_name,
    ir_value_t *const *arguments, size_t argument_count,
    ir_value_t *rest_argument, const ir_trait_named_arg_t *keywords,
    size_t keyword_count);

ir_value_t *ir_trait_method_call(th_ctx *types, ir_block_t *block,
                                 core_string_t result_name,
                                 ir_value_t *trait_value,
                                 th_type *vtable_type,
                                 core_string_t method_name,
                                 ir_value_t *const *arguments,
                                 size_t argument_count);

#endif
