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
// While the body is being lowered, each manager is a failure handler, like
// a `try`: a body failure stores its value in the manager's error local and
// jumps to the manager's `failed` block, which calls `__exit` with it. When
// `__exit` returns `true` the failure is dropped, and the program continues
// after the manager's exit (`after`); otherwise the failure goes on to the
// handler outside the manager. A failure of `__enter` or `__exit` itself
// goes to the handlers outside that manager.

static bool
with_cleanups_before(ir_lower_context_t *context, parse_tree_t *exit_node,
                     uint64_t before_order);

// Sends `error`, raised at a point where only the handlers entered before
// `before_order` are active, on to the innermost of them, or out of the
// function.
static bool
with_propagate(ir_lower_context_t *context, parse_tree_t *source,
               ir_value_t *error, ir_span_t span, uint64_t before_order,
               n00bb_text_t phrase)
{
    if (!with_cleanups_before(context, source, before_order)) return false;
    int routed = n00bb_route_failure_before(context, error, span,
                                            before_order);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error->type))
        return report_uncaught_failure(context, span, error->type, phrase);
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_internal(context, span, "could not pass on a `with` failure");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

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

// Calls the manager's `method`, a `Context_Manager` method, with `argument` after
// the manager when there is one and `failure` as the `failure` keyword
// argument when there is one. The call may fail: a failure goes to the
// handlers entered before `before_order`. `*value` is what it returns.
static bool
with_call(ir_lower_context_t *context, ir_local_t *manager,
          core_string_t method, ir_value_t *argument, ir_value_t *failure,
          parse_tree_t *source, uint64_t before_order, n00bb_text_t phrase,
          ir_value_t **value)
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
        || !with_propagate(context, source, raised, span, before_order,
                           phrase))
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

// Runs `item`'s `__exit` on the way out of its body, with `failure` when
// the body failed. `*swallowed` is what `__exit` returned: whether the
// failure is dropped.
static bool
with_exit(ir_lower_context_t *context, n00bb_with_item_t *item,
          parse_tree_t *source, ir_value_t *failure, ir_value_t **swallowed)
{
    ir_value_t *returned = nullptr;
    if (!with_call(context, item->manager, CORE_STRING_STATIC("__exit"),
                   item->value, failure, source, item->order,
                   N00B_TEXT_PHRASE_LEAVING_THIS_MANAGER, &returned))
        return false;
    if (swallowed) *swallowed = returned;
    return true;
}

static bool
with_cleanups_before(ir_lower_context_t *context, parse_tree_t *exit_node,
                     uint64_t before_order)
{
    n00bb_try_frame_t *try_frame = nullptr;
    n00bb_with_item_t *with_item = nullptr;
    uint64_t stop = n00bb_failure_handler(context->user, before_order,
                                          &try_frame, &with_item);
    return n00bb_emit_scoped_cleanups(context, exit_node, stop, false,
                                      before_order);
}

uint64_t
n00bb_failure_handler(n00bb_lower_context_t *lower, uint64_t before_order,
                      n00bb_try_frame_t **try_frame,
                      n00bb_with_item_t **with_item)
{
    *try_frame = nullptr;
    *with_item = nullptr;
    for (size_t i = lower->tries.len; i > 0 && !*try_frame; i--) {
        n00bb_try_frame_t *frame = &lower->tries.data[i - 1];
        if (frame->active && frame->in_body && frame->order < before_order)
            *try_frame = frame;
    }
    for (size_t i = 0; i < lower->withs.len; i++) {
        n00bb_with_frame_t *frame = &lower->withs.data[i];
        for (size_t j = 0; frame->active && j < frame->items.len; j++) {
            n00bb_with_item_t *item = &frame->items.data[j];
            if (item->in_body && item->order < before_order
                && (!*with_item || item->order > (*with_item)->order))
                *with_item = item;
        }
    }
    // Only the innermost of the two handles the failure.
    if (*try_frame && *with_item) {
        if ((*with_item)->order > (*try_frame)->order) *try_frame = nullptr;
        else *with_item = nullptr;
    }
    return *try_frame ? (*try_frame)->order
         : *with_item ? (*with_item)->order : 0;
}

bool
n00bb_route_with_failure(ir_lower_context_t *context, n00bb_with_item_t *item,
                         ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!item->failed)
        item->failed = n00bb_branch_block(context, lower, "with_failed");
    if (!item->error && item->failed) {
        char buffer[48];
        item->error = ir_local_add(context->function,
            n00bb_format_name(buffer, sizeof(buffer),
                "__n00bb_with_error_%llu",
                (unsigned long long)context->next_value_id++),
            error->type);
        if (item->error) item->error->synthetic = true;
    }
    if (!item->failed || !item->error
        || !n00bb_emit_local_write(context, item->error, error, span)
        || !n00bb_emit_jump(context, item->failed, span)) {
        lower_internal(context, span,
                       "could not send a failure to its `with` manager");
        return false;
    }
    item->failure_reachable = true;
    context->reachable = false;
    return true;
}

bool
n00bb_emit_scoped_cleanups(ir_lower_context_t *context,
                           parse_tree_t *exit_node, uint64_t stop_order,
                           bool success, uint64_t before_order)
{
    n00bb_lower_context_t *lower = context->user;
    uint64_t before = before_order;
    for (;;) {
        n00bb_loop_frame_t *loop = nullptr;
        n00bb_with_item_t *with_item = nullptr;
        for (size_t i = lower->loops.len; i > 0; i--) {
            n00bb_loop_frame_t *candidate = &lower->loops.data[i - 1];
            if (!candidate->active || !candidate->is_container
                || !candidate->cleanup_active
                || candidate->order <= stop_order
                || candidate->order >= before)
                continue;
            if (!loop || candidate->order > loop->order) loop = candidate;
        }
        // A failure leaves each manager to its handler; only a success
        // runs `__exit` here.
        for (size_t i = 0; success && i < lower->withs.len; i++) {
            n00bb_with_frame_t *frame = &lower->withs.data[i];
            for (size_t j = 0; frame->active && j < frame->items.len; j++) {
                n00bb_with_item_t *item = &frame->items.data[j];
                if (!item->in_body || item->order <= stop_order
                    || item->order >= before
                    || (with_item && item->order <= with_item->order))
                    continue;
                with_item = item;
            }
        }
        if (!loop && !with_item) break;
        if (loop && (!with_item || loop->order > with_item->order)) {
            before = loop->order;
            if (!n00bb_emit_iteration_cleanup(context, loop, exit_node))
                return false;
            continue;
        }
        before = with_item->order;
        if (!with_exit(context, with_item, exit_node, nullptr, nullptr))
            return false;
        if (!context->reachable) break;
    }
    return true;
}

bool
n00bb_emit_scoped_success(ir_lower_context_t *context,
                          parse_tree_t *exit_node,
                          n00bb_loop_frame_t *stop_before)
{
    return n00bb_emit_scoped_cleanups(context, exit_node,
                                      stop_before ? stop_before->order : 0,
                                      true, UINT64_MAX);
}

bool
n00bb_emit_with_success(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return n00bb_emit_scoped_success(context, exit_node, nullptr);
}

bool
n00bb_emit_with_failure(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return with_cleanups_before(context, exit_node, UINT64_MAX);
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
                   nullptr, visit->node, UINT64_MAX,
                   N00B_TEXT_PHRASE_ENTERING_THIS_MANAGER, &entered))
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

    n00bb_with_item_t item = {
        .node = visit->node,
        .manager = manager,
        .value = entered,
        .order = ++lower->next_control_order,
        .in_body = true,
    };
    core_list_push(frame->items, item);
    return true;
}

// Leaves the managers innermost first. Each one's `__exit` runs on the
// path that reached the end of the body, and again in its failure handler;
// a dropped failure continues where the fallthrough path does, after that
// manager's exit.
static bool
with_leave(ir_lower_context_t *context, n00bb_with_frame_t *frame,
           parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, source);
    for (size_t i = frame->items.len; i > 0; i--) {
        n00bb_with_item_t *item = &frame->items.data[i - 1];
        item->in_body = false;
        // Made only when a path continues past this manager.
        item->after = nullptr;
        if (context->reachable) {
            if (!with_exit(context, item, item->node, nullptr, nullptr))
                return false;
            if (context->reachable) {
                item->after = n00bb_branch_block(context, lower,
                                                 "with_after");
                if (!item->after
                    || !n00bb_emit_jump(context, item->after, span))
                    return false;
            }
        }
        if (item->failure_reachable) {
            context->block = item->failed;
            context->reachable = true;
            ir_value_t *failure = n00bb_emit_local_read(context, item->error,
                                                        span);
            ir_value_t *swallowed = nullptr;
            if (!failure
                || !with_exit(context, item, item->node, failure,
                              &swallowed))
                return false;
            if (context->reachable) {
                if (!item->after)
                    item->after = n00bb_branch_block(context, lower,
                                                     "with_after");
                ir_block_t *rethrow = n00bb_branch_block(context, lower,
                                                         "with_rethrow");
                if (!item->after || !rethrow
                    || !n00bb_emit_branch(context, swallowed, item->after,
                                          rethrow, span))
                    return false;
                context->block = rethrow;
                context->reachable = true;
                ir_value_t *again = n00bb_emit_local_read(context,
                                                          item->error, span);
                if (!again
                    || !with_propagate(context, source, again, span,
                                       item->order,
                                       N00B_TEXT_PHRASE_THIS_WITH_BLOCK))
                    return false;
            }
        }
        if (item->after) {
            context->block = item->after;
            context->reachable = true;
        }
        else context->reachable = false;
    }
    return true;
}

bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !nt_is(visit->grammar, visit->node,
                         CORE_STRING_STATIC("with-stmt")))
        return true;
    if (entering) {
        n00bb_with_frame_t frame = {
            .node = visit->node,
            .body = child_nt(visit->grammar, visit->node,
                             CORE_STRING_STATIC("body")),
            .items = core_list_new(n00bb_with_item_t),
            .active = context->reachable,
        };
        core_list_push(lower->withs, frame);
        return true;
    }
    if (!lower->withs.len
        || lower->withs.data[lower->withs.len - 1].node != visit->node)
        return false;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    bool ok = !frame->active || with_leave(context, frame, visit->node);
    core_list_free(frame->items);
    lower->withs.len--;
    return ok;
}
