#ifndef IR_TRAIT_H
#define IR_TRAIT_H

#include "ir/ir.h"

#define IR_TRAIT_VALUE_DATA_FIELD "data"
#define IR_TRAIT_VALUE_VTABLE_FIELD "vtable"
#define IR_TRAIT_VTABLE_TYPE_INFO_FIELD "type_info"

/* Lowered storage for a trait-typed value. The concrete value is indirect;
 * its vtable identifies the concrete runtime type and provides method slots.
 * Both pointers are opaque here because their referents are target/link-time
 * metadata. */
th_type *ir_trait_value_type(th_ctx *types);

/* IR constructors/extractors for the two-field trait-object value. Inputs to
 * the constructor must already use the opaque pointer fields above. */
ir_value_t *ir_trait_value_make(th_ctx *types, ir_block_t *block,
                                const char *name, ir_value_t *data,
                                ir_value_t *vtable);
ir_value_t *ir_trait_value_data(ir_block_t *block, const char *name,
                                ir_value_t *trait_value);
ir_value_t *ir_trait_value_vtable(ir_block_t *block, const char *name,
                                  ir_value_t *trait_value);

/* Build a declaration-ordered vtable record. Each supplied signature is the
 * client's lowered callable ABI and must take the erased data pointer first;
 * adapter functions can bridge concrete receiver types to this uniform slot
 * type. */
th_type *ir_trait_vtable_type(th_ctx *types, const char *const *method_names,
                              th_type *const *lowered_signatures,
                              size_t method_count);

/* Add a static vtable global. Adapter functions and method names must follow
 * trait declaration order; the first record member is the 32-bit type-info
 * index used by runtime type tests. */
ir_global_t *ir_trait_vtable_global_add(
    ir_module_t *module, const char *global_name, uint32_t type_info_index,
    const char *const *method_names, ir_func_t *const *adapters,
    size_t method_count);

/* Generate an internal thunk with `slot_signature` that adapts its erased
 * data-pointer receiver to `implementation`'s concrete receiver ABI. Fixed
 * positional and keyword arguments are forwarded unchanged; rest parameters
 * are forwarded with an IR spread operand. */
ir_func_t *ir_trait_receiver_adapter_add(ir_module_t *module,
                                        const char *adapter_name,
                                        ir_func_t *implementation,
                                        th_type *slot_signature);

typedef struct {
  const char *name;
  ir_value_t *value;
} ir_trait_named_arg_t;

/* Variant for trait methods with keyword parameters. */
ir_value_t *ir_trait_method_call_with_keywords(
    th_ctx *types, ir_block_t *block, const char *result_name,
    ir_value_t *trait_value, th_type *vtable_type, const char *method_name,
    ir_value_t *const *arguments, size_t argument_count,
    const ir_trait_named_arg_t *keywords, size_t keyword_count);

ir_value_t *ir_trait_method_call_with_rest(
    th_ctx *types, ir_block_t *block, const char *result_name,
    ir_value_t *trait_value, th_type *vtable_type, const char *method_name,
    ir_value_t *const *arguments, size_t argument_count,
    ir_value_t *rest_argument, const ir_trait_named_arg_t *keywords,
    size_t keyword_count);

/* Load and call one method slot from a trait value. The erased data pointer is
 * passed as argument zero, followed by `arguments`. The callable signature is
 * taken from the named field in `vtable_type`. */
ir_value_t *ir_trait_method_call(th_ctx *types, ir_block_t *block,
                                 const char *result_name,
                                 ir_value_t *trait_value,
                                 th_type *vtable_type,
                                 const char *method_name,
                                 ir_value_t *const *arguments,
                                 size_t argument_count);

#endif
