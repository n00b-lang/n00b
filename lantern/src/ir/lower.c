#include <stdio.h>
#include <string.h>

#include "ir/lower.h"
#include "private/ir/internal.h"
#include "lib/dict.h"
#include "parse/tree_util.h"

typedef struct {
    const char *name;
    ir_lower_action_fn callback;
    void *state;
} ir_lower_action_entry_t;

typedef struct {
    parse_annot_kind_t kind;
    ir_lower_visit_fn callback;
    void *state;
} ir_lower_visit_entry_t;

struct ir_lowerer_t {
    ir_ctx_t *ctx;
    ir_lower_action_entry_t *actions;
    size_t action_count;
    size_t action_capacity;
    ir_lower_visit_entry_t *visitors;
    size_t visitor_count;
    size_t visitor_capacity;
};

typedef struct {
    ir_lowerer_t *lowerer;
    ir_lower_context_t *context;
    core_dict_t values;
    bool failed;
    bool terminated;
} ir_lower_tree_state_t;

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

bool
ir_lowerer_register_visit(ir_lowerer_t *lowerer, parse_annot_kind_t kind,
                          ir_lower_visit_fn callback, void *state)
{
    if (!lowerer || kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT || !callback)
        return false;
    size_t cap = lowerer->visitor_capacity;
    lowerer->visitors = ir_grow(lowerer->ctx, lowerer->visitors, &cap,
                                lowerer->visitor_count + 1,
                                sizeof(*lowerer->visitors));
    if (!lowerer->visitors) return false;
    lowerer->visitor_capacity = cap;
    lowerer->visitors[lowerer->visitor_count++] = (ir_lower_visit_entry_t){
        .kind = kind,
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

static bool
lower_tree_hooks(ir_lower_tree_state_t *state, parse_visit_t *visit,
                 bool entering)
{
    size_t nt_count = visit->nt ? parse_nt_annot_count(visit->nt) : 0;
    size_t rule_count = visit->rule ? parse_rule_annot_count(visit->rule) : 0;
    size_t total = nt_count + rule_count;
    bool ok = true;
    for (size_t k = 0; k < total; k++) {
        size_t annotation_ix = entering ? k : total - 1 - k;
        parse_annotation_t *annotation = annotation_ix < nt_count
            ? parse_nt_annot_at(visit->nt, annotation_ix)
            : parse_rule_annot_at(visit->rule, annotation_ix - nt_count);
        for (size_t j = 0; j < state->lowerer->visitor_count; j++) {
            size_t visitor_ix = entering
                ? j : state->lowerer->visitor_count - 1 - j;
            ir_lower_visit_entry_t *entry =
                &state->lowerer->visitors[visitor_ix];
            if (entry->kind != annotation->kind) continue;
            visit->annot = annotation;
            if (!entry->callback(state->context, visit, entering,
                                 entry->state))
                ok = false;
        }
    }
    visit->annot = nullptr;
    if (!ok) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
    }
    return ok;
}

static bool
lower_tree_enter(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    return lower_tree_hooks(state, visit, true);
}

static parse_token_info_t *
last_terminal(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token = last_terminal(core_tree_child(node, i - 1));
        if (token) return token;
    }
    return nullptr;
}

static ir_span_t
span_of_tree(ir_lower_context_t *context, parse_tree_t *node)
{
    if (!context || context->file_id < 0) return IR_SPAN_UNKNOWN;
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_terminal(node);
    if (!first || !last || first->start_byte > UINT32_MAX
        || last->end_byte > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id, (uint32_t)first->start_byte,
                        (uint32_t)last->end_byte, first->line, first->column,
                        last->endline, last->endcol);
}

static bool
lower_tree_node(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    bool hooks_ok = lower_tree_hooks(state, visit, false);
    if (state->failed || !hooks_ok) return false;
    parse_annotation_t *annotation = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_EMIT)
        : nullptr;
    if (!annotation && visit->nt)
        annotation = parse_nt_find_annot(visit->grammar, visit->nt,
                                         PARSE_ANNOT_EMIT);

    if (!annotation || !state->context->reachable) {
        _core_dict_put(&state->values, visit->node, nullptr);
        if (!state->context->reachable) state->terminated = true;
        return true;
    }

    size_t count = core_list_len(annotation->emit_operands);
    ir_value_t **operands = count
        ? core_alloc_array(ir_value_t *, count) : nullptr;
    for (size_t i = 0; i < count; i++) {
        parse_tree_t *child = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, annotation->emit_operands.data[i]);
        bool found = false;
        operands[i] = child
            ? core_dict_get(&state->values, child, &found) : nullptr;
        if (!found || !operands[i]) {
            core_free(operands);
            lower_failure(state->context, IR_DIAG_VERIFY_PARSED,
                          span_of_tree(state->context, visit->node),
                          "emit annotation refers to a child with no lowered value");
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
    }

    ir_lower_result_t result = ir_lower_emit(
        state->lowerer, state->context, annotation, visit, operands, count,
        visit->pn ? visit->pn->type : nullptr,
        span_of_tree(state->context, visit->node));
    core_free(operands);
    if (result.status == IR_LOWER_FAILED) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
        return false;
    }
    state->terminated |= result.status == IR_LOWER_TERMINATED;
    core_dict_put(&state->values, visit->node, result.value);
    return true;
}

ir_lower_result_t
ir_lower_tree(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              parse_grammar_t *grammar, parse_tree_t *root)
{
    if (!lowerer || !context || !grammar || !root)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    ir_lower_tree_state_t state = {
        .lowerer = lowerer,
        .context = context,
    };
    core_dict_init(&state.values, nullptr, nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar, &state);
    parse_walk_on_node(walk, lower_tree_enter, lower_tree_node, nullptr);
    parse_tree_walk_run(walk, root);
    bool found = false;
    ir_value_t *value = core_dict_get(&state.values, root, &found);
    parse_tree_walk_free(walk);
    core_dict_free(&state.values);
    if (state.failed || !found)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (state.terminated || !context->reachable)
        return (ir_lower_result_t){.status = IR_LOWER_TERMINATED};
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
                 : (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}
