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
// Each manager has one exit block, the only place its `__exit` is called.
// Every way out of the body (running off its end, a failure, `return`,
// `break` or `continue`) sets the manager's reason local and jumps there;
// the exit block calls `__exit` and goes on by the reason. While the body
// is being lowered, each manager is a failure handler, like a `try`: a body
// failure is kept in the manager's error local and given to `__exit`. When
// `__exit` returns `true` the failure is dropped, and the program continues
// after the manager; otherwise the failure goes on to the handler outside
// it. A failure of `__enter` or `__exit` itself goes to the handlers outside
// that manager.

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

static ir_local_t *
with_local(ir_lower_context_t *context, const char *role, th_type *type)
{
    char buffer[48];
    ir_local_t *local = ir_local_add(context->function,
        n00bb_format_name(buffer, sizeof(buffer), "__n00bb_with_%s_%llu",
                          role, (unsigned long long)context->next_value_id++),
        type);
    if (local) local->synthetic = true;
    return local;
}

static ir_value_t *
with_reason_value(ir_lower_context_t *context, uint64_t reason,
                  ir_span_t span)
{
    th_type *uint_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("uint"));
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    return constant && ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_with_reason_"), uint_type)
        && ir_instr_operand_uint(constant, reason)
        ? constant->dest : nullptr;
}

// Leaves `item`'s body for its exit block, saying why, with the failure
// when it is leaving because of one.
static bool
with_jump_to_exit(ir_lower_context_t *context, n00bb_with_item_t *item,
                  uint64_t reason, ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!item->exit)
        item->exit = n00bb_branch_block(context, lower, "with_exit");
    th_type *option = th_option(ir_module_types(context->module),
                                item->failure_type);
    ir_value_t *failure = n00bb_emit_wrapping(context,
        error ? IR_OP_OPTION_SOME : IR_OP_OPTION_NONE, error, option, span);
    ir_value_t *value = failure && item->exit
        ? with_reason_value(context, reason, span) : nullptr;
    if (!value || !n00bb_emit_local_write(context, item->failure, failure, span)
        || !n00bb_emit_local_write(context, item->reason, value, span)
        || !n00bb_emit_jump(context, item->exit, span)) {
        lower_internal(context, span, "could not leave a `with` block");
        return false;
    }
    context->reachable = false;
    return true;
}

// A `return`, `break` or `continue` leaves through `item`'s exit block and
// goes on from a block of its own, where lowering continues.
static bool
with_leave_through(ir_lower_context_t *context, n00bb_with_item_t *item,
                   ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *resume = n00bb_branch_block(context, lower, "with_resume");
    if (!resume
        || !with_jump_to_exit(context, item,
               N00BB_WITH_RESUMES + item->resumes.len, nullptr, span))
        return false;
    core_list_push(item->resumes, resume);
    context->block = resume;
    context->reachable = true;
    return true;
}

bool
n00bb_route_with_failure(ir_lower_context_t *context, n00bb_with_item_t *item,
                         ir_value_t *error, ir_span_t span)
{
    if (!with_jump_to_exit(context, item, N00BB_WITH_FAILURE, error, span)) {
        lower_internal(context, span,
                       "could not send a failure to its `with` manager");
        return false;
    }
    item->failure_reachable = true;
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
        if (!with_leave_through(context, with_item,
                                lower_span_for_tree(context, exit_node)))
            return false;
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

    th_ctx *types = ir_module_types(context->module);
    th_type *failure_type = th_var(types);
    th_type *result_type = th_var(types);
    n00bb_with_item_t item = {
        .node = visit->node,
        .manager = manager,
        .value = entered,
        .failure_type = failure_type,
        .failure = with_local(context, "failure",
                              th_option(types, failure_type)),
        .result_type = result_type,
        .result = with_local(context, "result", th_option(types, result_type)),
        .reason = with_local(context, "reason",
            n00bb_ir_builtin_type(context->module,
                                  CORE_STRING_STATIC("uint"))),
        .order = ++lower->next_control_order,
        .resumes = core_list_new(n00bb_ir_block_ptr_t),
        .in_body = true,
    };
    ir_value_t *no_result = item.result
        ? n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr,
                              item.result->type, span) : nullptr;
    if (!item.failure || !item.reason || !no_result
        || !n00bb_emit_local_write(context, item.result, no_result, span)) {
        lower_internal(context, span, "could not enter a `with` manager");
        return false;
    }
    core_list_push(frame->items, item);
    return true;
}

// Goes on from `item`'s exit block to the block for its reason.
static bool
with_dispatch_reason(ir_lower_context_t *context, n00bb_with_item_t *item,
                     ir_block_t **targets, size_t count, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    size_t last = count;
    while (last > 0 && !targets[last - 1]) last--;
    for (size_t reason = 0; reason + 1 < last; reason++) {
        if (!targets[reason]) continue;
        ir_value_t *current = n00bb_emit_local_read(context, item->reason,
                                                    span);
        ir_value_t *wanted = current
            ? with_reason_value(context, reason, span) : nullptr;
        ir_value_t *hit = wanted
            ? n00bb_emit_binary_ir(context, IR_OP_EQ, bool_type, current,
                                   wanted, span) : nullptr;
        ir_block_t *next = n00bb_branch_block(context, lower, "with_reason");
        if (!hit || !next
            || !n00bb_emit_branch(context, hit, targets[reason], next, span))
            return false;
        context->block = next;
    }
    return last && n00bb_emit_jump(context, targets[last - 1], span);
}

// Leaves the managers innermost first. Every way out of a manager's body
// sets its reason local and jumps to its one exit block, which calls
// `__exit` and goes on by the reason: after the manager when the body ran
// off its end or `__exit` dropped a failure, out to the next handler when
// it didn't, or back to the `return`, `break` or `continue` that left.
static bool
with_leave(ir_lower_context_t *context, n00bb_with_frame_t *frame,
           parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, source);
    for (size_t i = frame->items.len; i > 0; i--) {
        n00bb_with_item_t *item = &frame->items.data[i - 1];
        item->in_body = false;
        if (context->reachable) {
            item->fallthrough = true;
            if (!with_jump_to_exit(context, item, N00BB_WITH_FALLTHROUGH,
                                   nullptr, span))
                return false;
        }
        // With no failure to give it, `__exit` gets its default.
        if (!item->failure_reachable)
            (void)th_var_bind(item->failure_type,
                              th_nil(ir_module_types(context->module)));
        if (!item->carries_result)
            (void)th_var_bind(item->result_type,
                              th_nil(ir_module_types(context->module)));
        if (!item->exit) {
            context->reachable = false;
            continue;
        }
        context->block = item->exit;
        context->reachable = true;
        ir_value_t *failure = item->failure_reachable
            ? n00bb_emit_local_read(context, item->failure, span) : nullptr;
        ir_value_t *swallowed = nullptr;
        if ((item->failure_reachable && !failure)
            || !with_exit(context, item, item->node, failure, &swallowed))
            return false;
        if (!context->reachable) continue;
        ir_block_t *after = item->fallthrough || item->failure_reachable
            ? n00bb_branch_block(context, lower, "with_after") : nullptr;
        size_t count = N00BB_WITH_RESUMES + item->resumes.len;
        ir_block_t **targets = core_alloc_array(ir_block_t *, count);
        targets[N00BB_WITH_FALLTHROUGH] = item->fallthrough ? after : nullptr;
        ir_block_t *failed = item->failure_reachable
            ? n00bb_branch_block(context, lower, "with_failed") : nullptr;
        targets[N00BB_WITH_FAILURE] = failed;
        for (size_t r = 0; r < item->resumes.len; r++)
            targets[N00BB_WITH_RESUMES + r] = item->resumes.data[r];
        bool ok = with_dispatch_reason(context, item, targets, count, span);
        core_free(targets);
        if (!ok) {
            lower_internal(context, span, "could not leave a `with` block");
            return false;
        }
        if (failed) {
            ir_block_t *rethrow = n00bb_branch_block(context, lower,
                                                     "with_rethrow");
            context->block = failed;
            context->reachable = true;
            if (!rethrow
                || !n00bb_emit_branch(context, swallowed, after, rethrow,
                                      span))
                return false;
            context->block = rethrow;
            ir_value_t *held = n00bb_emit_local_read(context, item->failure,
                                                     span);
            ir_instr_t *get = held
                ? ir_lower_emit_instruction(context, IR_OP_OPTION_GET, span)
                : nullptr;
            ir_value_t *again = get && ir_lower_set_temp_dest(context, get,
                    CORE_STRING_STATIC("__n00bb_with_error_"),
                    item->failure_type)
                && ir_instr_operand_value(get, held) ? get->dest : nullptr;
            if (!again
                || !with_propagate(context, source, again, span, item->order,
                                   N00B_TEXT_PHRASE_THIS_WITH_BLOCK))
                return false;
        }
        if (after) {
            context->block = after;
            context->reachable = true;
        }
        else context->reachable = false;
    }
    return true;
}

void
n00bb_free_with_frame(n00bb_with_frame_t *frame)
{
    for (size_t i = 0; i < frame->items.len; i++)
        core_list_free(frame->items.data[i].resumes);
    core_list_free(frame->items);
}

// A `return` or a function's value body gives a value computed in the body
// of a `with`, but returns it after leaving through the managers' exit
// blocks, so the value is kept across them in the outermost manager's
// result.
bool
n00bb_emit_with_success_carrying(ir_lower_context_t *context,
                                 parse_tree_t *exit_node, ir_value_t **value)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_with_item_t *outermost = nullptr;
    for (size_t i = 0; i < lower->withs.len; i++) {
        n00bb_with_frame_t *frame = &lower->withs.data[i];
        for (size_t j = 0; frame->active && j < frame->items.len; j++) {
            n00bb_with_item_t *item = &frame->items.data[j];
            if (item->in_body && (!outermost || item->order < outermost->order))
                outermost = item;
        }
    }
    if (!outermost || !*value)
        return n00bb_emit_with_success(context, exit_node);
    ir_span_t span = lower_span_for_tree(context, exit_node);
    ir_value_t *kept = n00bb_emit_wrapping(context, IR_OP_OPTION_SOME, *value,
                                           outermost->result->type, span);
    if (!kept || !n00bb_emit_local_write(context, outermost->result, kept,
                                         span))
        return false;
    outermost->carries_result = true;
    th_type *result_type = outermost->result_type;
    ir_local_t *result = outermost->result;
    // Leaving may move the frames `outermost` points into.
    if (!n00bb_emit_with_success(context, exit_node)) return false;
    if (!context->reachable) return true;
    ir_value_t *held = n00bb_emit_local_read(context, result, span);
    ir_instr_t *get = held
        ? ir_lower_emit_instruction(context, IR_OP_OPTION_GET, span)
        : nullptr;
    *value = get && ir_lower_set_temp_dest(context, get,
            CORE_STRING_STATIC("__n00bb_with_result_"), result_type)
        && ir_instr_operand_value(get, held) ? get->dest : nullptr;
    return *value != nullptr;
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
    n00bb_free_with_frame(frame);
    lower->withs.len--;
    return ok;
}
