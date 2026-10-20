#include <string.h>

#include "compiler_link.h"
#include "compiler_baked.h"
#include "compiler_tu.h"
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0015"),
                    CORE_STRING_STATIC(
                        "variant switch cases must name variant arms"),
                    link_source_location(site->node));
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
    if (n00bb_unify_node_type(state->types, node, field_type)) return true;
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
        unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
        CORE_STRING_STATIC("field access type conflicts with its resolved field"),
        link_source_location(node));
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
resolved_member_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     n00bb_tu_state_t *state, parse_tree_t *node,
                     parse_sym_entry_t *member, th_type *receiver_type)
{
    if (member->kind == PARSE_SYM_ENUM_CONST)
        return n00bb_flagset_member(member)
            && n00bb_flagset_instance_access(grammar, unit, node, member)
            ? th_name(state->types, CORE_STRING_STATIC("bool")) : member->type;
    return receiver_type
        ? th_record_lookup_in(state->types, receiver_type, member->name)
        : nullptr;
}

bool
resolve_inferred_field_references(core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types || !state->annotations) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            parse_tree_t *node = site->node;
            if (!site->dotted_path
                || !nt_is(analysis->grammar, node, CORE_STRING_STATIC("postfix-expr"))
                || core_tree_num_children(node) < 3
                || !parse_tree_leaf_is(core_tree_child(node, 1),
                                       CORE_STRING_STATIC("."))
                || n00bb_is_trait_dispatch_reference(analysis->grammar, unit, node))
                continue;
            if (variant_assignment_arm(analysis->grammar, unit, node))
                continue;
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
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                            CORE_STRING_STATIC(
                                "field access has no type in its receiver"),
                            link_source_location(node));
                        return false;
                    }
                    if (!bind_resolved_field_type(unit, state, node,
                                                  member_type)) return false;
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
            if (!receiver_type || th_kind_of(receiver_type) == TH_VAR) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0019"),
                    CORE_STRING_STATIC(
                        "cannot resolve member access with an unknown receiver type"),
                    link_source_location(node));
                return false;
            }
            if (th_kind_of(receiver_type) == TH_RECORD) {
                core_string_t member = first_token_text(
                    core_tree_child(node, 2));
                th_type *member_type = record_lookup_text(state->types,
                    receiver_type, member);
                if (!member_type) {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "structural record has no field named '%.*s'",
                        (int)member.u8_bytes, member.data);
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0016"),
                        core_buffer_string(&message),
                        link_source_location(node));
                    core_buffer_release(&message);
                    return false;
                }
                if (!bind_resolved_field_type(unit, state, node,
                                              member_type)) return false;
                continue;
            }
            if (th_kind_of(receiver_type) != TH_NAMED) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0020"),
                    CORE_STRING_STATIC(
                        "member access requires a record value or trait-dispatched call"),
                    link_source_location(node));
                return false;
            }

            core_translation_unit_t *owner = nullptr;
            parse_scope_t *record_scope = record_scope_for_type(
                analysis->grammar, &analysis->units, receiver_type, &owner);
            if (!record_scope || !owner) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0020"),
                    CORE_STRING_STATIC(
                        "member access receiver has no visible record definition"),
                    link_source_location(node));
                return false;
            }
            parse_sym_entry_t *field = parse_symtab_lookup_in(record_scope,
                                                               member);
            bool external_field = field && field->owner_type
                && tu_state(owner)->interface_module;
            bool enum_constant = field && field->kind == PARSE_SYM_ENUM_CONST;
            if (!field || (!field->is_field && !enum_constant)
                || (!field->decl_node && !external_field)) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "record type has no field named '%.*s'",
                    (int)member.u8_bytes, member.data);
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0016"),
                    core_buffer_string(&message),
                    link_source_location(node));
                core_buffer_release(&message);
                return false;
            }
            parse_sym_entry_t *owner_type = field->owner_type;
            if (!field_access_is_visible(unit, owner, owner_type, field)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0021"),
                    CORE_STRING_STATIC(
                        "record field is not visible from this module"),
                    link_source_location(node));
                return false;
            }

            th_type *member_type = resolved_member_type(analysis->grammar,
                unit, state, node, field, receiver_type);
            if (!member_type) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0017"),
                    CORE_STRING_STATIC(
                        "record field symbol has no matching canonical field type"),
                    link_source_location(node));
                return false;
            }
            if (!bind_resolved_field_type(unit, state, node,
                                          member_type)) return false;
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
    return true;
}
core_list_decl(th_type_ptr_t);
bool
diagnose_known_missing_record_fields(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types || !state->annotations) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            parse_tree_t *node = site->node;
            if (!site->dotted_path
                || !nt_is(analysis->grammar, node, CORE_STRING_STATIC("postfix-expr"))
                || core_tree_num_children(node) < 3
                || !parse_tree_leaf_is(core_tree_child(node, 1),
                                       CORE_STRING_STATIC("."))
                || n00bb_is_trait_dispatch_reference(analysis->grammar, unit, node)
                || variant_payload_arm(analysis->grammar, unit, node)
                || variant_assignment_arm(analysis->grammar, unit, node))
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
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "record type has no field named '%.*s'",
                (int)member.u8_bytes, member.data);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0016"),
                core_buffer_string(&message),
                link_source_location(node));
            core_buffer_release(&message);
            return false;
        }
    }
    return true;
}
core_list_decl(th_member);

static bool
nominal_declaration_is_record(parse_tree_t *declaration)
{
    for (size_t i = 0; declaration && !core_tree_is_leaf(declaration)
         && i < core_tree_num_children(declaration); i++) {
        parse_tree_t *child = core_tree_child(declaration, i);
        if (core_tree_is_leaf(child)
            && parse_tree_leaf_is(child, CORE_STRING_STATIC("record")))
            return true;
    }
    return false;
}

static th_type *
record_representation(th_ctx *types, th_type *type)
{
    core_list_t(th_type_ptr_t) visited = core_list_new(th_type_ptr_t);
    type = type ? th_resolve(type) : nullptr;
    while (type && th_kind_of(type) != TH_RECORD) {
        bool cycle = false;
        for (size_t i = 0; i < visited.len; i++)
            cycle |= visited.data[i] == type;
        if (cycle) {
            type = nullptr;
            break;
        }
        core_list_push(visited, type);
        type = th_type_definition(types, type);
        type = type ? th_resolve(type) : nullptr;
    }
    core_list_free(visited);
    return type;
}






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
        if (alias_declaration_is_transparent(declaration))
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
                || !alias_declaration_is_transparent(dependency->decl_node))
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
                core_diagnostics_report(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                    CORE_STRING_STATIC(
                        "source module has no complete semantic state"));
            return false;
        }
        if (!universe) universe = state->types;
        else if (state->types != universe) {
            core_diagnostics_report(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "module import closure spans multiple type-inference contexts"));
            return false;
        }
    }
    return true;
}

bool
finalize_named_declaration_types(parse_grammar_t *grammar,
                                 core_list_t(core_translation_unit_ptr_t) *units)
{
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0007"),
                    CORE_STRING_STATIC(
                        "a value cannot be used as a type argument"),
                    link_source_location(reference));
                return false;
            }
            if (!symbol) {
                if (!name.data || !builtin_type_name(name)) {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "no visible type named '%.*s'",
                        (int)name.u8_bytes, name.data ? name.data : "");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0007"),
                        core_string_borrow(message.data, message.byte_len),
                        link_source_location(reference));
                    core_buffer_release(&message);
                    return false;
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0013"),
                    CORE_STRING_STATIC(
                        "could not bind a deferred local type reference"),
                    link_source_location(reference));
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                    CORE_STRING_STATIC(
                        "deferred type reference conflicts with its declaration"),
                    link_source_location(reference));
                return false;
            }
            if (resolved_type)
                parse_tree_set_type(reference, resolved_type);
            if (resolved_type)
                propagate_resolved_type_reference(grammar, unit, reference,
                                                  resolved_type);
            refresh_cached_type_path(grammar, unit, reference);
        }
    }

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
                        && alias_declaration_is_transparent(
                            symbol->decl_node)
                        && previous) {
                        type = th_unify_in_place(state->types, previous,
                                                 type);
                        if (!type) {
                            core_diagnostics_report_at(
                                core_translation_unit_diagnostics(unit), unit,
                                CORE_DIAG_ERROR,
                                CORE_STRING_STATIC("N00BB-T0002"),
                                CORE_STRING_STATIC(
                                    "transparent alias conflicts with its resolved target type"),
                                link_source_location(symbol->decl_node));
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
                            core_diagnostics_report_at(
                                core_translation_unit_diagnostics(owner),
                                owner, CORE_DIAG_ERROR,
                                CORE_STRING_STATIC("N00BB-T0002"),
                                CORE_STRING_STATIC(
                                    "type reference conflicts with its declaration"),
                                link_source_location(reference));
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
                    && alias_declaration_is_transparent(symbol->decl_node)
                    && transparent_alias_has_cycle(grammar, units, symbol)) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR,
                        CORE_STRING_STATIC("N00BB-T0012"),
                        CORE_STRING_STATIC("cyclic type alias definition"),
                        link_source_location(symbol->decl_node));
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                        CORE_STRING_STATIC(
                            "named declaration has no finalized semantic type"),
                        link_source_location(symbol->decl_node));
                    return false;
                }
                if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && alias_declaration_is_transparent(symbol->decl_node))
                    symbol->adt_kind = CORE_STRING_STATIC("alias");
                if (!symbol->type) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                        CORE_STRING_STATIC(
                            "named declaration has no finalized semantic type"),
                        link_source_location(symbol->decl_node));
                    return false;
                }
                if (nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("alias-stmt"))
                    && !alias_declaration_is_transparent(symbol->decl_node)) {
                    th_type *representation = declared_type_from_node(
                        grammar, unit, symbol->decl_node);
                    if (nominal_declaration_is_record(symbol->decl_node)
                        && !record_representation(state->types,
                                                  representation)) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0022"),
                            CORE_STRING_STATIC(
                                "a record declaration requires a record representation"),
                            link_source_location(symbol->decl_node));
                        return false;
                    }
                    if (!representation || !th_type_define(state->types,
                            symbol->type, representation)) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0012"),
                            CORE_STRING_STATIC("nominal type has no canonical representation"),
                            link_source_location(symbol->decl_node));
                        return false;
                    }
                    if (nominal_declaration_is_record(symbol->decl_node))
                        symbol->adt_kind = CORE_STRING_STATIC("record");
                    else
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
                        || !alias_declaration_is_transparent(
                            origin_declaration))
                    && th_named_info(th_resolve(symbol->type), nullptr,
                                     nullptr, nullptr)
                    && !parse_symtab_bind_nominal_type(
                        state->annotations->symtab, symbol->type, symbol))
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
    bool same_package = n00bb_same_package(use_unit, owner_unit);
    if (core_string_eq(field->visibility, CORE_STRING_STATIC("private")))
        return use_unit == owner_unit;
    if (field->kind == PARSE_SYM_ENUM_CONST) {
        if (core_string_eq(owner_type->visibility,
                           CORE_STRING_STATIC("private")))
            return use_unit == owner_unit;
        return same_package || core_string_eq(owner_type->visibility,
                                              CORE_STRING_STATIC("public"));
    }
    if (same_package) return true;
    if (!core_string_eq(field->visibility, CORE_STRING_STATIC("public"))
        || parse_sym_binding(owner_type,
            CORE_STRING_STATIC("n00bb.opaque-type"))) return false;
    return core_string_eq(owner_type->visibility,
        CORE_STRING_STATIC("public"))
        || parse_sym_binding(owner_type,
            CORE_STRING_STATIC("n00bb.exposed-record")) != nullptr;
}
static bool
imported_symbol_is_visible(parse_grammar_t *grammar,
                            core_translation_unit_t *use_unit,
                            core_translation_unit_t *owner,
                            parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    if (context_symbol(grammar, symbol)) return true;
    if (core_string_eq(symbol->visibility, CORE_STRING_STATIC("public")))
        return true;
    if (symbol_is_public_declaration(symbol))
        return true;
    return use_unit == owner;
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
overload_argument_fits(n00bb_tu_state_t *state, parse_tree_t *argument,
                       th_type *parameter, bool receiver, bool *typed)
{
    th_type *type = argument ? parse_tree_type(argument) : nullptr;
    if (!type || !parameter) {
        *typed = false;
        return true;
    }
    if (!th_is_closed(state->types, type)) {
        *typed = false;
        return !receiver || th_unify_probe(state->types, type, parameter);
    }
    th_type *element = nullptr;
    return th_unify_probe(state->types, type, parameter)
        || n00bb_value_coerces(state->types, type, parameter)
        || n00bb_dereferences_to(state->types, type, parameter)
        || (n00bb_option_type_element(parameter, &element)
            && (th_is_nil(type)
                || th_unify_probe(state->types, type, element)))
        || (parse_tree_integer_literal_origin(argument)
            && is_integer_type(th_resolve(parameter)));
}

static bool
overload_arguments_fit(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_sym_entry_t *candidate, th_type *signature,
                       core_list_t(n00bb_tree_ptr_t) *positional,
                       core_list_t(n00bb_tree_ptr_t) *keywords, bool *typed)
{
    bool fits = true;
    for (size_t i = 0; fits && i < positional->len; i++) {
        const th_param *parameter = th_func_param(signature, i);
        fits = overload_argument_fits(state, positional->data[i],
                                      parameter ? parameter->type : nullptr,
                                      i == 0, typed);
    }
    parse_sym_entry_t *declared = canonical_symbol(candidate);
    parse_tree_t *formals = declared->decl_node
        ? child_nt(grammar, child_nt(grammar, declared->decl_node,
                                     CORE_STRING_STATIC("func-type-signature")), CORE_STRING_STATIC("param-decl"))
        : nullptr;
    for (size_t k = 0; fits && k < keywords->len; k++) {
        size_t index = 0;
        const th_param *parameter = function_parameter_named(grammar,
                declared, formals, call_keyword_name(keywords->data[k]),
                &index)
            ? th_func_param(signature, index) : nullptr;
        fits = overload_argument_fits(state,
            child_nt(grammar, keywords->data[k], CORE_STRING_STATIC("expression")),
            parameter ? parameter->type : nullptr, false, typed);
    }
    return fits;
}

parse_sym_entry_t *
n00bb_select_call_overload(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                           parse_cf_label_t *call, parse_sym_entry_t *symbol,
                           bool *pending)
{
    if (pending) *pending = false;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION || !call || !symbol->scope
        || !state || !state->types)
        return symbol;
    parse_sym_entry_t *head = parse_symtab_lookup_in(symbol->scope,
                                                     symbol->name);
    if (head && head->scope == symbol->scope) symbol = head;
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
    bool typed = true;
    parse_sym_entry_t *match = nullptr, *fitting = nullptr;
    size_t matches = 0, fits = 0;
    for (parse_sym_entry_t *candidate = symbol; candidate
         && candidate->scope == symbol->scope
         && core_string_eq(candidate->name, symbol->name);
         candidate = candidate->shadowed) {
        th_type *signature = candidate->callable_contract
            ? candidate->callable_contract : candidate->type;
        signature = signature ? th_resolve(signature) : nullptr;
        if (candidate->kind != PARSE_SYM_FUNCTION || !signature
            || th_kind_of(signature) != TH_FUNCTION
            || !call_binds_candidate(grammar, candidate, signature,
                                     arguments.len, &keywords)) continue;
        matches++;
        match = candidate;
        if (overload_arguments_fit(grammar, state, candidate, signature,
                                   &arguments, &keywords, &typed)) {
            fits++;
            fitting = candidate;
        }
    }
    core_list_free(arguments);
    core_list_free(keywords);
    if (matches == 1) return match;
    if (fits == 1) return fitting;
    if (pending) *pending = matches > 1 && !typed;
    return symbol;
}

bool
n00bb_function_has_overloads(parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *head = symbol->scope
        ? parse_symtab_lookup_in(symbol->scope, symbol->name) : nullptr;
    size_t count = 0;
    for (parse_sym_entry_t *candidate = head ? head : symbol; candidate
         && candidate->scope == symbol->scope
         && core_string_eq(candidate->name, symbol->name);
         candidate = candidate->shadowed)
        count += candidate->kind == PARSE_SYM_FUNCTION;
    return count > 1;
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
        && n00bb_function_has_overloads(symbol)
        && module_member_is_call(grammar, node))
        parse_tree_set_type(node, th_var(state->types));
}

static parse_sym_entry_t *
module_call_exact_arity(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *member, parse_sym_entry_t *symbol)
{
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return symbol;
    return n00bb_select_call_overload(grammar, state,
        call_label_for_member(grammar, state, member), symbol, nullptr);
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

bool
resolve_module_member_site(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
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
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-M0011"),
                CORE_STRING_STATIC(
                    "a package cannot reference its own external namespace; use its internal module name"),
                link_source_location(node));
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
    if (module_member_is_call(grammar, node))
        symbol = module_call_exact_arity(grammar, tu_state(unit), node,
                                         symbol);
    bool package_export = core_string_eq(exposed_scope->scope_tag,
                                         CORE_STRING_STATIC(
                                             "package-interface"));
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
                || imported_symbol_is_visible(grammar, unit, imported,
                                              type));
        if (visible) {
            return parse_tree_set_resolution_symbol(node, type);
        }
        if (package_export && !state->semantic_annotation_complete)
            return n00bb_pending_type_reference(state, node);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "module '%.*s' has no visible type named '%.*s'",
            (int)imported->module.identifier.u8_bytes,
            imported->module.identifier.data,
            (int)member.u8_bytes, member.data);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0007"),
            core_buffer_string(&message),
            link_source_location(node));
        core_buffer_release(&message);
        return false;
    }

    if (symbol) {
        if (symbol->kind != PARSE_SYM_MODULE
            && !package_export
            && !imported_symbol_is_visible(grammar, unit, imported, symbol)) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "module '%.*s' has no visible %s named '%.*s'",
                (int)imported->module.identifier.u8_bytes,
                imported->module.identifier.data,
                symbol->kind == PARSE_SYM_FUNCTION ? "function" : "member",
                (int)member.u8_bytes, member.data);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0008"),
                core_buffer_string(&message),
                link_source_location(node));
            core_buffer_release(&message);
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
            && !n00bb_function_has_overloads(symbol)) {
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0005"),
                    CORE_STRING_STATIC(
                        "imported symbol type does not match the inferred member access type"),
                    link_source_location(node));
                return false;
            }
            parse_tree_set_type(node, unified);
        }
        return true;
    }
    else {
        if (package_export && !state->semantic_annotation_complete)
            return true;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "module '%.*s' has no visible %s named '%.*s'",
            (int)imported->module.identifier.u8_bytes,
            imported->module.identifier.data,
            module_member_is_call(grammar, node) ? "function" : "member",
            (int)member.u8_bytes, member.data);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0009"),
            core_buffer_string(&message),
            link_source_location(node));
        core_buffer_release(&message);
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











static parse_sym_entry_t *
implicit_builtin_export(core_translation_unit_t *unit,
                        core_string_t name, bool type_reference)
{
    core_translation_unit_t *builtin = find_direct_import(
        unit, CORE_STRING_STATIC("builtin"));
    n00bb_package_declaration_t *package = builtin
        ? find_package_declaration(builtin, CORE_STRING_STATIC("builtin"))
        : nullptr;
    n00bb_tu_state_t *state = tu_state(builtin);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!package || !package->scope || !symtab) return nullptr;
    parse_scope_t *types = parse_symtab_scope_in_namespace(
        symtab, package->scope, CORE_STRING_STATIC("type"));
    parse_sym_entry_t *type = types ? parse_symtab_lookup_in(types, name)
                                   : nullptr;
    parse_sym_entry_t *value = parse_symtab_lookup_in(package->scope, name);
    parse_sym_entry_t *found = type_reference ? type : value ? value : type;
    core_string_t consumer_package = n00bb_package_namespace(unit);
    if (!found && consumer_package.u8_bytes) {
        parse_sym_entry_t *granted = granted_export_symbol(builtin, name,
                                                           consumer_package);
        if (granted && (!type_reference
                        || granted->kind == PARSE_SYM_TYPEDEF))
            found = granted;
    }
    return found;
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

static bool
names_imported_module(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, parse_tree_parent(node), CORE_STRING_STATIC("import-stmt"));
}

static bool
resolve_prelude_value(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      n00bb_tu_state_t *state, n00bb_link_use_site_t *site)
{
    parse_sym_entry_t *builtin = parse_symtab_lookup_scoped(
        state->annotations->symtab, site->scope,
        CORE_STRING_STATIC("builtin"), site->member_name);
    site->symbol = n00bb_builtin_function(builtin) ? builtin
        : implicit_builtin_export(unit, site->member_name,
            is_type_reference_node(grammar, site->node));
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
            if (site->intrinsic_operator) continue;
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
                && !resolve_module_member_site(grammar, unit, site->node))
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
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                            CORE_STRING_STATIC(
                                "imported type reference conflicts with its inferred type"),
                            link_source_location(site->node));
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
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0013"),
                    CORE_STRING_STATIC(
                        "could not record the resolved symbol use site"),
                    link_source_location(site->node));
                return false;
            }
            else if (!site->symbol && !site->dotted_path
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
                core_string_t name = site->member_name;
                core_buffer_t message = {0};
                if (identifier_is_variant_case_expression(grammar, unit,
                                                          site->node)) {
                    core_buffer_puts(&message,
                        "variant switch cases must name variant arms");
                }
                else {
                    core_buffer_printf(&message, "unresolved name '%.*s'",
                        (int)name.u8_bytes, name.data ? name.data : "");
                }
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0014"),
                    core_buffer_string(&message),
                    link_source_location(site->node));
                core_buffer_release(&message);
                all_resolved = false;
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

core_translation_unit_t *
unit_for_module_site(core_translation_unit_t *unit,
                     n00bb_link_use_site_t *site, bool allow_suffix)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (module_path_matches_site(state, site, unit->module.identifier,
                                 allow_suffix)) return unit;
    for (size_t i = 0; state && i < state->import_aliases.len; i++)
        if (module_path_matches_site(state, site,
                state->import_aliases.data[i].name, allow_suffix))
            return state->import_aliases.data[i].unit;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        if (module_path_matches_site(state, site,
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
    payload->storage_lifetime_complete = payload->alias_analysis_complete
        && payload->lifetime.has_region
        && payload->storage_duration != PARSE_SYM_STORAGE_UNSPECIFIED;
    payload->facts_complete = payload->array_shape_complete
        && payload->initializer_facts_complete;
    return true;
}

uint32_t
effect_summary_kind(uint32_t kind)
{
    switch (kind) {
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ:
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY:
        return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY;
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE:
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY:
        return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY;
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE:
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY:
        return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY;
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS:
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY:
        return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY;
    case N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN:
        return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN;
    default:
        return 0;
    }
}

bool
restore_context_effect_dependencies(core_link_unit_t *link,
                                    uint32_t producer_stage)
{
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *source =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *payload = source
            ? core_link_declaration_payload(source) : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        const uint8_t *data = symbol
            ? parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.context-effects")) : nullptr;
        if (!data) continue;
        uint32_t count = read_u32_le(data + 4);
        size_t offset = 8;
        for (uint32_t e = 0; e < count; e++) {
            uint32_t kind = data[offset++];
            uint32_t module_size = read_u32_le(data + offset);
            offset += 4;
            uint32_t key_size = read_u32_le(data + offset);
            offset += 4;
            core_link_declaration_t *target = nullptr;
            core_link_dependency_state_t state =
                CORE_LINK_DEPENDENCY_UNAVAILABLE;
            if (kind != N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN) {
                core_string_t module = core_string_borrow(
                    (const char *)data + offset, module_size);
                core_string_t key = core_string_borrow(
                    (const char *)data + offset + module_size, key_size);
                target = core_link_unit_find_declaration(link, module, key);
                if (target) state = CORE_LINK_DEPENDENCY_RESOLVED;
            }
            offset += module_size + key_size;
            if (!core_link_unit_add_dependency(link, kind, producer_stage,
                    state, source, target, nullptr, nullptr,
                    (core_diag_location_t){0}, (core_string_t){0}, 0,
                    nullptr)) return false;
        }
    }
    return true;
}

bool
register_context_effect_metadata(n00bb_link_analysis_t *analysis,
                                 core_link_unit_t *link)
{
    th_env *environment = analysis ? analysis->interface_environment : nullptr;
    if (!environment || !link) return false;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *context =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *payload = context
            ? core_link_declaration_payload(context) : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        if (!symbol || !symbol->decl_node
            || !nt_is(analysis->grammar, symbol->decl_node, CORE_STRING_STATIC("context-def")))
            continue;
        core_buffer_t records = {0};
        uint32_t count = 0;
        bool ok = true;
        for (size_t d = 0; ok && d < core_link_unit_dependency_count(link); d++) {
            core_link_dependency_view_t effect = {0};
            if (!core_link_unit_dependency_at(link, d, &effect)) {
                ok = false;
                break;
            }
            uint32_t kind = effect_summary_kind(effect.kind);
            if (effect.from != context || !kind) continue;
            core_string_t module = effect.to
                ? core_link_declaration_module(effect.to) : (core_string_t){0};
            core_string_t key = effect.to
                ? core_link_declaration_key(effect.to) : (core_string_t){0};
            if ((effect.to && (!module.data || !key.data))
                || module.u8_bytes > UINT32_MAX
                || key.u8_bytes > UINT32_MAX || count == UINT32_MAX) {
                ok = false;
                break;
            }
            core_buffer_putc(&records, (char)kind);
            ok = append_u32_le(&records, (uint32_t)module.u8_bytes)
                && append_u32_le(&records, (uint32_t)key.u8_bytes);
            if (ok && module.u8_bytes)
                core_buffer_append(&records, module.data, module.u8_bytes);
            if (ok && key.u8_bytes)
                core_buffer_append(&records, key.data, key.u8_bytes);
            count++;
        }
        core_buffer_t metadata = {0};
        if (ok) {
            core_buffer_append(&metadata, "N0CE", 4);
            ok = append_u32_le(&metadata, count);
        }
        if (ok && records.byte_len)
            core_buffer_append(&metadata, records.data, records.byte_len);
        core_string_t module = core_link_declaration_module(context);
        core_string_t key = symbol->name;
        core_buffer_t qualified = {0};
        if (ok && module.data && key.data) {
            core_buffer_append(&qualified, module.data, module.u8_bytes);
            core_buffer_putc(&qualified, '.');
            core_buffer_append(&qualified, key.data, key.u8_bytes);
            ok = th_env_register_metadata(environment,
                TH_ENV_METADATA_VALUE, core_buffer_string(&qualified),
                CORE_STRING_STATIC("n00b.context-effects"), 1, metadata.data,
                metadata.byte_len);
        }
        else ok = false;
        core_buffer_release(&qualified);
        core_buffer_release(&metadata);
        core_buffer_release(&records);
        if (!ok) return false;
    }
    return true;
}

static bool
diag_location_precedes(core_diag_location_t left,
                       core_diag_location_t right)
{
    return left.has_location && right.has_location
        && (left.line < right.line
            || (left.line == right.line && left.column < right.column));
}

static bool static_literal_global_initializer(parse_grammar_t *grammar,
                                               parse_sym_entry_t *symbol);

void
free_initializer_order(void *opaque)
{
    n00bb_initializer_order_t *order = opaque;
    if (!order) return;
    core_free(order->units);
    core_free(order);
}

n00bb_initializer_order_t *
build_initializer_order(core_link_unit_t *link,
                         n00bb_link_analysis_t *analysis,
                         core_link_contribution_state_t *state_out)
{
    *state_out = CORE_LINK_CONTRIBUTION_COMPLETE;
    size_t unit_count = core_link_unit_translation_unit_count(link);
    n00bb_initializer_order_t *order =
        core_alloc(n00bb_initializer_order_t);
    order->units = unit_count
        ? core_alloc_array(core_translation_unit_t *, unit_count) : nullptr;
    order->count = 0;
    bool incomplete = false;
    bool valid = true;
    bool *selected = nullptr;
    size_t scc_count = core_link_unit_scc_count(link);
    core_dict_t member_positions;
    core_dict_init(&member_positions, nullptr, nullptr);
    for (size_t scc = 0; scc < scc_count; scc++) {
        size_t member_count = core_link_unit_scc_member_count(link, scc);
        for (size_t i = 0; i < member_count; i++) {
            core_translation_unit_t *member = core_link_unit_scc_member(
                link, scc, i);
            if (!member) {
                valid = false;
                goto finish;
            }
            core_dict_add(&member_positions, member, i + 1);
        }
        selected = core_alloc_array(bool, member_count);
        for (size_t step = 0; step < member_count; step++) {
            size_t choice = SIZE_MAX;
            core_link_dependency_view_t blocked_edge = {0};
            for (size_t candidate = 0; candidate < member_count; candidate++) {
                if (selected[candidate]) continue;
                bool blocked = false;
                core_translation_unit_t *candidate_unit =
                    core_link_unit_scc_member(link, scc, candidate);
                for (size_t d = 0;
                     d < core_link_unit_dependency_count(link); d++) {
                    core_link_dependency_view_t edge;
                    if (!core_link_unit_dependency_at(link, d, &edge)) {
                        valid = false;
                        goto finish;
                    }
                    if (edge.kind != N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER)
                        continue;
                    if (edge.state != CORE_LINK_DEPENDENCY_RESOLVED) {
                        incomplete = true;
                        continue;
                    }
                    core_translation_unit_t *source = edge.from
                        ? core_link_declaration_owner(edge.from)
                        : edge.source_unit;
                    core_translation_unit_t *target = edge.to
                        ? core_link_declaration_owner(edge.to) : nullptr;
                    if (source != candidate_unit || !target) continue;
                    if (!dynamic_initializer_declaration(
                            analysis->grammar, edge.to)) continue;
                    if (target == source) {
                        parse_sym_entry_t *target_symbol =
                            initializer_declaration_symbol(edge.to);
                        parse_token_info_t *target_token = target_symbol
                            ? parse_tree_find_first_terminal(
                                symbol_binding_node(target_symbol)) : nullptr;
                        core_diag_location_t target_location = target_token
                            ? (core_diag_location_t){
                                .line = parse_token_line(target_token),
                                .column = parse_token_column(target_token),
                                .has_location = true,
                            } : (core_diag_location_t){0};
                        if (diag_location_precedes(edge.source_location,
                                                   target_location)) {
                            core_diagnostics_report_at(
                                core_translation_unit_diagnostics(source),
                                source, CORE_DIAG_ERROR,
                                CORE_STRING_STATIC("N00BB-L0043"),
                                CORE_STRING_STATIC("initializer reads a global before its initializer runs"),
                                edge.source_location);
                            *state_out = CORE_LINK_CONTRIBUTION_FAILED;
                            valid = false;
                            goto finish;
                        }
                        continue;
                    }
                    size_t target_scc;
                    if (!core_link_unit_scc_of(link, target, &target_scc)
                        || target_scc != scc) continue;
                    size_t target_index = (size_t)(uintptr_t)core_dict_get(
                        &member_positions, target, nullptr);
                    if (!target_index || selected[target_index - 1])
                        continue;
                    blocked = true;
                    blocked_edge = edge;
                    break;
                }
                if (!blocked) {
                    choice = candidate;
                    break;
                }
            }
            if (choice == SIZE_MAX) {
                core_translation_unit_t *source = blocked_edge.from
                    ? core_link_declaration_owner(blocked_edge.from)
                    : blocked_edge.source_unit;
                if (source) {
                    core_buffer_t message = {0};
                    core_buffer_puts(&message,
                        "initializer dependency cycle requires reading an uninitialized global: no initialization order exists for modules ");
                    size_t listed = 0;
                    for (size_t m = 0; m < member_count; m++) {
                        if (selected[m]) continue;
                        core_string_t name = core_link_unit_scc_member(
                            link, scc, m)->module.identifier;
                        core_buffer_printf(&message, "%s'%.*s'",
                            listed++ ? ", " : "",
                            (int)name.u8_bytes, name.data);
                    }
                    core_buffer_puts(&message, " (");
                    append_initializer_declaration_name(&message,
                                                        blocked_edge.from);
                    core_buffer_puts(&message, " reads ");
                    append_initializer_declaration_name(&message,
                                                        blocked_edge.to);
                    core_buffer_puts(&message, ")");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(source), source,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0040"),
                        core_string_borrow(message.data, message.byte_len),
                        blocked_edge.source_location);
                    core_buffer_release(&message);
                }
                *state_out = CORE_LINK_CONTRIBUTION_FAILED;
                valid = false;
                goto finish;
            }
            selected[choice] = true;
            order->units[order->count++] = core_link_unit_scc_member(
                link, scc, choice);
        }
        core_free(selected);
        selected = nullptr;
    }
    if (order->count != unit_count) valid = false;
    if (incomplete) {
        *state_out = CORE_LINK_CONTRIBUTION_UNAVAILABLE;
        valid = false;
    }
finish:
    core_free(selected);
    core_dict_free(&member_positions);
    if (!valid) {
        free_initializer_order(order);
        return nullptr;
    }
    return order;
}

parse_sym_entry_t *
initializer_declaration_symbol(core_link_declaration_t *declaration)
{
    n00bb_link_payload_t *payload = declaration
        ? core_link_declaration_payload(declaration) : nullptr;
    return payload ? payload->symbol : nullptr;
}

void
append_initializer_declaration_name(core_buffer_t *out,
                                    core_link_declaration_t *declaration)
{
    core_string_t module = declaration
        ? core_link_declaration_module(declaration) : (core_string_t){0};
    parse_sym_entry_t *symbol = initializer_declaration_symbol(declaration);
    core_string_t name = symbol ? symbol->name
        : declaration ? core_link_declaration_key(declaration)
                      : CORE_STRING_STATIC("<module initializer>");
    if (module.u8_bytes)
        core_buffer_printf(out, "%.*s.", (int)module.u8_bytes, module.data);
    core_buffer_printf(out, "%.*s", (int)name.u8_bytes, name.data);
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

static uint32_t
function_global_effect_kind(parse_grammar_t *grammar,
                            parse_annot_result_t *annotations,
                            n00bb_link_use_site_t *site,
                            parse_sym_entry_t *effect_owner)
{
    for (parse_tree_t *at = site->node; at && at != effect_owner->decl_node;
         at = parse_tree_parent(at)) {
        if (!nt_is(grammar, at, CORE_STRING_STATIC("unary-expr"))) continue;
        if (core_string_eq(first_token_text(at), CORE_STRING_STATIC("ref")))
            return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS;
    }

    if (!annotations) return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ;
    for (size_t label_index = 0;
         label_index < annotations->cf_label_order.len;
         label_index++) {
        parse_cf_label_t *label =
            annotations->cf_label_order.data[label_index];
        if (!label || label->kind != PARSE_CF_ASSIGNS || !label->cond
            || !node_is_within(site->node, label->cond)
            || link_effect_owner_symbol(grammar, label->self)
                != effect_owner) continue;
        return nt_is(grammar, label->self, CORE_STRING_STATIC("binop-assign-stmt"))
            ? N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
            : N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE;
    }
    return N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ;
}

bool
publish_function_global_effects(core_link_unit_t *link,
                                n00bb_link_analysis_t *analysis,
                                uint32_t producer_stage,
                                core_link_contribution_state_t *state_out)
{
    bool complete = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state) return false;
        for (size_t s = 0; s < state->link_use_sites.len; s++) {
            n00bb_link_use_site_t *site = &state->link_use_sites.data[s];
            parse_sym_entry_t *effect_owner = link_effect_owner_symbol(
                analysis->grammar, site->node);
            if (!effect_owner || !is_module_global_symbol(
                    analysis->grammar, site->symbol)) continue;
            core_link_declaration_t *source = link_declaration_for_symbol(
                analysis, effect_owner);
            core_link_declaration_t *target = link_declaration_for_symbol(
                analysis, site->symbol);
            uint32_t kind = function_global_effect_kind(
                analysis->grammar, state->annotations, site, effect_owner);
            bool resolved = source && target
                && kind != N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN;
            if (!resolved) complete = false;
            if (!core_link_unit_add_dependency(link, kind, producer_stage,
                    resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                             : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    source, target, unit, site->node,
                    link_source_location(site->node),
                    core_source_file_path(&unit->source), 1, nullptr))
                return false;
        }
    }
    *state_out = complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                          : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}


parse_sym_entry_t *
unit_trait_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                core_string_t name, bool imported)
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
    if (imported && !symbol_is_public_declaration(symbol))
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
        if (item->subpackage || !core_string_eq(item->export_name, name))
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
            ? unit_trait_symbol(grammar, origin, source_name, true) : nullptr;
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
                    symbol = unit_trait_symbol(grammar, target, trait, true);
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
            symbol = unit_trait_symbol(grammar, target, trait,
                                       target != unit);
        if (!symbol) target = nullptr;
    }
    else {
        symbol = unit_trait_symbol(grammar, unit, trait, false);
        if (symbol) {
            target = unit;
        }
        else {
            for (size_t i = 0; i < core_translation_unit_import_count(unit);
                 i++) {
                core_translation_unit_t *imported =
                    core_translation_unit_import(unit, i);
                parse_sym_entry_t *candidate = unit_trait_symbol(
                    grammar, imported, trait, true);
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
    if (declaration_registered && !registered) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "could not register link-unit declaration '%.*s'",
            (int)key.byte_len, key.data);
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0002"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
    }
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
        CORE_STRING_STATIC("has_type"), CORE_STRING_STATIC("ref_read"),
        CORE_STRING_STATIC("ref_write"),
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
                core_string_t module = core_link_reference_module(reference);
                core_string_t key = core_link_reference_key(reference);
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "resolved LinkUnit reference '%.*s:%.*s' has no frontend declaration payload",
                    (int)module.u8_bytes, module.data,
                    (int)key.u8_bytes, key.data);
                core_diagnostics_report(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0005"),
                    core_string_borrow(message.data, message.byte_len));
                core_buffer_release(&message);
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
        || !connect_package_subpackages(analysis)
        || !bind_imported_module_scopes(&analysis->units)
        || !populate_package_interfaces(analysis)
        || !resolve_imported_module_symbols(analysis->grammar,
                                        &analysis->units))
        return CORE_LINK_STAGE_RESULT_FAILED;
    if (!register_link_declarations(link, analysis->grammar,
                                    &analysis->units,
                                    &analysis->declarations_by_symbol)) {
        if (!core_link_unit_has_fatal_diagnostics(link))
            core_diagnostics_report(core_translation_unit_diagnostics(
                    analysis->units.data[0]), analysis->units.data[0],
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0003"),
                CORE_STRING_STATIC("could not index module declarations in the link unit"));
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!register_link_use_sites(link, analysis->grammar,
                                 &analysis->units)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
                analysis->units.data[0]), analysis->units.data[0],
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0004"),
            CORE_STRING_STATIC("could not register resolved source references in the link unit"));
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

static parse_sym_liveness_t
parse_liveness_state(ir_liveness_t state)
{
    switch (state) {
    case IR_LIVENESS_DEAD: return PARSE_SYM_LIVENESS_DEAD;
    case IR_LIVENESS_LIVE: return PARSE_SYM_LIVENESS_LIVE;
    case IR_LIVENESS_UNKNOWN: return PARSE_SYM_LIVENESS_UNKNOWN;
    }
    return PARSE_SYM_LIVENESS_UNKNOWN;
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

typedef struct {
    parse_sym_entry_t *symbol;
    parse_sym_phase_t phase;
    n00bb_link_payload_t *facts;
} n00bb_lifetime_endpoint_context_t;

static bool
record_symbol_lifetime_endpoint(void *opaque,
                                ir_lifetime_endpoint_t endpoint)
{
    n00bb_lifetime_endpoint_context_t *context = opaque;
    if (!context || !context->symbol || !context->facts) return false;
    parse_sym_lifetime_end_t end = {
        .phase = context->phase,
        .function = endpoint.function,
        .binding = endpoint.binding,
        .point = endpoint.point,
        .from = endpoint.from,
        .to = endpoint.to,
        .on_edge = endpoint.from != nullptr,
    };
    if (!parse_sym_set_lifetime_end(context->symbol, end)) return false;
    if (end.on_edge) context->facts->cfg_liveness_end_edges++;
    else context->facts->cfg_liveness_end_points++;
    return true;
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
    ir_liveness_query_t *query = ir_liveness_query_new(value);
    if (!query) {
        facts->cfg_liveness_complete = false;
        return false;
    }
    n00bb_lifetime_endpoint_context_t endpoint_context = {
        .symbol = symbol,
        .phase = phase,
        .facts = facts,
    };
    if (!ir_liveness_query_visit_endpoints(
            query, record_symbol_lifetime_endpoint, &endpoint_context))
        complete = false;
    size_t edge_count = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        if (!terminator) {
            complete = false;
            facts->cfg_liveness_incomplete_blocks++;
            continue;
        }
        uint32_t successors = ir_succ_count(terminator);
        ir_liveness_t after_terminator =
            ir_liveness_query_alias_after(query, terminator);
        for (uint32_t s = 0; s < successors; s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (!target) {
                complete = false;
                continue;
            }
            parse_sym_lifetime_edge_t edge = {
                .function = function,
                .binding = value,
                .from = block,
                .to = target,
                .phase = phase,
                .direct = parse_liveness_state(
                    ir_liveness_query_direct_on_edge(query, block, target)),
                .aliases = parse_liveness_state(
                    ir_liveness_query_alias_on_edge(query, block, target)),
            };
            edge.ends_live_range = after_terminator == IR_LIVENESS_LIVE
                && edge.aliases == PARSE_SYM_LIVENESS_DEAD;
            if (!parse_sym_set_lifetime_edge(symbol, edge)) {
                complete = false;
                continue;
            }
            if (edge.direct == PARSE_SYM_LIVENESS_LIVE)
                facts->cfg_liveness_direct_live_edges++;
            else if (edge.direct == PARSE_SYM_LIVENESS_DEAD)
                facts->cfg_liveness_direct_dead_edges++;
            else
                facts->cfg_liveness_direct_unknown_edges++;
            if (edge.aliases == PARSE_SYM_LIVENESS_LIVE)
                facts->cfg_liveness_alias_live_edges++;
            else if (edge.aliases == PARSE_SYM_LIVENESS_DEAD)
                facts->cfg_liveness_alias_dead_edges++;
            else
                facts->cfg_liveness_alias_unknown_edges++;
            facts->cfg_liveness_range_ends += edge.ends_live_range;
            edge_count++;
        }
        if (successors == 0) {
            parse_sym_lifetime_edge_t edge = {
                .function = function,
                .binding = value,
                .from = block,
                .phase = phase,
                .direct = parse_liveness_state(
                    ir_liveness_query_direct_after(query, terminator)),
                .aliases = parse_liveness_state(
                    ir_liveness_query_alias_after(query, terminator)),
                .is_exit = true,
            };
            edge.ends_live_range =
                edge.aliases == PARSE_SYM_LIVENESS_DEAD;
            if (!parse_sym_set_lifetime_edge(symbol, edge)) {
                complete = false;
                continue;
            }
            if (edge.direct == PARSE_SYM_LIVENESS_LIVE)
                facts->cfg_liveness_direct_live_edges++;
            else if (edge.direct == PARSE_SYM_LIVENESS_DEAD)
                facts->cfg_liveness_direct_dead_edges++;
            else
                facts->cfg_liveness_direct_unknown_edges++;
            if (edge.aliases == PARSE_SYM_LIVENESS_LIVE)
                facts->cfg_liveness_alias_live_edges++;
            else if (edge.aliases == PARSE_SYM_LIVENESS_DEAD)
                facts->cfg_liveness_alias_dead_edges++;
            else
                facts->cfg_liveness_alias_unknown_edges++;
            facts->cfg_liveness_range_ends += edge.ends_live_range;
            edge_count++;
        }
    }
    facts->cfg_liveness_edges += edge_count;
    facts->cfg_liveness_complete &= complete;
    ir_liveness_query_free(query);
    bool range_complete = complete && edge_count != 0;
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

static bool
cleanup_fact_add(n00bb_link_payload_t *owner_facts,
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
    size_t observed_edges = 0;
    size_t observed_endpoints = 0;
    if (range) {
        for (size_t i = 0; i < symbol->lifetime.cfg_edge_count; i++) {
            const parse_sym_lifetime_edge_t *edge =
                &symbol->lifetime.cfg_edges[i];
            if (edge->phase != phase || edge->function != function
                || edge->binding != binding) continue;
            observed_edges++;
            if (edge->is_exit) {
                fact.exit_edges++;
                if (edge->direct == PARSE_SYM_LIVENESS_DEAD
                    && edge->aliases == PARSE_SYM_LIVENESS_DEAD)
                    fact.safe_exit_edges++;
                else if (edge->direct == PARSE_SYM_LIVENESS_UNKNOWN
                         || edge->aliases == PARSE_SYM_LIVENESS_UNKNOWN)
                    fact.unknown_exit_edges++;
                else {
                    fact.live_exit_edges++;
                    complete = false;
                }
            }
            if (edge->direct == PARSE_SYM_LIVENESS_UNKNOWN
                || edge->aliases == PARSE_SYM_LIVENESS_UNKNOWN)
                complete = false;
        }
        for (size_t i = 0; i < symbol->lifetime.end_count; i++) {
            const parse_sym_lifetime_end_t *end =
                &symbol->lifetime.ends[i];
            if (end->phase != phase || end->function != function
                || end->binding != binding) continue;
            observed_endpoints++;
            fact.safe_endpoints++;
            if (end->on_edge) fact.edge_endpoints++;
            else fact.point_endpoints++;
        }
        fact.cfg_edges = observed_edges;
        complete &= observed_edges == range->edge_count
            && observed_endpoints == range->end_count
            && symbol->alias_analysis_complete
            && !symbol->lifetime.escapes;
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
                    bool added = cleanup_fact_add(facts, symbol, nullptr,
                        nullptr, PARSE_SYM_PHASE_RUNTIME, nullptr);
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
                        && cleanup_fact_add(owner_facts, symbol, function,
                            value, PARSE_SYM_PHASE_RUNTIME, range);
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
            return range->complete && range->start_count != 0
                && range->edge_count != 0;
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

    core_translation_unit_t *unit = unit_for_symbol(analysis->grammar,
        &analysis->units, symbol);
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    if (!scope_has_lexical_lifetime(symbol->scope)
        || (type_may_contain_reference_alias(state ? state->types : nullptr,
                                              symbol->type)
            && !symbol->alias_analysis_complete)) return false;
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
    core_free((void *)analysis->linked_interfaces);
    th_env_free(analysis->export_environment);
    th_env_free(analysis->interface_environment);
    for (size_t i = 0; i < analysis->trait_specializations.len; i++)
        core_free(analysis->trait_specializations.data[i]);
    core_list_free(analysis->trait_specializations);
    core_list_free(analysis->reachable_functions);
    core_list_free(analysis->emitted_functions);
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
            symbol_is_public_declaration(symbol) ? TH_ENV_PUBLIC
                                                 : TH_ENV_PRIVATE);
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
                bool scalar = core_string_eq(symbol->adt_kind,
                    CORE_STRING_STATIC("enum"))
                    || core_string_eq(symbol->adt_kind,
                        CORE_STRING_STATIC("flagset"))
                    || (symbol->member_scope
                        && core_string_eq(
                            symbol->member_scope->scope_tag,
                            CORE_STRING_STATIC("flagset")));
                if (!record && !scalar) continue;
                if (n00bb_flagset_type(unit, state, receiver, nullptr)
                    && !register_flagset_operators(defaults, state->types,
                        unit, symbol, receiver)) {
                    th_ctx_free(probe);
                    return false;
                }
                th_type *shape = th_type_definition(state->types, receiver);
                if (!th_is_closed(state->types, receiver)
                    || n00bb_value_coercion_target(state->types, receiver)
                    || (record && (!shape
                        || th_kind_of(th_resolve(shape)) != TH_RECORD
                        || !th_is_closed(state->types, shape)))) continue;
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
                bool derive_equal = record && !custom_equal
                    && record_default_fields_support(probe, state->types,
                        receiver, CORE_STRING_STATIC("Equal"),
                        CORE_STRING_STATIC("__equal"), &visiting);
                bool derive_hash = hash_available && !custom_equal
                    && !custom_hash
                    && (scalar || record_default_fields_support(probe,
                        state->types, receiver,
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
                    generated->visibility = CORE_STRING_STATIC("public");
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

