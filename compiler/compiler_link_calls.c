#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "parse/tree_util.h"

typedef struct {
    parse_grammar_t *grammar;
} module_inference_context_t;

void
report_call_type_error(core_translation_unit_t *unit, parse_tree_t *call,
                       th_ctx *types, th_type *actual, th_type *expected,
                       n00bb_call_role_t role)
{
    core_buffer_t message = {0};
    core_string_t actual_text = actual ? n00bb_print_type(types, actual)
                                       : CORE_STRING_STATIC("unknown");
    core_string_t expected_text = expected
        ? n00bb_print_type(types, expected) : CORE_STRING_STATIC("unknown");
    int actual_size = (int)actual_text.u8_bytes;
    int expected_size = (int)expected_text.u8_bytes;
    switch (role) {
    case N00BB_CALL_ARGUMENT:
        core_buffer_printf(&message,
            "argument of type %.*s does not convert to parameter type %.*s",
            actual_size, actual_text.data, expected_size, expected_text.data);
        break;
    case N00BB_CALL_RECEIVER:
        core_buffer_printf(&message,
            "receiver of type %.*s does not match the method's receiver type %.*s",
            actual_size, actual_text.data, expected_size, expected_text.data);
        break;
    case N00BB_CALL_RESULT:
        core_buffer_printf(&message,
            "this function returns %.*s, which does not match its use as %.*s",
            expected_size, expected_text.data, actual_size, actual_text.data);
        break;
    case N00BB_CALL_SIGNATURE:
        core_buffer_printf(&message, "call signature %.*s does not match %.*s",
            actual_size, actual_text.data, expected_size, expected_text.data);
        break;
    }
    parse_token_info_t *site = parse_tree_find_first_terminal(call);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
        core_buffer_string(&message),
        site ? (core_diag_location_t){
            .line = parse_token_line(site),
            .column = parse_token_column(site),
            .end_line = parse_token_endline(site),
            .end_column = parse_token_endcol(site),
            .has_location = true,
        } : (core_diag_location_t){0});
    core_buffer_release(&message);
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
    bool converted = false;
    th_type *materialized = n00bb_type_materialize(state->link_analysis
            ? state->link_analysis->grammar : nullptr,
        state, typed_node, actual, expected, &converted);
    if (!materialized)
        report_call_type_error(unit, diagnostic_node, state->types, actual,
                               expected, role);
    return materialized != nullptr;
}

void
n00bb_report_conversion_error(core_translation_unit_t *unit,
                              parse_tree_t *site, th_ctx *types,
                              th_type *value, th_type *target)
{
    core_string_t actual_text = n00bb_print_type(types, value);
    core_string_t expected_text = n00bb_print_type(types, target);
    core_buffer_t message = {0};
    core_buffer_printf(&message, "value of type %.*s does not convert to %.*s",
        (int)actual_text.u8_bytes, actual_text.data,
        (int)expected_text.u8_bytes, expected_text.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0042"),
        core_buffer_string(&message), link_source_location(site));
    core_buffer_release(&message);
}

bool
n00bb_check_deferred_conversions(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return true;
    for (size_t i = 0; i < state->deferred_conversions.len; i++) {
        n00bb_deferred_conversion_t *pending =
            &state->deferred_conversions.data[i];
        th_type *value = th_resolve(pending->value);
        th_type *target = th_resolve(pending->target);
        if (!th_is_closed(state->types, target)) continue;
        if (!th_is_closed(state->types, value)) {
            (void)th_unify_in_place(state->types, value, target);
            continue;
        }
        bool converted = false;
        if (n00bb_type_materialize(state->link_analysis
                ? state->link_analysis->grammar : nullptr, state,
                pending->site, value, target, &converted)) continue;
        if (core_diagnostics_failed(core_translation_unit_diagnostics(unit)))
            continue;
        n00bb_report_conversion_error(unit, pending->site, state->types,
                                      value, target);
    }
    state->deferred_conversions.len = 0;
    return !core_diagnostics_failed(core_translation_unit_diagnostics(unit));
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
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && resolution.target.symbol == from) {
        parse_tree_set_resolution_symbol(node, to);
        if (parse_sym_remove_use(from, node)) (void)parse_sym_add_use(to, node);
    }
    if (parse_tree_sym(node) == from)
        parse_tree_set_sym(node, to);
    n00bb_link_use_site_t *site = link_use_site_for_node(state, node);
    if (site && site->symbol == from) site->symbol = to;
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
unify_call_argument(core_translation_unit_t *unit, n00bb_tu_state_t *state,
                    parse_tree_t *diagnostic_node, parse_tree_t *argument,
                    th_type *expected, bool context_call)
{
    th_type *actual = parse_tree_type(argument);
    if (context_call && n00bb_value_coerces(state->types, actual, expected))
        report_call_type_error(unit, argument, state->types, actual,
                               expected, N00BB_CALL_ARGUMENT);
    else (void)unify_call_node_type(unit, state, diagnostic_node, argument,
                                    actual, expected, N00BB_CALL_ARGUMENT);
    (void)n00bb_contextual_float_literal(argument, expected);
}

static void
report_call_binding_error(core_translation_unit_t *unit, parse_tree_t *node,
                          core_string_t code, core_string_t message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, code, message, link_source_location(node));
}

static void
report_missing_parameter(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_sym_entry_t *callee, parse_tree_t *formals,
                         size_t index, parse_tree_t *site)
{
    core_string_t name = {0};
    (void)function_parameter_info(grammar, callee, formals, index, nullptr,
                                  &name, nullptr, nullptr);
    core_buffer_t message = {0};
    core_buffer_printf(&message, "call omits the required parameter '%.*s'",
                       (int)name.u8_bytes, name.data ? name.data : "");
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0040"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(site));
    core_buffer_release(&message);
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
    size_t parameter_count = th_func_nparams(signature);
    const th_param *rest = th_func_rest(signature);
    bool *bound = core_alloc_array(bool,
                                   parameter_count ? parameter_count : 1);
    for (size_t i = 0; i < offset && i < parameter_count; i++)
        bound[i] = true;
    bool valid = true;
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(grammar,
        child_nt(grammar, call_args, CORE_STRING_STATIC("positional-args")),
        &positional);
    for (size_t i = 0; i < positional.len; i++) {
        size_t index = i + offset;
        const th_param *parameter = index < parameter_count
            ? th_func_param(signature, index) : rest;
        if (!parameter) {
            report_call_binding_error(unit, positional.data[i],
                CORE_STRING_STATIC("N00BB-T0039"),
                CORE_STRING_STATIC(
                    "call passes more arguments than its function has parameters"));
            valid = false;
            break;
        }
        if (index < parameter_count) bound[index] = true;
        if (!core_tree_is_leaf(positional.data[i]))
            core_list_push(*bindings, ((n00bb_call_binding_t){
                positional.data[i], parameter->type, false}));
    }
    core_list_free(positional);
    parse_tree_t *formals = callee
        ? callable_formal_parameters(grammar, callee) : nullptr;
    core_list_t(n00bb_tree_ptr_t) keywords = core_list_new(n00bb_tree_ptr_t);
    if (callee)
        collect_keyword_nodes(grammar,
            child_nt(grammar, call_args, CORE_STRING_STATIC("keyword-args")),
            &keywords);
    for (size_t i = 0; i < keywords.len; i++) {
        parse_tree_t *keyword = keywords.data[i];
        parse_tree_t *argument = child_nt(grammar, keyword,
                                          CORE_STRING_STATIC("expression"));
        size_t index = 0;
        th_type *expected = nullptr;
        if (function_parameter_named(grammar, callee, formals,
                call_keyword_name(keyword), &index)) {
            const th_param *parameter = th_func_param(signature, index);
            const th_kwparam *option = index >= parameter_count
                ? th_func_kw(signature, index - parameter_count) : nullptr;
            expected = parameter ? parameter->type
                : option ? option->type : nullptr;
        }
        if (!expected) {
            report_call_binding_error(unit, keyword,
                CORE_STRING_STATIC("N00BB-T0037"),
                CORE_STRING_STATIC("named argument does not match a parameter"));
            valid = false;
            continue;
        }
        if (index < parameter_count && bound[index]) {
            report_call_binding_error(unit, keyword,
                CORE_STRING_STATIC("N00BB-T0038"),
                CORE_STRING_STATIC("a parameter is supplied more than once"));
            valid = false;
            continue;
        }
        if (index < parameter_count) bound[index] = true;
        if (argument && !core_tree_is_leaf(argument))
            core_list_push(*bindings, ((n00bb_call_binding_t){
                argument, expected, true}));
    }
    core_list_free(keywords);
    for (size_t i = 0; valid && callee && i < parameter_count; i++) {
        uint8_t flags = 0;
        if (bound[i] || !function_parameter_info(grammar, callee, formals,
                i, nullptr, nullptr, &flags, nullptr)
            || (flags & 1u)) continue;
        report_missing_parameter(grammar, unit, callee, formals, i, site);
        valid = false;
    }
    core_free(bound);
    return valid;
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
        core_diagnostics_report(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not set call-site type-environment scope"));
        return;
    }

    for (size_t label_index = 0;
         label_index < state->annotations->cf_label_order.len;
         label_index++) {
        parse_cf_label_t *label =
            state->annotations->cf_label_order.data[label_index];
        if (!label) continue;
        if (label->kind != PARSE_CF_CALL || !label->cond ||
            core_tree_is_leaf(label->cond)) continue;

        parse_tree_t *member = member_access_node(grammar, label->cond);
        if ((member != nullptr) != resolve_member_calls) continue;
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
        if (callee_symbol && !context_call) {
            bool pending = false;
            parse_sym_entry_t *selected = n00bb_select_call_overload(grammar,
                state, label, callee_symbol, &pending);
            if (pending && !force_overloads) {
                (*deferred)++;
                continue;
            }
            if (selected && selected != callee_symbol) {
                rebind_call_callee(grammar, state, label->self, label,
                                   callee_symbol, selected);
                callee_symbol = selected;
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
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                        CORE_STRING_STATIC(
                            "could not inspect generic trait method requirements"),
                        link_source_location(member));
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
                            core_diagnostics_report_at(
                                core_translation_unit_diagnostics(unit), unit,
                                CORE_DIAG_ERROR,
                                CORE_STRING_STATIC("N00BB-T0002"),
                                CORE_STRING_STATIC(
                                    "resolved trait target has no callable signature"),
                                link_source_location(member));
                            continue;
                        }
                    }
                }
                else if (receiver_type &&
                         th_kind_of(th_resolve(receiver_type)) != TH_VAR) {
                    core_buffer_t message = {0};
                    core_string_t receiver_text = n00bb_print_type(
                        state->types, receiver_type);
                    core_buffer_printf(&message,
                        "no matching trait method '%.*s' for receiver type %.*s",
                        (int)method_text.u8_bytes,
                        method_text.data ? method_text.data : "",
                        (int)receiver_text.u8_bytes, receiver_text.data);
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                        core_string_borrow(message.data, message.byte_len),
                        link_source_location(member));
                    core_buffer_release(&message);
                }
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
            if (!receiver || !unify_call_node_type(unit, state, label->self,
                    receiver_node, receiver_type, receiver->type,
                    N00BB_CALL_RECEIVER))
                continue;
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
            unify_call_argument(unit, state,
                binding->keyword ? binding->argument : label->self,
                binding->argument, binding->expected, context_call);
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
        parse_sym_entry_t *handler = propagated
            ? n00bb_try_error_symbol(grammar, label->self) : nullptr;
        if (handler) {
            th_type *bound = handler->type
                ? th_unify_in_place(state->types, handler->type, failure_type)
                : failure_type;
            if (bound) handler->type = bound;
        }
        else if (propagated && caller_symbol
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

bool
unify_calls_until_stable(parse_grammar_t *grammar, core_link_unit_t *link,
                         core_translation_unit_t *const *units,
                         size_t unit_count, bool resolve_ordinary_calls,
                         bool resolve_member_calls,
                         bool deferred_trait_requirements_only)
{
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
        if (call_units_failed(units, unit_count)) {
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
        if (stable && deferred && !force_overloads) {
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
        core_diagnostics_report(core_translation_unit_diagnostics(units[0]),
            units[0], CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC(
                "call type constraints did not stabilize in the shared inference universe"));
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
            true, false, false)) return false;
    bool shaped = true;
    for (size_t i = 0; i < member_count; i++)
        shaped = n00bb_resolve_shape_literals(grammar, members[i]) && shaped;
    if (!shaped || !unify_calls_until_stable(grammar, nullptr, members,
            member_count, true, false, false)) return false;

    for (size_t i = 0; i < member_count; i++) {
        n00bb_tu_state_t *state = tu_state(members[i]);
        if (!state || !state->types || !state->annotations
            || !state->annotations->symtab) {
            if (members[i])
                core_diagnostics_report(
                    core_translation_unit_diagnostics(members[i]),
                    members[i], CORE_DIAG_FATAL,
                    CORE_STRING_STATIC("N00BB-T0004"),
                    CORE_STRING_STATIC(
                        "source module has no complete semantic state"));
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
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(members[i]),
                            members[i], CORE_DIAG_FATAL,
                            CORE_STRING_STATIC("N00BB-T0004"),
                            CORE_STRING_STATIC(
                                "could not join the declared and inferred function contracts"),
                            symbol->decl_node
                                ? link_source_location(symbol->decl_node)
                                : (core_diag_location_t){0});
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
    parse_tree_t *signature = child_nt(grammar, function->decl_node,
                                       CORE_STRING_STATIC("func-type-signature"));
    parse_tree_t *param_decl = child_nt(grammar, signature,
                                        CORE_STRING_STATIC("param-decl"));
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    if (function->decl_node)
        collect_ir_parameters(grammar, param_decl, &formals, &rest);
    const uint8_t *interface_params = function->decl_node
        ? nullptr : function->user_data;
    uint32_t interface_param_count = 0;
    if (interface_params && !memcmp(interface_params, "N0FP", 4)
        && interface_params[4] == 1)
        interface_param_count = read_u32_le(interface_params + 5);
    if (trait_dispatch && target == 0) {
        parse_tree_t *member = member_access_node(grammar, call);
        if (!member && core_tree_num_children(call))
            member = member_access_node(grammar,
                                         core_tree_child(call, 0));
        core_list_free(formals);
        return member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    }
    if (trait_dispatch) target--;
    core_string_t target_name = target < formals.len
        ? formals.data[target]->name : (core_string_t){0};
    if (interface_params && target < interface_param_count) {
        size_t offset = 9;
        for (size_t i = 0; i <= target; i++) {
            uint16_t name_size = (uint16_t)interface_params[offset]
                | (uint16_t)interface_params[offset + 1] << 8;
            offset += 4;
            if (i == target) {
                target_name = core_string_borrow(
                    (const char *)interface_params + offset, name_size);
                break;
            }
            offset += name_size;
        }
    }
    parse_tree_t *args = call_arguments_node(grammar, call);
    parse_tree_t *positional = child_nt(grammar, args,
                                        CORE_STRING_STATIC("positional-args"));
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(grammar, positional, &positional_nodes);
    parse_tree_t *found = target < positional_nodes.len
        ? positional_nodes.data[target] : nullptr;
    if (!found) {
        parse_tree_t *keyword_args = child_nt(grammar, args,
                                              CORE_STRING_STATIC("keyword-args"));
        core_list_t(n00bb_tree_ptr_t) keywords =
            core_list_new(n00bb_tree_ptr_t);
        if (keyword_args)
            collect_keyword_nodes(grammar, keyword_args, &keywords);
        for (size_t i = 0; !found && i < keywords.len; i++) {
            core_string_t name = call_keyword_name(keywords.data[i]);
            parse_tree_t *expression = child_nt(grammar, keywords.data[i],
                                                 CORE_STRING_STATIC("expression"));
            if (target_name.data && name.data && expression
                && core_string_eq(name, target_name))
                found = expression;
        }
        core_list_free(keywords);
    }
    core_list_free(positional_nodes);
    core_list_free(formals);
    (void)rest;
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



