#include "lower_internal.h"

static bool
ir_code_registry_entry(ir_diagcode_t code, n00bb_diag_t *out);
static size_t
report_ir_diagnostic(void *state, ir_diags_t *d, ir_diagcode_t code,
                     core_diag_severity_t severity, core_diag_location_t at,
                     core_string_t message);
static parse_tree_t *
leaf_named(parse_tree_t *node, core_string_t name);
static n00bb_text_t
mode_parameter(th_mode mode);

bool
n00bb_output_target_profile(core_target_desc_t *target)
{
    core_string_t selected = n00bb_setting_string(
        N00BB_KEY_BUILD_TARGET);
    core_target_desc_t host;
    if (!selected.u8_bytes
        || core_string_eq(selected, CORE_STRING_STATIC("host"))) {
        if (!core_target_host(&host)) return false;
        selected = host.triple;
    }
    bool wide = n00bb_setting_bool(N00BB_KEY_BUILD_ATOMIC128)
        && core_string_starts_with(selected, CORE_STRING_STATIC("x86_64"));
    return core_target_profile(selected,
        wide ? CORE_TARGET_FEATURE_NATIVE_ATOMIC128 : 0, target);
}

bool
n00bb_cross_target(core_target_desc_t *target)
{
    core_target_desc_t host;
    return n00bb_output_target_profile(target) && core_target_host(&host)
        && !core_string_eq(target->triple, host.triple);
}

[[gnu::format(printf, 3, 4)]] core_string_t
n00bb_format_name(char *buffer, size_t size, const char *format, ...)
{
    if (!buffer || !size) return (core_string_t){};
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    if (written < 0) written = 0;
    size_t length = (size_t)written < size ? (size_t)written : size - 1;
    return core_string_borrow(buffer, length);
}

void
n00bb_order_copyouts(core_list_t(n00bb_copyout_t) *copyouts)
{
    for (size_t i = 1; i < copyouts->len; i++) {
        n00bb_copyout_t item = copyouts->data[i];
        size_t j = i;
        while (j && copyouts->data[j - 1].output_index > item.output_index) {
            copyouts->data[j] = copyouts->data[j - 1];
            j--;
        }
        copyouts->data[j] = item;
    }
}

th_type *
n00bb_node_type(ir_lower_context_t *context, parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_type *type = lower && lower->node_types_ready && node
        ? core_dict_get(&lower->node_types, node, nullptr) : nullptr;
    return type ? type : node ? parse_tree_type(node) : nullptr;
}

th_type *
n00bb_node_result_type(ir_lower_context_t *context, parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *type = n00bb_node_type(context, node);
    if (type || !lower) return type;
    if (!lower->node_types_ready) {
        core_dict_init(&lower->node_types, nullptr, nullptr);
        lower->node_types_ready = true;
    }
    type = th_var(ir_module_types(context->module));
    core_dict_put(&lower->node_types, node, type);
    return type;
}

ir_value_t *
n00bb_open_record_literal(ir_lower_context_t *context,
                          n00bb_tuple_parts_t *parts, ir_span_t span)
{
    size_t count = parts->values.len;
    th_member *members = core_alloc_array(th_member, count);
    for (size_t i = 0; i < count; i++) {
        parse_tree_t *field = parse_tree_parent(parts->expressions.data[i]);
        members[i] = (th_member){
            .name = field ? first_token_text(field) : (core_string_t){0},
            .type = th_var(ir_module_types(context->module)),
        };
    }
    th_type *record = th_record_sorted(ir_module_types(context->module),
                                       members, count);
    ir_instr_t *aggregate = record
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
        CORE_STRING_STATIC("__n00bb_record_"), record);
    for (size_t j = 0; ok && j < th_record_len(record); j++) {
        core_string_t name = th_record_member(record, j)->name;
        size_t i = 0;
        while (i < count && !core_string_eq(
                   first_token_text(parse_tree_parent(
                       parts->expressions.data[i])), name))
            i++;
        ok = i < count && parts->values.data[i]
            && ir_instr_operand_value(aggregate, parts->values.data[i]);
    }
    core_free(members);
    n00bb_lower_context_t *lower = context->user;
    ok = ok && ir_annot_set(lower->ir_context, &aggregate->annots,
        (ir_annot_t){.key = N00BB_RECORD_LITERAL_KEY, .kind = IR_ANNOT_BOOL,
                     .b = true});
    if (ok) n00bb_rewrite_later(context, aggregate);
    return ok ? aggregate->dest : nullptr;
}

void
n00bb_rewrite_later(ir_lower_context_t *context, ir_instr_t *in)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (in && lower && lower->unit && !lower->solved)
        core_list_push(tu_state(lower->unit)->rewrite_sites, in);
}

bool
n00bb_open_type(ir_lower_context_t *context, th_type *type)
{
    return !type || !th_is_closed(ir_module_types(context->module), type);
}

ir_value_t *
n00bb_dispatch(ir_lower_context_t *context, core_string_t method,
               ir_value_t *const *operands, size_t count,
               th_type *result_type, ir_span_t span)
{
    ir_value_t *result = ir_lower_dispatch(context, method, operands, count,
                                           result_type, span);
    if (result) n00bb_rewrite_later(context, result->def);
    return result;
}

bool
n00bb_add_iteration_backedge(n00bb_loop_frame_t *frame,
                              ir_block_t *predecessor)
{
    return !frame || !frame->is_container
        || (frame->iteration_cursor_phi && frame->iteration_cursor_value
            && predecessor
            && ir_instr_operand_phi_arm(frame->iteration_cursor_phi,
                                        predecessor,
                                        frame->iteration_cursor_value));
}

ir_module_t *
unit_module(n00bb_tu_state_t *state)
{
    return state ? state->ir_module : nullptr;
}

parse_tree_t *
value_body_expression(parse_grammar_t *grammar, parse_tree_t *body)
{
    parse_tree_t *statements = child_nt(grammar, body, CORE_STRING_STATIC("value-body-stmts"));
    parse_tree_t *value = child_nt(grammar, statements,
                                   CORE_STRING_STATIC("value-stmt"));
    return child_nt(grammar, value, CORE_STRING_STATIC("expression"));
}

core_string_t
n00bb_control_label(parse_grammar_t *grammar, parse_tree_t *node,
                    core_string_t labeled_form)
{
    parse_tree_t *parent = node
        ? parse_tree_parent(node) : nullptr;
    if (!nt_is(grammar, parent, labeled_form)) return (core_string_t){0};
    return first_token_text(child_nt(grammar, parent, CORE_STRING_STATIC("label")));
}

void
append_module_symbol_name(core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol, core_buffer_t *buffer)
{
    core_string_t module = unit->module.identifier;
    if (symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("package-interface"))
        && symbol->scope->name.data)
        module = symbol->scope->name;
    n00bb_qualify(buffer, module, symbol->name);
}

ir_lower_result_t
lower_error_text(ir_lower_context_t *context, ir_diagcode_t code,
                 ir_span_t span, core_string_t message)
{
    (void)code;
    return lower_internal_string(context, span, message);
}

static bool
ir_code_registry_entry(ir_diagcode_t code, n00bb_diag_t *out)
{
    switch (code) {
    case IR_DIAG_UNSET_LOCAL: *out = N00B_DIAG_READ_BEFORE_ASSIGNMENT; return true;
    case IR_DIAG_DEAD_CODE: *out = N00B_DIAG_UNREACHABLE_CODE; return true;
    case IR_DIAG_UNUSED_LOCAL: *out = N00B_DIAG_UNUSED_ASSIGNMENT; return true;
    case IR_DIAG_UNUSED_RESULT: *out = N00B_DIAG_UNUSED_VALUE; return true;
    case IR_DIAG_UNBOUNDED_RECURSION:
        *out = N00B_DIAG_UNBOUNDED_RECURSION;
        return true;
    case IR_DIAG_GUARDED_WRITE: *out = N00B_DIAG_VIEW_WRITE; return true;
    case IR_DIAG_VIEW_ESCAPE: *out = N00B_DIAG_VIEW_ESCAPE; return true;
    case IR_DIAG_IMMUTABLE_REINITIALIZED:
        *out = N00B_DIAG_LET_ASSIGNED_TWICE;
        return true;
    default: return false;
    }
}

static size_t
report_ir_diagnostic(void *state, ir_diags_t *d, ir_diagcode_t code,
                     core_diag_severity_t severity, core_diag_location_t at,
                     core_string_t message)
{
    const char *policy = state;
    core_diagnostics_t *destination = ir_diags_destination(d);
    const core_translation_unit_t *unit = ir_diags_unit(d);
    if (!destination) return SIZE_MAX;
    size_t index = core_diagnostics_count(destination);
    n00bb_diag_t id;
    if (!ir_code_registry_entry(code, &id)) {
        if (severity != CORE_DIAG_ERROR && severity != CORE_DIAG_FATAL
            && severity != CORE_DIAG_INTERNAL)
            return SIZE_MAX;
        n00bb_internal_error(destination, unit, at, "analyzing the program",
                             "%.*s", (int)message.u8_bytes, message.data);
        return core_diagnostics_count(destination) > index ? index : SIZE_MAX;
    }
    core_diag_severity_t registered = n00bb_diag_severity(id);
    bool promoted = registered == CORE_DIAG_WARNING
        && severity == CORE_DIAG_ERROR;
    if (!core_diagnostics_report_at(destination, unit,
            promoted ? CORE_DIAG_ERROR : registered, n00bb_diag_code(id),
            n00bb_diag_title(id), at)
        || core_diagnostics_count(destination) == index)
        return SIZE_MAX;
    if (promoted && policy)
        n00bb_report_note(destination, N00B_TEXT_PROMOTED_BY_POLICY,
                          N00BB_ARGS(ARG_NATIVE(policy)));
    else if (promoted)
        n00bb_report_note(destination, N00B_TEXT_PROMOTED_BY_DEFAULT_POLICY,
                          N00BB_NO_ARGS);
    return index;
}

ir_diags_t *
attach_ir_diags(ir_ctx_t *ir_context, ir_policy_t *policy,
                core_translation_unit_t *unit, const char *policy_name)
{
    ir_diags_t *diagnostics = ir_diags_new_attached(ir_context, policy,
        core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return nullptr;
    ir_diags_set_reporter(diagnostics, report_ir_diagnostic,
                          (void *)policy_name);
    ir_diags_set_text(diagnostics, n00bb_ir_text, nullptr);
    return diagnostics;
}

core_diagnostics_t *
lower_diagnostics(ir_lower_context_t *context)
{
    return context ? ir_diags_destination(context->diagnostics) : nullptr;
}

size_t
lower_failures(ir_lower_context_t *context)
{
    return core_diagnostics_failures(lower_diagnostics(context));
}

const core_translation_unit_t *
lower_unit(ir_lower_context_t *context)
{
    return context ? ir_diags_unit(context->diagnostics) : nullptr;
}

ir_lower_result_t
lower_report(ir_lower_context_t *context, n00bb_diag_t id, ir_span_t span,
             n00bb_text_t text, n00bb_args_t args)
{
    n00bb_report(lower_diagnostics(context), lower_unit(context), id,
                 n00bb_span_location(span), text, args);
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

void
lower_secondary(ir_lower_context_t *context, ir_span_t span,
                n00bb_text_t text, n00bb_args_t args)
{
    if (ir_span_known(span))
        n00bb_report_secondary(lower_diagnostics(context), nullptr,
                               n00bb_span_location(span), text, args);
}

void
lower_declared_secondary(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, n00bb_text_t text,
                         n00bb_args_t args)
{
    parse_sym_entry_t *origin = symbol ? canonical_symbol(symbol) : nullptr;
    core_diag_location_t at = n00bb_symbol_location(origin);
    if (at.has_location && origin->owner_unit)
        n00bb_report_secondary(lower_diagnostics(context), origin->owner_unit,
                               at, text, args);
}

void
lower_help(ir_lower_context_t *context, n00bb_text_t text, n00bb_args_t args)
{
    n00bb_report_help(lower_diagnostics(context), text, args);
}

ir_lower_result_t
lower_internal_string(ir_lower_context_t *context, ir_span_t span,
                      core_string_t what)
{
    core_diagnostics_t *diagnostics = lower_diagnostics(context);
    if (!diagnostics) return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *function = lower ? lower->function_symbol : nullptr;
    char stage[256];
    if (function && function->name.u8_bytes
        && !core_string_starts_with(function->name,
                                    CORE_STRING_STATIC("__")))
        snprintf(stage, sizeof(stage), "generating code for `%.*s`",
                 (int)function->name.u8_bytes, function->name.data);
    else
        snprintf(stage, sizeof(stage), "generating code for top-level "
                 "statements");
    n00bb_internal_error(diagnostics, lower_unit(context),
                         n00bb_span_location(span), stage, "%.*s",
                         (int)what.u8_bytes, what.data);
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

bool
lower_mark_failed(ir_lower_context_t *context)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    return lower && context->function
        && ir_annot_set(lower->ir_context, &context->function->annots,
            (ir_annot_t){.key = N00BB_FAILED_KEY, .kind = IR_ANNOT_BOOL,
                         .b = true});
}

ir_lower_result_t
lower_placeholder(ir_lower_context_t *context, th_type *type, ir_span_t span)
{
    ir_instr_t *undef = type
        ? ir_lower_emit_instruction(context, IR_OP_UNDEF, span) : nullptr;
    return undef && lower_mark_failed(context)
        && ir_lower_set_temp_dest(context, undef,
            CORE_STRING_STATIC("__n00bb_failed_"), type)
        ? (ir_lower_result_t){IR_LOWER_VALUE, undef->dest}
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

ir_lower_result_t
lower_failed_after(ir_lower_context_t *context, ir_span_t span,
                   core_string_t what)
{
    core_diagnostics_t *diagnostics = lower_diagnostics(context);
    if (diagnostics && core_diagnostics_failed(diagnostics))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    return lower_internal_string(context, span, what);
}

parse_token_info_t *
last_tree_terminal(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token =
            last_tree_terminal(core_tree_child(node, i - 1));
        if (token) return token;
    }
    return nullptr;
}

ir_span_t
lower_span_for_tree(ir_lower_context_t *context, parse_tree_t *node)
{
    return context ? tree_span(context->file_id, node) : IR_SPAN_UNKNOWN;
}

ir_span_t
tree_span(int32_t file_id, parse_tree_t *node)
{
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_tree_terminal(node);
    if (file_id < 0 || !first || !last
        || parse_token_start_byte(first) > UINT32_MAX
        || parse_token_end_byte(last) > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(file_id,
                        (uint32_t)parse_token_start_byte(first),
                        (uint32_t)parse_token_end_byte(last),
                        parse_token_line(first), parse_token_column(first),
                        parse_token_endline(last), parse_token_endcol(last));
}

ir_span_t
lower_span_for_first_token(ir_lower_context_t *context, parse_tree_t *node)
{
    parse_token_info_t *first = node ? parse_tree_find_first_terminal(node)
                                     : nullptr;
    if (!context || context->file_id < 0 || !first
        || parse_token_end_byte(first) > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id,
                        (uint32_t)parse_token_start_byte(first),
                        (uint32_t)parse_token_end_byte(first),
                        parse_token_line(first), parse_token_column(first),
                        parse_token_endline(first), parse_token_endcol(first));
}

static parse_tree_t *
leaf_named(parse_tree_t *node, core_string_t name)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node))
        return core_string_eq(first_token_text(node), name) ? node : nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = leaf_named(core_tree_child(node, i), name);
        if (found) return found;
    }
    return nullptr;
}

ir_span_t
lower_symbol_name_span(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    if (!symbol || !symbol->decl_node) return IR_SPAN_UNKNOWN;
    parse_tree_t *name = leaf_named(symbol->decl_node, symbol->name);
    return lower_span_for_tree(context, name ? name : symbol->decl_node);
}

core_string_t
source_text_of(const core_translation_unit_t *unit, parse_tree_t *node)
{
    if (!node) return (core_string_t){0};
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_tree_terminal(node);
    if (!unit || !first || !last) return (core_string_t){0};
    core_string_t source = core_source_file_contents(&unit->source);
    size_t start = parse_token_start_byte(first);
    size_t end = parse_token_end_byte(last);
    if (!source.data || end <= start || end > source.u8_bytes
        || end - start > 60)
        return (core_string_t){0};
    core_string_t text = core_string_slice(source, start, end - start);
    for (size_t i = 0; i < text.u8_bytes; i++)
        if (text.data[i] == '\n' || text.data[i] == '\r')
            return (core_string_t){0};
    return text;
}

core_string_t
lower_type_text(ir_lower_context_t *context, th_type *type)
{
    th_ctx *types = context && context->module
        ? ir_module_types(context->module) : nullptr;
    if (!types || !type) return core_string_dup(CORE_STRING_STATIC("?"));
    core_string_t text = n00bb_print_type(types, type);
    return core_string_dup(text.data ? text : CORE_STRING_STATIC("?"));
}

static void
lower_type_text_pair(ir_lower_context_t *context, th_type *first,
                     th_type *second, core_string_t *first_text,
                     core_string_t *second_text)
{
    *first_text = lower_type_text(context, first);
    *second_text = lower_type_text(context, second);
    th_ctx *types = context && context->module
        ? ir_module_types(context->module) : nullptr;
    if (!types || !first || !second
        || !core_string_eq(*first_text, *second_text)) return;
    core_string_t first_qualified = n00bb_print_type_for_user(types, first);
    core_string_t second_qualified = n00bb_print_type_for_user(types, second);
    if (!first_qualified.data || !second_qualified.data) {
        core_free(first_qualified.data);
        core_free(second_qualified.data);
        return;
    }
    core_free(first_text->data);
    core_free(second_text->data);
    *first_text = first_qualified;
    *second_text = second_qualified;
}

parse_sym_entry_t *
lower_user_function(ir_lower_context_t *context)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_sym_entry_t *function = lower ? lower->function_symbol : nullptr;
    if (!function || core_string_starts_with(function->name,
                                             CORE_STRING_STATIC("__")))
        return nullptr;
    return function;
}

bool
report_uncaught_failure(ir_lower_context_t *context, ir_span_t span,
                        th_type *error_type, n00bb_text_t source)
{
    parse_sym_entry_t *function = lower_user_function(context);
    th_type *channel = function && function->type
        ? th_func_error(function->type) : nullptr;
    core_string_t error_text = lower_type_text(context, error_type);
    if (!function) {
        lower_report(context, N00B_DIAG_TOP_LEVEL_FAILURE, span,
            N00B_TEXT_CAN_FAIL_WITH_BUT_IT,
            N00BB_ARGS(ARG_PHRASE_ID(source), ARG_TEXT(error_text)));
        lower_help(context, N00B_TEXT_HANDLE_THE_FAILURE_WITH_TRY,
            N00BB_NO_ARGS);
    }
    else if (!channel || th_is_nil(channel))
        lower_report(context, N00B_DIAG_FAILURE_TYPE_MISMATCH, span,
            N00B_TEXT_CAN_FAIL_WITH_BUT_DECLARES,
            N00BB_ARGS(ARG_PHRASE_ID(source), ARG_TEXT(error_text),
                       ARG_TEXT(function->name)));
    else {
        core_string_t channel_text = {0};
        core_free(error_text.data);
        lower_type_text_pair(context, error_type, channel, &error_text,
                             &channel_text);
        lower_report(context, N00B_DIAG_FAILURE_TYPE_MISMATCH, span,
            N00B_TEXT_CAN_FAIL_WITH_BUT_FAILS,
            N00BB_ARGS(ARG_PHRASE_ID(source), ARG_TEXT(error_text),
                       ARG_TEXT(function->name), ARG_TEXT(channel_text)));
        core_free(channel_text.data);
    }
    if (function)
        lower_help(context, N00B_TEXT_HANDLE_THE_FAILURE_WITH_TRY_2,
                   N00BB_NO_ARGS);
    core_free(error_text.data);
    ir_instr_t *stop = ir_lower_emit_terminator(context, IR_OP_UNREACHABLE,
                                                span);
    context->reachable = false;
    return stop != nullptr;
}

core_string_t
mode_keyword(th_mode mode)
{
    switch (mode) {
    case TH_OUT: return CORE_STRING_STATIC("out");
    case TH_INOUT: return CORE_STRING_STATIC("modify");
    case TH_CONSUME: return CORE_STRING_STATIC("consume");
    case TH_VIEW: return CORE_STRING_STATIC("view");
    default: return CORE_STRING_STATIC("in");
    }
}

static n00bb_text_t
mode_parameter(th_mode mode)
{
    switch (mode) {
    case TH_OUT: return N00B_TEXT_OUT_PARAMETER;
    case TH_INOUT: return N00B_TEXT_MODIFY_PARAMETER;
    case TH_CONSUME: return N00B_TEXT_CONSUME_PARAMETER;
    case TH_VIEW: return N00B_TEXT_VIEW_PARAMETER;
    default: return N00B_TEXT_IN_PARAMETER;
    }
}

core_string_t
describe_parameter_target(th_mode mode, parse_sym_entry_t *callee,
                          core_string_t parameter)
{
    if (parameter.u8_bytes && callee)
        return n00bb_text(N00B_TEXT_PARAMETER_TARGET_NAMED,
            N00BB_ARGS(ARG_TEXT(mode_keyword(mode)), ARG_TEXT(parameter),
                       ARG_TEXT(callee->name)));
    if (callee)
        return n00bb_text(N00B_TEXT_PARAMETER_TARGET_OF,
            N00BB_ARGS(ARG_TEXT(mode_keyword(mode)), ARG_TEXT(callee->name)));
    return n00bb_text(N00B_TEXT_PARAMETER_TARGET,
                      N00BB_ARGS(ARG_TEXT(mode_keyword(mode))));
}

void
report_unwritable_output(ir_lower_context_t *context,
                         parse_grammar_t *grammar, parse_tree_t *argument,
                         th_mode mode, parse_sym_entry_t *callee,
                         core_string_t parameter)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
    parse_sym_entry_t *variable = lvalue && lower
        ? attached_symbol(grammar, lower->unit, lvalue) : nullptr;
    core_string_t target = describe_parameter_target(mode, callee, parameter);
    ir_span_t span = lower_span_for_tree(context, argument);
    if (variable && variable->kind == PARSE_SYM_PARAM
        && !(variable->has_mode && th_mode_produces_output(variable->mode))) {
        th_mode own = variable->has_mode ? variable->mode : TH_IN;
        lower_report(context, N00B_DIAG_OUTPUT_ARGUMENT_NOT_WRITABLE, span,
            N00B_TEXT_PARAMETER_CANNOT_BE_PASSED,
            N00BB_ARGS(ARG_TEXT(variable->name),
                       ARG_PHRASE_ID(mode_parameter(own)), ARG_TEXT(target)));
        lower_declared_secondary(context, variable, N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(variable->name)));
        lower_help(context, N00B_TEXT_DECLARE_AS_MODIFY_OR_COPY,
            N00BB_ARGS(ARG_TEXT(variable->name), ARG_TEXT(variable->name)));
    }
    else if (variable && (variable->mutability == PARSE_SYM_IMMUTABLE
                          || variable->mutability == PARSE_SYM_CONST)) {
        core_string_t kind = variable->mutability == PARSE_SYM_CONST
            ? CORE_STRING_STATIC("const") : CORE_STRING_STATIC("let");
        lower_report(context, N00B_DIAG_OUTPUT_ARGUMENT_NOT_WRITABLE, span,
            N00B_TEXT_IS_A_SO_IT_CANNOT,
            N00BB_ARGS(ARG_TEXT(variable->name), ARG_TEXT(kind),
                       ARG_TEXT(target)));
        lower_declared_secondary(context, variable, N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(variable->name)));
        lower_help(context, N00B_TEXT_DECLARE_WITH_VAR,
            N00BB_ARGS(ARG_TEXT(variable->name)));
    }
    else
        lower_report(context, N00B_DIAG_OUTPUT_ARGUMENT_NOT_WRITABLE, span,
            N00B_TEXT_ONLY_A_VARIABLE_FIELD_OR, N00BB_ARGS(ARG_TEXT(target)));
    core_free(target.data);
}


const core_string_t effects_subject_key =
    CORE_STRING_STATIC("n00bb.effects.subject");

const core_string_t effects_name_key =
    CORE_STRING_STATIC("n00bb.effects.name");

bool
effects_query_builtin(const parse_sym_entry_t *symbol)
{
    static const core_string_t queries[] = {
        CORE_STRING_STATIC("__function_has_effect"),
        CORE_STRING_STATIC("__function_reads"),
        CORE_STRING_STATIC("__function_writes"),
    };
    for (size_t q = 0; q < sizeof(queries) / sizeof(*queries); q++)
        if (core_string_eq(symbol->name, queries[q])) return true;
    return false;
}

bool
annotate_effects_query(ir_ctx_t *ctx, ir_instr_t *call, ir_value_t *subject,
                       core_string_t name)
{
    return (!subject || ir_annot_set(ctx, &call->annots, (ir_annot_t){
                .key = effects_subject_key, .kind = IR_ANNOT_VALUE,
                .v = subject}))
        && (!name.u8_bytes || ir_annot_set(ctx, &call->annots, (ir_annot_t){
                .key = effects_name_key, .kind = IR_ANNOT_STR,
                .s = ir_strdup(ctx, name)}));
}

ir_lower_result_t
lower_builtin_failure(ir_lower_context_t *context, bool valid, ir_span_t span,
                      parse_sym_entry_t *builtin, n00bb_text_t expectation)
{
    core_string_t name = builtin ? builtin->name
                                 : CORE_STRING_STATIC("?");
    if (valid) {
        core_buffer_t what = {0};
        core_buffer_printf(&what, "could not emit a call to `%.*s`",
                           (int)name.u8_bytes, name.data);
        ir_lower_result_t result = lower_internal_string(context, span,
            core_buffer_string(&what));
        core_buffer_release(&what);
        return result;
    }
    return lower_report(context, N00B_DIAG_BUILTIN_ARGUMENTS, span,
        N00B_TEXT_BUILTIN_EXPECTS,
        N00BB_ARGS(ARG_TEXT(name), ARG_PHRASE_ID(expectation)));
}

ir_value_t *
option_present(ir_lower_context_t *context, ir_value_t *option,
               th_type *bool_type, ir_span_t span)
{
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_OPTION_IS_NIL,
                                                 span);
    if (!test || !ir_lower_set_temp_dest(context, test,
                                         CORE_STRING_STATIC("__n00bb_"),
                                         bool_type)
        || !ir_instr_operand_value(test, option)) return nullptr;
    ir_instr_t *false_value = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                        span);
    if (!false_value || !ir_lower_set_temp_dest(context, false_value,
                                                CORE_STRING_STATIC("__n00bb_"),
                                                bool_type)
        || !ir_instr_operand_int(false_value, 0)) return nullptr;
    ir_instr_t *present = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    return present && ir_lower_set_temp_dest(context, present,
                                             CORE_STRING_STATIC("__n00bb_"),
                                             bool_type)
        && ir_instr_operand_value(present, test->dest)
        && ir_instr_operand_value(present, false_value->dest)
        ? present->dest : nullptr;
}

ir_value_t *
condition_value(ir_lower_context_t *context, parse_tree_t *node)
{
    ir_value_t *value = ir_lower_node_value(context, node);
    th_type *element = value ? th_option_item(value->type) : nullptr;
    if (!element
        || !th_zero_size(ir_module_types(context->module), element))
        return value;
    ir_value_t *present = option_present(context, value,
        th_name(ir_module_types(context->module), CORE_STRING_STATIC("bool")),
        lower_span_for_tree(context, node));
    return present ? present : value;
}

ir_lower_result_t
lower_condition_error(ir_lower_context_t *context, parse_tree_t *condition,
                      ir_value_t *value, n00bb_text_t construct)
{
    if (!value || !value->type)
        return lower_internal(context,
            lower_span_for_tree(context, condition),
            "a condition has no value");
    core_string_t type = lower_type_text(context, value->type);
    core_string_t text = source_text_of(lower_unit(context), condition);
    ir_span_t span = lower_span_for_tree(context, condition);
    if (text.u8_bytes)
        lower_report(context, N00B_DIAG_CONDITION_NOT_BOOL, span,
            N00B_TEXT_IS_BUT_MUST_BE_BOOL,
            N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(type),
                       ARG_PHRASE_ID(construct)));
    else
        lower_report(context, N00B_DIAG_CONDITION_NOT_BOOL, span,
            N00B_TEXT_THIS_EXPRESSION_IS_BUT_MUST,
            N00BB_ARGS(ARG_TEXT(type), ARG_PHRASE_ID(construct)));
    if (text.u8_bytes && th_option_item(value->type))
        lower_help(context, N00B_TEXT_TO_TEST_FOR_A_VALUE,
            N00BB_ARGS(ARG_TEXT(text)));
    else if (text.u8_bytes && is_integer_type(value->type))
        lower_help(context, N00B_TEXT_WRITE_THE_COMPARISON_YOU_MEAN,
            N00BB_ARGS(ARG_TEXT(text)));
    core_free(type.data);
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

n00bb_text_t
logical_construct(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (nt_is(grammar, node, CORE_STRING_STATIC("and-expr")))
        return N00B_TEXT_PHRASE_AN_OPERAND_OF_AND;
    if (nt_is(grammar, node, CORE_STRING_STATIC("or-expr")))
        return N00B_TEXT_PHRASE_AN_OPERAND_OF_OR;
    if (nt_is(grammar, node, CORE_STRING_STATIC("while-stmt")))
        return N00B_TEXT_PHRASE_A_WHILE_CONDITION;
    return N00B_TEXT_PHRASE_AN_IF_CONDITION;
}

bool
n00bb_mark_condition(ir_lower_context_t *context, n00bb_text_t construct,
                     parse_tree_t *condition)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *block = context->block;
    ir_instr_t *consumer = block && block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    if (!consumer) return false;
    consumer->span = lower_span_for_tree(context, condition);
    ir_value_t *tested = consumer->op == IR_OP_BRANCH && consumer->n_operands
        ? ir_operand_value(&consumer->operands[0]) : nullptr;
    th_type *truth = th_name(ir_module_types(context->module),
                             CORE_STRING_STATIC("bool"));
    if (tested && !ir_type_eq(context->module, tested->type, truth)
        && lower->unit && !lower->solved)
        core_list_push(tu_state(lower->unit)->condition_sites, consumer);
    return ir_annot_set(lower->ir_context, &consumer->annots,
        (ir_annot_t){.key = N00BB_CONDITION_KEY, .kind = IR_ANNOT_INT,
                     .i = ir_lit_of(construct)});
}

ir_block_t *
n00bb_branch_block_named(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower, core_string_t role)
{
    char buffer[64];
    for (;;) {
        core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
            "__n00bb_%.*s_%llu", (int)role.u8_bytes, role.data,
            (unsigned long long)lower->next_block_id++);
        if (!ir_block_find(context->function, name))
            return ir_block_add(context->function, name);
    }
}

bool
n00bb_atomic_native_support(ir_module_t *module, th_type *atomic_type,
                            const core_target_desc_t *target)
{
    bool handled = false;
    if (!module || !target || !n00bb_atomic_element_type(atomic_type)
        || !n00bb_abi_publish_direct_layout(module, atomic_type, target,
                                            &handled)
        || !handled
        || !ir_layout_compute(module, atomic_type, nullptr)) return false;
    ir_descriptor_t *layout = ir_layout_of(module, atomic_type,
                                           (core_string_t){});
    return layout && ir_layout_is_settled(&layout->size)
        && ir_layout_is_settled(&layout->align)
        && layout->size.value > 0
        && layout->size.value <= UINT32_MAX / 8
        && core_target_atomic_for_bits(
            target, (uint32_t)(layout->size.value * 8));
}

static ir_value_t *
n00bb_atomic_binding_address(ir_lower_context_t *context,
                             parse_sym_entry_t *symbol, ir_span_t span)
{
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    if (address) return address;
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    th_type *value_type = local ? local->type
        : binding ? binding->type : nullptr;
    if (!value_type || (!local && (!binding
            || (binding->kind != IR_VAL_PARAM
                && binding->kind != IR_VAL_GLOBAL)))) return nullptr;
    th_type *address_type = th_ind(ir_module_types(context->module), 0,
                                   value_type);
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                           IR_OP_ADDRESS_OF, span);
    if (!address_type || !instruction
        || !ir_lower_set_temp_dest(context, instruction,
                                   CORE_STRING_STATIC("__n00bb_atomic_address_"), address_type)
        || !(local ? ir_instr_operand_local(instruction, local)
                   : ir_instr_operand_value(instruction, binding)))
        return nullptr;
    return instruction->dest;
}

bool
n00bb_lower_atomic_reference(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    if (!entering) return true;
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = lower
        ? n00bb_atomic_load_symbol(visit->grammar, lower->unit, visit->node)
        : nullptr;
    if (!symbol) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *address = n00bb_atomic_binding_address(context, symbol, span);
    ir_value_t *value = address
        ? n00bb_emit_indirect_read(context, address, symbol->type, span)
        : nullptr;
    if (!value || !ir_lower_set_node_value(context, visit->node, value)) {
        lower_internal(context, span,
            "atomic variable has no addressable storage for an atomic load");
        return false;
    }
    return true;
}

ir_value_t *
n00bb_emit_local_read(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span)
{
    ir_value_t *address = ir_lower_local_address(context, local);
    if (address)
        return n00bb_emit_indirect_read(context, address, local->type, span);
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), local->type)
        || !ir_instr_operand_local(instruction, local)) return nullptr;
    return instruction->dest;
}

bool
n00bb_emit_sequence_shape_check_impl(ir_lower_context_t *context,
                                     ir_value_t *value, ir_shape_t *expected,
                                     ir_span_t span, bool force)
{
    if (!expected || !value) return true;
    bool constrained = false;
    bool opaque = false;
    for (uint32_t i = 0; i < ir_shape_rank(expected); i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(expected, i, &extent)) return false;
        bool unknown_runtime = extent.kind == IR_SHAPE_EXTENT_RUNTIME
            && (!extent.value || extent.value->owner != context->function);
        opaque |= unknown_runtime;
        constrained |= extent.kind != IR_SHAPE_EXTENT_UNRESOLVED
            && !unknown_runtime;
    }
    if (!constrained) return true;
    if (opaque) {
        n00bb_lower_context_t *lower = context->user;
        ir_shape_t *checkable = lower && lower->ir_context
            ? ir_shape_new(lower->ir_context,
                ir_shape_element_type(expected), ir_shape_rank(expected))
            : nullptr;
        if (!checkable) return false;
        for (uint32_t i = 0; i < ir_shape_rank(expected); i++) {
            ir_shape_extent_t extent;
            if (!ir_shape_extent(expected, i, &extent)) return false;
            if (extent.kind == IR_SHAPE_EXTENT_RUNTIME
                && (!extent.value
                    || extent.value->owner != context->function))
                extent = (ir_shape_extent_t){
                    .kind = IR_SHAPE_EXTENT_UNRESOLVED,
                };
            if (!ir_shape_set_extent(checkable, i, extent)) return false;
        }
        expected = checkable;
    }
    ir_shape_t *actual = ir_value_shape(value);
    if (actual == expected && !force) return true;
    ir_instr_t *check = ir_lower_emit_instruction(context,
                                     IR_OP_SEQUENCE_SHAPE_CHECK, span);
    if (!check || !ir_instr_operand_value(check, value)
        || !ir_instr_set_shape_constraint(check, expected)) {
        lower_internal(context, span,
                    "could not emit a sequence shape check");
        return false;
    }
    return true;
}

bool
n00bb_emit_sequence_shape_check(ir_lower_context_t *context,
                                ir_value_t *value, ir_shape_t *expected,
                                ir_span_t span)
{
    return n00bb_emit_sequence_shape_check_impl(context, value, expected,
                                                 span, false);
}

bool
n00bb_attach_expression_shape(ir_lower_context_t *context,
                              parse_grammar_t *grammar,
                              parse_tree_t *expression, ir_value_t *value)
{
    if (!context || !value || ir_value_shape(value)) return true;
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = attached_symbol(grammar,
        lower ? lower->unit : nullptr, expression);
    if (!symbol) return true;
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    ir_shape_t *shape = local ? ir_local_shape(local)
        : binding ? ir_value_shape(binding) : nullptr;
    if (!shape)
        shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    return !shape || ir_value_set_shape(value, shape);
}

bool
n00bb_emit_local_assign(ir_lower_context_t *context, ir_local_t *local,
                        ir_value_t *value, bool initializes, ir_span_t span)
{
    ir_instr_t *copy = local && value
        ? ir_lower_emit_instruction(context, IR_OP_COPY, span) : nullptr;
    return copy && ir_instr_set_binding_initialization(copy, initializes)
        && ir_instr_set_dest_local(copy, local)
        && ir_instr_operand_value(copy, value);
}

bool
n00bb_emit_local_write(ir_lower_context_t *context, ir_local_t *local,
                       ir_value_t *value, ir_span_t span)
{
    if (!context || !local || !value) return false;
    if (!n00bb_emit_sequence_shape_check(context, value,
            ir_local_shape(local), span)) return false;
    ir_value_t *address = ir_lower_local_address(context, local);
    return address ? n00bb_emit_indirect_write(context, address, value, span)
                   : n00bb_emit_local_assign(context, local, value, true,
                                             span);
}

ir_value_t *
n00bb_emit_indirect_read(ir_lower_context_t *context, ir_value_t *address,
                        th_type *type, ir_span_t span)
{
    if (!address || !type) return nullptr;
    th_type *atomic_element = n00bb_atomic_element_type(type);
    th_type *result_type = atomic_element ? atomic_element : type;
    if (!result_type) return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_LOAD,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(instruction, address)
        || (atomic_element
            && !ir_instr_add_mod(instruction, CORE_STRING_STATIC("atomic")))) return nullptr;
    if (th_kind_of(th_resolve(result_type)) == TH_INDIRECTION
        || n00bb_reference_definition(ir_module_types(context->module),
                                      result_type)) {
        n00bb_lower_context_t *lower = context->user;
        if (!lower || !ir_value_set_indirect_access(lower->ir_context,
                instruction->dest, IR_INDIRECT_ACCESS_READ_WRITE))
            return nullptr;
    }
    else if (n00bb_open_type(context, result_type))
        n00bb_rewrite_later(context, instruction);
    return instruction->dest;
}

ir_value_t *
n00bb_emit_atomic_wrap(ir_lower_context_t *context, ir_value_t *value,
                       th_type *atomic_type, ir_span_t span)
{
    if (!context || !value || !atomic_type) return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                                        IR_OP_ATOMIC_WRAP,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), atomic_type)
        || !ir_instr_operand_value(instruction, value)) return nullptr;
    return instruction->dest;
}

bool
n00bb_emit_store(ir_lower_context_t *context, ir_value_t *address,
                 ir_value_t *value, bool initializes, ir_span_t span)
{
    ir_instr_t *instruction = address && value
        ? ir_lower_emit_instruction(context, IR_OP_STORE, span) : nullptr;
    th_type *referent = address ? th_ind_referent(address->type) : nullptr;
    return instruction
        && (ir_value_addresses_sequence_contents(address)
            || ir_instr_set_binding_initialization(instruction, initializes))
        && ir_instr_operand_value(instruction, address)
        && ir_instr_operand_value(instruction, value)
        && (!n00bb_atomic_element_type(referent)
            || ir_instr_add_mod(instruction, CORE_STRING_STATIC("atomic")));
}

bool
n00bb_emit_indirect_write(ir_lower_context_t *context, ir_value_t *address,
                          ir_value_t *value, ir_span_t span)
{
    return n00bb_emit_store(context, address, value, true, span);
}

ir_instr_t *
n00bb_emit_success_return(ir_lower_context_t *context, ir_value_t *result,
                          const core_list_t(n00bb_copyout_t) *copyouts,
                          ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *result_symbol = lower && lower->function_symbol
        ? parse_sym_binding(lower->function_symbol, N00BB_RESULT_KEY)
        : nullptr;
    ir_local_t *result_local = result_symbol
        ? ir_lower_bound_local(context, result_symbol) : nullptr;
    if (result_local) {
        if (result
            && !n00bb_emit_local_write(context, result_local, result, span))
            return nullptr;
        result = n00bb_emit_local_read(context, result_local, span);
        if (!result) return nullptr;
    }
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    for (size_t i = 0; i < copyouts->len; i++) {
        ir_value_t *value = n00bb_emit_local_read(context,
            copyouts->data[i].local, span);
        if (!value || !lower || (value->def && !ir_annot_set(
                lower->ir_context, &value->def->annots, (ir_annot_t){
                    .key = CORE_STRING_STATIC("ir.copyout_read"),
                    .kind = IR_ANNOT_BOOL, .b = true}))) {
            core_list_free(values);
            return nullptr;
        }
        core_list_push(values, value);
    }

    ir_instr_t *ret = ir_lower_emit_terminator(context, IR_OP_RETURN, span);
    bool emitted = ret && (!result || ir_instr_operand_value(ret, result));
    for (size_t i = 0; emitted && i < values.len; i++)
        emitted = ir_instr_operand_value(ret, values.data[i]);
    core_list_free(values);
    if (!emitted) return nullptr;
    if (result) n00bb_rewrite_later(context, ret);
    return ret;
}

th_type *
n00bb_indirect_type(th_ctx *types, th_type *referent)
{
    return th_ind(types, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE,
                  referent);
}

bool
n00bb_set_reference_access(ir_ctx_t *context, ir_value_t *value,
                           th_type *type)
{
    th_ctx *types = value && value->owner && value->owner->owner
        ? ir_module_types(value->owner->owner) : nullptr;
    bool reference = type && (th_kind_of(th_resolve(type)) == TH_INDIRECTION
        || n00bb_reference_definition(types, type));
    return value && type
        && (!reference
            || ir_value_set_indirect_access(context, value,
                IR_INDIRECT_ACCESS_READ_WRITE));
}
