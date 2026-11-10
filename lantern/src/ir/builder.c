#include "ir/builder.h"
#include "lib/buffer.h"

#define lower_failure(context, code, span, literal) \
    ir_lower_fail((context), (code), (span), CORE_STRING_STATIC(literal))

ir_lower_result_t
ir_lower_fail(ir_lower_context_t *context, ir_diagcode_t code,
                   ir_span_t span, core_string_t message)
{
    if (context && context->diagnostics) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics, code,
                                              span, IR_NO_LABEL);
        ir_diag_message(diagnostic, context->diagnostics, IR_DETAIL(message));
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static ir_instr_t *
lower_append_instruction(ir_lower_context_t *context, ir_op_t op,
                        ir_span_t span, bool terminator)
{
    if (!context || !context->module || !context->function || !context->block
        || context->function->owner != context->module
        || context->block->owner != context->function) {
        lower_failure(context, IR_DIAG_INTERNAL, span,
                      "lowering context objects do not belong together");
        return nullptr;
    }
    if (!context->reachable || ir_block_terminator(context->block)) {
        lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                      "cannot emit an instruction in an unreachable or terminated block");
        return nullptr;
    }
    if (ir_op_is_terminator(op) != terminator) {
        ir_lower_fail(context, IR_DIAG_VERIFY_PARSED, span,
            terminator
                ? CORE_STRING_STATIC(
                      "terminator helper requires a terminator operation")
                : CORE_STRING_STATIC(
                      "instruction helper cannot emit a terminator operation"));
        return nullptr;
    }
    ir_instr_t *instruction = ir_instr_add(context->block, op);
    if (!instruction) {
        lower_failure(context, IR_DIAG_INTERNAL, span,
                      "could not append an IR instruction");
        return nullptr;
    }
    instruction->span = span;
    if (terminator) context->reachable = false;
    return instruction;
}

ir_instr_t *
ir_lower_emit_instruction(ir_lower_context_t *context, ir_op_t op,
                          ir_span_t span)
{
    return lower_append_instruction(context, op, span, false);
}

ir_instr_t *
ir_lower_emit_terminator(ir_lower_context_t *context, ir_op_t op,
                         ir_span_t span)
{
    return lower_append_instruction(context, op, span, true);
}

bool
ir_lower_set_temp_dest(ir_lower_context_t *context, ir_instr_t *instruction,
                       core_string_t prefix, th_type *type)
{
    if (!context || !context->function || !instruction || !prefix.data
        || !type || instruction->block->owner != context->function)
        return false;
    for (;;) {
        core_buffer_t name = {0};
        core_buffer_append_string(&name, prefix);
        core_buffer_printf(&name, "%llu",
            (unsigned long long)context->next_value_id++);
        core_string_t text = core_buffer_string(&name);
        bool exists = name.data
            && ir_value_find(context->function, text);
        bool bound = name.data && !exists
            && ir_instr_set_dest(instruction, text, type);
        core_buffer_release(&name);
        if (!exists) return bound;
    }
}

ir_value_t *
ir_lower_dispatch(ir_lower_context_t *context, core_string_t method,
                  ir_value_t *const *operands, size_t count,
                  th_type *result_type, ir_span_t span)
{
    if (!context || !method.data || !count || !result_type) return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
        IR_OP_DISPATCH, span);
    if (!instruction
        || !ir_lower_set_temp_dest(context, instruction,
               CORE_STRING_STATIC("__dispatch_"), result_type)
        || !ir_instr_operand_name(instruction, method))
        return nullptr;
    for (size_t i = 0; i < count; i++)
        if (!operands[i] || !ir_instr_operand_value(instruction, operands[i]))
            return nullptr;
    return instruction->dest;
}
