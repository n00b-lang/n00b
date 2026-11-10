#include "lower_internal.h"

static ir_value_t *
n00bb_emit_retained_reference(ir_lower_context_t *context,
                              ir_value_t *reference, ir_span_t span);
static th_type *
n00bb_open_contract_arguments(th_ctx *types,
                              n00bb_callback_contract_t *contract,
                              th_type *type);

static ir_value_t *
n00bb_emit_retained_reference(ir_lower_context_t *context,
                              ir_value_t *reference, ir_span_t span)
{
    if (!context || !reference
        || (!n00bb_open_type(context, reference->type)
            && th_kind_of(th_resolve(reference->type)) != TH_INDIRECTION))
        return nullptr;
    ir_instr_t *retain = ir_lower_emit_instruction(context, IR_OP_RETAIN_REF,
                                                   span);
    if (!retain || !ir_lower_set_temp_dest(context, retain,
                                           CORE_STRING_STATIC("__n00bb_retained_ref_"),
                                           reference->type)
        || !ir_instr_operand_value(retain, reference))
        return nullptr;
    return retain->dest;
}

static th_type *
n00bb_open_contract_arguments(th_ctx *types,
                              n00bb_callback_contract_t *contract,
                              th_type *type)
{
    core_string_t name = {0};
    th_type *resolved = th_resolve(type);
    if (th_kind_of(resolved) != TH_NAMED || th_named_nargs(resolved)
        || !th_named_info(resolved, &name, nullptr, nullptr))
        return type;
    th_type **variables = nullptr;
    size_t arity = th_free_vars(types, contract->handler->type, &variables);
    if (!arity) return type;
    th_arg *arguments = core_alloc_array(th_arg, arity);
    for (size_t i = 0; i < arity; i++)
        arguments[i] = (th_arg){.kind = TH_ARG_TYPE, .type = th_var(types)};
    th_type *applied = th_apply(types, name, arguments, arity);
    core_free(arguments);
    return applied ? applied : type;
}

ir_lower_result_t
n00bb_lower_callback_create(ir_lower_context_t *context,
                            parse_visit_t *visit,
                            ir_value_t *const *operands,
                            size_t operand_count,
                            th_type *result_type, ir_span_t span,
                            void *opaque)
{
    (void)operands;
    (void)operand_count;
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_type *declared = parse_tree_type(child_nt(visit->grammar, visit->node,
        CORE_STRING_STATIC("type-spec")));
    if (declared) result_type = declared;
    if (n00bb_callback_rejected(state, result_type))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    n00bb_callback_contract_t *contract =
        n00bb_callback_contract_for_type(state, result_type);
    if (!contract || !contract->call || !contract->handler)
        return lower_internal(context, span,
            "callback construction has no resolved nominal contract");
    result_type = n00bb_open_contract_arguments(
        ir_module_types(context->module), contract, result_type);
    if (!n00bb_register_callback_representation(context->module, result_type))
        return lower_internal(context, span,
            "callback construction has no registered value representation");

    parse_tree_t *args = child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("call-args"));
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    core_list_t(n00bb_tree_ptr_t) keywords =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, args, CORE_STRING_STATIC("positional-args")), &positional);
    collect_keyword_nodes(visit->grammar,
        child_nt(visit->grammar, args, CORE_STRING_STATIC("keyword-args")), &keywords);
    size_t hidden = contract->stateful ? 2u : 1u;
    bool emitted = positional.len >= hidden;
    ir_value_t *handler = emitted
        ? ir_lower_node_value(context, positional.data[0]) : nullptr;
    handler = handler
        ? n00bb_emit_retained_reference(context, handler,
                                        lower_span_for_tree(context,
                                            positional.data[0]))
        : nullptr;
    ir_value_t *state_reference = nullptr;
    if (emitted && contract->stateful) {
        state_reference = ir_lower_node_value(context, positional.data[1]);
        state_reference = state_reference
            ? n00bb_emit_retained_reference(context, state_reference,
                lower_span_for_tree(context, positional.data[1])) : nullptr;
    }
    emitted = emitted && handler
        && (!contract->stateful || state_reference);

    th_type *setup_type = contract->setup ? contract->setup->type : nullptr;
    th_type *failure = setup_type ? th_func_error(setup_type) : nullptr;
    bool can_fail = failure && !th_is_nil(failure);
    th_type *construction_signature = nullptr;
    th_type *ir_result = result_type;
    if (can_fail) {
        construction_signature = th_func_with_error(state->types,
            result_type, failure, nullptr, 0, nullptr, nullptr, 0);
        ir_result = construction_signature
            ? ir_call_outcome_type(context->module,
                                   construction_signature) : nullptr;
    }
    th_type *call_contract = n00bb_specialize_callback_role(state->types,
        contract, result_type, contract->call->type);
    th_type *handler_contract = n00bb_specialize_callback_role(state->types,
        contract, result_type, contract->handler->type);
    th_type *setup_contract = setup_type
        ? n00bb_specialize_callback_role(state->types, contract,
                                          result_type, setup_type) : nullptr;
    core_list_t(n00bb_ir_value_ptr_t) arguments =
        core_list_new(n00bb_ir_value_ptr_t);
    for (size_t i = hidden; emitted && i < positional.len; i++) {
        ir_value_t *value = ir_lower_node_value(context, positional.data[i]);
        emitted = value != nullptr;
        if (emitted) core_list_push(arguments, value);
    }
    for (size_t i = 0; emitted && i < keywords.len; i++) {
        parse_tree_t *expression = child_nt(visit->grammar,
            keywords.data[i], CORE_STRING_STATIC("expression"));
        core_string_t keyword = call_keyword_name(keywords.data[i]);
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        emitted = keyword.data && value != nullptr;
        if (emitted) core_list_push(arguments, value);
    }
    ir_instr_t *instruction = emitted && ir_result
        ? ir_lower_emit_instruction(context, IR_OP_CALLBACK_CREATE, span)
        : nullptr;
    emitted = instruction && ir_lower_set_temp_dest(context, instruction,
        CORE_STRING_STATIC("__n00bb_callback_"), ir_result)
        && call_contract && handler_contract
        && (!setup_type || setup_contract)
        && ir_instr_set_callback_contract(instruction, call_contract,
            handler_contract, setup_contract);
    if (emitted && contract->stateful) {
        emitted = ir_annot_set(ir_module_ctx(context->module),
            &instruction->annots, (ir_annot_t){
                .key = N00BB_CALLBACK_SETUP_KEY,
                .kind = IR_ANNOT_BOOL, .b = true});
        n00bb_rewrite_later(context, instruction);
    }
    if (emitted) emitted = ir_instr_operand_value(instruction, handler);
    if (emitted && state_reference)
        emitted = ir_instr_operand_value(instruction, state_reference);
    size_t extra = positional.len > hidden ? positional.len - hidden : 0;
    for (size_t i = 0; emitted && i < arguments.len; i++)
        emitted = i < extra
            ? ir_instr_operand_value(instruction, arguments.data[i])
            : ir_instr_operand_named(instruction,
                  call_keyword_name(keywords.data[i - extra]),
                  arguments.data[i]);
    core_list_free(arguments);
    core_list_free(positional);
    core_list_free(keywords);
    if (!emitted)
        return lower_internal(context, span,
            "could not emit semantic callback construction");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}
