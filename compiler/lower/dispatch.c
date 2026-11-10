#include "lower_internal.h"

ir_value_t *
n00bb_emit_iteration_call(ir_lower_context_t *context, core_string_t method,
                          ir_value_t *receiver, th_mode receiver_mode,
                          th_type *return_type, ir_value_t **copyout_value,
                          parse_tree_t *source)
{
    ir_span_t span = lower_span_for_tree(context, source);
    ir_value_t *result = receiver && return_type
        ? n00bb_dispatch(context, method, &receiver, 1, return_type, span)
        : nullptr;
    if (!result || receiver_mode != TH_INOUT || !copyout_value)
        return result;
    ir_instr_t *output = ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT,
                                                   span);
    if (!output || !ir_lower_set_temp_dest(context, output,
            CORE_STRING_STATIC("__n00bb_updated_"), receiver->type)
        || !ir_instr_operand_value(output, result)
        || !ir_instr_operand_uint(output, 0))
        return nullptr;
    *copyout_value = output->dest;
    return result;
}

bool
n00bb_emit_iteration_cleanup(ir_lower_context_t *context,
                             n00bb_loop_frame_t *frame,
                             parse_tree_t *source)
{
    if (!frame || !frame->cleanup_active || !frame->iteration_plan
        || !frame->iteration_cursor_value) return true;
    th_type *nil_type = th_name(ir_module_types(context->module), CORE_STRING_STATIC("nil"));
    return nil_type
        && n00bb_emit_iteration_call(context, CORE_STRING_STATIC("__cleanup"),
            frame->iteration_cursor_value, TH_CONSUME, nil_type,
            nullptr, source) != nullptr;
}

ir_value_t *
n00bb_emit_consume_value(ir_lower_context_t *context, ir_value_t *value,
                         ir_value_t *binding, ir_span_t span)
{
    if (!context || !value) return nullptr;
    ir_instr_t *consume = ir_lower_emit_instruction(context, IR_OP_CONSUME,
                                                    span);
    if (!consume || !ir_lower_set_temp_dest(context, consume,
        CORE_STRING_STATIC("__n00bb_consumed_"), value->type)
        || !ir_instr_operand_value(consume, value)
        || (binding && binding != value
            && !ir_instr_operand_value(consume, binding))) {
        lower_internal(context, span,
            "could not emit the semantic ownership transfer");
        return nullptr;
    }
    return consume->dest;
}

ir_value_t *
n00bb_emit_consume(ir_lower_context_t *context, parse_grammar_t *grammar,
                   core_translation_unit_t *unit, parse_tree_t *argument,
                   ir_value_t *value)
{
    if (!context || !argument || !value) return nullptr;
    parse_tree_t *index = n00bb_index_expression_node(grammar, argument);
    parse_tree_t *member = index ? nullptr
        : member_access_node(grammar, argument);
    if (index || member) {
        core_string_t text = source_text_of(unit, argument);
        lower_report(context, N00B_DIAG_CONSUME_PART,
            lower_span_for_tree(context, argument),
            index ? N00B_TEXT_CONSUME_ELEMENT : N00B_TEXT_CONSUME_FIELD,
            N00BB_ARGS(ARG_TEXT(text)));
        lower_help(context, N00B_TEXT_CONSUME_THE_WHOLE_VALUE_OR,
            N00BB_NO_ARGS);
        return nullptr;
    }

    parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
    parse_sym_entry_t *symbol = lvalue
        ? attached_symbol(grammar, unit, lvalue) : nullptr;
    ir_value_t *binding = nullptr;
    if (symbol && (symbol->kind == PARSE_SYM_VARIABLE
                   || symbol->kind == PARSE_SYM_PARAM)) {
        binding = ir_lower_bound_address(context, symbol);
        if (!binding) {
            ir_local_t *local = ir_lower_bound_local(context, symbol);
            binding = local ? ir_local_value(context->function, local)
                            : ir_lower_bound_value(context, symbol);
        }
        if (!binding) {
            lower_internal(context,
                lower_span_for_tree(context, argument),
                "consumed binding has no lowered semantic identity");
            return nullptr;
        }
    }

    ir_span_t span = lower_span_for_tree(context, argument);
    th_mode mode = symbol && symbol->kind == PARSE_SYM_PARAM
        && symbol->has_mode ? symbol->mode : TH_IN;
    if (symbol && symbol->kind == PARSE_SYM_PARAM && mode != TH_CONSUME
        && mode != TH_OUT) {
        lower_report(context, N00B_DIAG_CONSUME_UNOWNED_PARAMETER, span,
            N00B_TEXT_CONSUME_NOT_OWNED,
            N00BB_ARGS(ARG_TEXT(symbol->name),
                       ARG_TEXT(n00bb_text_form(mode == TH_VIEW
                           ? N00B_TEXT_VIEW_PARAMETER
                           : mode == TH_INOUT ? N00B_TEXT_MODIFY_PARAMETER
                                              : N00B_TEXT_IN_PARAMETER))));
        n00bb_report_label(lower_diagnostics(context),
                           N00B_TEXT_THE_CALLER_STILL_OWNS_IT, N00BB_NO_ARGS);
        lower_declared_secondary(context, symbol, N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        lower_help(context, N00B_TEXT_PASS_A_COPY_OR_DECLARE,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        return nullptr;
    }
    ir_value_t *consumed = n00bb_emit_consume_value(context, value, binding,
                                                    span);
    n00bb_final_frame_t *frame = symbol
        ? n00bb_final_frame(context->user, symbol) : nullptr;
    if (consumed && frame && !n00bb_set_owned(context, frame, false, span))
        return nullptr;
    return consumed;
}
