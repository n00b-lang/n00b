#include "lower_internal.h"

static bool
n00bb_context_has_hook(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                       core_string_t name);
static bool
n00bb_emit_context_hook_call(ir_lower_context_t *context,
                             n00bb_with_item_t *item,
                             core_string_t hook_kind, ir_value_t *hook_value,
                             ir_span_t span, ir_value_t **outcome,
                             th_type **failure);
static bool
n00bb_emit_context_hook(ir_lower_context_t *context,
                        n00bb_with_item_t *item, core_string_t hook_kind,
                        ir_value_t *hook_value, ir_span_t span);
static bool
n00bb_emit_context_hook_outcome_branch(ir_lower_context_t *context,
                                      ir_value_t *outcome, ir_span_t span,
                                      ir_block_t **ok_block,
                                      ir_block_t **error_block);
static bool
n00bb_propagate_context_hook_failure(ir_lower_context_t *context,
                                     parse_tree_t *source,
                                     ir_value_t *error, ir_span_t span,
                                     uint64_t before_order);
static bool
n00bb_emit_context_fail_handler(ir_lower_context_t *context,
                                n00bb_with_item_t *item,
                                parse_tree_t *source, ir_value_t *error,
                                ir_span_t span);
static bool
n00bb_emit_context_exit(ir_lower_context_t *context,
                        n00bb_with_item_t *item, parse_tree_t *source,
                        ir_span_t span);
static bool
n00bb_emit_with_failure_before(ir_lower_context_t *context,
                               parse_tree_t *exit_node,
                               uint64_t before_order);
static bool
n00bb_propagate_context_computation_failure(ir_lower_context_t *context,
                                            parse_tree_t *source,
                                            ir_value_t *error,
                                            th_type *error_type,
                                            ir_span_t span);
static ir_value_t *
n00bb_call_context_default(ir_lower_context_t *context,
                           parse_tree_t *source,
                           core_translation_unit_t *owner,
                           ir_func_t *function,
                           ir_value_t **arguments, size_t argument_count,
                           ir_span_t span);
static bool
n00bb_context_ensure_default(
    ir_lower_context_t *context, parse_visit_t *visit,
    core_translation_unit_t *owner, parse_sym_entry_t *symbol,
    parse_tree_t *formal_params,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    size_t parameter_count, size_t argument_count,
    ir_value_t **value_by_parameter, bool *provided_by_parameter,
    uint8_t *default_state, size_t index, ir_span_t span);
static bool
n00bb_bind_with_manager(ir_lower_context_t *context, parse_tree_t *node);
static bool
n00bb_bind_with_managers(ir_lower_context_t *context, parse_tree_t *node,
                         parse_tree_t *body);

static bool
n00bb_context_has_hook(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                       core_string_t name)
{
    if (!context_symbol(grammar, symbol)) return false;
    core_string_t role = n00bb_context_hook_role(name);
    if (symbol->decl_node)
        return role.data && parse_sym_binding(symbol, role) != nullptr;
    core_string_t suffix =
        core_string_eq(name, CORE_STRING_STATIC("context-enter"))
            ? CORE_STRING_STATIC("enter")
        : core_string_eq(name, CORE_STRING_STATIC("context-exit"))
            ? CORE_STRING_STATIC("exit")
        : core_string_eq(name, CORE_STRING_STATIC("context-fail"))
            ? CORE_STRING_STATIC("fail")
        : core_string_eq(name, CORE_STRING_STATIC("context-ensure"))
            ? CORE_STRING_STATIC("ensure") : (core_string_t){};
    core_buffer_t hook_name = {0};
    if (!suffix.data || !context_hook_name(symbol, suffix, &hook_name)) {
        core_buffer_release(&hook_name);
        return false;
    }
    bool found = false;
    for (parse_sym_entry_t *candidate = symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         !found && candidate; candidate = candidate->next_in_scope)
        found = candidate->kind == PARSE_SYM_FUNCTION
            && core_string_eq(candidate->name,
                              core_buffer_string(&hook_name));
    core_buffer_release(&hook_name);
    return found;
}

static bool
n00bb_emit_context_hook_call(ir_lower_context_t *context,
                             n00bb_with_item_t *item,
                             core_string_t hook_kind, ir_value_t *hook_value,
                             ir_span_t span, ir_value_t **outcome,
                             th_type **failure)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = item->context_unit
        ? tu_state(item->context_unit) : nullptr;
    core_string_t role = n00bb_context_hook_role(hook_kind);
    if (!lower || !n00bb_context_has_hook(lower->grammar, item->context_symbol,
                                hook_kind)
        || !state || !state->ir_module || !role.data) return false;
    core_link_declaration_t *declaration = link_declaration_for_symbol(
        state->link_analysis, item->context_symbol);
    ir_func_t *function = declaration
        ? core_link_declaration_binding(declaration, role)
        : nullptr;
    ir_value_t *address = function
        ? ir_func_address(state->ir_module, function) : nullptr;
    th_type *signature = function ? th_resolve(function->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION) {
        lower_internal(context, span, "context hook has no function");
        return false;
    }
    th_type *error_type = th_func_error(signature);
    bool can_fail = error_type
        && !th_is_nil(error_type);
    if (can_fail && !failure) {
        core_string_t name = item->context_symbol
            ? item->context_symbol->name : CORE_STRING_STATIC("context");
        lower_report(context, N00B_DIAG_CONTEXT_HOOK_CANNOT_FAIL, span,
            N00B_TEXT_THIS_HOOK_OF_CONTEXT_DECLARES,
            N00BB_ARGS(ARG_TEXT(name)));
        lower_help(context, N00B_TEXT_HANDLE_THE_FAILURE_INSIDE_THE,
            N00BB_NO_ARGS);
        return false;
    }
    size_t expected = th_func_nparams(signature);
    if (expected != item->captures.len + (hook_value ? 1 : 0)) {
        lower_internal(context, span,
                       "context hook capture count does not match its "
                       "signature");
        return false;
    }
    th_type *result_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : nullptr;
    if (can_fail && !result_type) {
        lower_internal(context, span,
                    "could not construct the context hook outcome type");
        return false;
    }
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    char result_buffer[48];
    core_string_t result_name = can_fail
        ? n00bb_format_name(result_buffer, sizeof(result_buffer),
            "__n00bb_hook_%llu", (unsigned long long)context->next_value_id++)
        : (core_string_t){};
    th_type *context_type = th_resolve(item->context_symbol->type);
    size_t fixed_captures = context_type
        ? th_func_nparams(context_type) : 0;
    bool has_rest_capture = context_type
        && th_func_rest(context_type);
    bool emitted = context_type
        && item->captures.len == fixed_captures + has_rest_capture
        && call && (!can_fail
        || ir_instr_set_dest(call, result_name, result_type))
        && ir_instr_operand_value(call, address)
        && ir_instr_set_call_signature(call, signature);
    for (size_t i = 0; emitted && i < fixed_captures; i++)
        emitted = ir_instr_operand_value(call, item->captures.data[i]);
    if (emitted && hook_value)
        emitted = ir_instr_operand_value(call, hook_value);
    if (emitted && has_rest_capture)
        emitted = ir_instr_operand_value(call,
            item->captures.data[fixed_captures]);
    if (!emitted) {
        lower_internal(context, span,
                    "could not invoke context hook");
        return false;
    }
    if (outcome) *outcome = can_fail ? call->dest : nullptr;
    if (failure) *failure = can_fail ? error_type : nullptr;
    return true;
}

static bool
n00bb_emit_context_hook(ir_lower_context_t *context,
                        n00bb_with_item_t *item, core_string_t hook_kind,
                        ir_value_t *hook_value, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, hook_kind)) return true;
    return n00bb_emit_context_hook_call(context, item, hook_kind,
        hook_value, span, nullptr, nullptr);
}

static bool
n00bb_emit_context_hook_outcome_branch(ir_lower_context_t *context,
                                      ir_value_t *outcome, ir_span_t span,
                                      ir_block_t **ok_block,
                                      ir_block_t **error_block)
{
    uint64_t id = context->next_value_id++;
    char ok_buffer[48], error_buffer[48], tag_buffer[48];
    char literal_buffer[48], test_buffer[48];
    core_string_t ok_name = n00bb_format_name(ok_buffer, sizeof(ok_buffer),
        "__n00bb_hook_ok_%llu", (unsigned long long)id);
    core_string_t error_name = n00bb_format_name(error_buffer,
        sizeof(error_buffer), "__n00bb_hook_error_%llu",
        (unsigned long long)id);
    core_string_t tag_name = n00bb_format_name(tag_buffer, sizeof(tag_buffer),
        "__n00bb_hook_tag_%llu", (unsigned long long)id);
    core_string_t literal_name = n00bb_format_name(literal_buffer,
        sizeof(literal_buffer), "__n00bb_hook_tag_lit_%llu",
        (unsigned long long)id);
    core_string_t test_name = n00bb_format_name(test_buffer,
        sizeof(test_buffer), "__n00bb_hook_is_ok_%llu",
        (unsigned long long)id);
    *ok_block = ir_block_add(context->function, ok_name);
    *error_block = ir_block_add(context->function, error_name);
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool emitted = *ok_block && *error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag(CORE_STRING_STATIC("ok")))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    emitted = emitted && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, *ok_block)
        && ir_instr_operand_block(branch, *error_block);
    if (!emitted) {
        lower_internal(context, span,
                    "could not branch on a context hook outcome");
        return false;
    }
    branch->span = span;
    return true;
}

static bool
n00bb_propagate_context_hook_failure(ir_lower_context_t *context,
                                     parse_tree_t *source,
                                     ir_value_t *error, ir_span_t span,
                                     uint64_t before_order)
{
    if (!n00bb_emit_with_failure_before(context, source, before_order))
        return false;
    int routed = n00bb_route_try_error(context, source, error, span);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error->type))
        return report_uncaught_failure(context, span, error->type,
                                       N00B_TEXT_PHRASE_THIS_CONTEXT_HOOK);
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_internal(context, span,
                    "could not propagate the context hook error");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

static bool
n00bb_emit_context_fail_handler(ir_lower_context_t *context,
                                n00bb_with_item_t *item,
                                parse_tree_t *source, ir_value_t *error,
                                ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, CORE_STRING_STATIC("context-fail")))
        return n00bb_propagate_context_hook_failure(context, source, error,
                                                    span, item->order);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, CORE_STRING_STATIC("context-fail"), error,
                                      span, &outcome, &failure)
        || !outcome || !failure)
        return false;
    ir_block_t *handled = nullptr, *failed = nullptr;
    if (!n00bb_emit_context_hook_outcome_branch(context, outcome, span,
                                                 &handled, &failed))
        return false;

    context->block = failed;
    ir_value_t *handler_error = n00bb_emit_outcome_payload(context, outcome,
        CORE_STRING_STATIC("error"), failure, span);
    if (!handler_error || !n00bb_emit_context_hook(context, item,
            CORE_STRING_STATIC("context-ensure"), item->provided_value, span)
        || !n00bb_propagate_context_hook_failure(context, source,
                                                  handler_error, span,
                                                  item->order))
        return false;

    context->block = handled;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, CORE_STRING_STATIC("context-ensure"),
                                 item->provided_value, span)
        || !n00bb_propagate_context_hook_failure(context, source, error,
                                                  span, item->order))
        return false;
    return true;
}

static bool
n00bb_emit_context_exit(ir_lower_context_t *context,
                        n00bb_with_item_t *item, parse_tree_t *source,
                        ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, CORE_STRING_STATIC("context-exit")))
        return n00bb_emit_context_hook(context, item, CORE_STRING_STATIC("context-ensure"),
                                       item->provided_value, span);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, CORE_STRING_STATIC("context-exit"),
            item->provided_value, span, &outcome, &failure)) return false;
    if (!outcome)
        return n00bb_emit_context_hook(context, item, CORE_STRING_STATIC("context-ensure"),
                                       item->provided_value, span);

    ir_block_t *normal = nullptr, *failed = nullptr;
    if (!failure || !n00bb_emit_context_hook_outcome_branch(context, outcome,
            span, &normal, &failed)) return false;
    uint64_t id = context->next_value_id++;
    char join_buffer[48];
    core_string_t join_name = n00bb_format_name(join_buffer,
        sizeof(join_buffer), "__n00bb_hook_join_%llu",
        (unsigned long long)id);
    ir_block_t *join = ir_block_add(context->function, join_name);
    if (!join) return false;

    context->block = normal;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, CORE_STRING_STATIC("context-ensure"),
                                 item->provided_value, span)
        || !n00bb_emit_jump(context, join, span)) return false;

    context->block = failed;
    context->reachable = true;
    ir_value_t *error = n00bb_emit_outcome_payload(context, outcome, CORE_STRING_STATIC("error"),
                                                    failure, span);
    if (!error) return false;
    if (!n00bb_context_has_hook(lower->grammar, item->context_symbol,
                                CORE_STRING_STATIC("context-fail"))) {
        if (!n00bb_emit_context_hook(context, item, CORE_STRING_STATIC("context-ensure"),
                                     item->provided_value, span)
            || !n00bb_propagate_context_hook_failure(context, source, error,
                                                      span, item->order))
            return false;
    }
    else if (!n00bb_emit_context_fail_handler(context, item, source, error,
                                               span)) return false;

    context->block = join;
    context->reachable = true;
    return true;
}

bool
n00bb_emit_scoped_cleanups(ir_lower_context_t *context,
                           parse_tree_t *exit_node, uint64_t stop_order,
                           n00bb_try_frame_t *handler, bool success,
                           uint64_t before_order)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, exit_node);
    uint64_t before = before_order;
    for (;;) {
        n00bb_loop_frame_t *loop = nullptr;
        n00bb_with_item_t *with_item = nullptr;
        uint64_t with_order = 0;
        for (size_t i = lower->loops.len; i > 0; i--) {
            n00bb_loop_frame_t *candidate = &lower->loops.data[i - 1];
            if (!candidate->active || !candidate->is_container
                || !candidate->cleanup_active
                || candidate->order <= stop_order
                || candidate->order >= before
                || !node_is_within(exit_node, candidate->node)
                || (handler && node_is_within(handler->node,
                                              candidate->node)))
                continue;
            if (!loop || candidate->order > loop->order) loop = candidate;
        }
        for (size_t i = lower->withs.len; i > 0; i--) {
            n00bb_with_frame_t *candidate = &lower->withs.data[i - 1];
            if (!candidate->active
                || !node_is_within(exit_node, candidate->node)
                || (handler && node_is_within(handler->node,
                                              candidate->node)))
                continue;
            for (size_t j = candidate->items.len; j > 0; j--) {
                n00bb_with_item_t *item = &candidate->items.data[j - 1];
                if (!item->active || item->order <= stop_order
                    || item->order >= before
                    || (with_item && item->order <= with_order))
                    continue;
                with_item = item;
                with_order = item->order;
            }
        }
        n00bb_final_frame_t *final = nullptr;
        for (size_t i = lower->finals.len; i > lower->final_base; i--) {
            n00bb_final_frame_t *candidate = &lower->finals.data[i - 1];
            if (candidate->order <= stop_order
                || candidate->order >= before
                || !node_is_within(exit_node, candidate->scope)
                || (handler && node_is_within(handler->node,
                                              candidate->scope))
                || (final && candidate->order <= final->order))
                continue;
            final = candidate;
        }
        if (final && (!loop || final->order > loop->order)
            && (!with_item || final->order > with_order)) {
            before = final->order;
            if (!n00bb_emit_scope_finalize(context, final, exit_node))
                return false;
            continue;
        }
        if (!loop && !with_item) break;
        if (loop && (!with_item || loop->order > with_order)) {
            before = loop->order;
            if (!n00bb_emit_iteration_cleanup(context, loop, exit_node))
                return false;
            continue;
        }
        before = with_order;
        if (success) {
            if (!n00bb_emit_context_exit(context, with_item, exit_node, span))
                return false;
        }
        else if (!n00bb_emit_context_hook(context, with_item,
                CORE_STRING_STATIC("context-ensure"), with_item->provided_value, span))
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
                                      nullptr, true, UINT64_MAX);
}

bool
n00bb_emit_with_success(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return n00bb_emit_scoped_success(context, exit_node, nullptr);
}

bool
n00bb_emit_with_failure(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return n00bb_emit_with_failure_before(context, exit_node, UINT64_MAX);
}

static bool
n00bb_emit_with_failure_before(ir_lower_context_t *context,
                               parse_tree_t *exit_node,
                               uint64_t before_order)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *handler = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && node_is_within(exit_node, candidate->body)) {
            handler = candidate;
            break;
        }
    }
    return n00bb_emit_scoped_cleanups(context, exit_node, 0, handler,
                                      false, before_order);
}

static bool
n00bb_propagate_context_computation_failure(ir_lower_context_t *context,
                                            parse_tree_t *source,
                                            ir_value_t *error,
                                            th_type *error_type,
                                            ir_span_t span)
{
    if (!error || !error_type
        || !n00bb_emit_with_failure(context, source)) return false;
    int routed = n00bb_route_try_error(context, source, error, span);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error_type))
        return report_uncaught_failure(context, span, error_type,
                                       N00B_TEXT_PHRASE_CONTEXT_ARGUMENTS);
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_internal(context, span,
                    "could not propagate context constructor failure");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

static ir_value_t *
n00bb_call_context_default(ir_lower_context_t *context,
                           parse_tree_t *source,
                           core_translation_unit_t *owner,
                           ir_func_t *function,
                           ir_value_t **arguments, size_t argument_count,
                           ir_span_t span)
{
    n00bb_tu_state_t *owner_state = tu_state(owner);
    th_type *signature = function ? th_resolve(function->type) : nullptr;
    if (!owner_state || !signature || th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != argument_count
        || th_func_rest(signature)) {
        lower_internal(context, span,
            "context default helper does not match its dependency contract");
        return nullptr;
    }
    for (size_t i = 0; i < argument_count; i++) {
        const th_param *parameter = th_func_param(signature, i);
        if (!parameter || !arguments[i]) {
            lower_internal(context, span,
                "a context default dependency does not match its helper "
                "parameter");
            return nullptr;
        }
    }
    ir_value_t *address = ir_func_address(owner_state->ir_module, function);
    th_type *failure = th_func_error(signature);
    bool can_fail = failure
        && !th_is_nil(failure);
    th_type *result_type = th_func_ret(signature);
    if (!address || !result_type
        || th_is_nil(result_type)) {
        lower_internal(context, span,
            "context default produces no value for its parameter");
        return nullptr;
    }
    th_type *outcome_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : result_type;
    uint64_t id = context->next_value_id++;
    char call_buffer[48];
    core_string_t call_name = n00bb_format_name(call_buffer,
        sizeof(call_buffer), "__n00bb_context_default_%llu",
        (unsigned long long)id);
    ir_instr_t *call = outcome_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool emitted = call && ir_instr_set_dest(call, call_name, outcome_type)
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < argument_count; i++)
        emitted = ir_instr_operand_value(call, arguments[i]);
    if (!emitted) {
        lower_internal(context, span,
                    "could not invoke a context default helper");
        return nullptr;
    }
    if (!can_fail) return call->dest;

    th_type *definition = ir_type_definition(context->module, outcome_type);
    bool variant = false;
    th_type *ok_type = th_record_lookup(definition, CORE_STRING_STATIC("ok"));
    th_type *error_payload_type = th_record_lookup(definition, CORE_STRING_STATIC("error"));
    if (!definition || th_kind_of(definition) != TH_RECORD
        || !th_named_info(outcome_type, nullptr, &variant, nullptr)
        || !variant || !ok_type || !error_payload_type) {
        lower_internal(context, span,
            "context default outcome is not a complete variant");
        return nullptr;
    }
    char ok_buffer[48], error_buffer[48], tag_buffer[48];
    char literal_buffer[48], test_buffer[48];
    core_string_t ok_name = n00bb_format_name(ok_buffer, sizeof(ok_buffer),
        "__n00bb_default_ok_%llu", (unsigned long long)id);
    core_string_t error_name = n00bb_format_name(error_buffer,
        sizeof(error_buffer), "__n00bb_default_error_%llu",
        (unsigned long long)id);
    core_string_t tag_name = n00bb_format_name(tag_buffer, sizeof(tag_buffer),
        "__n00bb_default_tag_%llu", (unsigned long long)id);
    core_string_t literal_name = n00bb_format_name(literal_buffer,
        sizeof(literal_buffer), "__n00bb_default_tag_lit_%llu",
        (unsigned long long)id);
    core_string_t test_name = n00bb_format_name(test_buffer,
        sizeof(test_buffer), "__n00bb_default_is_ok_%llu",
        (unsigned long long)id);
    ir_block_t *ok_block = ir_block_add(context->function, ok_name);
    ir_block_t *error_block = ir_block_add(context->function, error_name);
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool branched = ok_block && error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, call->dest)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag(CORE_STRING_STATIC("ok")))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = branched
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    branched = branched && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, ok_block)
        && ir_instr_operand_block(branch, error_block);
    if (!branched) {
        lower_internal(context, span,
                    "could not branch on context default outcome");
        return nullptr;
    }
    branch->span = span;
    context->block = error_block;
    ir_value_t *error = n00bb_emit_outcome_payload(context, call->dest,
        CORE_STRING_STATIC("error"), error_payload_type, span);
    if (!n00bb_propagate_context_computation_failure(context, source, error,
                                                       failure, span))
        return nullptr;
    context->block = ok_block;
    context->reachable = true;
    ir_value_t *value = n00bb_emit_outcome_payload(context, call->dest,
        CORE_STRING_STATIC("ok"), ok_type, span);
    if (th_is_nil(ok_type))
        value = nullptr;
    if (!value) {
        lower_internal(context, span,
            "context default helper returned no successful value");
        return nullptr;
    }
    return value;
}

static bool
n00bb_context_ensure_default(
    ir_lower_context_t *context, parse_visit_t *visit,
    core_translation_unit_t *owner, parse_sym_entry_t *symbol,
    parse_tree_t *formal_params,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    size_t parameter_count, size_t argument_count,
    ir_value_t **value_by_parameter, bool *provided_by_parameter,
    uint8_t *default_state, size_t index, ir_span_t span)
{
    if (provided_by_parameter[index]) return true;
    if (default_state[index] == 1) {
        parse_sym_entry_t *cyclic = parameters && index < parameters->len
            ? parameters->data[index] : nullptr;
        if (cyclic)
            lower_report(context, N00B_DIAG_DEFAULT_CYCLE, span,
                N00B_TEXT_THE_DEFAULT_VALUE_OF_PARAMETER,
                N00BB_ARGS(ARG_TEXT(cyclic->name), ARG_TEXT(symbol->name)));
        else
            lower_report(context, N00B_DIAG_DEFAULT_CYCLE, span,
                N00B_TEXT_THE_DEFAULT_VALUES_OF_CONTEXT,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
        lower_help(context, N00B_TEXT_PASS_ONE_OF_THE_PARAMETERS,
            N00BB_NO_ARGS);
        return false;
    }
    default_state[index] = 1;

    bool interface_context = !symbol->decl_node && symbol->user_data;
    parse_sym_entry_t *formal = interface_context ? nullptr
        : formal_symbol_at(visit->grammar, formal_params, index,
                           &(size_t){0});
    uint8_t archived_flags = 0;
    bool optional = interface_context
        ? context_interface_parameter(symbol, index, nullptr,
            &archived_flags, nullptr) && (archived_flags & 1u)
        : formal && formal->is_optional;
    if (!optional) {
        default_state[index] = 0;
        parse_sym_entry_t *missing = parameters && index < parameters->len
            ? parameters->data[index] : formal;
        if (missing)
            lower_report(context, N00B_DIAG_CALL_MISSING_ARGUMENT, span,
                N00B_TEXT_CONTEXT_NEEDS_A_VALUE_FOR,
                N00BB_ARGS(ARG_TEXT(symbol->name), ARG_TEXT(missing->name)));
        else
            lower_report(context, N00B_DIAG_CALL_MISSING_ARGUMENT, span,
                N00B_TEXT_CONTEXT_IS_MISSING_A_REQUIRED,
                N00BB_ARGS(ARG_TEXT(symbol->name)));
        return false;
    }

    parse_tree_t *default_expr = formal
        ? child_nt(visit->grammar, formal->decl_node, CORE_STRING_STATIC("expression")) : nullptr;
    core_list_t(size_t) dependencies = core_list_new(size_t);
    bool valid = true;
    if (interface_context) {
        size_t *indices = core_alloc_array(size_t, argument_count);
        size_t dependency_count = 0;
        valid = context_interface_dependencies(symbol, index, indices,
                                               argument_count,
                                               &dependency_count);
        for (size_t d = 0; valid && d < dependency_count; d++) {
            if (indices[d] >= argument_count) valid = false;
            else core_list_push(dependencies, indices[d]);
        }
        core_free(indices);
    }
    else {
        core_list_t(parse_sym_entry_ptr_t) symbols =
            core_list_new(parse_sym_entry_ptr_t);
        context_default_dependencies(default_expr,
                                     parameters, &symbols);
        for (size_t d = 0; d < symbols.len; d++) {
            size_t dependency = 0;
            while (dependency < parameters->len
                   && parameters->data[dependency] != symbols.data[d])
                dependency++;
            if (dependency == parameters->len) valid = false;
            else core_list_push(dependencies, dependency);
        }
        core_list_free(symbols);
    }

    for (size_t d = 0; valid && d < dependencies.len; d++) {
        size_t dependency = dependencies.data[d];
        if (dependency >= argument_count || dependency == index) {
            valid = false;
            break;
        }
        bool dependency_provided = dependency >= parameter_count
            || provided_by_parameter[dependency];
        if (!dependency_provided
            && !n00bb_context_ensure_default(context, visit, owner, symbol,
                formal_params, parameters, parameter_count, argument_count,
                value_by_parameter, provided_by_parameter, default_state,
                dependency, span))
            valid = false;
    }

    char role_buffer[64];
    core_string_t role = {};
    core_link_declaration_t *declaration = link_declaration_for_symbol(
        tu_state(owner)->link_analysis, symbol);
    ir_func_t *helper = context_default_role(index, role_buffer,
                                             sizeof(role_buffer), &role)
        && declaration ? core_link_declaration_binding(declaration, role)
        : nullptr;
    ir_value_t **dependency_values = valid
        ? core_alloc_array(ir_value_t *, dependencies.len) : nullptr;
    for (size_t d = 0; valid && d < dependencies.len; d++) {
        dependency_values[d] = value_by_parameter[dependencies.data[d]];
        if (!dependency_values[d]) valid = false;
    }
    ir_value_t *default_value = valid && helper
        ? n00bb_call_context_default(context, visit->node, owner, helper,
            dependency_values, dependencies.len, span) : nullptr;
    core_free(dependency_values);
    core_list_free(dependencies);
    if (!default_value) {
        default_state[index] = 0;
        return false;
    }
    value_by_parameter[index] = default_value;
    provided_by_parameter[index] = true;
    default_state[index] = 2;
    (void)parameter_count;
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

int
n00bb_context_call(ir_lower_context_t *context, parse_visit_t *visit,
                   th_type *result_type, ir_span_t span,
                   ir_lower_result_t *result)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->withs.len)
        return 0;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (!frame->active || !node_is_within(visit->node, frame->node))
        return 0;

    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *symbol = attached_symbol(visit->grammar, lower->unit,
                                                 callee);
    if (!context_symbol(visit->grammar, symbol)) return 0;
    core_translation_unit_t *owner = unit_for_symbol(visit->grammar,
                                                      lower->units, symbol);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    bool has_enter = n00bb_context_has_hook(visit->grammar, symbol,
                                            CORE_STRING_STATIC("context-enter"));
    core_string_t hook_role =
        n00bb_context_hook_role(CORE_STRING_STATIC("context-enter"));
    core_link_declaration_t *context_declaration = owner_state
        ? link_declaration_for_symbol(owner_state->link_analysis, symbol)
        : nullptr;
    ir_func_t *function = context_declaration && hook_role.data
        ? core_link_declaration_binding(context_declaration, hook_role)
        : nullptr;
    if (!owner_state || !has_enter || !function) {
        lower_internal(context, span,
            "context enter hook has no canonical function");
        return -1;
    }
    ir_value_t *address = function
        ? ir_func_address(owner_state->ir_module, function) : nullptr;
    th_type *signature = symbol && symbol->type
        ? th_resolve(symbol->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION)
        return -1;
    th_type *failure = th_func_error(signature);
    bool can_fail = failure
        && !th_is_nil(failure);

    parse_tree_t *args = call_arguments_node(visit->grammar, visit->node);
    core_list_t(n00bb_ir_value_ptr_t) captures =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    size_t parameter_count = th_func_nparams(signature);
    bool interface_context = !symbol->decl_node && symbol->user_data;
    parse_tree_t *formal_params = interface_context ? nullptr
        : child_nt(visit->grammar, symbol->decl_node, CORE_STRING_STATIC("param-decl"));
    parse_sym_entry_t *rest_symbol = interface_context ? nullptr
        : formal_rest_symbol(visit->grammar, formal_params);
    const th_param *rest_parameter = th_func_rest(signature);
    bool archived_rest = false;
    if (interface_context &&
        context_interface_parameter_count(symbol) > parameter_count) {
        uint8_t flags = 0;
        archived_rest = context_interface_parameter(symbol, parameter_count,
            nullptr, &flags, nullptr) && (flags & 2u);
    }
    bool has_rest = (rest_symbol || archived_rest) && rest_parameter;
    if (interface_context &&
        context_interface_parameter_count(symbol) != parameter_count
            + (rest_parameter != nullptr)) {
        core_list_free(captures);
        core_list_free(actuals);
        lower_internal(context, span,
            "archived context parameters do not match its callable contract");
        return -1;
    }
    if ((rest_symbol != nullptr) != (rest_parameter != nullptr)) {
        core_list_free(captures);
        core_list_free(actuals);
        lower_internal(context, span,
            "context rest parameter does not match its constructor signature");
        return -1;
    }
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    size_t argument_count = parameter_count + (has_rest ? 1 : 0);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, argument_count);
    bool *provided_by_parameter = core_alloc_array(bool, parameter_count);
    n00bb_call_plan_t plan;
    bool mapped = n00bb_plan_call(&plan, visit->grammar, symbol, signature,
                                  args, 0);
    if (!mapped)
        n00bb_call_plan_report(&plan, lower_unit(context),
                               lower_diagnostics(context), visit->node);
    for (size_t i = 0; mapped && i < n00bb_call_plan_argument_count(&plan);
         i++) {
        size_t index = 0;
        parse_tree_t *argument = n00bb_call_plan_argument(&plan, i, &index);
        if (index != SIZE_MAX && index >= parameter_count) {
            lower_internal(context, lower_span_for_tree(context, argument),
                "a context argument has no parameter to lower into");
            mapped = false;
            break;
        }
        if (index != SIZE_MAX) provided_by_parameter[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    n00bb_call_plan_free(&plan);
    size_t missing_required = 0;
    for (size_t i = 0; i < parameter_count; i++) {
        parse_sym_entry_t *formal = interface_context ? nullptr
            : formal_symbol_at(visit->grammar, formal_params, i,
                               &(size_t){0});
        const th_param *param = th_func_param(signature, i);
        uint8_t archived_flags = 0, archived_mode = TH_IN;
        if (interface_context && !context_interface_parameter(symbol, i,
                nullptr, &archived_flags, &archived_mode))
            mapped = false;
        bool optional = interface_context
            ? (archived_flags & 1u) != 0
            : formal && formal->is_optional;
        bool mode_is_in = interface_context
            ? th_mode_is_in(archived_mode)
            : formal && formal->has_mode ? th_mode_is_in(formal->mode)
                                          : true;
        if ((!interface_context && !formal) || !param
            || !th_mode_is_in(param->mode) || !mode_is_in) {
            if (formal)
                lower_report(context, N00B_DIAG_CONTEXT_PARAMETER_MODE,
                    lower_symbol_name_span(context, formal),
                    N00B_TEXT_PARAMETER_OF_CONTEXT_MUST_BE,
                    N00BB_ARGS(ARG_TEXT(formal->name), ARG_TEXT(symbol->name)));
            else
                lower_internal(context, span,
                               "context parameter has no declaration");
            mapped = false;
            break;
        }
        if (!optional && !provided_by_parameter[i])
            missing_required++;
    }
    if (interface_context && mapped) {
        for (size_t i = 0; i < parameter_count; i++) {
            uint8_t flags = 0;
            context_interface_parameter(symbol, i, nullptr, &flags,
                                        nullptr);
            if ((flags & 1u) && !(flags & 4u)) {
                lower_internal(context, span,
                    "the imported context interface does not provide its "
                    "default computation");
                mapped = false;
                break;
            }
        }
    }
    if (mapped && missing_required) {
        core_list_t(core_string_t) missing = core_list_new(core_string_t);
        for (size_t i = 0; i < parameter_count; i++) {
            if (provided_by_parameter[i] || interface_context) continue;
            parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
                formal_params, i, &(size_t){0});
            if (formal && !formal->is_optional)
                core_list_push(missing, formal->name);
        }
        if (missing.len)
            lower_report(context, N00B_DIAG_CALL_MISSING_ARGUMENT, span,
                N00B_TEXT_CONTEXT_NEEDS_VALUES_FOR,
                N00BB_ARGS(ARG_NAMES(missing.data, missing.len),
                           ARG_TEXT(symbol->name)));
        else
            lower_report(context, N00B_DIAG_CALL_MISSING_ARGUMENT, span,
                N00B_TEXT_CONTEXT_IS_MISSING_ARGUMENTS,
                N00BB_ARGS(ARG_NUMBER(missing_required),
                           ARG_TEXT(symbol->name)));
        core_list_free(missing);
        mapped = false;
    }
    void *binding_checkpoint = ir_lower_bindings_checkpoint(context);
    bool collected = mapped;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        ir_value_t *value = ir_lower_node_value(context,
                                                 actuals.data[i].node);
        if (index == SIZE_MAX) {
            if (!rest_parameter || !value) {
                collected = false;
                break;
            }
            core_list_push(rest_values, value);
            continue;
        }
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, index, &(size_t){0});
        if ((!interface_context && !formal)
            || (interface_context && !value)) {
            collected = false;
            break;
        }
        value_by_parameter[index] = value;
    }

    if (collected && has_rest) {
        ir_value_t *array = n00bb_pack_rest_values(context,
            rest_parameter->type, &rest_values, span);
        value_by_parameter[parameter_count] = array;
        collected = array && (!rest_symbol || interface_context
            || ir_lower_bind_scoped_value(context, rest_symbol, array,
                                          nullptr));
    }
    core_list_t(parse_sym_entry_ptr_t) default_parameters =
        core_list_new(parse_sym_entry_ptr_t);
    if (!interface_context) {
        parse_sym_entry_t *default_rest = nullptr;
        collect_ir_parameters(visit->grammar, formal_params,
                              &default_parameters, &default_rest);
        if (default_rest) core_list_push(default_parameters, default_rest);
    }
    uint8_t *default_state = core_alloc_array(uint8_t, parameter_count);
    for (size_t i = 0; collected && i < parameter_count; i++)
        if (!provided_by_parameter[i])
            collected = n00bb_context_ensure_default(context, visit, owner,
                symbol, formal_params, &default_parameters, parameter_count,
                argument_count, value_by_parameter, provided_by_parameter,
                default_state, i, span);
    core_free(default_state);
    core_list_free(default_parameters);
    if (collected) {
        for (size_t i = 0; i < parameter_count; i++) {
            if (!value_by_parameter[i]) {
                collected = false;
                break;
            }
            core_list_push(captures, value_by_parameter[i]);
        }
    }
    if (collected && has_rest)
        core_list_push(captures, value_by_parameter[parameter_count]);
    core_list_free(rest_values);
    if (!ir_lower_bindings_restore(context, binding_checkpoint))
        collected = false;
    core_list_free(actuals);
    core_free(value_by_parameter);
    core_free(provided_by_parameter);
    if (!collected) {
        core_list_free(captures);
        lower_failed_after(context, span,
            CORE_STRING_STATIC("context arguments do not match the "
                               "constructor signature"));
        return -1;
    }
    for (size_t i = 0; i < parameter_count; i++) {
        const th_param *param = th_func_param(signature, i);
        if (!param || !th_mode_is_in(param->mode)) {
            core_list_free(captures);
            lower_internal(context, span,
                           "context constructor parameter is not `in`");
            return -1;
        }
    }
    if (rest_parameter && !th_mode_is_in(rest_parameter->mode)) {
        core_list_free(captures);
        lower_report(context, N00B_DIAG_CONTEXT_PARAMETER_MODE, span,
            N00B_TEXT_THE_EXTRA_ARGUMENTS_PARAMETER_OF,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        return -1;
    }
    th_type *provided_type = th_func_ret(signature);
    if (th_is_closed(ir_module_types(context->module), result_type)
        && !ir_type_eq(context->module, provided_type, result_type)) {
        core_buffer_t message = {0};
        th_ctx *types = ir_module_types(context->module);
        core_string_t provided_text = n00bb_print_type(types, provided_type);
        core_string_t result_text = n00bb_print_type(types, result_type);
        core_buffer_printf(&message,
            "context enter result type %.*s does not match inferred context-call type %.*s",
            (int)provided_text.u8_bytes, provided_text.data,
            (int)result_text.u8_bytes, result_text.data);
        lower_internal_string(context, span, core_buffer_string(&message));
        core_buffer_release(&message);
        core_list_free(captures);
        return -1;
    }
    bool has_value = !th_is_nil(provided_type);
    th_type *outcome_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : provided_type;
    if (!outcome_type) {
        core_list_free(captures);
        lower_internal(context, span,
                    "could not construct the context enter outcome type");
        return -1;
    }
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    ir_value_t *provided = nullptr;
    char value_buffer[48];
    core_string_t value_name = n00bb_format_name(value_buffer,
        sizeof(value_buffer), "__n00bb_context_%llu",
        (unsigned long long)context->next_value_id++);
    bool emitted = call && ir_instr_set_dest(call, value_name, outcome_type)
        && ir_instr_operand_value(call, address)
        && ir_instr_set_call_signature(call, signature);
    for (size_t i = 0; emitted && i < captures.len; i++)
        emitted = ir_instr_operand_value(call, captures.data[i]);
    if (!emitted) {
        core_list_free(captures);
    {
        lower_internal(context, span,
                    "could not invoke context enter hook");
        return -1;
    }
    }
    if (can_fail) {
        th_type *definition = ir_type_definition(context->module,
                                                  outcome_type);
        bool variant = false;
        th_type *ok_type = th_record_lookup(definition, CORE_STRING_STATIC("ok"));
        th_type *error_type = th_record_lookup(definition, CORE_STRING_STATIC("error"));
        if (!definition || th_kind_of(definition) != TH_RECORD
            || !th_named_info(outcome_type, nullptr, &variant, nullptr)
            || !variant || !ok_type || !error_type) {
            lower_internal(context, span,
                           "context enter outcome is not a complete variant");
            return -1;
        }
        uint64_t id = context->next_value_id++;
        char ok_buffer[48], error_buffer[48];
        core_string_t ok_name = n00bb_format_name(ok_buffer,
            sizeof(ok_buffer), "__n00bb_enter_ok_%llu",
            (unsigned long long)id);
        core_string_t error_name = n00bb_format_name(error_buffer,
            sizeof(error_buffer), "__n00bb_enter_error_%llu",
            (unsigned long long)id);
        ir_block_t *ok_block = ir_block_add(context->function, ok_name);
        ir_block_t *error_block = ir_block_add(context->function, error_name);
        th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
        th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
        char tag_buffer[48], literal_buffer[48], test_buffer[48];
        core_string_t tag_name = n00bb_format_name(tag_buffer,
            sizeof(tag_buffer), "__n00bb_enter_tag_%llu",
            (unsigned long long)id);
        core_string_t literal_name = n00bb_format_name(literal_buffer,
            sizeof(literal_buffer), "__n00bb_enter_tag_lit_%llu",
            (unsigned long long)id);
        core_string_t test_name = n00bb_format_name(test_buffer,
            sizeof(test_buffer), "__n00bb_enter_is_ok_%llu",
            (unsigned long long)id);
        ir_instr_t *tag = ir_lower_emit_instruction(context,
            IR_OP_VARIANT_TAG, span);
        ir_instr_t *literal = ir_lower_emit_instruction(context,
            IR_OP_CONST, span);
        ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
        bool branched = ok_block && error_block && uint_type && bool_type
            && tag && ir_instr_set_dest(tag, tag_name, uint_type)
            && ir_instr_operand_value(tag, call->dest)
            && literal && ir_instr_set_dest(literal, literal_name, uint_type)
            && ir_instr_operand_uint(literal, th_arm_tag(CORE_STRING_STATIC("ok")))
            && test && ir_instr_set_dest(test, test_name, bool_type)
            && ir_instr_operand_value(test, tag->dest)
            && ir_instr_operand_value(test, literal->dest);
        ir_instr_t *branch = branched
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        branched = branched && branch
            && ir_instr_operand_value(branch, test->dest)
            && ir_instr_operand_block(branch, ok_block)
            && ir_instr_operand_block(branch, error_block);
        if (!branched) {
            lower_internal(context, span,
                        "could not branch on context enter outcome");
            return -1;
        }
        branch->span = span;
        context->block = error_block;
        ir_value_t *enter_error = n00bb_emit_outcome_payload(
            context, call->dest, CORE_STRING_STATIC("error"), error_type, span);
        if (!enter_error || !n00bb_emit_with_failure(context,
                                                     visit->node)) {
            lower_internal(context, span,
                        "could not unwind active contexts after failed enter");
            return -1;
        }
        int routed = n00bb_route_try_error(context, visit->node,
                                            enter_error, span);
        if (routed < 0) return -1;
        if (!routed && !n00bb_failure_channel_accepts(context, error_type)) {
            if (!report_uncaught_failure(context, span, error_type,
                    N00B_TEXT_PHRASE_ENTERING_THIS_CONTEXT))
                return -1;
        }
        else if (!routed) {
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, enter_error)) {
                lower_internal(context, span,
                            "could not propagate context enter failure");
                return -1;
            }
            fail->span = span;
            context->reachable = false;
        }
        context->block = ok_block;
        context->reachable = true;
        if (!th_is_nil(ok_type))
            provided = n00bb_emit_outcome_payload(context, call->dest,
                                                   CORE_STRING_STATIC("ok"), ok_type, span);
        if (!th_is_nil(ok_type)
            && !provided) {
            lower_internal(context, span,
                        "could not extract successful context enter value");
            return -1;
        }
    }
    else provided = has_value ? call->dest : nullptr;

    n00bb_with_item_t item = {
        .context_symbol = symbol,
        .context_unit = owner,
        .call_node = visit->node,
        .captures = captures,
        .provided_value = provided,
        .order = ++lower->next_control_order,
        .active = true,
    };
    core_list_push(frame->items, item);

    *result = provided
        ? (ir_lower_result_t){IR_LOWER_VALUE, provided}
        : (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    return 1;
}

static bool
n00bb_bind_with_manager(ir_lower_context_t *context, parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = tu_state(lower->unit);
    if (!tu || !tu->annotations || !tu->annotations->symtab
        || core_tree_num_children(node) < 3)
        return true;
    core_string_t separator = first_token_text(core_tree_child(node, 1));
    if (!core_string_eq(separator, CORE_STRING_STATIC("="))
        && !core_string_eq(separator, CORE_STRING_STATIC(":")))
        return true;
    parse_sym_entry_t *binding = parse_symtab_resolve_node(
        tu->annotations->symtab, node, CORE_STRING_STATIC(""));
    if (!binding || !binding->type) {
        lower_internal(context, lower_span_for_tree(context, node),
                       "named context binding has no inferred type");
        return false;
    }
    char local_buffer[48];
    core_string_t local_name = n00bb_format_name(local_buffer,
        sizeof(local_buffer), "__n00bb_context_value_%llu",
        (unsigned long long)context->next_value_id++);
    ir_local_t *local = ir_local_add(context->function, local_name,
                                     binding->type);
    if (!local || !ir_lower_bind_local(context, binding, local)) {
        lower_internal(context,
            lower_span_for_tree(context, node),
            "could not bind the context-provided local");
        return false;
    }
    return true;
}

static bool
n00bb_bind_with_managers(ir_lower_context_t *context, parse_tree_t *node,
                         parse_tree_t *body)
{
    if (!node || node == body || core_tree_is_leaf(node)) return true;
    n00bb_lower_context_t *lower = context->user;
    if (nt_is(lower->grammar, node, CORE_STRING_STATIC("context-manager")))
        return n00bb_bind_with_manager(context, node);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_bind_with_managers(context, core_tree_child(node, i),
                                      body))
            return false;
    return true;
}

bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("with-stmt")))
        return true;
    if (entering) {
        n00bb_with_frame_t frame = {
            .node = visit->node,
            .body = child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("body")),
            .items = core_list_new(n00bb_with_item_t),
            .active = context->reachable,
        };
        if (frame.active && !n00bb_bind_with_managers(
                context, visit->node, frame.body))
            return false;
        core_list_push(lower->withs, frame);
        return true;
    }
    if (!lower->withs.len
        || lower->withs.data[lower->withs.len - 1].node != visit->node)
        return false;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (frame->active && context->reachable) {
        ir_span_t span = lower_span_for_tree(context, visit->node);
        for (size_t i = frame->items.len; i > 0; i--) {
            n00bb_with_item_t *item = &frame->items.data[i - 1];
            if (!n00bb_emit_context_exit(context, item, visit->node, span))
                return false;
        }
    }
    for (size_t i = 0; i < frame->items.len; i++)
        core_list_free(frame->items.data[i].captures);
    core_list_free(frame->items);
    lower->withs.len--;
    return true;
}
