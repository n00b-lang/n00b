#include "call_plan.h"

#include "compiler_link.h"
#include "diagnostics.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

bool
n00bb_call_plan_init(n00bb_call_plan_t *plan, parse_grammar_t *grammar,
                     parse_sym_entry_t *callee, th_type *signature,
                     size_t offset)
{
    signature = signature ? th_resolve(signature) : nullptr;
    *plan = (n00bb_call_plan_t){
        .grammar = grammar,
        .callee = callee,
        .formals = callee ? callable_formal_parameters(grammar, callee)
                          : nullptr,
        .signature = signature,
        .offset = offset,
        .positional = core_list_new(n00bb_tree_ptr_t),
        .keywords = core_list_new(n00bb_tree_ptr_t),
        .rest = core_list_new(n00bb_tree_ptr_t),
    };
    if (!signature || th_kind_of(signature) != TH_FUNCTION) return false;
    plan->parameter_count = th_func_nparams(signature);
    plan->slot_count = plan->parameter_count + th_func_nkw(signature);
    plan->slots = core_alloc_array(n00bb_call_slot_t,
                                   plan->slot_count ? plan->slot_count : 1);
    plan->optional = core_alloc_array(bool, plan->parameter_count
                                            ? plan->parameter_count : 1);
    if (!plan->slots || !plan->optional) return false;
    for (size_t i = 0; i < plan->parameter_count; i++) {
        uint8_t flags = 0;
        plan->optional[i] = callee
            && function_parameter_info(grammar, callee, plan->formals, i,
                   nullptr, nullptr, &flags, nullptr)
            && (flags & 1u);
    }
    return true;
}

void
n00bb_call_plan_collect(n00bb_call_plan_t *plan, parse_tree_t *call_args)
{
    collect_positional_nodes(plan->grammar,
        child_nt(plan->grammar, call_args,
                 CORE_STRING_STATIC("positional-args")), &plan->positional);
    collect_keyword_nodes(plan->grammar,
        child_nt(plan->grammar, call_args, CORE_STRING_STATIC("keyword-args")),
        &plan->keywords);
}

void
n00bb_call_plan_free(n00bb_call_plan_t *plan)
{
    core_free(plan->slots);
    core_free(plan->optional);
    core_free(plan->argument_slots);
    core_list_free(plan->positional);
    core_list_free(plan->keywords);
    core_list_free(plan->rest);
    plan->slots = nullptr;
    plan->optional = nullptr;
    plan->argument_slots = nullptr;
}

th_type *
n00bb_call_plan_parameter_type(const n00bb_call_plan_t *plan, size_t index)
{
    if (index < plan->parameter_count) {
        const th_param *parameter = th_func_param(plan->signature, index);
        return parameter ? parameter->type : nullptr;
    }
    const th_kwparam *keyword = th_func_kw(plan->signature,
                                           index - plan->parameter_count);
    return keyword ? keyword->type : nullptr;
}

size_t
n00bb_call_plan_argument_count(const n00bb_call_plan_t *plan)
{
    return plan->positional.len + plan->keywords.len;
}

parse_tree_t *
n00bb_call_plan_argument(const n00bb_call_plan_t *plan, size_t position,
                         size_t *index)
{
    *index = plan->argument_slots ? plan->argument_slots[position] : SIZE_MAX;
    if (*index != SIZE_MAX) return plan->slots[*index].argument;
    return position < plan->positional.len
        ? plan->positional.data[position]
        : plan->keywords.data[position - plan->positional.len];
}

parse_tree_t *
n00bb_call_plan_parameter_argument(const n00bb_call_plan_t *plan, size_t index)
{
    return plan->slots && plan->status == N00BB_CALL_PLAN_OK
        && index < plan->slot_count ? plan->slots[index].argument : nullptr;
}

th_type *
n00bb_call_plan_formal_type(const n00bb_call_plan_t *plan, size_t index)
{
    if (index != SIZE_MAX) return n00bb_call_plan_parameter_type(plan, index);
    const th_param *rest = th_func_rest(plan->signature);
    return rest ? rest->type : nullptr;
}

bool
n00bb_call_plan_optional(const n00bb_call_plan_t *plan, size_t index)
{
    return index >= plan->parameter_count || plan->optional[index];
}

core_string_t
n00bb_call_plan_parameter_name(const n00bb_call_plan_t *plan, size_t index)
{
    if (index >= plan->parameter_count) {
        const th_kwparam *keyword = th_func_kw(plan->signature,
                                               index - plan->parameter_count);
        return keyword ? keyword->name : (core_string_t){0};
    }
    core_string_t name = {0};
    if (plan->callee)
        (void)function_parameter_info(plan->grammar, plan->callee,
            plan->formals, index, nullptr, &name, nullptr, nullptr);
    return name;
}

core_string_t
n00bb_call_plan_rest_name(const n00bb_call_plan_t *plan)
{
    if (!th_func_rest(plan->signature) || !plan->callee) return (core_string_t){0};
    parse_sym_entry_t *rest = plan->formals
        ? formal_rest_symbol(plan->grammar, plan->formals) : nullptr;
    if (rest) return rest->name;
    core_string_t name = {0};
    uint8_t flags = 0;
    if (!plan->formals
        && function_interface_parameter(plan->callee, plan->parameter_count,
               &name, &flags, nullptr) && (flags & 2u))
        return name;
    return (core_string_t){0};
}

static bool
fail(n00bb_call_plan_t *plan, n00bb_call_plan_status_t status,
     parse_tree_t *problem, size_t index)
{
    plan->status = status;
    plan->problem = problem;
    plan->problem_index = index;
    return false;
}

bool
n00bb_call_plan_bind(n00bb_call_plan_t *plan)
{
    if (!plan->slots) return false;
    size_t count = n00bb_call_plan_argument_count(plan);
    plan->argument_slots = core_alloc_array(size_t, count ? count : 1);
    if (!plan->argument_slots) return false;
    for (size_t i = 0; i < count; i++) plan->argument_slots[i] = SIZE_MAX;
    size_t next = plan->offset;
    size_t by_position = 0;
    for (size_t i = 0; i < plan->positional.len; i++) {
        while (next < plan->parameter_count
               && n00bb_call_plan_optional(plan, next)) next++;
        parse_tree_t *argument = plan->positional.data[i];
        if (next < plan->parameter_count) {
            plan->argument_slots[i] = next;
            plan->slots[next++] = (n00bb_call_slot_t){argument, argument,
                                                       false};
            by_position++;
        }
        else if (th_func_rest(plan->signature))
            core_list_push(plan->rest, argument);
        else return fail(plan, N00BB_CALL_PLAN_TOO_MANY, argument, i);
    }
    core_string_t varargs = n00bb_call_plan_rest_name(plan);
    parse_tree_t *named_positional = nullptr;
    size_t named_index = 0;
    for (size_t i = 0; i < plan->keywords.len; i++) {
        parse_tree_t *keyword = plan->keywords.data[i];
        core_string_t name = call_keyword_name(keyword);
        size_t index = 0;
        if (varargs.data && core_string_eq(name, varargs))
            return fail(plan, N00BB_CALL_PLAN_VARARGS_BY_NAME, keyword, 0);
        if (!plan->callee || !function_parameter_named(plan->grammar,
                plan->callee, plan->formals, name, &index)
            || index >= plan->slot_count)
            return fail(plan, N00BB_CALL_PLAN_UNKNOWN_NAME, keyword, 0);
        if (index < plan->offset || plan->slots[index].site)
            return fail(plan, N00BB_CALL_PLAN_REPEATED, keyword, index);
        plan->argument_slots[plan->positional.len + i] = index;
        plan->slots[index] = (n00bb_call_slot_t){
            child_nt(plan->grammar, keyword, CORE_STRING_STATIC("expression")),
            keyword, true};
        if (!named_positional && !n00bb_call_plan_optional(plan, index)) {
            named_positional = keyword;
            named_index = index;
        }
    }
    if (by_position && named_positional)
        return fail(plan, N00BB_CALL_PLAN_MIXED_STYLES, named_positional,
                    named_index);
    for (size_t i = plan->offset; i < plan->parameter_count; i++)
        if (!plan->slots[i].site && !n00bb_call_plan_optional(plan, i))
            return fail(plan, N00BB_CALL_PLAN_MISSING, nullptr, i);
    plan->status = N00BB_CALL_PLAN_OK;
    return true;
}

bool
n00bb_plan_call(n00bb_call_plan_t *plan, parse_grammar_t *grammar,
                parse_sym_entry_t *callee, th_type *signature,
                parse_tree_t *call_args, size_t offset)
{
    if (!n00bb_call_plan_init(plan, grammar, callee, signature, offset))
        return false;
    n00bb_call_plan_collect(plan, call_args);
    return n00bb_call_plan_bind(plan);
}

static core_string_t
callee_display_name(parse_sym_entry_t *callee)
{
    return callee && callee->name.u8_bytes && callee->name.data[0] != '$'
        ? callee->name : (core_string_t){0};
}

static size_t
positional_parameter_count(const n00bb_call_plan_t *plan)
{
    size_t count = 0;
    for (size_t i = plan->offset; i < plan->parameter_count; i++)
        if (!n00bb_call_plan_optional(plan, i)) count++;
    return count;
}

static void
report_arity(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
             core_diagnostics_t *diagnostics, n00bb_diag_t id,
             core_diag_location_t at)
{
    size_t required = positional_parameter_count(plan);
    core_string_t count = n00bb_text(th_func_rest(plan->signature)
            ? N00B_TEXT_AT_LEAST_ARGUMENTS
            : id == N00B_DIAG_CALL_TOO_MANY_ARGUMENTS
                ? N00B_TEXT_POSITIONAL_ARGUMENT_COUNT
                : N00B_TEXT_ARGUMENT_COUNT,
        N00BB_ARGS(ARG_NUMBER(required)));
    core_string_t name = callee_display_name(plan->callee);
    size_t passed = plan->positional.len
        + (id == N00B_DIAG_CALL_TOO_MANY_ARGUMENTS ? 0 : plan->keywords.len);
    n00bb_report(diagnostics, unit, id, at,
        name.u8_bytes ? N00B_TEXT_TAKES_BUT_CALL_PASSES
                      : N00B_TEXT_FUNCTION_TAKES_BUT_CALL_PASSES,
        N00BB_ARGS(ARG_TEXT(count), ARG_NUMBER(passed), ARG_TEXT(name)));
    core_free(count.data);
}

static void
declared_here(const n00bb_call_plan_t *plan, core_diagnostics_t *diagnostics)
{
    if (plan->callee)
        n00bb_diag_declared_here(diagnostics, plan->callee,
                                 N00B_TEXT_IS_DECLARED_HERE);
}

static void
report_too_many(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
                core_diagnostics_t *diagnostics)
{
    size_t first = plan->problem_index;
    core_diag_location_t at = n00bb_node_span(plan->positional.data[first]);
    core_diag_location_t last = n00bb_node_span(
        plan->positional.data[plan->positional.len - 1]);
    at.end_line = last.end_line;
    at.end_column = last.end_column;
    size_t extra = plan->positional.len - first;
    report_arity(plan, unit, diagnostics, N00B_DIAG_CALL_TOO_MANY_ARGUMENTS,
                 at);
    n00bb_report_label(diagnostics, N00B_TEXT_UNEXPECTED_ARGUMENTS,
                       N00BB_ARGS(ARG_NUMBER(extra)));
    declared_here(plan, diagnostics);
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    for (size_t i = plan->offset; i < plan->slot_count; i++)
        if (n00bb_call_plan_optional(plan, i) && !plan->slots[i].site) {
            core_string_t name = n00bb_call_plan_parameter_name(plan, i);
            if (name.u8_bytes) core_list_push(names, name);
        }
    if (names.len < extra) {
        n00bb_report_help(diagnostics, N00B_TEXT_REMOVE_EXTRA_ARGUMENTS,
                          N00BB_ARGS(ARG_NUMBER(extra)));
        core_list_free(names);
        return;
    }
    n00bb_report_help(diagnostics, N00B_TEXT_KEYWORD_PARAMETERS_BY_NAME,
                      N00BB_NO_ARGS);
    n00bb_edit_t *edits = core_alloc_array(n00bb_edit_t, extra);
    core_list_t(core_string_t) owned = core_list_new(core_string_t);
    bool placed = true;
    for (size_t i = 0; placed && i < extra; i++) {
        size_t start = 0, end = 0;
        placed = n00bb_node_bytes(plan->positional.data[first + i], &start,
                                  &end);
        core_buffer_t *prefix = core_buffer_empty();
        core_buffer_append_string(prefix, names.data[i]);
        core_buffer_puts(prefix, " = ");
        core_list_push(owned, core_buffer_take_string(prefix));
        edits[i] = (n00bb_edit_t){start, start, owned.data[i]};
    }
    if (placed) n00bb_report_fix(diagnostics, unit, edits, extra);
    for (size_t i = 0; i < owned.len; i++) core_free(owned.data[i].data);
    core_list_free(owned);
    core_list_free(names);
    core_free(edits);
}

static void
report_missing(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
               core_diagnostics_t *diagnostics, parse_tree_t *site)
{
    core_list_t(core_string_t) missing = core_list_new(core_string_t);
    for (size_t i = plan->offset; i < plan->parameter_count; i++) {
        if (plan->slots[i].site || n00bb_call_plan_optional(plan, i)) continue;
        core_string_t name = n00bb_call_plan_parameter_name(plan, i);
        core_list_push(missing, name.u8_bytes
            ? n00bb_text(N00B_TEXT_QUOTED_NAME, N00BB_ARGS(ARG_TEXT(name)))
            : n00bb_text(N00B_TEXT_PARAMETER_NUMBER,
                         N00BB_ARGS(ARG_NUMBER(i - plan->offset + 1))));
    }
    report_arity(plan, unit, diagnostics, N00B_DIAG_CALL_MISSING_ARGUMENT,
                 n00bb_node_span(site));
    n00bb_report_label(diagnostics, N00B_TEXT_MISSING,
                       N00BB_ARGS(ARG_LIST(missing.data, missing.len)));
    for (size_t i = 0; i < missing.len; i++) core_free(missing.data[i].data);
    core_list_free(missing);
    declared_here(plan, diagnostics);
}

static void
report_unknown(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
               core_diagnostics_t *diagnostics)
{
    core_string_t name = call_keyword_name(plan->problem);
    core_string_t callee = callee_display_name(plan->callee);
    if (callee.u8_bytes)
        n00bb_report(diagnostics, unit, N00B_DIAG_CALL_UNKNOWN_NAMED_ARGUMENT,
            link_source_location(plan->problem),
            N00B_TEXT_HAS_NO_PARAMETER_NAMED,
            N00BB_ARGS(ARG_TEXT(callee), ARG_TEXT(name)));
    else
        n00bb_report(diagnostics, unit, N00B_DIAG_CALL_UNKNOWN_NAMED_ARGUMENT,
            link_source_location(plan->problem),
            N00B_TEXT_THIS_FUNCTION_HAS_NO_PARAMETER,
            N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_label(diagnostics, N00B_TEXT_UNKNOWN_PARAMETER, N00BB_NO_ARGS);
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    for (size_t i = plan->offset; i < plan->slot_count; i++) {
        core_string_t candidate = n00bb_call_plan_parameter_name(plan, i);
        if (candidate.u8_bytes) core_list_push(names, candidate);
    }
    if (!n00bb_diag_did_you_mean(diagnostics, name, names.data, names.len)
        && names.len)
        n00bb_report_note(diagnostics, N00B_TEXT_THE_PARAMETERS_ARE,
            N00BB_ARGS(ARG_NAMES(names.data, names.len)));
    core_list_free(names);
    declared_here(plan, diagnostics);
}

static void
report_repeated(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
                core_diagnostics_t *diagnostics)
{
    core_string_t name = call_keyword_name(plan->problem);
    n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_REPEATED,
        link_source_location(plan->problem),
        N00B_TEXT_PARAMETER_IS_GIVEN_AN_ARGUMENT, N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_label(diagnostics, N00B_TEXT_SECOND_ARGUMENT_FOR,
                       N00BB_ARGS(ARG_TEXT(name)));
    parse_tree_t *first = plan->problem_index < plan->slot_count
        ? plan->slots[plan->problem_index].site : nullptr;
    if (first)
        n00bb_report_secondary(diagnostics, nullptr, n00bb_node_span(first),
            N00B_TEXT_IS_FIRST_GIVEN_HERE, N00BB_ARGS(ARG_TEXT(name)));
}

static core_string_t
arguments_text(const n00bb_call_plan_t *plan, const core_translation_unit_t *unit,
               bool by_name)
{
    core_buffer_t *text = core_buffer_empty();
    for (size_t i = plan->offset; i < plan->parameter_count; i++) {
        if (n00bb_call_plan_optional(plan, i)) continue;
        if (text->byte_len) core_buffer_puts(text, ", ");
        if (by_name) {
            core_buffer_append_string(text,
                n00bb_call_plan_parameter_name(plan, i));
            core_buffer_puts(text, " = ");
        }
        core_buffer_append_string(text, n00bb_node_source_text(unit,
            plan->slots[i].argument, (core_string_t){0}));
    }
    for (size_t i = 0; i < plan->keywords.len; i++) {
        size_t index = 0;
        if (function_parameter_named(plan->grammar, plan->callee,
                plan->formals, call_keyword_name(plan->keywords.data[i]),
                &index) && !n00bb_call_plan_optional(plan, index))
            continue;
        if (text->byte_len) core_buffer_puts(text, ", ");
        core_buffer_append_string(text, n00bb_node_source_text(unit,
            plan->keywords.data[i], (core_string_t){0}));
    }
    return core_buffer_take_string(text);
}

static void
report_mixed_styles(const n00bb_call_plan_t *plan,
                    const core_translation_unit_t *unit,
                    core_diagnostics_t *diagnostics)
{
    core_string_t name = call_keyword_name(plan->problem);
    if (!n00bb_report(diagnostics, unit, N00B_DIAG_CALL_MIXED_ARGUMENT_STYLES,
            link_source_location(plan->problem),
            N00B_TEXT_MIXED_ARGUMENT_STYLES,
            N00BB_ARGS(ARG_TEXT(name),
                       ARG_TEXT(callee_display_name(plan->callee)))))
        return;
    core_diag_mark(diagnostics, n00bb_node_span(plan->positional.data[0]));
    n00bb_report_help(diagnostics, N00B_TEXT_MIXED_ARGUMENT_STYLES_HELP,
                      N00BB_NO_ARGS);
    for (size_t i = plan->offset; i < plan->parameter_count; i++)
        if (!plan->slots[i].site && !n00bb_call_plan_optional(plan, i))
            return;
    size_t start = SIZE_MAX, end = 0;
    const core_list_t(n00bb_tree_ptr_t) *groups[] = {&plan->positional,
                                                     &plan->keywords};
    for (size_t g = 0; g < 2; g++)
        for (size_t i = 0; i < groups[g]->len; i++) {
            size_t first = 0, last = 0;
            if (!n00bb_node_bytes(groups[g]->data[i], &first, &last)) return;
            if (first < start) start = first;
            if (last > end) end = last;
        }
    for (int by_name = 0; by_name < 2; by_name++) {
        core_string_t text = arguments_text(plan, unit, by_name);
        n00bb_report_fix(diagnostics, unit,
                         &(n00bb_edit_t){start, end, text}, 1);
        core_free(text.data);
    }
}

static void
report_varargs_by_name(const n00bb_call_plan_t *plan,
                       const core_translation_unit_t *unit,
                       core_diagnostics_t *diagnostics)
{
    if (!n00bb_report(diagnostics, unit, N00B_DIAG_CALL_VARARGS_BY_NAME,
            link_source_location(plan->problem), N00B_TEXT_VARARGS_BY_NAME,
            N00BB_ARGS(ARG_TEXT(call_keyword_name(plan->problem)),
                       ARG_TEXT(callee_display_name(plan->callee)))))
        return;
    n00bb_report_help(diagnostics, N00B_TEXT_VARARGS_BY_NAME_HELP,
                      N00BB_NO_ARGS);
    declared_here(plan, diagnostics);
}

void
n00bb_call_plan_report(const n00bb_call_plan_t *plan,
                       const core_translation_unit_t *unit,
                       core_diagnostics_t *diagnostics, parse_tree_t *site)
{
    switch (plan->status) {
    case N00BB_CALL_PLAN_OK: return;
    case N00BB_CALL_PLAN_TOO_MANY:
        report_too_many(plan, unit, diagnostics);
        return;
    case N00BB_CALL_PLAN_UNKNOWN_NAME:
        report_unknown(plan, unit, diagnostics);
        return;
    case N00BB_CALL_PLAN_VARARGS_BY_NAME:
        report_varargs_by_name(plan, unit, diagnostics);
        return;
    case N00BB_CALL_PLAN_REPEATED:
        report_repeated(plan, unit, diagnostics);
        return;
    case N00BB_CALL_PLAN_MIXED_STYLES:
        report_mixed_styles(plan, unit, diagnostics);
        return;
    case N00BB_CALL_PLAN_MISSING:
        report_missing(plan, unit, diagnostics, site);
        return;
    }
}
