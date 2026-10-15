#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "parse/tree_util.h"

typedef struct {
    parse_grammar_t *grammar;
} module_inference_context_t;

void
report_call_type_error(core_translation_unit_t *unit, parse_tree_t *call,
                       th_ctx *types, th_type *actual, th_type *expected)
{
    core_buffer_t message = {0};
    const char *detail = th_last_error(types);
    core_buffer_printf(&message,
                       "call type does not match its function (%s vs %s)%s%s",
                       actual ? n00bb_print_type(types, actual) : "unknown",
                       expected ? n00bb_print_type(types, expected) : "unknown",
                       detail && *detail ? ": " : "",
                       detail && *detail ? detail : "");
    parse_token_info_t *site = parse_tree_find_first_terminal(call);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
        core_string_from_cstr(message.data),
        site ? (core_diag_location_t){
            .line = site->line, .column = site->column,
            .end_line = site->endline, .end_column = site->endcol,
            .has_location = true,
        } : (core_diag_location_t){0});
    core_buffer_release(&message);
}

bool
unify_call_node_type(core_translation_unit_t *unit,
                     n00bb_tu_state_t *state, parse_tree_t *diagnostic_node,
                     parse_tree_t *typed_node, th_type *actual,
                     th_type *expected)
{
    if (!actual || !expected) return true;
    th_type *actual_element = n00bb_atomic_element_type(actual);
    th_type *expected_element = n00bb_atomic_element_type(expected);
    bool unwrap_actual = actual_element && !expected_element;
    bool wrap_expected = expected_element && !actual_element;
    th_type *unification_actual = unwrap_actual ? actual_element : actual;
    th_type *unification_expected = wrap_expected
        ? expected_element : expected;
    th_type *option_element = nullptr;
    bool option_context = n00bb_option_type_element(unification_actual,
        &option_element) && th_is_closed(state->types, unification_expected);
    th_type *expected_option_element = nullptr;
    bool into_option_context = n00bb_option_type_element(
        unification_expected, &expected_option_element)
        && th_is_closed(state->types, unification_actual)
        && (n00bb_nil_type(state->types, unification_actual)
            || th_unify(state->types, unification_actual,
                        expected_option_element));
    th_type *unified = nullptr;
    parse_tree_t *origin = typed_node && !wrap_expected && !unwrap_actual
        ? core_tree_node_value(typed_node).integer_literal_origin : nullptr;
    if (origin && n00bb_contextual_integer_literal(state, typed_node,
                                                    expected)) {
        unified = expected;
    }
    if (!unified && typed_node && !wrap_expected && !unwrap_actual
        && is_integer_type(unification_actual)
        && is_integer_type(unification_expected)) {
        int64_t minimum = 0, maximum = 0;
        if (n00bb_integer_type_promotes_to(unification_actual,
                                            unification_expected)
            || n00bb_practical_unsigned_to_signed(state->link_analysis
                    ? state->link_analysis->grammar : nullptr,
                state, typed_node, unification_actual,
                unification_expected)
            || (n00bb_current_integer_range(state, typed_node,
                                            &minimum, &maximum)
                && n00bb_integer_interval_fits_type(unification_expected,
                                                    minimum, maximum)))
            return true;
    }
    if (!unified && into_option_context) unified = unification_expected;
    if (!unified && typed_node && !core_tree_is_leaf(typed_node)
        && n00bb_node_record_literal_origin(typed_node)
        && th_kind_of(th_resolve(unification_actual)) == TH_RECORD
        && th_kind_of(th_resolve(unification_expected)) == TH_NAMED) {
        th_type *definition = th_type_definition(state->types,
                                                 unification_expected);
        if (definition && th_kind_of(th_resolve(definition)) == TH_RECORD
            && th_unify_probe(state->types, unification_actual, definition)
            && th_unify_in_place(state->types, unification_actual, definition))
            unified = unification_expected;
    }
    if (!unified && option_context
        && th_unify_in_place(state->types, option_element,
                             unification_expected))
        unified = unification_expected;
    if (!unified)
        unified = th_unify_in_place(state->types, unification_actual,
                                    unification_expected);
    if (!unified) {
        report_call_type_error(unit, diagnostic_node, state->types, actual,
                               expected);
        return false;
    }
    for (parse_tree_t *node = origin; node;
         node = core_tree_node_value(node).parent) {
        core_tree_node_value(node).type = into_option_context
            ? unification_actual : unified;
        if (node == typed_node) break;
    }
    if (typed_node && !core_tree_is_leaf(typed_node)) {
        th_type *contextual_type = into_option_context
            ? unification_actual : wrap_expected ? expected : unified;
        core_tree_node_value(typed_node).type = contextual_type;
        propagate_tree_type_to_wrappers(typed_node, state->types,
                                        actual, contextual_type);
    }
    return true;
}


bool
dotted_receiver_prefix_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;) {
        parse_tree_t *parent = core_tree_node_value(at).parent;
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
    const char *requirement_trait;
} n00bb_call_signature_t;

typedef struct {
    core_dict_t by_caller;
    core_dict_t instantiated_signatures;
} n00bb_call_site_index_t;

static th_type *
call_site_signature_instance(n00bb_call_site_index_t *index,
                             parse_cf_label_t *label, th_ctx *types,
                             th_type *source, bool channels,
                             const char *requirement_trait)
{
    if (!source) return nullptr;
    n00bb_call_signature_t *cached = core_dict_get(
        &index->instantiated_signatures, label, nullptr);
    /* Lantern returns a fresh method requirement signature on every query.
     * Its trait name identifies the same frozen contract across inference
     * rounds; using the fresh pointer as the cache key would create another
     * type variable and rebind the receiver on every round. */
    bool same_requirement = requirement_trait && cached
        && cached->requirement_trait
        && !strcmp(cached->requirement_trait, requirement_trait);
    if (cached && cached->channels == channels
        && (same_requirement
            || (!requirement_trait && !cached->requirement_trait
                && cached->source == source)))
        return cached->instance;
    th_type *instance = nullptr;
    if (channels) {
        if (!th_env_signature_channels(types, source, &instance, nullptr))
            return nullptr;
    }
    else instance = th_type_clone(types, source);
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
                                        nullptr);
}

static th_type *
call_site_requirement_signature(n00bb_call_site_index_t *index,
                                parse_cf_label_t *label, th_ctx *types,
                                th_type *source, const char *trait_name)
{
    return call_site_signature_instance(index, label, types, source, false,
                                        trait_name);
}

static th_type *
call_site_channels(n00bb_call_site_index_t *index, parse_cf_label_t *label,
                   th_ctx *types, th_type *source)
{
    return call_site_signature_instance(index, label, types, source, true,
                                        nullptr);
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
        core_dict_foreach(state->annotations->cf_labels, entry) {
            parse_cf_label_t *label = entry->value;
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
                 core_list_t(parse_sym_entry_ptr_t) *visited)
{
    if (from == target) return true;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == from) return false;
    core_list_push(*visited, from);

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
    core_list_t(parse_sym_entry_ptr_t) visited =
        core_list_new(parse_sym_entry_ptr_t);
    bool recursive = function_reaches(grammar, index, callee, caller,
                                      &visited);
    core_list_free(visited);
    return recursive;
}


static void
unify_call_results(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   core_link_unit_t *link,
                   n00bb_call_site_index_t *call_sites,
                   bool resolve_member_calls,
                   bool deferred_trait_requirements_only)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->types) return;
    if (resolve_member_calls
        && !th_ctx_set_environment_scope(state->types,
            unit->module.identifier.data,
            n00bb_package_id(unit).data)) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not set call-site type-environment scope"));
        return;
    }

    core_dict_foreach(state->annotations->cf_labels, item) {
        parse_cf_label_t *label = item->value;
        if (!label) continue;
        if (label->kind != PARSE_CF_CALL || !label->cond ||
            core_tree_is_leaf(label->cond)) continue;

        parse_nt_node_t *callee_node = &core_tree_node_value(label->cond);
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
            nt_is(grammar, label->self, "context-call")
                ? attached_symbol(grammar, unit, label->cond) : nullptr;
        bool context_call = context_symbol(grammar, context_target);
        parse_sym_entry_t *caller_symbol = enclosing_function_symbol(
            grammar, label->self);
        bool member_call = member && !context_call
            && (!callee_symbol
                || n00bb_is_trait_dispatch_reference(grammar, unit, member));
        th_type *signature = context_call ? context_target->type
            : callee_symbol ? callee_symbol->type : callee_node->type;
        bool signature_instantiated = false;
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
                && !core_tree_is_leaf(receiver_node)
                ? core_tree_node_value(receiver_node).type : nullptr;
            th_type *caller_contract = caller_symbol
                ? (caller_symbol->callable_contract
                    ? caller_symbol->callable_contract
                    : caller_symbol->type) : nullptr;
            bool generic_open_receiver = caller_contract
                && receiver_type
                && !th_is_closed(state->types, caller_contract)
                && !th_is_closed(state->types, receiver_type);
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
                char *method_name = method_text.data
                    ? core_strndup(method_text.data,
                                   method_text.u8_bytes) : nullptr;
                size_t requirement_count = method_name
                    ? th_env_method_requirements(state->types, method_name,
                                                  nullptr, 0) : 0;
                th_env_method_requirement_t *requirements =
                    requirement_count ? core_alloc_array(
                        th_env_method_requirement_t, requirement_count)
                        : nullptr;
                if (requirement_count && !requirements) {
                    core_free(method_name);
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                        CORE_STRING_STATIC(
                            "could not inspect generic trait method requirements"),
                        link_source_location(member));
                    continue;
                }
                if (requirement_count)
                    th_env_method_requirements(state->types, method_name,
                        requirements, requirement_count);
                size_t matched = SIZE_MAX;
                for (size_t r = 0; r < requirement_count; r++) {
                    if (!has_trait_constraint(receiver_type,
                                              requirements[r].trait_name)
                        && !(has_deferred_method_constraint(receiver_type,
                                method_text)
                            && n00bb_method_requirement_matches(
                                receiver_type, method_text,
                                requirements[r].signature)))
                        continue;
                    if (matched != SIZE_MAX) {
                        matched = SIZE_MAX;
                        break;
                    }
                    matched = r;
                }
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
                core_free(method_name);
            }
            else if (receiver_type && method_text.data) {
                char *method_name = core_strndup(method_text.data,
                                                 method_text.u8_bytes);
                th_env_method_t method = {0};
                const char *trait_name = nullptr;
                th_env_method_status_t method_status =
                    th_env_method_resolve_by_name(state->types,
                        receiver_type, method_name, &method, &trait_name);
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
                            core_free(method_name);
                            continue;
                        }
                    }
                }
                else if (receiver_type &&
                         th_kind_of(th_resolve(receiver_type)) != TH_VAR) {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "no matching trait method '%.*s' for receiver type %s",
                        (int)method_text.u8_bytes,
                        method_text.data ? method_text.data : "",
                        n00bb_print_type(state->types, receiver_type));
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                        core_string_borrow(message.data, message.byte_len),
                        link_source_location(member));
                    core_buffer_release(&message);
                }
                core_free(method_name);
            }
        }
        th_type *result = label->self && !core_tree_is_leaf(label->self)
            ? core_tree_node_value(label->self).type : nullptr;
        if (!signature || !result) continue;
        bool recursive = call_is_recursive(grammar, call_sites,
                                           caller_symbol, callee_symbol);
        if (!signature_instantiated) {
            if (callee_symbol && !recursive)
                signature = call_site_signature(call_sites, label,
                    state->types, signature);
        }
        parse_tree_t *positional = child_nt(grammar, label->then_body,
                                            "positional-args");
        size_t positional_capacity = positional
            ? core_tree_num_children(positional) : 0;
        const th_type **argument_sources = positional_capacity
            ? core_alloc_array(const th_type *, positional_capacity) : nullptr;
        th_type **argument_copies = positional_capacity
            ? core_alloc_array(th_type *, positional_capacity) : nullptr;
        if (positional_capacity && (!argument_sources || !argument_copies)) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("could not allocate call argument types"));
            core_free(argument_sources);
            core_free(argument_copies);
            continue;
        }
        size_t argument_source_count = 0;
        for (size_t i = 0; positional && i < positional_capacity; i++) {
            parse_tree_t *argument = core_tree_child(positional, i);
            if (!nt_is(grammar, argument, "expression")) continue;
            th_type *actual = core_tree_is_leaf(argument) ? nullptr
                : core_tree_node_value(argument).type;
            bool open = actual && !th_is_closed(state->types, actual)
                && !core_tree_node_value(argument).integer_literal_origin
                && !n00bb_node_record_literal_origin(argument);
            argument_sources[argument_source_count++] = open ? actual : nullptr;
        }
        bool arguments_copied = th_type_clone_many(state->types,
            argument_sources, argument_copies, argument_source_count);
        if (!arguments_copied) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("could not copy call argument type context"));
            core_free(argument_sources);
            core_free(argument_copies);
            continue;
        }
        size_t parameter_count = th_func_nparams(signature);
        size_t argument_index = 0;
        size_t parameter_offset = member_call ? 1 : 0;
        if (member_call) {
            const th_param *receiver = th_func_param(signature, 0);
            parse_tree_t *receiver_node = core_tree_child(member, 0);
            th_type *receiver_type = receiver_node
                && !core_tree_is_leaf(receiver_node)
                ? core_tree_node_value(receiver_node).type : nullptr;
            if (!receiver || !unify_call_node_type(unit, state, label->self,
                    receiver_node, receiver_type, receiver->type)) {
                core_free(argument_sources);
                core_free(argument_copies);
                continue;
            }
        }
        for (size_t i = 0; positional
             && i < core_tree_num_children(positional); i++) {
            parse_tree_t *argument = core_tree_child(positional, i);
            if (!nt_is(grammar, argument, "expression")) continue;
            size_t parameter_index = argument_index + parameter_offset;
            const th_param *parameter = th_func_param(signature,
                                                       parameter_index);
            if (!parameter && parameter_index >= parameter_count)
                parameter = th_func_rest(signature);
            if (parameter && !core_tree_is_leaf(argument)) {
                th_type *actual = core_tree_node_value(argument).type;
                th_type *copy = argument_copies[argument_index];
                (void)unify_call_node_type(unit, state, label->self,
                    copy ? nullptr : argument, copy ? copy : actual,
                    parameter->type);
            }
            argument_index++;
        }
        core_list_t(n00bb_tree_ptr_t) arguments =
            core_list_new(n00bb_tree_ptr_t);
        collect_positional_nodes(grammar, positional, &arguments);
        for (size_t i = 0; i < arguments.len; i++) {
            const th_param *parameter = th_func_param(signature,
                i + parameter_offset);
            if (!parameter && i + parameter_offset >= parameter_count)
                parameter = th_func_rest(signature);
            if (parameter)
                (void)n00bb_contextual_float_literal(arguments.data[i],
                                                     parameter->type);
        }
        core_list_free(arguments);
        core_free(argument_sources);
        core_free(argument_copies);
        th_type *return_type = th_func_ret(signature);
        if (return_type)
            (void)unify_call_node_type(unit, state, label->self, label->self,
                                       result, return_type);
        th_type *failure_type = th_func_error(signature);
        if (failure_type && n00bb_call_is_error_propagated(
                grammar, label->self) && caller_symbol
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
        core_dict_foreach(state->annotations->cf_labels, item) {
            parse_cf_label_t *label = item->value;
            if (!label || label->kind != PARSE_CF_CALL) continue;
            parse_tree_t *nodes[] = {label->cond, label->self};
            for (size_t n = 0; n < 2; n++) {
                parse_tree_t *node = nodes[n];
                th_type *type = node && !core_tree_is_leaf(node)
                    ? core_tree_node_value(node).type : nullptr;
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
        core_dict_foreach(state->annotations->cf_labels, item) {
            parse_cf_label_t *label = item->value;
            call_count += label && label->kind == PARSE_CF_CALL;
        }
    }

    n00bb_call_site_index_t call_sites = {0};
    call_site_index_init(&call_sites, grammar, units, unit_count);

    for (size_t round = 0; round <= call_count; round++) {
        core_buffer_t before = call_type_snapshot(units, unit_count);
        for (size_t i = 0; i < unit_count; i++) {
            if (resolve_ordinary_calls)
                unify_call_results(grammar, units[i], link, &call_sites,
                    false, false);
            if (resolve_member_calls)
                unify_call_results(grammar, units[i], link, &call_sites,
                    true, deferred_trait_requirements_only);
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
                                       "func-type-signature");
    parse_tree_t *param_decl = child_nt(grammar, signature, "param-decl");
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
    parse_tree_t *positional = child_nt(grammar, args, "positional-args");
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(grammar, positional, &positional_nodes);
    parse_tree_t *found = target < positional_nodes.len
        ? positional_nodes.data[target] : nullptr;
    if (!found) {
        parse_tree_t *keyword_args = child_nt(grammar, args, "keyword-args");
        core_list_t(n00bb_tree_ptr_t) keywords =
            core_list_new(n00bb_tree_ptr_t);
        if (keyword_args)
            collect_keyword_nodes(grammar, keyword_args, &keywords);
        for (size_t i = 0; !found && i < keywords.len; i++) {
            core_string_t name = call_keyword_name(keywords.data[i]);
            parse_tree_t *expression = child_nt(grammar, keywords.data[i],
                                                 "expression");
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
        core_dict_foreach(state->annotations->cf_labels, item) {
            parse_cf_label_t *label = item->value;
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
                    && !core_tree_is_leaf(receiver_node)
                    ? core_tree_node_value(receiver_node).type : nullptr;
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



