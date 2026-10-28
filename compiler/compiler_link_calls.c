#include <string.h>

#include "call_plan.h"
#include "compiler_link.h"
#include "compiler_tu.h"
#include "diagnostics.h"
#include "parse/tree_util.h"

typedef struct {
    parse_grammar_t *grammar;
} module_inference_context_t;

static bool
dereference_would_fit(th_ctx *types, th_type *value, th_type *wanted)
{
    value = value ? th_resolve(value) : nullptr;
    wanted = wanted ? th_resolve(wanted) : nullptr;
    if (!value || !wanted || th_kind_of(value) != TH_INDIRECTION
        || th_kind_of(wanted) == TH_INDIRECTION) return false;
    th_type *referent = th_resolve(th_ind_referent(value));
    th_type *element = th_option_item(wanted);
    return th_unify_probe(types, referent, wanted)
        || (is_integer_type(referent) && is_integer_type(wanted))
        || (element && th_unify_probe(types, referent, element));
}

static void
add_dereference_help(core_diagnostics_t *diagnostics, th_ctx *types,
                     th_type *value, th_type *wanted)
{
    if (dereference_would_fit(types, value, wanted))
        n00bb_report_help(diagnostics,
            N00B_TEXT_READ_THE_REFERENCED_VALUE_WITH, N00BB_NO_ARGS);
}

static core_string_t
callee_display_name(parse_sym_entry_t *callee)
{
    return callee && callee->name.u8_bytes && callee->name.data[0] != '$'
        ? callee->name : (core_string_t){0};
}

void
n00bb_report_argument_type(core_translation_unit_t *unit, th_ctx *types,
                           parse_tree_t *argument, th_type *actual,
                           th_type *expected, parse_sym_entry_t *callee,
                           size_t position, core_string_t parameter)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t actual_text = n00bb_print_type(types, actual);
    core_string_t expected_text = n00bb_print_type(types, expected);
    core_string_t name = callee_display_name(callee);
    n00bb_text_t subject = parameter.u8_bytes
        ? name.u8_bytes ? N00B_TEXT_PARAMETER_NAMED_OF
                        : N00B_TEXT_PARAMETER_NAMED
        : name.u8_bytes ? N00B_TEXT_PARAMETER_NUMBER_OF
                        : N00B_TEXT_PARAMETER_NUMBER;
    core_string_t subject_text = n00bb_text(subject,
        N00BB_ARGS(ARG_NUMBER(position + 1), ARG_TEXT(parameter),
                   ARG_TEXT(name)));
    n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
        n00bb_node_span(argument), N00B_TEXT_IS_BUT_THIS_ARGUMENT_IS,
        N00BB_ARGS(ARG_TEXT(subject_text), ARG_TEXT(expected_text),
                   ARG_TEXT(actual_text)));
    core_free(subject_text.data);
    n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
        N00BB_ARGS(ARG_TEXT(expected_text)));
    if (callee) n00bb_diag_declared_here(diagnostics, callee,
        N00B_TEXT_IS_DECLARED_HERE);
    add_dereference_help(diagnostics, types, actual, expected);
}

static void
report_varargs_array(core_translation_unit_t *unit, th_ctx *types,
                     parse_tree_t *argument, th_type *expected,
                     parse_sym_entry_t *callee, core_string_t parameter)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t expected_text = n00bb_print_type(types, expected);
    n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
        n00bb_node_span(argument), N00B_TEXT_VARARGS_GIVEN_ARRAY,
        N00BB_ARGS(ARG_TEXT(callee_display_name(callee)),
                   ARG_TEXT(parameter)));
    n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
        N00BB_ARGS(ARG_TEXT(expected_text)));
    n00bb_report_help(diagnostics, N00B_TEXT_VARARGS_GIVEN_ARRAY_HELP,
                      N00BB_NO_ARGS);
    n00bb_diag_declared_here(diagnostics, callee, N00B_TEXT_IS_DECLARED_HERE);
}

void
report_call_type_error(core_translation_unit_t *unit, parse_tree_t *call,
                       th_ctx *types, th_type *actual, th_type *expected,
                       n00bb_call_role_t role)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t actual_text = actual ? n00bb_print_type(types, actual)
                                       : CORE_STRING_STATIC("unknown");
    core_string_t expected_text = expected
        ? n00bb_print_type(types, expected) : CORE_STRING_STATIC("unknown");
    int actual_size = (int)actual_text.u8_bytes;
    int expected_size = (int)expected_text.u8_bytes;
    core_string_t text = n00bb_node_source_text(unit, call,
        n00bb_text_form(N00B_TEXT_THIS_VALUE));
    core_diag_location_t at = n00bb_node_span(call);
    switch (role) {
    case N00BB_CALL_ARGUMENT:
        n00bb_report_argument_type(unit, types, call, actual, expected,
                                   nullptr, 0, (core_string_t){0});
        return;
    case N00BB_CALL_RECEIVER:
        n00bb_report(diagnostics, unit, N00B_DIAG_METHOD_RECEIVER_TYPE, at,
                     N00B_TEXT_THIS_METHOD_TAKES_BEFORE_THE,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(expected_text.data,
                (size_t)(expected_size))), ARG_TEXT(text),
            ARG_TEXT(core_string_borrow(actual_text.data,
                                        (size_t)(actual_size)))));
        n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(expected_text.data,
                                                   (size_t)(expected_size)))));
        add_dereference_help(diagnostics, types, actual, expected);
        return;
    case N00BB_CALL_RESULT:
        n00bb_report(diagnostics, unit, N00B_DIAG_CALL_RESULT_TYPE, at,
                     N00B_TEXT_THIS_CALL_RETURNS_BUT_ITS,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(expected_text.data,
                (size_t)(expected_size))),
            ARG_TEXT(core_string_borrow(actual_text.data,
                                        (size_t)(actual_size)))));
        n00bb_report_label(diagnostics, N00B_TEXT_RETURNS,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(expected_text.data,
                                                   (size_t)(expected_size)))));
        add_dereference_help(diagnostics, types, expected, actual);
        return;
    case N00BB_CALL_SIGNATURE:
        n00bb_report(diagnostics, unit, N00B_DIAG_TRAIT_SIGNATURE_MISMATCH, at,
                     N00B_TEXT_THE_IMPLEMENTATION_CHOSEN_FOR_THIS,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(expected_text.data,
                (size_t)(expected_size))),
            ARG_TEXT(core_string_borrow(actual_text.data,
                                        (size_t)(actual_size)))));
        return;
    }
}

bool
unify_call_node_type(core_translation_unit_t *unit,
                     n00bb_tu_state_t *state, parse_tree_t *diagnostic_node,
                     parse_tree_t *typed_node, th_type *actual,
                     th_type *expected, n00bb_call_role_t role)
{
    if (!actual || !expected) return true;
    if (n00bb_value_coerces(state->types, expected, actual)) {
        parse_tree_set_type(typed_node, expected);
        return true;
    }
    bool integer_result = role == N00BB_CALL_RESULT
        && is_integer_type(actual) && is_integer_type(expected)
        && th_hash(state->types, actual) != th_hash(state->types, expected);
    bool converted = false;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    size_t reported = core_diagnostics_count(diagnostics);
    th_type *materialized = n00bb_type_materialize(state->link_analysis
            ? state->link_analysis->grammar : nullptr,
        state, typed_node, integer_result ? expected : actual,
        integer_result ? actual : expected, &converted);
    if (!materialized && core_diagnostics_count(diagnostics) == reported)
        report_call_type_error(unit, diagnostic_node, state->types, actual,
                               expected, role);
    if (materialized && integer_result)
        parse_tree_set_type(typed_node, expected);
    return materialized != nullptr;
}

void
n00bb_report_conversion_error(core_translation_unit_t *unit,
                              parse_tree_t *site, th_ctx *types,
                              th_type *value, th_type *target)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t actual_text = n00bb_print_type(types, value);
    core_string_t expected_text = n00bb_print_type(types, target);
    core_string_t text = n00bb_node_source_text(unit, site,
        n00bb_text_form(N00B_TEXT_THIS_VALUE));
    n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
        n00bb_node_span(site), N00B_TEXT_IS_WHICH_DOES_NOT_CONVERT,
        N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(actual_text),
                   ARG_TEXT(expected_text)));
    n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
        N00BB_ARGS(ARG_TEXT(expected_text)));
    add_dereference_help(diagnostics, types, value, target);
}

bool
n00bb_check_deferred_conversions(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return true;
    size_t failures = core_diagnostics_failures(
        core_translation_unit_diagnostics(unit));
    for (size_t i = 0; i < state->deferred_conversions.len; i++) {
        n00bb_deferred_conversion_t *pending =
            &state->deferred_conversions.data[i];
        th_type *value = th_resolve(pending->value);
        th_type *target = th_resolve(pending->target);
        if (!th_is_closed(state->types, target)) continue;
        if (!th_is_closed(state->types, value)) {
            if (n00bb_open_integer(value) && is_integer_type(target))
                n00bb_integer_boundary(state, pending->site, target);
            (void)th_unify_in_place(state->types, value, target);
            continue;
        }
        bool converted = false;
        if (n00bb_type_materialize(state->link_analysis
                ? state->link_analysis->grammar : nullptr, state,
                pending->site, value, target, &converted)) continue;
        n00bb_report_conversion_error(unit, pending->site, state->types,
                                      value, target);
    }
    state->deferred_conversions.len = 0;
    return core_diagnostics_failures(core_translation_unit_diagnostics(unit))
        == failures;
}

bool
dotted_receiver_prefix_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;) {
        parse_tree_t *parent = parse_tree_parent(at);
        if (!parent || core_tree_is_leaf(parent)) return false;
        if (dotted_symbol_path_node(grammar, parent))
            return core_tree_child(parent, 0) == at;
        if (core_tree_num_children(parent) != 1
            || core_tree_child(parent, 0) != at) return false;
        at = parent;
    }
    return false;
}

bool n00bb_is_trait_dispatch_reference(parse_grammar_t *grammar,
                                       core_translation_unit_t *unit,
                                       parse_tree_t *node);

typedef struct {
    core_translation_unit_t *unit;
    parse_cf_label_t *label;
} n00bb_call_site_t;
core_list_decl(n00bb_call_site_t);

typedef struct {
    th_type *source;
    th_type *instance;
    bool channels;
    core_string_t requirement_trait;
} n00bb_call_signature_t;

typedef struct {
    core_dict_t by_caller;
    core_dict_t instantiated_signatures;
} n00bb_call_site_index_t;

static th_type *
call_site_signature_instance(n00bb_call_site_index_t *index,
                             parse_cf_label_t *label, th_ctx *types,
                             th_type *source, bool channels,
                             core_string_t requirement_trait)
{
    if (!source) return nullptr;
    n00bb_call_signature_t *cached = core_dict_get(
        &index->instantiated_signatures, label, nullptr);
    /* Lantern returns a fresh method requirement signature on every query.
     * Its trait name identifies the same frozen contract across inference
     * rounds; using the fresh pointer as the cache key would create another
     * type variable and rebind the receiver on every round. */
    bool same_requirement = requirement_trait.data && cached
        && cached->requirement_trait.data
        && core_string_eq(cached->requirement_trait, requirement_trait);
    if (cached && cached->channels == channels
        && (same_requirement
            || (!requirement_trait.data && !cached->requirement_trait.data
                && cached->source == source)))
        return cached->instance;
    th_type *instance = nullptr;
    if (channels) {
        if (!th_env_signature_channels(types, source, &instance, nullptr))
            return nullptr;
    }
    else instance = th_type_clone_fresh(types, source);
    if (!instance) return nullptr;
    if (cached) {
        cached->source = source;
        cached->instance = instance;
        cached->channels = channels;
        cached->requirement_trait = requirement_trait;
    }
    else {
        cached = core_alloc(n00bb_call_signature_t);
        if (!cached) return nullptr;
        *cached = (n00bb_call_signature_t){
            .source = source,
            .instance = instance,
            .channels = channels,
            .requirement_trait = requirement_trait,
        };
        core_dict_put(&index->instantiated_signatures, label, cached);
    }
    return instance;
}

static th_type *
call_site_signature(n00bb_call_site_index_t *index, parse_cf_label_t *label,
                    th_ctx *types, th_type *source)
{
    return call_site_signature_instance(index, label, types, source, false,
                                        (core_string_t){0});
}

static th_type *
call_site_requirement_signature(n00bb_call_site_index_t *index,
                                parse_cf_label_t *label, th_ctx *types,
                                th_type *source, core_string_t trait_name)
{
    return call_site_signature_instance(index, label, types, source, false,
                                        trait_name);
}

static th_type *
call_site_channels(n00bb_call_site_index_t *index, parse_cf_label_t *label,
                   th_ctx *types, th_type *source)
{
    return call_site_signature_instance(index, label, types, source, true,
                                        (core_string_t){0});
}

static void
call_site_index_init(n00bb_call_site_index_t *index,
                     parse_grammar_t *grammar,
                     core_translation_unit_t *const *members,
                     size_t member_count)
{
    core_dict_init(&index->by_caller, nullptr, nullptr);
    core_dict_init(&index->instantiated_signatures, nullptr, nullptr);
    for (size_t i = 0; i < member_count; i++) {
        n00bb_tu_state_t *state = tu_state(members[i]);
        if (!state || !state->annotations) continue;
        for (size_t label_index = 0;
             label_index < state->annotations->cf_label_order.len;
             label_index++) {
            parse_cf_label_t *label =
                state->annotations->cf_label_order.data[label_index];
            if (!label || label->kind != PARSE_CF_CALL || !label->cond
                || core_tree_is_leaf(label->cond)) continue;
            parse_sym_entry_t *caller = enclosing_function_symbol(grammar,
                                                                   label->self);
            if (!caller) continue;
            core_list_t(n00bb_call_site_t) *sites = core_dict_get(
                &index->by_caller, caller, nullptr);
            if (!sites) {
                sites = core_alloc(core_list_t(n00bb_call_site_t));
                core_dict_put(&index->by_caller, caller, sites);
            }
            core_list_push(*sites, ((n00bb_call_site_t){members[i], label}));
        }
    }
}

static void
call_site_index_free(n00bb_call_site_index_t *index)
{
    core_dict_foreach(&index->by_caller, entry) {
        core_list_t(n00bb_call_site_t) *sites = entry->value;
        core_list_free(*sites);
        core_free(sites);
    }
    core_dict_free(&index->by_caller);
    core_dict_foreach(&index->instantiated_signatures, entry)
        core_free(entry->value);
    core_dict_free(&index->instantiated_signatures);
}

static bool
function_reaches(parse_grammar_t *grammar,
                 n00bb_call_site_index_t *index,
                 parse_sym_entry_t *from, parse_sym_entry_t *target,
                 core_dict_t *visited)
{
    if (from == target) return true;
    if (!core_dict_add(visited, from, from)) return false;

    core_list_t(n00bb_call_site_t) *sites = core_dict_get(
        &index->by_caller, from, nullptr);
    for (size_t i = 0; sites && i < sites->len; i++) {
        parse_cf_label_t *label = sites->data[i].label;
        parse_sym_entry_t *callee = bound_function_symbol(label->cond);
        if (callee && function_reaches(grammar, index, callee, target,
                                       visited)) return true;
    }
    return false;
}

static bool
call_is_recursive(parse_grammar_t *grammar,
                  n00bb_call_site_index_t *index, parse_sym_entry_t *caller,
                  parse_sym_entry_t *callee)
{
    if (!caller || !callee) return false;
    core_dict_t visited;
    core_dict_init(&visited, nullptr, nullptr);
    bool recursive = function_reaches(grammar, index, callee, caller,
                                      &visited);
    core_dict_free(&visited);
    return recursive;
}


static void
rebind_call_node(n00bb_tu_state_t *state, parse_tree_t *node,
                 parse_sym_entry_t *from, parse_sym_entry_t *to)
{
    parse_sym_entry_t *origin = canonical_symbol(from);
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && canonical_symbol(resolution.target.symbol) == origin) {
        parse_sym_entry_t *bound = resolution.target.symbol;
        parse_tree_set_resolution_symbol(node, to);
        if (parse_sym_remove_use(bound, node)) (void)parse_sym_add_use(to, node);
    }
    if (parse_tree_sym(node) && canonical_symbol(parse_tree_sym(node)) == origin)
        parse_tree_set_sym(node, to);
    n00bb_link_use_site_t *site = link_use_site_for_node(state, node);
    if (site && site->symbol && canonical_symbol(site->symbol) == origin)
        link_use_site_bind(site, to);
}

static void
rebind_call_callee(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                   parse_tree_t *call, parse_cf_label_t *label,
                   parse_sym_entry_t *from, parse_sym_entry_t *to)
{
    for (parse_tree_t *at = label->cond; at && !core_tree_is_leaf(at);
         at = core_tree_num_children(at) == 1 ? core_tree_child(at, 0)
                                               : nullptr)
        rebind_call_node(state, at, from, to);
    parse_tree_t *member = member_access_node(grammar, label->cond);
    if (member) rebind_call_node(state, member, from, to);
    if (call && !core_tree_is_leaf(call)) rebind_call_node(state, call, from, to);
}

static void
unify_call_argument(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    n00bb_tu_state_t *state, parse_sym_entry_t *callee,
                    const n00bb_call_binding_t *binding, bool context_call)
{
    parse_tree_t *argument = binding->argument;
    th_type *expected = binding->expected;
    bool writes = binding->writes;
    th_type *actual = parse_tree_type(argument);
    if (writes && is_integer_type(expected)) {
        parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
        parse_sym_entry_t *symbol = lvalue
            ? attached_symbol(grammar, unit, lvalue) : nullptr;
        if (symbol && n00bb_open_integer(symbol->type))
            (void)th_ground_integer(state->types, symbol->type, expected);
        if (n00bb_open_integer(actual)
            && th_ground_integer(state->types, actual, expected)) {
            parse_tree_set_type(argument, expected);
            return;
        }
    }
    bool fits = actual && expected
        && !(context_call
             && n00bb_value_coerces(state->types, actual, expected));
    if (fits && n00bb_value_coerces(state->types, expected, actual))
        parse_tree_set_type(argument, expected);
    else if (actual && expected) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        size_t reported = core_diagnostics_count(diagnostics);
        bool converted = false;
        th_type *materialized = fits
            ? n00bb_type_materialize(grammar, state, argument, actual,
                                     expected, &converted)
            : nullptr;
        bool unreported = !materialized
            && core_diagnostics_count(diagnostics) == reported;
        n00bb_static_literal_probe_t literal = find_static_literal(grammar,
                                                                   argument);
        bool whole_array = th_kind_of(th_resolve(actual)) == TH_SEQUENCE
            || (literal.valid
                && nt_is(grammar, literal.literal,
                         CORE_STRING_STATIC("list-lit")));
        if (unreported && binding->rest && whole_array
            && callee_display_name(callee).u8_bytes)
            report_varargs_array(unit, state->types, argument, expected,
                                 callee, binding->parameter_name);
        else if (unreported)
            n00bb_report_argument_type(unit, state->types, argument, actual,
                expected, callee, binding->parameter,
                binding->parameter_name);
    }
    (void)n00bb_contextual_float_literal(argument, expected);
}

static void
check_call_argument_mode(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         n00bb_tu_state_t *state, parse_tree_t *argument,
                         th_mode mode)
{
    if (!argument || mode == TH_VIEW) return;
    parse_tree_t *root = mode == TH_CONSUME
        ? n00bb_addressable_root_lvalue(grammar, argument) : nullptr;
    parse_sym_entry_t *root_symbol = root
        ? attached_symbol(grammar, unit, root) : nullptr;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (root_symbol
        && root_symbol->kind == PARSE_SYM_PARAM
        && th_mode_is_in(root_symbol->has_mode ? root_symbol->mode : TH_IN)) {
        bool view = root_symbol->has_mode && root_symbol->mode == TH_VIEW;
        n00bb_report(diagnostics, unit, N00B_DIAG_CONSUME_BORROWED_PARAMETER,
            n00bb_node_span(argument), N00B_TEXT_CONSUME_BORROWED,
            N00BB_ARGS(ARG_TEXT(root_symbol->name),
                       ARG_PHRASE_ID(view ? N00B_TEXT_VIEW_PARAMETER
                                          : N00B_TEXT_IN_PARAMETER)));
        n00bb_report_label(diagnostics, N00B_TEXT_BORROWED_NOT_OWNED,
            N00BB_NO_ARGS);
        n00bb_diag_declared_here(diagnostics, root_symbol,
            N00B_TEXT_IS_DECLARED_HERE);
        n00bb_report_help(diagnostics, N00B_TEXT_PASS_A_COPY_OR_DECLARE,
            N00BB_ARGS(ARG_TEXT(root_symbol->name)));
        return;
    }
    th_type *type = parse_tree_type(argument);
    if (!type || !n00bb_unit_type_reaches_shared_state(state, type))
        return;
    parse_sym_entry_t *in_parameter = nullptr;
    parse_sym_entry_t *origin = n00bb_view_origin(grammar, unit, argument,
                                                  &in_parameter);
    if (in_parameter
        && !parse_sym_binding(in_parameter, n00bb_view_blocked_binding))
        parse_sym_set_binding(in_parameter, n00bb_view_blocked_binding,
                              in_parameter);
    if (!origin) return;
    n00bb_report(diagnostics, unit, N00B_DIAG_VIEW_VALUE_TO_NON_VIEW,
        n00bb_node_span(argument), N00B_TEXT_THIS_VALUE_IS_REACHED_THROUGH,
        N00BB_ARGS(ARG_TEXT(origin->name)));
    n00bb_report_label(diagnostics, N00B_TEXT_READ_ONLY_THROUGH,
        N00BB_ARGS(ARG_TEXT(origin->name)));
    n00bb_diag_declared_here(diagnostics, origin, N00B_TEXT_IS_DECLARED_HERE);
}



bool
n00bb_bind_call_arguments(parse_grammar_t *grammar,
                          core_translation_unit_t *unit,
                          parse_sym_entry_t *callee, th_type *signature,
                          parse_tree_t *call_args, size_t offset,
                          parse_tree_t *site,
                          core_list_t(n00bb_call_binding_t) *bindings)
{
    signature = signature ? th_resolve(signature) : nullptr;
    if (!signature || th_kind_of(signature) != TH_FUNCTION) return true;
    n00bb_call_plan_t plan;
    if (!n00bb_plan_call(&plan, grammar, callee, signature, call_args,
                         offset)) {
        n00bb_call_plan_report(&plan, unit,
            core_translation_unit_diagnostics(unit), site);
        n00bb_call_plan_free(&plan);
        return false;
    }
    const th_param *rest = th_func_rest(signature);
    size_t rest_position = 0;
    for (size_t i = 0; i < n00bb_call_plan_argument_count(&plan); i++) {
        size_t index = 0;
        parse_tree_t *argument = n00bb_call_plan_argument(&plan, i, &index);
        size_t position = index == SIZE_MAX
            ? plan.parameter_count - offset + rest_position++ : index - offset;
        if (!argument || core_tree_is_leaf(argument)) continue;
        const th_param *parameter = index == SIZE_MAX ? rest
            : index < plan.parameter_count ? th_func_param(signature, index)
            : nullptr;
        const th_kwparam *option = parameter ? nullptr
            : th_func_kw(signature, index - plan.parameter_count);
        th_mode mode = parameter ? parameter->mode : option->mode;
        core_list_push(*bindings, ((n00bb_call_binding_t){
            argument, parameter ? parameter->type : option->type,
            index != SIZE_MAX && plan.slots[index].by_name,
            th_mode_produces_output(mode), mode, position,
            index == SIZE_MAX ? n00bb_call_plan_rest_name(&plan)
                              : n00bb_call_plan_parameter_name(&plan, index),
            index == SIZE_MAX}));
    }
    n00bb_call_plan_free(&plan);
    return true;
}

static void
argument_types_text(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                    parse_cf_label_t *label, core_buffer_t *out)
{
    core_list_t(n00bb_tree_ptr_t) arguments =
        core_list_new(n00bb_tree_ptr_t);
    if (label->then_body)
        collect_positional_nodes(grammar,
            child_nt(grammar, label->then_body,
                     CORE_STRING_STATIC("positional-args")), &arguments);
    core_buffer_putc(out, '(');
    for (size_t i = 0; i < arguments.len; i++) {
        th_type *type = parse_tree_type(arguments.data[i]);
        core_string_t text = type ? n00bb_print_type(state->types, type)
                                  : CORE_STRING_STATIC("_");
        core_buffer_printf(out, "%s%.*s", i ? ", " : "",
                           (int)text.u8_bytes, text.data);
    }
    core_buffer_putc(out, ')');
    core_list_free(arguments);
}

static void
report_overload_failure(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        n00bb_tu_state_t *state, parse_cf_label_t *label,
                        parse_sym_entry_t *callee, n00bb_call_lookup_t lookup,
                        const n00bb_overload_choice_t *choice)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t at = n00bb_node_span(label->self);
    int name_size = (int)callee->name.u8_bytes;
    const char *name = callee->name.data;
    core_buffer_t arguments = {0};
    argument_types_text(grammar, state, label, &arguments);
    const core_list_t(parse_sym_entry_ptr_t) *listed = &choice->matches;
    core_list_t(parse_sym_entry_ptr_t) candidates = {0};
    switch (choice->status) {
    case N00BB_OVERLOAD_PENDING:
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_NEEDS_TYPE, at,
                     N00B_TEXT_CANNOT_CHOOSE_AN_OVERLOAD_OF,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(name,
                                                   (size_t)(name_size)))));
        n00bb_report_label(diagnostics, N00B_TEXT_ARGUMENTS_ARE,
            N00BB_ARGS(ARG_TEXT(core_buffer_string(&arguments))));
        n00bb_report_help(diagnostics, N00B_TEXT_ANNOTATE_THE_ARGUMENT_OR_THE,
            N00BB_NO_ARGS);
        candidates = n00bb_call_overload_candidates(state, callee, lookup);
        listed = &candidates;
        break;
    case N00BB_OVERLOAD_NO_MATCH:
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_NO_MATCH, at,
                     N00B_TEXT_NO_OVERLOAD_OF_ACCEPTS_ARGUMENTS,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(name, (size_t)(name_size))),
                       ARG_TEXT(core_buffer_string(&arguments))));
        candidates = n00bb_call_overload_candidates(state, callee, lookup);
        listed = &candidates;
        break;
    case N00BB_OVERLOAD_CONFLICT:
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_FROM_SEVERAL_MODULES,
                     at, N00B_TEXT_NAMES_FUNCTIONS_FROM_DIFFERENT_MODULES,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(name,
                                                   (size_t)(name_size)))));
        n00bb_report_help(diagnostics, N00B_TEXT_CALL_THE_ONE_YOU_MEAN,
            N00BB_NO_ARGS);
        break;
    default:
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_AMBIGUOUS, at,
                     N00B_TEXT_MORE_THAN_ONE_OVERLOAD_OF,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(name, (size_t)(name_size))),
                       ARG_TEXT(core_buffer_string(&arguments))));
        n00bb_report_help(diagnostics, N00B_TEXT_ANNOTATE_AN_ARGUMENT_WITH_THE,
            N00BB_NO_ARGS);
        break;
    }
    core_buffer_release(&arguments);
    for (size_t i = 0; i < listed->len; i++) {
        parse_sym_entry_t *candidate = listed->data[i];
        core_diag_location_t where = n00bb_symbol_location(candidate);
        core_string_t text = n00bb_describe_candidate(grammar, state,
                                                      candidate);
        parse_sym_entry_t *origin = canonical_symbol(candidate);
        if (where.has_location)
            n00bb_report_secondary(diagnostics, origin->owner_unit, where,
                N00B_TEXT_CANDIDATE_IS_DECLARED_HERE,
                N00BB_ARGS(ARG_TEXT(text)));
        else
            n00bb_report_note(diagnostics, N00B_TEXT_CANDIDATE,
                N00BB_ARGS(ARG_TEXT(text)));
        core_free(text.data);
    }
    core_list_free(candidates);
}

static void
settle_failed_call(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                   core_translation_unit_t *unit, parse_cf_label_t *label,
                   size_t failures)
{
    if (!label || core_diagnostics_failures(
            core_translation_unit_diagnostics(unit)) <= failures) return;
    core_dict_put(&state->failed_calls, label, label);
    n00bb_poison_node(state, label->self);
    parse_sym_entry_t *caller = enclosing_function_symbol(grammar,
                                                          label->self);
    th_type *contract = caller ? (caller->callable_contract
        ? caller->callable_contract : caller->type) : nullptr;
    if (contract && !th_is_closed(state->types, contract)) {
        caller->callable_contract = th_poison(state->types);
        caller->type = caller->callable_contract;
    }
}

static bool
callee_unresolved(n00bb_tu_state_t *state, parse_tree_t *callee)
{
    for (parse_tree_t *at = callee; at; at = core_tree_is_leaf(at)
             || !core_tree_num_children(at) ? nullptr : core_tree_child(at, 0)) {
        n00bb_link_use_site_t *use = link_use_site_for_node(state, at);
        if (use) return use->unresolved;
    }
    return false;
}

static void
unify_call_results(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   core_link_unit_t *link,
                   n00bb_call_site_index_t *call_sites,
                   bool resolve_member_calls,
                   bool deferred_trait_requirements_only,
                   bool force_overloads, size_t *deferred)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->types) return;
    if (resolve_member_calls
        && !th_ctx_set_environment_scope(state->types,
            unit->module.identifier, n00bb_package_id(unit))) {
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
            (core_diag_location_t){}, "checking calls",
            "could not select the trait implementations visible to module "
            "`%.*s`", (int)unit->module.identifier.u8_bytes,
            unit->module.identifier.data);
        return;
    }

    parse_cf_label_t *pending = nullptr;
    size_t failures = 0;
    for (size_t label_index = 0;
         label_index < state->annotations->cf_label_order.len;
         label_index++) {
        settle_failed_call(grammar, state, unit, pending, failures);
        pending = nullptr;
        parse_cf_label_t *label =
            state->annotations->cf_label_order.data[label_index];
        if (!label) continue;
        if (label->kind != PARSE_CF_CALL || !label->cond ||
            core_tree_is_leaf(label->cond)) continue;
        if (core_dict_contains(&state->failed_calls, label)) continue;

        parse_tree_t *member = member_access_node(grammar, label->cond);
        if ((member != nullptr) != resolve_member_calls) continue;
        pending = label;
        failures = core_diagnostics_failures(
            core_translation_unit_diagnostics(unit));
        parse_sym_entry_t *callee_symbol = bound_function_symbol(label->cond);
        if (deferred_trait_requirements_only) {
            n00bb_link_use_site_t *site = member
                ? link_use_site_for_node(state, member) : nullptr;
            core_link_reference_t *reference = site ? site->reference : nullptr;
            if (!site || !site->function_owner || !site->symbol
                || !site->symbol->owner_type || !reference
                || !core_link_reference_is_deferred(reference)) continue;
        }
        parse_sym_entry_t *context_target =
            nt_is(grammar, label->self, CORE_STRING_STATIC("context-call"))
                ? attached_symbol(grammar, unit, label->cond) : nullptr;
        bool context_call = context_symbol(grammar, context_target);
        bool free_function = !member || call_receiver_is_module(grammar,
            unit, core_tree_child(member, 0));
        n00bb_link_use_site_t *callee_site = member
            ? link_use_site_for_node(state, member) : nullptr;
        n00bb_call_lookup_t lookup = {
            .unqualified = !member,
            .origin_module = callee_site ? callee_site->origin_module
                                         : (core_string_t){0},
            .name = member ? (core_string_t){0} : first_token_text(label->cond),
        };
        if (callee_symbol && !context_call && free_function
            && n00bb_function_has_overloads(state, callee_symbol, lookup)) {
            n00bb_overload_choice_t choice = n00bb_select_call_overload(
                grammar, state, label, callee_symbol, lookup);
            if (choice.status == N00BB_OVERLOAD_PENDING
                && !force_overloads) {
                n00bb_overload_choice_release(&choice);
                (*deferred)++;
                continue;
            }
            if (choice.status != N00BB_OVERLOAD_SELECTED) {
                report_overload_failure(grammar, unit, state, label,
                                        callee_symbol, lookup, &choice);
                n00bb_overload_choice_release(&choice);
                continue;
            }
            n00bb_overload_choice_release(&choice);
            n00bb_node_facts_t *facts = choice.passed_over_conversion
                ? n00bb_ensure_node_facts(state, label->self) : nullptr;
            if (facts && !facts->overload_conversion_warning_reported) {
                facts->overload_conversion_warning_reported = true;
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                parse_sym_entry_t *chosen = choice.symbol
                    ? choice.symbol : callee_symbol;
                core_string_t chosen_text = n00bb_describe_candidate(grammar,
                    state, chosen);
                n00bb_report(diagnostics, unit,
                    N00B_DIAG_OVERLOAD_PREFERS_EXACT,
                    n00bb_node_span(label->self),
                    N00B_TEXT_THIS_CALL_USES_WHICH_TAKES,
                    N00BB_ARGS(ARG_TEXT(chosen_text)));
                core_free(chosen_text.data);
                n00bb_diag_declared_here(diagnostics, chosen,
                    N00B_TEXT_IS_DECLARED_HERE);
            }
            if (choice.symbol && choice.symbol != callee_symbol) {
                rebind_call_callee(grammar, state, label->self, label,
                                   callee_symbol, choice.symbol);
                callee_symbol = choice.symbol;
            }
            if (!n00bb_resolve_overload_reference(grammar, link, state,
                    member ? member : label->cond, callee_symbol)) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, n00bb_node_span(label->self), "choosing overloads",
                    "could not connect this call to the chosen overload of "
                    "`%.*s`", (int)callee_symbol->name.u8_bytes,
                    callee_symbol->name.data);
                continue;
            }
        }
        parse_sym_entry_t *caller_symbol = enclosing_function_symbol(
            grammar, label->self);
        bool member_call = member && !context_call
            && (!callee_symbol
                || n00bb_is_trait_dispatch_reference(grammar, unit, member));
        th_type *signature = context_call ? context_target->type
            : callee_symbol ? callee_symbol->type
            : parse_tree_type(label->cond);
        if (th_is_poison(signature)
            || callee_unresolved(state, label->cond)) {
            th_type *result = parse_tree_type(label->self);
            if (result) (void)th_unify_in_place(state->types, result,
                                                th_poison(state->types));
            continue;
        }
        bool signature_instantiated = false;
        parse_sym_entry_t *keyword_callee = nullptr;
        if (!member_call && callee_symbol
            && callee_symbol->callable_contract) {
            signature = call_site_channels(call_sites, label,
                state->types, callee_symbol->callable_contract);
            signature_instantiated = signature != nullptr;
        }
        if (member_call) {
            parse_tree_t *receiver_node = core_tree_child(member, 0);
            core_string_t method_text = first_token_text(
                core_tree_child(member, 2));
            n00bb_link_use_site_t *member_site = link_use_site_for_node(
                state, member);
            th_type *receiver_type = receiver_node
                ? parse_tree_type(receiver_node) : nullptr;
            if (th_is_poison(receiver_type)) {
                th_type *result = parse_tree_type(label->self);
                if (result) (void)th_unify_in_place(state->types, result,
                                                    receiver_type);
                continue;
            }
            th_type *caller_contract = caller_symbol
                ? (caller_symbol->callable_contract
                    ? caller_symbol->callable_contract
                    : caller_symbol->type) : nullptr;
            bool generic_open_receiver = caller_contract
                && receiver_type
                && !th_is_closed(state->types, caller_contract)
                && !th_is_closed(state->types, receiver_type);
            if (member_site && !member_site->callback_call
                && n00bb_callback_function_type(grammar, unit, member)
                && !n00bb_bind_callback_call(state, member_site)) continue;
            if (member_site && member_site->callback_call
                && member_site->symbol) {
                th_type *contract = member_site->symbol->callable_contract
                    ? member_site->symbol->callable_contract
                    : member_site->symbol->type;
                signature = call_site_signature(call_sites, label,
                    state->types, contract);
                signature_instantiated = signature != nullptr;
                callee_symbol = member_site->symbol;
            }
            else if (generic_open_receiver) {
                size_t requirement_count = method_text.data
                    ? th_env_method_requirements(state->types, method_text,
                                                  nullptr, 0) : 0;
                th_env_method_requirement_t *requirements =
                    requirement_count ? core_alloc_array(
                        th_env_method_requirement_t, requirement_count)
                        : nullptr;
                if (requirement_count && !requirements) {
                    n00bb_internal_error(
                        core_translation_unit_diagnostics(unit), unit,
                        n00bb_node_span(member), "checking calls",
                        "could not list the traits that declare `%.*s`",
                        (int)method_text.u8_bytes, method_text.data);
                    continue;
                }
                if (requirement_count)
                    th_env_method_requirements(state->types, method_text,
                        requirements, requirement_count);
                size_t matched = SIZE_MAX;
                parse_sym_entry_t *matched_requirement = nullptr;
                n00bb_requirement_fit_t best = N00BB_REQUIREMENT_DOES_NOT_FIT;
                bool ambiguous = false;
                size_t sole = SIZE_MAX, named_requirements = 0;
                parse_sym_entry_t *sole_requirement = nullptr;
                for (size_t r = 0; r < requirement_count; r++) {
                    parse_sym_entry_t *requirement =
                        trait_method_requirement_symbol(grammar,
                            state->link_analysis, unit,
                            requirements[r].trait_name,
                            method_text);
                    if (requirement && requirement != sole_requirement) {
                        sole_requirement = requirement;
                        sole = r;
                        named_requirements++;
                    }
                    n00bb_requirement_fit_t fit = has_trait_constraint(
                            receiver_type, requirements[r].trait_name)
                        ? N00BB_REQUIREMENT_FITS_EXACTLY
                        : has_deferred_method_constraint(receiver_type,
                                                         method_text)
                        ? n00bb_method_requirement_fit(grammar, state->types,
                            receiver_type, method_text,
                            requirements[r].signature, requirement)
                        : N00BB_REQUIREMENT_DOES_NOT_FIT;
                    if (fit == N00BB_REQUIREMENT_DOES_NOT_FIT || fit < best)
                        continue;
                    ambiguous = fit == best;
                    best = fit;
                    matched = r;
                    matched_requirement = requirement;
                }
                if (ambiguous) {
                    matched = SIZE_MAX;
                    matched_requirement = nullptr;
                }
                if (best == N00BB_REQUIREMENT_DOES_NOT_FIT
                    && named_requirements == 1
                    && has_deferred_method_constraint(receiver_type,
                                                      method_text)) {
                    matched = sole;
                    matched_requirement = sole_requirement;
                }
                if (!callee_symbol) keyword_callee = matched_requirement;
                signature = matched != SIZE_MAX
                    ? call_site_requirement_signature(call_sites, label,
                        state->types, requirements[matched].signature,
                        requirements[matched].trait_name)
                    : nullptr;
                signature_instantiated = signature != nullptr;
                n00bb_link_use_site_t *site = link_use_site_for_node(
                    state, member);
                if (site && matched != SIZE_MAX)
                    site->resolved_trait_name =
                        requirements[matched].trait_name;
                core_free(requirements);
            }
            else if (receiver_type && method_text.data) {
                core_string_t method_name = method_text;
                th_env_method_t method = {0};
                core_string_t trait_name = {0};
                th_env_method_status_t method_status =
                    th_env_method_resolve_by_name(state->types,
                        receiver_type, method_name, &method, &trait_name);
                th_type *promoted = method_status == TH_ENV_METHOD_NOT_FOUND
                    ? n00bb_dereferenced_type(state->types, receiver_type)
                    : nullptr;
                if (promoted && promoted != th_resolve(receiver_type)) {
                    method_status = th_env_method_resolve_by_name(
                        state->types, promoted, method_name, &method,
                        &trait_name);
                    if (method_status == TH_ENV_METHOD_FOUND)
                        receiver_type = promoted;
                }
                bool blanket = method_status == TH_ENV_METHOD_FOUND
                    && n00bb_blanket_receiver(method.contract);
                th_type *coerced = method_status == TH_ENV_METHOD_NOT_FOUND
                    || blanket
                    ? n00bb_value_coercion_target(state->types, receiver_type)
                    : nullptr;
                if (coerced) {
                    th_env_method_t target = {0};
                    core_string_t target_trait = {0};
                    th_env_method_status_t target_status =
                        th_env_method_resolve_by_name(state->types, coerced,
                            method_name, &target, &target_trait);
                    if (target_status == TH_ENV_METHOD_FOUND
                        && !(blanket
                             && n00bb_blanket_receiver(target.contract))) {
                        method_status = target_status;
                        method = target;
                        trait_name = target_trait;
                        receiver_type = coerced;
                    }
                }
                if (method_status == TH_ENV_METHOD_FOUND) {
                    signature = call_site_signature(call_sites, label,
                        state->types, method.signature);
                    signature_instantiated = signature != nullptr;
                    n00bb_link_use_site_t *site = link_use_site_for_node(
                        state, member);
                    if (site) site->resolved_trait_name = trait_name;
                    callee_symbol = resolve_linked_method(link, site, unit,
                                                          &method);
                    th_type *target_contract = callee_symbol
                        ? (callee_symbol->callable_contract
                            ? callee_symbol->callable_contract
                            : callee_symbol->type) : nullptr;
                    if (target_contract) {
                        th_type *target_signature = call_site_channels(call_sites,
                            label, state->types, target_contract);
                        if (target_signature) {
                            signature = target_signature;
                            signature_instantiated = true;
                        }
                        else {
                            n00bb_internal_error(
                                core_translation_unit_diagnostics(unit), unit,
                                n00bb_node_span(member), "checking calls",
                                "the implementation of `%.*s` has no "
                                "signature", (int)method_name.u8_bytes,
                                method_name.data);
                            continue;
                        }
                    }
                }
                else if (receiver_type &&
                         th_kind_of(th_resolve(receiver_type)) != TH_VAR)
                    n00bb_report_missing_method(unit, state->types, member,
                        receiver_type, method_text,
                        method_status == TH_ENV_METHOD_AMBIGUOUS);
            }
        }
        th_type *result = label->self ? parse_tree_type(label->self) : nullptr;
        if (!signature || !result) continue;
        bool recursive = call_is_recursive(grammar, call_sites,
                                           caller_symbol, callee_symbol);
        if (!signature_instantiated) {
            if (callee_symbol && !recursive)
                signature = call_site_signature(call_sites, label,
                    state->types, signature);
        }
        size_t parameter_offset = member_call ? 1 : 0;
        if (member_call) {
            const th_param *receiver = th_func_param(signature, 0);
            parse_tree_t *receiver_node = core_tree_child(member, 0);
            th_type *receiver_type = receiver_node
                ? parse_tree_type(receiver_node) : nullptr;
            if (!receiver || !unify_call_node_type(unit, state, receiver_node,
                    receiver_node, receiver_type, receiver->type,
                    N00BB_CALL_RECEIVER))
                continue;
            check_call_argument_mode(grammar, unit, state, receiver_node,
                                     receiver->mode);
        }
        parse_sym_entry_t *named = context_call ? context_target
            : keyword_callee ? keyword_callee
            : member_call ? callee_symbol : canonical_symbol(callee_symbol);
        core_list_t(n00bb_call_binding_t) bindings =
            core_list_new(n00bb_call_binding_t);
        (void)n00bb_bind_call_arguments(grammar, unit, named, signature,
            label->then_body, parameter_offset, label->self, &bindings);
        for (size_t i = 0; i < bindings.len; i++) {
            n00bb_call_binding_t *binding = &bindings.data[i];
            unify_call_argument(grammar, unit, state, named, binding,
                                context_call);
            check_call_argument_mode(grammar, unit, state,
                                     binding->argument, binding->mode);
        }
        core_list_free(bindings);
        th_type *return_type = th_func_ret(signature);
        if (return_type && result
            && th_kind_of(th_resolve(result)) == TH_VAR) {
            if (!th_unify_in_place(state->types, result, return_type))
                report_call_type_error(unit, label->self, state->types,
                                       result, return_type,
                                       N00BB_CALL_RESULT);
        }
        else if (return_type)
            (void)unify_call_node_type(unit, state, label->self, label->self,
                                       result, return_type,
                                       N00BB_CALL_RESULT);
        th_type *failure_type = th_func_error(signature);
        bool propagated = failure_type
            && n00bb_call_is_error_propagated(grammar, label->self);
        parse_tree_t *clause = propagated
            ? n00bb_try_handler_clause(grammar, label->self) : nullptr;
        parse_sym_entry_t *handler = n00bb_try_clause_symbol(grammar, clause);
        if (handler) {
            th_type *bound = handler->type
                ? th_unify_in_place(state->types, handler->type, failure_type)
                : failure_type;
            if (bound) handler->type = bound;
        }
        else if (propagated && !clause && caller_symbol
            && caller_symbol->error_type) {
            th_type *caller_failure = th_type_clone(
                state->types, caller_symbol->error_type);
            if (caller_failure)
                failure_type = th_unify_in_place(
                    state->types, failure_type, caller_failure);
        }
        if (failure_type)
            (void)parse_tree_set_error_type(label->self, failure_type);
    }
    settle_failed_call(grammar, state, unit, pending, failures);
}

static core_buffer_t
call_type_snapshot(core_translation_unit_t *const *units,
                   size_t unit_count)
{
    core_buffer_t snapshot = {0};
    for (size_t i = 0; i < unit_count; i++) {
        n00bb_tu_state_t *state = tu_state(units[i]);
        if (!state || !state->types || !state->annotations) continue;
        core_buffer_t roots = {0};
        for (size_t label_index = 0;
             label_index < state->annotations->cf_label_order.len;
             label_index++) {
            parse_cf_label_t *label =
                state->annotations->cf_label_order.data[label_index];
            if (!label || label->kind != PARSE_CF_CALL) continue;
            parse_tree_t *nodes[] = {label->cond, label->self};
            for (size_t n = 0; n < 2; n++) {
                parse_tree_t *node = nodes[n];
                th_type *type = node ? parse_tree_type(node) : nullptr;
                uint64_t identity = type ? th_hash(state->types, type) : 0;
                core_buffer_append(&snapshot, (const char *)&identity,
                                   sizeof(identity));
                core_buffer_append(&roots, (const char *)&type,
                                   sizeof(type));
            }
        }
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION) continue;
                uint64_t identity = symbol->type
                    ? th_hash(state->types, symbol->type) : 0;
                core_buffer_append(&snapshot, (const char *)&identity,
                                   sizeof(identity));
                core_buffer_append(&roots, (const char *)&symbol->type,
                                   sizeof(symbol->type));
            }
        }
        core_list_free(scopes);
        uint64_t revision = th_type_graph_revision(
            (th_type *const *)roots.data,
            roots.byte_len / sizeof(th_type *));
        core_buffer_append(&snapshot, (const char *)&revision,
                           sizeof(revision));
        core_buffer_release(&roots);
    }
    return snapshot;
}

bool
call_units_failed(core_translation_unit_t *const *units, size_t unit_count)
{
    for (size_t i = 0; i < unit_count; i++)
        if (units[i] && core_diagnostics_failed(
                core_translation_unit_diagnostics(units[i])))
            return true;
    return false;
}

static size_t
call_unit_internal_errors(core_translation_unit_t *const *units,
                          size_t unit_count)
{
    size_t internal = 0;
    for (size_t i = 0; i < unit_count; i++)
        if (units[i])
            internal += core_diagnostics_count_severity(
                core_translation_unit_diagnostics(units[i]),
                CORE_DIAG_INTERNAL);
    return internal;
}

bool
unify_calls_until_stable(parse_grammar_t *grammar, core_link_unit_t *link,
                         core_translation_unit_t *const *units,
                         size_t unit_count, bool resolve_ordinary_calls,
                         bool resolve_member_calls,
                         bool deferred_trait_requirements_only,
                         bool settle_overloads)
{
    size_t internal = call_unit_internal_errors(units, unit_count);
    size_t call_count = 0;
    for (size_t i = 0; i < unit_count; i++) {
        n00bb_tu_state_t *state = tu_state(units[i]);
        if (!state || !state->annotations) continue;
        for (size_t label_index = 0;
             label_index < state->annotations->cf_label_order.len;
             label_index++) {
            parse_cf_label_t *label =
                state->annotations->cf_label_order.data[label_index];
            call_count += label && label->kind == PARSE_CF_CALL;
        }
    }

    n00bb_call_site_index_t call_sites = {0};
    call_site_index_init(&call_sites, grammar, units, unit_count);

    bool force_overloads = false;
    for (size_t round = 0; round <= call_count + 1; round++) {
        core_buffer_t before = call_type_snapshot(units, unit_count);
        size_t deferred = 0;
        for (size_t i = 0; i < unit_count; i++) {
            if (resolve_ordinary_calls)
                unify_call_results(grammar, units[i], link, &call_sites,
                    false, false, force_overloads, &deferred);
            if (resolve_member_calls)
                unify_call_results(grammar, units[i], link, &call_sites,
                    true, deferred_trait_requirements_only,
                    force_overloads, &deferred);
        }
        if (call_unit_internal_errors(units, unit_count) > internal) {
            core_buffer_release(&before);
            call_site_index_free(&call_sites);
            return false;
        }
        core_buffer_t after = call_type_snapshot(units, unit_count);
        bool stable = before.byte_len == after.byte_len
            && (!before.byte_len
                || !memcmp(before.data, after.data, before.byte_len));
        core_buffer_release(&before);
        core_buffer_release(&after);
        if (stable && deferred && !force_overloads && settle_overloads) {
            force_overloads = true;
            continue;
        }
        if (stable) {
            call_site_index_free(&call_sites);
            return true;
        }
    }

    call_site_index_free(&call_sites);

    if (unit_count && units[0])
        n00bb_internal_error(core_translation_unit_diagnostics(units[0]),
            units[0], (core_diag_location_t){}, "checking calls",
            "the types of calls kept changing after %zu rounds",
            call_count + 2);
    return false;
}

static bool
finalize_module_group_contracts(void *context,
                        core_translation_unit_t *const *members,
                        size_t member_count)
{
    module_inference_context_t *inference = context;
    parse_grammar_t *grammar = inference->grammar;
    if (!unify_calls_until_stable(grammar, nullptr, members, member_count,
            true, false, false, false)) return false;
    bool shaped = true;
    for (size_t i = 0; i < member_count; i++)
        shaped = n00bb_resolve_shape_literals(grammar, members[i]) && shaped;
    bool grounded = false;
    if (!shaped || !unify_calls_until_stable(grammar, nullptr, members,
            member_count, true, false, false, true)
        || !n00bb_ground_integers(members, member_count, true, &grounded)
        || (grounded && !unify_calls_until_stable(grammar, nullptr, members,
            member_count, true, false, false, true))) return false;

    for (size_t i = 0; i < member_count; i++) {
        n00bb_tu_state_t *state = tu_state(members[i]);
        if (!state || !state->types || !state->annotations
            || !state->annotations->symtab) {
            if (members[i])
                n00bb_internal_error(
                    core_translation_unit_diagnostics(members[i]),
                    members[i], (core_diag_location_t){},
                    "checking function signatures",
                    "module `%.*s` reached checking without its declarations",
                    (int)members[i]->module.identifier.u8_bytes,
                    members[i]->module.identifier.data);
            return false;
        }
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION) continue;
                if (symbol->type && symbol->error_type
                    && !symbol->callable_contract) {
                    th_type *body = parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.body-function-signature"));
                    th_type *declared = th_resolve(symbol->type);
                    th_type *contract_type = body && declared
                        && th_kind_of(declared) == TH_FUNCTION
                        ? th_unify(state->types, body, declared)
                        : body ? body : symbol->type;
                    if (!contract_type) {
                        core_diagnostics_t *diagnostics =
                            core_translation_unit_diagnostics(members[i]);
                        core_string_t declared_text = n00bb_print_type(
                            state->types, declared);
                        core_string_t body_text = n00bb_print_type(
                            state->types, body);
                        n00bb_report(diagnostics, members[i],
                            N00B_DIAG_TYPE_MISMATCH,
                            n00bb_symbol_location(symbol),
                            N00B_TEXT_IS_DECLARED_AS_BUT_ITS,
                            N00BB_ARGS(ARG_TEXT(symbol->name),
                                ARG_TEXT(declared_text), ARG_TEXT(body_text)));
                        core_list_free(scopes);
                        return false;
                    }
                    th_type *contract_error = body
                        ? th_func_error(body) : symbol->error_type;
                    symbol->callable_contract = function_with_error_channel(
                        state->types, contract_type, contract_error);
                }
            }
        }
        core_list_free(scopes);
    }
    return true;
}

bool
generalize_link_unit(parse_grammar_t *grammar, core_link_unit_t *link,
                     core_list_t(core_translation_unit_ptr_t) *units)
{
    if (!module_type_universe_is_shared(units)) return false;
    if (!resolve_imported_module_symbols(grammar, units)) return false;
    for (size_t i = 0; i < units->len; i++)
        finalize_module_inference(units->data[i]);
    module_inference_context_t inference = {
        .grammar = grammar,
    };
    return core_link_unit_visit_sccs(link, finalize_module_group_contracts,
                                     &inference);
}

parse_tree_t *
call_argument_for_parameter(parse_grammar_t *grammar, parse_tree_t *call,
                           parse_sym_entry_t *function, size_t target,
                           bool trait_dispatch)
{
    if (trait_dispatch && target == 0) {
        parse_tree_t *member = member_access_node(grammar, call);
        if (!member && core_tree_num_children(call))
            member = member_access_node(grammar,
                                         core_tree_child(call, 0));
        return member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    }
    n00bb_call_plan_t plan;
    (void)n00bb_plan_call(&plan, grammar, function,
        function->callable_contract ? function->callable_contract
                                    : function->type,
        call_arguments_node(grammar, call), trait_dispatch ? 1 : 0);
    parse_tree_t *found = n00bb_call_plan_parameter_argument(&plan, target);
    n00bb_call_plan_free(&plan);
    return found;
}

bool
publish_link_call_dependencies(core_link_unit_t *link,
                               n00bb_link_analysis_t *analysis,
                               uint32_t producer_stage,
                               core_link_contribution_state_t *state_out)
{
    bool complete = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        for (size_t label_index = 0;
             label_index < state->annotations->cf_label_order.len;
             label_index++) {
            parse_cf_label_t *label =
                state->annotations->cf_label_order.data[label_index];
            if (!label || label->kind != PARSE_CF_CALL || !label->cond)
                continue;
            parse_tree_t *member = member_access_node(analysis->grammar,
                                                       label->cond);
            bool trait_dispatch = member
                && !call_receiver_is_module(analysis->grammar, unit,
                    core_tree_child(member, 0));
            parse_sym_entry_t *callee = bound_function_symbol(label->cond);
            if (trait_dispatch && n00bb_builtin_option_function(callee))
                continue;
            if (trait_dispatch && callee && analysis->may_link_further) {
                n00bb_link_use_site_t *site = link_use_site_for_node(
                    state, member);
                callee = linkable_trait_requirement(analysis, unit,
                                                     site, callee);
            }
            parse_sym_entry_t *context = attached_symbol(
                analysis->grammar, unit, label->cond);
            bool context_call = !callee
                && context_symbol(analysis->grammar, context);
            if (context_call) callee = context;
            bool generic_dispatch = false;
            if (trait_dispatch && member) {
                parse_tree_t *receiver_node = core_tree_num_children(member)
                    ? core_tree_child(member, 0) : nullptr;
                th_type *receiver_type = receiver_node
                    ? parse_tree_type(receiver_node) : nullptr;
                core_string_t method_name =
                    core_tree_num_children(member) >= 3
                        ? first_token_text(core_tree_child(member, 2))
                        : (core_string_t){0};
                generic_dispatch = method_name.data
                    && has_deferred_method_constraint(receiver_type,
                                                       method_name);
            }
            core_link_declaration_t *target = callee
                ? link_declaration_for_symbol(analysis, callee) : nullptr;
            bool external = target
                && core_link_declaration_is_external_requirement(target);
            parse_sym_entry_t *caller = link_effect_owner_symbol(
                analysis->grammar, label->self);
            core_link_declaration_t *source = caller
                ? link_declaration_for_symbol(analysis, caller) : nullptr;
            n00bb_link_use_site_t *dispatch_site = trait_dispatch && member
                ? link_use_site_for_node(state, member) : nullptr;
            if (dispatch_site && dispatch_site->closed_dispatch) {
                bool resolved_plan = !caller || source;
                for (size_t p = 0; resolved_plan
                     && p < dispatch_site->closed_dispatch->count; p++) {
                    parse_sym_entry_t *implementation =
                        dispatch_site->closed_dispatch->entries[p]
                            .implementation;
                    core_link_declaration_t *implementation_declaration =
                        link_declaration_for_symbol(analysis,
                                                    implementation);
                    resolved_plan = implementation_declaration
                        && core_link_unit_add_dependency(link,
                            N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH,
                            producer_stage, CORE_LINK_DEPENDENCY_RESOLVED,
                            source, implementation_declaration, unit,
                            label->self, link_source_location(label->self),
                            core_source_file_path(&unit->source), 1,
                            nullptr);
                }
                if (!resolved_plan) {
                    complete = false;
                    return false;
                }
                continue;
            }
            bool valid_caller = !caller
                || caller->kind == PARSE_SYM_FUNCTION
                || context_symbol(analysis->grammar, caller);
            bool resolved = target && callee
                && (context_call
                    ? context_symbol(analysis->grammar, callee)
                    : callee->kind == PARSE_SYM_FUNCTION)
                && valid_caller && (!caller || source);
            n00bb_return_source_t *return_source = n00bb_node_return_source(
                label->self);
            if (resolved && return_source
                && return_source->kind == N00BB_RETURN_SOURCE_CALL
                && return_source->node == label->self)
                return_source->symbol = callee;
            if (caller && !source)
                source = nullptr;
            if (!core_link_unit_add_dependency(link,
                    context_call ? N00BB_LINK_DEPENDENCY_CONTEXT_CALL
                    : trait_dispatch
                        ? N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                        : N00BB_LINK_DEPENDENCY_CALL,
                    producer_stage,
                    external && resolved ? CORE_LINK_DEPENDENCY_EXTERNAL
                    : resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                    : generic_dispatch ? CORE_LINK_DEPENDENCY_DEFERRED
                                        : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    source, resolved ? target : nullptr, unit, label->self,
                    link_source_location(label->self),
                    core_source_file_path(&unit->source), 1, nullptr))
                return false;
            if (!resolved) complete = false;
        }
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (n00bb_builtin_option_function(site->symbol)
                || n00bb_builtin_array_read_function(site->symbol)
                || n00bb_builtin_array_write_function(site->symbol)) continue;
            if (!site->dispatch_trait.data) continue;
            parse_sym_entry_t *caller = link_effect_owner_symbol(
                analysis->grammar, site->node);
            core_link_declaration_t *source = caller
                ? link_declaration_for_symbol(analysis, caller) : nullptr;
            parse_sym_entry_t *callee = site->symbol
                ? linkable_trait_requirement(analysis, unit,
                    site, site->symbol) : nullptr;
            core_link_declaration_t *target = callee
                ? link_declaration_for_symbol(analysis, callee) : nullptr;
            core_link_reference_t *reference = site->reference;
            if (!reference && primitive_operator_site(state, site)) continue;
            bool resolved = target && (!caller || source)
                && reference && !core_link_reference_is_deferred(reference);
            bool deferred = reference
                && core_link_reference_is_deferred(reference);
            if (!resolved) complete = false;
            if (!core_link_unit_add_dependency(link,
                    N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH, producer_stage,
                    resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                    : deferred ? CORE_LINK_DEPENDENCY_DEFERRED
                               : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    source, resolved ? target : nullptr, unit, site->node,
                    link_source_location(site->node),
                    core_source_file_path(&unit->source), 1, nullptr))
                return false;
        }
    }
    *state_out = complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                          : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}



