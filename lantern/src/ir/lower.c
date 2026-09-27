#include <stdio.h>
#include <string.h>

#include "ir/lower.h"
#include "private/ir/internal.h"

typedef struct {
    const char *name;
    ir_lower_action_fn callback;
    void *state;
} ir_lower_action_entry_t;

struct ir_lowerer_t {
    ir_ctx_t *ctx;
    ir_lower_action_entry_t *actions;
    size_t action_count;
    size_t action_capacity;
};

ir_lowerer_t *
ir_lowerer_new(ir_ctx_t *ctx)
{
    if (!ctx) return nullptr;
    ir_lowerer_t *lowerer = ir_alloc(ctx, sizeof(*lowerer));
    lowerer->ctx = ctx;
    return lowerer;
}

bool
ir_lowerer_register(ir_lowerer_t *lowerer, const char *action,
                    ir_lower_action_fn callback, void *state)
{
    if (!lowerer || !action || !*action || !callback) return false;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        if (!strcmp(lowerer->actions[i].name, action)) return false;
    }
    size_t cap = lowerer->action_capacity;
    lowerer->actions = ir_grow(lowerer->ctx, lowerer->actions, &cap,
                               lowerer->action_count + 1,
                               sizeof(*lowerer->actions));
    if (!lowerer->actions) return false;
    lowerer->action_capacity = cap;
    lowerer->actions[lowerer->action_count++] = (ir_lower_action_entry_t){
        .name = ir_strdup(lowerer->ctx, action),
        .callback = callback,
        .state = state,
    };
    return true;
}

static ir_lower_result_t
lower_failure(ir_lower_context_t *context, ir_diagcode_t code,
              ir_span_t span, const char *message)
{
    if (context && context->diagnostics) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics, code,
                                              span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message);
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

ir_lower_result_t
ir_lower_emit(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              const parse_annotation_t *annotation, parse_visit_t *visit,
              ir_value_t *const *operands, size_t operand_count,
              th_type *result_type, ir_span_t span)
{
    if (!lowerer || !context || !annotation
        || annotation->kind != PARSE_ANNOT_EMIT
        || !annotation->emit_action.data || !context->module
        || !context->function || !context->block || !context->reachable)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (context->function->owner != context->module
        || context->block->owner != context->function)
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "lowering context objects do not belong together");
    if (core_list_len(annotation->emit_operands) != operand_count)
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "resolved operand count does not match the emit annotation");

    const char *action = annotation->emit_action.data;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        ir_lower_action_entry_t *entry = &lowerer->actions[i];
        if (strcmp(entry->name, action)) continue;
        ir_lower_result_t result = entry->callback(
            context, visit, operands, operand_count, result_type, span,
            entry->state);
        if (result.status > IR_LOWER_FAILED
            || ((result.status == IR_LOWER_VALUE)
                != (result.value != nullptr))
            || (result.value && result.value->owner != context->function))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "lowering callback returned an inconsistent result");
        if (result.status == IR_LOWER_TERMINATED) context->reachable = false;
        return result;
    }

    ir_op_t op;
    if (!ir_op_lookup(action, &op))
        return lower_failure(context, IR_DIAG_UNKNOWN_OPCODE, span,
                             "no lowering callback or IR operation matches this action");
    if (operand_count > UINT32_MAX
        || !ir_op_accepts_operands(op, (uint32_t)operand_count))
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "lowered operation has an invalid operand count");
    for (size_t i = 0; i < operand_count; i++) {
        if (!operands || !operands[i]
            || operands[i]->owner != context->function
            || ir_op_operand_kind(op, (uint32_t)i) != IR_OPD_VALUE)
            return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                                 "declarative lowering requires value operands");
    }
    bool has_result = ir_op_has_result(op);
    if (has_result != (result_type != nullptr))
        return lower_failure(context, IR_DIAG_VERIFY_TYPED, span,
                             "lowered operation result type does not match its IR signature");

    ir_instr_t *instruction = ir_instr_add(context->block, op);
    if (!instruction)
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "could not append lowered IR operation");
    instruction->span = span;
    for (size_t i = 0; i < operand_count; i++) {
        if (!ir_instr_operand_value(instruction, operands[i]))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not attach a lowered IR operand");
    }

    ir_lower_result_t result = {.status = IR_LOWER_NO_VALUE};
    if (has_result) {
        char name[48];
        do {
            snprintf(name, sizeof(name), "__lowered_%llu",
                     (unsigned long long)context->next_value_id++);
        } while (ir_value_find(context->function, name));
        if (!ir_instr_set_dest(instruction, name, result_type))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not bind the lowered IR result");
        result.status = IR_LOWER_VALUE;
        result.value = instruction->dest;
    }
    if (ir_op_is_terminator(op)) {
        context->reachable = false;
        result.status = IR_LOWER_TERMINATED;
        result.value = nullptr;
    }
    return result;
}
