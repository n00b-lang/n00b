#include "lower_internal.h"

// `with` statements.
//
// `with [name =] manager { body }` uses the manager's `Context_Manager`
// methods:
// `__enter` runs before the body, and what it returns is bound to `name`;
// `__exit` runs on every way out of the body, given that value and, when
// the body failed, the failure as its `failure` keyword argument. Both are
// dispatched like any method call, and either may fail. A manager is
// entered after the managers to its left, and exited before them.
//
// Each manager is an exit scope (see n00bb_exit_scope_t): its exit block is
// the only place its `__exit` is called. While the body is being lowered,
// the manager is a failure handler, like a `try`: a body failure is given
// to `__exit`. When `__exit` returns `true` the failure is dropped, and the
// program continues after the manager; otherwise the failure goes on to the
// handler outside it. A failure of `__enter` or `__exit` itself goes to the
// handlers outside that manager.

// `failure` is passed to `__exit` as the option its parameter is.
static ir_value_t *
with_failure_argument(ir_lower_context_t *context, ir_value_t *failure,
                      ir_span_t span)
{
    th_type *given = th_resolve(failure->type);
    if (th_is_nil(given) || th_option_item(given)) return failure;
    ir_instr_t *some = ir_lower_emit_instruction(context, IR_OP_OPTION_SOME,
                                                 span);
    return some && ir_lower_set_temp_dest(context, some,
            CORE_STRING_STATIC("__n00bb_with_failure_"),
            th_option(ir_module_types(context->module), failure->type))
        && ir_instr_operand_value(some, failure)
        ? some->dest : nullptr;
}

// Calls the manager's `method`, a `Context_Manager` method, with `argument`
// after the manager when there is one and `failure` as the `failure`
// keyword argument when there is one. The call may fail: a failure goes to
// the handlers entered before `before_order`. `*value` is what it returns.
// `value_form` marks the `__exit` of a `with` used as a value.
static bool
with_call(ir_lower_context_t *context, ir_local_t *manager,
          core_string_t method, ir_value_t *argument, ir_value_t *failure,
          parse_tree_t *source, uint64_t before_order, n00bb_text_t phrase,
          bool value_form, ir_value_t **value)
{
    n00bb_lower_context_t *lower = context->user;
    th_ctx *types = ir_module_types(context->module);
    ir_span_t span = lower_span_for_tree(context, source);
    th_type *returned = th_var(types);
    th_type *error = th_var(types);
    th_type *outcome = ir_call_outcome_type(context->module,
        th_func_with_error(types, returned, error, nullptr, 0, nullptr,
                           nullptr, 0));
    ir_value_t *receiver = n00bb_emit_local_read(context, manager, span);
    ir_value_t *operands[] = {receiver, argument};
    ir_value_t *named = failure
        ? with_failure_argument(context, failure, span) : nullptr;
    ir_value_t *call = receiver && outcome && (!failure || named)
        ? n00bb_dispatch(context, method, operands, argument ? 2 : 1,
                         outcome, span)
        : nullptr;
    // A `modify` receiver is passed by the local holding the manager.
    bool ok = call && ir_annot_set(lower->ir_context, &call->def->annots,
            (ir_annot_t){.key = N00BB_WRITE_BACK_KEY, .kind = IR_ANNOT_BOOL,
                         .b = true})
        && ir_annot_set(lower->ir_context, &call->def->annots,
            (ir_annot_t){.key = N00BB_MAY_FAIL_KEY, .kind = IR_ANNOT_BOOL,
                         .b = true})
        && (!value_form || ir_annot_set(lower->ir_context, &call->def->annots,
            (ir_annot_t){.key = N00BB_WITH_VALUE_EXIT_KEY,
                         .kind = IR_ANNOT_BOOL, .b = true}))
        && (!named || ir_instr_operand_named(call->def,
                          CORE_STRING_STATIC("failure"), named));
    ir_block_t *ok_block = ok ? n00bb_branch_block(context, lower, "with_ok")
                              : nullptr;
    ir_block_t *error_block = ok_block
        ? n00bb_branch_block(context, lower, "with_error") : nullptr;
    // The test is the compiler's: with a hook that cannot fail it always
    // holds, which is not something to warn about.
    ir_span_t test_span = span;
    test_span.implied = true;
    if (!error_block
        || !n00bb_branch_on_outcome(context, call, ok_block, error_block,
                                    test_span)) {
        lower_internal(context, span, "could not call a `with` hook");
        return false;
    }
    context->block = error_block;
    ir_value_t *raised = n00bb_emit_outcome_payload(context, call,
        CORE_STRING_STATIC("error"), error, span);
    if (!raised
        || !n00bb_propagate_failure(context, source, raised, span,
                                    before_order, phrase))
        return false;
    context->block = ok_block;
    context->reachable = true;
    *value = n00bb_emit_outcome_payload(context, call,
        CORE_STRING_STATIC("ok"), returned, span);
    if (!*value) {
        lower_internal(context, span, "could not read a `with` hook result");
        return false;
    }
    return true;
}

ir_value_t *
n00bb_pack_rest_values(ir_lower_context_t *context, th_type *element,
                       const core_list_t(n00bb_ir_value_ptr_t) *values,
                       ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *array_type = th_array(types, th_is_closed(types, element)
        ? element : th_var(types));
    char buffer[48];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_rest_%llu", (unsigned long long)context->next_value_id++);
    ir_instr_t *array = array_type
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    bool packed = array && ir_instr_set_dest(array, name, array_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; packed && i < values->len; i++)
        packed = ir_instr_operand_value(array, values->data[i]);
    if (packed) return array->dest;
    lower_internal(context, span,
                   "could not pack variadic arguments into an array");
    return nullptr;
}

// Lowers one manager once its expression has been: holds it, runs its
// `__enter`, binds the result, and starts handling the body's failures.
bool
n00bb_lower_with_manager(ir_lower_context_t *context, parse_visit_t *visit,
                         bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (entering || !lower || !context->reachable || !lower->withs.len
        || !nt_is(visit->grammar, visit->node,
                  CORE_STRING_STATIC("context-manager")))
        return true;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (!frame->active) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                        CORE_STRING_STATIC("expression"));
    parse_tree_t *name = expression
        && core_tree_num_children(visit->node) >= 3
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *value = expression
        ? ir_lower_node_value(context, expression) : nullptr;
    if (!value) {
        lower_internal(context, span, "a `with` manager has no value");
        return false;
    }
    parse_sym_entry_t *binding = nullptr;
    if (name) {
        n00bb_tu_state_t *tu = tu_state(lower->unit);
        binding = tu && tu->annotations && tu->annotations->symtab
            ? parse_symtab_resolve_node(tu->annotations->symtab,
                                        visit->node, CORE_STRING_STATIC(""))
            : nullptr;
        if (!binding) {
            lower_internal(context, span, "a `with` name has no symbol");
            return false;
        }
    }

    char buffer[48];
    ir_local_t *manager = ir_local_add(context->function,
        n00bb_format_name(buffer, sizeof(buffer), "__n00bb_with_manager_%llu",
                          (unsigned long long)context->next_value_id++),
        value->type);
    if (!manager || !n00bb_emit_local_write(context, manager, value, span)) {
        lower_internal(context, span, "could not hold a `with` manager");
        return false;
    }
    manager->synthetic = true;

    ir_value_t *entered = nullptr;
    if (!with_call(context, manager, CORE_STRING_STATIC("__enter"), nullptr,
                   nullptr, expression, UINT64_MAX,
                   N00B_TEXT_PHRASE_ENTERING_THIS_MANAGER, false, &entered))
        return false;
    if (binding) {
        ir_local_t *local = ir_local_add(context->function,
            n00bb_format_name(buffer, sizeof(buffer), "__n00bb_with_value_%llu",
                              (unsigned long long)context->next_value_id++),
            binding->type ? binding->type : entered->type);
        if (!local || !ir_lower_bind_local(context, binding, local)) {
            lower_internal(context, span, "could not bind a `with` name");
            return false;
        }
        if (ir_lower_assign_symbol(context, binding, entered, true,
                                   span).status == IR_LOWER_FAILED)
            return false;
    }

    // The hooks' calls are the manager expression's, without its name.
    n00bb_with_item_t item = {
        .node = expression,
        .manager = manager,
        .value = entered,
    };
    if (!n00bb_exit_scope_open(context, &item.scope,
                               ++lower->next_control_order, true, span))
        return false;
    core_list_push(frame->items, item);
    return true;
}

// Leaves the managers innermost first: each one's exit block calls
// `__exit`, then goes on by the reason it was reached for. A `with` used
// as a value keeps its body's value across the exit blocks, and is that
// value after them. Its managers can't drop a failure, which would leave
// it no value: typing reports one whose `__exit` returns a `bool`.
static bool
with_leave(ir_lower_context_t *context, parse_grammar_t *grammar,
           n00bb_with_frame_t *frame, parse_tree_t *source)
{
    ir_span_t span = lower_span_for_tree(context, source);
    ir_local_t *kept = nullptr;
    th_type *kept_type = nullptr;
    if (frame->expression && context->reachable) {
        parse_tree_t *expression = value_body_expression(grammar,
                                                         frame->body);
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        if (!value) {
            lower_internal(context, span, "a `with` value has no value");
            return false;
        }
        kept_type = value->type;
        kept = n00bb_keep_across_exits(context, value, span);
        if (!kept) return false;
    }
    for (size_t i = frame->items.len; i > 0; i--) {
        n00bb_with_item_t *item = &frame->items.data[i - 1];
        bool reached = false;
        ir_value_t *failure = nullptr;
        if (!n00bb_exit_scope_close(context, &item->scope, span, &reached,
                                    &failure))
            return false;
        if (!reached) continue;
        ir_value_t *verdict = nullptr;
        if (!with_call(context, item->manager, CORE_STRING_STATIC("__exit"),
                       item->value, failure, item->node, item->scope.order,
                       N00B_TEXT_PHRASE_LEAVING_THIS_MANAGER,
                       frame->expression, &verdict)
            || !n00bb_exit_scope_finish(context, &item->scope, nullptr,
                   item->scope.failure_reachable && !frame->expression
                       ? verdict : nullptr,
                   source, N00B_TEXT_PHRASE_THIS_WITH_BLOCK))
            return false;
    }
    if (!frame->expression || !context->reachable) return true;
    ir_value_t *value = kept
        ? n00bb_kept_value(context, kept, kept_type, span) : nullptr;
    return value && ir_lower_set_node_value(context, frame->node, value);
}

void
n00bb_free_with_frame(n00bb_with_frame_t *frame)
{
    for (size_t i = 0; i < frame->items.len; i++)
        n00bb_exit_scope_free(&frame->items.data[i].scope);
    core_list_free(frame->items);
}

bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    bool expression = nt_is(visit->grammar, visit->node,
                            CORE_STRING_STATIC("with-value-expr"));
    if (!lower || (!expression && !nt_is(visit->grammar, visit->node,
                                         CORE_STRING_STATIC("with-stmt"))))
        return true;
    if (entering) {
        n00bb_with_frame_t frame = {
            .node = visit->node,
            .body = child_nt(visit->grammar, visit->node, expression
                ? CORE_STRING_STATIC("value-body")
                : CORE_STRING_STATIC("body")),
            .items = core_list_new(n00bb_with_item_t),
            .active = context->reachable,
            .expression = expression,
        };
        core_list_push(lower->withs, frame);
        return true;
    }
    if (!lower->withs.len
        || lower->withs.data[lower->withs.len - 1].node != visit->node)
        return false;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    bool ok = !frame->active
        || with_leave(context, visit->grammar, frame, visit->node);
    n00bb_free_with_frame(frame);
    lower->withs.len--;
    return ok;
}
