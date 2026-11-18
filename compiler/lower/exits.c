#include "lower_internal.h"
#include "ir/pass.h"

// Exit scopes: the one exit block of a `with` manager or a loop over a
// container. See n00bb_exit_scope_t.

static ir_local_t *
exit_local(ir_lower_context_t *context, const char *role, th_type *type)
{
    char buffer[48];
    ir_local_t *local = ir_local_add(context->function,
        n00bb_format_name(buffer, sizeof(buffer), "__n00bb_exit_%s_%llu",
                          role, (unsigned long long)context->next_value_id++),
        type);
    if (local) local->synthetic = true;
    return local;
}

static ir_value_t *
reason_value(ir_lower_context_t *context, uint64_t reason, ir_span_t span)
{
    th_type *uint_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("uint"));
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    return constant && ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_exit_reason_"), uint_type)
        && ir_instr_operand_uint(constant, reason)
        ? constant->dest : nullptr;
}

// The item of the option `local` holds.
static ir_value_t *
option_item(ir_lower_context_t *context, ir_local_t *local, th_type *item,
            ir_span_t span)
{
    ir_value_t *held = n00bb_emit_local_read(context, local, span);
    ir_instr_t *get = held
        ? ir_lower_emit_instruction(context, IR_OP_OPTION_GET, span)
        : nullptr;
    return get && ir_lower_set_temp_dest(context, get,
            CORE_STRING_STATIC("__n00bb_exit_item_"), item)
        && ir_instr_operand_value(get, held) ? get->dest : nullptr;
}

bool
n00bb_exit_scope_open(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                      uint64_t order, bool handles, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    *scope = (n00bb_exit_scope_t){
        .failure_type = th_var(types),
        .resumes = core_list_new(n00bb_ir_block_ptr_t),
        .order = order,
        .open = true,
        .handles = handles,
    };
    scope->failure = exit_local(context, "failure",
                                th_option(types, scope->failure_type));
    scope->reason = exit_local(context, "reason",
        n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint")));
    if (scope->failure && scope->reason) return true;
    lower_internal(context, span, "could not start a construct's exit");
    return false;
}

void
n00bb_exit_scope_free(n00bb_exit_scope_t *scope)
{
    core_list_free(scope->resumes);
}

bool
n00bb_exit_scope_jump(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                      uint64_t reason, ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!scope->exit)
        scope->exit = n00bb_branch_block(context, lower, "exit");
    th_type *option = th_option(ir_module_types(context->module),
                                scope->failure_type);
    ir_value_t *failure = scope->exit ? n00bb_emit_wrapping(context,
        error ? IR_OP_OPTION_SOME : IR_OP_OPTION_NONE, error, option, span)
        : nullptr;
    ir_value_t *value = failure ? reason_value(context, reason, span)
                                : nullptr;
    if (!value
        || !n00bb_emit_local_write(context, scope->failure, failure, span)
        || !n00bb_emit_local_write(context, scope->reason, value, span)
        || !n00bb_emit_jump(context, scope->exit, span)) {
        lower_internal(context, span, "could not leave a construct");
        return false;
    }
    if (reason == N00BB_EXIT_FALLTHROUGH) scope->fallthrough = true;
    if (reason == N00BB_EXIT_FAILURE) scope->failure_reachable = true;
    context->reachable = false;
    return true;
}

// A `return`, `break` or `continue` leaves through `scope`'s exit block and
// goes on from a block of its own, where lowering continues.
static bool
leave_through(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
              ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *resume = n00bb_branch_block(context, lower, "exit_resume");
    if (!resume
        || !n00bb_exit_scope_jump(context, scope,
               N00BB_EXIT_RESUMES + scope->resumes.len, nullptr, span))
        return false;
    core_list_push(scope->resumes, resume);
    context->block = resume;
    context->reachable = true;
    return true;
}

bool
n00bb_exit_scope_close(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                       ir_span_t span, bool *reached, ir_value_t **failure)
{
    *reached = false;
    *failure = nullptr;
    scope->open = false;
    if (context->reachable
        && !n00bb_exit_scope_jump(context, scope, N00BB_EXIT_FALLTHROUGH,
                                  nullptr, span))
        return false;
    // With no failure reaching it, the failure option holds nothing.
    if (!scope->failure_reachable)
        (void)th_var_bind(scope->failure_type,
                          th_nil(ir_module_types(context->module)));
    if (!scope->exit) {
        context->reachable = false;
        return true;
    }
    context->block = scope->exit;
    context->reachable = true;
    *reached = true;
    if (!scope->failure_reachable) return true;
    *failure = n00bb_emit_local_read(context, scope->failure, span);
    return *failure != nullptr;
}

// Branches to `target` when the reason is `reason`, and continues lowering
// where it isn't.
static bool
branch_on_reason(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                 uint64_t reason, ir_block_t *target, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_value_t *current = n00bb_emit_local_read(context, scope->reason, span);
    ir_value_t *wanted = current
        ? reason_value(context, reason, span) : nullptr;
    ir_value_t *hit = wanted
        ? n00bb_emit_binary_ir(context, IR_OP_EQ, bool_type, current, wanted,
                               span) : nullptr;
    ir_block_t *next = n00bb_branch_block(context, lower, "exit_reason");
    if (!hit || !next || !n00bb_emit_branch(context, hit, target, next, span))
        return false;
    context->block = next;
    return true;
}

// Goes on from the exit block to the block for the reason: `failed` for a
// failure, or `targets[reason]` (some of which may be missing).
static bool
dispatch_reason(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                ir_block_t *failed, ir_block_t **targets, size_t count,
                ir_span_t span)
{
    size_t last = count;
    while (last > 0 && !targets[last - 1]) last--;
    if (failed && last
        && !branch_on_reason(context, scope, N00BB_EXIT_FAILURE, failed,
                             span))
        return false;
    if (!last) return failed && n00bb_emit_jump(context, failed, span);
    for (size_t reason = 0; reason + 1 < last; reason++)
        if (targets[reason]
            && !branch_on_reason(context, scope, reason, targets[reason],
                                 span))
            return false;
    return n00bb_emit_jump(context, targets[last - 1], span);
}

// Goes on to `after` when `verdict`, what the construct's exit code said
// about a failure, drops it, and continues lowering where it doesn't. Only
// a `bool` verdict can drop one, by being `true`: an `__exit` that returns
// nothing never does. Which it is is known once types are, and the test
// folds away then.
static bool
drop_failure(ir_lower_context_t *context, ir_value_t *verdict,
             ir_block_t *after, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_block_t *decide = n00bb_branch_block(context, lower, "exit_decide");
    ir_block_t *rethrow = n00bb_branch_block(context, lower, "exit_rethrow");
    ir_instr_t *test = decide && rethrow
        ? ir_lower_emit_instruction(context, IR_OP_TYPE_TEST, span) : nullptr;
    if (!test || !ir_lower_set_temp_dest(context, test,
            CORE_STRING_STATIC("__n00bb_exit_verdict_"), bool_type)
        || !ir_instr_operand_value(test, verdict)
        || !ir_instr_operand_type(test, bool_type)
        || !n00bb_emit_branch(context, test->dest, decide, rethrow, span))
        return false;
    context->block = decide;
    ir_instr_t *narrow = ir_lower_emit_instruction(context, IR_OP_TYPE_NARROW,
                                                   span);
    if (!narrow || !ir_lower_set_temp_dest(context, narrow,
            CORE_STRING_STATIC("__n00bb_exit_drops_"), bool_type)
        || !ir_instr_operand_value(narrow, verdict)
        || !ir_instr_operand_type(narrow, bool_type)
        || !n00bb_emit_branch(context, narrow->dest, after, rethrow, span))
        return false;
    context->block = rethrow;
    return true;
}

bool
n00bb_exit_scope_finish(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                        ir_block_t *after, ir_value_t *verdict,
                        parse_tree_t *source, n00bb_text_t phrase)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, source);
    if (!context->reachable) return true;
    bool continues = scope->fallthrough
        || (scope->failure_reachable && verdict);
    if (!after && continues)
        after = n00bb_branch_block(context, lower, "exit_after");
    ir_block_t *failed = scope->failure_reachable
        ? n00bb_branch_block(context, lower, "exit_failed") : nullptr;
    size_t count = N00BB_EXIT_RESUMES + scope->resumes.len;
    ir_block_t **targets = core_alloc_array(ir_block_t *, count);
    targets[N00BB_EXIT_FALLTHROUGH] = scope->fallthrough ? after : nullptr;
    for (size_t r = 0; r < scope->resumes.len; r++)
        targets[N00BB_EXIT_RESUMES + r] = scope->resumes.data[r];
    bool ok = (!continues || after) && (!scope->failure_reachable || failed)
        && dispatch_reason(context, scope, failed, targets, count, span);
    core_free(targets);
    if (!ok) {
        lower_internal(context, span, "could not leave a construct");
        return false;
    }
    if (failed) {
        context->block = failed;
        context->reachable = true;
        if (verdict && !drop_failure(context, verdict, after, span))
            return false;
        ir_value_t *error = option_item(context, scope->failure,
                                        scope->failure_type, span);
        if (!error
            || !n00bb_propagate_failure(context, source, error, span,
                                        scope->order, phrase))
            return false;
    }
    context->block = continues ? after : context->block;
    context->reachable = continues;
    return true;
}

n00bb_exit_scope_t *
n00bb_innermost_exit_scope(n00bb_lower_context_t *lower, uint64_t stop_order,
                           uint64_t before_order)
{
    n00bb_exit_scope_t *found = nullptr;
    for (size_t i = 0; i < lower->withs.len; i++) {
        n00bb_with_frame_t *frame = &lower->withs.data[i];
        for (size_t j = 0; frame->active && j < frame->items.len; j++) {
            n00bb_exit_scope_t *scope = &frame->items.data[j].scope;
            if (scope->open && scope->order > stop_order
                && scope->order < before_order
                && (!found || scope->order > found->order))
                found = scope;
        }
    }
    for (size_t i = 0; i < lower->loops.len; i++) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i];
        n00bb_exit_scope_t *scope = &frame->scope;
        if (frame->active && scope->open && scope->order > stop_order
            && scope->order < before_order
            && (!found || scope->order > found->order))
            found = scope;
    }
    return found;
}

void
n00bb_failure_handler(n00bb_lower_context_t *lower, uint64_t before_order,
                      n00bb_try_frame_t **try_frame,
                      n00bb_exit_scope_t **scope)
{
    *try_frame = nullptr;
    for (size_t i = lower->tries.len; i > 0 && !*try_frame; i--) {
        n00bb_try_frame_t *frame = &lower->tries.data[i - 1];
        if (frame->active && frame->in_body && frame->order < before_order)
            *try_frame = frame;
    }
    *scope = n00bb_innermost_exit_scope(lower,
        *try_frame ? (*try_frame)->order : 0, before_order);
    // Only the innermost of the two handles the failure.
    if (*scope) *try_frame = nullptr;
}

bool
n00bb_propagate_failure(ir_lower_context_t *context, parse_tree_t *source,
                        ir_value_t *error, ir_span_t span,
                        uint64_t before_order, n00bb_text_t phrase)
{
    (void)source;
    int routed = n00bb_route_failure_before(context, error, span,
                                            before_order);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error->type))
        return report_uncaught_failure(context, span, error->type, phrase);
    return n00bb_emit_function_failure(context, error, span, before_order);
}

bool
n00bb_emit_function_failure(ir_lower_context_t *context, ir_value_t *error,
                            ir_span_t span, uint64_t before_order)
{
    n00bb_exit_scope_t *loop = n00bb_innermost_exit_scope(context->user, 0,
                                                          before_order);
    if (loop)
        return n00bb_exit_scope_jump(context, loop, N00BB_EXIT_FAILURE, error,
                                     span);
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_internal(context, span, "could not fail the function");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

// An option local that is `nil` from the function's entry, so every path
// has written it: a value read after leaving through exit blocks, which
// other paths reach too, is kept in one.
static ir_local_t *
carrier(ir_lower_context_t *context, th_type *option, ir_span_t span)
{
    ir_local_t *local = exit_local(context, "carried", option);
    ir_block_t *entry = ir_block_entry(context->function);
    ir_instr_t *none = local && entry
        ? ir_instr_insert(entry, IR_OP_OPTION_NONE, 0) : nullptr;
    bool made = none && ir_lower_set_temp_dest(context, none,
            CORE_STRING_STATIC("__n00bb_carried_"), option);
    ir_instr_t *store = made ? ir_instr_insert(entry, IR_OP_COPY, 1)
                             : nullptr;
    if (!store || !ir_instr_set_binding_initialization(store, true)
        || !ir_instr_set_dest_local(store, local)
        || !ir_instr_operand_value(store, none->dest))
        return nullptr;
    none->span = span;
    store->span = span;
    return local;
}

ir_local_t *
n00bb_keep_across_exits(ir_lower_context_t *context, ir_value_t *value,
                        ir_span_t span)
{
    th_type *option = th_option(ir_module_types(context->module),
                                value->type);
    ir_local_t *kept = carrier(context, option, span);
    ir_value_t *some = kept ? n00bb_emit_wrapping(context, IR_OP_OPTION_SOME,
                                                  value, option, span)
                            : nullptr;
    if (some && n00bb_emit_local_write(context, kept, some, span))
        return kept;
    lower_internal(context, span, "could not keep a value");
    return nullptr;
}

ir_value_t *
n00bb_kept_value(ir_lower_context_t *context, ir_local_t *kept,
                 th_type *type, ir_span_t span)
{
    ir_value_t *value = option_item(context, kept, type, span);
    if (!value) lower_internal(context, span, "could not read a kept value");
    return value;
}

bool
n00bb_emit_scoped_success(ir_lower_context_t *context,
                          parse_tree_t *exit_node, uint64_t stop_order,
                          ir_value_t **value)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, exit_node);
    n00bb_exit_scope_t *scope = n00bb_innermost_exit_scope(lower, stop_order,
                                                           UINT64_MAX);
    if (!scope) return true;
    th_type *kept_type = value && *value ? (*value)->type : nullptr;
    ir_local_t *kept = kept_type
        ? n00bb_keep_across_exits(context, *value, span) : nullptr;
    if (kept_type && !kept) return false;
    while (scope) {
        uint64_t before = scope->order;
        if (!leave_through(context, scope, span)) return false;
        scope = n00bb_innermost_exit_scope(lower, stop_order, before);
    }
    if (kept) *value = n00bb_kept_value(context, kept, kept_type, span);
    return !kept || *value;
}
