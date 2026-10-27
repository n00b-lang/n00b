#include <string.h>

#include "compiler_link.h"
#include "compiler_baked.h"
#include "interface_summaries.h"
#include "storage_placement.h"
#include "compiler_tu.h"
#include "diagnostics.h"
#include "preprocessor.h"

bool
n00bb_unit_reaches_import(core_translation_unit_t *unit,
                          core_translation_unit_t *target)
{
    core_list_t(core_translation_unit_ptr_t) pending =
        core_list_new(core_translation_unit_ptr_t);
    core_dict_t visited;
    core_dict_init(&visited, nullptr, nullptr);
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
        core_list_push(pending, core_translation_unit_import(unit, i));
    bool found = false;
    while (pending.len && !found) {
        core_translation_unit_t *candidate = pending.data[--pending.len];
        if (candidate == target) {
            found = true;
            break;
        }
        if (!core_dict_add(&visited, candidate, candidate)) continue;
        for (size_t i = 0; i < core_translation_unit_import_count(candidate);
             i++)
            core_list_push(pending, core_translation_unit_import(candidate, i));
    }
    core_dict_free(&visited);
    core_list_free(pending);
    return found;
}

bool
global_has_explicit_initializer(parse_grammar_t *grammar,
                                parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    if (parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.global-explicit-initializer")))
        return true;
    if (symbol->has_comptime_value && symbol->comptime_value) return true;
    if (symbol->decl_node
        && child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("expression"))) return true;
    for (size_t i = 0; i < symbol->definition_count; i++) {
        parse_tree_t *definition = symbol->definition_nodes[i];
        if (!child_nt(grammar, definition, CORE_STRING_STATIC("expression"))) continue;
        if (nt_is(grammar, definition, CORE_STRING_STATIC("sym-decl"))) return true;
        if (nt_is(grammar, definition, CORE_STRING_STATIC("assign-stmt"))
            && symbol_binding_node(symbol) == definition
            && is_module_initializer_use(grammar, definition, nullptr))
            return true;
    }
    return false;
}

bool
native_root_matches(core_string_t qualified, core_string_t module,
                    core_string_t name)
{
    return qualified.u8_bytes == module.u8_bytes + 1 + name.u8_bytes
        && !memcmp(qualified.data, module.data, module.u8_bytes)
        && qualified.data[module.u8_bytes] == '.'
        && !memcmp(qualified.data + module.u8_bytes + 1,
                   name.data, name.u8_bytes);
}

static void
report_variant_case_not_arm(core_translation_unit_t *unit,
                            n00bb_tu_state_t *state, parse_tree_t *node,
                            core_string_t name, th_type *variant_type,
                            parse_scope_t *arms)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t variant = variant_type && state && state->types
        ? n00bb_print_type(state->types, variant_type) : (core_string_t){0};
    if (variant.u8_bytes)
        n00bb_report(diagnostics, unit, N00B_DIAG_VARIANT_CASE_NOT_ARM,
            n00bb_node_span(node), N00B_TEXT_IS_NOT_AN_ARM_OF,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(variant)));
    else
        n00bb_report(diagnostics, unit, N00B_DIAG_VARIANT_CASE_NOT_ARM,
            n00bb_node_span(node), N00B_TEXT_IS_NOT_AN_ARM_OF_2,
            N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_label(diagnostics, N00B_TEXT_NOT_A_VARIANT_ARM, N00BB_NO_ARGS);
    if (arms) n00bb_suggest_scope_member(diagnostics, unit, nullptr, arms,
                                         name);
}

bool
resolve_variant_case_references(core_link_unit_t *link,
                                n00bb_link_analysis_t *analysis)
{
    bool all_resolved = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (!site->variant_case_candidate) continue;
            parse_tree_t *case_item = parse_tree_parent(site->node);
            while (case_item
                   && !nt_is(analysis->grammar, case_item,
                             CORE_STRING_STATIC("case-expr-item")))
                case_item = parse_tree_parent(case_item);
            parse_tree_t *expression = case_item
                ? child_nt(analysis->grammar, case_item, CORE_STRING_STATIC("expression"))
                : nullptr;
            if (!expression || !node_is_within(site->node, expression)
                || child_nt(analysis->grammar, case_item, CORE_STRING_STATIC("range-sep")))
                continue;
            parse_tree_t *switch_node = case_item
                ? parse_tree_parent(case_item) : nullptr;
            while (switch_node
                   && !nt_is(analysis->grammar, switch_node, CORE_STRING_STATIC("switch-stmt"))
                   && !nt_is(analysis->grammar, switch_node,
                             CORE_STRING_STATIC("switch-value-expr")))
                switch_node = parse_tree_parent(switch_node);
            if (!switch_node) continue;
            parse_cf_label_t *label = parse_cf_label_lookup(
                state->annotations, switch_node);
            parse_tree_t *subject = label && label->cond
                ? n00bb_simple_lvalue(label->cond) : nullptr;
            parse_sym_entry_t *subject_symbol = subject
                ? attached_symbol(analysis->grammar, unit, subject) : nullptr;
            th_type *subject_type = label && label->cond
                ? parse_tree_type(label->cond) : nullptr;
            if (!subject_type && subject_symbol)
                subject_type = subject_symbol->type;
            parse_scope_t *variant_scope = n00bb_variant_scope_for_type(
                unit, state, subject_type);
            if (!variant_scope) continue;

            core_string_t name = site->member_name;
            if (!name.data) continue;
            parse_sym_entry_t *arm = site->symbol;
            if (!n00bb_variant_arm_symbol(analysis->grammar, arm)) {
                parse_node_resolution_t resolution =
                    parse_tree_resolution(site->node);
                arm = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                    ? resolution.target.symbol : nullptr;
            }
            if (!n00bb_variant_arm_symbol(analysis->grammar, arm))
                arm = parse_symtab_lookup_in(variant_scope, name);
            if (!n00bb_variant_arm_symbol(analysis->grammar, arm)) {
                report_variant_case_not_arm(unit, state, site->node, name,
                    subject_type, variant_scope);
                all_resolved = false;
                continue;
            }
            parse_sym_entry_t *previous = site->symbol;
            if (!previous) {
                parse_node_resolution_t resolution =
                    parse_tree_resolution(site->node);
                if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
                    previous = resolution.target.symbol;
            }
            if (previous && previous != arm)
                parse_sym_remove_use(previous, site->node);
            if (!parse_tree_set_resolution_symbol(site->node, arm)
                || !parse_sym_add_use(arm, site->node)) return false;
            site->symbol = arm;

            core_translation_unit_t *owner = unit_for_symbol(
                analysis->grammar, &analysis->units, arm);
            if (!owner) return false;
            core_buffer_t key = {0};
            if (!append_link_symbol_key(analysis->grammar, owner, arm, &key))
                return false;
            core_link_reference_t *reference = nullptr;
            bool registered = core_link_unit_reference_at_scoped_kind(
                link, unit, site->node, site->scope,
                link_symbol_module(owner, arm),
                core_buffer_string(&key), (uint32_t)arm->kind,
                &reference);
            core_buffer_release(&key);
            if (!registered
                || !core_link_reference_set_source_info(link, reference,
                    link_source_location(site->node),
                    core_source_file_path(&unit->source))) return false;
        }
    }
    return all_resolved;
}

typedef th_type *th_type_ptr_t;
parse_scope_t *
record_scope_for_type(parse_grammar_t *grammar,
                      core_list_t(core_translation_unit_ptr_t) *units,
                      th_type *type,
                      core_translation_unit_t **owner_out)
{
    th_type *wanted = type ? th_resolve(type) : nullptr;
    if (!grammar || !units || !wanted || th_kind_of(wanted) != TH_NAMED)
        return nullptr;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_sym_entry_t *binding = state && state->annotations
            ? parse_symtab_nominal_type(state->annotations->symtab, wanted)
            : nullptr;
        parse_sym_entry_t *symbol = canonical_symbol(binding);
        if (!symbol || !symbol->member_scope || !symbol->type) continue;
        th_type *candidate = th_resolve(symbol->type);
        size_t wanted_args = 0;
        if (!th_named_info(wanted, nullptr, nullptr, &wanted_args)) continue;
        n00bb_signature_typevars_t *variables = parse_sym_binding(
            symbol, N00BB_SIGNATURE_TYPEVARS_KEY);
        bool same_instance = th_hash(state->types, candidate)
            == th_hash(state->types, wanted);
        bool generic_instance = symbol->decl_node
            && field_record_declaration(grammar, symbol->decl_node)
            && variables && variables->len == wanted_args;
        if (!same_instance && !generic_instance) continue;
        if (owner_out) *owner_out = symbol->owner_unit
            ? symbol->owner_unit : unit;
        return symbol->member_scope;
    }
    return nullptr;
}

static bool
bind_resolved_field_type(core_translation_unit_t *unit,
                         n00bb_tu_state_t *state, parse_tree_t *node,
                         th_type *field_type)
{
    th_type *used_as = parse_tree_type(node);
    if (n00bb_unify_node_type(state->types, node, field_type)) return true;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t text = n00bb_node_source_text(unit, node,
        n00bb_text_form(N00B_TEXT_THIS_FIELD));
    core_string_t field_text = n00bb_print_type(state->types, field_type);
    core_string_t use_text = used_as
        ? n00bb_print_type(state->types, used_as) : (core_string_t){0};
    n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_TYPE_MISMATCH,
        n00bb_node_span(node), N00B_TEXT_HAS_TYPE_BUT_IT_IS,
        N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(field_text), ARG_TEXT(use_text)));
    n00bb_report_label(diagnostics, N00B_TEXT_THIS_IS,
        N00BB_ARGS(ARG_TEXT(field_text)));
    parse_tree_set_type(node, field_type);
    return false;
}

static th_type *
field_receiver_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *receiver)
{
    th_type *type = receiver ? parse_tree_type(receiver) : nullptr;
    if (!type || th_kind_of(th_resolve(type)) == TH_VAR) {
        parse_sym_entry_t *symbol = attached_symbol(grammar, unit, receiver);
        if (symbol && symbol->type) type = symbol->type;
    }
    n00bb_tu_state_t *state = tu_state(unit);
    return n00bb_dereferenced_type(state ? state->types : nullptr, type);
}

static th_type *
referent_member_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     parse_tree_t *receiver)
{
    th_type *type = receiver ? parse_tree_type(receiver) : nullptr;
    if (!type || th_kind_of(th_resolve(type)) == TH_VAR) {
        parse_sym_entry_t *symbol = attached_symbol(grammar, unit, receiver);
        if (symbol && symbol->type) type = symbol->type;
    }
    type = type ? th_resolve(type) : nullptr;
    return type && th_kind_of(type) == TH_INDIRECTION
        ? th_ind_referent(type) : nullptr;
}

static th_type *
resolved_member_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     n00bb_tu_state_t *state, parse_tree_t *node,
                     parse_sym_entry_t *member, th_type *receiver_type)
{
    if (member->kind == PARSE_SYM_ENUM_CONST)
        return n00bb_flagset_member(member)
            && n00bb_flagset_instance_access(grammar, unit, node, member)
            ? th_name(state->types, CORE_STRING_STATIC("bool")) : member->type;
    return receiver_type
        ? th_record_lookup_in(state->types,
                              th_auto_deref(state->types, receiver_type),
                              member->name)
        : nullptr;
}

static parse_sym_entry_t *
record_symbol_for_scope(parse_scope_t *scope)
{
    for (parse_sym_entry_t *symbol = scope ? scope->first_in_scope : nullptr;
         symbol; symbol = symbol->next_in_scope)
        if (symbol->owner_type) return symbol->owner_type;
    return nullptr;
}

static void
report_member_without_fields(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *node,
                             th_type *receiver_type)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_tree_t *member_node = core_tree_child(node, 2);
    core_string_t member = first_token_text(member_node);
    core_string_t receiver = n00bb_node_source_text(unit,
        core_tree_child(node, 0), n00bb_text_form(N00B_TEXT_THIS_VALUE));
    core_string_t type_text = n00bb_print_type(state->types, receiver_type);
    n00bb_report(diagnostics, unit, N00B_DIAG_MEMBER_ON_NON_RECORD,
        n00bb_node_span(member_node), N00B_TEXT_HAS_NO_FIELD_ITS_TYPE,
        N00BB_ARGS(ARG_TEXT(receiver), ARG_TEXT(member), ARG_TEXT(type_text)));
    n00bb_report_label(diagnostics, N00B_TEXT_UNKNOWN_FIELD, N00BB_NO_ARGS);
    n00bb_report_secondary(diagnostics, nullptr,
        n00bb_node_span(core_tree_child(node, 0)), N00B_TEXT_IS_HERE,
        N00BB_ARGS(ARG_TEXT(receiver), ARG_TEXT(type_text)));
}

static void
report_value_without_referent(core_translation_unit_t *unit,
                              n00bb_tu_state_t *state, parse_tree_t *node)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_tree_t *receiver_node = core_tree_child(node, 0);
    th_type *receiver_type = parse_tree_type(receiver_node);
    core_string_t receiver = n00bb_node_source_text(unit, receiver_node,
        n00bb_text_form(N00B_TEXT_THIS_VALUE));
    core_string_t type_text = receiver_type
        ? n00bb_print_type(state->types, receiver_type) : (core_string_t){0};
    if (type_text.u8_bytes)
        n00bb_report(diagnostics, unit, N00B_DIAG_REFERENT_NEEDS_REFERENCE,
            n00bb_node_span(core_tree_child(node, 2)),
            N00B_TEXT_VALUE_NEEDS_A_REF_OR,
            N00BB_ARGS(ARG_TEXT(receiver), ARG_TEXT(type_text)));
    else
        n00bb_report(diagnostics, unit, N00B_DIAG_REFERENT_NEEDS_REFERENCE,
            n00bb_node_span(core_tree_child(node, 2)),
            N00B_TEXT_VALUE_NEEDS_A_REF_OR_2,
            N00BB_ARGS(ARG_TEXT(receiver)));
    n00bb_report_help(diagnostics, N00B_TEXT_USE_DIRECTLY,
        N00BB_ARGS(ARG_TEXT(receiver)));
}

static void
report_unknown_receiver(core_translation_unit_t *unit, parse_tree_t *node)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_tree_t *member_node = core_tree_child(node, 2);
    core_string_t member = first_token_text(member_node);
    core_string_t receiver = n00bb_node_source_text(unit,
        core_tree_child(node, 0), n00bb_text_form(N00B_TEXT_THIS_VALUE));
    n00bb_report(diagnostics, unit, N00B_DIAG_MEMBER_ON_UNKNOWN_TYPE,
        n00bb_node_span(member_node), N00B_TEXT_CANNOT_READ_BECAUSE_THE_TYPE,
        N00BB_ARGS(ARG_TEXT(member), ARG_TEXT(receiver)));
    n00bb_report_secondary(diagnostics, nullptr,
        n00bb_node_span(core_tree_child(node, 0)), N00B_TEXT_THE_TYPE_OF_IS_NOT,
        N00BB_ARGS(ARG_TEXT(receiver)));
    n00bb_report_help(diagnostics, N00B_TEXT_ANNOTATE_THE_VARIABLE_OR_PARAMETER,
        N00BB_ARGS(ARG_TEXT(receiver)));
}

static void
report_hidden_field(core_translation_unit_t *unit, n00bb_tu_state_t *state,
                    parse_tree_t *node, th_type *receiver_type,
                    parse_sym_entry_t *field)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_tree_t *member_node = core_tree_child(node, 2);
    core_string_t type_text = n00bb_print_type(state->types, receiver_type);
    core_string_t module = field->owner_unit
        ? field->owner_unit->module.identifier : (core_string_t){0};
    n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_NOT_VISIBLE,
        n00bb_node_span(member_node), N00B_TEXT_FIELD_OF_IS_PRIVATE_TO,
        N00BB_ARGS(ARG_TEXT(field->name), ARG_TEXT(type_text),
                   ARG_TEXT(module)));
    n00bb_report_label(diagnostics, N00B_TEXT_PRIVATE_FIELD, N00BB_NO_ARGS);
    n00bb_diag_declared_here(diagnostics, field,
        N00B_TEXT_FIELD_IS_DECLARED_HERE);
    n00bb_report_help(diagnostics, N00B_TEXT_USE_A_FUNCTION_OF_MODULE,
        N00BB_ARGS(ARG_TEXT(module)));
}

bool
resolve_inferred_field_references(core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis)
{
    bool all_resolved = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types || !state->annotations) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            parse_tree_t *node = site->node;
            if (!site->dotted_path || site->unresolved
                || !nt_is(analysis->grammar, node, CORE_STRING_STATIC("postfix-expr"))
                || core_tree_num_children(node) < 3
                || !parse_tree_leaf_is(core_tree_child(node, 1),
                                       CORE_STRING_STATIC("."))
                || n00bb_is_trait_dispatch_reference(analysis->grammar, unit, node))
                continue;
            if (variant_assignment_arm(analysis->grammar, unit, node))
                continue;
            if (referent_access_node(analysis->grammar, node)) {
                th_type *referent = referent_member_type(analysis->grammar,
                    unit, core_tree_child(node, 0));
                if (!referent) {
                    report_value_without_referent(unit, state, node);
                    all_resolved = false;
                    continue;
                }
                if (!bind_resolved_field_type(unit, state, node, referent))
                    all_resolved = false;
                continue;
            }
            parse_node_resolution_t existing = parse_tree_resolution(node);
            bool resolved_to_receiver = existing.kind
                    == PARSE_NODE_RESOLUTION_SYMBOL
                && existing.target.symbol
                && (existing.target.symbol->kind == PARSE_SYM_TYPEDEF
                    || (existing.target.symbol->kind == PARSE_SYM_VARIABLE
                        && !existing.target.symbol->is_field))
                && attached_symbol(analysis->grammar, unit,
                       core_tree_child(node, 0)) == existing.target.symbol;
            if (existing.kind == PARSE_NODE_RESOLUTION_SYMBOL
                && !resolved_to_receiver) {
                parse_sym_entry_t *field = existing.target.symbol;
                if (field && field->type && (field->is_field
                        || field->kind == PARSE_SYM_ENUM_CONST)) {
                    parse_tree_t *receiver = core_tree_child(node, 0);
                    th_type *receiver_type = field->is_field
                        ? field_receiver_type(analysis->grammar, unit,
                                              receiver) : nullptr;
                    th_type *member_type = resolved_member_type(
                        analysis->grammar, unit, state, node, field,
                        receiver_type);
                    if (!member_type) {
                        n00bb_internal_error(
                            core_translation_unit_diagnostics(unit), unit,
                            n00bb_node_span(node), "resolving field types",
                            "field `%.*s` has no type in its record",
                            (int)field->name.u8_bytes, field->name.data);
                        return false;
                    }
                    if (!bind_resolved_field_type(unit, state, node,
                                                  member_type))
                        all_resolved = false;
                }
                continue;
            }
            if (existing.kind == PARSE_NODE_RESOLUTION_SCOPE) continue;
            core_string_t member = first_token_text(core_tree_child(node, 2));
            if (site->symbol
                && n00bb_variant_arm_symbol(analysis->grammar, site->symbol)
                && core_string_eq(site->symbol->name, member)) {
                parse_tree_t *receiver_node = core_tree_child(node, 0);
                parse_sym_entry_t *receiver = attached_symbol(
                    analysis->grammar, unit, receiver_node);
                th_type *receiver_type = parse_tree_type(receiver_node);
                if (!receiver_type && receiver) receiver_type = receiver->type;
                th_type *payload_type = n00bb_variant_arm_payload_type(
                    state->types, receiver_type, site->symbol);
                if (!payload_type) return false;
                parse_tree_set_type(node, payload_type);
                if (!parse_tree_set_resolution_symbol(node, site->symbol))
                    return false;
                continue;
            }
            if (variant_payload_arm(analysis->grammar, unit, node)) continue;
            parse_tree_t *receiver_node = core_tree_child(node, 0);
            th_type *receiver_type = field_receiver_type(
                analysis->grammar, unit, receiver_node);
            if (th_is_poison(receiver_type)) {
                if (!bind_resolved_field_type(unit, state, node,
                                              receiver_type))
                    all_resolved = false;
                continue;
            }
            if (!receiver_type || th_kind_of(receiver_type) == TH_VAR) {
                report_unknown_receiver(unit, node);
                all_resolved = false;
                continue;
            }
            if (th_kind_of(receiver_type) == TH_RECORD) {
                core_string_t member = first_token_text(
                    core_tree_child(node, 2));
                th_type *member_type = record_lookup_text(state->types,
                    receiver_type, member);
                if (!member_type) {
                    n00bb_report_missing_field(unit, state->types,
                        core_tree_child(node, 2), receiver_type, member,
                        nullptr, nullptr);
                    all_resolved = false;
                    continue;
                }
                if (n00bb_assignment_target(analysis->grammar, node,
                                            nullptr)) {
                    core_diagnostics_t *diagnostics =
                        core_translation_unit_diagnostics(unit);
                    if (n00bb_report(diagnostics, unit,
                            N00B_DIAG_TUPLE_IMMUTABLE, n00bb_node_span(node),
                            N00B_TEXT_TUPLE_IMMUTABLE, N00BB_NO_ARGS))
                        n00bb_report_help(diagnostics,
                            N00B_TEXT_TUPLE_IMMUTABLE_HELP, N00BB_NO_ARGS);
                    all_resolved = false;
                    continue;
                }
                if (!bind_resolved_field_type(unit, state, node,
                                              member_type))
                    all_resolved = false;
                continue;
            }
            if (th_kind_of(receiver_type) != TH_NAMED) {
                report_member_without_fields(unit, state, node,
                                             receiver_type);
                all_resolved = false;
                continue;
            }

            core_translation_unit_t *owner = nullptr;
            parse_scope_t *record_scope = record_scope_for_type(
                analysis->grammar, &analysis->units, receiver_type, &owner);
            if (!record_scope || !owner) {
                report_member_without_fields(unit, state, node,
                                             receiver_type);
                all_resolved = false;
                continue;
            }
            parse_sym_entry_t *field = parse_symtab_lookup_in(record_scope,
                                                               member);
            bool external_field = field && field->owner_type
                && tu_state(owner)->interface_module;
            bool enum_constant = field && field->kind == PARSE_SYM_ENUM_CONST;
            if (!field || (!field->is_field && !enum_constant)
                || (!field->decl_node && !external_field)) {
                n00bb_report_missing_field(unit, state->types,
                    core_tree_child(node, 2), receiver_type, member,
                    record_scope, record_symbol_for_scope(record_scope));
                all_resolved = false;
                continue;
            }
            parse_sym_entry_t *owner_type = field->owner_type;
            if (!field_access_is_visible(unit, owner, owner_type, field)) {
                report_hidden_field(unit, state, node, receiver_type, field);
                all_resolved = false;
                continue;
            }

            th_type *member_type = resolved_member_type(analysis->grammar,
                unit, state, node, field, receiver_type);
            if (!member_type) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, n00bb_node_span(node), "resolving field types",
                    "field `%.*s` has no type in its record",
                    (int)field->name.u8_bytes, field->name.data);
                return false;
            }
            if (!bind_resolved_field_type(unit, state, node,
                                          member_type)) {
                all_resolved = false;
                continue;
            }
            if (site->symbol && site->symbol != field)
                parse_sym_remove_use(site->symbol, node);
            if (!parse_tree_set_resolution_symbol(node, field)
                || !parse_sym_add_use(field, node)) return false;
            site->symbol = field;

            core_buffer_t key = {0};
            if (!append_link_symbol_key(analysis->grammar, owner, field,
                                        &key)) return false;
            core_link_reference_t *reference = nullptr;
            bool registered = core_link_unit_reference_at_scoped_kind(
                link, unit, node, site->scope,
                link_symbol_module(owner, field),
                core_buffer_string(&key), (uint32_t)field->kind,
                &reference);
            core_buffer_release(&key);
            if (!registered
                || !core_link_reference_set_source_info(link,
                    reference, link_source_location(node),
                    core_source_file_path(&unit->source))) return false;
        }
    }
    return all_resolved;
}

static void
report_reserved_declaration(core_translation_unit_t *unit,
                            parse_sym_entry_t *symbol, core_string_t hint)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t at = n00bb_symbol_location(symbol);
    int size = (int)symbol->name.u8_bytes;
    if (n00bb_builtin_member(symbol->name)) {
        n00bb_report(diagnostics, unit, N00B_DIAG_RESERVED_NAME, at,
            N00B_TEXT_IS_THE_BUILT_IN_DEREFERENCE,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(symbol->name.data,
                                                   (size_t)(size)))));
        return;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_RESERVED_NAME, at,
        N00B_TEXT_IS_NOT_A_LANGUAGE_HOOK,
        N00BB_ARGS(ARG_TEXT(core_string_borrow(symbol->name.data,
                                               (size_t)(size)))));
    if (hint.data)
        n00bb_report_help(diagnostics, N00B_TEXT_DID_YOU_MEAN_THE_HOOK,
            N00BB_ARGS(ARG_TEXT(hint)));
    else
        n00bb_report_help(diagnostics, N00B_TEXT_CHOOSE_A_NAME_THAT_DOES,
            N00BB_NO_ARGS);
}

static bool
spells_name(parse_tree_t *node, core_string_t name)
{
    if (!node) return false;
    if (core_tree_is_leaf(node))
        return core_string_eq(first_token_text(node), name);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (spells_name(core_tree_child(node, i), name)) return true;
    return false;
}

bool
diagnose_reserved_declarations(core_translation_unit_t *unit,
                               const th_env *environment)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->interface_module || !state->annotations
        || !state->annotations->symtab) return true;
    bool system = n00bb_system_module(unit->module.identifier)
        || n00bb_generated_source(core_source_file_path(&unit->source));
    bool clean = true;
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (!symbol->decl_node || symbol->owner_unit != unit
                || !n00bb_reserved_name(symbol->name)
                || !spells_name(symbol->decl_node, symbol->name)) continue;
            bool member = n00bb_builtin_member(symbol->name);
            if (!member && (system || (symbol->kind == PARSE_SYM_FUNCTION
                    && n00bb_language_hook(environment, symbol->name))))
                continue;
            report_reserved_declaration(unit, symbol, member
                ? (core_string_t){0}
                : n00bb_nearest_language_hook(environment, symbol->name));
            clean = false;
        }
    return clean;
}

core_list_decl(th_type_ptr_t);
bool
diagnose_known_missing_record_fields(n00bb_link_analysis_t *analysis)
{
    bool clean = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types || !state->annotations) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            parse_tree_t *node = site->node;
            if (!site->dotted_path || site->unresolved
                || !nt_is(analysis->grammar, node, CORE_STRING_STATIC("postfix-expr"))
                || core_tree_num_children(node) < 3
                || !parse_tree_leaf_is(core_tree_child(node, 1),
                                       CORE_STRING_STATIC("."))
                || n00bb_is_trait_dispatch_reference(analysis->grammar, unit, node)
                || variant_payload_arm(analysis->grammar, unit, node)
                || variant_assignment_arm(analysis->grammar, unit, node)
                || referent_access_node(analysis->grammar, node))
                continue;
            parse_tree_t *receiver = core_tree_child(node, 0);
            th_type *receiver_type = receiver ? parse_tree_type(receiver)
                                              : nullptr;
            th_type *resolved_receiver = receiver_type
                ? th_resolve(receiver_type) : nullptr;
            if (!resolved_receiver
                || th_kind_of(resolved_receiver) == TH_VAR) {
                parse_sym_entry_t *symbol = attached_symbol(
                    analysis->grammar, unit, receiver);
                if (symbol && symbol->type) receiver_type = symbol->type;
            }
            receiver_type = n00bb_dereferenced_type(state->types,
                                                    receiver_type);
            if (!receiver_type || th_kind_of(receiver_type) != TH_NAMED)
                continue;

            core_translation_unit_t *owner = nullptr;
            parse_scope_t *scope = record_scope_for_type(
                analysis->grammar, &analysis->units, receiver_type, &owner);
            if (!scope || !owner) continue;
            core_string_t member = first_token_text(core_tree_child(node, 2));
            parse_sym_entry_t *field = parse_symtab_lookup_in(scope, member);
            if (field && (field->is_field
                          || field->kind == PARSE_SYM_ENUM_CONST)) continue;
            n00bb_report_missing_field(unit, state->types,
                core_tree_child(node, 2), receiver_type, member, scope,
                record_symbol_for_scope(scope));
            clean = false;
        }
    }
    return clean;
}
core_list_decl(th_member);

th_type *
link_semantic_type_for_symbol(parse_grammar_t *grammar,
                              core_translation_unit_t *owner,
                              parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(owner);
    if (!grammar || !state || !state->types || !symbol) return nullptr;
    parse_tree_t *declaration = symbol->decl_node;
    if (!is_named_type_declaration(grammar, symbol))
        return symbol->type ? th_resolve(symbol->type) : nullptr;
    if (nt_is(grammar, declaration, CORE_STRING_STATIC("alias-stmt"))) {
        if (alias_declaration_is_transparent(grammar, declaration))
            return declared_type_from_node(grammar, owner, declaration);
        core_buffer_t nominal_name = {0};
        append_module_type_name(owner, symbol->name, &nominal_name);
        th_type *nominal = nominal_name.data
            ? th_name(state->types, core_buffer_string(&nominal_name))
            : nullptr;
        core_buffer_release(&nominal_name);
        return nominal;
    }

    if (nt_is(grammar, declaration, CORE_STRING_STATIC("trait-decl"))) {
        if (!symbol->member_scope) return nullptr;
        if (symbol->type && th_kind_of(th_resolve(symbol->type)) == TH_TRAIT)
            return symbol->type;
        core_buffer_t qualified = {0};
        append_module_type_name(owner, symbol->name, &qualified);
        core_list_t(th_member) methods = core_list_new(th_member);
        core_list_t(n00bb_typevar_binding_t) variables =
            core_list_new(n00bb_typevar_binding_t);
        bool valid = qualified.data != nullptr;
        for (parse_sym_entry_t *method = symbol->member_scope->first_in_scope;
             valid && method; method = method->next_in_scope) {
            if (method->kind != PARSE_SYM_FUNCTION) continue;
            if (!method->type) { valid = false; break; }
            th_type *contract = method->callable_contract
                ? method->callable_contract
                : th_env_signature_contract(state->types, method->type,
                    method->error_type ? method->error_type
                                       : th_name(state->types, CORE_STRING_STATIC("nil")));
            if (!contract) { valid = false; break; }
            th_type **free_variables = nullptr;
            size_t count = th_free_vars(state->types, contract,
                                        &free_variables);
            th_type **replacements = count
                ? core_alloc_array(th_type *, count) : nullptr;
            if (count && !replacements) { valid = false; break; }
            for (size_t i = 0; i < count; i++) {
                core_string_t key = th_var_name(free_variables[i]);
                replacements[i] = free_variables[i];
                if (!key.data) continue;
                bool found = false;
                for (size_t j = 0; j < variables.len; j++) {
                    if (!core_string_eq(variables.data[j].name, key)) continue;
                    replacements[i] = variables.data[j].type;
                    found = true;
                    break;
                }
                if (!found)
                    core_list_push(variables,
                        ((n00bb_typevar_binding_t){key, free_variables[i]}));
            }
            th_type *shared = count
                ? th_type_clone_replacing(state->types, contract,
                    (const th_type *const *)free_variables, replacements,
                    count) : contract;
            core_free(replacements);
            if (!shared) { valid = false; break; }
            core_list_push(methods, ((th_member){
                .name = method->name,
                .type = shared,
            }));
        }
        n00bb_declared_trait_requirements_t *requirements =
            parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.trait.requirements"));
        th_type *trait = valid && requirements
            ? th_trait(state->types, core_buffer_string(&qualified), methods.data,
                methods.len, requirements->data, requirements->len)
            : nullptr;
        core_list_free(methods);
        core_list_free(variables);
        core_buffer_release(&qualified);
        return trait;
    }

    core_buffer_t qualified = {0};
    append_module_type_name(owner, symbol->name, &qualified);
    th_type *type = nullptr;
    if (nt_is(grammar, declaration, CORE_STRING_STATIC("variant-decl"))) {
        type = th_variant(state->types, core_buffer_string(&qualified),
                          nullptr, 0);
    }
    else {
        type = th_name(state->types, core_buffer_string(&qualified));
    }
    core_buffer_release(&qualified);
    return type;
}

static bool
symbol_list_contains(core_list_t(parse_sym_entry_ptr_t) *symbols,
                     parse_sym_entry_t *symbol)
{
    for (size_t i = 0; i < symbols->len; i++)
        if (symbols->data[i] == symbol) return true;
    return false;
}

static bool
transparent_alias_cycle_from(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    parse_sym_entry_t *symbol,
    core_list_t(parse_sym_entry_ptr_t) *active,
    core_list_t(parse_sym_entry_ptr_t) *visited)
{
    if (symbol_list_contains(active, symbol)) return true;
    if (symbol_list_contains(visited, symbol)) return false;
    core_list_push(*active, symbol);
    bool cycle = false;
    for (size_t i = 0; i < units->len && !cycle; i++) {
        n00bb_tu_state_t *state = tu_state(units->data[i]);
        if (!state || !symbol->decl_node) continue;
        for (size_t j = 0; j < state->link_use_sites.len && !cycle; j++) {
            parse_tree_t *reference = state->link_use_sites.data[j].node;
            if (!is_type_reference_node(grammar, reference)
                || !node_is_within(reference, symbol->decl_node)) continue;
            parse_node_resolution_t resolution =
                parse_tree_resolution(reference);
            parse_sym_entry_t *dependency =
                resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                    ? resolution.target.symbol : nullptr;
            if (!dependency || !dependency->decl_node
                || !nt_is(grammar, dependency->decl_node, CORE_STRING_STATIC("alias-stmt"))
                || !alias_declaration_is_transparent(grammar,
                                                     dependency->decl_node))
                continue;
            cycle = transparent_alias_cycle_from(grammar, units, dependency,
                                                   active, visited);
        }
    }
    active->len--;
    if (!cycle) core_list_push(*visited, symbol);
    return cycle;
}

static bool
transparent_alias_has_cycle(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    parse_sym_entry_t *symbol)
{
    core_list_t(parse_sym_entry_ptr_t) active =
        core_list_new(parse_sym_entry_ptr_t);
    core_list_t(parse_sym_entry_ptr_t) visited =
        core_list_new(parse_sym_entry_ptr_t);
    bool cycle = transparent_alias_cycle_from(grammar, units, symbol,
                                               &active, &visited);
    core_list_free(active);
    core_list_free(visited);
    return cycle;
}

bool
module_type_universe_is_shared(
    core_list_t(core_translation_unit_ptr_t) *units)
{
    th_ctx *universe = nullptr;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!unit || !state || !state->types || !state->annotations
            || !state->annotations->symtab) {
            if (unit)
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, (core_diag_location_t){}, "checking modules",
                    "module `%.*s` reached checking without its declarations",
                    (int)unit->module.identifier.u8_bytes,
                    unit->module.identifier.data);
            return false;
        }
        if (!universe) universe = state->types;
        else if (state->types != universe) {
            n00bb_internal_error(core_translation_unit_diagnostics(unit),
                unit, (core_diag_location_t){}, "checking modules",
                "module `%.*s` was checked with a separate set of types",
                (int)unit->module.identifier.u8_bytes,
                unit->module.identifier.data);
            return false;
        }
    }
    return true;
}

static void
report_value_used_as_type(core_translation_unit_t *unit,
                          parse_tree_t *reference, parse_sym_entry_t *value)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t text = n00bb_node_source_text(unit, reference,
                                                value->name);
    n00bb_text_t kind = value->kind == PARSE_SYM_FUNCTION
        ? N00B_TEXT_A_FUNCTION
        : value->kind == PARSE_SYM_PARAM ? N00B_TEXT_A_PARAMETER
        : value->mutability == PARSE_SYM_CONST ? N00B_TEXT_A_CONSTANT
                                               : N00B_TEXT_A_VARIABLE;
    n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_USED_AS_TYPE,
        n00bb_node_span(reference), N00B_TEXT_IS_NOT_A_TYPE,
        N00BB_ARGS(ARG_TEXT(text), ARG_PHRASE_ID(kind)));
    n00bb_report_label(diagnostics, N00B_TEXT_A_TYPE_IS_REQUIRED_HERE,
        N00BB_NO_ARGS);
    n00bb_diag_declared_here(diagnostics, value, N00B_TEXT_IS_DECLARED_HERE);
}

static void
report_unknown_type(core_translation_unit_t *unit, parse_scope_t *scope,
                    parse_tree_t *reference, core_string_t name)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    n00bb_report(diagnostics, unit, N00B_DIAG_UNKNOWN_TYPE,
        n00bb_node_span(reference), N00B_TEXT_CANNOT_FIND_TYPE_IN_THIS,
        N00BB_ARGS(ARG_TEXT(core_string_borrow(name.data ? name.data : "",
                                               (size_t)(name.u8_bytes)))));
    n00bb_report_label(diagnostics, N00B_TEXT_UNKNOWN_TYPE, N00BB_NO_ARGS);
    if (!n00bb_suggest_visible_name(diagnostics, unit, scope, name,
                                    N00BB_NAMES_TYPES))
        (void)n00bb_explain_hidden_name(diagnostics, unit, scope, name, true);
}

static void
report_type_reference_conflict(core_translation_unit_t *unit, th_ctx *types,
                               parse_tree_t *reference, th_type *declared,
                               th_type *required)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t text = n00bb_node_source_text(unit, reference,
        n00bb_text_form(N00B_TEXT_THIS_TYPE));
    core_string_t declared_text = declared
        ? n00bb_print_type(types, declared) : (core_string_t){0};
    core_string_t required_text = required
        ? n00bb_print_type(types, required) : (core_string_t){0};
    n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_REFERENCE_CONFLICT,
        n00bb_node_span(reference), N00B_TEXT_IS_BUT_IS_REQUIRED_HERE,
        N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(declared_text),
                   ARG_TEXT(required_text)));
}

bool
finalize_named_declaration_types(parse_grammar_t *grammar,
                                 core_list_t(core_translation_unit_ptr_t) *units)
{
    bool unresolved_types = false;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!state || !symtab) return false;
        for (size_t site_ix = 0; site_ix < state->link_use_sites.len;
             site_ix++) {
            parse_tree_t *reference = state->link_use_sites.data[site_ix].node;
            if (!is_type_reference_node(grammar, reference)
                || dotted_symbol_path_node(grammar, reference)) continue;
            parse_node_resolution_t resolution =
                parse_tree_resolution(reference);
            bool value_reference = resolution.kind
                    == PARSE_NODE_RESOLUTION_SYMBOL
                && resolution.target.symbol->kind != PARSE_SYM_MODULE;
            if (value_reference
                && is_named_type_symbol(grammar, resolution.target.symbol))
                continue;
            n00bb_link_use_site_t *site = &state->link_use_sites.data[site_ix];
            parse_scope_t *scope = site->scope;
            core_string_t name = site->member_name;
            parse_sym_entry_t *symbol = lookup_named_type_reference(grammar,
                state, symtab, scope, name);
            if (!symbol && value_reference
                && !(name.data && builtin_type_name(name))) {
                report_value_used_as_type(unit, reference,
                                          resolution.target.symbol);
                unresolved_types = true;
                continue;
            }
            if (!symbol) {
                if (!name.data || !builtin_type_name(name)) {
                    report_unknown_type(unit, scope, reference, name);
                    unresolved_types = true;
                    continue;
                }
                th_type *builtin = name.data
                    ? th_name(state->types,
                        canonical_builtin_type_name(name)) : nullptr;
                th_type *previous = parse_tree_type(reference);
                th_type *resolved = previous && builtin
                    ? th_unify_in_place(state->types, previous, builtin)
                    : builtin;
                if (!resolved) continue;
                parse_tree_set_type(reference, resolved);
                propagate_resolved_type_reference(grammar, unit, reference,
                                                  resolved);
                refresh_cached_type_path(grammar, unit, reference);
                continue;
            }
            if (!parse_tree_set_resolution_symbol(reference, symbol)
                || !parse_sym_add_use(symbol, reference)
                || !bind_array_shape_type_source(grammar, reference, symbol)
                || !bind_array_shape_alias_source(grammar, reference,
                                                  symbol)) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, n00bb_node_span(reference), "resolving type names",
                    "could not record the use of type `%.*s`",
                    (int)symbol->name.u8_bytes, symbol->name.data);
                return false;
            }
            th_type *target_type = is_trait_type_symbol(grammar, symbol)
                ? trait_type_reference(grammar, unit, unit, symbol, nullptr)
                : symbol->type;
            th_type *previous_type = parse_tree_type(reference);
            th_type *resolved_type = previous_type && target_type
                ? th_unify_in_place(state->types, previous_type, target_type)
                : target_type ? target_type : previous_type;
            if (previous_type && target_type && !resolved_type) {
                report_type_reference_conflict(unit, state->types, reference,
                                               target_type, previous_type);
                unresolved_types = true;
                continue;
            }
            if (resolved_type)
                parse_tree_set_type(reference, resolved_type);
            if (resolved_type)
                propagate_resolved_type_reference(grammar, unit, reference,
                                                  resolved_type);
            refresh_cached_type_path(grammar, unit, reference);
        }
    }
    if (unresolved_types) return false;

    bool progress;
    do {
        progress = false;
        for (size_t i = 0; i < units->len; i++) {
            core_translation_unit_t *unit = units->data[i];
            n00bb_tu_state_t *state = tu_state(unit);
            if (!state || !state->types || !state->annotations
                || !state->annotations->symtab) return false;
            for (parse_scope_t *scope =
                     state->annotations->symtab->all_scopes;
                 scope; scope = scope->all_next) {
                for (parse_sym_entry_t *symbol = scope->first_in_scope;
                     symbol; symbol = symbol->next_in_scope) {
                    if (!is_named_type_declaration(grammar, symbol))
                        continue;
                    th_type *type = link_semantic_type_for_symbol(
                        grammar, unit, symbol);
                    if (!type) continue;
                    th_type *previous = symbol->type;
                    bool changed = !previous
                        || th_hash(state->types, th_resolve(previous))
                            != th_hash(state->types, th_resolve(type));
                    if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                        && alias_declaration_is_transparent(grammar,
                            symbol->decl_node)
                        && previous) {
                        type = th_unify_in_place(state->types, previous,
                                                 type);
                        if (!type) {
                            report_type_reference_conflict(unit,
                                state->types, symbol->decl_node,
                                link_semantic_type_for_symbol(grammar, unit,
                                                              symbol),
                                previous);
                            return false;
                        }
                    }
                    symbol->type = type;
                    parse_tree_set_type(symbol->decl_node, type);
                    if (changed) progress = true;
                    for (size_t use_ix = 0; use_ix < symbol->use_count;
                         use_ix++) {
                        parse_tree_t *reference = symbol->use_nodes[use_ix];
                        if (!is_type_reference_node(grammar, reference))
                            continue;
                        parse_node_resolution_t resolution =
                            parse_tree_resolution(reference);
                        if (resolution.kind
                                != PARSE_NODE_RESOLUTION_SYMBOL
                            || resolution.target.symbol != symbol)
                            continue;
                        parse_scope_t *reference_scope =
                            parse_tree_scope(reference);
                        core_translation_unit_t *owner = reference_scope
                            ? reference_scope->owner_unit : nullptr;
                        n00bb_tu_state_t *owner_state = tu_state(owner);
                        if (!owner_state || !owner_state->types)
                            return false;
                        th_type *reference_type =
                            is_trait_type_symbol(grammar, symbol)
                            ? trait_type_reference(grammar, owner, unit,
                                                   symbol, nullptr)
                            : type;
                        if (!reference_type) continue;
                        th_type *previous_reference_type =
                            parse_tree_type(reference);
                        bool reference_changed =
                            !previous_reference_type
                            || th_hash(owner_state->types,
                                th_resolve(previous_reference_type))
                                != th_hash(owner_state->types,
                                    th_resolve(reference_type));
                        th_type *resolved_reference_type =
                            previous_reference_type
                            ? th_unify_in_place(owner_state->types,
                                previous_reference_type, reference_type)
                            : reference_type;
                        if (!resolved_reference_type) {
                            report_type_reference_conflict(owner,
                                owner_state->types, reference,
                                reference_type, previous_reference_type);
                            return false;
                        }
                        parse_tree_set_type(reference,
                                            resolved_reference_type);
                        propagate_resolved_type_reference(grammar, owner,
                            reference, resolved_reference_type);
                        refresh_cached_type_path(grammar, owner,
                                                 reference);
                        if (reference_changed) progress = true;
                    }
                }
            }
        }
    } while (progress);

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types || !state->annotations
            || !state->annotations->symtab) return false;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (!is_named_type_declaration(grammar, symbol)) continue;
                if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && alias_declaration_is_transparent(grammar,
                                                        symbol->decl_node)
                    && transparent_alias_has_cycle(grammar, units, symbol)) {
                    core_diagnostics_t *diagnostics =
                        core_translation_unit_diagnostics(unit);
                    n00bb_report(diagnostics, unit, N00B_DIAG_CYCLIC_ALIAS,
                        n00bb_symbol_location(symbol),
                        N00B_TEXT_TYPE_ALIAS_REFERS_BACK_TO,
                        N00BB_ARGS(ARG_TEXT(symbol->name)));
                    n00bb_report_help(diagnostics,
                        N00B_TEXT_BREAK_THE_CYCLE_WITH_A_2,
                        N00BB_NO_ARGS);
                    return false;
                }
                if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && alias_declaration_is_transparent(grammar,
                                                        symbol->decl_node))
                    symbol->adt_kind = CORE_STRING_STATIC("alias");
                if (!symbol->type) {
                    n00bb_internal_error(
                        core_translation_unit_diagnostics(unit), unit,
                        link_source_location(symbol->decl_node),
                        "resolving type declarations",
                        "type `%.*s` has no type after resolution",
                        (int)symbol->name.u8_bytes, symbol->name.data);
                    return false;
                }
                if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && !alias_declaration_is_transparent(grammar,
                                                         symbol->decl_node)) {
                    th_type *representation = declared_type_from_node(
                        grammar, unit, symbol->decl_node);
                    if (!representation || !th_type_define(state->types,
                            symbol->type, representation)) {
                        n00bb_internal_error(
                            core_translation_unit_diagnostics(unit), unit,
                            link_source_location(symbol->decl_node),
                            "resolving type declarations",
                            "alias `%.*s` has no representation type",
                            (int)symbol->name.u8_bytes, symbol->name.data);
                        return false;
                    }
                    symbol->adt_kind = CORE_STRING_STATIC("nominal");
                }
            }
        }
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                parse_tree_t *origin_declaration =
                    canonical_symbol(symbol)->decl_node;
                if (symbol->kind == PARSE_SYM_TYPEDEF && symbol->member_scope
                    && symbol->type
                    && (!origin_declaration || !nt_is(grammar,
                        origin_declaration, CORE_STRING_STATIC("alias-stmt"))
                        || !alias_declaration_is_transparent(grammar,
                            origin_declaration))
                    && th_named_info(th_resolve(symbol->type), nullptr,
                                     nullptr, nullptr)
                    && !parse_symtab_bind_nominal_type(
                        state->annotations->symtab, symbol->type,
                        canonical_symbol(symbol)))
                    return false;
                parse_sym_entry_t *owner = symbol->owner_type;
                if (!owner || !owner->decl_node
                    || !nt_is(grammar, owner->decl_node, CORE_STRING_STATIC("enum-stmt")))
                    continue;
                if (!owner->type) return false;
                symbol->type = owner->type;
                symbol->mutability = PARSE_SYM_CONST;
                parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                    PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_COMPTIME);
                parse_sym_set_storage_duration(symbol,
                    PARSE_SYM_STORAGE_COMPTIME);
                if (symbol->decl_node)
                    parse_tree_set_type(symbol->decl_node, owner->type);
            }
        }
    }
    return true;
}

bool
field_access_is_visible(core_translation_unit_t *use_unit,
                        core_translation_unit_t *owner_unit,
                        parse_sym_entry_t *owner_type,
                        parse_sym_entry_t *field)
{
    if (!use_unit || !owner_unit || !owner_type || !field) return false;
    if (use_unit == owner_unit) return true;
    if (member_is_private(field, owner_type)) return false;
    return n00bb_same_package(use_unit, owner_unit)
        || field->kind == PARSE_SYM_ENUM_CONST
        || !parse_sym_binding(owner_type,
               CORE_STRING_STATIC("n00bb.opaque-type"));
}

bool
module_member_is_call(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        const parse_annotation_t *call = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL) : nullptr;
        if (!call) continue;
        parse_tree_t *callee = parse_tree_resolve_child_ref(
            grammar, at, call->name_ref);
        return callee && node_is_within(node, callee);
    }
    return false;
}

static bool
call_binds_candidate(parse_grammar_t *grammar, parse_sym_entry_t *candidate,
                     th_type *signature, size_t positional,
                     core_list_t(n00bb_tree_ptr_t) *keywords)
{
    size_t total = th_func_nparams(signature);
    if (positional > total) return false;
    candidate = canonical_symbol(candidate);
    parse_tree_t *formals = candidate->decl_node
        ? child_nt(grammar, child_nt(grammar, candidate->decl_node,
                                     CORE_STRING_STATIC("func-type-signature")),
                   CORE_STRING_STATIC("param-decl"))
        : nullptr;
    bool *bound = core_alloc_array(bool, total ? total : 1);
    for (size_t i = 0; i < positional; i++) bound[i] = true;
    bool binds = true;
    for (size_t k = 0; binds && k < keywords->len; k++) {
        size_t index = 0;
        core_string_t name = call_keyword_name(keywords->data[k]);
        binds = name.data && function_parameter_named(grammar, candidate,
                    formals, name, &index)
            && index < total && !bound[index];
        if (binds) bound[index] = true;
    }
    for (size_t i = 0; binds && i < total; i++) {
        uint8_t flags = 0;
        binds = bound[i] || (function_parameter_info(grammar, candidate,
                    formals, i, nullptr, nullptr, &flags, nullptr)
                && (flags & 1u));
    }
    core_free(bound);
    return binds;
}

static bool
overload_argument_converts(n00bb_tu_state_t *state, parse_tree_t *argument,
                           th_type *type, th_type *parameter)
{
    th_type *element = nullptr;
    return n00bb_value_coerces(state->types, type, parameter)
        || (n00bb_option_type_element(parameter, &element)
            && (th_is_nil(type)
                || th_unify_probe(state->types, type, element)))
        || (parse_tree_integer_literal_origin(argument)
            && is_integer_type(th_resolve(parameter)));
}

typedef struct {
    parse_sym_entry_t *symbol;
    th_type *pattern;
    size_t conversions;
} n00bb_overload_fit_t;
core_list_decl(n00bb_overload_fit_t);

static size_t
overload_typevar_ordinal(parse_sym_entry_t *symbol, th_type *subject)
{
    n00bb_signature_typevars_t *variables = symbol
        ? parse_sym_binding(canonical_symbol(symbol),
                            N00BB_SIGNATURE_TYPEVARS_KEY) : nullptr;
    subject = subject ? th_resolve(subject) : nullptr;
    for (size_t i = 0; variables && i < variables->len; i++)
        if (th_resolve(variables->data[i].type) == subject) return i;
    return SIZE_MAX;
}

static bool
overload_requirement_type_equal(th_ctx *types, th_type *left, th_type *right)
{
    const uint8_t *left_encoding = nullptr, *right_encoding = nullptr;
    size_t left_size = 0, right_size = 0;
    return left && right
        && th_encode(types, left, &left_encoding, &left_size)
        && th_encode(types, right, &right_encoding, &right_size)
        && left_size == right_size
        && !memcmp(left_encoding, right_encoding, left_size);
}

static bool
overload_requirement_equal(th_ctx *types,
                           const th_trait_requirement *left,
                           const th_trait_requirement *right)
{
    if (!left || !right || left->kind != right->kind) return false;
    if (left->kind == TH_CON_IMPLEMENTS)
        return core_string_eq(left->iface, right->iface);
    if (left->kind == TH_CON_EXCLUDES)
        return overload_requirement_type_equal(types, left->target,
                                                right->target);
    return false;
}

/* A pattern with fewer declared requirements accepts at least everything
 * accepted by the otherwise equivalent pattern with a superset of them. */
static bool
overload_constraints_subsume(th_ctx *types, parse_sym_entry_t *general,
                             parse_sym_entry_t *specific)
{
    general = canonical_symbol(general);
    specific = canonical_symbol(specific);
    n00bb_declared_where_t *general_where = general
        ? parse_sym_binding(general, N00BB_DECLARED_WHERE_KEY) : nullptr;
    n00bb_declared_where_t *specific_where = specific
        ? parse_sym_binding(specific, N00BB_DECLARED_WHERE_KEY) : nullptr;
    for (size_t i = 0; general_where && i < general_where->len; i++) {
        th_trait_requirement *requirement = &general_where->data[i];
        size_t ordinal = overload_typevar_ordinal(general,
                                                  requirement->subject);
        bool found = false;
        for (size_t j = 0; specific_where && j < specific_where->len; j++) {
            th_trait_requirement *candidate = &specific_where->data[j];
            if (ordinal == overload_typevar_ordinal(specific,
                                                    candidate->subject)
                && overload_requirement_equal(types, requirement,
                                               candidate)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool
overload_candidate_fit(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_sym_entry_t *candidate, th_type *signature,
                       core_list_t(n00bb_tree_ptr_t) *positional,
                       core_list_t(n00bb_tree_ptr_t) *keywords,
                       n00bb_overload_fit_t *fit, bool *pending)
{
    size_t count = positional->len + keywords->len;
    th_type **actual = core_alloc_array(th_type *, count ? count : 1);
    th_type **formal = core_alloc_array(th_type *, count ? count : 1);
    parse_tree_t **arguments = core_alloc_array(parse_tree_t *,
                                                count ? count : 1);
    bool valid = true;
    for (size_t i = 0; i < positional->len; i++) {
        const th_param *parameter = th_func_param(signature, i);
        arguments[i] = positional->data[i];
        actual[i] = parse_tree_type(arguments[i]);
        formal[i] = parameter ? parameter->type : nullptr;
        valid &= actual[i] && formal[i];
    }
    parse_sym_entry_t *declared = canonical_symbol(candidate);
    parse_tree_t *formals = declared->decl_node
        ? child_nt(grammar, child_nt(grammar, declared->decl_node,
                                     CORE_STRING_STATIC("func-type-signature")),
                   CORE_STRING_STATIC("param-decl")) : nullptr;
    for (size_t k = 0; k < keywords->len; k++) {
        size_t index = 0, slot = positional->len + k;
        const th_param *parameter = function_parameter_named(grammar,
                declared, formals, call_keyword_name(keywords->data[k]),
                &index)
            ? th_func_param(signature, index) : nullptr;
        arguments[slot] = child_nt(grammar, keywords->data[k],
                                  CORE_STRING_STATIC("expression"));
        actual[slot] = arguments[slot]
            ? parse_tree_type(arguments[slot]) : nullptr;
        formal[slot] = parameter ? parameter->type : nullptr;
        valid &= actual[slot] && formal[slot];
    }
    for (size_t i = 0; valid && i < count; i++)
        if (n00bb_open_integer(actual[i]) && is_integer_type(formal[i])
            && th_is_closed(state->types, formal[i])
            && th_unify_probe(state->types, actual[i], formal[i]))
            actual[i] = formal[i];
    bool closed = valid;
    for (size_t i = 0; i < count && closed; i++)
        closed = th_is_closed(state->types, actual[i]);
    *pending = !valid;
    bool matched = false;
    if (valid) {
        fit->pattern = th_tuple(state->types, formal, count);
        th_type *provided = th_tuple(state->types, actual, count);
        if (!closed) {
            matched = fit->pattern && provided
                && th_env_type_pattern_subsumes(fit->pattern, provided)
                && th_unify_probe(state->types, provided, fit->pattern);
            *pending = !matched && fit->pattern && provided
                && th_unify_probe(state->types, provided, fit->pattern);
        }
        else if (fit->pattern && provided
                 && th_unify_probe(state->types, provided, fit->pattern)) {
            matched = true;
        }
        else if (fit->pattern) {
            th_type **converted = core_alloc_array(th_type *,
                                                    count ? count : 1);
            bool convertible = true;
            for (size_t i = 0; i < count; i++) {
                converted[i] = actual[i];
                if (th_unify_probe(state->types, actual[i], formal[i]))
                    continue;
                if (!overload_argument_converts(state, arguments[i],
                                                actual[i], formal[i])) {
                    convertible = false;
                    break;
                }
                converted[i] = formal[i];
                fit->conversions++;
            }
            th_type *effective = convertible && fit->conversions
                ? th_tuple(state->types, converted, count) : nullptr;
            matched = effective && th_unify_probe(state->types, effective,
                                                   fit->pattern);
            core_free(converted);
        }
    }
    core_free(arguments);
    core_free(formal);
    core_free(actual);
    return matched;
}

static bool
symbol_from_module(parse_sym_entry_t *symbol, core_string_t module)
{
    parse_sym_entry_t *origin = canonical_symbol(symbol);
    return !module.data || (origin && origin->owner_unit
        && core_string_eq(origin->owner_unit->module.identifier, module));
}

parse_sym_entry_t *
n00bb_module_view_member(parse_symtab_t *symtab,
                         const n00bb_module_view_t *view, core_string_t name,
                         bool type_member)
{
    parse_scope_t *scope = view && type_member
        ? parse_symtab_scope_in_namespace(symtab, view->package_scope,
                                          CORE_STRING_STATIC("type"))
        : view ? view->package_scope : nullptr;
    for (parse_sym_entry_t *candidate = n00bb_scope_named_next(scope, name,
             nullptr);
         candidate; candidate = n00bb_scope_named_next(scope, name, candidate))
        if (symbol_from_module(candidate, view->module)) return candidate;
    return nullptr;
}

static void
push_scope_functions(parse_scope_t *scope, core_string_t name,
                     core_string_t module, core_translation_unit_t *consumer,
                     core_translation_unit_t *provider,
                     core_list_t(parse_sym_entry_ptr_t) *out)
{
    for (parse_sym_entry_t *candidate = n00bb_scope_named_next(scope, name,
             nullptr);
         candidate; candidate = n00bb_scope_named_next(scope, name, candidate))
        if (candidate->kind == PARSE_SYM_FUNCTION
            && symbol_from_module(candidate, module)
            && (!consumer || n00bb_symbol_visible_to(consumer, provider,
                    canonical_symbol(candidate)))
            && !n00bb_same_declaration_listed(out, candidate))
            core_list_push(*out, candidate);
}

static bool
source_provides(const n00bb_import_source_t *source, core_string_t name,
                core_string_t *provided)
{
    const n00bb_brought_name_t *brought = source->brought;
    if (brought && !brought->all && !core_string_eq(brought->local, name))
        return false;
    *provided = brought && !brought->all ? brought->remote : name;
    return true;
}

static bool
symbol_is_local(n00bb_tu_state_t *state, parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *origin = symbol ? canonical_symbol(symbol) : nullptr;
    return origin && state && origin->owner_unit == state->unit;
}

static void
push_import_functions(core_translation_unit_t *unit, core_string_t name,
                      bool prelude_only,
                      core_list_t(parse_sym_entry_ptr_t) *out)
{
    const core_list_t(n00bb_import_source_t) *sources =
        unqualified_import_sources(unit);
    for (size_t i = 0; sources && i < sources->len; i++) {
        core_string_t provided = {0};
        if ((!prelude_only || !sources->data[i].brought)
            && source_provides(&sources->data[i], name, &provided))
            push_scope_functions(sources->data[i].values, provided,
                                 (core_string_t){0}, unit,
                                 sources->data[i].provider, out);
    }
}

static core_list_t(parse_sym_entry_ptr_t)
function_candidates(n00bb_tu_state_t *state, parse_sym_entry_t *symbol,
                    n00bb_call_lookup_t lookup)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return candidates;
    if (!symbol->scope) {
        core_list_push(candidates, symbol);
        return candidates;
    }
    push_scope_functions(symbol->scope, symbol->name, lookup.origin_module,
                         nullptr, nullptr, &candidates);
    if (lookup.unqualified)
        push_import_functions(state->unit,
            lookup.name.data ? lookup.name : symbol->name, false,
            &candidates);
    return candidates;
}

void
n00bb_overload_choice_release(n00bb_overload_choice_t *choice)
{
    core_list_free(choice->matches);
}

core_string_t
n00bb_describe_candidate(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                         parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *origin = canonical_symbol(symbol);
    core_string_t module = origin->owner_unit
        ? origin->owner_unit->module.identifier : (core_string_t){0};
    parse_tree_t *signature = origin->decl_node
        ? child_nt(grammar, origin->decl_node,
                   CORE_STRING_STATIC("func-type-signature")) : nullptr;
    core_string_t text = signature
        ? n00bb_node_source_text(origin->owner_unit, signature,
                                 (core_string_t){0})
        : (core_string_t){0};
    if (!text.u8_bytes && origin->type && state && state->types)
        text = n00bb_print_type_brief(state->types, origin->type);
    bool function = origin->kind == PARSE_SYM_FUNCTION;
    bool typed_value = !function && origin->kind != PARSE_SYM_TYPEDEF
        && text.u8_bytes;
    core_buffer_t *qualified = core_buffer_empty();
    if (module.u8_bytes) {
        core_buffer_append_string(qualified, module);
        core_buffer_putc(qualified, '.');
    }
    core_buffer_append_string(qualified, origin->name);
    if (function) core_buffer_append_string(qualified, text);
    core_string_t name = core_buffer_take_string(qualified);
    core_string_t described = n00bb_text(typed_value
            ? N00B_TEXT_CANDIDATE_OF_TYPE : N00B_TEXT_QUOTED_NAME,
        N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(text)));
    core_free(name.data);
    return described;
}

core_string_t
n00bb_describe_candidates(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                          const core_list_t(parse_sym_entry_ptr_t) *symbols)
{
    core_list_t(core_string_t) items = core_list_new(core_string_t);
    for (size_t i = 0; i < symbols->len; i++)
        core_list_push(items, n00bb_describe_candidate(grammar, state,
                                                       symbols->data[i]));
    core_string_t described = n00bb_text(N00B_TEXT_TEXT,
        N00BB_ARGS(ARG_LIST(items.data, items.len)));
    for (size_t i = 0; i < items.len; i++) core_free(items.data[i].data);
    core_list_free(items);
    return described;
}

core_list_t(parse_sym_entry_ptr_t)
n00bb_call_overload_candidates(n00bb_tu_state_t *state,
                               parse_sym_entry_t *symbol,
                               n00bb_call_lookup_t lookup)
{
    return function_candidates(state, symbol, lookup);
}

n00bb_overload_choice_t
n00bb_select_call_overload(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                           parse_cf_label_t *call, parse_sym_entry_t *symbol,
                           n00bb_call_lookup_t lookup)
{
    n00bb_overload_choice_t choice = {N00BB_OVERLOAD_SELECTED, symbol, false,
        core_list_new(parse_sym_entry_ptr_t)};
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION || !call || !symbol->scope
        || !state || !state->types)
        return choice;
    core_list_t(parse_sym_entry_ptr_t) candidates = function_candidates(state,
        symbol, lookup);
    core_list_t(n00bb_tree_ptr_t) arguments =
        core_list_new(n00bb_tree_ptr_t);
    core_list_t(n00bb_tree_ptr_t) keywords = core_list_new(n00bb_tree_ptr_t);
    if (call->then_body) {
        collect_positional_nodes(grammar,
            child_nt(grammar, call->then_body, CORE_STRING_STATIC("positional-args")),
            &arguments);
        collect_keyword_nodes(grammar,
            child_nt(grammar, call->then_body, CORE_STRING_STATIC("keyword-args")), &keywords);
    }
    core_list_t(n00bb_overload_fit_t) fits =
        core_list_new(n00bb_overload_fit_t);
    parse_sym_entry_t *sole = nullptr;
    size_t shapes = 0;
    bool pending = false;
    for (size_t c = 0; c < candidates.len; c++) {
        parse_sym_entry_t *candidate = candidates.data[c];
        th_type *signature = candidate->callable_contract
            ? candidate->callable_contract : candidate->type;
        signature = signature ? th_resolve(signature) : nullptr;
        if (!signature || th_kind_of(signature) != TH_FUNCTION
            || !call_binds_candidate(grammar, candidate, signature,
                                     arguments.len, &keywords)) continue;
        shapes++;
        sole = candidate;
        n00bb_overload_fit_t fit = {.symbol = candidate};
        bool candidate_pending = false;
        if (overload_candidate_fit(grammar, state, candidate, signature,
                                   &arguments, &keywords, &fit,
                                   &candidate_pending))
            core_list_push(fits, fit);
        pending |= candidate_pending;
    }
    core_list_free(arguments);
    core_list_free(keywords);
    core_list_free(candidates);
    if (shapes == 1) {
        choice.symbol = sole;
        core_list_free(fits);
        return choice;
    }
    choice.symbol = nullptr;
    if (pending) {
        choice.status = N00BB_OVERLOAD_PENDING;
        core_list_free(fits);
        return choice;
    }
    bool local_fit = false;
    for (size_t i = 0; i < fits.len; i++)
        local_fit |= symbol_is_local(state, fits.data[i].symbol);
    size_t kept = 0;
    for (size_t i = 0; i < fits.len; i++)
        if (!local_fit || symbol_is_local(state, fits.data[i].symbol))
            fits.data[kept++] = fits.data[i];
    fits.len = kept;
    size_t tier = SIZE_MAX;
    for (size_t i = 0; i < fits.len; i++)
        if (!fits.data[i].conversions) {
            tier = 0;
            break;
        }
        else tier = 1;
    if (tier == SIZE_MAX) {
        choice.status = N00BB_OVERLOAD_NO_MATCH;
        core_list_free(fits);
        return choice;
    }
    core_translation_unit_t *origin = nullptr;
    bool mixed_origins = false;
    for (size_t i = 0; i < fits.len; i++) {
        if ((fits.data[i].conversions != 0) != (tier != 0)) continue;
        core_translation_unit_t *owner =
            canonical_symbol(fits.data[i].symbol)->owner_unit;
        mixed_origins |= origin && owner != origin;
        origin = owner;
    }
    if (mixed_origins) {
        choice.status = N00BB_OVERLOAD_CONFLICT;
        for (size_t i = 0; i < fits.len; i++)
            if ((fits.data[i].conversions != 0) == (tier != 0))
                core_list_push(choice.matches, fits.data[i].symbol);
        core_list_free(fits);
        return choice;
    }
    size_t selected = SIZE_MAX;
    for (size_t i = 0; i < fits.len; i++) {
        if ((fits.data[i].conversions != 0) != (tier != 0)) continue;
        bool dominated = false;
        for (size_t j = 0; j < fits.len; j++) {
            if (i == j
                || ((fits.data[j].conversions != 0) != (tier != 0)))
                continue;
            bool i_general = th_env_type_pattern_subsumes(
                fits.data[i].pattern, fits.data[j].pattern);
            bool j_general = th_env_type_pattern_subsumes(
                fits.data[j].pattern, fits.data[i].pattern);
            if (i_general && j_general) {
                i_general = overload_constraints_subsume(state->types,
                    fits.data[i].symbol, fits.data[j].symbol);
                j_general = overload_constraints_subsume(state->types,
                    fits.data[j].symbol, fits.data[i].symbol);
            }
            if (i_general && !j_general) {
                dominated = true;
                break;
            }
        }
        if (dominated) continue;
        core_list_push(choice.matches, fits.data[i].symbol);
        selected = i;
    }
    if (choice.matches.len != 1) choice.status = N00BB_OVERLOAD_AMBIGUOUS;
    else {
        choice.matches.len = 0;
        choice.symbol = fits.data[selected].symbol;
        if (!tier)
            for (size_t i = 0; i < fits.len; i++)
                choice.passed_over_conversion |= fits.data[i].conversions > 0;
    }
    core_list_free(fits);
    return choice;
}

bool
n00bb_function_has_overloads(n00bb_tu_state_t *state,
                             parse_sym_entry_t *symbol,
                             n00bb_call_lookup_t lookup)
{
    core_list_t(parse_sym_entry_ptr_t) candidates = function_candidates(state,
        symbol, lookup);
    bool overloaded = candidates.len > 1;
    core_list_free(candidates);
    return overloaded;
}

void
n00bb_defer_overloaded_callee(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *symbol = nt_is(grammar, node, CORE_STRING_STATIC("primary"))
        ? attached_symbol(grammar, unit, node) : nullptr;
    if (state && state->types && symbol
        && symbol->kind == PARSE_SYM_FUNCTION
        && n00bb_function_has_overloads(state, symbol,
               (n00bb_call_lookup_t){
                   .unqualified = !dotted_symbol_path_node(grammar, node),
                   .name = first_token_text(node)})
        && module_member_is_call(grammar, node))
        parse_tree_set_type(node, th_var(state->types));
}

static parse_sym_entry_t *
module_call_exact_arity(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *member, parse_sym_entry_t *symbol,
                        core_string_t origin_module)
{
    n00bb_call_lookup_t lookup = {.origin_module = origin_module};
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION
        || !n00bb_function_has_overloads(state, symbol, lookup)) return symbol;
    n00bb_overload_choice_t choice = n00bb_select_call_overload(grammar,
        state, call_label_for_member(grammar, state, member), symbol, lookup);
    parse_sym_entry_t *selected = choice.symbol ? choice.symbol : symbol;
    n00bb_overload_choice_release(&choice);
    return selected;
}

bool
n00bb_pending_type_reference(n00bb_tu_state_t *state, parse_tree_t *node)
{
    th_type *current = parse_tree_type(node);
    parse_tree_t *receiver = core_tree_num_children(node)
        ? core_tree_child(node, 0) : nullptr;
    th_type *receiver_type = receiver ? parse_tree_type(receiver) : nullptr;
    if (current && (!receiver_type
                    || th_resolve(current) != th_resolve(receiver_type)))
        return true;
    parse_tree_set_type(node, th_var(state->types));
    return parse_tree_type(node) != nullptr;
}

static bool
names_imported_module(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *at = parse_tree_parent(node);
    while (nt_is(grammar, at, CORE_STRING_STATIC("member-chain")))
        at = parse_tree_parent(at);
    return nt_is(grammar, at, CORE_STRING_STATIC("module-source"));
}

static parse_tree_t *
member_name_node(parse_tree_t *node)
{
    size_t count = node && !core_tree_is_leaf(node)
        ? core_tree_num_children(node) : 0;
    return count >= 3 ? core_tree_child(node, count - 1) : node;
}

static parse_sym_entry_t *
hidden_module_declaration(core_translation_unit_t *imported,
                          core_string_t member, bool type_member)
{
    n00bb_tu_state_t *state = tu_state(imported);
    parse_scope_t *scopes[] = {type_member ? type_module_scope(state)
                                           : module_scope(state),
                               type_module_scope(state)};
    for (size_t i = 0; i < 2; i++) {
        parse_sym_entry_t *symbol = scopes[i]
            ? parse_symtab_lookup_in(scopes[i], member) : nullptr;
        if (symbol && (symbol->decl_node || state->interface_module)
            && !symbol->is_synthetic && symbol->kind != PARSE_SYM_MODULE)
            return symbol;
    }
    core_string_t package = imported->module.identifier;
    size_t dot = package.u8_bytes;
    while (dot > 0 && package.data[dot - 1] != '.') dot--;
    if (dot > 1) package.u8_bytes = dot - 1;
    for (size_t i = 0; i < core_translation_unit_import_count(imported); i++) {
        core_translation_unit_t *inner = core_translation_unit_import(imported, i);
        core_string_t name = inner ? inner->module.identifier
                                   : (core_string_t){};
        if (name.u8_bytes <= package.u8_bytes + 1
            || !core_string_starts_with(name, package)
            || name.data[package.u8_bytes] != '.')
            continue;
        parse_sym_entry_t *symbol = hidden_module_declaration(inner, member,
                                                              type_member);
        if (symbol) return symbol;
    }
    return nullptr;
}

static void
report_module_member(core_translation_unit_t *unit, parse_tree_t *node,
                     core_translation_unit_t *imported, core_string_t member,
                     parse_sym_entry_t *found, parse_scope_t *exposed,
                     n00bb_text_t kind)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    size_t parts = node && !core_tree_is_leaf(node)
        ? core_tree_num_children(node) : 0;
    core_string_t module = parts >= 3
        ? n00bb_node_source_text(unit, core_tree_child(node, 0),
                                 imported->module.identifier)
        : imported->module.identifier;
    bool type_member = kind == N00B_TEXT_TYPE;
    parse_sym_entry_t *hidden = found
        && !n00bb_symbol_visible_to(unit, imported, canonical_symbol(found))
        ? found : hidden_module_declaration(imported, member, type_member);
    core_diag_location_t at = n00bb_node_span(member_name_node(node));
    if (hidden && !n00bb_symbol_visible_to(unit, imported,
                                           canonical_symbol(hidden))) {
        bool private = symbol_is_private(canonical_symbol(hidden));
        if (private)
            n00bb_report(diagnostics, unit, N00B_DIAG_MODULE_MEMBER_PRIVATE, at,
                N00B_TEXT_IS_PRIVATE_TO_MODULE,
                N00BB_ARGS(ARG_TEXT(member), ARG_TEXT(module)));
        else
            n00bb_report(diagnostics, unit, N00B_DIAG_MODULE_MEMBER_PRIVATE, at,
                N00B_TEXT_IS_NOT_EXPORTED_TO_THIS,
                N00BB_ARGS(ARG_TEXT(member), ARG_TEXT(module)));
        n00bb_report_label(diagnostics, N00B_TEXT_NOT_VISIBLE_HERE,
            N00BB_NO_ARGS);
        n00bb_diag_declared_here(diagnostics, hidden,
            private ? N00B_TEXT_PRIVATE_IS_DECLARED_HERE
                    : N00B_TEXT_IS_DECLARED_HERE);
        if (private)
            n00bb_report_help(diagnostics, N00B_TEXT_REMOVE_PRIVATE_FROM_TO_USE,
                N00BB_ARGS(ARG_TEXT(member)));
        else
            n00bb_report_help(diagnostics,
                N00B_TEXT_EXPORT_FROM_ITS_PACKAGE_MANIFEST,
                N00BB_ARGS(ARG_TEXT(member)));
        return;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_MODULE_MEMBER_MISSING, at,
        N00B_TEXT_MODULE_HAS_NO,
        N00BB_ARGS(ARG_TEXT(module), ARG_PHRASE_ID(kind), ARG_TEXT(member)));
    n00bb_report_label(diagnostics, N00B_TEXT_NOT_FOUND_IN,
        N00BB_ARGS(ARG_TEXT(module)));
    n00bb_tu_state_t *imported_state = tu_state(imported);
    parse_scope_t *types = imported_state && imported_state->annotations
        && exposed
        ? parse_symtab_scope_in_namespace(imported_state->annotations->symtab,
                                          exposed, CORE_STRING_STATIC("type"))
        : nullptr;
    if (!n00bb_suggest_scope_member(diagnostics, unit, imported, exposed,
                                    member)
        && types && types != exposed)
        (void)n00bb_suggest_scope_member(diagnostics, unit, imported, types,
                                         member);
}

static void
poison_unresolved_site(n00bb_tu_state_t *state, n00bb_link_use_site_t *site)
{
    site->unresolved = true;
    n00bb_poison_node(state, site->node);
}

static bool resolve_member_site(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node);

bool
resolve_module_member_site(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_link_use_site_t *site = state
        ? link_use_site_for_node(state, node) : nullptr;
    if (site && site->unresolved) return false;
    const core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(unit);
    size_t failures = core_diagnostics_failures(diagnostics);
    bool resolved = resolve_member_site(grammar, unit, node);
    if (!resolved && site
        && core_diagnostics_failures(diagnostics) > failures)
        poison_unresolved_site(state, site);
    return resolved;
}

static bool
resolve_member_site(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *node)
{
    if (!dotted_symbol_path_node(grammar, node)) return true;

    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_link_use_site_t *site = link_use_site_for_node(state, node);
    if (!site) return false;
    n00bb_link_use_site_t *root = site;
    while (root && root->dotted_path)
        root = link_use_site_for_node(state, root->receiver_node);
    parse_sym_entry_t *base_symbol = root ? root->symbol : nullptr;
    if (base_symbol && base_symbol->kind != PARSE_SYM_MODULE) return true;
    core_string_t package_namespace = n00bb_package_namespace(unit);
    if (root && root->member_name.data && package_namespace.u8_bytes) {
        if (core_string_eq(root->member_name, package_namespace)) {
            core_diagnostics_t *diagnostics =
                core_translation_unit_diagnostics(unit);
            n00bb_report(diagnostics, unit, N00B_DIAG_OWN_PACKAGE_NAMESPACE,
                n00bb_node_span(root->node),
                N00B_TEXT_IS_THIS_PACKAGE_S_EXTERNAL,
                N00BB_ARGS(ARG_TEXT(package_namespace)));
            n00bb_report_help(diagnostics, N00B_TEXT_REFER_TO_THE_MODULE_BY,
                N00BB_NO_ARGS);
            return false;
        }
    }

    core_translation_unit_t *whole_module =
        unit_for_module_site(unit, site, false);
    if (whole_module) {
        parse_scope_t *scope = module_scope(tu_state(whole_module));
        return scope && parse_tree_set_resolution_scope(node, scope);
    }

    core_translation_unit_t *prefix_module =
        unit_for_module_site(unit, site, true);
    if (prefix_module && !is_type_reference_node(grammar, node)) {
        parse_scope_t *scope = module_scope(tu_state(prefix_module));
        return scope && parse_tree_set_resolution_scope(node, scope);
    }

    n00bb_link_use_site_t *receiver_site =
        link_use_site_for_node(state, site->receiver_node);
    core_translation_unit_t *imported = receiver_site
        ? unit_for_module_site(unit, receiver_site, false) : nullptr;
    parse_scope_t *exposed_scope = imported
        ? package_scope_for_module_site(unit, receiver_site, imported)
        : nullptr;
    if (imported && !exposed_scope)
        exposed_scope = module_scope(tu_state(imported));
    parse_sym_entry_t *prefix = nullptr;
    if (!imported) {
        prefix = receiver_site ? receiver_site->symbol : nullptr;
        if (!prefix || prefix->kind != PARSE_SYM_MODULE
            || !prefix->exposed_scope) {
            return true;
        }
        imported = parse_sym_binding(
            prefix, CORE_STRING_STATIC("n00bb.package-unit"));
        if (!imported) imported = find_direct_import(unit, prefix->name);
        exposed_scope = prefix->exposed_scope;
    }
    if (!imported || !exposed_scope) return true;

    core_string_t member = site->member_name;
    bool type_reference = is_type_reference_node(grammar, node);
    n00bb_tu_state_t *imported_state = tu_state(imported);
    parse_scope_t *exposed_types = imported_state && exposed_scope
        ? parse_symtab_scope_in_namespace(imported_state->annotations->symtab,
            exposed_scope, CORE_STRING_STATIC("type")) : nullptr;
    parse_sym_entry_t *type = imported_state && imported_state->annotations
        ? parse_symtab_lookup_in(exposed_types, member)
        : nullptr;
    parse_sym_entry_t *symbol = parse_symtab_lookup_in(exposed_scope, member);
    const n00bb_module_view_t *view = prefix
        ? parse_sym_binding(prefix, CORE_STRING_STATIC(N00BB_MODULE_VIEW_KEY))
        : nullptr;
    if (view && imported_state && imported_state->annotations) {
        parse_symtab_t *symtab = imported_state->annotations->symtab;
        type = n00bb_module_view_member(symtab, view, member, true);
        symbol = n00bb_module_view_member(symtab, view, member, false);
        site->origin_module = view->module;
    }
    if (module_member_is_call(grammar, node))
        symbol = module_call_exact_arity(grammar, tu_state(unit), node,
                                         symbol, site->origin_module);
    bool package_export = core_string_eq(exposed_scope->scope_tag,
                                         CORE_STRING_STATIC(
                                             "package-interface"));
    if (!symbol && !type && package_export && imported_state
        && imported_state->annotations)
        symbol = n00bb_package_module_view(imported_state->annotations->symtab,
                                           exposed_scope, member);
    if (package_export && !view && (symbol || type)
        && n00bb_report_name_conflict(grammar, unit, node, member,
               type_reference || !symbol ? exposed_types : exposed_scope,
               type_reference || !symbol))
        return false;
    core_string_t consumer_package = n00bb_package_namespace(unit);
    parse_sym_entry_t *granted = !symbol && !type
        && consumer_package.u8_bytes
        ? granted_export_symbol(imported, member, consumer_package) : nullptr;
    if (granted && granted->kind == PARSE_SYM_TYPEDEF) {
        type = granted;
        package_export = true;
    }
    else if (granted) {
        symbol = granted;
        package_export = true;
    }
    if (type_reference || (!symbol && type)) {
        bool visible = type && type->kind == PARSE_SYM_TYPEDEF
            && (package_export
                || n00bb_symbol_visible_to(unit, imported, type));
        if (visible) {
            return parse_tree_set_resolution_symbol(node, type);
        }
        if (package_export && !state->semantic_annotation_complete)
            return n00bb_pending_type_reference(state, node);
        report_module_member(unit, node, imported, member,
                             type && type->kind == PARSE_SYM_TYPEDEF
                                 ? type : nullptr,
                             exposed_types, N00B_TEXT_TYPE);
        return false;
    }

    if (symbol) {
        if (symbol->kind != PARSE_SYM_MODULE
            && !package_export
            && !n00bb_symbol_visible_to(unit, imported, symbol)) {
            report_module_member(unit, node, imported, member, symbol,
                exposed_scope,
                symbol->kind == PARSE_SYM_FUNCTION ? N00B_TEXT_FUNCTION_WORD
                : N00B_TEXT_MEMBER_WORD);
            return false;
        }
        if (!parse_tree_set_resolution_symbol(node, symbol)
            || !parse_tree_mark_direct_symbol_reference(node)) return false;
        n00bb_tu_state_t *state = tu_state(unit);
        th_type *use_type = parse_tree_type(node);
        bool direct_call = module_member_is_call(grammar, node);
        if (direct_call && symbol->kind == PARSE_SYM_FUNCTION && use_type
            && symbol->type && state && state->types
            && !state->semantic_annotation_complete
            && th_is_closed(state->types, symbol->type)
            && !n00bb_function_has_overloads(state, symbol,
                   (n00bb_call_lookup_t){
                       .origin_module = site->origin_module})) {
            th_type *instance = th_resolve(th_type_clone(state->types,
                                                         symbol->type));
            th_type *use = th_resolve(use_type);
            bool shapes = instance && use
                && th_kind_of(instance) == TH_FUNCTION
                && th_kind_of(use) == TH_FUNCTION
                && th_func_nparams(instance) == th_func_nparams(use)
                && th_unify_probe(state->types, th_func_ret(use),
                                  th_func_ret(instance));
            if (shapes)
                th_unify_in_place(state->types, th_func_ret(use),
                                  th_func_ret(instance));
        }
        int64_t constant = 0;
        bool contextual_constant = use_type
            && th_kind_of(th_resolve(use_type)) != TH_VAR
            && n00bb_integer_constant_reference(grammar, node, &constant);
        if (!direct_call && !contextual_constant && symbol->type && use_type
            && state && state->types) {
            bool context_constructor = context_symbol(grammar, symbol);
            bool copied = context_constructor
                || symbol->kind == PARSE_SYM_FUNCTION;
            th_type *source = copied
                ? th_type_clone(state->types, symbol->type) : symbol->type;
            th_type *unified = source
                ? th_unify_in_place(state->types, use_type, source) : nullptr;
            if (!unified) {
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                core_string_t text = n00bb_node_source_text(unit, node,
                                                            member);
                core_string_t has = n00bb_print_type(state->types,
                                                     symbol->type);
                core_string_t wanted = n00bb_print_type(state->types,
                                                        use_type);
                n00bb_report(diagnostics, unit, N00B_DIAG_IMPORTED_VALUE_TYPE,
                    n00bb_node_span(node), N00B_TEXT_HAS_TYPE_BUT_IT_IS,
                    N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(has),
                               ARG_TEXT(wanted)));
                n00bb_diag_declared_here(diagnostics, symbol,
                    N00B_TEXT_IS_DECLARED_HERE);
                return false;
            }
            parse_tree_set_type(node, unified);
        }
        return true;
    }
    else {
        if ((package_export && !state->semantic_annotation_complete)
            || names_imported_module(grammar, node))
            return true;
        report_module_member(unit, node, imported, member, nullptr,
            exposed_scope,
            module_member_is_call(grammar,
                node) ? N00B_TEXT_FUNCTION_WORD : N00B_TEXT_MEMBER_WORD);
        return false;
    }
}

bool
site_is_resolved_module_prefix(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state,
                               n00bb_link_use_site_t *site)
{
    if (site && site->member_name.data
        && find_direct_import(unit, site->member_name)) return true;
    parse_tree_t *node = site ? site->node : nullptr;
    for (parse_tree_t *parent = node ? parse_tree_parent(node) : nullptr;
         parent; parent = parse_tree_parent(parent)) {
        if (!dotted_symbol_path_node(grammar, parent)) continue;
        n00bb_link_use_site_t *parent_site = link_use_site_for_node(state,
                                                                    parent);
        if (parent_site && unit_for_module_site(unit, parent_site, false))
            return true;
    }
    return false;
}

static void collect_origin_candidates(core_translation_unit_t *consumer,
    core_translation_unit_t *provider, parse_scope_t *scope,
    core_string_t name, core_list_t(parse_sym_entry_ptr_t) *out);

static bool
push_import_source(core_list_t(n00bb_import_source_t) *sources,
                   core_translation_unit_t *provider, core_string_t package,
                   const n00bb_brought_name_t *brought)
{
    n00bb_tu_state_t *state = tu_state(provider);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    n00bb_package_declaration_t *declaration = provider
        ? find_package_declaration(provider, package) : nullptr;
    parse_scope_t *values = declaration && declaration->scope
        ? declaration->scope : brought ? module_scope(state) : nullptr;
    if (!values || !symtab) return false;
    core_list_push(*sources, ((n00bb_import_source_t){
        .provider = provider,
        .values = values,
        .types = parse_symtab_scope_in_namespace(symtab, values,
            CORE_STRING_STATIC("type")),
        .brought = brought,
    }));
    return true;
}

const core_list_t(n00bb_import_source_t) *
unqualified_import_sources(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *consumer = tu_state(unit);
    if (!consumer) return nullptr;
    if (consumer->import_sources_ready) return &consumer->import_sources;
    consumer->import_sources.len = 0;
    for (size_t i = 0; i < consumer->brought_names.len; i++) {
        const n00bb_brought_name_t *brought = &consumer->brought_names.data[i];
        (void)push_import_source(&consumer->import_sources, brought->provider,
            brought->provider->module.identifier, brought);
    }
    core_translation_unit_t *builtin = find_direct_import(unit,
        CORE_STRING_STATIC("builtin"));
    consumer->import_sources_ready = !builtin
        || push_import_source(&consumer->import_sources, builtin,
                              CORE_STRING_STATIC("builtin"), nullptr);
    return &consumer->import_sources;
}

static parse_sym_entry_t *
granted_import(core_translation_unit_t *unit,
               const n00bb_import_source_t *source, core_string_t name,
               bool type_reference)
{
    core_string_t consumer = n00bb_package_namespace(unit);
    parse_sym_entry_t *granted = consumer.u8_bytes
        ? granted_export_symbol(source->provider, name, consumer) : nullptr;
    return granted && (!type_reference || granted->kind == PARSE_SYM_TYPEDEF)
        ? granted : nullptr;
}

static void
collect_tier_candidates(core_translation_unit_t *unit, core_string_t name,
                        bool type_reference, bool brought,
                        core_list_t(parse_sym_entry_ptr_t) *out)
{
    const core_list_t(n00bb_import_source_t) *sources =
        unqualified_import_sources(unit);
    for (size_t i = 0; sources && i < sources->len; i++) {
        const n00bb_import_source_t *source = &sources->data[i];
        core_string_t provided = {0};
        if (!source->brought != !brought
            || !source_provides(source, name, &provided)) continue;
        size_t before = out->len;
        if (!type_reference)
            collect_origin_candidates(unit, source->provider, source->values,
                                      provided, out);
        if (out->len == before)
            collect_origin_candidates(unit, source->provider, source->types,
                                      provided, out);
        parse_sym_entry_t *granted = out->len == before && !brought
            ? granted_import(unit, source, provided, type_reference)
            : nullptr;
        if (granted) core_list_push(*out, granted);
    }
}

core_list_t(parse_sym_entry_ptr_t)
unqualified_import_candidates(core_translation_unit_t *unit,
                              core_string_t name, bool type_reference)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    collect_tier_candidates(unit, name, type_reference, true, &candidates);
    if (!candidates.len)
        collect_tier_candidates(unit, name, type_reference, false,
                                &candidates);
    return candidates;
}

static parse_sym_entry_t *
unqualified_import_type(core_translation_unit_t *unit, core_string_t name)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    collect_tier_candidates(unit, name, true, false, &candidates);
    parse_sym_entry_t *type = candidates.len ? candidates.data[0] : nullptr;
    core_list_free(candidates);
    return type;
}

static bool
alias_reexports(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                parse_sym_entry_t *symbol, parse_sym_entry_t *type)
{
    parse_tree_t *declaration = symbol->decl_node;
    parse_sym_entry_t *target = canonical_symbol(type);
    return nt_is(grammar, declaration, CORE_STRING_STATIC("alias-stmt"))
        && alias_declaration_is_transparent(grammar, declaration)
        && symbol->type && target->type && state->types
        && th_hash(state->types, th_resolve(symbol->type))
            == th_hash(state->types, th_resolve(target->type));
}

static bool
function_shapes_overlap(th_ctx *types, th_type *left, th_type *right)
{
    left = left ? th_resolve(th_type_clone(types, left)) : nullptr;
    right = right ? th_resolve(th_type_clone(types, right)) : nullptr;
    if (!left || !right || th_kind_of(left) != TH_FUNCTION
        || th_kind_of(right) != TH_FUNCTION
        || th_func_nparams(left) != th_func_nparams(right)) return false;
    size_t count = th_func_nparams(left);
    th_type **left_params = core_alloc_array(th_type *, count + 1);
    th_type **right_params = core_alloc_array(th_type *, count + 1);
    for (size_t i = 0; i < count; i++) {
        left_params[i] = th_func_param(left, i)->type;
        right_params[i] = th_func_param(right, i)->type;
    }
    bool overlap = th_unify_probe(types, th_tuple(types, left_params, count),
                                  th_tuple(types, right_params, count));
    core_free(left_params);
    core_free(right_params);
    return overlap;
}

static void
warn_overlapping_import(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        n00bb_tu_state_t *state, parse_sym_entry_t *local)
{
    core_list_t(parse_sym_entry_ptr_t) imported =
        core_list_new(parse_sym_entry_ptr_t);
    push_import_functions(unit, local->name, true, &imported);
    size_t overlapping = 0;
    for (size_t i = 0; i < imported.len; i++) {
        parse_sym_entry_t *other = canonical_symbol(imported.data[i]);
        if (!other->owner_type && function_shapes_overlap(state->types,
                local->callable_contract ? local->callable_contract
                                         : local->type,
                other->callable_contract ? other->callable_contract
                                         : other->type))
            imported.data[overlapping++] = other;
    }
    imported.len = overlapping;
    if (imported.len) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t described = n00bb_describe_candidates(grammar, state,
                                                            &imported);
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLAPPING_IMPORT,
            n00bb_symbol_location(local), N00B_TEXT_OVERLAPS_IMPORTED,
            N00BB_ARGS(ARG_NUMBER(imported.len), ARG_TEXT(local->name),
                       ARG_TEXT(described)));
        core_free(described.data);
        n00bb_report_note(diagnostics, N00B_TEXT_CALLS_THAT_BOTH_ACCEPT_BIND,
            N00BB_NO_ARGS);
        for (size_t i = 0; i < imported.len; i++)
            n00bb_diag_declared_here(diagnostics, imported.data[i],
                                     N00B_TEXT_IMPORTED_IS_DECLARED_HERE);
    }
    core_list_free(imported);
}

static bool
shadows_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
             n00bb_tu_state_t *state, parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *type = unqualified_import_type(unit, symbol->name);
    if (type ? alias_reexports(grammar, state, symbol, type)
             : !builtin_type_name(symbol->name))
        return false;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t described = type
        ? n00bb_describe_candidate(grammar, state, type)
        : n00bb_text(N00B_TEXT_QUOTED_NAME,
                     N00BB_ARGS(ARG_TEXT(symbol->name)));
    n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_NAME_SHADOWED,
        n00bb_symbol_location(symbol),
        symbol->kind == PARSE_SYM_FUNCTION
            ? N00B_TEXT_TYPE_NAME_CANNOT_NAME_FUNCTION
            : N00B_TEXT_TYPE_NAME_CANNOT_NAME_VALUE,
        N00BB_ARGS(ARG_TEXT(symbol->name), ARG_TEXT(described)));
    core_free(described.data);
    if (type)
        n00bb_diag_declared_here(diagnostics, type,
                                 N00B_TEXT_TYPE_IS_DECLARED_HERE);
    n00bb_report_help(diagnostics, N00B_TEXT_TYPE_NAMES_CANNOT_BE_SHADOWED,
        N00BB_NO_ARGS);
    return true;
}

bool
check_local_names_against_imports(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        const core_list_t(n00bb_import_source_t) *sources =
            unqualified_import_sources(unit);
        if (!sources || !sources->len || state->interface_module
            || !state->annotations || !state->annotations->symtab) continue;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            if (core_string_eq(scope->scope_tag,
                               CORE_STRING_STATIC("package-interface")))
                continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (!symbol->decl_node || symbol->is_synthetic
                    || symbol->is_field
                    || symbol->kind == PARSE_SYM_MODULE
                    || symbol->kind == PARSE_SYM_ENUM_CONST
                    || canonical_symbol(symbol) != symbol) continue;
                if (symbol->kind == PARSE_SYM_FUNCTION && !symbol->owner_type
                    && core_string_eq(scope->scope_tag,
                                      CORE_STRING_STATIC("module")))
                    warn_overlapping_import(analysis->grammar, unit, state,
                                            symbol);
                ok = !shadows_type(analysis->grammar, unit, state, symbol)
                    && ok;
            }
        }
    }
    return ok;
}

static bool
node_is_whole_callee(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        parse_tree_t *parent = parse_tree_parent(at);
        const parse_rule_t *rule = parent
            ? parse_tree_rule(grammar, parent) : nullptr;
        const parse_annotation_t *call = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)
            : nullptr;
        if (call)
            return parse_tree_resolve_child_ref(grammar, parent,
                                                call->name_ref) == at;
        if (!parent || core_tree_num_children(parent) != 1) return false;
    }
    return false;
}

parse_sym_entry_t *
n00bb_unqualified_import_named(core_translation_unit_t *unit,
                               core_string_t name, bool function)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    collect_tier_candidates(unit, name, false, false, &candidates);
    parse_sym_entry_t *found = nullptr;
    for (size_t i = 0; !found && i < candidates.len; i++)
        if ((candidates.data[i]->kind == PARSE_SYM_FUNCTION) == function
            && candidates.data[i]->kind != PARSE_SYM_TYPEDEF)
            found = candidates.data[i];
    core_list_free(candidates);
    return found;
}

void
n00bb_check_clobbered_import(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *symbol = state && state->types
        && simple_identifier_primary(grammar, node)
        ? attached_symbol(grammar, unit, node) : nullptr;
    if (!symbol || !symbol_is_local(state, symbol)
        || (symbol->kind != PARSE_SYM_FUNCTION
            && symbol->kind != PARSE_SYM_VARIABLE)) return;
    bool call = node_is_whole_callee(grammar, node);
    th_type *type = symbol->type ? th_resolve(symbol->type) : nullptr;
    bool callable = symbol->kind == PARSE_SYM_FUNCTION || !type
        || th_kind_of(type) == TH_FUNCTION
        || (th_kind_of(type) == TH_VAR && !n00bb_open_integer(type));
    parse_sym_entry_t *clobbered = call && !callable
        ? n00bb_unqualified_import_named(unit, symbol->name, true)
        : !call && symbol->kind == PARSE_SYM_FUNCTION
        ? n00bb_unqualified_import_named(unit, symbol->name, false)
        : nullptr;
    if (!clobbered) return;
    core_list_t(parse_sym_entry_ptr_t) hidden =
        core_list_new(parse_sym_entry_ptr_t);
    if (clobbered->kind == PARSE_SYM_FUNCTION)
        push_import_functions(unit, symbol->name, true, &hidden);
    else core_list_push(hidden, clobbered);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t described = n00bb_describe_candidates(grammar, state,
                                                        &hidden);
    bool local_function = symbol->kind == PARSE_SYM_FUNCTION;
    n00bb_text_t message = clobbered->kind == PARSE_SYM_FUNCTION
        ? local_function ? N00B_TEXT_LOCAL_FUNCTION_HIDES_FUNCTIONS
                         : N00B_TEXT_LOCAL_VALUE_HIDES_FUNCTIONS
        : local_function ? N00B_TEXT_LOCAL_FUNCTION_HIDES_VALUE
                         : N00B_TEXT_LOCAL_VALUE_HIDES_VALUE;
    n00bb_report(diagnostics, unit, N00B_DIAG_LOCAL_HIDES_IMPORT,
        n00bb_node_span(node), message,
        N00BB_ARGS(ARG_NUMBER(hidden.len), ARG_TEXT(symbol->name),
                   ARG_TEXT(described)));
    core_free(described.data);
    core_list_free(hidden);
    n00bb_report_label(diagnostics, call ? N00B_TEXT_CALLED_AS_FUNCTION_HERE
                                         : N00B_TEXT_USED_AS_VALUE_HERE,
                       N00BB_NO_ARGS);
    n00bb_diag_declared_here(diagnostics, symbol,
                             N00B_TEXT_LOCAL_IS_DECLARED_HERE);
    n00bb_report_help(diagnostics, N00B_TEXT_RENAME_THE_LOCAL_DECLARATION_OR,
        N00BB_NO_ARGS);
    if (call) parse_tree_set_type(node, th_var(state->types));
}

static bool
names_member_receiver(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        parse_tree_t *parent = parse_tree_parent(at);
        if (!parent) return false;
        if (dotted_symbol_path_node(grammar, parent))
            return core_tree_child(parent, 0) == at;
        if (core_tree_num_children(parent) != 1) return false;
    }
    return false;
}

static void
collect_origin_candidates(core_translation_unit_t *consumer,
                          core_translation_unit_t *provider,
                          parse_scope_t *scope, core_string_t name,
                          core_list_t(parse_sym_entry_ptr_t) *out)
{
    for (parse_sym_entry_t *candidate = n00bb_scope_named_next(scope, name,
             nullptr);
         candidate; candidate = n00bb_scope_named_next(scope, name, candidate))
        if (candidate->kind != PARSE_SYM_MODULE
            && (!consumer || n00bb_symbol_visible_to(consumer, provider,
                    canonical_symbol(candidate)))
            && !n00bb_same_declaration_listed(out, candidate))
            core_list_push(*out, candidate);
}

static bool
candidates_conflict(const core_list_t(parse_sym_entry_ptr_t) *candidates,
                    bool call)
{
    core_translation_unit_t *origin = nullptr;
    bool mixed = false, value = false;
    for (size_t i = 0; i < candidates->len; i++) {
        parse_sym_entry_t *declaration = canonical_symbol(candidates->data[i]);
        mixed |= origin && declaration->owner_unit != origin;
        origin = declaration->owner_unit;
        value |= declaration->kind != PARSE_SYM_FUNCTION;
    }
    return mixed && (value || !call);
}

static bool
report_conflicting_candidates(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *node, core_string_t name,
                              const core_list_t(parse_sym_entry_ptr_t)
                                  *candidates,
                              bool type_reference)
{
    n00bb_tu_state_t *state = tu_state(unit);
    bool conflict = candidates_conflict(candidates,
        !type_reference && module_member_is_call(grammar, node));
    if (conflict) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t described = n00bb_describe_candidates(grammar, state,
                                                            candidates);
        n00bb_report(diagnostics, unit, N00B_DIAG_AMBIGUOUS_NAME,
            n00bb_node_span(member_name_node(node)),
            N00B_TEXT_NAMES_DECLARATIONS_FROM_DIFFERENT_MODULES,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(described)));
        core_free(described.data);
        n00bb_report_label(diagnostics, N00B_TEXT_AMBIGUOUS_NAME,
            N00BB_NO_ARGS);
        for (size_t i = 0; i < candidates->len; i++)
            n00bb_diag_declared_here(diagnostics, candidates->data[i],
                N00B_TEXT_IS_DECLARED_HERE);
        n00bb_report_help(diagnostics, N00B_TEXT_REFER_TO_THE_ONE_YOU,
            N00BB_NO_ARGS);
    }
    return conflict;
}

bool
n00bb_report_name_conflict(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node, core_string_t name,
                           parse_scope_t *scope, bool type_reference)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    collect_origin_candidates(nullptr, nullptr, scope, name, &candidates);
    bool conflict = report_conflicting_candidates(grammar, unit, node, name,
                                                  &candidates, type_reference);
    core_list_free(candidates);
    return conflict;
}

static bool
resolve_prelude_value(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      n00bb_tu_state_t *state, n00bb_link_use_site_t *site)
{
    parse_sym_entry_t *intrinsic = parse_symtab_lookup_scoped(
        state->annotations->symtab, site->scope,
        CORE_STRING_STATIC("builtin"), site->member_name);
    bool type_reference = is_type_reference_node(grammar, site->node);
    if (n00bb_builtin_function(intrinsic)) site->symbol = intrinsic;
    else {
        core_list_t(parse_sym_entry_ptr_t) candidates =
            unqualified_import_candidates(unit, site->member_name,
                                          type_reference);
        bool conflict = report_conflicting_candidates(grammar, unit,
            site->node, site->member_name, &candidates, type_reference);
        site->symbol = !conflict && candidates.len ? candidates.data[0]
                                                   : nullptr;
        core_list_free(candidates);
    }
    return !site->symbol
        || (parse_tree_set_resolution_symbol(site->node, site->symbol)
            && parse_tree_mark_direct_symbol_reference(site->node));
}

static bool
mark_flagset_operator_site(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           n00bb_tu_state_t *state,
                           n00bb_link_use_site_t *site)
{
    if (!site->operator_dispatch
        || !n00bb_flagset_operator_site(grammar, unit, state, site->node))
        return false;
    if (site->symbol) parse_sym_remove_use(site->symbol, site->node);
    site->symbol = nullptr;
    site->reference = nullptr;
    site->dispatch_trait = (core_string_t){0};
    site->dispatch_method = (core_string_t){0};
    site->operator_dispatch = false;
    site->intrinsic_operator = true;
    return true;
}

void
mark_resolved_flagset_operator_sites(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        for (size_t s = 0; s < state->link_use_sites.len; s++)
            mark_flagset_operator_site(grammar, unit, state,
                                       &state->link_use_sites.data[s]);
    }
}

static bool report_hidden_import(core_translation_unit_t *unit,
    n00bb_tu_state_t *state, n00bb_link_use_site_t *site);

typedef struct {
    parse_sym_entry_t *declaration;
    const n00bb_import_source_t *source;
} brought_entry_t;
core_list_decl(brought_entry_t);

typedef struct {
    core_translation_unit_t *unit;
    n00bb_tu_state_t *state;
    core_dict_t names;
    core_list_t(brought_entry_t) entries;
} bring_check_t;

static core_diagnostics_t *
report_brought(core_translation_unit_t *unit, parse_tree_t *node,
               n00bb_text_t message, n00bb_args_t args)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    n00bb_report(diagnostics, unit, N00B_DIAG_BRING_CONFLICT,
                 n00bb_node_span(node), message, args);
    return diagnostics;
}

static bool
brought_name_declared_locally(bring_check_t *check, core_string_t name,
                              const n00bb_import_source_t *source)
{
    parse_scope_t *scopes[] = {module_scope(check->state),
                               type_module_scope(check->state)};
    for (size_t s = 0; s < sizeof(scopes) / sizeof(scopes[0]); s++) {
        parse_sym_entry_t *local = n00bb_scope_named_next(scopes[s], name,
                                                          nullptr);
        if (!local || local->kind == PARSE_SYM_MODULE || !local->decl_node
            || canonical_symbol(local) != local) continue;
        core_diagnostics_t *diagnostics = report_brought(check->unit,
            source->brought->node, N00B_TEXT_DECLARED_AND_BROUGHT,
            N00BB_ARGS(ARG_TEXT(name)));
        n00bb_diag_declared_here(diagnostics, local,
            N00B_TEXT_IS_DECLARED_HERE);
        n00bb_report_help(diagnostics, N00B_TEXT_RENAME_THE_BROUGHT_NAME_WITH,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(name),
                       ARG_TEXT(source->provider->module.identifier)));
        return true;
    }
    return false;
}

static bool
add_brought_entry(bring_check_t *check, const core_string_t *local,
                  parse_sym_entry_t *candidate,
                  const n00bb_import_source_t *source)
{
    parse_sym_entry_t *declaration = canonical_symbol(candidate);
    bool seen = false;
    size_t index = (uintptr_t)core_dict_get(&check->names, local, &seen);
    if (!seen) {
        core_dict_put(&check->names, local, check->entries.len);
        core_list_push(check->entries, ((brought_entry_t){
            .declaration = declaration, .source = source,
        }));
        return !brought_name_declared_locally(check, *local, source);
    }
    brought_entry_t *prior = &check->entries.data[index];
    if (prior->declaration == declaration
        || (prior->source == source
            && prior->declaration->owner_unit == declaration->owner_unit))
        return true;
    core_diagnostics_t *diagnostics = report_brought(check->unit,
        source->brought->node,
        prior->source == source ? N00B_TEXT_EXPORTS_DIFFERENT_NAMED
                                : N00B_TEXT_ALREADY_BROUGHT,
        N00BB_ARGS(ARG_TEXT(*local),
                   ARG_TEXT(source->provider->module.identifier)));
    if (prior->source == source)
        n00bb_report_help(diagnostics, N00B_TEXT_BRING_THEM_BY_NAME_AND,
            N00BB_NO_ARGS);
    else {
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_node_span(prior->source->brought->node),
            N00B_TEXT_IS_FIRST_BROUGHT_IN_HERE,
            N00BB_ARGS(ARG_TEXT(*local)));
        n00bb_report_help(diagnostics, N00B_TEXT_RENAME_ONE_OF_THEM_WITH,
            N00BB_NO_ARGS);
    }
    return false;
}

static bool
bring_scope_entries(bring_check_t *check, parse_scope_t *scope,
                    const n00bb_import_source_t *source)
{
    bool ok = true;
    for (parse_sym_entry_t *entry = scope ? scope->first_in_scope : nullptr;
         entry; entry = entry->next_in_scope)
        if (entry->kind != PARSE_SYM_MODULE
            && n00bb_symbol_visible_to(check->unit, source->provider,
                                          canonical_symbol(entry)))
            ok = add_brought_entry(check, &entry->name, entry, source) && ok;
    return ok;
}

static bool
bring_named_entries(bring_check_t *check, const n00bb_import_source_t *source)
{
    core_translation_unit_t *unit = check->unit;
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    collect_origin_candidates(unit, source->provider, source->values,
                              source->brought->remote, &candidates);
    collect_origin_candidates(unit, source->provider, source->types,
                              source->brought->remote, &candidates);
    bool ok = candidates.len != 0;
    if (!ok) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t module = source->provider->module.identifier;
        core_string_t remote = source->brought->remote;
        parse_sym_entry_t *hidden = hidden_module_declaration(
            source->provider, remote, false);
        n00bb_report(diagnostics, unit, N00B_DIAG_BRING_MISSING,
            n00bb_node_span(source->brought->node),
            hidden ? N00B_TEXT_BRING_NOT_VISIBLE
            : N00B_TEXT_BRING_NOTHING_NAMED,
            N00BB_ARGS(ARG_TEXT(module), ARG_TEXT(remote)));
        if (hidden)
            n00bb_diag_declared_here(diagnostics, hidden,
                symbol_is_private(canonical_symbol(hidden))
                    ? N00B_TEXT_PRIVATE_IS_DECLARED_HERE
                    : N00B_TEXT_IS_DECLARED_HERE);
        if (core_string_eq(remote, CORE_STRING_STATIC("all")))
            n00bb_report_help(diagnostics,
                N00B_TEXT_BRINGING_EVERYTHING_IS_WRITTEN_BRING,
                N00BB_ARGS(ARG_TEXT(module)));
        else if (!hidden) {
            (void)n00bb_suggest_scope_member(diagnostics, unit,
                source->provider, source->values, remote);
        }
    }
    for (size_t i = 0; ok && i < candidates.len; i++)
        ok = add_brought_entry(check, &source->brought->local,
                               candidates.data[i], source);
    core_list_free(candidates);
    return ok;
}

static bool
check_brought_names(core_translation_unit_t *unit, n00bb_tu_state_t *state)
{
    const core_list_t(n00bb_import_source_t) *sources =
        unqualified_import_sources(unit);
    bring_check_t check = {
        .unit = unit, .state = state,
        .entries = core_list_new(brought_entry_t),
    };
    core_dict_init_string(&check.names);
    bool ok = true;
    for (size_t i = 0; sources && i < sources->len; i++) {
        const n00bb_import_source_t *source = &sources->data[i];
        if (!source->brought) continue;
        if (!source->brought->all) {
            ok = bring_named_entries(&check, source) && ok;
            continue;
        }
        ok = bring_scope_entries(&check, source->values, source) && ok;
        ok = bring_scope_entries(&check, source->types, source) && ok;
    }
    core_dict_free(&check.names);
    core_list_free(check.entries);
    return ok;
}

bool
check_import_brings(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (state && !state->interface_module && state->brought_names.len)
            ok = check_brought_names(analysis->units.data[u], state) && ok;
    }
    return ok;
}

static void
report_unresolved_name(core_translation_unit_t *unit,
                       n00bb_link_use_site_t *site, core_string_t name)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    n00bb_report(diagnostics, unit, N00B_DIAG_UNRESOLVED_NAME,
        n00bb_node_span(site->node), N00B_TEXT_CANNOT_FIND_IN_THIS_SCOPE,
        N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_label(diagnostics, N00B_TEXT_NOT_FOUND_IN_THIS_SCOPE,
        N00BB_NO_ARGS);
    if (!n00bb_suggest_visible_name(diagnostics, unit, site->scope, name,
                                    N00BB_NAMES_VALUES))
        (void)n00bb_explain_hidden_name(diagnostics, unit, site->scope, name,
                                        false);
}

bool
resolve_imported_module_symbols(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units)
{
    if (!bind_imported_module_scopes(units)) return false;
    bool all_resolved = true;

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (site->intrinsic_operator || site->unresolved) continue;
            if (mark_flagset_operator_site(grammar, unit, state, site))
                continue;
            if (site->variant_case_candidate) continue;
            if (site->constructor_call) continue;
            if (site->trait_reference && !site->symbol) {
                core_translation_unit_t *owner = nullptr;
                core_buffer_t path = {0};
                if (!n00bb_resolve_trait_reference(grammar, unit, site,
                                              &owner, &site->symbol, &path)) {
                    core_buffer_release(&path);
                    return false;
                }
                core_buffer_release(&path);
            }
            if (site->dotted_path
                && !resolve_module_member_site(grammar, unit, site->node)
                && !site->unresolved)
                all_resolved = false;
            if (site->dotted_path
                && is_type_reference_node(grammar, site->node)) {
                parse_node_resolution_t type_resolution =
                    parse_tree_resolution(site->node);
                if (type_resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                    && type_resolution.target.symbol
                    && type_resolution.target.symbol->type) {
                    parse_sym_entry_t *type_symbol =
                        type_resolution.target.symbol;
                    bool trait = is_trait_type_symbol(grammar, type_symbol);
                    core_translation_unit_t *declaration_unit = trait
                        ? type_symbol->decl_node
                            ? type_symbol->owner_unit
                            : unit
                        : nullptr;
                    th_type *previous = parse_tree_type(site->node);
                    th_type *target_type = trait
                        ? trait_type_reference(grammar, unit,
                            declaration_unit, type_symbol, previous)
                        : type_symbol->type;
                    th_type *resolved = !target_type ? nullptr : previous
                        ? th_unify_in_place(state->types, previous,
                            target_type)
                        : target_type;
                    if (!resolved) {
                        report_type_reference_conflict(unit, state->types,
                            site->node, target_type, previous);
                        all_resolved = false;
                        continue;
                    }
                    parse_tree_set_type(site->node, resolved);
                    propagate_resolved_type_reference(grammar, unit,
                        site->node, resolved);
                }
            }
            if (site->dotted_path
                && !bind_resolved_reference_alias(grammar, unit,
                                                   site->node))
                return false;
            parse_node_resolution_t resolution =
                parse_tree_resolution(site->node);
            if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
                site->symbol = resolution.target.symbol;
            else if (parse_tree_sym(site->node))
                site->symbol = parse_tree_sym(site->node);
            else if (!site->dotted_path && state->annotations
                     && state->annotations->symtab) {
                site->symbol = parse_symtab_resolve_node(
                    state->annotations->symtab, site->node,
                    CORE_STRING_STATIC(""));
                if (!site->symbol
                    && !resolve_prelude_value(grammar, unit, state, site))
                    return false;
                if (!site->symbol)
                    resolve_local_enum_constant(grammar, state, site);
            }
            if (site->symbol && site->symbol->kind == PARSE_SYM_MODULE
                && !site->dotted_path
                && !names_member_receiver(grammar, site->node)
                && !names_imported_module(grammar, site->node)) {
                parse_sym_entry_t *module = site->symbol;
                site->symbol = nullptr;
                if (!resolve_prelude_value(grammar, unit, state, site))
                    return false;
                for (parse_tree_t *at = site->symbol
                         ? parse_tree_parent(site->node) : nullptr;
                     at; at = parse_tree_parent(at)) {
                    parse_node_resolution_t wrapped =
                        parse_tree_resolution(at);
                    if (wrapped.kind != PARSE_NODE_RESOLUTION_SYMBOL
                        || wrapped.target.symbol != module) break;
                    if (!parse_tree_set_resolution_symbol(at, site->symbol))
                        return false;
                    if (parse_tree_sym(at) == module)
                        parse_tree_set_sym(at, site->symbol);
                }
            }
            bool bracket_type_name = !site->symbol && !site->dotted_path
                && inside_array_dimension(grammar, site->node);
            if (bracket_type_name && state->annotations
                && state->annotations->symtab) {
                parse_sym_entry_t *type_symbol = lookup_named_type_reference(
                    grammar, state, state->annotations->symtab,
                    site->scope, site->member_name);
                if (type_symbol) {
                    site->symbol = type_symbol;
                    parse_tree_set_resolution_symbol(site->node,
                                                     type_symbol);
                }
            }
            if (site->symbol && reference_is_simple_assignment_target(
                    grammar, unit, site->node, site->symbol)) {
                parse_sym_remove_use(site->symbol, site->node);
            }
            else if (site->symbol
                     && !parse_sym_add_use(site->symbol, site->node)) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, n00bb_node_span(site->node), "resolving names",
                    "could not record a use of `%.*s`",
                    (int)site->symbol->name.u8_bytes,
                    site->symbol->name.data);
                return false;
            }
            else if (!site->symbol && !site->dotted_path
                     && !names_imported_module(grammar, site->node)
                     && report_hidden_import(unit, state, site)) {
                all_resolved = false;
            }
            else if (!site->symbol && !site->dotted_path
                     && !names_imported_module(grammar, site->node)
                     && !site->trait_reference
                     && !site->variant_case_candidate
                     && !site->constructor_call
                     && !site_is_resolved_module_prefix(grammar, unit,
                                                        state, site)
                     && !site->dispatch_trait.data
                     && !n00bb_preprocessor_builtin_name(
                            site->member_name)
                     && !(bracket_type_name && site->member_name.data
                          && builtin_type_name(site->member_name))
                     && !is_type_reference_node(grammar, site->node)) {
                core_string_t name = site->member_name.data
                    ? site->member_name : CORE_STRING_STATIC("");
                if (identifier_is_variant_case_expression(grammar, unit,
                                                          site->node))
                    report_variant_case_not_arm(unit, state, site->node,
                                                name, nullptr, nullptr);
                else report_unresolved_name(unit, site, name);
                poison_unresolved_site(state, site);
            }
        }
    }
    return all_resolved;
}

bool
function_interface_parameter(parse_sym_entry_t *symbol, size_t target,
                             core_string_t *name,
                             uint8_t *flags, uint8_t *mode)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (!data || memcmp(data, "N0FP", 4) || data[4] != 1) return false;
    uint32_t count = read_u32_le(data + 5);
    if (target >= count) return false;
    size_t offset = 9;
    for (size_t i = 0; i < count; i++) {
        if (offset > UINT32_MAX || offset > SIZE_MAX - 4) return false;
        uint16_t current_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t current_flags = data[offset + 2];
        uint8_t current_mode = data[offset + 3];
        offset += 4;
        if (!current_size) return false;
        if (i == target) {
            if (name)
                *name = core_string_borrow((const char *)data + offset,
                                           current_size);
            if (flags) *flags = current_flags;
            if (mode) *mode = current_mode;
            return true;
        }
        offset += current_size;
    }
    return false;
}

size_t
function_interface_parameter_count(parse_sym_entry_t *symbol)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    return data && !memcmp(data, "N0FP", 4) && data[4] == 1
        ? read_u32_le(data + 5) : 0;
}

bool
function_interface_parameter_named(parse_sym_entry_t *symbol,
                                   core_string_t target, size_t *index)
{
    for (size_t i = 0; i < function_interface_parameter_count(symbol); i++) {
        core_string_t name = {0};
        if (!function_interface_parameter(symbol, i, &name,
                                          nullptr, nullptr)) return false;
        if (target.data && core_string_eq(name, target)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

bool
context_interface_parameter(parse_sym_entry_t *symbol, size_t target,
                            core_string_t *name,
                            uint8_t *flags, uint8_t *mode)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (!data || (data[0] & 0x70u)) return false;
    bool extended = (data[0] & 0x80u) != 0;
    uint32_t count = read_u32_le(data + 1);
    if (target >= count) return false;
    size_t offset = 5;
    for (size_t i = 0; i < count; i++) {
        if (offset > SIZE_MAX - 4) return false;
        uint16_t current_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t current_flags = data[offset + 2];
        uint8_t current_mode = data[offset + 3];
        offset += 4;
        if (!current_size || current_size > SIZE_MAX - offset) return false;
        if (i == target) {
            if (name)
                *name = core_string_borrow((const char *)data + offset,
                                           current_size);
            if (flags) *flags = current_flags;
            if (mode) *mode = current_mode;
            return true;
        }
        offset += current_size;
        if (extended && (current_flags & 4u)) {
            if (offset > SIZE_MAX - 2) return false;
            uint16_t dependencies = (uint16_t)data[offset]
                | (uint16_t)data[offset + 1] << 8;
            offset += 2;
            if (dependencies > (SIZE_MAX - offset) / 2) return false;
            offset += (size_t)dependencies * 2;
        }
    }
    return false;
}

size_t
context_interface_parameter_count(parse_sym_entry_t *symbol)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    return data ? read_u32_le(data + 1) : 0;
}

bool
context_interface_parameter_named(parse_sym_entry_t *symbol,
                                  core_string_t target, size_t *index)
{
    for (size_t i = 0; i < context_interface_parameter_count(symbol); i++) {
        core_string_t name = {0};
        if (!context_interface_parameter(symbol, i, &name,
                                         nullptr, nullptr)) return false;
        if (target.data && core_string_eq(name, target)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

static bool
module_path_matches_site(n00bb_tu_state_t *state,
                         n00bb_link_use_site_t *site,
                         core_string_t module, bool allow_suffix)
{
    if (!state || !site || !module.data) return false;
    size_t components = 1;
    n00bb_link_use_site_t *at = site;
    while (at && at->dotted_path) {
        if (!at->member_name.data) return false;
        components++;
        at = link_use_site_for_node(state, at->receiver_node);
    }
    if (!at) return false;
    core_string_t root = at->symbol && at->symbol->kind == PARSE_SYM_MODULE
        ? at->symbol->name : at->member_name;
    if (!root.data) return false;

    size_t prefix_end = module.u8_bytes;
    size_t parts = 1;
    for (size_t i = 0; i < module.u8_bytes; i++) {
        if (module.data[i] != '.') continue;
        if (parts == components) {
            prefix_end = i;
            break;
        }
        parts++;
    }
    if (parts < components || (!allow_suffix
        && prefix_end != module.u8_bytes)) return false;

    size_t end = prefix_end;
    for (at = site; at->dotted_path;
         at = link_use_site_for_node(state, at->receiver_node)) {
        size_t start = end;
        while (start && module.data[start - 1] != '.') start--;
        if (!start || at->member_name.u8_bytes != end - start
            || memcmp(at->member_name.data, module.data + start,
                      end - start)) return false;
        end = start - 1;
    }
    return root.u8_bytes == end
        && !memcmp(root.data, module.data, end);
}

static bool
listed_unit(const core_list_t(core_translation_unit_ptr_t) *list,
            core_translation_unit_t *unit)
{
    for (size_t i = 0; i < list->len; i++)
        if (list->data[i] == unit) return true;
    return false;
}

static bool
import_path_hidden(n00bb_tu_state_t *state, core_translation_unit_t *unit)
{
    return state && listed_unit(&state->restricted_imports, unit)
        && !listed_unit(&state->path_imports, unit);
}

static bool
report_hidden_import(core_translation_unit_t *unit, n00bb_tu_state_t *state,
                     n00bb_link_use_site_t *site)
{
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        core_string_t path = imported->module.identifier;
        if (!import_path_hidden(state, imported)
            || !module_path_matches_site(state, site, path, true))
            continue;
        core_string_t alias = {0};
        for (size_t a = 0; !alias.data && a < state->import_aliases.len; a++)
            if (state->import_aliases.data[a].unit == imported
                && state->import_aliases.data[a].renamed)
                alias = state->import_aliases.data[a].name;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        if (alias.data) {
            n00bb_report(diagnostics, unit, N00B_DIAG_MODULE_NAME_NOT_IMPORTED,
                n00bb_node_span(site->node), N00B_TEXT_MODULE_IS_IMPORTED_AS_SO,
                N00BB_ARGS(ARG_TEXT(path), ARG_TEXT(alias), ARG_TEXT(path)));
            n00bb_report_help(diagnostics, N00B_TEXT_REFER_TO_IT_AS,
                N00BB_ARGS(ARG_TEXT(alias)));
        }
        else {
            n00bb_report(diagnostics, unit, N00B_DIAG_MODULE_NAME_NOT_IMPORTED,
                n00bb_node_span(site->node),
                N00B_TEXT_MODULE_IS_NOT_IMPORTED_NAMES,
                N00BB_ARGS(ARG_TEXT(path)));
            n00bb_report_help(diagnostics, N00B_TEXT_ADD_IMPORT_TO_USE_IT,
                N00BB_ARGS(ARG_TEXT(path)));
        }
        return true;
    }
    return false;
}

core_translation_unit_t *
unit_for_module_site(core_translation_unit_t *unit,
                     n00bb_link_use_site_t *site, bool allow_suffix)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (module_path_matches_site(state, site, unit->module.identifier,
                                 allow_suffix)) return unit;
    for (size_t i = 0; state && i < state->import_aliases.len; i++) {
        n00bb_import_alias_t *alias = &state->import_aliases.data[i];
        if ((alias->renamed || !import_path_hidden(state, alias->unit))
            && module_path_matches_site(state, site, alias->name,
                                        allow_suffix))
            return alias->unit;
    }
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        if (!import_path_hidden(state, imported)
            && module_path_matches_site(state, site,
                                        imported->module.identifier,
                                        allow_suffix)) return imported;
    }
    return nullptr;
}

parse_scope_t *
package_scope_for_module_site(core_translation_unit_t *unit,
                              n00bb_link_use_site_t *site,
                              core_translation_unit_t *imported)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (size_t i = 0; state && i < state->import_aliases.len; i++) {
        n00bb_import_alias_t *alias = &state->import_aliases.data[i];
        if (alias->unit != imported
            || !module_path_matches_site(state, site, alias->name, false))
            continue;
        n00bb_package_declaration_t *package =
            find_package_declaration(imported, alias->name);
        return package ? package->scope : nullptr;
    }
    return nullptr;
}

parse_sym_entry_t *
call_receiver_symbol(parse_grammar_t *grammar,
                     core_translation_unit_t *unit,
                     parse_tree_t *receiver)
{
    while (receiver && !core_tree_is_leaf(receiver)) {
        if (dotted_symbol_path_node(grammar, receiver)) {
            receiver = core_tree_child(receiver, 0);
            continue;
        }
        break;
    }
    return attached_symbol(grammar, unit, receiver);
}



bool
call_receiver_is_module(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        parse_tree_t *receiver)
{
    parse_sym_entry_t *symbol = call_receiver_symbol(grammar, unit, receiver);
    if (symbol && symbol->kind == PARSE_SYM_MODULE) return true;

    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_link_use_site_t *site = link_use_site_for_node(state, receiver);
    while (!site && receiver && !core_tree_is_leaf(receiver)
           && core_tree_num_children(receiver) == 1) {
        receiver = core_tree_child(receiver, 0);
        site = link_use_site_for_node(state, receiver);
    }
    return site && site->dotted_path
        && (unit_for_module_site(unit, site, false)
            || unit_for_module_site(unit, site, true));
}

parse_sym_entry_t *
n00bb_enum_type_symbol(n00bb_link_analysis_t *analysis, th_type *type)
{
    if (!analysis || !type || !analysis->units.len) return nullptr;
    if (!analysis->enum_symbols_indexed) {
        analysis->enum_symbols_indexed = true;
        core_dict_init(&analysis->enum_symbols_by_type, nullptr, nullptr);
        for (size_t i = 0; i < analysis->units.len; i++) {
            n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
            parse_symtab_t *symtab = state && state->annotations
                ? state->annotations->symtab : nullptr;
            parse_namespace_t *type_ns = symtab
                ? parse_symtab_ns(symtab, CORE_STRING_STATIC("type"))
                : nullptr;
            for (parse_scope_t *scope = type_ns ? symtab->all_scopes
                                                : nullptr;
                 scope; scope = scope->all_next) {
                if (scope->ns != type_ns) continue;
                for (parse_sym_entry_t *symbol = scope->first_in_scope;
                     symbol; symbol = symbol->next_in_scope)
                    if (symbol->type && symbol->member_scope
                        && core_string_eq(symbol->member_scope->scope_tag,
                                          CORE_STRING_STATIC("enum")))
                        (void)core_dict_add(&analysis->enum_symbols_by_type,
                            th_hash(state->types, symbol->type),
                            canonical_symbol(symbol));
            }
        }
    }
    th_ctx *types = tu_state(analysis->units.data[0])->types;
    return core_dict_get(&analysis->enum_symbols_by_type,
                         th_hash(types, type), nullptr);
}

parse_tree_t *
link_initializer_value(parse_grammar_t *grammar, parse_tree_t *definition)
{
    if (!definition) return nullptr;
    if (nt_is(grammar, definition, CORE_STRING_STATIC("visible-inferred-assign")))
        definition = child_nt(grammar, definition, CORE_STRING_STATIC("assign-stmt"));
    if (nt_is(grammar, definition, CORE_STRING_STATIC("sym-decl")))
        return child_nt(grammar, definition, CORE_STRING_STATIC("expression"));
    if (nt_is(grammar, definition, CORE_STRING_STATIC("assign-stmt")))
        return core_tree_num_children(definition) > 2
            ? core_tree_child(definition, 2) : nullptr;
    return nullptr;
}

static bool
link_payload_add_initializer(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_link_payload_t *payload,
                             parse_tree_t *definition)
{
    parse_tree_t *value = link_initializer_value(grammar, definition);
    if (!value) return true;
    for (size_t i = 0; i < payload->initializers.len; i++)
        if (payload->initializers.data[i].value == value) return true;
    core_list_push(payload->initializers,
        ((n00bb_initializer_fact_t){
            .site = definition,
            .value = value,
            .type = parse_tree_type(value),
            .location = link_source_location(definition),
            .provenance = core_source_file_path(&unit->source),
        }));
    return true;
}


bool
populate_link_declaration_facts(parse_grammar_t *grammar,
                                ir_ctx_t *ir_context,
                                core_translation_unit_t *unit,
                                n00bb_link_payload_t *payload)
{
    parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
    if (!grammar || !ir_context || !unit || !symbol
        || payload->facts_complete) return false;
    payload->semantic_type = symbol->type ? symbol->type
        : symbol->callable_contract;
    payload->static_value = symbol->kind == PARSE_SYM_VARIABLE
            && symbol->has_comptime_value
            ? symbol->comptime_value : nullptr;
    payload->phases = symbol->phases;
    payload->mutable_phases = symbol->mutable_phases;
    payload->initializer_phases = symbol->initializer_phases;
    payload->storage_duration = symbol->storage_duration;
    payload->lifetime = symbol->lifetime;
    payload->requested_storage_class = symbol->requested_storage_class;
    payload->selected_storage_class = symbol->selected_storage_class;
    payload->storage_class_required = symbol->storage_class_required;
    payload->is_thread_local = symbol->is_thread_local;
    payload->alias_analysis_complete = symbol->alias_analysis_complete;
    if (!payload->semantic_type) return false;

    (void)array_shape_from_declaration(grammar, unit, ir_context, symbol,
        payload->semantic_type, &payload->array_shape_complete);

    payload->initializer_facts_complete = true;
    if (symbol->decl_node
        && !link_payload_add_initializer(grammar, unit, payload,
                                          symbol->decl_node))
        payload->initializer_facts_complete = false;
    for (size_t i = 0; i < symbol->definition_count; i++)
        if (!link_payload_add_initializer(grammar, unit, payload,
                                           symbol->definition_nodes[i]))
            payload->initializer_facts_complete = false;
    for (size_t i = 0; i < payload->initializers.len; i++)
        if (!payload->initializers.data[i].type)
            payload->initializer_facts_complete = false;
    bool requires_comptime_value = symbol->mutability == PARSE_SYM_CONST;
    payload->phase_value_complete = !requires_comptime_value
        || payload->static_value != nullptr;
    payload->storage_lifetime_complete = payload->lifetime.has_region
        && payload->storage_duration != PARSE_SYM_STORAGE_UNSPECIFIED;
    payload->facts_complete = payload->array_shape_complete
        && payload->initializer_facts_complete;
    return true;
}

parse_sym_entry_t *
initializer_declaration_symbol(core_link_declaration_t *declaration)
{
    n00bb_link_payload_t *payload = declaration
        ? core_link_declaration_payload(declaration) : nullptr;
    return payload ? payload->symbol : nullptr;
}

static bool
static_literal_global_initializer(parse_grammar_t *grammar,
                                  parse_sym_entry_t *symbol)
{
    parse_tree_t *initializer = nullptr;
    if (symbol && symbol->decl_node)
        initializer = child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("expression"));
    if (!initializer && symbol && symbol->definition_count)
        initializer = child_nt(grammar, symbol->definition_nodes[0],
                                CORE_STRING_STATIC("expression"));
    if (!initializer) return false;
    n00bb_static_literal_probe_t probe = find_static_literal(grammar,
                                                              initializer);
    uint64_t folded = 0;
    return (probe.valid && nt_is(grammar, probe.literal, CORE_STRING_STATIC("simple-lit")))
        || (!probe.valid && n00bb_static_integer_expression(grammar,
                initializer, symbol->type, &folded));
}

bool
dynamic_initializer_declaration(parse_grammar_t *grammar,
                                core_link_declaration_t *declaration)
{
    parse_sym_entry_t *symbol = initializer_declaration_symbol(declaration);
    parse_tree_t *initializer_node = symbol ? symbol->decl_node : nullptr;
    parse_tree_t *expression = initializer_node
        ? child_nt(grammar, initializer_node, CORE_STRING_STATIC("expression")) : nullptr;
    if (!expression && symbol && symbol->definition_count) {
        initializer_node = symbol->definition_nodes[0];
        expression = child_nt(grammar, initializer_node, CORE_STRING_STATIC("expression"));
    }
    if (!is_module_global_symbol(grammar, symbol) || !expression)
        return false;
    for (parse_tree_t *at = initializer_node; at;
         at = parse_tree_parent(at)) {
        if (!nt_is(grammar, at, CORE_STRING_STATIC("variable-decl"))) continue;
        parse_tree_t *prefix = child_nt(grammar, at, CORE_STRING_STATIC("declaration-prefix"));
        if (child_nt(grammar, prefix, CORE_STRING_STATIC("storage-class"))) return false;
        if (core_string_eq(first_token_text(child_nt(grammar, prefix,
                CORE_STRING_STATIC("binding-kind"))),
                CORE_STRING_STATIC("const"))
            && n00bb_compile_phase() != PARSE_SYM_PHASE_COMPTIME)
            return false;
        break;
    }
    return !static_literal_global_initializer(grammar, symbol);
}

const core_list_t(core_translation_unit_ptr_t) *
n00bb_link_analysis_units(const n00bb_link_analysis_t *analysis)
{
    return analysis ? &analysis->units : nullptr;
}

void
bind_return_call_sources(n00bb_link_analysis_t *analysis)
{
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
            n00bb_return_source_t *source = n00bb_node_return_source(
                label->self);
            if (!source || source->kind != N00BB_RETURN_SOURCE_CALL
                || source->node != label->self) continue;
            parse_sym_entry_t *callee = bound_function_symbol(label->cond);
            if (callee && callee->kind == PARSE_SYM_FUNCTION)
                source->symbol = callee;
        }
    }
}

ir_func_t *
link_ir_function_for_symbol(n00bb_link_analysis_t *analysis,
                            parse_sym_entry_t *symbol)
{
    core_link_declaration_t *declaration =
        link_declaration_for_symbol(analysis, symbol);
    return declaration ? core_link_declaration_binding(declaration,
        CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
}

ir_global_t *
link_ir_global_for_symbol(n00bb_link_analysis_t *analysis,
                          parse_sym_entry_t *symbol)
{
    core_link_declaration_t *declaration =
        link_declaration_for_symbol(analysis, symbol);
    return declaration
        ? core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.ir.global")) : nullptr;
}

bool
append_u32_le(core_buffer_t *buffer, uint32_t value)
{
    uint8_t bytes[4];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(value >> (i * 8));
    core_buffer_append(buffer, (const char *)bytes, sizeof(bytes));
    return true;
}

uint32_t
read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}


parse_sym_entry_t *
unit_trait_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                core_string_t name, core_translation_unit_t *consumer)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return nullptr;
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        symtab, type_module_scope(state), CORE_STRING_STATIC("type"), name);
    bool interface_trait = symbol && !symbol->decl_node
        && state->interface_module && symbol->member_scope
        && core_string_eq(symbol->adt_kind, CORE_STRING_STATIC("trait"));
    if (!symbol || (!interface_trait && (!symbol->decl_node
        || !nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("trait-decl"))))) return nullptr;
    if (!n00bb_symbol_visible_to(consumer, unit, symbol))
        return nullptr;
    return symbol;
}

static bool
trait_module_matches_parts(const core_string_t *parts, size_t part_count,
                           core_string_t module)
{
    size_t part = 0;
    size_t start = 0;
    for (size_t i = 0; i <= module.u8_bytes; i++) {
        if (i != module.u8_bytes && module.data[i] != '.') continue;
        if (part == part_count
            || !core_string_eq(parts[part],
                core_string_borrow(module.data + start, i - start)))
            return false;
        part++;
        start = i + 1;
    }
    return part == part_count;
}

static parse_sym_entry_t *
package_trait_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     core_string_t package_name, core_string_t trait_name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    n00bb_package_declaration_t *package =
        find_package_declaration(unit, package_name);
    parse_scope_t *types = symtab && package && package->scope
        ? parse_symtab_scope_in_namespace(symtab, package->scope,
                                          CORE_STRING_STATIC("type"))
        : nullptr;
    parse_sym_entry_t *binding = types
        ? parse_symtab_lookup_in(types, trait_name) : nullptr;
    parse_sym_entry_t *origin = canonical_symbol(binding);
    return origin && origin->decl_node
        && nt_is(grammar, origin->decl_node, CORE_STRING_STATIC("trait-decl"))
        ? binding : nullptr;
}

static parse_sym_entry_t *
implicit_builtin_trait_symbol(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              core_string_t name,
                              core_translation_unit_t **owner_out)
{
    core_translation_unit_t *builtin = find_direct_import(
        unit, CORE_STRING_STATIC("builtin"));
    n00bb_package_declaration_t *package = builtin
        ? find_package_declaration(builtin, CORE_STRING_STATIC("builtin"))
        : nullptr;
    for (size_t i = 0; package && i < package->exports.len; i++) {
        n00bb_package_export_t *item = &package->exports.data[i];
        if (!core_string_eq(item->export_name, name))
            continue;
        core_string_t source = item->source_name;
        size_t split = source.u8_bytes;
        while (split && source.data[split - 1] != '.') split--;
        if (!split) continue;
        core_string_t module_name = core_string_borrow(source.data,
                                                       split - 1);
        core_string_t source_name = core_string_borrow(source.data + split,
                                                       source.u8_bytes - split);
        core_translation_unit_t *origin = find_direct_import(builtin,
                                                               module_name);
        parse_sym_entry_t *symbol = origin
            ? unit_trait_symbol(grammar, origin, source_name, unit) : nullptr;
        if (!symbol) continue;
        if (owner_out) *owner_out = origin;
        return symbol;
    }
    return nullptr;
}

bool
n00bb_resolve_trait_reference(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         n00bb_link_use_site_t *site,
                         core_translation_unit_t **owner_out,
                         parse_sym_entry_t **symbol_out,
                         core_buffer_t *path_out)
{
    if (owner_out) *owner_out = nullptr;
    if (symbol_out) *symbol_out = nullptr;
    parse_tree_t *node = site ? site->node : nullptr;
    if (!unit || !site || !site->trait_path_parts
        || !site->trait_path_part_count) return false;

    core_string_t *parts = site->trait_path_parts;
    size_t part_count = site->trait_path_part_count;
    core_string_t trait = parts[part_count - 1];
    core_translation_unit_t *target = nullptr;
    parse_sym_entry_t *symbol = nullptr;
    if (part_count > 1) {
        size_t module_part_count = part_count - 1;
        if (trait_module_matches_parts(parts, module_part_count,
                                       unit->module.identifier)) {
            target = unit;
        }
        else {
            n00bb_tu_state_t *state = tu_state(unit);
            for (size_t i = 0; state && i < state->import_aliases.len; i++) {
                n00bb_import_alias_t *alias = &state->import_aliases.data[i];
                if (!trait_module_matches_parts(parts, module_part_count,
                                                alias->name)) continue;
                target = alias->unit;
                symbol = package_trait_symbol(grammar, target, alias->name,
                                              trait);
                if (!symbol && n00bb_same_package(unit, target))
                    symbol = unit_trait_symbol(grammar, target, trait, unit);
                if (symbol) break;
                target = nullptr;
            }
            for (size_t i = 0;
                 !target && i < core_translation_unit_import_count(unit);
                 i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                if (trait_module_matches_parts(parts, module_part_count,
                        imported->module.identifier)) {
                    target = imported;
                    break;
                }
            }
        }
        if (target && !symbol)
            symbol = unit_trait_symbol(grammar, target, trait, unit);
        if (!symbol) target = nullptr;
    }
    else {
        symbol = unit_trait_symbol(grammar, unit, trait, unit);
        if (symbol) {
            target = unit;
        }
        else {
            for (size_t i = 0; i < core_translation_unit_import_count(unit);
                 i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                parse_sym_entry_t *candidate = unit_trait_symbol(
                    grammar, imported, trait, unit);
                if (!candidate) continue;
                if (target) {
                    target = nullptr;
                    symbol = nullptr;
                    break;
                }
                target = imported;
                symbol = candidate;
            }
            if (!symbol)
                symbol = implicit_builtin_trait_symbol(grammar, unit,
                                                       trait, &target);
        }
    }

    parse_sym_entry_t *resolved_symbol = symbol;
    if (symbol) {
        parse_sym_entry_t *origin = canonical_symbol(symbol);
        core_translation_unit_t *origin_owner = origin != symbol
            ? origin->owner_unit : target;
        if (origin_owner) {
            symbol = origin;
            target = origin_owner;
        }
    }

    for (size_t i = 0; i < part_count; i++) {
        if (i) core_buffer_putc(path_out, '.');
        core_buffer_append(path_out, parts[i].data, parts[i].u8_bytes);
    }
    if (owner_out) *owner_out = target;
    if (symbol_out) *symbol_out = symbol;
    if (resolved_symbol
        && !parse_tree_set_resolution_symbol(node, resolved_symbol))
        return false;
    return !symbol || parse_sym_add_use(symbol, node);
}

bool
n00bb_bind_site_reference(core_translation_unit_t *unit,
                          n00bb_link_use_site_t *site,
                          core_link_reference_t *reference)
{
    if (!site || !reference || core_link_reference_referrer(reference) != unit
        || core_link_reference_use_site(reference) != site->node)
        return false;
    site->reference = reference;
    return true;
}

bool
linkable_module_symbol(parse_sym_entry_t *symbol)
{
    if (!symbol || parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-type"))
        || (symbol->owner_type && parse_sym_binding(symbol->owner_type,
            CORE_STRING_STATIC("n00bb.builtin-type")))) return false;
    return symbol->kind == PARSE_SYM_FUNCTION
        || symbol->kind == PARSE_SYM_VARIABLE
        || symbol->kind == PARSE_SYM_TYPEDEF
        || symbol->kind == PARSE_SYM_TAG
        || symbol->kind == PARSE_SYM_ENUM_CONST;
}

core_translation_unit_t *
unit_for_symbol(parse_grammar_t *grammar,
                core_list_t(core_translation_unit_ptr_t) *units,
                parse_sym_entry_t *symbol)
{
    if (symbol && symbol->owner_unit) {
        n00bb_tu_state_t *owner_state = tu_state(symbol->owner_unit);
        if (owner_state && owner_state->interface_module)
            for (size_t i = 0; units && i < units->len; i++)
                if (units->data[i] == symbol->owner_unit)
                    return symbol->owner_unit;
    }
    if (!linkable_module_symbol(symbol)
        || (!symbol->scope
            || (!core_string_eq(symbol->scope->scope_tag,
                                CORE_STRING_STATIC("module"))
                && !core_string_eq(symbol->scope->scope_tag,
                                   CORE_STRING_STATIC("package-interface"))
                && !(symbol->owner_type && symbol->owner_type->scope
                    && core_string_eq(symbol->owner_type->scope->scope_tag,
                                      CORE_STRING_STATIC("module")))
                && !nested_module_declaration_owner(grammar,
                                                    symbol->decl_node))))
        return nullptr;
    for (size_t i = 0; units && i < units->len; i++)
        if (units->data[i] == symbol->owner_unit) return symbol->owner_unit;
    return nullptr;
}

parse_tree_t *
nested_module_declaration_owner(parse_grammar_t *grammar,
                               parse_tree_t *declaration)
{
    for (parse_tree_t *parent = declaration
            ? parse_tree_parent(declaration) : nullptr;
         parent; parent = parse_tree_parent(parent)) {
        if (nt_is(grammar, parent, CORE_STRING_STATIC("func-def"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("context-def"))) return nullptr;
        if (field_record_declaration(grammar, parent)
            || nt_is(grammar, parent, CORE_STRING_STATIC("enum-stmt"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("flagset-stmt"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("variant-decl"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("trait-decl"))) return parent;
    }
    return nullptr;
}

static void
append_link_symbol_base(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                        core_buffer_t *key)
{
    parse_tree_t *owner = nested_module_declaration_owner(
        grammar, symbol->decl_node);
    parse_sym_entry_t *owner_symbol = owner
        ? parse_tree_sym(owner) : symbol->owner_type;
    core_string_t owner_name = owner_symbol
        ? owner_symbol->name : (core_string_t){0};
    core_buffer_printf(key, "%u:", (unsigned)symbol->kind);
    if (owner_name.data) {
        core_buffer_append(key, owner_name.data, owner_name.u8_bytes);
        core_buffer_putc(key, '.');
    }
    core_buffer_append(key, symbol->name.data, symbol->name.u8_bytes);
}

bool
append_function_contract_identity(core_translation_unit_t *unit,
                                  parse_sym_entry_t *symbol,
                                  core_buffer_t *key)
{
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return true;
    n00bb_tu_state_t *state = tu_state(unit);
    for (size_t i = 0; state && i < state->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *registration =
            &state->literal_modifiers.data[i];
        if (registration->declaration
                ? registration->declaration != symbol->decl_node
                : registration->function != symbol) continue;
        core_buffer_printf(key, "#litmod_%.*s",
            (int)registration->kind.u8_bytes, registration->kind.data);
        return true;
    }
    bool overloaded = false;
    for (parse_sym_entry_t *candidate = symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         candidate; candidate = candidate->next_in_scope) {
        if (candidate != symbol && candidate->kind == PARSE_SYM_FUNCTION
            && core_string_eq(candidate->name, symbol->name)) {
            overloaded = true;
            break;
        }
    }
    if (!overloaded) return true;
    th_type *contract = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    n00bb_declared_where_t *declared = parse_sym_binding(
        canonical_symbol(symbol), N00BB_DECLARED_WHERE_KEY);
    const uint8_t *encoding = nullptr;
    size_t length = 0;
    if (!state || !state->types || !contract
        || !th_encode(state->types, contract, &encoding, &length))
        return false;
    static const char hex[] = "0123456789abcdef";
    core_buffer_putc(key, '#');
    for (size_t i = 0; i < length; i++) {
        core_buffer_putc(key, hex[encoding[i] >> 4]);
        core_buffer_putc(key, hex[encoding[i] & 0x0f]);
    }
    for (size_t i = 0; declared && i < declared->len; i++) {
        th_trait_requirement *requirement = &declared->data[i];
        size_t ordinal = overload_typevar_ordinal(symbol,
                                                  requirement->subject);
        if (ordinal == SIZE_MAX) return false;
        core_buffer_printf(key, "w%zu:%u:", ordinal,
                           (unsigned)requirement->kind);
        if (requirement->kind == TH_CON_IMPLEMENTS) {
            core_buffer_printf(key, "%zu:", requirement->iface.u8_bytes);
            core_buffer_append(key, requirement->iface.data,
                               requirement->iface.u8_bytes);
        }
        else if (requirement->kind == TH_CON_EXCLUDES) {
            const uint8_t *target = nullptr;
            size_t target_length = 0;
            if (!requirement->target
                || !th_encode(state->types, requirement->target, &target,
                              &target_length)) return false;
            core_buffer_printf(key, "%zu:", target_length);
            for (size_t j = 0; j < target_length; j++) {
                core_buffer_putc(key, hex[target[j] >> 4]);
                core_buffer_putc(key, hex[target[j] & 0x0f]);
            }
        }
        else return false;
    }
    return true;
}

bool
append_link_symbol_key(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol, core_buffer_t *key)
{
    n00bb_export_origin_t *origin = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.export-origin"));
    bool inherited = false;
    if (!origin && symbol->owner_type) {
        origin = parse_sym_binding(symbol->owner_type,
            CORE_STRING_STATIC("n00bb.export-origin"));
        inherited = origin != nullptr;
    }
    if (origin) {
        size_t module_size = origin->module.u8_bytes;
        if (origin->declaration.u8_bytes <= module_size
            || memcmp(origin->declaration.data, origin->module.data,
                      module_size)
            || origin->declaration.data[module_size] != '.') return false;
        core_buffer_printf(key, "%u:%.*s", (unsigned)symbol->kind,
            (int)(origin->declaration.u8_bytes - module_size - 1),
            origin->declaration.data + module_size + 1);
        if (inherited) {
            core_buffer_putc(key, '.');
            core_buffer_append(key, symbol->name.data, symbol->name.u8_bytes);
        }
        return true;
    }
    append_link_symbol_base(grammar, symbol, key);
    return append_function_contract_identity(unit, symbol, key);
}

core_string_t
link_symbol_module(core_translation_unit_t *unit,
                   parse_sym_entry_t *symbol)
{
    n00bb_export_origin_t *origin = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.export-origin"));
    if (!origin && symbol->owner_type)
        origin = parse_sym_binding(symbol->owner_type,
            CORE_STRING_STATIC("n00bb.export-origin"));
    return origin ? origin->module : unit->module.identifier;
}

bool
append_environment_symbol_name(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_sym_entry_t *symbol,
                               core_buffer_t *qualified)
{
    core_buffer_t identity = {0};
    append_link_symbol_base(grammar, symbol, &identity);
    core_string_t text = core_buffer_string(&identity);
    int64_t colon = text.data ? core_string_find_byte(text, ':') : -1;
    bool ok = colon >= 0;
    if (ok) {
        core_buffer_printf(qualified, "%.*s.%.*s",
            (int)unit->module.identifier.u8_bytes,
            unit->module.identifier.data,
            (int)(text.u8_bytes - (size_t)colon - 1),
            text.data + colon + 1);
        ok = append_function_contract_identity(unit, symbol, qualified);
    }
    core_buffer_release(&identity);
    return ok;
}

core_list_t(parse_scope_ptr_t)
link_declaration_scopes(n00bb_tu_state_t *state,
                        core_string_t namespace_name)
{
    core_list_t(parse_scope_ptr_t) scopes =
        module_scopes_in_namespace(state, namespace_name);
    if (!state || !state->interface_module || !state->annotations
        || !state->annotations->symtab) return scopes;

    parse_symtab_t *symtab = state->annotations->symtab;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (!scope->ns
            || !core_string_eq(scope->ns->ns_name, CORE_STRING_STATIC(""))
            || !core_string_eq(scope->scope_tag,
                               CORE_STRING_STATIC("package-interface")))
            continue;
        bool export_scope = false;
        for (size_t p = 0; !export_scope
             && p < state->package_declarations.len; p++)
            export_scope = state->package_declarations.data[p].scope == scope;
        if (export_scope) continue;
        parse_scope_t *candidate = namespace_name.u8_bytes
            ? parse_symtab_scope_in_namespace(symtab, scope, namespace_name)
            : scope;
        if (candidate) core_list_push(scopes, candidate);
    }
    return scopes;
}

void
free_n00bb_link_payload(void *payload)
{
    n00bb_link_payload_t *link_payload = payload;
    if (!link_payload) return;
    core_list_free(link_payload->initializers);
    core_list_free(link_payload->cleanup_facts);
    core_free(payload);
}

bool
n00bb_link_contract_compatible(const void *existing, const void *candidate,
                               void *opaque)
{
    th_ctx *types = opaque;
    if (existing == candidate) return true;
    if (!existing || !candidate || !types) return false;
    const uint8_t *existing_encoding = nullptr;
    const uint8_t *candidate_encoding = nullptr;
    size_t existing_length = 0, candidate_length = 0;
    return th_encode(types, (th_type *)existing, &existing_encoding,
                     &existing_length)
        && th_encode(types, (th_type *)candidate, &candidate_encoding,
                     &candidate_length)
        && existing_length == candidate_length
        && memcmp(existing_encoding, candidate_encoding,
                  existing_length) == 0;
}

static void
report_duplicate_definition(core_translation_unit_t *unit,
                            parse_sym_entry_t *symbol,
                            parse_sym_entry_t *first)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t again = n00bb_symbol_location(symbol);
    core_diag_location_t at = n00bb_symbol_location(first);
    if (first->owner_unit == unit && again.has_location && at.has_location
        && (again.line < at.line
            || (again.line == at.line && again.column < at.column))) {
        core_diag_location_t earlier = again;
        again = at;
        at = earlier;
    }
    n00bb_text_t kind = symbol->kind == PARSE_SYM_FUNCTION
        ? N00B_TEXT_FUNCTION_WORD
        : symbol->kind == PARSE_SYM_TYPEDEF ? N00B_TEXT_TYPE
                                            : N00B_TEXT_DECLARATION_WORD;
    n00bb_report(diagnostics, unit, N00B_DIAG_DUPLICATE_DEFINITION, again,
        N00B_TEXT_IS_DEFINED_MORE_THAN_ONCE,
        N00BB_ARGS(ARG_PHRASE_ID(kind), ARG_TEXT(symbol->name)));
    n00bb_report_label(diagnostics, N00B_TEXT_DEFINED_AGAIN_HERE,
        N00BB_NO_ARGS);
    if (at.has_location)
        n00bb_report_secondary(diagnostics, first->owner_unit, at,
            N00B_TEXT_IS_FIRST_DEFINED_HERE,
            N00BB_ARGS(ARG_TEXT(first->name)));
    if (symbol->kind == PARSE_SYM_FUNCTION)
        n00bb_report_help(diagnostics,
            N00B_TEXT_REMOVE_ONE_DEFINITION_RENAME_IT,
            N00BB_NO_ARGS);
    else
        n00bb_report_help(diagnostics,
            N00B_TEXT_REMOVE_ONE_DEFINITION_OR_RENAME,
            N00BB_NO_ARGS);
}

bool
register_link_declaration(core_link_unit_t *link,
                          core_translation_unit_t *unit,
                          parse_grammar_t *grammar,
                          core_dict_t *declarations_by_symbol,
                          parse_sym_entry_t *symbol)
{
    core_buffer_t key = {0};
    if (!append_link_symbol_key(grammar, unit, symbol, &key)) {
        return false;
    }
    core_buffer_t overload_group = {0};
    if (symbol->kind == PARSE_SYM_FUNCTION) {
        int64_t specialization = core_string_find_byte(
            core_buffer_string(&key), '#');
        core_buffer_append(&overload_group, key.data,
            specialization >= 0 ? (size_t)specialization : key.byte_len);
    }
    core_string_t source_path = core_source_file_path(&unit->source);
    parse_tree_t *source_site = symbol_binding_node(symbol);
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *contract = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    core_link_declaration_metadata_t metadata = {
        .kind = (uint32_t)symbol->kind,
        .visibility = (uint32_t)symbol_visibility(symbol),
        .overload_group = overload_group.data
            ? core_buffer_string(&overload_group)
            : (core_string_t){0},
        .source_site = source_site,
        .source_location = link_source_location(source_site),
        .provenance = source_path,
        .provenance_version = 1,
        .contract = contract,
        .compatible = n00bb_link_contract_compatible,
        .compatibility_context = state ? state->types : nullptr,
    };
    core_link_declaration_t *declaration = nullptr;
    core_string_t declaration_module = link_symbol_module(unit, symbol);
    core_string_t declaration_key = core_buffer_string(&key);
    core_link_declaration_t *aliased = state && state->interface_module
        ? core_link_unit_find_declaration(link, declaration_module,
                                          declaration_key)
        : nullptr;
    bool alias_declaration = aliased != nullptr;
    if (aliased) metadata.contract = core_link_declaration_contract(aliased);
    if (!state || !state->interface_module) {
        core_link_declaration_t *existing = core_link_unit_find_declaration(
            link, declaration_module, declaration_key);
        n00bb_link_payload_t *existing_payload = existing
            ? core_link_declaration_payload(existing) : nullptr;
        parse_sym_entry_t *first = existing_payload
            ? existing_payload->symbol : nullptr;
        if (first && canonical_symbol(first) != canonical_symbol(symbol)
            && first->decl_node && first->decl_node != symbol->decl_node) {
            bool redeclared_in_scope = symbol->kind == PARSE_SYM_VARIABLE
                && symbol->scope == first->scope
                && (symbol->shadowed == first || first->shadowed == symbol);
            if (!redeclared_in_scope)
                report_duplicate_definition(unit, symbol, first);
            core_buffer_release(&key);
            core_buffer_release(&overload_group);
            return false;
        }
    }
    n00bb_link_payload_t *payload = nullptr;
    if (!alias_declaration) {
        payload = core_alloc(n00bb_link_payload_t);
        *payload = (n00bb_link_payload_t){.symbol = symbol};
    }
    bool declaration_registered = alias_declaration
        ? core_link_unit_redeclare_external_typed(link, declaration_module,
            declaration_key, &metadata, &declaration)
        : state && state->interface_module
            ? core_link_unit_define_external_typed(link, declaration_module,
                declaration_key, &metadata, payload,
                CORE_LINK_PAYLOAD_OWNED, free_n00bb_link_payload,
                &declaration)
            : core_link_unit_register_declaration(link, unit,
                declaration_key, &metadata, true, payload,
                CORE_LINK_PAYLOAD_OWNED, free_n00bb_link_payload,
                &declaration);
    bool registered = declaration_registered
        && core_dict_add(declarations_by_symbol, symbol, declaration);
    if (declaration_registered && !registered)
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
            link_source_location(source_site), "indexing declarations",
            "`%.*s` was indexed twice", (int)symbol->name.u8_bytes,
            symbol->name.data);
    core_buffer_release(&key);
    core_buffer_release(&overload_group);
    if (!declaration_registered && payload) core_free(payload);
    return registered;
}

bool
register_link_declarations(core_link_unit_t *link, parse_grammar_t *grammar,
                           core_list_t(core_translation_unit_ptr_t) *units,
                           core_dict_t *declarations_by_symbol)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations || !state->annotations->symtab)
            return false;
        core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
            state, CORE_STRING_STATIC(""));
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (linkable_module_symbol(symbol)
                    && (symbol_binding_node(symbol) || state->interface_module)
                    && !register_link_declaration(link, unit, grammar,
                                                  declarations_by_symbol,
                                                  symbol)) {
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
        scopes = link_declaration_scopes(state, CORE_STRING_STATIC("type"));
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (linkable_module_symbol(symbol)
                    && (symbol_binding_node(symbol) || state->interface_module)
                    && !register_link_declaration(link, unit, grammar,
                                                  declarations_by_symbol,
                                                  symbol)) {
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                bool interface_member = state->interface_module
                    && symbol->owner_type;
                bool source_member = symbol->decl_node
                    && nested_module_declaration_owner(grammar,
                                                       symbol->decl_node);
                if (!linkable_module_symbol(symbol)
                    || (!interface_member && !source_member))
                    continue;
                if (!register_link_declaration(link, unit, grammar,
                                               declarations_by_symbol,
                                               symbol))
                    return false;
            }
        }
    }

    static const core_string_t builtin_names[] = {
        CORE_STRING_STATIC("has_type"),
        CORE_STRING_STATIC("__gc_byte_offset"),
        CORE_STRING_STATIC("__gc_byte_diff"),
        CORE_STRING_STATIC("__count_leading_zeros"),
        CORE_STRING_STATIC("__count_trailing_zeros"),
        CORE_STRING_STATIC("__gc_memset"), CORE_STRING_STATIC("__gc_memcpy"),
        CORE_STRING_STATIC("__gc_invoke_finalizer"),
        CORE_STRING_STATIC("__volatile_load"),
        CORE_STRING_STATIC("__gc_signal_fence"),
        CORE_STRING_STATIC("__gc_prefetch"),
        CORE_STRING_STATIC("__atomic_fetch_or"),
        CORE_STRING_STATIC("__atomic_fetch_or_release"),
        CORE_STRING_STATIC("__atomic_fetch_or_relaxed"),
        CORE_STRING_STATIC("__atomic_fetch_and"),
        CORE_STRING_STATIC("__atomic_exchange"),
        CORE_STRING_STATIC("__atomic_exchange_acquire"),
        CORE_STRING_STATIC("__atomic_exchange_relaxed"),
        CORE_STRING_STATIC("__atomic_fetch_add"),
        CORE_STRING_STATIC("__atomic_fetch_add_relaxed"),
        CORE_STRING_STATIC("__atomic_fetch_sub"),
        CORE_STRING_STATIC("__atomic_load"),
        CORE_STRING_STATIC("__atomic_load_relaxed"),
        CORE_STRING_STATIC("__atomic_load_acquire"),
        CORE_STRING_STATIC("__atomic_store"),
        CORE_STRING_STATIC("__atomic_store_relaxed"),
        CORE_STRING_STATIC("__atomic_store_release"),
        CORE_STRING_STATIC("__atomic_compare_exchange"),
    };
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (!n00bb_builtin_function(symbol) || !symbol->uses) continue;
            core_string_t name = symbol->name;
            bool selected = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.abi.native"))
                || n00bb_builtin_unsafe_arithmetic(symbol);
            for (size_t b = 0; !selected
                 && b < sizeof(builtin_names) / sizeof(builtin_names[0]); b++)
                selected = core_string_eq(name, builtin_names[b]);
            if (!selected) continue;
            core_link_declaration_t *declaration = nullptr;
            for (size_t d = 0; d < core_link_unit_declaration_count(link);
                 d++) {
                core_link_declaration_t *candidate =
                    core_link_unit_declaration_at(link, d);
                if (core_string_eq(core_link_declaration_module(candidate),
                                   CORE_STRING_STATIC("builtins"))
                    && core_string_eq(core_link_declaration_key(candidate),
                                      name)) {
                    declaration = candidate;
                    break;
                }
            }
            if (!declaration) {
                core_link_declaration_metadata_t metadata = {
                    .kind = PARSE_SYM_FUNCTION,
                    .visibility = 0,
                    .contract = symbol->callable_contract
                        ? symbol->callable_contract : symbol->type,
                };
                if (!core_link_unit_require_external(link,
                        CORE_STRING_STATIC("builtins"), name, &metadata,
                        &declaration)) return false;
            }
            if (!core_dict_add(declarations_by_symbol, symbol, declaration))
                return false;
        }
    }
    return true;
}

bool
n00bb_resolve_overload_reference(parse_grammar_t *grammar,
                                 core_link_unit_t *link,
                                 n00bb_tu_state_t *state, parse_tree_t *callee,
                                 parse_sym_entry_t *selected)
{
    parse_sym_entry_t *declaration = selected ? canonical_symbol(selected)
                                              : nullptr;
    for (parse_tree_t *at = callee; link && declaration && at
             && !core_tree_is_leaf(at);
         at = core_tree_num_children(at) == 1 ? core_tree_child(at, 0)
                                               : nullptr) {
        n00bb_link_use_site_t *site = link_use_site_for_node(state, at);
        if (!site || !site->reference
            || !core_link_reference_is_deferred(site->reference)) continue;
        core_translation_unit_t *owner = declaration->owner_unit;
        core_buffer_t key = {0};
        bool ok = owner && append_link_symbol_key(grammar, owner,
                                                  declaration, &key)
            && core_link_unit_resolve_reference(link, site->reference,
                   link_symbol_module(owner, declaration),
                   core_buffer_string(&key));
        core_buffer_release(&key);
        return ok;
    }
    return true;
}

bool
register_link_use_sites(core_link_unit_t *link,
                        parse_grammar_t *grammar,
                        core_list_t(core_translation_unit_ptr_t) *units)
{
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !prepare_callback_call_symbols(unit))
            return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            if (site->intrinsic_operator) continue;
            if (site->variant_case_candidate) continue;
            parse_tree_t *node = site->node;
            parse_sym_entry_t *symbol = canonical_symbol(site->symbol);
            if (symbol && symbol != site->symbol
                && !parse_sym_add_use(symbol, node)) return false;
            if (site->constructor_call || site->operator_dispatch) continue;
            if (site->callback_call) continue;
            if (site->callback_setup) {
                core_link_reference_t *reference = nullptr;
                bool deferred =
                    core_link_unit_add_deferred_reference_at_scoped_kind(
                        link, unit, node, site->scope, PARSE_SYM_FUNCTION,
                        CORE_STRING_STATIC(
                            "callback setup awaits open-protocol resolution"),
                        N00BB_LINK_STAGE_SEMANTICS, &reference);
                core_string_t provenance = core_source_file_path(
                    &unit->source);
                if (!deferred
                    || !core_link_reference_set_source_info(link, reference,
                        link_source_location(node), provenance)) return false;
                if (!n00bb_bind_site_reference(unit, site, reference))
                    return false;
                continue;
            }
            if (symbol && symbol->kind == PARSE_SYM_FUNCTION
                && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.init"))) continue;
            if (symbol && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.implicit-package"))) continue;
            if (n00bb_builtin_function(symbol)) {
                core_link_reference_t *reference = nullptr;
                bool ok = core_link_unit_reference_at_scoped_kind(link, unit,
                    node, site->scope, CORE_STRING_STATIC("builtins"),
                    symbol->name, PARSE_SYM_FUNCTION, &reference);
                core_string_t provenance = core_source_file_path(&unit->source);
                if (!ok || !core_link_reference_set_source_info(link,
                        reference, link_source_location(node), provenance))
                    return false;
                if (!n00bb_bind_site_reference(unit, site, reference))
                    return false;
                continue;
            }
            if (symbol && symbol->kind == PARSE_SYM_FUNCTION
                && module_member_is_call(grammar, node)
                && n00bb_function_has_overloads(state, site->symbol,
                       (n00bb_call_lookup_t){
                           .unqualified = !site->dotted_path,
                           .origin_module = site->origin_module,
                           .name = site->dotted_path ? (core_string_t){0}
                                                     : site->member_name})) {
                core_link_reference_t *reference = nullptr;
                bool deferred =
                    core_link_unit_add_deferred_reference_at_scoped_kind(
                        link, unit, node, site->scope, PARSE_SYM_FUNCTION,
                        CORE_STRING_STATIC(
                            "overloaded call awaits overload selection"),
                        N00BB_LINK_STAGE_SEMANTICS, &reference);
                core_string_t provenance = core_source_file_path(
                    &unit->source);
                if (!deferred
                    || !core_link_reference_set_source_info(link, reference,
                        link_source_location(node), provenance)
                    || !n00bb_bind_site_reference(unit, site, reference))
                    return false;
                continue;
            }
            core_translation_unit_t *owner = linkable_module_symbol(symbol)
                ? unit_for_symbol(grammar, units, symbol) : nullptr;
            if (!owner) {
                bool trait_call = site->dotted_path
                    && n00bb_is_trait_dispatch_reference(grammar, unit, node);
                if (!site->dispatch_trait.data && !trait_call)
                    continue;
                core_string_t reason = site->dispatch_trait.data
                    ? CORE_STRING_STATIC(
                        "index dispatch awaits joint source inference")
                    : CORE_STRING_STATIC(
                        "trait dispatch awaits joint source inference");
                core_link_reference_t *reference = nullptr;
                bool deferred =
                    core_link_unit_add_deferred_reference_at_scoped_kind(
                    link, unit, node, site->scope,
                    PARSE_SYM_FUNCTION,
                    reason,
                    N00BB_LINK_STAGE_SEMANTICS, &reference);
                core_string_t provenance = core_source_file_path(&unit->source);
                if (!deferred
                    || !core_link_reference_set_source_info(link,
                        reference, link_source_location(node),
                        provenance)) return false;
                if (!n00bb_bind_site_reference(unit, site, reference))
                    return false;
                continue;
            }
            core_buffer_t key = {0};
            if (!append_link_symbol_key(grammar, owner, symbol, &key))
                return false;
            core_link_reference_t *reference = nullptr;
            bool ok = core_link_unit_reference_at_scoped_kind(link, unit,
                node, site->scope, link_symbol_module(owner, symbol),
                core_buffer_string(&key), (uint32_t)symbol->kind,
                &reference);
            core_buffer_release(&key);
            core_string_t provenance = core_source_file_path(&unit->source);
            if (!ok || !core_link_reference_set_source_info(link, reference,
                    link_source_location(node), provenance))
                return false;
            if (!n00bb_bind_site_reference(unit, site, reference))
                return false;
        }
    }
    return true;
}

bool
bind_link_references_to_nodes(parse_grammar_t *grammar, core_link_unit_t *link)
{
    for (size_t i = 0; i < core_link_unit_reference_count(link); i++) {
        core_link_reference_t *reference =
            core_link_unit_reference_at_index(link, i);
        if (core_link_reference_is_deferred(reference)) continue;
        parse_tree_t *use_site = (parse_tree_t *)
            core_link_reference_use_site(reference);
        core_link_declaration_t *target =
            core_link_reference_target(reference);
        if (core_link_declaration_is_external_requirement(target)) continue;
        n00bb_link_payload_t *payload = target
            ? core_link_declaration_payload(target) : nullptr;
        if (!use_site || !payload || !payload->symbol
            || !parse_tree_set_resolution_symbol(use_site, payload->symbol)
            || !bind_array_shape_type_source(grammar, use_site,
                                             payload->symbol)) {
            core_translation_unit_t *unit =
                core_link_reference_referrer(reference);
            if (unit) {
                core_string_t key = core_link_reference_key(reference);
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, use_site ? n00bb_node_span(use_site)
                                   : (core_diag_location_t){},
                    "connecting uses to declarations",
                    "the use of `%.*s` resolved to a declaration with no "
                    "source", (int)key.u8_bytes, key.data);
            }
            return false;
        }
    }
    return true;
}

core_link_stage_result_t
n00bb_link_stage_symbols(core_link_unit_t *link, uint32_t stage_kind,
                        void *opaque)
{
    (void)stage_kind;
    n00bb_link_analysis_t *analysis = opaque;
    for (size_t i = 0; i < analysis->units.len; i++)
        if (!validate_type_value_namespaces(analysis->units.data[i]))
            return CORE_LINK_STAGE_RESULT_FAILED;
    if (!validate_package_interfaces(analysis)
        || !bind_imported_module_scopes(&analysis->units)
        || !populate_package_interfaces(analysis)
        || !reject_ambiguous_external_names(analysis))
        return CORE_LINK_STAGE_RESULT_FAILED;
    bool names_resolved = resolve_imported_module_symbols(analysis->grammar,
                                                          &analysis->units);
    bool brings_valid = check_import_brings(analysis);
    bool declarations_indexed = register_link_declarations(link,
        analysis->grammar, &analysis->units,
        &analysis->declarations_by_symbol);
    core_translation_unit_t *root = analysis->units.data[0];
    if (!declarations_indexed
        && !call_units_failed(analysis->units.data, analysis->units.len)
        && !core_link_unit_has_fatal_diagnostics(link))
        n00bb_internal_error(core_translation_unit_diagnostics(root), root,
            (core_diag_location_t){}, "indexing declarations",
            "could not index the program's declarations");
    if (!names_resolved || !brings_valid || !declarations_indexed)
        return CORE_LINK_STAGE_RESULT_FAILED;
    if (!register_link_use_sites(link, analysis->grammar,
                                 &analysis->units)) {
        if (!call_units_failed(analysis->units.data, analysis->units.len))
            n00bb_internal_error(core_translation_unit_diagnostics(root),
                root, (core_diag_location_t){}, "indexing declarations",
                "could not record the program's references to its "
                "declarations");
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!core_link_unit_validate(link)) {
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!bind_link_references_to_nodes(analysis->grammar, link))
        return CORE_LINK_STAGE_RESULT_FAILED;
    bind_return_call_sources(analysis);
    return CORE_LINK_STAGE_RESULT_COMPLETE;
}


core_link_declaration_t *
link_declaration_for_symbol(n00bb_link_analysis_t *analysis,
                            parse_sym_entry_t *symbol)
{
    if (!analysis || !symbol) return nullptr;
    symbol = canonical_symbol(symbol);
    bool found = false;
    core_link_declaration_t *declaration = core_dict_get(
        &analysis->declarations_by_symbol, symbol, &found);
    return found ? declaration : nullptr;
}

bool
link_declaration_has_only_interface_definition(
    core_link_declaration_t *declaration)
{
    bool saw_definition = false;
    for (size_t i = 0; i < core_link_declaration_site_count(declaration); i++) {
        core_link_declaration_site_view_t site = {0};
        if (!core_link_declaration_site(declaration, i, &site)
            || !site.is_definition) continue;
        saw_definition = true;
        if (site.origin != CORE_LINK_ORIGIN_LIBRARY) return false;
    }
    return saw_definition;
}

parse_sym_entry_t *
lifetime_function_owner(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    if (!symbol) return nullptr;
    parse_sym_entry_t *owner = link_effect_owner_symbol(
        grammar, symbol->decl_node);
    for (size_t i = 0; !owner && i < symbol->definition_count; i++)
        owner = link_effect_owner_symbol(grammar, symbol->definition_nodes[i]);
    for (parse_scope_t *scope = symbol->scope; !owner && scope;
         scope = scope->parent) {
        if (core_string_eq(scope->scope_tag,
                           CORE_STRING_STATIC("function"))) {
            if (scope->node)
                owner = parse_tree_sym(scope->node);
        }
        else if (core_string_eq(scope->scope_tag,
                                CORE_STRING_STATIC("context-hook"))) {
            owner = link_effect_owner_symbol(grammar, scope->node);
            for (parse_scope_t *parent = scope->parent;
                 !owner && parent; parent = parent->parent) {
                if (!core_string_eq(parent->scope_tag,
                                    CORE_STRING_STATIC("context"))) continue;
                parse_sym_entry_t *candidate = parent->node
                    ? parse_tree_sym(parent->node) : nullptr;
                if (context_symbol(grammar, candidate)) owner = candidate;
            }
        }
        if (owner && context_symbol(grammar, owner)) break;
        if (owner && !core_string_eq(scope->scope_tag,
                                     CORE_STRING_STATIC("function")))
            owner = nullptr;
    }
    return owner;
}

static bool
record_symbol_lifetime_starts(parse_sym_entry_t *symbol,
                              ir_value_t *value,
                              ir_func_t *function,
                              parse_sym_phase_t phase,
                              size_t *start_count)
{
    if (!symbol || !value || !function || !start_count) return false;
    if (!parse_sym_begin_lifetime_range(symbol, phase, function, value))
        return false;
    *start_count = 0;
    if (value->kind == IR_VAL_PARAM) {
        ir_block_t *entry = ir_block_entry(function);
        if (!entry || !parse_sym_set_lifetime_start(symbol,
                (parse_sym_lifetime_start_t){
                    .phase = phase,
                    .function = function,
                    .binding = value,
                    .point = entry,
                    .at_entry = true,
                })) return false;
        *start_count = 1;
        return true;
    }
    if (value->kind == IR_VAL_INSTR) {
        if (!value->def || !parse_sym_set_lifetime_start(symbol,
                (parse_sym_lifetime_start_t){
                    .phase = phase,
                    .function = function,
                    .binding = value,
                    .point = value->def,
                })) return false;
        *start_count = 1;
        return true;
    }
    if (value->kind != IR_VAL_LOCAL || !value->local) return false;

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!ir_instr_is_binding_initialization(instruction)
                || instruction->op != IR_OP_COPY
                || instruction->dest_kind != IR_DEST_LOCAL
                || instruction->dest_local != value->local)
                continue;
            if (!parse_sym_set_lifetime_start(symbol,
                    (parse_sym_lifetime_start_t){
                        .phase = phase,
                        .function = function,
                        .binding = value,
                        .point = instruction,
                    })) return false;
            (*start_count)++;
        }
    }
    if (!*start_count) {
        ir_block_t *entry = ir_block_entry(function);
        if (!entry || !parse_sym_set_lifetime_start(symbol,
                (parse_sym_lifetime_start_t){
                    .phase = phase,
                    .function = function,
                    .binding = value,
                    .point = entry,
                    .at_entry = true,
                })) return false;
        *start_count = 1;
    }
    return *start_count != 0;
}

static bool
record_symbol_cfg_liveness(parse_sym_entry_t *symbol,
                           ir_value_t *value,
                           ir_func_t *function,
                           parse_sym_phase_t phase,
                           n00bb_link_payload_t *facts)
{
    if (!value) {
        facts->cfg_liveness_complete = false;
        facts->cfg_liveness_missing_bindings++;
        return false;
    }
    if (!function || value->owner != function
        || ir_func_execution_phase(function)
            != (phase == PARSE_SYM_PHASE_COMPTIME
                ? IR_PHASE_COMPTIME : IR_PHASE_RUNTIME)) {
        facts->cfg_liveness_complete = false;
        facts->cfg_liveness_owner_mismatches++;
        return false;
    }
    if (value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
        && value->kind != IR_VAL_INSTR) {
        facts->cfg_liveness_complete = false;
        return false;
    }
    if (!function->n_blocks) {
        facts->cfg_liveness_complete = false;
        facts->cfg_liveness_empty_functions++;
        return false;
    }

    size_t start_count = 0;
    bool complete = record_symbol_lifetime_starts(
        symbol, value, function, phase, &start_count);
    facts->cfg_liveness_start_points += start_count;
    if (!complete) facts->cfg_liveness_missing_starts++;
    ir_cleanup_range_t range;
    if (!ir_value_cleanup_range(value, nullptr, nullptr, &range)) {
        facts->cfg_liveness_complete = false;
        return false;
    }
    complete &= range.resolved;
    facts->cfg_liveness_end_points += range.point_endpoints;
    facts->cfg_liveness_end_edges += range.edge_endpoints;
    facts->cfg_liveness_direct_live_edges += range.direct_live_edges;
    facts->cfg_liveness_direct_dead_edges += range.direct_dead_edges;
    facts->cfg_liveness_direct_unknown_edges += range.direct_unknown_edges;
    facts->cfg_liveness_alias_live_edges += range.alias_live_edges;
    facts->cfg_liveness_alias_dead_edges += range.alias_dead_edges;
    facts->cfg_liveness_alias_unknown_edges += range.alias_unknown_edges;
    facts->cfg_liveness_range_ends += range.range_ends;
    facts->cfg_liveness_edges += range.cfg_edges;
    facts->cfg_liveness_complete &= complete;
    bool range_complete = complete;
    bool range_recorded = parse_sym_finish_lifetime_range(
        symbol, phase, function, value, range_complete);
    facts->cfg_liveness_ranges++;
    facts->cfg_liveness_complete_ranges += range_recorded && range_complete;
    return range_complete && range_recorded;
}

core_link_stage_result_t
n00bb_link_stage_cfg_liveness(core_link_unit_t *link, uint32_t stage_kind,
                             void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    bool complete = analysis && analysis->units.len != 0;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *payload = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
            : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        if (!symbol
            || (symbol->kind != PARSE_SYM_FUNCTION
                && !context_symbol(analysis->grammar, symbol))
            || link_declaration_has_only_interface_definition(declaration))
            continue;
        payload->cfg_liveness_complete = true;
        payload->cfg_liveness_symbols = 0;
        payload->cfg_liveness_ranges = 0;
        payload->cfg_liveness_complete_ranges = 0;
        payload->cfg_liveness_start_points = 0;
        payload->cfg_liveness_end_points = 0;
        payload->cfg_liveness_end_edges = 0;
        payload->cfg_liveness_missing_starts = 0;
        payload->cfg_liveness_edges = 0;
        payload->cfg_liveness_direct_live_edges = 0;
        payload->cfg_liveness_direct_dead_edges = 0;
        payload->cfg_liveness_direct_unknown_edges = 0;
        payload->cfg_liveness_alias_live_edges = 0;
        payload->cfg_liveness_alias_dead_edges = 0;
        payload->cfg_liveness_alias_unknown_edges = 0;
        payload->cfg_liveness_range_ends = 0;
        payload->cfg_liveness_missing_bindings = 0;
        payload->cfg_liveness_incomplete_blocks = 0;
        payload->cfg_liveness_owner_mismatches = 0;
        payload->cfg_liveness_empty_functions = 0;
    }

    for (size_t i = 0; analysis && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) {
            complete = false;
            continue;
        }
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM) continue;
                if (parse_sym_binding_origin(symbol)) continue;
                if (is_module_global_symbol(analysis->grammar, symbol))
                    continue;
                if (!(symbol->phases & PARSE_SYM_PHASE_RUNTIME)) continue;
                parse_sym_entry_t *owner_symbol =
                    lifetime_function_owner(analysis->grammar, symbol);
                if (!owner_symbol) {
                    if (scope_has_lexical_lifetime(symbol->scope)
                        && symbol->has_runtime_binding)
                        complete = false;
                    continue;
                }
                core_link_declaration_t *owner_declaration =
                    link_declaration_for_symbol(analysis, owner_symbol);
                n00bb_link_payload_t *owner_facts = owner_declaration
                    ? core_link_declaration_binding(owner_declaration,
                        CORE_STRING_STATIC(
                            "n00bb.semantic.declaration-facts"))
                    : nullptr;
                if (!owner_facts) {
                    complete = false;
                    continue;
                }
                size_t bindings = parse_sym_function_binding_count(
                    symbol, PARSE_SYM_PHASE_RUNTIME);
                if (!bindings) {
                    owner_facts->cfg_liveness_complete = false;
                    owner_facts->cfg_liveness_missing_bindings++;
                    complete = false;
                    continue;
                }
                for (size_t b = 0; b < bindings; b++) {
                    const parse_sym_function_binding_t *binding =
                        parse_sym_function_binding_at(symbol,
                            PARSE_SYM_PHASE_RUNTIME, b);
                    owner_facts->cfg_liveness_symbols++;
                    bool binding_complete = binding
                        && record_symbol_cfg_liveness(symbol,
                            binding ? binding->binding : nullptr,
                            binding ? binding->function : nullptr,
                            PARSE_SYM_PHASE_RUNTIME, owner_facts);
                    complete &= binding_complete;
                }
            }
        }
    }
    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.cfg-liveness"), 1, stage_kind,
        complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                 : CORE_LINK_CONTRIBUTION_UNAVAILABLE,
        CORE_STRING_STATIC("n00bb.post-lowering-cfg-liveness"), 1,
        nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}

static n00bb_cleanup_release_policy_t
cleanup_release_policy(const parse_sym_entry_t *symbol)
{
    if (!symbol || symbol->storage_class_required
        || (symbol->selected_storage_class.data
            && symbol->selected_storage_class.u8_bytes))
        return N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE;
    switch (symbol->storage_duration) {
    case PARSE_SYM_STORAGE_AUTOMATIC:
    case PARSE_SYM_STORAGE_COMPTIME:
        return N00BB_CLEANUP_RELEASE_NONE;
    case PARSE_SYM_STORAGE_MODULE:
    case PARSE_SYM_STORAGE_THREAD:
    case PARSE_SYM_STORAGE_DYNAMIC:
        return N00BB_CLEANUP_RELEASE_RUNTIME_MANAGED;
    case PARSE_SYM_STORAGE_UNSPECIFIED:
        return N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE;
    }
    return N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE;
}

static const parse_sym_lifetime_range_t *
cleanup_lifetime_range(const parse_sym_entry_t *symbol,
                      parse_sym_phase_t phase, ir_func_t *function,
                      ir_value_t *binding)
{
    for (size_t i = 0; i < parse_sym_lifetime_range_count(symbol); i++) {
        const parse_sym_lifetime_range_t *range =
            parse_sym_lifetime_range_at(symbol, i);
        if (range && range->phase == phase
            && range->function == function && range->binding == binding)
            return range;
    }
    return nullptr;
}

typedef struct {
    ir_summaries_t *summaries;
    ir_func_t *function;
} cleanup_escape_t;

static bool
cleanup_binding_escapes(void *context, const ir_value_t *binding)
{
    cleanup_escape_t *escape = context;
    return n00bb_binding_escapes(escape->summaries, escape->function, binding);
}

static bool
cleanup_fact_add(ir_summaries_t *escapes, n00bb_link_payload_t *owner_facts,
                 parse_sym_entry_t *symbol, ir_func_t *function,
                 ir_value_t *binding, parse_sym_phase_t phase,
                 const parse_sym_lifetime_range_t *range)
{
    if (!owner_facts || !symbol) return false;
    for (size_t i = 0; i < owner_facts->cleanup_facts.len; i++) {
        n00bb_cleanup_fact_t *old = &owner_facts->cleanup_facts.data[i];
        if (old->symbol == symbol && old->function == function
            && old->binding == binding && old->phase == phase) return true;
    }

    n00bb_cleanup_fact_t fact = {
        .symbol = symbol,
        .function = function,
        .binding = binding,
        .phase = phase,
        .storage_duration = symbol->storage_duration,
        .release_policy = cleanup_release_policy(symbol),
    };
    bool complete = function || binding
        ? range && range->complete : true;
    cleanup_escape_t escape = {.summaries = escapes, .function = function};
    ir_cleanup_range_t cleanup;
    if (range && binding) {
        if (!ir_value_cleanup_range(binding, cleanup_binding_escapes,
                                    &escape, &cleanup))
            complete = false;
        else {
            complete &= cleanup.complete;
            fact.cfg_edges = cleanup.cfg_edges;
            fact.exit_edges = cleanup.exit_edges;
            fact.safe_exit_edges = cleanup.safe_exit_edges;
            fact.live_exit_edges = cleanup.live_exit_edges;
            fact.unknown_exit_edges = cleanup.unknown_exit_edges;
            fact.point_endpoints = cleanup.point_endpoints;
            fact.edge_endpoints = cleanup.edge_endpoints;
            fact.safe_endpoints = cleanup.point_endpoints
                + cleanup.edge_endpoints;
        }
    }
    fact.range_complete = complete;
    core_list_push(owner_facts->cleanup_facts, fact);
    owner_facts->cleanup_range_count++;
    owner_facts->cleanup_complete_ranges += fact.range_complete;
    owner_facts->cleanup_cfg_edges += fact.cfg_edges;
    owner_facts->cleanup_endpoint_count += fact.safe_endpoints;
    owner_facts->cleanup_exit_edges += fact.exit_edges;
    owner_facts->cleanup_safe_exit_edges += fact.safe_exit_edges;
    owner_facts->cleanup_live_exit_edges += fact.live_exit_edges;
    owner_facts->cleanup_unknown_exit_edges += fact.unknown_exit_edges;
    switch (fact.release_policy) {
    case N00BB_CLEANUP_RELEASE_NONE:
        owner_facts->cleanup_no_release_ranges++;
        break;
    case N00BB_CLEANUP_RELEASE_RUNTIME_MANAGED:
        owner_facts->cleanup_runtime_managed_ranges++;
        break;
    case N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE:
        owner_facts->cleanup_unavailable_ranges++;
        break;
    }
    /* Liveness evidence may be open for borrowed values, but neither
       automatic storage nor runtime-managed storage needs an emitted
       release. Keep that evidence in the range fact; the obligation is
       complete once its release policy is known. */
    owner_facts->cleanup_obligations_complete &=
        fact.release_policy != N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE;
    return true;
}

core_link_stage_result_t
n00bb_link_stage_cleanup_obligations(core_link_unit_t *link,
                                    uint32_t stage_kind, void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    bool complete = analysis && analysis->units.len != 0;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *facts = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
            : nullptr;
        if (!facts) continue;
        core_list_free(facts->cleanup_facts);
        facts->cleanup_facts = core_list_new(n00bb_cleanup_fact_t);
        facts->cleanup_obligations_complete = true;
        facts->cleanup_range_count = 0;
        facts->cleanup_complete_ranges = 0;
        facts->cleanup_cfg_edges = 0;
        facts->cleanup_endpoint_count = 0;
        facts->cleanup_exit_edges = 0;
        facts->cleanup_safe_exit_edges = 0;
        facts->cleanup_live_exit_edges = 0;
        facts->cleanup_unknown_exit_edges = 0;
        facts->cleanup_no_release_ranges = 0;
        facts->cleanup_runtime_managed_ranges = 0;
        facts->cleanup_unavailable_ranges = 0;
    }

    for (size_t u = 0; analysis && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) {
            complete = false;
            continue;
        }
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM) continue;
                if (parse_sym_binding_origin(symbol)) continue;
                if (is_module_global_symbol(analysis->grammar, symbol)) {
                    if (!(symbol->phases & PARSE_SYM_PHASE_RUNTIME)) continue;
                    core_link_declaration_t *declaration =
                        link_declaration_for_symbol(analysis, symbol);
                    n00bb_link_payload_t *facts = declaration
                        ? core_link_declaration_binding(declaration,
                            CORE_STRING_STATIC(
                                "n00bb.semantic.declaration-facts"))
                        : nullptr;
                    bool added = cleanup_fact_add(analysis->escapes, facts,
                        symbol, nullptr, nullptr, PARSE_SYM_PHASE_RUNTIME,
                        nullptr);
                    complete &= added && facts
                        && facts->cleanup_obligations_complete;
                    continue;
                }
                if (!symbol->type
                    || !(symbol->phases & PARSE_SYM_PHASE_RUNTIME)
                    || !scope_has_lexical_lifetime(scope)) continue;

                parse_sym_entry_t *owner_symbol =
                    lifetime_function_owner(analysis->grammar, symbol);
                core_link_declaration_t *owner_declaration = owner_symbol
                    ? link_declaration_for_symbol(analysis, owner_symbol)
                    : nullptr;
                n00bb_link_payload_t *owner_facts = owner_declaration
                    ? core_link_declaration_binding(owner_declaration,
                        CORE_STRING_STATIC(
                            "n00bb.semantic.declaration-facts"))
                    : nullptr;
                size_t binding_count = parse_sym_function_binding_count(
                    symbol, PARSE_SYM_PHASE_RUNTIME);
                if (!owner_facts || !binding_count) {
                    complete = false;
                    if (owner_facts)
                        owner_facts->cleanup_obligations_complete = false;
                    continue;
                }
                for (size_t b = 0; b < binding_count; b++) {
                    const parse_sym_function_binding_t *binding =
                        parse_sym_function_binding_at(symbol,
                            PARSE_SYM_PHASE_RUNTIME, b);
                    ir_func_t *function = binding
                        ? binding->function : nullptr;
                    ir_value_t *value = binding
                        ? binding->binding : nullptr;
                    const parse_sym_lifetime_range_t *range =
                        cleanup_lifetime_range(symbol,
                            PARSE_SYM_PHASE_RUNTIME, function, value);
                    bool added = binding && function && value
                        && cleanup_fact_add(analysis->escapes, owner_facts,
                            symbol, function, value, PARSE_SYM_PHASE_RUNTIME,
                            range);
                    complete &= added && range && range->complete
                        && owner_facts->cleanup_obligations_complete;
                }
            }
        }
    }

    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.finalization"), 1, stage_kind,
        complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                 : CORE_LINK_CONTRIBUTION_UNAVAILABLE,
        CORE_STRING_STATIC("n00bb.post-lowering-cleanup-obligations"), 1,
        nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}

static bool
storage_lifetime_range_is_complete(const parse_sym_entry_t *symbol,
                                   ir_func_t *function,
                                   ir_value_t *binding)
{
    for (size_t i = 0; i < parse_sym_lifetime_range_count(symbol); i++) {
        const parse_sym_lifetime_range_t *range =
            parse_sym_lifetime_range_at(symbol, i);
        if (range && range->phase == PARSE_SYM_PHASE_RUNTIME
            && range->function == function && range->binding == binding)
            return range->complete && range->start_count != 0;
    }
    return false;
}

static bool
storage_lifetime_symbol_is_complete(n00bb_link_analysis_t *analysis,
                                   core_link_unit_t *link,
                                   parse_sym_entry_t *symbol)
{
    if (!analysis || !link || !symbol
        || (symbol->kind != PARSE_SYM_VARIABLE
            && symbol->kind != PARSE_SYM_PARAM)) return false;
    if (parse_sym_binding_origin(symbol)) return true;
    if (!(symbol->phases & PARSE_SYM_PHASE_RUNTIME)) return true;
    if (!symbol->type || !symbol->lifetime.has_region
        || symbol->storage_duration == PARSE_SYM_STORAGE_UNSPECIFIED)
        return false;

    if (symbol->storage_duration == PARSE_SYM_STORAGE_COMPTIME)
        return symbol->has_comptime_value;

    if (is_module_global_symbol(analysis->grammar, symbol)) {
        if (symbol->storage_duration != PARSE_SYM_STORAGE_MODULE
            && symbol->storage_duration != PARSE_SYM_STORAGE_THREAD)
            return false;
        core_link_declaration_t *declaration =
            link_declaration_for_symbol(analysis, symbol);
        n00bb_link_payload_t *facts = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
            : nullptr;
        return declaration
            && !core_link_declaration_is_external_requirement(declaration)
            && facts && facts->facts_complete && facts->semantic_type
            && facts->storage_duration == symbol->storage_duration;
    }

    if (!scope_has_lexical_lifetime(symbol->scope)) return false;
    parse_sym_entry_t *owner_symbol = lifetime_function_owner(
        analysis->grammar, symbol);
    core_link_declaration_t *owner_declaration = owner_symbol
        ? link_declaration_for_symbol(analysis, owner_symbol) : nullptr;
    n00bb_link_payload_t *owner_facts = owner_declaration
        ? core_link_declaration_binding(owner_declaration,
            CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
        : nullptr;
    size_t binding_count = parse_sym_function_binding_count(
        symbol, PARSE_SYM_PHASE_RUNTIME);
    if (!owner_facts || !owner_facts->cfg_liveness_complete || !binding_count)
        return false;
    for (size_t i = 0; i < binding_count; i++) {
        const parse_sym_function_binding_t *binding =
            parse_sym_function_binding_at(symbol, PARSE_SYM_PHASE_RUNTIME, i);
        if (!binding || !binding->function || !binding->binding
            || n00bb_binding_escapes(analysis->escapes, binding->function,
                                     binding->binding)
            || !storage_lifetime_range_is_complete(symbol,
                binding->function, binding->binding)) return false;
    }
    return true;
}

core_link_stage_result_t
n00bb_link_stage_storage_lifetimes(core_link_unit_t *link,
                                   uint32_t stage_kind, void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    bool complete = analysis && analysis->units.len != 0;
    for (size_t u = 0; analysis && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) {
            complete = false;
            continue;
        }
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM) continue;
                bool symbol_complete =
                    storage_lifetime_symbol_is_complete(analysis, link,
                                                        symbol);
                complete &= symbol_complete;
                if (is_module_global_symbol(analysis->grammar, symbol)) {
                    core_link_declaration_t *declaration =
                        link_declaration_for_symbol(analysis, symbol);
                    n00bb_link_payload_t *facts = declaration
                        ? core_link_declaration_binding(declaration,
                            CORE_STRING_STATIC(
                                "n00bb.semantic.declaration-facts"))
                        : nullptr;
                    if (facts)
                        facts->storage_lifetime_complete = symbol_complete;
                }
            }
        }
    }

    /* Cross-check every runtime global in the canonical declaration set. */
    for (size_t i = 0; link
         && i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *facts = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
            : nullptr;
        parse_sym_entry_t *symbol = facts ? facts->symbol : nullptr;
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
            || !(symbol->phases & PARSE_SYM_PHASE_RUNTIME)) continue;
        bool symbol_complete = storage_lifetime_symbol_is_complete(
            analysis, link, symbol);
        facts->storage_lifetime_complete = symbol_complete;
        complete &= symbol_complete;
    }

    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.storage-lifetimes"), 1, stage_kind,
        complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                 : CORE_LINK_CONTRIBUTION_UNAVAILABLE,
        CORE_STRING_STATIC("n00bb.post-cfg-storage-lifetimes"), 1,
        nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}

n00bb_link_analysis_t *
n00bb_link_analysis_new(parse_grammar_t *grammar, ir_ctx_t *ir_context,
    const th_env *const *linked_interfaces, size_t linked_interface_count,
    const core_string_t *native_roots, size_t native_root_count,
    bool may_link_further, n00bb_resolver_t *resolver)
{
    n00bb_link_analysis_t *analysis = core_alloc(n00bb_link_analysis_t);
    const th_env **interfaces = core_alloc_array(const th_env *,
                                                 linked_interface_count + 1);
    for (size_t i = 0; i < linked_interface_count; i++)
        interfaces[i] = linked_interfaces[i];
    *analysis = (n00bb_link_analysis_t){
        .grammar = grammar,
        .ir_context = ir_context,
        .units = core_list_new(core_translation_unit_ptr_t),
        .trait_specializations =
            core_list_new(n00bb_trait_specialization_ptr_t),
        .reachable_functions = core_list_new(n00bb_ir_func_ptr_t),
        .emitted_functions = core_list_new(n00bb_ir_func_ptr_t),
        .specializations = core_list_new(n00bb_specialization_t),
        .specialization_requests =
            core_list_new(n00bb_specialization_request_t),
        .baked_roots = core_list_new(n00bb_baked_root_t),
        .linked_interfaces = interfaces,
        .linked_interface_count = linked_interface_count,
        .native_roots = native_roots,
        .native_root_count = native_root_count,
        .may_link_further = may_link_further,
        .resolver = resolver,
    };
    core_dict_init(&analysis->declarations_by_symbol, nullptr, nullptr);
    core_dict_init_string(&analysis->baked_root_index);
    if (!n00bb_baked_graph_load(analysis)) {
        n00bb_link_analysis_free(analysis);
        return nullptr;
    }
    return analysis;
}

void
n00bb_link_analysis_free(n00bb_link_analysis_t *analysis)
{
    if (!analysis) return;
    if (analysis->dead_code_sites_ready)
        core_dict_free(&analysis->dead_code_sites);
    for (size_t i = 0; i < analysis->failed_regions.len; i++)
        core_free(analysis->failed_regions.data[i].module.data);
    core_list_free(analysis->failed_regions);
    ir_summaries_free(analysis->summaries);
    ir_summaries_free(analysis->escapes);
    core_list_free(analysis->assert_sites);
    core_list_free(analysis->missing_returns);
    n00bb_interface_summaries_free(analysis->interface_summaries);
    core_list_free(analysis->initializer_order);
    core_free((void *)analysis->linked_interfaces);
    th_env_free(analysis->export_environment);
    th_env_free(analysis->interface_environment);
    for (size_t i = 0; i < analysis->trait_specializations.len; i++)
        core_free(analysis->trait_specializations.data[i]);
    core_list_free(analysis->trait_specializations);
    core_list_free(analysis->reachable_functions);
    core_list_free(analysis->emitted_functions);
    core_list_free(analysis->callback_handlers);
    core_list_free(analysis->callback_handler_types);
    if (analysis->unit_of_module_ready)
        core_dict_free(&analysis->unit_of_module);
    if (analysis->global_symbols_ready)
        core_dict_free(&analysis->global_symbols);
    if (analysis->escaped_owner) core_dict_free(&analysis->escaped_globals);
    core_list_free(analysis->specializations);
    core_list_free(analysis->specialization_requests);
    core_list_free(analysis->record_requests);
    for (size_t p = 0; analysis->record_requests_indexed
         && p <= N00BB_RECORD_C; p++)
        core_dict_free(&analysis->record_requests_by_type[p]);
    if (analysis->enum_symbols_indexed)
        core_dict_free(&analysis->enum_symbols_by_type);
    for (size_t i = 0; i < analysis->static_graph_nodes.len; i++) {
        n00bb_static_graph_node_t *node =
            &analysis->static_graph_nodes.data[i].node;
        core_free((void *)node->bytes);
        core_free(node->symbol.data);
        for (size_t a = 0; a < node->alias_count; a++)
            core_free(node->aliases[a].data);
        core_free((void *)node->aliases);
        core_free(analysis->static_graph_nodes.data[i].type_name.data);
    }
    core_list_free(analysis->static_graph_nodes);
    core_list_free(analysis->static_graph_edges);
    core_free(analysis->baked_failure_root.data);
    core_free(analysis->baked_failure_path.data);
    core_free(analysis->baked_failure_message.data);
    for (size_t i = 0; i < analysis->baked_roots.len; i++)
        core_free(analysis->baked_roots.data[i].identifier.data);
    core_list_free(analysis->baked_roots);
    for (size_t i = 0; i < analysis->static_graph_slots.len; i++) {
        core_free(analysis->static_graph_slots.data[i].module.data);
        core_free(analysis->static_graph_slots.data[i].name.data);
    }
    core_list_free(analysis->static_graph_slots);
    core_list_free(analysis->units);
    core_dict_free(&analysis->declarations_by_symbol);
    core_dict_free(&analysis->baked_root_index);
    core_free(analysis);
}

core_link_stage_result_t
n00bb_link_stage_inputs(core_link_unit_t *link, uint32_t stage_kind,
                        void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    if (!core_link_unit_collect(link)
        || !core_link_unit_finalize_graph(link))
        return CORE_LINK_STAGE_RESULT_FAILED;
    for (size_t i = 0; i < core_link_unit_translation_unit_count(link); i++) {
        core_translation_unit_t *unit =
            core_link_unit_translation_unit(link, i);
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state) return CORE_LINK_STAGE_RESULT_FAILED;
        state->link_analysis = analysis;
        core_list_push(analysis->units, unit);
    }
    if (!analysis->units.len
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.source-closure"), 1, stage_kind,
            CORE_LINK_CONTRIBUTION_COMPLETE,
            CORE_STRING_STATIC("n00bb.import-preprocessing"), 1, nullptr,
            CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    return CORE_LINK_STAGE_RESULT_COMPLETE;
}

static bool
record_default_fields_support(th_ctx *probe, th_ctx *source,
                              th_type *receiver, core_string_t trait,
                              core_string_t method, core_dict_t *visiting)
{
    receiver = receiver ? th_resolve(receiver) : nullptr;
    if (receiver && n00bb_value_coercion_target(source, receiver))
        return false;
    th_type *shape = receiver
        ? th_type_definition(source, receiver) : nullptr;
    if (!shape || th_kind_of(th_resolve(shape)) != TH_RECORD
        || !th_is_closed(source, shape)) return false;
    bool present = false;
    core_dict_get(visiting, receiver, &present);
    if (present) return false;
    core_dict_put(visiting, receiver, receiver);
    bool supported = true;
    for (size_t i = 0; supported && i < th_record_len(shape); i++) {
        const th_member *field = th_record_member(shape, i);
        th_type *field_type = field && field->type
            ? th_type_clone(probe, field->type) : nullptr;
        th_env_method_t selected = {0};
        th_env_method_status_t status = field_type
            ? th_env_method_resolve(probe, field_type, trait, method,
                                    &selected)
            : TH_ENV_METHOD_NOT_FOUND;
        if (status == TH_ENV_METHOD_FOUND)
            supported = !selected.error_type
                || th_is_nil(selected.error_type);
        else if (status == TH_ENV_METHOD_NOT_FOUND)
            supported = record_default_fields_support(probe, source,
                field->type, trait, method, visiting);
        else supported = false;
    }
    core_dict_remove(visiting, receiver);
    return supported;
}

static bool
environment_has_trait(const th_env *environment, core_string_t name)
{
    for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration;
        if (th_env_declaration_at(environment, i, &declaration)
            && declaration.kind == TH_ENV_DECLARATION_TRAIT
            && declaration.qualified_name.data
            && core_string_eq(declaration.qualified_name, name)) return true;
    }
    return false;
}

static bool
register_flagset_operators(th_env *defaults, th_ctx *types,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *symbol, th_type *flagset)
{
    static const core_string_t operators[][2] = {
        {CORE_STRING_STATIC("Add"), CORE_STRING_STATIC("__add")},
        {CORE_STRING_STATIC("Subtract"), CORE_STRING_STATIC("__subtract")},
        {CORE_STRING_STATIC("Bit_And"), CORE_STRING_STATIC("__bit_and")},
        {CORE_STRING_STATIC("Bit_Or"), CORE_STRING_STATIC("__bit_or")},
        {CORE_STRING_STATIC("Bit_Xor"), CORE_STRING_STATIC("__bit_xor")},
    };
    core_string_t owner = symbol->name.data ? symbol->name
                                            : CORE_STRING_STATIC("");
    core_string_t package = n00bb_package_id(unit);
    if (!package.data) package = CORE_STRING_STATIC("");
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(operators) / sizeof(operators[0]);
         i++)
        ok = n00bb_register_operator_function(defaults, types,
            operators[i][0], operators[i][1], owner, flagset, flagset, flagset,
            unit->module.identifier, package,
            symbol_visibility(symbol));
    return ok;
}

bool
register_builtin_value_defaults(th_env *defaults, const th_env *base,
                                 core_link_unit_t *link,
                                 parse_grammar_t *grammar,
                                 n00bb_link_analysis_t *analysis)
{
    bool hash_available = environment_has_trait(base,
        CORE_STRING_STATIC("builtin.hashing.Hash"));
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symbols = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symbols || !module_scope(state)) continue;
        th_ctx *probe = th_ctx_new();
        th_ctx_set_probe_errors_observable(probe, false);
        core_string_t package = n00bb_package_id(unit);
        if (!package.data) package = CORE_STRING_STATIC("");
        bool ready = probe && th_ctx_set_environment(probe,
            base, unit->module.identifier, package);
        if (!ready) { th_ctx_free(probe); return false; }
        for (parse_scope_t *scope = symbols->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                bool interface_type = state->interface_module
                    && symbol->kind == PARSE_SYM_TYPEDEF
                    && parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.export-origin"));
                if (!(interface_type
                        || is_named_type_declaration(grammar, symbol))
                    || !symbol->type) continue;
                th_type *receiver = th_resolve(symbol->type);
                if (th_kind_of(receiver) != TH_NAMED) continue;
                bool record = core_string_eq(symbol->adt_kind,
                    CORE_STRING_STATIC("record"));
                bool object = core_string_eq(symbol->adt_kind,
                    CORE_STRING_STATIC("object"));
                bool scalar = core_string_eq(symbol->adt_kind,
                    CORE_STRING_STATIC("enum"))
                    || core_string_eq(symbol->adt_kind,
                        CORE_STRING_STATIC("flagset"))
                    || (symbol->member_scope
                        && core_string_eq(
                            symbol->member_scope->scope_tag,
                            CORE_STRING_STATIC("flagset")));
                if (!record && !scalar && !object) continue;
                core_string_t object_name = {0};
                if (object && th_named_info(receiver, &object_name, nullptr,
                                            nullptr))
                    receiver = record_template_type(state->types,
                        object_name, symbol);
                if (!receiver) continue;
                if (n00bb_flagset_type(unit, state, receiver, nullptr)
                    && !register_flagset_operators(defaults, state->types,
                        unit, symbol, receiver)) {
                    th_ctx_free(probe);
                    return false;
                }
                th_type *shape = th_type_definition(state->types, receiver);
                if ((!object && !th_is_closed(state->types, receiver))
                    || n00bb_value_coercion_target(state->types, receiver)
                    || (record && (!shape
                        || th_kind_of(th_resolve(shape)) != TH_RECORD
                        || !th_is_closed(state->types, shape)))
                    || (object && (!shape
                        || th_kind_of(th_resolve(shape))
                            != TH_INDIRECTION))) continue;
                th_type *candidate = th_type_clone(probe, receiver);
                th_env_method_t selected = {0};
                bool custom_equal = candidate &&
                    th_env_method_resolve(probe, candidate,
                        CORE_STRING_STATIC("Equal"),
                        CORE_STRING_STATIC("__equal"), &selected)
                        == TH_ENV_METHOD_FOUND;
                bool custom_hash = hash_available && candidate &&
                    th_env_method_resolve(probe, candidate,
                        CORE_STRING_STATIC("builtin.hashing.Hash"),
                        CORE_STRING_STATIC("hash"), &selected)
                        == TH_ENV_METHOD_FOUND;
                core_dict_t visiting;
                core_dict_init(&visiting, nullptr, nullptr);
                bool derive_equal = !custom_equal
                    && (object || (record && record_default_fields_support(
                        probe, state->types, receiver,
                        CORE_STRING_STATIC("Equal"),
                        CORE_STRING_STATIC("__equal"), &visiting)));
                bool derive_hash = hash_available && !custom_equal
                    && !custom_hash
                    && (scalar || object || record_default_fields_support(
                        probe, state->types, receiver,
                        CORE_STRING_STATIC("builtin.hashing.Hash"),
                        CORE_STRING_STATIC("hash"), &visiting));
                core_dict_free(&visiting);
                if (!derive_equal && !derive_hash) continue;
                for (size_t kind = 0; kind < 2; kind++) {
                    if (kind ? !derive_hash : !derive_equal) continue;
                    th_param parameters[] = {
                        {.mode = TH_IN, .type = receiver},
                        {.mode = TH_IN, .type = receiver},
                    };
                    th_type *signature = th_func_with_error(state->types,
                        th_name(state->types, kind ? CORE_STRING_STATIC("u128")
                                                   : CORE_STRING_STATIC("bool")),
                        th_name(state->types, CORE_STRING_STATIC("nil")), parameters,
                        kind ? 1 : 2, nullptr, nullptr, 0);
                    core_buffer_t generated_name = {0};
                    core_buffer_printf(&generated_name,
                        kind && scalar ? "__n00bb_scalar_hash_%016llx"
                        : kind && object ? "__n00bb_object_hash_%016llx"
                        : object ? "__n00bb_object_equal_%016llx"
                        : kind ? "__n00bb_record_hash_%016llx"
                             : "__n00bb_record_equal_%016llx",
                        (unsigned long long)th_hash(state->types, receiver));
                    parse_symtab_enter_scope_all(symbols,
                        module_scope(state));
                    parse_sym_entry_t *generated = signature
                        && generated_name.data
                        ? parse_symtab_add(symbols, CORE_STRING_STATIC(""),
                            core_buffer_string(&generated_name),
                            PARSE_SYM_FUNCTION, nullptr) : nullptr;
                    parse_symtab_leave_scope_all(symbols);
                    core_buffer_release(&generated_name);
                    if (!generated) { th_ctx_free(probe); return false; }
                    generated->type = generated->callable_contract = signature;
                    generated->error_type = th_name(state->types, CORE_STRING_STATIC("nil"));
                    generated->adt_kind = symbol->adt_kind;
                    generated->is_synthetic = true;
                    parse_sym_set_phases(generated, PARSE_SYM_PHASE_BOTH,
                        PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
                    if (!parse_sym_set_binding(generated,
                            CORE_STRING_STATIC("n00bb.generated-value-method"),
                            receiver)
                        || !register_link_declaration(link, unit, grammar,
                            &analysis->declarations_by_symbol, generated)) {
                        th_ctx_free(probe);
                        return false;
                    }
                    core_buffer_t qualified = {0};
                    bool named = append_environment_symbol_name(grammar,
                        unit, generated, &qualified);
                    bool ok = signature && qualified.data
                        && named
                        && th_env_register_function(defaults,
                            core_buffer_string(&qualified),
                            kind ? CORE_STRING_STATIC("hash")
                                 : CORE_STRING_STATIC("__equal"),
                            unit->module.identifier, package,
                            TH_ENV_PUBLIC, signature);
                    core_buffer_release(&qualified);
                    if (!ok) { th_ctx_free(probe); return false; }
                }
            }
        }
        th_ctx_free(probe);
    }
    return true;
}

