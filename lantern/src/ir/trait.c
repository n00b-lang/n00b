#include "ir/trait.h"
#include "private/ir/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static core_string_t bounded_text(char *buffer, size_t capacity, int written) {
  size_t length = written < 0 ? 0 : (size_t)written;
  if (length >= capacity) length = capacity - 1;
  return core_string_borrow(buffer, length);
}

th_type *ir_trait_value_type(th_ctx *types) {
  if (!types) return nullptr;
  th_type *opaque = th_name(types, CORE_STRING_STATIC(TH_ERASED));
  th_type *pointer = th_ind(types, 0, opaque);
  th_member fields[] = {
      {.name = CORE_STRING_STATIC(IR_TRAIT_VALUE_DATA_FIELD), .type = pointer},
      {.name = CORE_STRING_STATIC(IR_TRAIT_VALUE_VTABLE_FIELD),
       .type = pointer},
  };
  return th_record(types, fields, 2);
}

ir_value_t *ir_trait_value_make(th_ctx *types, ir_block_t *block,
                                core_string_t name, ir_value_t *data,
                                ir_value_t *vtable) {
  if (!types || !block || !name.data || !data || !vtable) return nullptr;
  th_type *object_type = ir_trait_value_type(types);
  th_type *data_type = th_record_lookup(
      object_type, CORE_STRING_STATIC(IR_TRAIT_VALUE_DATA_FIELD));
  th_type *vtable_type = th_record_lookup(
      object_type, CORE_STRING_STATIC(IR_TRAIT_VALUE_VTABLE_FIELD));
  if (!data_type || !vtable_type ||
      th_hash(types, data_type) != th_hash(types, data->type) ||
      th_hash(types, vtable_type) != th_hash(types, vtable->type))
    return nullptr;
  ir_instr_t *instruction = ir_instr_add(block, IR_OP_AGGREGATE);
  if (!instruction || !ir_instr_set_dest(instruction, name, object_type) ||
      !ir_instr_operand_value(instruction, data) ||
      !ir_instr_operand_value(instruction, vtable))
    return nullptr;
  return instruction->dest;
}

static ir_value_t *trait_value_extract(ir_block_t *block, core_string_t name,
                                       ir_value_t *trait_value,
                                       core_string_t field) {
  if (!block || !name.data || !trait_value || !trait_value->type
      || !field.data)
    return nullptr;
  th_type *field_type = th_record_lookup(trait_value->type, field);
  if (!field_type) return nullptr;
  ir_instr_t *instruction = ir_instr_add(block, IR_OP_EXTRACT);
  if (!instruction || !ir_instr_set_dest(instruction, name, field_type) ||
      !ir_instr_operand_value(instruction, trait_value) ||
      !ir_instr_operand_name(instruction, field))
    return nullptr;
  return instruction->dest;
}

ir_value_t *ir_trait_value_data(ir_block_t *block, core_string_t name,
                                ir_value_t *trait_value) {
  return trait_value_extract(block, name, trait_value,
                             CORE_STRING_STATIC(IR_TRAIT_VALUE_DATA_FIELD));
}

ir_value_t *ir_trait_value_vtable(ir_block_t *block, core_string_t name,
                                ir_value_t *trait_value) {
  return trait_value_extract(block, name, trait_value,
                             CORE_STRING_STATIC(IR_TRAIT_VALUE_VTABLE_FIELD));
}

th_type *ir_trait_vtable_type(th_ctx *types,
                              const core_string_t *method_names,
                              th_type *const *lowered_signatures,
                              size_t method_count) {
  if (!types || !method_names || !lowered_signatures || !method_count)
    return nullptr;
  th_type *data_type = th_record_lookup(
      ir_trait_value_type(types),
      CORE_STRING_STATIC(IR_TRAIT_VALUE_DATA_FIELD));
  if (!data_type) return nullptr;
  th_member *slots = calloc(method_count + 1, sizeof(th_member));
  if (!slots) return nullptr;
  slots[0] = (th_member){
      .name = CORE_STRING_STATIC(IR_TRAIT_VTABLE_TYPE_INFO_FIELD),
      .type = th_name(types, CORE_STRING_STATIC("u32")),
  };
  for (size_t i = 0; i < method_count; i++) {
    if (!method_names[i].data || !method_names[i].u8_bytes ||
        core_string_eq(method_names[i],
                       CORE_STRING_STATIC(IR_TRAIT_VTABLE_TYPE_INFO_FIELD)) ||
        !lowered_signatures[i] ||
        th_kind_of(lowered_signatures[i]) != TH_FUNCTION) {
      free(slots);
      return nullptr;
    }
    const th_param *receiver = th_func_param(lowered_signatures[i], 0);
    if (!receiver || th_hash(types, receiver->type) != th_hash(types, data_type)) {
      free(slots);
      return nullptr;
    }
    for (size_t j = 0; j < i; j++)
      if (core_string_eq(method_names[i], method_names[j])) {
        free(slots);
        return nullptr;
      }
    slots[i + 1] = (th_member){
        .name = method_names[i],
        .type = th_ind(types, 0, lowered_signatures[i]),
    };
  }
  th_type *record = th_record(types, slots, method_count + 1);
  free(slots);
  return record;
}

ir_global_t *ir_trait_vtable_global_add(
    ir_module_t *module, core_string_t global_name, uint32_t type_info_index,
    const core_string_t *method_names, ir_func_t *const *adapters,
    size_t method_count) {
  if (!module || !global_name.data || !global_name.u8_bytes || !method_names ||
      !adapters || !method_count)
    return nullptr;
  th_ctx *types = ir_ctx_types(module->ctx);
  th_type **signatures = calloc(method_count, sizeof(*signatures));
  if (!signatures) return nullptr;
  for (size_t i = 0; i < method_count; i++) {
    if (!adapters[i] || adapters[i]->owner != module) {
      free(signatures);
      return nullptr;
    }
    signatures[i] = adapters[i]->type;
  }
  th_type *vtable_type = ir_trait_vtable_type(
      types, method_names, (th_type *const *)signatures, method_count);
  free(signatures);
  if (!vtable_type) return nullptr;

  ir_global_t *global = ir_global_add(module, global_name, vtable_type);
  if (!global) return nullptr;
  global->is_static = true;
  global->mutability = IR_MUT_CONST;

  ir_global_init_value_t *values = calloc(method_count + 1, sizeof(*values));
  if (!values) return nullptr;
  values[0].member = CORE_STRING_STATIC(IR_TRAIT_VTABLE_TYPE_INFO_FIELD);
  values[0].kind = IR_GLOBAL_INIT_INT;
  values[0].integer = ir_lit_of(type_info_index);
  bool ok = true;
  for (size_t i = 0; i < method_count; i++) {
    values[i + 1].member = method_names[i];
    values[i + 1].kind = IR_GLOBAL_INIT_SYMBOL;
    values[i + 1].symbol = ir_func_address(module, adapters[i]);
    if (!values[i + 1].symbol) ok = false;
  }
  if (ok)
    ok = ir_global_set_record_initializer(module, global, values,
                                           method_count + 1);
  free(values);
  return ok ? global : nullptr;
}

static bool same_type(ir_module_t *module, const th_type *left,
                     const th_type *right) {
  return left && right && ir_type_eq(module, (th_type *)left, (th_type *)right);
}

static core_string_t adapter_param_name(ir_func_t *function,
                                        core_string_t stem) {
  if (!ir_value_find(function, stem))
    return ir_strdup(function->owner->ctx, stem);
  for (uint32_t suffix = 0;; suffix++) {
    char candidate[64];
    core_string_t text = bounded_text(
        candidate, sizeof(candidate),
        snprintf(candidate, sizeof(candidate), "%.*s_%u",
                 (int)stem.u8_bytes, stem.data, suffix));
    if (!ir_value_find(function, text))
      return ir_strdup(function->owner->ctx, text);
  }
}

static ir_value_t *adapter_call_output(ir_module_t *module,
                                       ir_block_t *block,
                                       ir_value_t *call_result,
                                       th_type *type, size_t index,
                                       core_string_t name) {
  if (!module || !block || !call_result || !type || !name.data ||
      index > INT64_MAX)
    return nullptr;
  ir_instr_t *output = ir_instr_add(block, IR_OP_CALL_OUTPUT);
  if (!output || !ir_instr_set_dest(output, name, type) ||
      !ir_instr_operand_value(output, call_result) ||
      !ir_instr_operand_uint(output, (ir_uint_t)index))
    return nullptr;
  return output->dest;
}

ir_func_t *ir_trait_receiver_adapter_add(ir_module_t *module,
                                        core_string_t adapter_name,
                                        ir_func_t *implementation,
                                        th_type *slot_signature) {
  if (!module || !adapter_name.data || !adapter_name.u8_bytes ||
      !implementation ||
      implementation->owner != module || !slot_signature ||
      th_kind_of(slot_signature) != TH_FUNCTION ||
      th_kind_of(implementation->type) != TH_FUNCTION ||
      th_func_nkw(slot_signature) != th_func_nkw(implementation->type))
    return nullptr;

  th_ctx *types = ir_ctx_types(module->ctx);
  th_type *erased_pointer = th_record_lookup(
      ir_trait_value_type(types),
      CORE_STRING_STATIC(IR_TRAIT_VALUE_DATA_FIELD));
  size_t count = th_func_nparams(slot_signature);
  if (!count || count != th_func_nparams(implementation->type) ||
      !same_type(module, th_func_ret(slot_signature),
                 th_func_ret(implementation->type)) ||
      !erased_pointer)
    return nullptr;

  const th_param *slot_receiver = th_func_param(slot_signature, 0);
  const th_param *concrete_receiver =
      th_func_param(implementation->type, 0);
  if (!slot_receiver || !concrete_receiver
      || !th_mode_is_in(slot_receiver->mode) ||
      !same_type(module, slot_receiver->type, erased_pointer))
    return nullptr;

  for (size_t i = 1; i < count; i++) {
    const th_param *slot = th_func_param(slot_signature, i);
    const th_param *concrete = th_func_param(implementation->type, i);
    if (!slot || !concrete || slot->mode != concrete->mode ||
        !same_type(module, slot->type, concrete->type))
      return nullptr;
  }
  const th_param *slot_rest = th_func_rest(slot_signature);
  const th_param *concrete_rest = th_func_rest(implementation->type);
  if (!!slot_rest != !!concrete_rest ||
      (slot_rest && (slot_rest->mode != concrete_rest->mode ||
                     !same_type(module, slot_rest->type,
                                concrete_rest->type))))
    return nullptr;
  for (size_t i = 0; i < th_func_nkw(slot_signature); i++) {
    const th_kwparam *slot = th_func_kw(slot_signature, i);
    if (!slot) return nullptr;
    const th_kwparam *concrete =
        th_func_kw_lookup(implementation->type, slot->name);
    if (!concrete || slot->mode != concrete->mode ||
        !same_type(module, slot->type, concrete->type))
      return nullptr;
  }
  ir_func_t *adapter = ir_func_add(module, adapter_name, slot_signature,
                                   IR_LINK_INTERNAL);
  if (!adapter) return nullptr;
  ir_value_t **arguments =
      ir_alloc(module->ctx, count * sizeof(ir_value_t *));
  arguments[0] = ir_param_add(
      adapter,
      adapter_param_name(adapter, CORE_STRING_STATIC("erased_data")),
      erased_pointer);
  for (size_t i = 1; arguments[0] && i < count; i++) {
    char name[32];
    core_string_t text = bounded_text(
        name, sizeof(name),
        snprintf(name, sizeof(name), "trait_arg_%zu", i - 1));
    const th_param *parameter = th_func_param(slot_signature, i);
    arguments[i] = parameter && parameter->mode != TH_OUT
        ? ir_param_add(adapter, adapter_param_name(adapter, text),
                       parameter->type) : nullptr;
  }
  if (!arguments[0]) return nullptr;
  for (size_t i = 1; i < count; i++) {
    const th_param *parameter = th_func_param(slot_signature, i);
    if (!parameter || (parameter->mode != TH_OUT && !arguments[i]))
      return nullptr;
  }
  ir_value_t *rest_value = slot_rest
      ? ir_restparam_add(adapter,
                         adapter_param_name(adapter,
                                            CORE_STRING_STATIC("trait_rest")),
                         slot_rest->type)
      : nullptr;
  if (slot_rest && !rest_value) return nullptr;
  for (size_t i = 0; i < th_func_nkw(slot_signature); i++) {
    const th_kwparam *keyword = th_func_kw(slot_signature, i);
    if (!keyword || (keyword->mode != TH_OUT
        && !ir_kwparam_add(adapter, keyword->name, keyword->type)))
      return nullptr;
  }

  ir_block_t *entry = ir_block_add(adapter, CORE_STRING_STATIC("entry"));
  if (!entry) return nullptr;
  th_type *concrete_pointer = th_ind(types, 0, concrete_receiver->type);
  ir_instr_t *cast = ir_instr_add(entry, IR_OP_BITCAST);
  if (!cast
      || !ir_instr_set_dest(cast, CORE_STRING_STATIC("typed_data"),
                            concrete_pointer) ||
      !ir_instr_operand_value(cast, arguments[0]))
    return nullptr;

  ir_value_t *receiver = cast->dest;
  if (concrete_receiver->mode != TH_OUT) {
    ir_instr_t *load = ir_instr_add(entry, IR_OP_LOAD);
    if (!load || !ir_instr_set_dest(load, CORE_STRING_STATIC("receiver"),
                                    concrete_receiver->type) ||
        !ir_instr_operand_value(load, cast->dest))
      return nullptr;
    receiver = load->dest;
  }

  ir_value_t *function = ir_alloc(module->ctx, sizeof(ir_value_t));
  function->kind = IR_VAL_FUNC;
  function->name = implementation->name;
  function->type = implementation->type;
  function->owner = implementation;
  ir_instr_t *call = ir_instr_add(entry, IR_OP_CALL);
  th_type *result_type = th_func_ret(implementation->type);
  bool returns_void = ir_machine_of(module, result_type).class == IR_CLASS_VOID;
  size_t output_count = 0;
  for (size_t i = 0; i < count; i++) {
    const th_param *parameter = th_func_param(implementation->type, i);
    if (parameter && th_mode_produces_output(parameter->mode)) output_count++;
  }
  for (size_t i = 0; i < th_func_nkw(implementation->type); i++) {
    const th_kwparam *parameter = th_func_kw(implementation->type, i);
    if (parameter && th_mode_produces_output(parameter->mode)) output_count++;
  }
  const th_param *implementation_rest = th_func_rest(implementation->type);
  if (implementation_rest && th_mode_produces_output(implementation_rest->mode))
    output_count++;
  bool carries_result = !returns_void || output_count;
  if (!call || (carries_result &&
                !ir_instr_set_dest(call, CORE_STRING_STATIC("call_result"),
                                   result_type)) ||
      !ir_instr_operand_value(call, function) ||
      (concrete_receiver->mode == TH_OUT
          ? !ir_instr_operand_out(call)
          : !ir_instr_operand_value(call, receiver)))
    return nullptr;
  for (size_t i = 1; i < count; i++) {
    const th_param *parameter = th_func_param(implementation->type, i);
    bool passed = parameter->mode == TH_OUT
        ? ir_instr_operand_out(call)
        : ir_instr_operand_value(call, arguments[i]);
    if (!passed) return nullptr;
  }
  if (rest_value && !ir_instr_operand_spread(call, rest_value)) return nullptr;
  for (size_t i = 0; i < th_func_nkw(slot_signature); i++) {
    const th_kwparam *keyword = th_func_kw(slot_signature, i);
    ir_value_t *value = ir_value_find(adapter, keyword->name);
    bool passed = keyword->mode == TH_OUT
        ? ir_instr_operand_named_out(call, keyword->name)
        : value && ir_instr_operand_named(call, keyword->name, value);
    if (!passed)
      return nullptr;
  }

  ir_value_t **return_outputs = output_count
      ? ir_alloc(module->ctx, output_count * sizeof(*return_outputs))
      : nullptr;
  if (output_count && !return_outputs) return nullptr;
  size_t return_output_count = 0;
  size_t output_index = 0;
  for (size_t i = 0; i < count; i++) {
    const th_param *parameter = th_func_param(implementation->type, i);
    if (!parameter || !th_mode_produces_output(parameter->mode)) continue;
    char output_name[48];
    core_string_t text = bounded_text(
        output_name, sizeof(output_name),
        snprintf(output_name, sizeof(output_name), "trait_output_%zu", i));
    ir_value_t *output = adapter_call_output(module, entry, call->dest,
        parameter->type, output_index++, text);
    if (!output) return nullptr;
    if (i == 0) {
      ir_instr_t *store = ir_instr_add(entry, IR_OP_STORE);
      if (!store || !ir_instr_operand_value(store, cast->dest) ||
          !ir_instr_operand_value(store, output))
        return nullptr;
    }
    else return_outputs[return_output_count++] = output;
  }
  for (size_t i = 0; i < th_func_nkw(implementation->type); i++) {
    const th_kwparam *parameter = th_func_kw(implementation->type, i);
    if (!parameter || !th_mode_produces_output(parameter->mode)) continue;
    char output_name[48];
    core_string_t text = bounded_text(
        output_name, sizeof(output_name),
        snprintf(output_name, sizeof(output_name),
                 "trait_keyword_output_%zu", i));
    ir_value_t *output = adapter_call_output(module, entry, call->dest,
        parameter->type, output_index++, text);
    if (!output) return nullptr;
    return_outputs[return_output_count++] = output;
  }
  if (implementation_rest
      && th_mode_produces_output(implementation_rest->mode)) {
    ir_value_t *output = adapter_call_output(module, entry, call->dest,
        implementation_rest->type, output_index,
        CORE_STRING_STATIC("trait_rest_output"));
    if (!output) return nullptr;
    return_outputs[return_output_count++] = output;
  }

  ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
  if (!ret || (!returns_void &&
               !ir_instr_operand_value(ret, call->dest)))
    return nullptr;
  for (size_t i = 0; i < return_output_count; i++)
    if (!ir_instr_operand_value(ret, return_outputs[i])) return nullptr;

  return adapter;
}

static ir_instr_t *trait_named_instruction(ir_block_t *block, ir_op_t op,
                                           uint32_t suffix,
                                           th_type *result_type) {
  char name[48];
  core_string_t text = bounded_text(
      name, sizeof(name),
      snprintf(name, sizeof(name), "__trait_dispatch_%u", suffix));
  ir_instr_t *instruction = ir_instr_add(block, op);
  return instruction && ir_instr_set_dest(instruction, text, result_type)
      ? instruction : nullptr;
}

ir_value_t *ir_trait_method_call_with_rest(
    th_ctx *types, ir_block_t *block, core_string_t result_name,
    ir_value_t *trait_value, th_type *vtable_type, core_string_t method_name,
    ir_value_t *const *arguments, size_t argument_count,
    ir_value_t *rest_argument, const ir_trait_named_arg_t *keywords,
    size_t keyword_count) {
  if (!types || !block || !block->owner || !block->owner->owner ||
      !result_name.data || !trait_value || !vtable_type ||
      !method_name.data || th_kind_of(vtable_type) != TH_RECORD ||
      (argument_count && !arguments) || (keyword_count && !keywords) ||
      !trait_value->type ||
      th_hash(types, trait_value->type) !=
          th_hash(types, ir_trait_value_type(types)))
    return nullptr;

  const th_type *slot_type = th_record_lookup(vtable_type, method_name);
  if (!slot_type || th_kind_of(slot_type) != TH_INDIRECTION)
    return nullptr;
  th_type *signature = th_ind_referent((th_type *)slot_type);
  if (!signature || th_kind_of(signature) != TH_FUNCTION ||
      !th_func_ret(signature))
    return nullptr;
  const th_param *receiver = th_func_param(signature, 0);
  if (!receiver) return nullptr;
  size_t fixed_arguments = th_func_nparams(signature) - 1;
  const th_param *rest = th_func_rest(signature);
  if (!same_type(block->owner->owner, receiver->type,
                 th_record_lookup(ir_trait_value_type(types),
                                  CORE_STRING_STATIC(
                                      IR_TRAIT_VALUE_DATA_FIELD))) ||
      argument_count < fixed_arguments ||
      (argument_count > fixed_arguments && !rest) ||
      (rest_argument && (!rest || !rest_argument->is_rest ||
                         !same_type(block->owner->owner, rest->type,
                                    rest_argument->type))))
    return nullptr;
  for (size_t i = 0; i < argument_count; i++)
    if (!arguments[i]) return nullptr;
  for (size_t i = 0; i < keyword_count; i++) {
    if (!keywords[i].name.data || !keywords[i].value ||
        !th_func_kw_lookup(signature, keywords[i].name))
      return nullptr;
    for (size_t j = 0; j < i; j++)
      if (core_string_eq(keywords[i].name, keywords[j].name)) return nullptr;
  }

  uint32_t suffix = block->n_instrs;
  char vtable_name[48];
  core_string_t vtable_text = bounded_text(
      vtable_name, sizeof(vtable_name),
      snprintf(vtable_name, sizeof(vtable_name), "__trait_vtable_%u",
               suffix));
  ir_value_t *raw_vtable =
      ir_trait_value_vtable(block, vtable_text, trait_value);
  th_type *typed_vtable = th_ind(types, 0, vtable_type);
  ir_instr_t *cast = raw_vtable
      ? trait_named_instruction(block, IR_OP_BITCAST, suffix + 1,
                                typed_vtable)
      : nullptr;
  if (!cast || !ir_instr_operand_value(cast, raw_vtable)) return nullptr;

  th_type *field_pointer = th_ind(types, 0, (th_type *)slot_type);
  ir_instr_t *field =
      trait_named_instruction(block, IR_OP_FIELD, suffix + 2, field_pointer);
  if (!field || !ir_instr_operand_value(field, cast->dest) ||
      !ir_instr_operand_name(field, method_name))
    return nullptr;

  ir_instr_t *load = trait_named_instruction(
      block, IR_OP_LOAD, suffix + 3, (th_type *)slot_type);
  if (!load || !ir_instr_operand_value(load, field->dest)) return nullptr;

  char data_name[48];
  core_string_t data_text = bounded_text(
      data_name, sizeof(data_name),
      snprintf(data_name, sizeof(data_name), "__trait_data_%u", suffix + 4));
  ir_value_t *data = ir_trait_value_data(block, data_text, trait_value);
  if (!data) return nullptr;

  ir_instr_t *call = ir_instr_add(block, IR_OP_CALL);
  if (!call || !ir_instr_set_dest(call, result_name, th_func_ret(signature)) ||
      !ir_instr_operand_value(call, load->dest) ||
      !ir_instr_operand_value(call, data))
    return nullptr;
  for (size_t i = 0; i < argument_count; i++)
    if (!ir_instr_operand_value(call, arguments[i])) return nullptr;
  if (rest_argument && !ir_instr_operand_spread(call, rest_argument))
    return nullptr;
  for (size_t i = 0; i < keyword_count; i++)
    if (!ir_instr_operand_named(call, keywords[i].name, keywords[i].value))
      return nullptr;
  return call->dest;
}

ir_value_t *ir_trait_method_call_with_keywords(
    th_ctx *types, ir_block_t *block, core_string_t result_name,
    ir_value_t *trait_value, th_type *vtable_type, core_string_t method_name,
    ir_value_t *const *arguments, size_t argument_count,
    const ir_trait_named_arg_t *keywords, size_t keyword_count) {
  return ir_trait_method_call_with_rest(
      types, block, result_name, trait_value, vtable_type, method_name,
      arguments, argument_count, nullptr, keywords, keyword_count);
}

ir_value_t *ir_trait_method_call(th_ctx *types, ir_block_t *block,
                                 core_string_t result_name,
                                 ir_value_t *trait_value,
                                 th_type *vtable_type,
                                 core_string_t method_name,
                                 ir_value_t *const *arguments,
                                 size_t argument_count) {
  return ir_trait_method_call_with_keywords(
      types, block, result_name, trait_value, vtable_type, method_name,
      arguments, argument_count, nullptr, 0);
}
