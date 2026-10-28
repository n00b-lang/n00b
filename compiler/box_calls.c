#include "box_calls.h"
#include "abi_type_index.h"
#include "n00b-abi-v1.h"
#include "cached_body.h"
#include "compiler_cache.h"
#include "compiler_lower.h"
#include "sequence_transfer.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/typelang.h"
#include "lib/buffer.h"

typedef struct {
    n00bb_link_analysis_t *analysis;
    ir_module_t *runtime;
    ir_module_t *store;
    ir_module_t *runtime_body;
    ir_module_t *store_body;
    ir_module_t *floating;
    core_dict_t emitted;
    core_dict_t type_indices;
    ir_func_t *indexed_function;
} box_calls_t;

typedef enum {
    ADDRESS_UNKNOWN,
    ADDRESS_RAW,
    ADDRESS_BOXED,
} address_storage_t;

static address_storage_t address_storage(ir_func_t *caller,
                                          ir_value_t *address, unsigned depth);
static address_storage_t address_storage_within(ir_func_t *caller,
    ir_value_t *address, unsigned depth, ir_instr_t **visiting);
static bool boxed_address(ir_func_t *caller, ir_value_t *address);
static bool heap_allocation(ir_func_t *caller, ir_instr_t *allocation);
static bool open_reference(ir_func_t *caller, ir_value_t *address);
static bool two_variable_call(box_calls_t *calls, ir_func_t *caller,
    ir_instr_t *instruction, ir_func_t *target, th_type *owner_type,
    th_type *other_type, ir_value_t *const *arguments,
    size_t argument_count);

static bool
runtime_module(box_calls_t *calls, const ir_module_t *module)
{
    return module == calls->runtime || module == calls->store
        || (calls->runtime_body && module == calls->runtime_body)
        || (calls->store_body && module == calls->store_body);
}

static bool
retarget(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction,
         ir_func_t *target, th_type *const *actuals, size_t actual_count,
         ir_value_t *const *arguments, size_t argument_count)
{
    th_ctx *types = ir_module_types(target->owner);
    th_type **variables = nullptr;
    size_t count = th_free_vars(types, target->type, &variables);
    if (count != actual_count) return false;
    th_type *signature = th_type_clone_replacing(types, target->type,
        (const th_type *const *)variables, actuals, count);
    ir_value_t *callee = ir_func_address(target->owner, target);
    if (!signature || !callee) return false;
    instruction->op = IR_OP_CALL;
    instruction->n_operands = 0;
    instruction->n_mods = 0;
    if (!ir_instr_operand_value(instruction, callee)) return false;
    for (size_t i = 0; i < argument_count; i++)
        if (!ir_instr_operand_value(instruction, arguments[i])) return false;
    if (!ir_instr_set_call_signature(instruction, signature)) return false;
    core_list_push(calls->analysis->specialization_requests,
        ((n00bb_specialization_request_t){caller->owner, target}));
    if (core_dict_add(&calls->emitted, target, 1))
        core_list_push(calls->analysis->emitted_functions, target);
    return true;
}

static ir_func_t *
runtime_function(box_calls_t *calls, core_string_t name)
{
    return calls->runtime ? ir_func_find(calls->runtime, name) : nullptr;
}

static th_type *
open_option_payload(ir_module_t *module, th_type *type)
{
    th_type *option = th_builtin_applied(th_resolve(type), TH_BUILTIN_OPTION,
                                         1);
    const th_arg *argument = option ? th_named_arg(option, 0) : nullptr;
    th_type *payload = argument && argument->kind == TH_ARG_TYPE
        ? argument->type : nullptr;
    return payload && !th_is_closed(ir_module_types(module), option)
        && n00bb_boxed_signature_position(module, payload) ? payload : nullptr;
}

static bool
rewrite_load(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    th_ctx *types = ir_module_types(module);
    ir_value_t *address = instruction->n_operands == 1
        && instruction->operands[0].kind == IR_OPD_VALUE
        ? instruction->operands[0].value : nullptr;
    th_type *reference = address ? th_resolve(address->type) : nullptr;
    if (instruction->n_mods || !reference
        || th_kind_of(reference) != TH_INDIRECTION
        || th_is_closed(types, reference)
        || !boxed_address(caller, address)) return true;
    th_type *referent = th_ind_referent(reference);
    th_type *payload = open_option_payload(module, referent);
    if (payload) {
        ir_func_t *target = runtime_function(calls,
                                             CORE_STRING_STATIC("option_get"));
        return !target || retarget(calls, caller, instruction, target,
                                   &payload, 1, &address, 1);
    }
    if (!n00bb_boxed_signature_position(module, referent)) return true;
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("ref_get"));
    return !target || retarget(calls, caller, instruction, target, &referent,
                               1, &address, 1);
}

static uint32_t
member_ordinal(ir_module_t *module, th_type *record, core_string_t name)
{
    th_type *resolved = th_resolve(record);
    th_type *definition = ir_type_definition(module, resolved);
    if (!definition && th_kind_of(resolved) == TH_RECORD)
        definition = resolved;
    if (!definition || th_kind_of(definition) != TH_RECORD)
        return UINT32_MAX;
    for (size_t i = 0; i < th_record_len(definition) && i < UINT32_MAX; i++) {
        const th_member *member = th_record_member(definition, i);
        if (member && member->name.data && core_string_eq(member->name, name))
            return (uint32_t)i;
    }
    return UINT32_MAX;
}

static ir_instr_t *
fresh_instruction(ir_block_t *block, uint32_t at, ir_span_t span, ir_op_t op,
                  th_type *type)
{
    ir_instr_t *instruction = type ? ir_instr_generated(block, op, span,
        CORE_STRING_STATIC("box_calls"), at) : nullptr;
    return instruction
        && ir_instr_set_fresh_dest(instruction, "__box_value", type)
        ? instruction : nullptr;
}

static ir_value_t *
typed_constant(ir_func_t *caller, ir_block_t *block, uint32_t at,
               ir_span_t span, uint64_t value, core_string_t type_name)
{
    ir_instr_t *constant = fresh_instruction(block, at, span,
        IR_OP_CONST, th_name(ir_module_types(caller->owner), type_name));
    return constant && ir_instr_operand_uint(constant, value)
        ? constant->dest : nullptr;
}

static ir_value_t *
ordinal_constant(ir_func_t *caller, ir_instr_t *before, uint32_t ordinal)
{
    return typed_constant(caller, before->block, before->index, before->span,
                          ordinal, CORE_STRING_STATIC("u32"));
}

static bool
rewrite_extract(box_calls_t *calls, ir_func_t *caller,
                ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_NAME
        || !instruction->operands[1].name.data
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *record = instruction->operands[0].value;
    th_type *record_type = record ? th_resolve(record->type) : nullptr;
    if (!record_type || th_is_closed(ir_module_types(module), record_type)
        || !n00bb_boxed_signature_position(module, record_type)) return true;
    uint32_t ordinal = member_ordinal(module, record_type,
                                      instruction->operands[1].name);
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("field_get"));
    if (ordinal == UINT32_MAX || !target) return true;
    ir_value_t *constant = ordinal_constant(caller, instruction, ordinal);
    ir_value_t *arguments[] = {record, constant};
    return constant && two_variable_call(calls, caller, instruction, target,
        record_type, instruction->dest->type, arguments, 2);
}

static bool
rewrite_sequence_length(box_calls_t *calls, ir_func_t *caller,
                        ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *items = instruction->operands[0].value;
    th_type *sequence = th_resolve(items->type);
    th_type *count = th_name(ir_module_types(module),
                             CORE_STRING_STATIC("uint"));
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("array_length"));
    if (th_kind_of(sequence) != TH_SEQUENCE
        || th_is_closed(ir_module_types(module), sequence) || !count
        || !target || !ir_type_eq(module, instruction->dest->type, count))
        return true;
    th_type *element = th_seq_elem(sequence);
    return retarget(calls, caller, instruction, target, &element, 1,
                    &items, 1);
}

static bool
rewrite_extract_index(box_calls_t *calls, ir_func_t *caller,
                      ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_CONST
        || instruction->operands[1].lit.is_float
        || instruction->operands[1].lit.i.neg
        || instruction->operands[1].lit.i.mag >= UINT32_MAX
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *tuple = instruction->operands[0].value;
    th_type *tuple_type = tuple ? th_resolve(tuple->type) : nullptr;
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("field_get"));
    if (!tuple_type || th_is_closed(ir_module_types(module), tuple_type)
        || !n00bb_boxed_signature_position(module, tuple_type) || !target)
        return true;
    ir_value_t *constant = ordinal_constant(caller, instruction,
        (uint32_t)instruction->operands[1].lit.i.mag);
    ir_value_t *arguments[] = {tuple, constant};
    return constant && two_variable_call(calls, caller, instruction, target,
        tuple_type, instruction->dest->type, arguments, 2);
}

static bool
two_variable_call(box_calls_t *calls, ir_func_t *caller,
                  ir_instr_t *instruction, ir_func_t *target,
                  th_type *owner_type, th_type *other_type,
                  ir_value_t *const *arguments, size_t argument_count)
{
    th_ctx *types = ir_module_types(target->owner);
    th_type **variables = nullptr;
    if (th_free_vars(types, target->type, &variables) != 2) return true;
    const th_param *formal = th_func_param(target->type, 0);
    bool first_is_owner = formal && th_occurs(types, variables[0],
                                               formal->type);
    th_type *actuals[2] = {
        first_is_owner ? owner_type : other_type,
        first_is_owner ? other_type : owner_type,
    };
    return retarget(calls, caller, instruction, target, actuals, 2,
                    arguments, argument_count);
}

static ir_value_t *
slot_reference(box_calls_t *calls, ir_func_t *caller, ir_instr_t *before,
               ir_value_t *address)
{
    th_ctx *types = ir_module_types(caller->owner);
    th_type *reference = address ? th_resolve(address->type) : nullptr;
    th_type *any = th_name(types, CORE_STRING_STATIC("Any"));
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("slot_reference"));
    if (!reference || th_kind_of(reference) != TH_INDIRECTION
        || th_is_closed(types, reference) || !any || !target
        || address_storage(caller, address, 0) != ADDRESS_RAW)
        return nullptr;
    th_type *referent = th_ind_referent(reference);
    if (!n00bb_boxed_signature_position(caller->owner, referent))
        return nullptr;
    ir_instr_t *slot = fresh_instruction(before->block, before->index,
        before->span, IR_OP_BITCAST, th_ind(types, 0, any));
    if (!slot || !ir_instr_operand_value(slot, address)) return nullptr;
    ir_instr_t *call = fresh_instruction(before->block, before->index,
        before->span, IR_OP_CALL, address->type);
    return call && retarget(calls, caller, call, target, &referent, 1,
                            &slot->dest, 1) ? call->dest : nullptr;
}

static bool
rewrite_field(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_NAME
        || !instruction->operands[1].name.data
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *address = instruction->operands[0].value;
    th_type *base = address ? th_resolve(address->type) : nullptr;
    if (!base || th_kind_of(base) != TH_INDIRECTION) return true;
    th_type *record_type = th_resolve(th_ind_referent(base));
    th_type *reference = th_resolve(instruction->dest->type);
    if (!n00bb_boxed_signature_position(module, record_type)
        || th_kind_of(reference) != TH_INDIRECTION) return true;
    uint32_t ordinal = member_ordinal(module, record_type,
                                      instruction->operands[1].name);
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("field_ref"));
    if (ordinal == UINT32_MAX || !target) return true;
    if (!open_reference(caller, address)) {
        if (address_storage(caller, address, 0) != ADDRESS_RAW) return true;
        address = slot_reference(calls, caller, instruction, address);
        if (!address) return false;
    }
    ir_value_t *constant = ordinal_constant(caller, instruction, ordinal);
    ir_value_t *arguments[] = {address, constant};
    return constant && two_variable_call(calls, caller, instruction, target,
        record_type, th_ind_referent(reference), arguments, 2);
}

static ir_func_t *
store_function(box_calls_t *calls, core_string_t name)
{
    return calls->store ? ir_func_find(calls->store, name) : nullptr;
}

static th_type *
boxed_variant(ir_module_t *module, const ir_value_t *value)
{
    th_type *type = value ? th_resolve(value->type) : nullptr;
    bool variant = false;
    return type && th_named_info(type, nullptr, &variant, nullptr) && variant
        && !th_is_closed(ir_module_types(module), type)
        && n00bb_boxed_signature_position(module, type) ? type : nullptr;
}

static bool
rewrite_variant_tag(box_calls_t *calls, ir_func_t *caller,
                    ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *variant = instruction->operands[0].value;
    th_type *type = boxed_variant(module, variant);
    th_type *tag = ir_profile_scalar(module, IR_CLASS_UINT, 64);
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("variant_tag"));
    if (!type || !tag || !target
        || !ir_type_eq(module, instruction->dest->type, tag)) return true;
    return retarget(calls, caller, instruction, target, &type, 1, &variant, 1);
}

static bool
rewrite_variant_payload(box_calls_t *calls, ir_func_t *caller,
                        ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_NAME
        || !instruction->operands[1].name.data
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || ir_machine_of(module, instruction->dest->type).class
            == IR_CLASS_VOID) return true;
    ir_value_t *variant = instruction->operands[0].value;
    th_type *type = boxed_variant(module, variant);
    uint32_t ordinal = type ? member_ordinal(module, type,
        instruction->operands[1].name) : UINT32_MAX;
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("field_get"));
    if (ordinal == UINT32_MAX || !target) return true;
    ir_value_t *constant = ordinal_constant(caller, instruction, ordinal);
    ir_value_t *arguments[] = {variant, constant};
    return constant && two_variable_call(calls, caller, instruction, target,
        type, instruction->dest->type, arguments, 2);
}

static bool
rewrite_variant_make(box_calls_t *calls, ir_func_t *caller,
                     ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_NAME
        || instruction->operands[1].kind != IR_OPD_VALUE
        || !instruction->operands[1].value
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    th_type *type = boxed_variant(module, instruction->dest);
    uint32_t ordinal = type ? member_ordinal(module, type,
        instruction->operands[0].name) : UINT32_MAX;
    ir_func_t *create = store_function(calls,
                                       CORE_STRING_STATIC("variant_new"));
    ir_func_t *put = store_function(calls, CORE_STRING_STATIC("field_put"));
    if (ordinal == UINT32_MAX || !create || !put) return true;
    ir_value_t *payload = instruction->operands[1].value;
    bool carries = ir_machine_of(module, payload->type).class
        != IR_CLASS_VOID && !th_is_nil(payload->type);
    ir_value_t *physical = typed_constant(caller, instruction->block,
        instruction->index, instruction->span, (uint64_t)ordinal + 1,
        CORE_STRING_STATIC("u64"));
    if (!physical || !retarget(calls, caller, instruction, create, &type, 1,
                               &physical, 1)) return false;
    if (!carries) return true;
    ir_block_t *block = instruction->block;
    uint32_t at = instruction->index + 1;
    ir_value_t *constant = typed_constant(caller, block, at,
        instruction->span, ordinal, CORE_STRING_STATIC("u32"));
    ir_instr_t *store = constant ? ir_instr_generated(block, IR_OP_CALL,
        instruction->span, CORE_STRING_STATIC("box_calls"), at + 1)
        : nullptr;
    ir_value_t *arguments[] = {instruction->dest, constant, payload};
    return store && two_variable_call(calls, caller, store, put, type,
                                      payload->type, arguments, 3);
}

static ir_value_t *
owned_sequence(box_calls_t *calls, ir_func_t *caller, ir_instr_t *before,
               ir_value_t *value, bool transfers)
{
    th_type *type = value ? th_resolve(value->type) : nullptr;
    if (!type || transfers || th_kind_of(type) != TH_SEQUENCE
        || th_is_closed(ir_module_types(caller->owner), type)) return value;
    ir_func_t *copy = store_function(calls, CORE_STRING_STATIC("array_copy"));
    if (!copy) return value;
    ir_instr_t *call = fresh_instruction(before->block, before->index,
                                         before->span, IR_OP_CALL,
                                         value->type);
    th_type *element = th_seq_elem(type);
    return call && retarget(calls, caller, call, copy, &element, 1, &value, 1)
        ? call->dest : nullptr;
}

static bool
own_operand(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction,
            uint32_t index, bool returned)
{
    if (index >= instruction->n_operands
        || instruction->operands[index].kind != IR_OPD_VALUE) return true;
    ir_value_t *value = instruction->operands[index].value;
    bool transfers = returned
        ? n00bb_return_transfers_storage(caller, value)
        : n00bb_sequence_transfers_storage(value);
    ir_value_t *owned = owned_sequence(calls, caller, instruction, value,
                                       transfers);
    if (!owned) return false;
    instruction->operands[index].value = owned;
    return true;
}

static bool
box_raw_arguments(box_calls_t *calls, ir_func_t *caller,
                  ir_instr_t *instruction)
{
    if (instruction->op != IR_OP_CALL) return true;
    th_ctx *types = ir_module_types(caller->owner);
    for (uint32_t k = 1; k < instruction->n_operands; k++) {
        ir_operand_t *operand = &instruction->operands[k];
        ir_value_t *value = operand->kind == IR_OPD_VALUE ? operand->value
                                                          : nullptr;
        th_type *type = value ? th_resolve(value->type) : nullptr;
        if (!type || th_kind_of(type) != TH_INDIRECTION
            || th_is_closed(types, type)
            || address_storage(caller, value, 0) != ADDRESS_RAW) continue;
        ir_value_t *boxed = slot_reference(calls, caller, instruction, value);
        if (boxed) operand->value = boxed;
    }
    return true;
}

static bool
own_sequence_operands(box_calls_t *calls, ir_func_t *caller,
                      ir_instr_t *instruction)
{
    switch (instruction->op) {
    case IR_OP_AGGREGATE:
        for (uint32_t i = 0; i < instruction->n_operands; i++)
            if (!own_operand(calls, caller, instruction, i, false))
                return false;
        return true;
    case IR_OP_COPY: {
        ir_annot_t *promoted = ir_annot_get(&instruction->annots,
            CORE_STRING_STATIC("ir.promoted_local_write"));
        bool local = (instruction->dest_kind == IR_DEST_LOCAL
                && instruction->dest_local)
            || (promoted && promoted->kind == IR_ANNOT_BOOL && promoted->b
                && instruction->dest);
        return !local || own_operand(calls, caller, instruction, 0, false);
    }
    case IR_OP_STORE:
    case IR_OP_GLOBAL_STORE:
        return own_operand(calls, caller, instruction, 1, false);
    case IR_OP_RETURN:
    case IR_OP_FAIL:
        return own_operand(calls, caller, instruction, 0, true);
    default:
        return true;
    }
}

static ir_value_t *
converted(ir_func_t *caller, ir_instr_t *before, ir_value_t *value,
          th_type *type)
{
    if (!value || ir_type_eq(caller->owner, value->type, type)) return value;
    ir_instr_t *cast = fresh_instruction(before->block, before->index,
                                         before->span, IR_OP_CAST, type);
    return cast && ir_instr_operand_value(cast, value) ? cast->dest : nullptr;
}

static ir_value_t *
scratch_words(ir_func_t *caller, ir_instr_t *before, th_type *word,
              ir_value_t *const *values, uint32_t count, th_type **tuple_out)
{
    th_ctx *types = ir_module_types(caller->owner);
    th_type **items = core_alloc_array(th_type *, count);
    if (!items) return nullptr;
    for (uint32_t i = 0; i < count; i++) items[i] = word;
    th_type *tuple = th_tuple(types, items, count);
    core_free(items);
    ir_instr_t *scratch = tuple ? fresh_instruction(before->block,
        before->index, before->span, IR_OP_ALLOCA, th_ind(types, 0, tuple))
        : nullptr;
    ir_instr_t *words = scratch && ir_instr_operand_type(scratch, tuple)
        ? fresh_instruction(before->block, before->index,
                            before->span, IR_OP_AGGREGATE, tuple)
        : nullptr;
    bool ok = words != nullptr;
    for (uint32_t i = 0; ok && i < count; i++)
        ok = ir_instr_operand_value(words, values[i]);
    ir_instr_t *store = ok ? ir_instr_generated(before->block, IR_OP_STORE,
        before->span, CORE_STRING_STATIC("box_calls"), before->index)
        : nullptr;
    if (!store || !ir_instr_set_binding_initialization(store, true)
        || !ir_instr_operand_value(store, scratch->dest)
        || !ir_instr_operand_value(store, words->dest)) return nullptr;
    *tuple_out = tuple;
    return scratch->dest;
}

static bool
retarget_with_scratch(box_calls_t *calls, ir_func_t *caller,
                      ir_instr_t *instruction, ir_func_t *target,
                      th_type *element, th_type *tuple, uint32_t scratch_index,
                      ir_value_t *const *arguments, size_t argument_count)
{
    th_ctx *types = ir_module_types(target->owner);
    th_type **variables = nullptr;
    if (th_free_vars(types, target->type, &variables) != 2) return false;
    const th_param *formal = th_func_param(target->type, scratch_index);
    bool scratch_first = formal && th_occurs(types, variables[0],
                                             formal->type);
    th_type *actuals[2] = {scratch_first ? tuple : element,
                           scratch_first ? element : tuple};
    return retarget(calls, caller, instruction, target, actuals, 2,
                    arguments, argument_count);
}

static bool
rewrite_array_new(box_calls_t *calls, ir_func_t *caller,
                  ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    th_ctx *types = ir_module_types(module);
    if (instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    th_type *type = th_resolve(instruction->dest->type);
    ir_shape_t *shape = ir_value_shape(instruction->dest);
    ir_shape_storage_t storage = {0};
    ir_shape_measure_t measure = {0};
    ir_array_init_t initialization;
    uint32_t rank = shape ? ir_shape_rank(shape) : 0;
    if (th_kind_of(type) != TH_SEQUENCE || th_is_closed(types, type)
        || !n00bb_boxed_signature_position(module, type) || !rank
        || !ir_shape_storage(shape, &storage)
        || !storage.classification_known || !storage.flat_prefix_rank
        || storage.flat_prefix_rank > rank
        || instruction->n_operands != storage.flat_prefix_rank
        || !ir_instr_array_init(instruction, &initialization)
        || initialization != IR_ARRAY_INIT_LANGUAGE_ZERO
        || !ir_shape_element_count(shape, &measure) || !measure.value)
        return true;
    uint32_t flat = storage.flat_prefix_rank;
    th_type *word = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *u32 = th_name(types, CORE_STRING_STATIC("u32"));
    ir_func_t *create = store_function(calls,
                                       CORE_STRING_STATIC("array_with_shape"));
    if (!word || !u32 || !create) return true;
    for (uint32_t i = 0; i < flat; i++)
        if (instruction->operands[i].kind != IR_OPD_VALUE
            || !instruction->operands[i].value) return true;
    ir_value_t **values = core_alloc_array(ir_value_t *, 3 + rank + flat);
    if (!values) return false;
    ir_value_t *zero = typed_constant(caller, instruction->block,
        instruction->index, instruction->span, 0, CORE_STRING_STATIC("uint"));
    bool ok = zero != nullptr;
    for (uint32_t i = 0; ok && i < 3 + rank + flat; i++) {
        values[i] = i < 3 + rank ? zero : converted(caller, instruction,
            instruction->operands[i - 3 - rank].value, word);
        ok = values[i] != nullptr;
    }
    th_type *tuple = nullptr;
    ir_value_t *scratch = ok ? scratch_words(caller, instruction, word,
        values, 3 + rank + flat, &tuple) : nullptr;
    core_free(values);
    ir_value_t *count = scratch ? converted(caller, instruction,
                                            measure.value, word) : nullptr;
    ir_value_t *ranks = count ? typed_constant(caller, instruction->block,
        instruction->index, instruction->span, rank,
        CORE_STRING_STATIC("u32")) : nullptr;
    ir_value_t *flats = ranks ? typed_constant(caller, instruction->block,
        instruction->index, instruction->span, flat,
        CORE_STRING_STATIC("u32")) : nullptr;
    if (!flats) return false;
    ir_value_t *arguments[] = {count, ranks, flats, scratch};
    return retarget_with_scratch(calls, caller, instruction, create,
        th_seq_elem(type), tuple, 3, arguments, 4);
}

typedef struct {
    uint64_t constant;
    th_type *type;
    bool head;
} pattern_word_t;

typedef struct {
    ir_func_t *caller;
    pattern_word_t words[256];
    uint32_t count;
    th_type *variables[16];
    uint32_t slots;
} pattern_plan_t;

static bool
plan_word(pattern_plan_t *plan, uint64_t constant, th_type *type, bool head)
{
    if (plan->count >= sizeof(plan->words) / sizeof(plan->words[0]))
        return false;
    plan->words[plan->count++] = (pattern_word_t){constant, type, head};
    return true;
}

static th_type *
trait_contract(th_ctx *types, core_string_t qualified_name)
{
    const th_env *environment = th_ctx_environment(types);
    for (size_t i = 0; environment && qualified_name.data
         && i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration;
        if (!th_env_declaration_at(environment, i, &declaration))
            return nullptr;
        if (declaration.kind == TH_ENV_DECLARATION_TRAIT
            && declaration.qualified_name.data
            && core_string_eq(declaration.qualified_name, qualified_name)
            && declaration.contract)
            return declaration.contract;
    }
    return nullptr;
}

static bool
function_binds(ir_func_t *function, th_type *variable)
{
    ir_module_t *module = function->owner;
    th_ctx *types = ir_module_types(module);
    for (size_t i = 0; i < th_func_nparams(function->type); i++) {
        const th_param *formal = th_func_param(function->type, i);
        if (formal && !ir_func_parameter_is_optional(function, i)
            && th_mode_accepts_input(formal->mode)
            && th_occurs(types, variable, formal->type)
            && n00bb_boxed_signature_position(module, formal->type))
            return true;
    }
    th_type *success = th_func_ret(function->type);
    return success && th_occurs(types, variable, success)
        && n00bb_boxed_signature_position(module, success);
}

static bool
plan_pattern(pattern_plan_t *plan, th_type *pattern)
{
    th_ctx *types = ir_module_types(plan->caller->owner);
    pattern = pattern ? th_resolve(pattern) : nullptr;
    if (!pattern) return false;
    if (th_kind_of(pattern) == TH_TRAIT) {
        core_string_t name = {0};
        th_type *contract = th_trait_name(pattern, &name)
            ? trait_contract(types, name) : nullptr;
        return contract && plan_word(plan, 5, nullptr, false)
            && plan_word(plan, 0, contract, true);
    }
    if (th_is_closed(types, pattern))
        return plan_word(plan, 1, nullptr, false)
            && plan_word(plan, 0, pattern, false);
    switch (th_kind_of(pattern)) {
    case TH_VAR: {
        for (uint32_t i = 0; i < plan->slots; i++)
            if (plan->variables[i] == pattern)
                return plan_word(plan, 2, nullptr, false)
                    && plan_word(plan, i, nullptr, false)
                    && plan_word(plan, 0, nullptr, false);
        if (function_binds(plan->caller, pattern))
            return plan_word(plan, 1, nullptr, false)
                && plan_word(plan, 0, pattern, false);
        if (plan->slots >= sizeof(plan->variables)
                           / sizeof(plan->variables[0])) return false;
        uint32_t slot = plan->slots;
        plan->variables[plan->slots++] = pattern;
        th_type *contracts[16];
        uint32_t contract_count = 0;
        for (size_t i = 0; i < th_var_constraint_count(pattern); i++) {
            th_constraint constraint;
            if (!th_var_constraint_at(pattern, i, &constraint)) return false;
            if (constraint.kind == TH_CON_METHOD) continue;
            th_type *contract = constraint.kind == TH_CON_IMPLEMENTS
                ? trait_contract(types, constraint.iface) : nullptr;
            if (!contract || contract_count == 16) return false;
            contracts[contract_count++] = contract;
        }
        bool ok = plan_word(plan, 2, nullptr, false)
            && plan_word(plan, slot, nullptr, false)
            && plan_word(plan, contract_count, nullptr, false);
        for (uint32_t i = 0; ok && i < contract_count; i++)
            ok = plan_word(plan, 0, contracts[i], true);
        return ok;
    }
    case TH_SEQUENCE:
    case TH_INDIRECTION: {
        bool sequence = th_kind_of(pattern) == TH_SEQUENCE;
        return plan_word(plan, 3, nullptr, false)
            && plan_word(plan, sequence ? N00B_ABI_TYPE_ARRAY
                                        : N00B_ABI_TYPE_REFERENCE,
                         nullptr, false)
            && plan_pattern(plan, sequence ? th_seq_elem(pattern)
                                           : th_ind_referent(pattern));
    }
    case TH_NAMED: {
        th_type *head = n00bb_generic_head(types, pattern);
        size_t count = th_named_nargs(pattern);
        if (!head || !plan_word(plan, 4, nullptr, false)
            || !plan_word(plan, 0, head, true)
            || !plan_word(plan, count, nullptr, false)) return false;
        for (size_t i = 0; i < count; i++) {
            const th_arg *argument = th_named_arg(pattern, i);
            if (!argument || argument->kind != TH_ARG_TYPE
                || !plan_pattern(plan, argument->type)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

static ir_value_t *
type_index_word(ir_ctx_t *context, ir_func_t *caller, ir_instr_t *before,
                th_type *type, bool head, th_type *word)
{
    th_type *u32 = th_name(ir_module_types(caller->owner),
                           CORE_STRING_STATIC("u32"));
    ir_instr_t *index = fresh_instruction(before->block,
        before->index, before->span, IR_OP_TYPE_INDEX, u32);
    if (!index || !ir_instr_operand_type(index, type)) return nullptr;
    if (head && !ir_annot_set(context, &index->annots,
            (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.type_index.static"),
                         .kind = IR_ANNOT_BOOL, .b = true})) return nullptr;
    return converted(caller, before, index->dest, word);
}

static ir_value_t *
type_index_of_value(box_calls_t *calls, ir_func_t *caller, ir_value_t *value,
                    ir_instr_t *instruction)
{
    th_type *u32 = th_name(ir_module_types(caller->owner),
                           CORE_STRING_STATIC("u32"));
    if (calls->indexed_function != caller) {
        core_dict_free(&calls->type_indices);
        core_dict_init(&calls->type_indices, nullptr, nullptr);
        calls->indexed_function = caller;
    }
    bool known = false;
    ir_value_t *memo = core_dict_get(&calls->type_indices, value, &known);
    if (known) return memo;
    ir_block_t *block = instruction->block;
    uint32_t at = instruction->index;
    bool shared = value->kind == IR_VAL_PARAM
        || (value->kind == IR_VAL_INSTR && value->def && value->def->block);
    if (value->kind == IR_VAL_PARAM) {
        block = ir_block_entry(caller);
        at = 0;
    }
    else if (shared) {
        block = value->def->block;
        at = value->def->index + 1;
        while (at < block->n_instrs && block->instrs[at]->op == IR_OP_PHI)
            at++;
    }
    th_type *source = th_resolve(value->type);
    bool any = th_builtin_applied(source, TH_BUILTIN_ANY, 0) != nullptr;
    ir_func_t *field = runtime_function(calls, any
        ? CORE_STRING_STATIC("any_type_field")
        : CORE_STRING_STATIC("type_field_of"));
    ir_instr_t *actual = u32 && field ? fresh_instruction(block, at,
        instruction->span, IR_OP_CALL, u32) : nullptr;
    if (!actual || !retarget(calls, caller, actual, field, &source,
                             any ? 0 : 1, &value, 1)) return nullptr;
    ir_value_t *mask = typed_constant(caller, block, actual->index + 1,
        instruction->span, UINT32_C(0x7fffffff), CORE_STRING_STATIC("u32"));
    ir_instr_t *masked = mask ? fresh_instruction(block,
        mask->def->index + 1, instruction->span, IR_OP_AND, u32) : nullptr;
    if (!masked || !ir_instr_operand_value(masked, actual->dest)
        || !ir_instr_operand_value(masked, mask)) return nullptr;
    if (shared) core_dict_put(&calls->type_indices, value, masked->dest);
    return masked->dest;
}

static ir_value_t *
type_index_of_box(box_calls_t *calls, ir_func_t *caller,
                  ir_instr_t *instruction)
{
    return type_index_of_value(calls, caller, instruction->operands[0].value,
                               instruction);
}

static bool
rewrite_closed_type_test(box_calls_t *calls, ir_func_t *caller,
                         ir_instr_t *instruction, th_type *pattern)
{
    th_type *u32 = th_name(ir_module_types(caller->owner),
                           CORE_STRING_STATIC("u32"));
    ir_value_t *actual = type_index_of_box(calls, caller, instruction);
    ir_value_t *expected = actual ? type_index_word(
        calls->analysis->ir_context, caller, instruction, pattern, false,
        u32) : nullptr;
    if (!expected) return false;
    instruction->op = IR_OP_EQ;
    instruction->n_operands = 0;
    instruction->n_mods = 0;
    return ir_instr_operand_value(instruction, actual)
        && ir_instr_operand_value(instruction, expected);
}

static bool
rewrite_type_test(box_calls_t *calls, ir_func_t *caller,
                  ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    th_ctx *types = ir_module_types(module);
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_TYPE
        || !instruction->operands[0].value
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *value = instruction->operands[0].value;
    th_type *source = th_resolve(value->type);
    th_type *word = th_name(types, CORE_STRING_STATIC("uint"));
    ir_func_t *target = runtime_function(calls,
                                         CORE_STRING_STATIC("type_matches"));
    bool boxed = n00bb_boxed_signature_position(module, source)
        || th_builtin_applied(source, TH_BUILTIN_ANY, 0);
    th_type *pattern = instruction->operands[1].type;
    if (!word || !target || !boxed) return true;
    if (pattern && th_kind_of(th_resolve(pattern)) != TH_TRAIT
        && th_is_closed(types, pattern))
        return rewrite_closed_type_test(calls, caller, instruction, pattern);
    pattern_plan_t *plan = core_alloc(pattern_plan_t);
    if (!plan) return false;
    plan->caller = caller;
    if (!plan_pattern(plan, pattern)) {
        core_free(plan);
        return true;
    }
    uint32_t total = plan->count + plan->slots;
    ir_value_t **values = core_alloc_array(ir_value_t *, total);
    bool ok = values != nullptr;
    for (uint32_t i = 0; ok && i < total; i++) {
        pattern_word_t *entry = i < plan->count ? &plan->words[i] : nullptr;
        values[i] = entry && entry->type
            ? type_index_word(calls->analysis->ir_context, caller,
                              instruction, entry->type, entry->head, word)
            : typed_constant(caller, instruction->block, instruction->index,
                instruction->span, entry ? entry->constant : 0,
                CORE_STRING_STATIC("uint"));
        ok = values[i] != nullptr;
    }
    th_type *tuple = nullptr;
    ir_value_t *scratch = ok ? scratch_words(caller, instruction, word,
                                             values, total, &tuple) : nullptr;
    core_free(values);
    ir_value_t *length = scratch ? typed_constant(caller, instruction->block,
        instruction->index, instruction->span, plan->count,
        CORE_STRING_STATIC("uint")) : nullptr;
    core_free(plan);
    ir_value_t *actual = length
        ? type_index_of_box(calls, caller, instruction) : nullptr;
    if (!actual) return false;
    ir_value_t *arguments[] = {actual, scratch, length};
    return retarget(calls, caller, instruction, target, &tuple, 1,
                    arguments, 3);
}

static bool
rewrite_float_remainder(box_calls_t *calls, ir_func_t *caller,
                        ir_instr_t *instruction)
{
    ir_value_t *arguments[2] = {0};
    for (uint32_t k = 0; k < 2 && k < instruction->n_operands; k++)
        if (instruction->operands[k].kind == IR_OPD_VALUE)
            arguments[k] = instruction->operands[k].value;
    if (instruction->n_operands != 2 || !arguments[0] || !arguments[1])
        return true;
    ir_machine_t machine = ir_machine_of(caller->owner, arguments[0]->type);
    if (machine.class != IR_CLASS_FLOAT) return true;
    ir_func_t *target = calls->floating ? ir_func_find(calls->floating,
        machine.width == 32 ? CORE_STRING_STATIC("remainder_f32")
                            : CORE_STRING_STATIC("remainder_f64")) : nullptr;
    return target && retarget(calls, caller, instruction, target, nullptr, 0,
                              arguments, 2);
}

static bool
rewrite_type_of(box_calls_t *calls, ir_func_t *caller,
                ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *value = instruction->operands[0].value;
    th_type *source = th_resolve(value->type);
    if (!(n00bb_boxed_signature_position(module, source)
          || th_builtin_applied(source, TH_BUILTIN_ANY, 0))) return true;
    ir_value_t *actual = type_index_of_box(calls, caller, instruction);
    if (!actual) return false;
    instruction->op = IR_OP_BITCAST;
    instruction->n_operands = 0;
    instruction->n_mods = 0;
    return ir_instr_operand_value(instruction, actual);
}

static bool
rewrite_shape_check(box_calls_t *calls, ir_func_t *caller,
                    ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    th_ctx *types = ir_module_types(module);
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || !instruction->shape_constraint) return true;
    ir_value_t *items = instruction->operands[0].value;
    th_type *sequence = th_resolve(items->type);
    uint32_t rank = ir_shape_rank(instruction->shape_constraint);
    th_type *word = th_name(types, CORE_STRING_STATIC("uint"));
    ir_func_t *target = runtime_function(calls,
        CORE_STRING_STATIC("array_shape_check"));
    if (th_kind_of(sequence) != TH_SEQUENCE || th_is_closed(types, sequence)
        || !rank || !word || !target) return true;
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t expected = {0};
        if (!ir_shape_extent(instruction->shape_constraint, i, &expected)
            || (expected.kind != IR_SHAPE_EXTENT_STATIC
                && expected.kind != IR_SHAPE_EXTENT_UNRESOLVED
                && !expected.value)) return true;
    }
    ir_value_t **values = core_alloc_array(ir_value_t *, rank);
    if (!values) return false;
    bool ok = true;
    for (uint32_t i = 0; ok && i < rank; i++) {
        ir_shape_extent_t expected = {0};
        ok = ir_shape_extent(instruction->shape_constraint, i, &expected);
        if (!ok) break;
        values[i] = expected.kind == IR_SHAPE_EXTENT_STATIC
            ? typed_constant(caller, instruction->block, instruction->index,
                instruction->span, expected.static_value,
                CORE_STRING_STATIC("uint"))
            : expected.kind == IR_SHAPE_EXTENT_UNRESOLVED
            ? typed_constant(caller, instruction->block, instruction->index,
                instruction->span, UINT64_MAX, CORE_STRING_STATIC("uint"))
            : converted(caller, instruction, expected.value, word);
        ok = values[i] != nullptr;
    }
    th_type *tuple = nullptr;
    ir_value_t *scratch = ok ? scratch_words(caller, instruction, word,
                                             values, rank, &tuple) : nullptr;
    core_free(values);
    ir_value_t *ranks = scratch ? typed_constant(caller, instruction->block,
        instruction->index, instruction->span, rank,
        CORE_STRING_STATIC("u32")) : nullptr;
    if (!ranks) return false;
    ir_value_t *arguments[] = {items, ranks, scratch};
    return retarget_with_scratch(calls, caller, instruction, target,
        th_seq_elem(sequence), tuple, 2, arguments, 3);
}

static bool
rewrite_nominal_cast(box_calls_t *calls, ir_func_t *caller,
                     ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *source = instruction->operands[0].value;
    th_type *type = th_resolve(instruction->dest->type);
    th_type *target_shape = ir_type_definition(module, type);
    th_type *source_shape = ir_type_definition(module, source->type);
    if (!source_shape && th_kind_of(th_resolve(source->type)) == TH_RECORD)
        source_shape = th_resolve(source->type);
    if (!target_shape || !source_shape
        || th_kind_of(target_shape) != TH_RECORD
        || th_kind_of(source_shape) != TH_RECORD
        || th_is_closed(ir_module_types(module), type)
        || !n00bb_boxed_signature_position(module, type)) return true;
    ir_func_t *create = store_function(calls,
                                       CORE_STRING_STATIC("record_new"));
    ir_func_t *put = store_function(calls, CORE_STRING_STATIC("field_put"));
    if (!create || !put) return true;
    if (!retarget(calls, caller, instruction, create, &type, 1, nullptr, 0))
        return false;
    ir_block_t *block = instruction->block;
    uint32_t at = instruction->index + 1;
    for (size_t i = 0; i < th_record_len(target_shape); i++) {
        const th_member *member = th_record_member(target_shape, i);
        th_type *field_type = member && member->name.data
            ? th_record_lookup(source_shape, member->name) : nullptr;
        ir_instr_t *read = field_type ? fresh_instruction(block, at,
            instruction->span, IR_OP_EXTRACT, field_type) : nullptr;
        if (!read || !ir_instr_operand_value(read, source)
            || !ir_instr_operand_name(read, member->name)) return false;
        ir_value_t *ordinal = typed_constant(caller, block, at + 1,
            instruction->span, i, CORE_STRING_STATIC("u32"));
        ir_instr_t *store = ordinal ? ir_instr_generated(block, IR_OP_CALL,
            instruction->span, CORE_STRING_STATIC("box_calls"), at + 2)
            : nullptr;
        ir_value_t *arguments[] = {instruction->dest, ordinal, read->dest};
        if (!store || !two_variable_call(calls, caller, store, put, type,
                                         field_type, arguments, 3))
            return false;
        at += 3;
    }
    return true;
}

static bool
rewrite_record_literal(box_calls_t *calls, ir_func_t *caller,
                       ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    th_type *type = th_resolve(instruction->dest->type);
    bool tuple = th_builtin_kind_of(type) == TH_BUILTIN_TUPLE;
    th_type *definition = tuple ? nullptr : ir_type_definition(module, type);
    if (!tuple && !definition && th_kind_of(type) == TH_RECORD)
        definition = type;
    bool variant = false;
    th_named_info(type, nullptr, &variant, nullptr);
    size_t fields_declared = tuple ? th_named_nargs(type)
        : definition && th_kind_of(definition) == TH_RECORD
            ? th_record_len(definition) : SIZE_MAX;
    if (variant || fields_declared != instruction->n_operands
        || !n00bb_boxed_signature_position(module, type)) return true;
    uint32_t count = instruction->n_operands;
    for (uint32_t i = 0; i < count; i++)
        if (instruction->operands[i].kind != IR_OPD_VALUE
            || !instruction->operands[i].value) return true;
    ir_func_t *create = store_function(calls,
                                       CORE_STRING_STATIC("record_new"));
    ir_func_t *put = store_function(calls, CORE_STRING_STATIC("field_put"));
    if (!create || !put) return true;
    ir_value_t **fields = core_alloc_array(ir_value_t *, count + 1);
    if (!fields) return false;
    for (uint32_t i = 0; i < count; i++)
        fields[i] = instruction->operands[i].value;
    bool ok = retarget(calls, caller, instruction, create, &type, 1,
                       nullptr, 0);
    ir_block_t *block = instruction->block;
    uint32_t at = instruction->index + 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        ir_value_t *ordinal = typed_constant(caller, block, at,
            instruction->span, i, CORE_STRING_STATIC("u32"));
        ir_instr_t *store = ordinal ? ir_instr_generated(block, IR_OP_CALL,
            instruction->span, CORE_STRING_STATIC("box_calls"), at + 1)
            : nullptr;
        ir_value_t *arguments[] = {instruction->dest, ordinal, fields[i]};
        ok = store && two_variable_call(calls, caller, store, put, type,
                                        fields[i]->type, arguments, 3);
        at += 2;
    }
    core_free(fields);
    return ok;
}

static bool
rewrite_array_literal(box_calls_t *calls, ir_func_t *caller,
                      ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    ir_aggregate_form_t form = IR_AGGREGATE_ELEMENTS;
    th_type *sequence = instruction->dest_kind == IR_DEST_VALUE
        && instruction->dest ? th_resolve(instruction->dest->type) : nullptr;
    if (sequence && th_kind_of(sequence) != TH_SEQUENCE
        && !th_is_closed(ir_module_types(module), sequence))
        return rewrite_record_literal(calls, caller, instruction);
    if (!sequence || th_kind_of(sequence) != TH_SEQUENCE
        || th_is_closed(ir_module_types(module), sequence)
        || (ir_instr_aggregate_form(instruction, &form)
            && form != IR_AGGREGATE_ELEMENTS)) return true;
    th_type *element = th_seq_elem(sequence);
    uint32_t count = instruction->n_operands;
    for (uint32_t i = 0; i < count; i++)
        if (instruction->operands[i].kind != IR_OPD_VALUE
            || !instruction->operands[i].value
            || !ir_type_eq(module, instruction->operands[i].value->type,
                           element)) return true;
    ir_func_t *create = store_function(calls,
        CORE_STRING_STATIC("array_with_length"));
    ir_func_t *put = store_function(calls, CORE_STRING_STATIC("array_put"));
    if (!create || !put) return true;
    ir_value_t **elements = core_alloc_array(ir_value_t *, count + 1);
    if (!elements) return false;
    for (uint32_t i = 0; i < count; i++)
        elements[i] = instruction->operands[i].value;
    ir_value_t *length = typed_constant(caller, instruction->block,
        instruction->index, instruction->span, count,
        CORE_STRING_STATIC("uint"));
    bool ok = length && retarget(calls, caller, instruction, create,
                                 &element, 1, &length, 1);
    ir_block_t *block = instruction->block;
    uint32_t at = instruction->index + 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        ir_value_t *index = typed_constant(caller, block, at,
            instruction->span, i, CORE_STRING_STATIC("int"));
        ir_instr_t *store = index ? ir_instr_generated(block, IR_OP_CALL,
            instruction->span, CORE_STRING_STATIC("box_calls"), at + 1)
            : nullptr;
        ir_value_t *arguments[] = {instruction->dest, index, elements[i]};
        ok = store && retarget(calls, caller, store, put, &element, 1,
                               arguments, 3);
        at += 2;
    }
    core_free(elements);
    return ok;
}

static bool
rewrite_reference_compare(box_calls_t *calls, ir_func_t *caller,
                          ir_instr_t *instruction)
{
    if (instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_VALUE
        || instruction->dest_kind != IR_DEST_VALUE) return true;
    ir_value_t *arguments[] = {instruction->operands[0].value,
                               instruction->operands[1].value};
    if (!open_reference(caller, arguments[0])
        || !open_reference(caller, arguments[1])
        || !ir_type_eq(caller->owner, arguments[0]->type,
                       arguments[1]->type)) return true;
    th_type *referent = th_ind_referent(th_resolve(arguments[0]->type));
    ir_func_t *target = runtime_function(calls,
        instruction->op == IR_OP_EQ ? CORE_STRING_STATIC("ref_equal")
                                    : CORE_STRING_STATIC("ref_differ"));
    return !target || retarget(calls, caller, instruction, target, &referent,
                               1, arguments, 2);
}

static bool
native_named(const ir_instr_t *call, core_string_t name)
{
    ir_func_t *target = n00bb_direct_call_target(call);
    ir_annot_t *native = target ? ir_annot_get(&target->annots,
        CORE_STRING_STATIC("n00bb.abi.native")) : nullptr;
    return native && native->kind == IR_ANNOT_STR && native->s.data
        && core_string_eq(native->s, name);
}

static bool
rewrite_native_call(box_calls_t *calls, ir_func_t *caller,
                    ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 3
        || instruction->operands[1].kind != IR_OPD_VALUE
        || instruction->operands[2].kind != IR_OPD_VALUE) return true;
    ir_value_t *arguments[] = {instruction->operands[1].value,
                               instruction->operands[2].value};
    th_ctx *types = ir_module_types(module);
    if (native_named(instruction, CORE_STRING_STATIC("__n00bb_array_prefix"))) {
        th_type *sequence = th_resolve(arguments[0]->type);
        if (th_kind_of(sequence) != TH_SEQUENCE
            || th_is_closed(types, sequence)
            || instruction->dest_kind != IR_DEST_VALUE) return true;
        th_type *element = th_seq_elem(sequence);
        ir_func_t *target = store_function(calls,
            CORE_STRING_STATIC("array_prefix_of"));
        return !target || retarget(calls, caller, instruction, target,
                                   &element, 1, arguments, 2);
    }
    if (native_named(instruction, CORE_STRING_STATIC("__n00bb_array_clear"))) {
        th_type *base = th_resolve(arguments[0]->type);
        th_type *sequence = th_kind_of(base) == TH_INDIRECTION
            ? th_resolve(th_ind_referent(base)) : nullptr;
        ir_func_t *target = store_function(calls,
            CORE_STRING_STATIC("array_clear_at"));
        if (!sequence || th_kind_of(sequence) != TH_SEQUENCE || !target
            || th_is_closed(types, sequence)) return true;
        if (!open_reference(caller, arguments[0])) {
            if (address_storage(caller, arguments[0], 0) != ADDRESS_RAW)
                return true;
            arguments[0] = slot_reference(calls, caller, instruction,
                                          arguments[0]);
            if (!arguments[0]) return false;
        }
        th_type *element = th_seq_elem(sequence);
        return retarget(calls, caller, instruction, target, &element, 1,
                        arguments, 2);
    }
    return true;
}

static bool
rewrite_alloc(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_TYPE
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || !heap_allocation(caller, instruction)) return true;
    th_type *element = instruction->operands[0].type;
    th_type *reference = th_resolve(instruction->dest->type);
    if (!element || th_is_closed(ir_module_types(module), element)
        || !n00bb_boxed_signature_position(module, element)
        || th_kind_of(reference) != TH_INDIRECTION
        || !ir_type_eq(module, th_ind_referent(reference), element))
        return true;
    ir_func_t *target = store_function(calls,
                                       CORE_STRING_STATIC("new_storage"));
    return !target || retarget(calls, caller, instruction, target, &element,
                               1, nullptr, 0);
}


static int
index_entry(ir_module_t *module, const ir_value_t *index)
{
    ir_machine_t machine = ir_machine_of(module, index->type);
    if (!machine.width || machine.width > 64) return 0;
    if (machine.class == IR_CLASS_SINT) return 1;
    return machine.class == IR_CLASS_UINT ? 2 : 0;
}

static bool
widen_index(ir_func_t *caller, ir_instr_t *instruction, ir_value_t **index,
            int entry)
{
    *index = converted(caller, instruction, *index,
        th_name(ir_module_types(caller->owner), entry == 1
            ? CORE_STRING_STATIC("int") : CORE_STRING_STATIC("uint")));
    return *index != nullptr;
}

static bool
heap_allocation(ir_func_t *caller, ir_instr_t *allocation)
{
    core_string_t storage_class = ir_instr_storage_class(allocation,
                                                         nullptr);
    return !ir_allocation_has_only_local_uses(caller, allocation)
        && (!storage_class.data
            || core_string_eq(storage_class, CORE_STRING_STATIC("gc")));
}

static address_storage_t
address_storage(ir_func_t *caller, ir_value_t *address, unsigned depth)
{
    ir_instr_t *visiting[17] = {0};
    return address_storage_within(caller, address, depth, visiting);
}

static address_storage_t
address_storage_within(ir_func_t *caller, ir_value_t *address, unsigned depth,
                       ir_instr_t **visiting)
{
    if (!address || depth > 16) return ADDRESS_UNKNOWN;
    if (address->kind == IR_VAL_PARAM) {
        const th_param *parameter = address->index
            < th_func_nparams(caller->type)
            ? th_func_param(caller->type, address->index) : nullptr;
        return !parameter ? ADDRESS_UNKNOWN
            : th_mode_is_in(parameter->mode) ? ADDRESS_BOXED : ADDRESS_RAW;
    }
    if (address->kind != IR_VAL_INSTR) return ADDRESS_BOXED;
    ir_instr_t *definition = address->def;
    if (!definition) return ADDRESS_UNKNOWN;
    if (definition->op == IR_OP_ALLOC)
        return heap_allocation(caller, definition)
            ? ADDRESS_BOXED : ADDRESS_RAW;
    if (definition->op == IR_OP_ALLOCA || definition->op == IR_OP_ADDRESS_OF)
        return ADDRESS_RAW;
    if (definition->op != IR_OP_COPY && definition->op != IR_OP_PHI)
        return ADDRESS_BOXED;
    visiting[depth] = definition;
    address_storage_t storage = ADDRESS_UNKNOWN;
    for (uint32_t i = 0; i < definition->n_operands; i++) {
        ir_operand_t *operand = &definition->operands[i];
        ir_value_t *source = operand->kind == IR_OPD_VALUE ? operand->value
            : operand->kind == IR_OPD_PHI_ARM ? operand->phi.value : nullptr;
        if (!source) continue;
        bool cycle = false;
        for (unsigned d = 0; !cycle && d <= depth; d++)
            cycle = source && source->kind == IR_VAL_INSTR
                && source->def == visiting[d];
        if (cycle) continue;
        address_storage_t incoming = address_storage_within(caller, source,
            depth + 1, visiting);
        if (incoming == ADDRESS_UNKNOWN
            || (storage != ADDRESS_UNKNOWN && incoming != storage))
            return ADDRESS_UNKNOWN;
        storage = incoming;
    }
    return storage;
}

static bool
boxed_address(ir_func_t *caller, ir_value_t *address)
{
    return address_storage(caller, address, 0) == ADDRESS_BOXED;
}

static bool
open_reference(ir_func_t *caller, ir_value_t *address)
{
    th_ctx *types = ir_module_types(caller->owner);
    th_type *reference = address ? th_resolve(address->type) : nullptr;
    return reference && th_kind_of(reference) == TH_INDIRECTION
        && !th_is_closed(types, reference)
        && boxed_address(caller, address);
}

static bool
rewrite_store(box_calls_t *calls, ir_func_t *caller, ir_instr_t *instruction)
{
    if (instruction->n_mods || instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_VALUE) return true;
    ir_value_t *arguments[] = {instruction->operands[0].value,
                               instruction->operands[1].value};
    if (!open_reference(caller, arguments[0])) return true;
    th_type *referent = th_ind_referent(th_resolve(arguments[0]->type));
    th_type *payload = open_option_payload(caller->owner, referent);
    if (payload) {
        ir_func_t *target = store_function(calls,
                                           CORE_STRING_STATIC("option_set"));
        return !target || retarget(calls, caller, instruction, target,
                                   &payload, 1, arguments, 2);
    }
    if (!n00bb_boxed_signature_position(caller->owner, referent)) return true;
    ir_func_t *target = store_function(calls, CORE_STRING_STATIC("ref_set"));
    return !target || retarget(calls, caller, instruction, target, &referent,
                               1, arguments, 2);
}

static bool
rewrite_sequence_reference(box_calls_t *calls, ir_func_t *caller,
                           ir_instr_t *instruction, ir_value_t **arguments)
{
    ir_module_t *module = caller->owner;
    th_type *base = th_resolve(arguments[0]->type);
    th_type *sequence = th_kind_of(base) == TH_INDIRECTION
        ? th_resolve(th_ind_referent(base)) : nullptr;
    th_type *reference = th_resolve(instruction->dest->type);
    if (!sequence || th_kind_of(sequence) != TH_SEQUENCE
        || th_is_closed(ir_module_types(module), sequence)
        || th_kind_of(reference) != TH_INDIRECTION) return true;
    th_type *element = th_seq_elem(sequence);
    int entry = index_entry(module, arguments[1]);
    ir_func_t *target = store_function(calls, entry == 1
        ? CORE_STRING_STATIC("array_ref")
        : CORE_STRING_STATIC("array_ref_unsigned"));
    if (!entry || !target
        || !ir_type_eq(module, element, th_ind_referent(reference)))
        return true;
    if (!open_reference(caller, arguments[0])) {
        if (address_storage(caller, arguments[0], 0) != ADDRESS_RAW)
            return true;
        arguments[0] = slot_reference(calls, caller, instruction,
                                      arguments[0]);
        if (!arguments[0]) return false;
    }
    return widen_index(caller, instruction, &arguments[1], entry)
        && retarget(calls, caller, instruction, target, &element, 1,
                    arguments, 2);
}

static bool
rewrite_sequence_get(box_calls_t *calls, ir_func_t *caller,
                     ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_mods || instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_VALUE
        || instruction->dest_kind != IR_DEST_VALUE || !instruction->dest)
        return true;
    ir_value_t *arguments[] = {instruction->operands[0].value,
                               instruction->operands[1].value};
    th_type *sequence = th_resolve(arguments[0]->type);
    if (th_kind_of(sequence) == TH_INDIRECTION)
        return rewrite_sequence_reference(calls, caller, instruction,
                                          arguments);
    if (th_kind_of(sequence) != TH_SEQUENCE
        || th_is_closed(ir_module_types(module), sequence)) return true;
    th_type *element = th_seq_elem(sequence);
    int entry = index_entry(module, arguments[1]);
    if (!entry || !ir_type_eq(module, element, instruction->dest->type))
        return true;
    th_type *payload = open_option_payload(module, element);
    if (payload) {
        ir_func_t *target = runtime_function(calls, entry == 1
            ? CORE_STRING_STATIC("array_get_option")
            : CORE_STRING_STATIC("array_get_option_unsigned"));
        return !target
            || (widen_index(caller, instruction, &arguments[1], entry)
                && retarget(calls, caller, instruction, target,
                            &payload, 1, arguments, 2));
    }
    if (!n00bb_boxed_signature_position(module, element)) return true;
    ir_func_t *target = store_function(calls, entry == 1
        ? CORE_STRING_STATIC("array_get")
        : CORE_STRING_STATIC("array_get_unsigned"));
    return !target
        || (widen_index(caller, instruction, &arguments[1], entry)
            && retarget(calls, caller, instruction, target, &element,
                        1, arguments, 2));
}

static bool
rewrite_sequence_set(box_calls_t *calls, ir_func_t *caller,
                     ir_instr_t *instruction)
{
    ir_module_t *module = caller->owner;
    if (instruction->n_mods || instruction->n_operands != 3
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_VALUE
        || instruction->operands[2].kind != IR_OPD_VALUE) return true;
    ir_value_t *arguments[] = {instruction->operands[0].value,
                               instruction->operands[1].value,
                               instruction->operands[2].value};
    th_type *base = th_resolve(arguments[0]->type);
    th_type *sequence = th_kind_of(base) == TH_INDIRECTION
        ? th_resolve(th_ind_referent(base)) : nullptr;
    if (!sequence || th_kind_of(sequence) != TH_SEQUENCE) return true;
    th_type *element = th_seq_elem(sequence);
    bool rows = th_kind_of(th_resolve(element)) == TH_SEQUENCE;
    bool open = !th_is_closed(ir_module_types(module), sequence);
    int entry = index_entry(module, arguments[1]);
    if (!entry || (!open && !rows)
        || !ir_type_eq(module, element, arguments[2]->type)) return true;
    th_type *payload = open_option_payload(module, element);
    th_type *actual = payload ? payload : element;
    ir_func_t *target = store_function(calls, payload
        ? entry == 1 ? CORE_STRING_STATIC("array_set_option")
                     : CORE_STRING_STATIC("array_set_option_unsigned")
        : entry == 1 ? CORE_STRING_STATIC("array_set")
                     : CORE_STRING_STATIC("array_set_unsigned"));
    if (!target || (!payload && !rows
                    && !n00bb_boxed_signature_position(module, element)))
        return true;
    if (open && !open_reference(caller, arguments[0])) {
        if (address_storage(caller, arguments[0], 0) != ADDRESS_RAW)
            return true;
        arguments[0] = slot_reference(calls, caller, instruction,
                                      arguments[0]);
        if (!arguments[0]) return false;
    }
    return widen_index(caller, instruction, &arguments[1], entry)
        && retarget(calls, caller, instruction, target, &actual, 1,
                    arguments, 3);
}

enum { DISPATCH_CACHE_ARMS = 8 };

typedef struct {
    ir_instr_t *edge;
    uint32_t operand;
    ir_block_t *arm;
} dispatch_arm_t;
core_list_decl(dispatch_arm_t);

static bool
dispatch_chain_arms(ir_func_t *dispatcher, core_list_t(dispatch_arm_t) *arms)
{
    ir_block_t *block = ir_block_entry(dispatcher);
    for (uint32_t guard = 0; block && guard < dispatcher->n_blocks; guard++) {
        ir_instr_t *term = ir_block_terminator(block);
        if (term && term->op == IR_OP_JUMP && term->n_operands == 1
            && term->operands[0].kind == IR_OPD_BLOCK) {
            core_list_push(*arms, ((dispatch_arm_t){term, 0,
                                                    term->operands[0].block}));
            return true;
        }
        if (!term || term->op != IR_OP_BRANCH || term->n_operands != 3
            || term->operands[1].kind != IR_OPD_BLOCK
            || term->operands[2].kind != IR_OPD_BLOCK) return false;
        core_list_push(*arms, ((dispatch_arm_t){term, 1,
                                                term->operands[1].block}));
        block = term->operands[2].block;
        ir_instr_t *next = ir_block_terminator(block);
        if (next && next->op == IR_OP_UNREACHABLE) return true;
    }
    return false;
}

static ir_value_t *
dispatch_call(box_calls_t *calls, ir_func_t *caller, ir_block_t *block,
              core_string_t name, th_type *result, ir_value_t **arguments,
              size_t count, ir_span_t span)
{
    ir_func_t *target = runtime_function(calls, name);
    ir_instr_t *call = target ? fresh_instruction(block, block->n_instrs,
        span, IR_OP_CALL, result) : nullptr;
    return call && retarget(calls, caller, call, target, nullptr, 0,
                            arguments, count) ? call->dest : nullptr;
}

static bool
cache_dispatcher(box_calls_t *calls, ir_func_t *dispatcher)
{
    if (!ir_annot_get(&dispatcher->annots,
                      CORE_STRING_STATIC("n00bb.closed_dispatcher"))
        || !dispatcher->n_params) return true;
    core_list_t(dispatch_arm_t) arms = core_list_new(dispatch_arm_t);
    if (!dispatch_chain_arms(dispatcher, &arms)
        || arms.len < DISPATCH_CACHE_ARMS) {
        core_list_free(arms);
        return true;
    }
    th_ctx *types = ir_module_types(dispatcher->owner);
    th_type *u32 = th_name(types, CORE_STRING_STATIC("u32"));
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *nil = th_name(types, CORE_STRING_STATIC("nil"));
    ir_block_t *chain = ir_block_entry(dispatcher);
    ir_span_t span = arms.data[0].edge->span;
    core_string_t module = ir_module_name(dispatcher->owner);
    core_buffer_t identity = {0};
    core_buffer_printf(&identity, "%.*s.%.*s", (int)module.u8_bytes,
        module.data, (int)dispatcher->name.u8_bytes, dispatcher->name.data);
    uint64_t site = core_xxh3_64bits_raw(identity.data, identity.byte_len);
    core_buffer_release(&identity);
    ir_block_t *probe = ir_block_add_entry(dispatcher,
        CORE_STRING_STATIC("__n00bb_dispatch_probe"));
    ir_block_t *cached = ir_block_add(dispatcher,
        CORE_STRING_STATIC("__n00bb_dispatch_cached"));
    ir_value_t *receiver = dispatcher->params[0];
    ir_value_t *index = probe && cached
        ? type_index_of_value(calls, dispatcher, receiver, arms.data[0].edge)
        : nullptr;
    ir_value_t *key = index ? typed_constant(dispatcher, probe,
        probe->n_instrs, span, site, CORE_STRING_STATIC("uint")) : nullptr;
    ir_value_t *probe_arguments[] = {key, index};
    ir_value_t *arm = key ? dispatch_call(calls, dispatcher, probe,
        CORE_STRING_STATIC("dispatch_arm"), u32, probe_arguments, 2, span)
        : nullptr;
    ir_value_t *zero = arm ? typed_constant(dispatcher, probe,
        probe->n_instrs, span, 0, CORE_STRING_STATIC("u32")) : nullptr;
    ir_instr_t *missed = zero ? fresh_instruction(probe, probe->n_instrs,
        span, IR_OP_EQ, boolean) : nullptr;
    ir_instr_t *branch = missed ? ir_instr_add(probe, IR_OP_BRANCH) : nullptr;
    ir_instr_t *select = branch ? ir_instr_add(cached, IR_OP_SWITCH) : nullptr;
    bool ok = select && ir_instr_operand_value(missed, arm)
        && ir_instr_operand_value(missed, zero)
        && ir_instr_operand_value(branch, missed->dest)
        && ir_instr_operand_block(branch, chain)
        && ir_instr_operand_block(branch, cached)
        && ir_instr_operand_value(select, arm)
        && ir_instr_operand_default(select, chain);
    if (branch) branch->span = span;
    if (select) select->span = span;
    for (size_t k = 0; ok && k < arms.len; k++) {
        dispatch_arm_t *entry = &arms.data[k];
        core_buffer_t record_name = {0};
        core_buffer_printf(&record_name, "__n00bb_dispatch_record_%zu", k);
        ir_block_t *record = record_name.data ? ir_block_add(dispatcher,
            core_buffer_string(&record_name)) : nullptr;
        core_buffer_release(&record_name);
        ir_value_t *ordinal = record ? typed_constant(dispatcher, record, 0,
            span, k + 1, CORE_STRING_STATIC("u32")) : nullptr;
        ir_value_t *record_arguments[] = {key, index, ordinal};
        ir_value_t *recorded = ordinal ? dispatch_call(calls, dispatcher,
            record, CORE_STRING_STATIC("dispatch_record"), nil,
            record_arguments, 3, span) : nullptr;
        ir_instr_t *jump = recorded ? ir_instr_add(record, IR_OP_JUMP)
                                    : nullptr;
        ok = jump && ir_instr_operand_block(jump, entry->arm)
            && ir_instr_operand_switch_arm(select, (ir_int_t)(k + 1),
                                           entry->arm);
        if (jump) jump->span = span;
        if (ok) entry->edge->operands[entry->operand].block = record;
    }
    core_list_free(arms);
    return ok;
}

static bool
rewrite_emitted(box_calls_t *calls)
{
    n00bb_link_analysis_t *analysis = calls->analysis;
    size_t count = analysis->emitted_functions.len;
    for (size_t f = 0; f < count; f++) {
        ir_func_t *function = n00bb_cached_body(
            analysis->emitted_functions.data[f]);
        if (!function || !function->n_blocks
            || runtime_module(calls,function->owner)) continue;
        if (!cache_dispatcher(calls, function)) return false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *instruction = block->instrs[i];
                if (!own_sequence_operands(calls,function, instruction)
                    || !box_raw_arguments(calls,function, instruction))
                    return false;
                bool rewritten = true;
                switch (instruction->op) {
                case IR_OP_LOAD:
                    rewritten = rewrite_load(calls,function, instruction);
                    break;
                case IR_OP_STORE:
                    rewritten = rewrite_store(calls,function, instruction);
                    break;
                case IR_OP_EXTRACT:
                    rewritten = rewrite_extract(calls,function,
                                                instruction);
                    break;
                case IR_OP_TYPE_OF:
                    rewritten = rewrite_type_of(calls,function, instruction);
                    break;
                case IR_OP_REM:
                    rewritten = rewrite_float_remainder(calls, function,
                                                        instruction);
                    break;
                case IR_OP_TYPE_TEST:
                    rewritten = rewrite_type_test(calls,function,
                                                  instruction);
                    break;
                case IR_OP_SEQUENCE_SHAPE_CHECK:
                    rewritten = rewrite_shape_check(calls,function,
                                                    instruction);
                    break;
                case IR_OP_SEQUENCE_LENGTH:
                    rewritten = rewrite_sequence_length(calls,function,
                                                        instruction);
                    break;
                case IR_OP_EXTRACT_INDEX:
                    rewritten = rewrite_extract_index(calls,function,
                                                      instruction);
                    break;
                case IR_OP_ALLOC:
                    rewritten = rewrite_alloc(calls,function, instruction);
                    break;
                case IR_OP_FIELD:
                    rewritten = rewrite_field(calls,function, instruction);
                    break;
                case IR_OP_CALL:
                    rewritten = rewrite_native_call(calls,function,
                                                    instruction);
                    break;
                case IR_OP_AGGREGATE:
                    rewritten = rewrite_array_literal(calls,function,
                                                      instruction);
                    break;
                case IR_OP_EQ:
                case IR_OP_NE:
                    rewritten = rewrite_reference_compare(calls,function,
                                                          instruction);
                    break;
                case IR_OP_INDEX:
                case IR_OP_SEQUENCE_GET:
                    rewritten = rewrite_sequence_get(calls,function,
                                                     instruction);
                    break;
                case IR_OP_SEQUENCE_SET:
                    rewritten = rewrite_sequence_set(calls,function,
                                                     instruction);
                    break;
                case IR_OP_ARRAY_NEW:
                    rewritten = rewrite_array_new(calls,function,
                                                  instruction);
                    break;
                case IR_OP_NOMINAL_CAST:
                    rewritten = rewrite_nominal_cast(calls,function,
                                                     instruction);
                    break;
                case IR_OP_VARIANT_TAG:
                    rewritten = rewrite_variant_tag(calls,function,
                                                    instruction);
                    break;
                case IR_OP_VARIANT_PAYLOAD:
                    rewritten = rewrite_variant_payload(calls,function,
                                                        instruction);
                    break;
                case IR_OP_VARIANT_MAKE:
                    rewritten = rewrite_variant_make(calls,function,
                                                     instruction);
                    break;
                default:
                    break;
                }
                if (!rewritten) return false;
            }
        }
    }
    return true;
}

static void
emit_callees(box_calls_t *calls, size_t first)
{
    core_list_t(n00bb_ir_func_ptr_t) *emitted =
        &calls->analysis->emitted_functions;
    for (size_t f = first; f < emitted->len; f++) {
        ir_func_t *function = n00bb_cached_body(emitted->data[f]);
        for (uint32_t b = 0; function && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *in = block->instrs[i];
                for (uint32_t k = 0; k < in->n_operands; k++) {
                    ir_value_t *value = in->operands[k].kind == IR_OPD_VALUE
                        ? in->operands[k].value : nullptr;
                    if (value && value->kind == IR_VAL_FUNC && value->owner
                        && core_dict_add(&calls->emitted, value->owner, 1))
                        core_list_push(*emitted, value->owner);
                }
            }
        }
    }
}

bool
n00bb_rewrite_box_operations(n00bb_link_analysis_t *analysis)
{
    core_string_t runtime = CORE_STRING_STATIC("sys.internal.box");
    core_string_t store = CORE_STRING_STATIC("sys.internal.gc.box_store");
    box_calls_t calls = {
        .analysis = analysis,
        .runtime = n00bb_linked_unit_module(analysis, runtime),
        .store = n00bb_linked_unit_module(analysis, store),
        .runtime_body = analysis->cache
            ? n00bb_cache_module(analysis->cache, runtime) : nullptr,
        .store_body = analysis->cache
            ? n00bb_cache_module(analysis->cache, store) : nullptr,
        .floating = n00bb_linked_unit_module(analysis,
            CORE_STRING_STATIC("builtin.floating")),
    };
    if (!calls.runtime && !calls.floating) return true;
    core_dict_init(&calls.emitted, nullptr, nullptr);
    core_dict_init(&calls.type_indices, nullptr, nullptr);
    for (size_t f = 0; f < analysis->emitted_functions.len; f++)
        core_dict_put(&calls.emitted, analysis->emitted_functions.data[f], 1);
    size_t reached = analysis->emitted_functions.len;
    bool ok = rewrite_emitted(&calls);
    if (ok) emit_callees(&calls, reached);
    core_dict_free(&calls.emitted);
    core_dict_free(&calls.type_indices);
    return ok;
}
