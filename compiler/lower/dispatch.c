#include "lower_internal.h"

ir_value_t *
n00bb_emit_iteration_call(ir_lower_context_t *context, core_string_t method,
                          ir_value_t *receiver, bool write_back,
                          th_type *return_type, parse_tree_t *source)
{
    ir_span_t span = lower_span_for_tree(context, source);
    n00bb_lower_context_t *lower = context->user;
    ir_value_t *result = receiver && return_type
        ? n00bb_dispatch(context, method, &receiver, 1, return_type, span)
        : nullptr;
    // A `modify` receiver is passed by the place it was read from.
    return result && (!write_back || ir_annot_set(lower->ir_context,
            &result->def->annots, (ir_annot_t){.key = N00BB_WRITE_BACK_KEY,
                                               .kind = IR_ANNOT_BOOL,
                                               .b = true}))
        ? result : nullptr;
}

bool
n00bb_emit_iteration_cleanup(ir_lower_context_t *context,
                             n00bb_loop_frame_t *frame,
                             parse_tree_t *source)
{
    if (!frame || !frame->cleanup_active || !frame->iteration_plan
        || !frame->iteration_cursor) return true;
    ir_span_t span = lower_span_for_tree(context, source);
    th_type *nil_type = th_name(ir_module_types(context->module), CORE_STRING_STATIC("nil"));
    ir_value_t *cursor = n00bb_emit_local_read(context,
                                               frame->iteration_cursor, span);
    return nil_type && cursor
        && n00bb_emit_iteration_call(context, CORE_STRING_STATIC("__cleanup"),
            cursor, false, nil_type, source) != nullptr;
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
    (void)grammar;
    // What is consumed is decided by how the argument was read: a whole
    // variable gives up its value, a part of one cannot be taken out of it,
    // and anything else is a temporary already owned by the call.
    ir_instr_t *read = value->kind == IR_VAL_INSTR ? value->def : nullptr;
    ir_value_t *address = read && read->op == IR_OP_LOAD
            && read->n_operands == 1
        ? ir_operand_value(&read->operands[0]) : nullptr;
    ir_instr_t *step = address && address->kind == IR_VAL_INSTR
        ? address->def : nullptr;
    ir_value_t *container = nullptr, *at = nullptr;
    bool element = (read && read->op == IR_OP_SEQUENCE_GET)
        || n00bb_accessed_element(value, &container, &at)
        || (step && step->op == IR_OP_INDEX);
    bool field = (read && read->op == IR_OP_EXTRACT)
        || (step && step->op == IR_OP_FIELD);
    if (element || field) {
        core_string_t text = source_text_of(unit, argument);
        lower_report(context, N00B_DIAG_CONSUME_PART,
            lower_span_for_tree(context, argument),
            element ? N00B_TEXT_CONSUME_ELEMENT : N00B_TEXT_CONSUME_FIELD,
            N00BB_ARGS(ARG_TEXT(text)));
        lower_help(context, N00B_TEXT_CONSUME_THE_WHOLE_VALUE_OR,
            N00BB_NO_ARGS);
        return nullptr;
    }
    ir_local_t *local = read && read->op == IR_OP_COPY && read->n_operands == 1
        ? ir_operand_local(&read->operands[0]) : nullptr;
    ir_value_t *copied = read && read->op == IR_OP_COPY && !local
            && read->n_operands == 1
        ? ir_operand_value(&read->operands[0]) : value;
    ir_value_t *binding = local ? ir_local_value(context->function, local)
        : step && step->op == IR_OP_ALLOC ? address
        : copied && (copied->kind == IR_VAL_PARAM
                     || copied->kind == IR_VAL_GLOBAL) ? copied
        : nullptr;
    parse_sym_entry_t *symbol = binding
        ? ir_lower_binding_symbol(context, local, local ? nullptr : binding)
        : nullptr;
    if (symbol && symbol->kind != PARSE_SYM_VARIABLE
        && symbol->kind != PARSE_SYM_PARAM) {
        symbol = nullptr;
        binding = nullptr;
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
    return n00bb_emit_consume_value(context, value, binding, span);
}
