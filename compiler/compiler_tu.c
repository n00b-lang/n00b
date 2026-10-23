#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_axioms.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "driver.h"
#include "signature_typevars.h"
#include "storage_selection.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "record_layout_request.h"
#include "field_extent.h"
#include "preprocessor.h"
#include "parse/tree_util.h"

#define NT_IS(g, n, lit) nt_is((g), (n), CORE_STRING_STATIC(lit))
#define CHILD_NT(g, n, lit) child_nt((g), (n), CORE_STRING_STATIC(lit))

core_list_decl(th_kwparam);
core_list_decl(th_member);
core_list_decl(size_t);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

typedef struct {
    th_type *slot;
    core_list_t(th_type_ptr_t) items;
    core_list_t(parse_tree_ptr_t) nodes;
} literal_item_group_t;

static bool unify_literal_items(th_ctx *types, literal_item_group_t *group,
                                size_t *conflict);
static void report_literal_item_conflict(core_translation_unit_t *unit,
                                         n00bb_tu_state_t *state,
                                         parse_tree_t *node,
                                         literal_item_group_t *group,
                                         size_t conflict);
static bool n00bb_contextual_method_argument(parse_grammar_t *grammar,
                                             parse_tree_t *expression);

bool
n00bb_assignment_target(parse_grammar_t *grammar, parse_tree_t *node,
                        bool *compound)
{
    for (parse_tree_t *parent = parse_tree_parent(node);
         parent; parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "index-contents")
            || NT_IS(grammar, parent, "slice-contents")) return false;
        bool is_compound = NT_IS(grammar, parent, "binop-assign-stmt");
        if (!is_compound && !NT_IS(grammar, parent, "assign-stmt"))
            continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        parse_annotation_t *assign = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
            : nullptr;
        parse_tree_t *target = assign
            ? parse_tree_resolve_child_ref(grammar, parent,
                                           assign->name_ref) : nullptr;
        if (target && node_is_within(node, target)) {
            if (compound) *compound = is_compound;
            return true;
        }
    }
    return false;
}

bool
n00bb_is_trait_dispatch_reference(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !node) return false;
    for (parse_tree_t *parent = parse_tree_parent(node);
         parent; parent = parse_tree_parent(parent)) {
        parse_cf_label_t *label = parse_cf_label_lookup(
            state->annotations, parent);
        if (!label || label->kind != PARSE_CF_CALL || !label->cond
            || member_access_node(grammar, label->cond) != node) continue;
        parse_tree_t *receiver_node = core_tree_num_children(node)
            ? core_tree_child(node, 0) : nullptr;
        return !call_receiver_is_module(grammar, unit, receiver_node);
    }
    return false;
}

bool
n00bb_call_is_error_propagated(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *current = node;
    for (;;) {
        parse_tree_t *parent = parse_tree_parent(current);
        if (!parent || core_tree_is_leaf(parent)
            || !core_tree_num_children(parent)
            || core_tree_child(parent, 0) != current) return false;
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        parse_annotation_t *op = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_OPERATOR)
            : nullptr;
        if (op)
            return core_string_eq(op->op_kind,
                                  CORE_STRING_STATIC("unwrap_result"));
        if (core_tree_num_children(parent) != 1) return false;
        current = parent;
    }
}

parse_sym_entry_t *
n00bb_try_error_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *current = node;
    for (parse_tree_t *at = node ? parse_tree_parent(node) : nullptr;
         at; current = at, at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "func-def")
            || NT_IS(grammar, at, "context-def")) return nullptr;
        bool statement = NT_IS(grammar, at, "try-stmt");
        if (!statement && !NT_IS(grammar, at, "try-value-expr")) continue;
        if (child_nt(grammar, at, statement ? CORE_STRING_STATIC("try-body")
                                            : CORE_STRING_STATIC("value-body"))
            != current) continue;
        return n00bb_try_clause_symbol(grammar, child_nt(grammar, at,
            statement ? CORE_STRING_STATIC("try-error-clause")
                      : CORE_STRING_STATIC("try-value-error-clause")));
    }
    return nullptr;
}

parse_sym_entry_t *
n00bb_try_clause_symbol(parse_grammar_t *grammar, parse_tree_t *clause)
{
    parse_tree_t *binding = CHILD_NT(grammar, clause, "try-error-binding");
    return binding ? parse_tree_sym(binding) : nullptr;
}

bool
bind_resolved_reference_alias(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *use_site)
{
    parse_tree_t *reference = nullptr;
    for (parse_tree_t *at = use_site; at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "unary-expr")
            && core_string_eq(first_token_text(at),
                              CORE_STRING_STATIC("ref"))) {
            reference = at;
            break;
        }
        if (NT_IS(grammar, at, "func-def")
            || NT_IS(grammar, at, "context-def")) return true;
    }
    if (!reference) return true;

    parse_tree_t *binding = nullptr;
    for (parse_tree_t *at = parse_tree_parent(reference); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "sym-decl")
            || NT_IS(grammar, at, "assign-stmt")
            || NT_IS(grammar, at, "binop-assign-stmt")) {
            binding = at;
            break;
        }
        if (NT_IS(grammar, at, "func-def")
            || NT_IS(grammar, at, "context-def")) return true;
    }
    if (!binding) return true;

    const parse_rule_t *rule = parse_tree_rule(grammar, binding);
    parse_annotation_t *assign = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS) : nullptr;
    if (!assign) return true;
    parse_tree_t *value = parse_tree_resolve_child_ref(grammar, binding,
                                                       assign->value_ref);
    if (!value || direct_ref_expression(grammar, value) != reference)
        return true;

    parse_sym_entry_t *holder = nullptr;
    if (NT_IS(grammar, binding, "sym-decl")) {
        parse_node_resolution_t resolution = parse_tree_resolution(binding);
        if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
            holder = resolution.target.symbol;
    }
    else {
        parse_tree_t *target = parse_tree_resolve_child_ref(grammar, binding,
                                                            assign->name_ref);
        parse_tree_t *lvalue = n00bb_simple_lvalue(target);
        holder = lvalue ? attached_symbol(grammar, unit, lvalue) : nullptr;
    }
    parse_sym_entry_t *referent = direct_ref_target(grammar, reference);
    if (!holder || !referent || holder == referent
        || (holder->kind != PARSE_SYM_VARIABLE
            && holder->kind != PARSE_SYM_PARAM)) return true;

    parse_token_info_t *site = parse_tree_find_first_terminal(value);
    uint64_t derivation = site ? parse_token_start_byte(site) : 0;
    bool outlives_target = scope_has_lexical_lifetime(referent->scope)
        && !scope_is_within(holder->scope, referent->scope);
    return parse_symtab_add_alias(holder, referent,
        PARSE_SYM_ALIAS_DERIVED, derivation, outlives_target);
}

parse_tree_t *
direct_ref_expression(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (NT_IS(grammar, node, "unary-expr"))
            return core_string_eq(first_token_text(node),
                                  CORE_STRING_STATIC("ref")) ? node : nullptr;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

bool
identifier_is_variant_case_expression(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *item = node;
    while (item && !NT_IS(grammar, item, "case-expr-item"))
        item = parse_tree_parent(item);
    parse_tree_t *expression = item
        ? CHILD_NT(grammar, item, "expression") : nullptr;
    if (!expression || !node_is_within(node, expression)) return false;
    parse_tree_t *switch_node = parse_tree_parent(item);
    while (switch_node
           && !NT_IS(grammar, switch_node, "switch-stmt")
           && !NT_IS(grammar, switch_node, "switch-value-expr"))
        switch_node = parse_tree_parent(switch_node);
    parse_cf_label_t *label = state && switch_node
        ? parse_cf_label_lookup(state->annotations, switch_node) : nullptr;
    parse_tree_t *subject = label && label->cond
        ? n00bb_simple_lvalue(label->cond) : nullptr;
    th_type *subject_type = label ? parse_tree_type(label->cond) : nullptr;
    if (!subject_type && subject) {
        parse_sym_entry_t *symbol = attached_symbol(grammar, unit, subject);
        subject_type = symbol ? symbol->type : nullptr;
    }
    return n00bb_variant_scope_for_type(unit, state, subject_type) != nullptr;
}

bool
reference_is_simple_assignment_target(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node,
                                     parse_sym_entry_t *symbol)
{
    if (!symbol || (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM)) return false;
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (!NT_IS(grammar, at, "assign-stmt")) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        parse_annotation_t *assign = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
            : nullptr;
        parse_tree_t *target = assign
            ? parse_tree_resolve_child_ref(grammar, at, assign->name_ref)
            : nullptr;
        if (!target || !node_is_within(node, target)) continue;
        parse_tree_t *lvalue = n00bb_simple_lvalue(target);
        return lvalue && attached_symbol(grammar, unit, lvalue) == symbol;
    }
    return false;
}

core_list_t(parse_sym_entry_ptr_t)
variant_arms_in_source_order(parse_grammar_t *grammar, parse_scope_t *scope)
{
    core_list_t(parse_sym_entry_ptr_t) ordered =
        core_list_new(parse_sym_entry_ptr_t);
    core_list_t(int32_t) starts = core_list_new(int32_t);
    for (parse_sym_entry_t *arm = scope ? scope->first_in_scope : nullptr;
         arm; arm = arm->next_in_scope) {
        if (!n00bb_variant_arm_symbol(grammar, arm)) continue;
        int32_t start = arm->decl_node
            ? core_tree_node_value(arm->decl_node).start : INT32_MAX;
        size_t at = ordered.len;
        while (at && starts.data[at - 1] > start) at--;
        core_list_insert(ordered, at, arm);
        core_list_insert(starts, at, start);
    }
    core_list_free(starts);
    return ordered;
}

bool
resolve_local_enum_constant(parse_grammar_t *grammar,
                            n00bb_tu_state_t *state,
                            n00bb_link_use_site_t *site)
{
    if (!state || !state->annotations || !site || site->dotted_path
        || !site->node || !site->member_name.data) return false;
    parse_tree_t *receiver = site->node;
    parse_tree_t *parent = parse_tree_parent(receiver);
    while (parent && !dotted_symbol_path_node(grammar, parent)
           && !core_tree_is_leaf(parent)
           && core_tree_num_children(parent) == 1
           && core_tree_child(parent, 0) == receiver) {
        receiver = parent;
        parent = parse_tree_parent(parent);
    }
    if (!dotted_symbol_path_node(grammar, parent)
        || core_tree_child(parent, 0) != receiver) return false;
    parse_sym_entry_t *type = parse_symtab_lookup_scoped(
        state->annotations->symtab, site->scope,
        CORE_STRING_STATIC("type"), site->member_name);
    if (!type || !type->member_scope
        || (!core_string_eq(type->member_scope->scope_tag,
                            CORE_STRING_STATIC("enum"))
            && !core_string_eq(type->member_scope->scope_tag,
                               CORE_STRING_STATIC("flagset")))) return false;
    core_string_t name = first_token_text(core_tree_child(parent, 2));
    parse_sym_entry_t *item = parse_symtab_lookup_in(type->member_scope, name);
    if (!item || item->kind != PARSE_SYM_ENUM_CONST || !item->type
        || !parse_tree_set_resolution_symbol(site->node, type)
        || !parse_tree_set_resolution_symbol(parent, item)) return false;
    site->symbol = type;
    parse_tree_set_type(site->node, type->type);
    (void)n00bb_unify_node_type(state->types, parent, item->type);
    return true;
}

parse_cf_label_t *
call_label_for_member(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                     parse_tree_t *member)
{
    if (!grammar || !state || !state->annotations || !member) return nullptr;
    for (parse_tree_t *node = member; node;
         node = parse_tree_parent(node)) {
        parse_cf_label_t *label = parse_cf_label_lookup(state->annotations,
                                                        node);
        if (label && label->kind == PARSE_CF_CALL && label->cond
            && member_access_node(grammar, label->cond) == member)
            return label;
    }
    return nullptr;
}

parse_scope_t *
n00bb_variant_scope_for_type(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, th_type *type)
{
    if (!state || !state->annotations || !state->annotations->symtab || !type)
        return nullptr;
    parse_sym_entry_t *type_symbol = type_symbol_in_loaded_imports(
        unit, state->types, type, nullptr);
    return type_symbol && type_symbol->member_scope
        && core_string_eq(type_symbol->member_scope->scope_tag,
                          CORE_STRING_STATIC("variant"))
        ? type_symbol->member_scope : nullptr;
}

bool
n00bb_variant_arm_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    if (symbol->decl_node)
        return NT_IS(grammar, symbol->decl_node, "variant-decl-one");
    return symbol->kind == PARSE_SYM_TAG && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("variant"));
}

th_type *
n00bb_variant_arm_payload_type(th_ctx *types, th_type *variant,
                               parse_sym_entry_t *arm)
{
    if (!types || !variant || !arm) return nullptr;
    variant = th_resolve(variant);
    while (variant && th_kind_of(variant) == TH_INDIRECTION)
        variant = th_resolve(th_ind_referent(variant));
    return variant ? th_record_lookup_in(types, variant, arm->name)
                   : nullptr;
}

static bool
member_step(parse_grammar_t *grammar, parse_tree_t *node)
{
    return NT_IS(grammar, node, "postfix-expr")
        && core_tree_num_children(node) == 3
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC("."));
}

static parse_tree_t *
access_path_step(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node) && !member_step(grammar, node)
           && parse_tree_resolution(node).kind
               != PARSE_NODE_RESOLUTION_SYMBOL
           && core_tree_num_children(node) == 1
           && !core_tree_is_leaf(core_tree_child(node, 0)))
        node = core_tree_child(node, 0);
    return node;
}

static bool
same_access_path(parse_grammar_t *grammar, core_translation_unit_t *unit,
                 parse_tree_t *left, parse_tree_t *right)
{
    left = access_path_step(grammar, left);
    right = access_path_step(grammar, right);
    if (!left || !right) return false;
    bool left_member = member_step(grammar, left);
    if (left_member != member_step(grammar, right)) return false;
    if (left_member)
        return core_string_eq(first_token_text(core_tree_child(left, 2)),
                              first_token_text(core_tree_child(right, 2)))
            && same_access_path(grammar, unit, core_tree_child(left, 0),
                                core_tree_child(right, 0));
    parse_sym_entry_t *symbol = attached_symbol(grammar, unit, left);
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM)
        && symbol == attached_symbol(grammar, unit, right);
}

parse_sym_entry_t *
variant_payload_arm(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("."))) return nullptr;
    parse_node_resolution_t existing = parse_tree_resolution(node);
    if (existing.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && n00bb_variant_arm_symbol(grammar, existing.target.symbol))
        return existing.target.symbol;
    parse_tree_t *receiver_node = core_tree_child(node, 0);
    parse_tree_t *receiver_step = access_path_step(grammar, receiver_node);
    parse_sym_entry_t *receiver = receiver_step
        && !member_step(grammar, receiver_step)
        ? attached_symbol(grammar, unit, receiver_node) : nullptr;
    th_type *receiver_type = receiver ? receiver->type
        : receiver_step ? parse_tree_type(receiver_step) : nullptr;
    if (!receiver_type) return nullptr;
    core_string_t member = first_token_text(core_tree_child(node, 2));
    parse_tree_t *case_block = parse_tree_parent(node);
    while (case_block && !NT_IS(grammar, case_block, "switch-case-block"))
        case_block = parse_tree_parent(case_block);
    if (!case_block) return nullptr;
    parse_tree_t *switch_node = parse_tree_parent(case_block);
    while (switch_node && !NT_IS(grammar, switch_node, "switch-stmt"))
        switch_node = parse_tree_parent(switch_node);
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *switch_expr = switch_node
        ? CHILD_NT(grammar, switch_node, "expression") : nullptr;
    if (!same_access_path(grammar, unit, switch_expr, receiver_node))
        return nullptr;
    parse_scope_t *variant_scope = n00bb_variant_scope_for_type(
        unit, state, receiver_type);
    parse_tree_t *case_list = CHILD_NT(grammar, case_block,
                                       "case-expr-list");
    parse_tree_t *item = CHILD_NT(grammar, case_list, "case-expr-item");
    parse_tree_t *pattern = item
        ? CHILD_NT(grammar, item, "expression") : nullptr;
    parse_tree_t *pattern_ref = simple_identifier_primary(grammar, pattern);
    n00bb_link_use_site_t *pattern_site = pattern_ref
        ? link_use_site_for_node(state, pattern_ref) : nullptr;
    core_string_t pattern_name = pattern_site
        ? pattern_site->member_name : (core_string_t){0};
    parse_node_resolution_t pattern_resolution = pattern_ref
        ? parse_tree_resolution(pattern_ref)
        : (parse_node_resolution_t){0};
    parse_sym_entry_t *arm = pattern_resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? pattern_resolution.target.symbol
        : pattern_site ? pattern_site->symbol : nullptr;
    if (!n00bb_variant_arm_symbol(grammar, arm) && variant_scope
        && pattern_name.data)
        arm = parse_symtab_lookup_in(variant_scope, pattern_name);
    if (!variant_scope || !n00bb_variant_arm_symbol(grammar, arm)
        || !core_string_eq(arm->name, member)) return nullptr;
    if (!n00bb_variant_arm_symbol(grammar, arm)
        || !parse_tree_set_resolution_symbol(node, arm)
        || !parse_sym_add_use(arm, node)) return nullptr;
    return arm;
}

parse_sym_entry_t *
variant_assignment_arm(parse_grammar_t *grammar,
                      core_translation_unit_t *unit, parse_tree_t *node)
{
    if (!unit || !NT_IS(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("."))) return nullptr;

    bool assignment_target = false;
    parse_tree_t *child = node;
    for (parse_tree_t *at = parse_tree_parent(node); at;
         child = at, at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "assign-stmt")) {
            parse_tree_t *target = core_tree_num_children(at)
                ? core_tree_child(at, 0) : nullptr;
            assignment_target = target == child
                && core_string_eq(first_token_text(core_tree_child(at, 1)),
                                  CORE_STRING_STATIC("="));
            break;
        }
        if (NT_IS(grammar, at, "postfix-expr")
            && core_tree_num_children(at)
            && core_tree_child(at, 0) == child) break;
        if (core_tree_num_children(at) != 1) break;
    }
    if (!assignment_target) return nullptr;

    parse_tree_t *receiver_node = core_tree_child(node, 0);
    th_type *receiver_type = parse_tree_type(receiver_node);
    parse_sym_entry_t *receiver_symbol = attached_symbol(grammar, unit,
                                                          receiver_node);
    if (!receiver_type && receiver_symbol) receiver_type = receiver_symbol->type;
    receiver_type = receiver_type ? th_resolve(receiver_type) : nullptr;
    while (receiver_type && th_kind_of(receiver_type) == TH_INDIRECTION)
        receiver_type = th_ind_referent(receiver_type);
    bool is_variant = false;
    if (!receiver_type || th_kind_of(receiver_type) != TH_NAMED
        || !th_named_info(receiver_type, nullptr, &is_variant, nullptr)
        || !is_variant) return nullptr;

    n00bb_tu_state_t *state = tu_state(unit);
    core_translation_unit_t *owner = nullptr;
    parse_sym_entry_t *owner_type = type_symbol_in_loaded_imports(
        unit, state ? state->types : nullptr, receiver_type, &owner);
    parse_scope_t *scope = owner_type ? owner_type->member_scope : nullptr;
    core_string_t member = first_token_text(core_tree_child(node, 2));
    parse_sym_entry_t *arm = scope ? parse_symtab_lookup_in(scope, member)
                                   : nullptr;
    if (!n00bb_variant_arm_symbol(grammar, arm) || !arm->type) return nullptr;

    if (!owner_type) owner_type = arm->owner_type;
    if (!field_access_is_visible(unit, owner, owner_type, arm)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0021"),
            CORE_STRING_STATIC("variant arm is not visible from this module"),
            link_source_location(node));
        return nullptr;
    }

    parse_node_resolution_t existing = parse_tree_resolution(node);
    if (existing.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && existing.target.symbol != arm)
        parse_sym_remove_use(existing.target.symbol, node);
    if (!parse_tree_set_resolution_symbol(node, arm)
        || !parse_sym_add_use(arm, node)) return nullptr;
    th_type *payload_type = n00bb_variant_arm_payload_type(state->types,
                                                            receiver_type, arm);
    if (!payload_type) return nullptr;
    parse_tree_set_type(node, payload_type);
    n00bb_link_use_site_t *site = link_use_site_for_node(tu_state(unit), node);
    if (site) site->symbol = arm;
    return arm;
}

bool
prepare_variant_payload_type(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node)
{
    parse_sym_entry_t *arm = variant_payload_arm(grammar, unit, node);
    if (!arm || !arm->type) return false;
    parse_tree_t *receiver_node = core_tree_child(node, 0);
    th_type *receiver_type = parse_tree_type(receiver_node);
    parse_sym_entry_t *receiver = attached_symbol(grammar, unit, receiver_node);
    if (!receiver_type && receiver) receiver_type = receiver->type;
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *payload_type = n00bb_variant_arm_payload_type(state->types,
                                                            receiver_type, arm);
    if (!payload_type) return false;
    parse_tree_set_type(node, payload_type);
    return true;
}

bool
n00bb_unify_node_type(th_ctx *types, parse_tree_t *node, th_type *type)
{
    if (!node || core_tree_is_leaf(node) || !type) return false;
    th_type *previous = parse_tree_type(node);
    th_type *resolved = previous && types
        ? th_unify_in_place(types, previous, type) : type;
    if (!resolved) return false;
    parse_tree_set_type(node, resolved);
    return true;
}

bool
n00bb_receiver_names_type(parse_grammar_t *grammar,
                          core_translation_unit_t *unit,
                          n00bb_tu_state_t *state, parse_tree_t *receiver,
                          parse_sym_entry_t *type_symbol)
{
    if (!receiver || !type_symbol) return false;
    parse_node_resolution_t resolution = parse_tree_resolution(receiver);
    n00bb_link_use_site_t *site = state
        ? link_use_site_for_node(state, receiver) : nullptr;
    return (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
            && resolution.target.symbol == type_symbol)
        || (site && site->symbol == type_symbol)
        || (!core_tree_is_leaf(receiver)
            && core_string_eq(parse_tree_unresolved_name(receiver),
                              type_symbol->name))
        || attached_symbol(grammar, unit, receiver) == type_symbol;
}

bool
check_local_record_field_reference(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("."))
        || n00bb_is_trait_dispatch_reference(grammar, unit, node)) return false;
    n00bb_tu_state_t *state = tu_state(unit);
    if (variant_payload_arm(grammar, unit, node)
        || variant_assignment_arm(grammar, unit, node)) return false;
    parse_tree_t *receiver_node = core_tree_child(node, 0);
    th_type *receiver_type = parse_tree_type(receiver_node);
    th_type *resolved = receiver_type ? th_resolve(receiver_type) : nullptr;
    if (!resolved || th_kind_of(resolved) == TH_VAR) {
        parse_sym_entry_t *receiver = attached_symbol(grammar, unit,
                                                       receiver_node);
        if (receiver && receiver->type) receiver_type = receiver->type;
    }
    receiver_type = state ? n00bb_dereferenced_type(state->types, receiver_type)
                          : nullptr;
    if (!state || !state->types || !state->annotations || !receiver_type
        || th_kind_of(receiver_type) != TH_NAMED) return false;

    parse_sym_entry_t *owner_type = nullptr;
    core_translation_unit_t *owner_unit = nullptr;
    owner_type = type_symbol_in_loaded_imports(
        unit, state->types, receiver_type, &owner_unit);
    for (size_t depth = 0; owner_type && owner_type->decl_node
         && NT_IS(grammar, owner_type->decl_node, "alias-stmt") && depth < 16;
         depth++) {
        th_type *representation = declared_type_from_node(grammar, owner_unit,
            owner_type->decl_node);
        representation = representation ? th_resolve(representation) : nullptr;
        if (!representation || th_kind_of(representation) != TH_INDIRECTION)
            break;
        receiver_type = n00bb_dereferenced_type(state->types, representation);
        if (!receiver_type || th_kind_of(receiver_type) != TH_NAMED)
            return false;
        owner_type = type_symbol_in_loaded_imports(
            unit, state->types, receiver_type, &owner_unit);
    }
    parse_scope_t *members = owner_type ? owner_type->member_scope : nullptr;
    if (!members) {
        if (owner_type && parse_sym_binding(owner_type,
                CORE_STRING_STATIC("n00bb.opaque-type"))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0021"),
                CORE_STRING_STATIC(
                    "record field is not visible from this module"),
                link_source_location(node));
            return true;
        }
        return false;
    }
    core_string_t name = first_token_text(core_tree_child(node, 2));
    parse_sym_entry_t *field = parse_symtab_lookup_in(members, name);
    if (field && n00bb_flagset_member(field)) {
        if (n00bb_receiver_names_type(grammar, unit, state, receiver_node,
                                      owner_type))
            return false;
        parse_tree_set_type(node,
                            th_name(state->types, CORE_STRING_STATIC("bool")));
        parse_tree_set_resolution_symbol(node, field);
        n00bb_flagset_value_t *value = field->has_comptime_value
            ? field->comptime_value : nullptr;
        if (value && !value->atomic
            && n00bb_assignment_target(grammar, node, nullptr))
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_WARNING,
                CORE_STRING_STATIC("N00BB-FLAGSET-COMPOSITE-WRITE"),
                CORE_STRING_STATIC(
                    "assigning a boolean to a composite flagset member updates every flag in the combination"),
                link_source_location(node));
        return false;
    }
    if (field && (field->is_field || field->kind == PARSE_SYM_ENUM_CONST))
        return false;

    core_buffer_t message = {0};
    core_buffer_printf(&message, "record type has no field named '%.*s'",
                       (int)name.u8_bytes, name.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0016"),
        core_buffer_string(&message), link_source_location(node));
    core_buffer_release(&message);
    return true;
}

void
collect_for_bindings(parse_grammar_t *grammar, parse_tree_t *node,
                      core_list_t(parse_tree_ptr_t) *bindings)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (NT_IS(grammar, node, "for-binding")) {
        core_list_push(*bindings, node);
        return;
    }
    if (!NT_IS(grammar, node, "for-var-list")
        && !NT_IS(grammar, node, "for-range-var")) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "for-var-list")
            || NT_IS(grammar, child, "for-range-var")
            || NT_IS(grammar, child, "for-binding"))
            collect_for_bindings(grammar, child, bindings);
    }
}

static bool
bind_for_item_type(parse_grammar_t *grammar,
                   core_translation_unit_t *unit, parse_tree_t *loop,
                   th_type *item_type)
{
    parse_tree_t *vars = CHILD_NT(grammar, loop, "for-var-list");
    if (!vars) vars = CHILD_NT(grammar, loop, "for-range-var");
    core_list_t(parse_tree_ptr_t) bindings = core_list_new(parse_tree_ptr_t);
    collect_for_bindings(grammar, vars, &bindings);
    th_type *resolved_item = item_type ? th_resolve(item_type) : nullptr;
    th_type *resolved = resolved_item;
    bool ordered_tuple = th_builtin_kind_of(resolved_item) == TH_BUILTIN_TUPLE;
    bool named_tuple = resolved && th_kind_of(resolved) == TH_RECORD;
    if (resolved && th_kind_of(resolved) == TH_NAMED && !ordered_tuple) {
        th_type *definition = th_type_definition(tu_state(unit)->types,
                                                  resolved);
        if (!definition) {
            parse_sym_entry_t *type_symbol = type_symbol_in_loaded_imports(
                unit, tu_state(unit)->types, resolved, nullptr);
            parse_scope_t *type_scope = type_symbol
                ? type_symbol->member_scope : nullptr;
            core_list_t(th_member) members = core_list_new(th_member);
            core_list_t(parse_sym_entry_ptr_t) fields =
                core_list_new(parse_sym_entry_ptr_t);
            record_fields_in_source_order(type_scope, &fields);
            for (size_t i = 0; i < fields.len; i++) {
                parse_sym_entry_t *field = fields.data[i];
                core_list_push(members, ((th_member){
                    .name = field->name,
                    .type = field->type,
                }));
            }
            if (members.len) {
                definition = th_record(tu_state(unit)->types,
                    members.data, members.len);
            }
            core_list_free(fields);
            core_list_free(members);
        }
        if (definition) {
            resolved = th_resolve(definition);
            named_tuple = th_kind_of(resolved) == TH_RECORD;
        }
    }
    bool valid = bindings.len != 0;
    bool unmatched_single_name = false;
    size_t tuple_arity = ordered_tuple ? th_named_nargs(resolved_item) : 0;
    if (ordered_tuple && bindings.len > 1) {
        valid = tuple_arity == bindings.len;
        for (size_t i = 0; i < bindings.len; i++) {
            parse_tree_t *binding = bindings.data[i];
            parse_sym_entry_t *symbol = parse_tree_sym(binding);
            const th_arg *element = i < tuple_arity
                ? th_named_arg(resolved_item, i) : nullptr;
            th_type *unified = valid && symbol && element
                && element->kind == TH_ARG_TYPE
                ? th_unify_in_place(tu_state(unit)->types, symbol->type,
                                    element->type) : nullptr;
            if (!unified) {
                valid = false;
                break;
            }
            symbol->type = unified;
            parse_tree_set_type(binding, unified);
            n00bb_loop_binding_projection_t *projection = core_arena_alloc(
                &tu_state(unit)->semantic_values, sizeof(*projection));
            if (!projection) {
                valid = false;
                break;
            }
            *projection = (n00bb_loop_binding_projection_t){
                .index = i,
                .positional = true,
            };
            valid = parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.loop-binding-projection"),
                projection);
        }
    }
    else if (named_tuple) {
        for (size_t i = 0; valid && i < bindings.len; i++) {
            parse_tree_t *binding = bindings.data[i];
            parse_sym_entry_t *symbol = parse_tree_sym(binding);
            const th_member *member = nullptr;
            for (size_t f = 0; symbol && f < th_record_len(resolved); f++) {
                const th_member *candidate = th_record_member(resolved, f);
                if (candidate && core_string_eq(symbol->name,
                                                candidate->name)) {
                    member = candidate;
                    break;
                }
            }
            for (size_t prior = 0; symbol && prior < i; prior++) {
                parse_sym_entry_t *earlier =
                    parse_tree_sym(bindings.data[prior]);
                if (earlier && core_string_eq(symbol->name, earlier->name)) {
                    member = nullptr;
                    break;
                }
            }
            if (!member && bindings.len == 1) {
                bool explicit_let = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.explicit-loop-let")) != nullptr;
                th_type *unified = symbol && item_type
                    ? th_unify_in_place(tu_state(unit)->types, symbol->type,
                                        item_type) : nullptr;
                valid = unified != nullptr;
                if (valid) {
                    symbol->type = unified;
                    parse_tree_set_type(binding, unified);
                    unmatched_single_name = !explicit_let;
                }
                continue;
            }
            th_type *unified = symbol && member
                ? th_unify_in_place(tu_state(unit)->types, symbol->type,
                                    member->type) : nullptr;
            valid = unified != nullptr;
            if (valid) {
                symbol->type = unified;
                parse_tree_set_type(binding, unified);
                n00bb_loop_binding_projection_t *projection =
                    core_arena_alloc(&tu_state(unit)->semantic_values,
                                     sizeof(*projection));
                if (!projection) valid = false;
                else {
                    *projection = (n00bb_loop_binding_projection_t){
                        .name = member->name,
                    };
                    valid = parse_sym_set_binding(symbol,
                        CORE_STRING_STATIC(
                            "n00bb.loop-binding-projection"), projection);
                }
            }
        }
    }
    else if (bindings.len == 1) {
        parse_tree_t *binding = bindings.data[0];
        parse_sym_entry_t *symbol = parse_tree_sym(binding);
        th_type *unified = symbol && item_type
            ? th_unify_in_place(tu_state(unit)->types, symbol->type, item_type)
            : nullptr;
        valid = unified != nullptr;
        if (valid) {
            symbol->type = unified;
            parse_tree_set_type(binding, unified);
        }
    }
    else valid = false;
    parse_sym_entry_t *single_symbol = bindings.len == 1
        ? parse_tree_sym(bindings.data[0]) : nullptr;
    bool warned = single_symbol && parse_sym_binding(single_symbol,
        CORE_STRING_STATIC("n00bb.loop-whole-warning"));
    if (valid && unmatched_single_name && !warned) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-W0005"),
            CORE_STRING_STATIC(
                "single loop name does not match a tuple field; write 'let' explicitly to bind the whole tuple"),
            link_source_location(loop));
        parse_sym_set_binding(single_symbol,
            CORE_STRING_STATIC("n00bb.loop-whole-warning"), single_symbol);
    }
    if (!valid) {
        core_buffer_t message = {0};
        core_string_t printed = resolved
            ? n00bb_print_type(tu_state(unit)->types, resolved)
            : CORE_STRING_STATIC("<unset>");
        core_buffer_printf(&message,
            "loop bindings do not match a named-field tuple item (got %.*s with %zu names)",
            (int)printed.u8_bytes, printed.data, bindings.len);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-T0007"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(loop));
        core_buffer_release(&message);
    }
    core_list_free(bindings);
    return valid;
}

bool
infer_callback_expression_type(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *node,
                               n00bb_tu_state_t *state)
{
    parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
    th_type *callback_type = type_node
        ? parse_tree_type(type_node) : nullptr;
    parse_sym_entry_t *callback = callback_type
        ? type_symbol_in_loaded_imports(unit, state->types, callback_type,
                                       nullptr) : nullptr;
    callback = canonical_symbol(callback);
    n00bb_callback_contract_t *contract = callback
        ? parse_sym_binding(callback,
            CORE_STRING_STATIC("n00bb.callback-contract")) : nullptr;
    if (!contract) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0024"),
            CORE_STRING_STATIC(
                "callback construction requires a callback contract type"),
            link_source_location(node));
        return false;
    }

    parse_tree_t *args = CHILD_NT(grammar, node, "call-args");
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    core_list_t(n00bb_tree_ptr_t) keywords =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(grammar, CHILD_NT(grammar, args,
                                                "positional-args"),
                             &positional);
    collect_keyword_nodes(grammar, CHILD_NT(grammar, args, "keyword-args"),
                          &keywords);
    bool valid = positional.len >= (contract->stateful ? 2u : 1u);
    bool state_writable = true;
    bool state_initialized = true;
    th_type *callable = contract->stateful && contract->setup
        ? contract->setup->type : contract->handler->type;
    th_type *instance = callable
        ? th_type_clone(state->types, callable) : nullptr;
    instance = instance ? th_resolve(instance) : nullptr;
    th_type **callback_variables = nullptr;
    size_t callback_variable_count = instance
        ? th_free_vars(state->types, instance, &callback_variables) : 0;
    size_t parameter_count = contract->stateful && instance
        ? th_func_nparams(instance) : 0;
    if (!contract->stateful) {
        th_type *handler_ref = instance
            ? th_ind(state->types, 0, instance) : nullptr;
        th_type *actual = positional.len
            ? parse_tree_type(positional.data[0]) : nullptr;
        th_type *actual_copy = actual
            ? th_type_clone(state->types, actual) : nullptr;
        valid = valid && positional.len == 1 && !keywords.len
            && handler_ref && actual_copy
            && th_unify_in_place(state->types, handler_ref, actual_copy);
    }
    else {
        parse_tree_t *signature = CHILD_NT(grammar, contract->setup->decl_node,
                                            "func-type-signature");
        parse_tree_t *formals = CHILD_NT(grammar, signature, "param-decl");
        bool *provided = parameter_count
            ? core_alloc_array(bool, parameter_count) : nullptr;
        valid = valid && instance && th_kind_of(instance) == TH_FUNCTION
            && parameter_count >= 2 && provided;
        const th_param *rest = valid ? th_func_rest(instance) : nullptr;
        for (size_t i = 0; valid && i < positional.len; i++) {
            const th_param *parameter = i < parameter_count
                ? th_func_param(instance, i) : rest;
            th_type *actual = parse_tree_type(positional.data[i]);
            th_type *actual_copy = actual
                ? th_type_clone(state->types, actual) : nullptr;
            if (!parameter || !actual_copy
                || !th_unify_in_place(state->types, parameter->type,
                                      actual_copy)) valid = false;
            else if (i < parameter_count) provided[i] = true;
        }
        for (size_t i = 0; valid && i < keywords.len; i++) {
            core_string_t name = call_keyword_name(keywords.data[i]);
            size_t index = 0;
            bool named = function_parameter_named(grammar, contract->setup,
                formals, name, &index);
            parse_tree_t *argument = CHILD_NT(grammar, keywords.data[i],
                                               "expression");
            const th_param *parameter = index < parameter_count
                ? th_func_param(instance, index) : nullptr;
            th_type *actual = argument
                ? parse_tree_type(argument) : nullptr;
            th_type *actual_copy = actual
                ? th_type_clone(state->types, actual) : nullptr;
            if (!named || index < 2 || index >= parameter_count
                || provided[index] || !parameter || !actual_copy
                || !th_unify_in_place(state->types, parameter->type,
                                      actual_copy)) valid = false;
            else provided[index] = true;
        }
        for (size_t i = 0; valid && i < parameter_count; i++) {
            uint8_t flags = 0;
            bool known = function_parameter_info(grammar, contract->setup,
                formals, i, nullptr, nullptr, &flags, nullptr);
            if (!provided[i] && (!known || !(flags & 1u)))
                valid = false;
        }
        core_free(provided);
    }
    if (valid && contract->stateful) {
        parse_sym_entry_t *state_symbol =
            parse_tree_reference_target(positional.data[1]);
        if (state_symbol) {
            state_writable = state_symbol->mutability == PARSE_SYM_MUTABLE;
            if (!state_symbol->is_field) {
                state_initialized = state_symbol->kind == PARSE_SYM_PARAM
                    ? !core_string_eq(state_symbol->direction,
                                      CORE_STRING_STATIC("out"))
                    : state_symbol->definition_count != 0
                        || state_symbol->has_comptime_value
                        || state_symbol->has_runtime_initializer;
            }
            valid = valid && state_writable && state_initialized;
        }
        core_string_t contract_name = {};
        th_type *handler_reference = positional.len
            ? parse_tree_type(positional.data[0]) : nullptr;
        parse_scope_t *scope = state->annotations
            ? parse_symtab_scope_for_node(state->annotations->symtab, node)
            : nullptr;
        valid = valid
            && th_named_info(th_resolve(callback->type), &contract_name,
                              nullptr, nullptr)
            && contract_name.data && contract_name.u8_bytes
            && handler_reference && scope;
        if (valid) {
            static const char marker[] = "__callback_setup__";
            int64_t dot = core_string_rfind_byte(contract_name, '.');
            size_t module_len = dot < 0 ? 0 : (size_t)dot + 1;
            size_t type_len = contract_name.u8_bytes - module_len;
            size_t protocol_len = module_len + sizeof(marker) - 1 + type_len;
            char *protocol_name = module_len && type_len
                ? core_arena_alloc(&state->semantic_values,
                    protocol_len + 1) : nullptr;
            if (protocol_name) {
                memcpy(protocol_name, contract_name.data, module_len);
                memcpy(protocol_name + module_len, marker,
                       sizeof(marker) - 1);
                memcpy(protocol_name + module_len + sizeof(marker) - 1,
                       contract_name.data + module_len, type_len);
                protocol_name[protocol_len] = '\0';
            }
            valid = protocol_name != nullptr;
            n00bb_link_use_site_t site = {
                .node = node,
                .scope = scope,
                .symbol = callback,
                .function_owner = enclosing_function_symbol(grammar, node),
                .initializer_owner = module_variable_initializer_owner(
                    grammar, state->annotations->symtab, node),
                .dispatch_trait = protocol_name
                    ? core_string_borrow(protocol_name,
                        protocol_len)
                    : (core_string_t){0},
                .dispatch_method = CORE_STRING_STATIC("setup"),
                .dispatch_receiver_type = handler_reference,
                .callback_setup = true,
                .semantic_dispatch = true,
                .in_module_initializer = is_module_initializer_use(
                    grammar, node, module_variable_initializer_owner(
                        grammar, state->annotations->symtab, node)),
            };
            if (valid) valid = append_link_use_site(state, site);
        }
    }
    core_list_free(positional);
    core_list_free(keywords);
    if (!valid) {
        core_string_t code = !state_writable
            ? CORE_STRING_STATIC("N00BB-T0026")
            : !state_initialized
                ? CORE_STRING_STATIC("N00BB-T0027")
                : CORE_STRING_STATIC("N00BB-T0024");
        core_string_t message = !state_writable
            ? CORE_STRING_STATIC("callback state must be writable")
            : !state_initialized
                ? CORE_STRING_STATIC(
                    "callback state must be initialized before construction")
                : CORE_STRING_STATIC(
                    "callback construction arguments do not match the handler and setup contract");
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, code, message, link_source_location(node));
        return false;
    }
    th_type *bound_callback = callback->type;
    if (callback_variable_count) {
        core_string_t name = {};
        th_arg *arguments = core_alloc_array(th_arg,
                                              callback_variable_count);
        valid = arguments && th_named_info(callback->type, &name,
            nullptr, nullptr);
        for (size_t i = 0; valid && i < callback_variable_count; i++) {
            th_type *bound = th_resolve(callback_variables[i]);
            valid = bound != nullptr;
            if (valid) arguments[i] = (th_arg){.kind = TH_ARG_TYPE,
                                               .type = bound};
        }
        bound_callback = valid && name.data
            ? th_apply(state->types, name, arguments,
                       callback_variable_count) : nullptr;
        core_free(arguments);
        if (!bound_callback) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0024"),
                CORE_STRING_STATIC(
                    "callback handler does not bind every type variable"),
                link_source_location(node));
            return false;
        }
    }
    parse_tree_set_type(node, bound_callback);
    return parse_tree_set_resolution_symbol(node, callback)
        && parse_sym_add_use(callback, node);
}

void
collect_keyword_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                      core_list_t(n00bb_tree_ptr_t) *arguments)
{
    if (!NT_IS(grammar, node, "keyword-args")) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "call-kw-arg")) {
            core_list_push(*arguments, child);
            continue;
        }
        if (!parse_tree_is_group(child)) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (NT_IS(grammar, item, "call-kw-arg"))
                core_list_push(*arguments, item);
        }
    }
}

bool
resolve_for_binding_types(parse_grammar_t *grammar,
                          core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
         scope; scope = scope->all_next) {
        if (!core_string_eq(scope->scope_tag, CORE_STRING_STATIC("for"))
            || !scope->node || !NT_IS(grammar, scope->node, "for-stmt"))
            continue;
        th_type *item_type = parse_tree_type(scope->node);
        if (CHILD_NT(grammar, scope->node, "for-in-target-list")) {
            parse_tree_t *target = CHILD_NT(grammar, scope->node,
                                             "for-in-target-list");
            parse_tree_t *source = CHILD_NT(grammar, target, "expression");
            th_type *source_type = parse_tree_type(source);
            th_type *cursor_type = nullptr;
            th_type *resolved = resolved_iteration_item_type(
                grammar, unit, scope->node, &cursor_type);
            if (resolved) item_type = resolved;
            if (!resolved) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0007"),
                    CORE_STRING_STATIC(
                        "iteration source does not satisfy the Iterable and Iterator contracts"),
                    link_source_location(scope->node));
                return false;
            }
            if (!cache_iteration_plan(grammar, unit, scope->node,
                    source_type, cursor_type, resolved)) return false;
        }
        if (!bind_for_item_type(grammar, unit, scope->node, item_type))
            return false;
    }
    return true;
}

th_type *
resolved_iteration_item_type(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *loop,
                             th_type **cursor_type_out)
{
    if (cursor_type_out) *cursor_type_out = nullptr;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *target = CHILD_NT(grammar, loop, "for-in-target-list");
    parse_tree_t *source = CHILD_NT(grammar, target, "expression");
    th_type *source_type = parse_tree_type(source);
    th_type *cursor_type = nullptr;
    static const core_string_t methods[] = {
        CORE_STRING_STATIC("iter"),
        CORE_STRING_STATIC("next"),
    };
    th_type *receiver = source_type;
    for (size_t i = 0; state && state->types && receiver && i < 2; i++) {
        th_env_method_t method = {0};
        if (th_env_method_resolve_by_name(state->types, receiver,
                methods[i], &method, nullptr) != TH_ENV_METHOD_FOUND
            || !method.signature) return nullptr;
        th_type *signature = th_type_clone(state->types, method.signature);
        const th_param *self = signature ? th_func_param(signature, 0)
                                         : nullptr;
        if (!self || !th_unify_in_place(state->types, self->type,
                th_type_clone_fresh(state->types, receiver))) return nullptr;
        receiver = th_func_ret(signature);
        if (!receiver) return nullptr;
        if (i == 0) cursor_type = receiver;
    }
    if (cursor_type_out) *cursor_type_out = cursor_type;
    th_type *result = receiver;
    if (th_builtin_kind_of(result) == TH_BUILTIN_OPTION) {
        const th_arg *item = th_named_arg(th_resolve(result), 0);
        return item && item->kind == TH_ARG_TYPE ? item->type : nullptr;
    }
    (void)cursor_type;
    return result;
}

bool
cache_iteration_plan(parse_grammar_t *grammar,
                     core_translation_unit_t *unit, parse_tree_t *loop,
                     th_type *source_type, th_type *cursor_type,
                     th_type *item_type)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !loop || !source_type
        || !cursor_type || !item_type) return false;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, loop);
    if (!facts) return false;
    if (facts->iteration_plan) return true;
    n00bb_iteration_plan_t *plan = core_arena_alloc(
        &state->semantic_values, sizeof(*plan));
    if (!plan) return false;
    parse_scope_t *scope = parse_symtab_scope_for_node(
        state->annotations->symtab, loop);
    parse_sym_entry_t *function_owner = enclosing_function_symbol(grammar,
                                                                   loop);
    n00bb_link_use_site_t site = {
        .node = loop,
        .scope = scope,
        .function_owner = function_owner,
        .dispatch_trait = CORE_STRING_STATIC("Iterable"),
        .dispatch_method = CORE_STRING_STATIC("iter"),
        .dispatch_receiver_type = source_type,
        .secondary_dispatch = true,
        .semantic_dispatch = true,
    };
    plan->iter_site = state->link_use_sites.len;
    if (!append_link_use_site(state, site)) return false;
    site.dispatch_trait = CORE_STRING_STATIC("Iterator");
    site.dispatch_method = CORE_STRING_STATIC("next");
    site.dispatch_receiver_type = cursor_type;
    plan->next_site = state->link_use_sites.len;
    if (!append_link_use_site(state, site)) return false;
    site.dispatch_method = CORE_STRING_STATIC("cleanup");
    plan->cleanup_site = state->link_use_sites.len;
    if (!append_link_use_site(state, site)) return false;
    plan->cursor_type = cursor_type;
    plan->item_type = item_type;
    facts->iteration_plan = plan;
    return true;
}

bool
n00bb_switch_case_integer(parse_grammar_t *grammar, parse_tree_t *node,
                         ir_lit_t *value)
{
    bool negative = false;
    bool saw_sign = false;
    parse_tree_t *literal = n00bb_simple_switch_literal(
        grammar, node, &negative, &saw_sign);
    if (!literal || !n00bb_parse_switch_integer(
            first_token_text(literal), value)) return false;
    if (negative) value->neg = true;
    return true;
}


th_type *
static_sequence_element_type(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return nullptr;
    if (th_kind_of(type) == TH_SEQUENCE) return th_seq_elem(type);
    type = th_builtin_applied(type, TH_BUILTIN_ARRAY, 1);
    const th_arg *argument = type ? th_named_arg(type, 0) : nullptr;
    return argument && argument->kind == TH_ARG_TYPE ? argument->type : nullptr;
}

th_type *
static_tuple_element_type(th_type *type, size_t index, size_t *count)
{
    type = th_builtin_applied(type, TH_BUILTIN_TUPLE, th_named_nargs(type));
    if (!type || index >= th_named_nargs(type)) return nullptr;
    if (count) *count = th_named_nargs(type);
    const th_arg *argument = th_named_arg(type, index);
    return argument && argument->kind == TH_ARG_TYPE ? argument->type : nullptr;
}

bool
static_value_matches_type(const n00bb_static_value_t *value, th_type *type,
                          n00bb_tu_state_t *state, size_t depth)
{
    if (!value || !type || !state || !state->types)
        return false;
    type = th_resolve(type);
    if (!type) return false;
    if (n00bb_atomic_element_type(type) != nullptr)
        return static_value_matches_type(value,
            n00bb_atomic_element_type(type), state, depth + 1);
    th_type *option_element = nullptr;
    if (value->kind != N00BB_STATIC_VALUE_NIL
        && n00bb_option_type_element(type, &option_element))
        return static_value_matches_type(value, option_element, state,
                                         depth + 1);
    if (value->kind == N00BB_STATIC_VALUE_ADDRESS) {
        th_type *referent = th_kind_of(type) == TH_INDIRECTION
            ? th_ind_referent(type) : nullptr;
        return referent && value->as.address && value->as.address->type
            && th_hash(state->types, referent)
                == th_hash(state->types, value->as.address->type);
    }
    if (value->kind == N00BB_STATIC_VALUE_SEQUENCE) {
        th_type *element_type = static_sequence_element_type(type);
        if (!element_type) return false;
        uint64_t extent = 0;
        if (th_kind_of(type) == TH_SEQUENCE
            && th_seq_extent(type, &extent)
            && extent != value->as.aggregate.count) return false;
        for (size_t i = 0; i < value->as.aggregate.count; i++)
            if (!static_value_matches_type(&value->as.aggregate.items[i],
                    element_type, state, depth + 1)) return false;
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_TUPLE) {
        size_t count = th_named_nargs(type);
        if (!th_builtin_applied(type, TH_BUILTIN_TUPLE, count)
            || count != value->as.aggregate.count) return false;
        for (size_t i = 0; i < count; i++) {
            th_type *element_type = static_tuple_element_type(type, i, nullptr);
            if (!element_type || !static_value_matches_type(
                    &value->as.aggregate.items[i], element_type, state,
                    depth + 1)) return false;
        }
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_RECORD) {
        th_type *shape = static_record_shape(state, type);
        if (!shape || th_kind_of(shape) != TH_RECORD
            || th_record_len(shape) != value->as.aggregate.count) return false;
        for (size_t i = 0; i < value->as.aggregate.count; i++) {
            const th_member *member = th_record_member(shape, i);
            if (!member || !static_value_matches_type(
                    &value->as.aggregate.items[i], member->type, state,
                    depth + 1)) return false;
        }
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_VARIANT) {
        bool variant = false;
        th_type *shape = static_record_shape(state, type);
        if (!th_named_info(type, nullptr, &variant, nullptr)
            || !variant || !shape || th_kind_of(shape) != TH_RECORD
            || !value->as.variant.payload) return false;
        for (size_t i = 0; i < th_record_len(shape); i++) {
            const th_member *arm = th_record_member(shape, i);
            if (arm && core_string_eq(value->as.variant.arm, arm->name))
                return value->as.variant.payload->kind
                    == N00BB_STATIC_VALUE_NIL
                    ? th_is_nil(arm->type)
                    : static_value_matches_type(value->as.variant.payload,
                        arm->type, state, depth + 1);
        }
        return false;
    }
    if (value->kind == N00BB_STATIC_VALUE_NIL) {
        th_type *element = nullptr;
        return th_is_nil(type)
            || n00bb_option_type_element(type, &element);
    }
    if (th_kind_of(type) == TH_BITINT)
        return value->kind == N00BB_STATIC_VALUE_INT;
    switch (value->kind) {
    case N00BB_STATIC_VALUE_BOOL:
        return n00bb_scalar_class_is(type, IR_CLASS_BOOL);
    case N00BB_STATIC_VALUE_INT: {
        th_type *definition = th_kind_of(type) == TH_NAMED
            ? th_type_definition(state->types, type) : nullptr;
        definition = definition ? th_resolve(definition) : nullptr;
        return is_integer_type(type)
            || n00bb_scalar_role_is(type, IR_ROLE_BYTE)
            || n00bb_scalar_role_is(type, IR_ROLE_CHAR)
            || (definition && (th_kind_of(definition) == TH_BITINT
                               || is_integer_type(definition)));
    }
    case N00BB_STATIC_VALUE_FLOAT:
        return is_float_type(type);
    case N00BB_STATIC_VALUE_STRING:
        return th_builtin_kind_of(type) == TH_BUILTIN_STRING;
    default:
        return false;
    }
}

static bool
semantic_static_scalar_value(parse_grammar_t *grammar, parse_tree_t *literal,
                             parse_tree_t *source, n00bb_tu_state_t *state,
                             n00bb_static_value_t *out)
{
    n00bb_static_literal_probe_t probe = find_static_literal(grammar, source);
    if (!literal || !source || !state || !out
        || !NT_IS(grammar, literal, "simple-lit")
        || !probe.valid || probe.literal != literal)
        return false;
    parse_token_info_t *token = parse_tree_find_first_terminal(literal);
    if (!token) return false;
    core_string_t terminal = parse_terminal_name(grammar, parse_token_tid(token));
    core_string_t spelling = first_token_text(literal);
    if (core_string_eq(terminal, CORE_STRING_STATIC("STRING_LIT"))) {
        core_buffer_t *decoded = core_buffer_empty();
        if (!n00bb_decode_string_literal(spelling, decoded)) {
            core_buffer_free(decoded);
            return false;
        }
        char *copy = decoded->byte_len
            ? core_arena_alloc(&state->semantic_values, decoded->byte_len)
            : nullptr;
        if (decoded->byte_len && !copy) {
            core_buffer_free(decoded);
            return false;
        }
        if (decoded->byte_len)
            memcpy(copy, decoded->data, decoded->byte_len);
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_STRING,
            .as.string = core_string_borrow(copy, decoded->byte_len),
        };
        core_buffer_free(decoded);
        return true;
    }
    if (core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
        || core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT"))) {
        ir_lit_t integer = {0};
        if (!n00bb_switch_case_integer(grammar, source, &integer))
            return false;
        __int128 value = integer.neg
            ? -(__int128)(integer.mag - 1) - 1
            : (__int128)integer.mag;
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_INT,
            .as.integer = value,
        };
        return true;
    }
    if (core_string_eq(terminal, CORE_STRING_STATIC("FLOAT_LIT"))) {
        if (!spelling.data) return false;
        double value = 0;
        size_t consumed = 0;
        if (!core_string_parse_f64(spelling, &value, &consumed)
            || consumed != spelling.u8_bytes) return false;
        if (probe.negative)
            value = -value;
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_FLOAT,
            .as.real = value,
        };
        return true;
    }
    if (core_string_eq(spelling, CORE_STRING_STATIC("true"))
        || core_string_eq(spelling, CORE_STRING_STATIC("false"))) {
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_BOOL,
            .as.boolean = core_string_eq(spelling,
                CORE_STRING_STATIC("true")),
        };
        return true;
    }
    if (n00bb_nil_spelling(spelling)) {
        *out = (n00bb_static_value_t){.kind = N00BB_STATIC_VALUE_NIL};
        return true;
    }
    return false;
}

bool
collect_ordered_children(parse_tree_t *node,
                         core_list_t(parse_tree_ptr_t) *children)
{
    if (!node) return true;
    n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(node);
    if (!cached) return false;
    for (size_t i = 0; i < cached->count; i++)
        core_list_push(*children, cached->nodes[i]);
    return true;
}

core_string_t
named_field_identifier(parse_grammar_t *grammar, parse_tree_t *field)
{
    if (!NT_IS(grammar, field, "named-field")
        || !core_tree_num_children(field)) return (core_string_t){0};
    parse_tree_t *name = core_tree_child(field, 0);
    parse_token_info_t *token = parse_node_token(name);
    if (!token || !core_string_eq(parse_terminal_name(grammar, parse_token_tid(token)),
                                  CORE_STRING_STATIC("IDENTIFIER")))
        return (core_string_t){0};
    return parse_tree_leaf_text(name);
}

static parse_sym_entry_t *
static_reference_symbol(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        parse_node_resolution_t resolution = parse_tree_resolution(node);
        if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
            return resolution.target.symbol;
        size_t children = core_tree_num_children(node);
        if (children == 3
            && parse_tree_leaf_is(core_tree_child(node, 1),
                                  CORE_STRING_STATIC("."))) {
            parse_sym_entry_t *owner = static_reference_symbol(grammar,
                state, core_tree_child(node, 0));
            if (!owner || owner->kind != PARSE_SYM_TYPEDEF
                || !owner->member_scope) return nullptr;
            core_string_t name = parse_tree_leaf_text(core_tree_child(node, 2));
            n00bb_tu_state_t *declaring = owner->owner_unit
                ? tu_state(owner->owner_unit) : state;
            parse_scope_t *values = declaring && declaring->annotations
                ? parse_symtab_scope_in_namespace(
                    declaring->annotations->symtab, owner->member_scope,
                    CORE_STRING_STATIC(""))
                : nullptr;
            parse_sym_entry_t *member = values
                ? parse_symtab_lookup_in(values, name) : nullptr;
            return member ? member
                : parse_symtab_lookup_in(owner->member_scope, name);
        }
        if (children == 1 && core_tree_is_leaf(core_tree_child(node, 0))) {
            parse_symtab_t *symtab = state && state->annotations
                ? state->annotations->symtab : nullptr;
            return symtab ? lookup_named_type_reference(grammar, state,
                symtab, parse_symtab_scope_for_node(symtab, node),
                parse_tree_leaf_text(core_tree_child(node, 0))) : nullptr;
        }
        if (children != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

static bool
static_reference_value(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                       n00bb_tu_state_t *state, n00bb_static_value_t *out,
                       size_t depth)
{
    if (symbol->kind == PARSE_SYM_ENUM_CONST) {
        if (!symbol->has_comptime_value || !symbol->comptime_value)
            return false;
        __int128 value = 0;
        if (n00bb_flagset_member(symbol))
            value = (__int128)((const n00bb_flagset_value_t *)
                symbol->comptime_value)->mask;
        else {
            const n00bb_enum_value_t *member = symbol->comptime_value;
            value = member->negative
                ? -(__int128)(member->magnitude - 1) - 1
                : (__int128)member->magnitude;
        }
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_INT,
            .as.integer = value,
        };
        return true;
    }
    parse_tree_t *declaration = symbol->kind == PARSE_SYM_VARIABLE
            && symbol->mutability == PARSE_SYM_CONST && symbol->type
        ? symbol->decl_node : nullptr;
    parse_tree_t *initializer = NT_IS(grammar, declaration, "sym-decl")
        ? CHILD_NT(grammar, declaration, "expression") : nullptr;
    n00bb_tu_state_t *owner = symbol->owner_unit
        ? tu_state(symbol->owner_unit) : nullptr;
    return initializer && semantic_static_value_from_expression(grammar,
        initializer, symbol->type, owner ? owner : state, out, depth + 1);
}

bool
semantic_static_value_from_expression(parse_grammar_t *grammar,
                                      parse_tree_t *expression,
                                      th_type *type,
                                      n00bb_tu_state_t *state,
                                      n00bb_static_value_t *out,
                                      size_t depth)
{
    if (!grammar || !expression || !type || !state || !out || depth > 64)
        return false;
    type = th_resolve(type);
    parse_tree_t *reference = direct_ref_expression(grammar, expression);
    if (reference) {
        parse_sym_entry_t *target = direct_ref_target(grammar, reference);
        if (!target || target->kind != PARSE_SYM_VARIABLE
            || target->mutability != PARSE_SYM_CONST
            || target->storage_duration != PARSE_SYM_STORAGE_MODULE
            || th_kind_of(type) != TH_INDIRECTION) return false;
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_ADDRESS,
            .as.address = target,
        };
        return true;
    }
    parse_sym_entry_t *referenced = static_reference_symbol(grammar, state,
                                                            expression);
    if (referenced)
        return static_reference_value(grammar, referenced, state, out, depth);
    n00bb_static_literal_probe_t probe = find_static_literal(grammar,
                                                              expression);
    parse_tree_t *literal = probe.valid ? probe.literal : nullptr;
    parse_tree_t *sequence = NT_IS(grammar, literal, "list-lit")
        ? literal : nullptr;
    parse_tree_t *tuple_or_record = NT_IS(grammar, literal, "tuple-or-paren")
        ? literal : nullptr;

    if (sequence) {
        th_type *element_type = static_sequence_element_type(type);
        if (!element_type) return false;
        parse_tree_t *elements = CHILD_NT(grammar, sequence, "list-elems");
        core_list_t(parse_tree_ptr_t) expressions =
            core_list_new(parse_tree_ptr_t);
        if (!collect_ordered_children(elements, &expressions)) {
            core_list_free(expressions);
            return false;
        }
        n00bb_static_value_t *items = expressions.len
            ? core_arena_alloc(&state->semantic_values,
                expressions.len * sizeof(*items)) : nullptr;
        bool ok = !expressions.len || items;
        for (size_t i = 0; ok && i < expressions.len; i++)
            ok = semantic_static_value_from_expression(grammar,
                expressions.data[i], element_type, state, &items[i],
                depth + 1);
        if (ok) *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_SEQUENCE,
            .as.aggregate = {.items = items, .count = expressions.len},
        };
        core_list_free(expressions);
        return ok;
    }

    if (tuple_or_record) {
        parse_tree_t *fields = CHILD_NT(grammar, tuple_or_record,
                                        "named-field-list");
        if (fields) {
            th_type *shape = static_record_shape(state, type);
            if (!shape || th_kind_of(shape) != TH_RECORD) return false;
            core_list_t(parse_tree_ptr_t) source_fields =
                core_list_new(parse_tree_ptr_t);
            bool ok = collect_ordered_children(fields, &source_fields);
            bool variant = false;
            (void)th_named_info(type, nullptr, &variant, nullptr);
            if (variant) {
                parse_tree_t *field = ok && source_fields.len == 1
                    ? source_fields.data[0] : nullptr;
                core_string_t name = named_field_identifier(grammar, field);
                const th_member *arm = nullptr;
                for (size_t i = 0; i < th_record_len(shape); i++) {
                    const th_member *candidate = th_record_member(shape, i);
                    if (candidate && core_string_eq(name, candidate->name)) {
                        arm = candidate;
                        break;
                    }
                }
                parse_tree_t *payload_node = field
                    ? CHILD_NT(grammar, field, "expression") : nullptr;
                n00bb_static_value_t *payload = arm && payload_node
                    ? core_arena_alloc(&state->semantic_values,
                                       sizeof(*payload)) : nullptr;
                ok = payload && semantic_static_value_from_expression(
                    grammar, payload_node, arm->type, state, payload,
                    depth + 1);
                if (ok) *out = (n00bb_static_value_t){
                    .kind = N00BB_STATIC_VALUE_VARIANT,
                    .as.variant = {
                        .arm = arm->name,
                        .payload = payload,
                    },
                };
                core_list_free(source_fields);
                return ok;
            }
            ok = ok && source_fields.len == th_record_len(shape);
            size_t count = th_record_len(shape);
            n00bb_static_value_t *items = ok && count
                ? core_arena_alloc(&state->semantic_values,
                                   count * sizeof(*items)) : nullptr;
            ok = ok && (!count || items);
            for (size_t i = 0; ok && i < count; i++) {
                const th_member *member = th_record_member(shape, i);
                parse_tree_t *field = nullptr;
                for (size_t j = 0; member && j < source_fields.len; j++)
                    if (core_string_eq(named_field_identifier(grammar,
                                       source_fields.data[j]),
                                       member->name)) {
                        field = source_fields.data[j];
                        break;
                    }
                parse_tree_t *field_value = field
                    ? CHILD_NT(grammar, field, "expression") : nullptr;
                ok = member && field_value
                    && semantic_static_value_from_expression(grammar,
                        field_value, member->type, state, &items[i],
                        depth + 1);
            }
            if (ok) *out = (n00bb_static_value_t){
                .kind = N00BB_STATIC_VALUE_RECORD,
                .as.aggregate = {.items = items, .count = count},
            };
            core_list_free(source_fields);
            return ok;
        }
        size_t tuple_count = th_named_nargs(type);
        if (!th_builtin_applied(type, TH_BUILTIN_TUPLE, tuple_count))
            return false;
        core_list_t(parse_tree_ptr_t) expressions =
            core_list_new(parse_tree_ptr_t);
        bool ok = collect_ordered_children(tuple_or_record, &expressions)
            && expressions.len == tuple_count;
        n00bb_static_value_t *items = ok && tuple_count
            ? core_arena_alloc(&state->semantic_values,
                tuple_count * sizeof(*items)) : nullptr;
        ok = ok && (!tuple_count || items);
        for (size_t i = 0; ok && i < tuple_count; i++) {
            th_type *element_type = static_tuple_element_type(type, i, nullptr);
            ok = element_type && semantic_static_value_from_expression(
                grammar, expressions.data[i], element_type, state, &items[i],
                depth + 1);
        }
        if (ok) *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_TUPLE,
            .as.aggregate = {.items = items, .count = tuple_count},
        };
        core_list_free(expressions);
        return ok;
    }

    uint64_t folded = 0;
    if (!literal && n00bb_static_integer_expression(grammar, expression,
                                                    type, &folded)) {
        *out = (n00bb_static_value_t){
            .kind = N00BB_STATIC_VALUE_INT,
            .as.integer = (__int128)folded,
        };
        return true;
    }
    return literal && NT_IS(grammar, literal, "simple-lit")
        && semantic_static_scalar_value(grammar, literal, expression,
                                        state, out);
}

bool
semantic_static_literal_value(parse_grammar_t *grammar,
                              parse_tree_t *declaration,
                              n00bb_tu_state_t *state,
                              n00bb_static_value_t *out)
{
    parse_sym_entry_t *symbol = parse_tree_sym(declaration);
    parse_tree_t *initializer = CHILD_NT(grammar, declaration, "expression");
    if (!symbol
        || (symbol->mutability != PARSE_SYM_CONST
            && !symbol->is_thread_local)
        || !initializer || !state || !symbol->type)
        return false;
    return semantic_static_value_from_expression(grammar, initializer,
        symbol->type, state, out, 0)
        && static_value_matches_type(out, symbol->type, state, 0);
}

bool
infer_tuple_literal_type(parse_grammar_t *grammar, parse_tree_t *node,
                         n00bb_tu_state_t *state)
{
    if (!grammar || !node || !state || !state->types) return false;
    bool has_comma = false;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)
            && parse_tree_leaf_is(child, CORE_STRING_STATIC(","))) {
            has_comma = true;
            break;
        }
    }
    if (!has_comma) return true;
    core_list_t(parse_tree_ptr_t) expressions =
        core_list_new(parse_tree_ptr_t);
    bool ok = collect_ordered_children(node, &expressions)
        && expressions.len > 0;
    core_list_t(th_type_ptr_t) elements = core_list_new(th_type_ptr_t);
    for (size_t i = 0; ok && i < expressions.len; i++) {
        th_type *element = parse_tree_type(expressions.data[i]);
        if (!element) ok = false;
        else core_list_push(elements, element);
    }
    th_type *tuple = ok
        ? th_tuple(state->types, elements.data, elements.len) : nullptr;
    th_type *existing = parse_tree_type(node);
    if (tuple && existing) {
        th_type *unified = th_unify_in_place(state->types, existing, tuple);
        if (!unified) ok = false;
        else parse_tree_set_type(node, unified);
    }
    else if (tuple) parse_tree_set_type(node, tuple);
    else ok = false;
    core_list_free(elements);
    core_list_free(expressions);
    return ok;
}

bool
infer_sequence_modifier_type(parse_grammar_t *grammar,
                             parse_tree_t *node,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state)
{
    core_string_t prefix = literal_modifier(grammar, node);
    th_type *context_type = parse_tree_type(node);
    th_type *resolved_context = context_type
        ? th_resolve(context_type) : nullptr;
    if (!resolved_context || th_kind_of(resolved_context) == TH_VAR
        || (th_kind_of(resolved_context) == TH_SEQUENCE
            && th_kind_of(th_resolve(th_seq_elem(resolved_context)))
                == TH_VAR)) {
        th_type *expected = record_literal_expected_type(grammar,
                                                         state->types, node);
        if (expected && th_kind_of(th_resolve(expected)) == TH_SEQUENCE)
            context_type = expected;
    }
    bool default_array = !prefix.data && context_type
        && th_kind_of(th_resolve(context_type)) == TH_SEQUENCE;
    const n00bb_literal_registration_t *handler = n00bb_literal_handler(
        prefix, N00BB_LITERAL_SEQUENCE_SHAPE);
    if (prefix.data && !handler) return false;
    bool is_array = default_array
        || (handler && handler->handler == N00BB_LITERAL_ARRAY);
    th_type *element = nullptr;
    parse_tree_t *elements = CHILD_NT(grammar, node, "list-elems");
    core_list_t(parse_tree_ptr_t) expressions =
        core_list_new(parse_tree_ptr_t);
    core_list_t(th_type_ptr_t) element_types =
        core_list_new(th_type_ptr_t);
    bool collected = !elements
        || collect_ordered_children(elements, &expressions);
    th_type *context_element = context_type
        && th_kind_of(th_resolve(context_type)) == TH_SEQUENCE
        ? th_seq_elem(th_resolve(context_type)) : nullptr;
    for (size_t i = 0; collected && i < expressions.len; i++) {
        if (context_element
            && !contextualize_record_literal(grammar, unit, state,
                                              expressions.data[i],
                                              context_element)) {
            collected = false;
            break;
        }
        if (context_element)
            (void)n00bb_integer_literal_joins_type(state,
                expressions.data[i], context_element);
        th_type *item = parse_tree_type(expressions.data[i]);
        if (!item) collected = false;
        else core_list_push(element_types, item);
    }
    literal_item_group_t group = {
        .slot = context_element ? context_element : th_var(state->types),
        .items = element_types,
        .nodes = expressions,
    };
    size_t conflict = SIZE_MAX;
    if (collected && !unify_literal_items(state->types, &group, &conflict)) {
        report_literal_item_conflict(unit, state, node, &group, conflict);
        collected = false;
    }
    element = collected ? group.slot : nullptr;
    core_list_free(element_types);
    core_list_free(expressions);
    if (!collected || !element) return false;
    th_arg element_arg = {.kind = TH_ARG_TYPE, .type = element};
    th_type *sequence_type = is_array ? th_array(state->types, element)
        : th_apply(state->types,
                   CORE_STRING_STATIC("builtin.containers.list.List"),
                   &element_arg, 1);
    if (!sequence_type) return false;
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing,
                                             sequence_type);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else parse_tree_set_type(node, sequence_type);
    return true;
}

bool
infer_brace_modifier_type(parse_grammar_t *grammar,
                          parse_tree_t *node,
                          n00bb_tu_state_t *state)
{
    core_string_t prefix = literal_modifier(grammar, node);
    const n00bb_literal_registration_t *handler = n00bb_literal_handler(
        prefix, N00BB_LITERAL_BRACE_SHAPE);
    if (!handler) return false;
    bool is_dict = handler->handler == N00BB_LITERAL_DICT;
    if (!is_dict && dict_only_empty_literal(grammar, node)) return false;
    core_string_t dict_entries = CORE_STRING_STATIC("dict-entries");
    core_string_t set_entries = CORE_STRING_STATIC("set-entries");
    if (child_nt(grammar, node, is_dict ? set_entries : dict_entries))
        return false;
    parse_tree_t *entries = child_nt(grammar, node,
        is_dict ? dict_entries : set_entries);
    th_type *entry_type = entries
        ? th_resolve(parse_tree_type(entries)) : nullptr;
    const th_arg *entry_key = entry_type
        ? th_named_arg(entry_type, 0) : nullptr;
    const th_arg *entry_value = entry_type && is_dict
        ? th_named_arg(entry_type, 1) : nullptr;
    th_arg args[] = {
        {.kind = TH_ARG_TYPE, .type = entry_key
            && entry_key->kind == TH_ARG_TYPE ? entry_key->type
            : th_var(state->types)},
        {.kind = TH_ARG_TYPE, .type = entry_value
            && entry_value->kind == TH_ARG_TYPE ? entry_value->type
            : th_var(state->types)},
    };
    core_string_t expected_name = is_dict
        ? CORE_STRING_STATIC("builtin.containers.dict.Dict")
        : CORE_STRING_STATIC("builtin.containers.set.Set");
    th_type *literal_type = th_apply(state->types, expected_name, args,
                                     is_dict ? 2 : 1);
    if (!literal_type) return false;

    core_string_t constructor = {};
    size_t argument_count = 0;
    if (!th_named_info(th_resolve(literal_type), &constructor, nullptr,
            &argument_count)) return false;
    bool matches = core_string_eq(constructor, expected_name)
        && argument_count == (is_dict ? 2u : 1u);
    if (!matches) return false;
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing,
                                             literal_type);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else parse_tree_set_type(node, literal_type);
    return true;
}

bool
infer_string_modifier_type(parse_grammar_t *grammar,
                           parse_tree_t *node,
                           n00bb_tu_state_t *state)
{
    core_string_t prefix = literal_modifier(grammar, node);
    const n00bb_literal_registration_t *handler = n00bb_literal_handler(
        prefix, N00BB_LITERAL_STRING_SHAPE);
    if (!handler) return false;
    bool u32 = handler->handler == N00BB_LITERAL_U32;
    th_type *literal_type = u32
        ? th_array(state->types,
                   th_name(state->types, CORE_STRING_STATIC("char")))
        : th_name(state->types, CORE_STRING_STATIC("string"));
    if (!literal_type) return false;
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing,
                                             literal_type);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else parse_tree_set_type(node, literal_type);
    return true;
}

static bool
specialize_literal_handler(n00bb_tu_state_t *state, th_type *signature,
                           th_type *expected, th_type **specialized)
{
    if (!state || !signature || !expected || !specialized) return false;
    *specialized = th_unify(state->types, signature, expected);
    return *specialized != nullptr;
}

static bool
unify_literal_items(th_ctx *types, literal_item_group_t *group,
                    size_t *conflict)
{
    th_type *declared = th_resolve(group->slot);
    if (th_builtin_kind(types, declared) == TH_BUILTIN_ANY) return true;
    th_comb comb;
    if (th_sum_comb(declared, &comb) && comb == TH_UNION) {
        for (size_t i = 0; i < group->items.len; i++) {
            bool arm = false;
            for (size_t a = 0; !arm && a < th_sum_len(declared); a++)
                arm = th_unify_probe(types, th_sum_arm(declared, a),
                                     group->items.data[i]);
            if (!arm) {
                *conflict = i;
                return false;
            }
        }
        return true;
    }
    bool has_nil = false;
    for (size_t i = 0; i < group->items.len; i++)
        has_nil |= th_is_nil(group->items.data[i]);
    th_type *payload = nullptr;
    if (!n00bb_option_type_element(group->slot, &payload) && has_nil) {
        payload = th_var(types);
        if (!th_unify_in_place(types, group->slot,
                               th_option(types, payload))) {
            *conflict = SIZE_MAX;
            return false;
        }
    }
    for (size_t i = 0; i < group->items.len; i++) {
        th_type *item = group->items.data[i];
        if (th_is_nil(item)) continue;
        th_type *target = payload && !n00bb_option_type_element(item, nullptr)
            ? payload : group->slot;
        if (n00bb_value_coerces(types, item, target)) continue;
        if (!th_unify_in_place(types, target, item)) {
            *conflict = i;
            return false;
        }
    }
    return true;
}

static void
report_literal_item_conflict(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *node,
                             literal_item_group_t *group, size_t conflict)
{
    th_type *item = conflict < group->items.len
        ? group->items.data[conflict] : group->slot;
    parse_tree_t *item_node = conflict < group->nodes.len
        ? group->nodes.data[conflict] : node;
    if (n00bb_string_where_buffer_expected(state->types, group->slot, item)) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0036"),
            core_string_borrow(n00bb_string_to_buffer_text,
                               sizeof(n00bb_string_to_buffer_text) - 1),
            link_source_location(item_node));
        return;
    }
    core_buffer_t message = {0};
    core_string_t item_text = n00bb_print_type(state->types, item);
    core_string_t slot_text = n00bb_print_type(state->types, group->slot);
    if (item_node != node && parse_tree_integer_literal_origin(item_node)
        && is_integer_type(group->slot)) {
        core_string_t spelled = n00bb_node_source_text(unit, item_node,
            CORE_STRING_STATIC("this constant"));
        core_buffer_printf(&message, "integer constant %.*s does not fit %.*s",
            (int)spelled.u8_bytes, spelled.data,
            (int)slot_text.u8_bytes, slot_text.data);
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0041"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(item_node));
        core_buffer_release(&message);
        return;
    }
    core_buffer_printf(&message,
        "literal items must share one type, but %.*s does not match %.*s",
        (int)item_text.u8_bytes, item_text.data,
        (int)slot_text.u8_bytes, slot_text.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0034"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(node));
    core_buffer_release(&message);
}

bool
infer_user_literal_type(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node,
                        n00bb_user_litmod_t *chosen)
{
    n00bb_tu_state_t *state = tu_state(unit);
    literal_item_group_t groups[2] = {0};
    size_t group_count = 0;
    parse_tree_t *sequence = NT_IS(grammar, node, "list-lit") ? node : nullptr;
    parse_token_info_t *scalar = NT_IS(grammar, node, "simple-lit")
        ? parse_tree_find_first_terminal(node) : nullptr;
    core_string_t scalar_kind = scalar
        ? parse_terminal_name(grammar, parse_token_tid(scalar))
        : (core_string_t){0};
    parse_tree_t *fence = NT_IS(grammar, node, "embed-lit") ? node : nullptr;
    parse_tree_t *braces = NT_IS(grammar, node, "dict-or-set-lit")
        ? node : nullptr;
    parse_tree_t *parenthesized = NT_IS(grammar, node, "tuple-or-paren")
        ? node : nullptr;
    parse_tree_t *record_fields = CHILD_NT(grammar, parenthesized,
                                            "named-field-list");
    bool tuple = parenthesized && !record_fields
        && core_tree_num_children(parenthesized) == 5;
    parse_tree_t *dict_entries = CHILD_NT(grammar, braces, "dict-entries");
    parse_tree_t *set_entries = CHILD_NT(grammar, braces, "set-entries");
    core_string_t modifier = chosen ? chosen->modifier
        : literal_modifier(grammar, node);
    if (!modifier.data && sequence)
        modifier = CORE_STRING_STATIC("list");
    else if (!modifier.data && dict_entries)
        modifier = CORE_STRING_STATIC("dict");
    else if (!modifier.data && set_entries)
        modifier = CORE_STRING_STATIC("set");
    core_string_t kind = sequence ? CORE_STRING_STATIC("sequence")
        : core_string_eq(scalar_kind, CORE_STRING_STATIC("STRING_LIT"))
            ? CORE_STRING_STATIC("quotes")
        : core_string_eq(scalar_kind, CORE_STRING_STATIC("INT_LIT"))
            ? CORE_STRING_STATIC("int")
        : core_string_eq(scalar_kind, CORE_STRING_STATIC("HEX_LIT"))
            ? CORE_STRING_STATIC("hex")
        : core_string_eq(scalar_kind, CORE_STRING_STATIC("FLOAT_LIT"))
            ? CORE_STRING_STATIC("float")
        : core_string_eq(scalar_kind, CORE_STRING_STATIC("true"))
            || core_string_eq(scalar_kind, CORE_STRING_STATIC("false"))
            ? CORE_STRING_STATIC("bool")
        : n00bb_nil_spelling(first_token_text(node))
            ? CORE_STRING_STATIC("nil")
        : fence ? CORE_STRING_STATIC("fence")
        : dict_entries || dict_only_empty_literal(grammar, braces)
            ? CORE_STRING_STATIC("dict")
        : set_entries ? CORE_STRING_STATIC("set")
        : record_fields ? CORE_STRING_STATIC("record")
        : tuple ? CORE_STRING_STATIC("tuple") : (core_string_t){0};
    if (braces && !kind.data && chosen) kind = chosen->kind;
    if (braces && !kind.data) {
        bool has_dict = user_litmod(state, modifier,
            CORE_STRING_STATIC("dict"), nullptr) != nullptr;
        bool has_set = user_litmod(state, modifier,
            CORE_STRING_STATIC("set"), nullptr) != nullptr;
        if (has_dict && has_set) return false;
        if (has_dict) kind = CORE_STRING_STATIC("dict");
        else if (has_set) kind = CORE_STRING_STATIC("set");
    }
    if (!kind.data) return false;
    bool ambiguous = false;
    n00bb_user_litmod_t *registration = chosen ? chosen
        : user_litmod(state, modifier, kind, &ambiguous);
    if (!registration && ambiguous) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "literal modifier '%.*s' for %.*s literals is exported by more than one imported module",
            (int)modifier.u8_bytes, modifier.data,
            (int)kind.u8_bytes, kind.data);
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0035"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(node));
        core_buffer_release(&message);
    }
    if (!registration) return false;
    th_type *input_type = th_name(state->types,
        core_string_eq(kind, CORE_STRING_STATIC("bool"))
            ? CORE_STRING_STATIC("bool") : CORE_STRING_STATIC("string"));
    parse_tree_t *entries = sequence
        ? CHILD_NT(grammar, sequence, "list-elems")
        : braces ? (dict_entries ? dict_entries : set_entries)
        : record_fields ? record_fields : tuple ? parenthesized : nullptr;
    bool dictionary = braces
        && core_string_eq(kind, CORE_STRING_STATIC("dict"));
    bool aggregate = sequence || braces || tuple || record_fields;
    bool ok = true;
    if (aggregate) {
        group_count = dictionary ? 2 : 1;
        for (size_t g = 0; g < group_count; g++)
            groups[g] = (literal_item_group_t){
                .slot = th_var(state->types),
                .items = core_list_new(th_type_ptr_t),
                .nodes = core_list_new(parse_tree_ptr_t),
            };
        core_list_t(parse_tree_ptr_t) expressions =
            core_list_new(parse_tree_ptr_t);
        ok = !entries || collect_ordered_children(entries, &expressions);
        ok = ok && (!dictionary || !(expressions.len & 1u));
        for (size_t i = 0; ok && i < expressions.len; i++) {
            th_type *type = parse_tree_type(expressions.data[i]);
            literal_item_group_t *group = &groups[dictionary ? i & 1u : 0];
            if (!type) ok = false;
            else {
                core_list_push(group->items, type);
                core_list_push(group->nodes, expressions.data[i]);
            }
        }
        core_list_free(expressions);
        th_type *element = groups[0].slot;
        if (dictionary) {
            th_type *members[] = {groups[0].slot, groups[1].slot};
            element = th_tuple(state->types, members, 2);
        }
        else if (record_fields) {
            th_type *members[] = {
                th_name(state->types, CORE_STRING_STATIC("string")),
                groups[0].slot};
            element = th_tuple(state->types, members, 2);
        }
        input_type = ok ? th_array(state->types, element) : nullptr;
    }
    bool no_payload = core_string_eq(kind, CORE_STRING_STATIC("nil"));
    th_param param = {.mode = TH_IN, .type = input_type};
    th_type *known = parse_tree_type(node) ? th_resolve(parse_tree_type(node))
                                           : nullptr;
    th_type *expected_result = known && th_kind_of(known) != TH_VAR
        ? known : th_var(state->types);
    parse_sym_entry_t *function = registration->declaration
        ? parse_tree_sym(registration->declaration)
        : registration->function;
    th_type *signature = function && function->type
        ? th_resolve(function->type) : nullptr;
    bool takes_site = core_string_eq(kind, CORE_STRING_STATIC("fence"))
        && n00bb_litmod_takes_site(state->types, signature);
    th_param params[2] = {
        param,
        {.mode = TH_IN, .type = n00bb_literal_site_type(state->types)},
    };
    th_type *expected = ok && (input_type || no_payload)
        ? th_func_with_error(state->types,
            expected_result,
            th_name(state->types, CORE_STRING_STATIC("string")),
            no_payload ? nullptr : params,
            no_payload ? 0 : takes_site ? 2 : 1,
            nullptr, nullptr, 0) : nullptr;
    bool deferred = !signature;
    th_type *specialized = expected;
    ok = expected && (deferred || (th_kind_of(signature) == TH_FUNCTION
        && specialize_literal_handler(state, signature, expected,
                                      &specialized)));
    const th_param *payload = ok && aggregate
        ? th_func_param(specialized, 0) : nullptr;
    th_type *element = payload ? th_resolve(payload->type) : nullptr;
    element = element && th_kind_of(element) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(element)) : nullptr;
    if (aggregate && ok && !element) ok = false;
    if (ok && (dictionary || record_fields)) {
        const th_arg *first = th_named_arg(element, 0);
        const th_arg *second = th_named_arg(element, 1);
        ok = first && second && first->kind == TH_ARG_TYPE
            && second->kind == TH_ARG_TYPE;
        if (ok && dictionary) {
            groups[0].slot = first->type;
            groups[1].slot = second->type;
        }
        else if (ok) groups[0].slot = second->type;
    }
    else if (ok && aggregate) groups[0].slot = element;
    size_t conflict = SIZE_MAX;
    for (size_t g = 0; g < group_count; g++) {
        th_type *slot = ok ? th_resolve(groups[g].slot) : nullptr;
        for (size_t i = 0; slot && is_integer_type(slot)
             && i < groups[g].nodes.len; i++)
            if (n00bb_integer_literal_joins_type(state,
                    groups[g].nodes.data[i], slot))
                groups[g].items.data[i] = slot;
        if (ok && !unify_literal_items(state->types, &groups[g], &conflict)) {
            report_literal_item_conflict(unit, state, node, &groups[g],
                                         conflict);
            ok = false;
        }
        core_list_free(groups[g].items);
        core_list_free(groups[g].nodes);
    }
    if (!ok) return false;
    th_type *result = specialized ? th_func_ret(specialized) : nullptr;
    if (!result) return false;
    core_string_t required_shape = n00bb_literal_shape_trait(kind);
    if (required_shape.data) {
        th_type *shape_type = th_var(state->types);
        if (!shape_type || !th_var_constrain(state->types, shape_type,
                (th_constraint){.kind = TH_CON_IMPLEMENTS,
                                .iface = required_shape})
            || !th_unify_in_place(state->types, shape_type, result))
            return false;
    }
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing, result);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else parse_tree_set_type(node, result);
    n00bb_litmod_site_t *site = core_arena_alloc(
        &state->semantic_values, sizeof(*site));
    const th_param *specialized_param = no_payload ? nullptr
        : th_func_param(specialized, 0);
    if (!site || (!no_payload && !specialized_param)) return false;
    *site = (n00bb_litmod_site_t){
        .registration = registration,
        .payload_type = specialized_param ? specialized_param->type : nullptr,
        .signature = specialized,
        .deferred = deferred,
    };
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    if (!facts->literal_site
        && !core_list_try_push(state->literal_site_nodes, node)) return false;
    facts->literal_site = site;
    return true;
}

static core_string_t
shape_literal_kind(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (NT_IS(grammar, node, "list-lit")) return CORE_STRING_STATIC("sequence");
    if (!NT_IS(grammar, node, "dict-or-set-lit")) return (core_string_t){0};
    if (CHILD_NT(grammar, node, "dict-entries")
        || dict_only_empty_literal(grammar, node))
        return CORE_STRING_STATIC("dict");
    if (CHILD_NT(grammar, node, "set-entries"))
        return CORE_STRING_STATIC("set");
    return (core_string_t){0};
}

bool
n00bb_defer_shape_literal(parse_grammar_t *grammar,
                          core_translation_unit_t *unit, parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_node_facts_t *facts = state ? n00bb_ensure_node_facts(state, node)
                                      : nullptr;
    if (!facts) return false;
    if (facts->shape_literal_pending) return true;
    core_string_t kind = shape_literal_kind(grammar, node);
    core_string_t trait = kind.data ? n00bb_literal_shape_trait(kind)
                                    : CORE_STRING_STATIC("Brace_Literal");
    th_type *shape = th_var(state->types);
    if (!shape || !th_var_constrain(state->types, shape,
            (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = trait}))
        return false;
    th_type *existing = parse_tree_type(node);
    th_type *typed = existing
        ? th_unify_in_place(state->types, existing, shape) : shape;
    if (!typed || !core_list_try_push(state->shape_literal_nodes, node))
        return false;
    parse_tree_set_type(node, typed);
    facts->shape_literal_pending = true;
    return true;
}

static th_type *
default_shape_container(n00bb_tu_state_t *state, core_string_t kind)
{
    th_arg args[] = {
        {.kind = TH_ARG_TYPE, .type = th_var(state->types)},
        {.kind = TH_ARG_TYPE, .type = th_var(state->types)},
    };
    if (core_string_eq(kind, CORE_STRING_STATIC("sequence")))
        return th_apply(state->types,
            CORE_STRING_STATIC("builtin.containers.list.List"), args, 1);
    if (core_string_eq(kind, CORE_STRING_STATIC("set")))
        return th_apply(state->types,
            CORE_STRING_STATIC("builtin.containers.set.Set"), args, 1);
    return th_apply(state->types,
        CORE_STRING_STATIC("builtin.containers.dict.Dict"), args, 2);
}

static void
collect_shape_handlers(n00bb_tu_state_t *state, core_string_t kind,
                       th_type *container,
                       core_list_t(n00bb_user_litmod_ptr_t) *matches)
{
    core_list_t(n00bb_user_litmod_ptr_t) visible =
        n00bb_visible_litmods(state, kind);
    for (size_t i = 0; i < visible.len; i++) {
        n00bb_user_litmod_t *registration = visible.data[i];
        parse_sym_entry_t *function = registration->declaration
            ? parse_tree_sym(registration->declaration)
            : registration->function;
        th_type *signature = function && function->type
            ? th_resolve(function->type) : nullptr;
        bool alias = false;
        for (size_t m = 0; m < matches->len; m++) {
            n00bb_user_litmod_t *seen = matches->data[m];
            alias |= (seen->declaration ? parse_tree_sym(seen->declaration)
                                        : seen->function) == function;
        }
        if (!alias && signature && th_kind_of(signature) == TH_FUNCTION
            && th_unify_probe(state->types, th_func_ret(signature),
                              container))
            core_list_push(*matches, registration);
    }
    core_list_free(visible);
}

static void
report_shape_literal(core_translation_unit_t *unit, parse_tree_t *node,
                     core_string_t message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0010"), message,
        link_source_location(node));
}

bool
n00bb_resolve_shape_literal(parse_grammar_t *grammar,
                            core_translation_unit_t *unit, parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_node_facts_t *facts = state ? n00bb_ensure_node_facts(state, node)
                                      : nullptr;
    if (!facts) return false;
    if (facts->shape_literal_resolved) return true;
    facts->shape_literal_resolved = true;
    core_string_t kind = shape_literal_kind(grammar, node);
    th_type *container = parse_tree_type(node)
        ? th_resolve(parse_tree_type(node)) : nullptr;
    if (!container || th_kind_of(container) == TH_VAR) {
        if (!kind.data) {
            report_shape_literal(unit, node, CORE_STRING_STATIC(
                "nothing tells whether this empty `{}` is a set or a "
                "dictionary; give it a type, or write `{:}` for an empty "
                "dictionary"));
            return false;
        }
        th_type *fallback = default_shape_container(state, kind);
        th_type *typed = fallback && parse_tree_type(node)
            ? th_unify_in_place(state->types, parse_tree_type(node),
                                fallback) : fallback;
        if (!typed) {
            core_string_t shown = n00bb_print_type(state->types, fallback);
            th_error_t detail = {0};
            bool explained = th_error_count(state->types)
                && th_error_at(state->types,
                               th_error_count(state->types) - 1, &detail)
                && detail.message.data && detail.got;
            core_string_t got = explained
                ? n00bb_print_type(state->types, detail.got)
                : (core_string_t){0};
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "nothing gives this literal a type, so it becomes %.*s, "
                "but a use of it then conflicts",
                (int)shown.u8_bytes, shown.data);
            if (explained)
                core_buffer_printf(&message, ": %.*s for %.*s",
                    (int)detail.message.u8_bytes, detail.message.data,
                    (int)got.u8_bytes, got.data);
            report_shape_literal(unit, node,
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            return false;
        }
        parse_tree_set_type(node, typed);
        container = th_resolve(typed);
    }
    if (core_string_eq(kind, CORE_STRING_STATIC("sequence"))
        && th_kind_of(container) == TH_SEQUENCE)
        return infer_sequence_modifier_type(grammar, node, unit, state);
    core_list_t(n00bb_user_litmod_ptr_t) matches =
        core_list_new(n00bb_user_litmod_ptr_t);
    if (kind.data) collect_shape_handlers(state, kind, container, &matches);
    else {
        collect_shape_handlers(state, CORE_STRING_STATIC("set"), container,
                               &matches);
        collect_shape_handlers(state, CORE_STRING_STATIC("dict"), container,
                               &matches);
    }
    bool ok = matches.len == 1;
    core_string_t shown = n00bb_print_type(state->types, container);
    if (!ok) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, matches.len
                ? "more than one visible literal modifier produces %.*s"
                : "no visible literal modifier produces %.*s from this "
                  "literal",
            (int)shown.u8_bytes, shown.data);
        report_shape_literal(unit, node,
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
    }
    else {
        size_t before = core_diagnostics_count(
            core_translation_unit_diagnostics(unit));
        ok = infer_user_literal_type(grammar, unit, node, matches.data[0]);
        if (!ok && core_diagnostics_count(
                core_translation_unit_diagnostics(unit)) == before) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "this literal's items do not fit %.*s",
                (int)shown.u8_bytes, shown.data);
            report_shape_literal(unit, node,
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
        }
    }
    core_list_free(matches);
    return ok;
}

static bool
shape_literal_produces(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_tree_t *node, th_type *container)
{
    core_string_t kind = shape_literal_kind(grammar, node);
    if (core_string_eq(kind, CORE_STRING_STATIC("sequence"))
        && th_kind_of(container) == TH_SEQUENCE) return true;
    core_list_t(n00bb_user_litmod_ptr_t) matches =
        core_list_new(n00bb_user_litmod_ptr_t);
    if (kind.data) collect_shape_handlers(state, kind, container, &matches);
    else {
        collect_shape_handlers(state, CORE_STRING_STATIC("set"), container,
                               &matches);
        collect_shape_handlers(state, CORE_STRING_STATIC("dict"), container,
                               &matches);
    }
    bool produces = matches.len != 0;
    core_list_free(matches);
    return produces;
}

bool
n00bb_type_shape_literal(parse_grammar_t *grammar,
                         core_translation_unit_t *unit, parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return false;
    th_type *existing = parse_tree_type(node)
        ? th_resolve(parse_tree_type(node)) : nullptr;
    th_type *context = existing && th_kind_of(existing) != TH_VAR
        ? existing : nullptr;
    if (!context) {
        th_type *expected = record_literal_expected_type(grammar,
                                                         state->types, node);
        expected = expected ? th_resolve(expected) : nullptr;
        if (expected && th_kind_of(expected) != TH_VAR)
            context = expected;
    }
    if (!context || !shape_literal_produces(grammar, state, node, context))
        return n00bb_defer_shape_literal(grammar, unit, node);
    th_type *typed = parse_tree_type(node)
        ? th_unify_in_place(state->types, parse_tree_type(node), context)
        : context;
    if (!typed) return n00bb_defer_shape_literal(grammar, unit, node);
    parse_tree_set_type(node, typed);
    return n00bb_resolve_shape_literal(grammar, unit, node);
}

bool
n00bb_resolve_shape_literals(parse_grammar_t *grammar,
                             core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    bool ok = true;
    for (size_t i = 0; state && i < state->shape_literal_nodes.len; i++)
        ok = n00bb_resolve_shape_literal(grammar, unit,
                 state->shape_literal_nodes.data[i]) && ok;
    return ok;
}

bool
resolve_forward_literal_sites(core_translation_unit_t *unit,
                              n00bb_tu_state_t *state)
{
    bool valid = true;
    for (size_t i = 0; i < state->literal_site_nodes.len; i++) {
        parse_tree_t *node = state->literal_site_nodes.data[i];
        n00bb_litmod_site_t *site = n00bb_node_literal_site(node);
        if (!site || !site->deferred) continue;
        parse_sym_entry_t *function = site->registration->declaration
            ? parse_tree_sym(site->registration->declaration)
            : site->registration->function;
        th_type *signature = function && function->type
            ? th_resolve(function->type) : nullptr;
        th_type *specialized = nullptr;
        if (signature && th_kind_of(signature) == TH_FUNCTION)
            specialize_literal_handler(state, signature, site->signature,
                                       &specialized);
        th_type *result = specialized ? th_func_ret(specialized) : nullptr;
        th_type *unified = result && parse_tree_type(node)
            ? th_unify_in_place(state->types, parse_tree_type(node), result)
            : nullptr;
        bool no_payload = core_string_eq(site->registration->kind,
                                         CORE_STRING_STATIC("nil"));
        const th_param *parameter = specialized && !no_payload
            ? th_func_param(specialized, 0) : nullptr;
        if (!unified || (!no_payload && !parameter)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0030"),
                CORE_STRING_STATIC(
                    "literal handler cannot specialize for this payload"),
                link_source_location(node));
            valid = false;
            continue;
        }
        parse_tree_set_type(node, unified);
        site->signature = specialized;
        site->payload_type = parameter ? parameter->type : nullptr;
        site->deferred = false;
    }
    return valid;
}

th_type *
record_template_type(th_ctx *types, core_string_t name,
                     parse_sym_entry_t *symbol)
{
    n00bb_signature_typevars_t *variables = symbol
        ? parse_sym_binding(symbol, N00BB_SIGNATURE_TYPEVARS_KEY) : nullptr;
    if (!variables || !variables->len) return th_name(types, name);
    th_arg *args = core_alloc_array(th_arg, variables->len);
    if (!args) return nullptr;
    for (size_t i = 0; i < variables->len; i++)
        args[i] = (th_arg){.kind = TH_ARG_TYPE,
                           .type = variables->data[i].type};
    th_type *named = th_apply(types, name, args, variables->len);
    core_free(args);
    return named;
}

th_type *
variant_template_type(th_ctx *types, core_string_t name,
                      parse_sym_entry_t *symbol)
{
    n00bb_signature_typevars_t *variables = symbol
        ? parse_sym_binding(symbol, N00BB_SIGNATURE_TYPEVARS_KEY) : nullptr;
    if (!variables || !variables->len)
        return th_variant(types, name, nullptr, 0);
    th_arg *args = core_alloc_array(th_arg, variables->len);
    if (!args) return nullptr;
    for (size_t i = 0; i < variables->len; i++)
        args[i] = (th_arg){.kind = TH_ARG_TYPE,
                           .type = variables->data[i].type};
    th_type *named = th_variant(types, name, args, variables->len);
    core_free(args);
    return named;
}

bool
cache_record_layout_request(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *declaration)
{
    if (!field_record_declaration(grammar, declaration)) return true;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *symbol = parse_tree_sym(declaration);
    if (!state || !symbol) return false;
    n00bb_record_layout_request_t request = {0};
    bool found = false;
    for (size_t i = 0; i < core_tree_num_children(declaration); i++) {
        parse_tree_t *child = core_tree_child(declaration, i);
        size_t count = parse_tree_is_group(child)
            ? core_tree_num_children(child) : 1;
        for (size_t j = 0; j < count; j++) {
            parse_tree_t *modifier = parse_tree_is_group(child)
                ? core_tree_child(child, j) : child;
            if (!NT_IS(grammar, modifier, "type-layout-modifier")) continue;
            found = true;
            parse_tree_t *layout = CHILD_NT(grammar, modifier,
                                            "type-layout");
            parse_tree_t *alignment = CHILD_NT(grammar, modifier,
                                               "type-align");
            if (layout) {
                core_string_t mode = core_tree_num_children(layout) == 4
                    ? first_token_text(core_tree_child(layout, 2))
                    : (core_string_t){0};
                bool c_mode = core_string_eq(mode, CORE_STRING_STATIC("c"));
                bool ordered_mode = core_string_eq(mode,
                    CORE_STRING_STATIC("ordered"));
                if (!c_mode && !ordered_mode) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0069"),
                        CORE_STRING_STATIC("unknown record layout policy"),
                        link_source_location(modifier));
                    return false;
                }
                if (request.policy_node) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0069"),
                        CORE_STRING_STATIC("duplicate record layout policy"),
                        link_source_location(modifier));
                    return false;
                }
                request.policy = c_mode ? N00BB_RECORD_C
                                        : N00BB_RECORD_ORDERED;
                request.policy_node = modifier;
            }
            else if (alignment) {
                if (request.alignment_node) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0069"),
                        CORE_STRING_STATIC("duplicate record alignment"),
                        link_source_location(modifier));
                    return false;
                }
                request.alignment_expression = CHILD_NT(grammar, alignment,
                                                        "expression");
                request.alignment_node = modifier;
                if (!request.alignment_expression) return false;
            }
            else return false;
        }
    }
    if (!found) return true;
    n00bb_record_layout_request_t *stored = core_arena_alloc(
        &state->semantic_values, sizeof(*stored));
    if (!stored) return false;
    *stored = request;
    return parse_sym_set_binding(symbol,
        CORE_STRING_STATIC("n00bb.record-layout-request"), stored);
}

static parse_sym_entry_t *
member_field_named(parse_scope_t *scope, core_string_t name)
{
    for (parse_sym_entry_t *member = scope ? scope->first_in_scope : nullptr;
         member; member = member->next_in_scope)
        if (member->is_field && core_string_eq(member->name, name))
            return member;
    return nullptr;
}

typedef struct {
    uint64_t record_hash;
    core_string_t field;
    uint32_t group;
} union_arm_entry_t;
core_list_decl(union_arm_entry_t);

static core_list_t(union_arm_entry_t) union_arms;

bool
n00bb_union_arm_register(uint64_t record_hash, core_string_t field,
                         uint32_t group)
{
    if (n00bb_union_arm_group(record_hash, field)) return true;
    core_string_t name = core_string_dup(field);
    if (!name.data) return false;
    core_list_push(union_arms, ((union_arm_entry_t){
        .record_hash = record_hash, .field = name, .group = group,
    }));
    return true;
}

uint32_t
n00bb_union_arm_group(uint64_t record_hash, core_string_t field)
{
    for (size_t i = 0; i < union_arms.len; i++)
        if (union_arms.data[i].record_hash == record_hash
            && core_string_eq(union_arms.data[i].field, field))
            return union_arms.data[i].group;
    return 0;
}

typedef enum {
    FOREIGN_CONSTANT_OK,
    FOREIGN_CONSTANT_NOT_CONSTANT,
    FOREIGN_CONSTANT_MISFIT,
} foreign_constant_status_t;

static foreign_constant_status_t
foreign_clause_constant(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *expression, th_type *target,
                        uint64_t *value)
{
    parse_tree_t *node = expression;
    bool negative = false;
    while (node && !core_tree_is_leaf(node)) {
        size_t count = core_tree_num_children(node);
        if (count == 2 && parse_tree_leaf_is(core_tree_child(node, 0),
                                             CORE_STRING_STATIC("-"))) {
            negative = !negative;
            node = core_tree_child(node, 1);
        }
        else if (count == 1 && !core_tree_is_leaf(core_tree_child(node, 0)))
            node = core_tree_child(node, 0);
        else if (count == 3 && parse_tree_leaf_is(core_tree_child(node, 0),
                                                  CORE_STRING_STATIC("(")))
            node = core_tree_child(node, 1);
        else break;
    }
    uint64_t magnitude = 0;
    ir_interval_t bounds = {0};
    if (!node || !n00bb_type_interval(target, &bounds)
        || !n00bb_static_integer_expression(grammar, node, target,
                                            &magnitude))
        return FOREIGN_CONSTANT_NOT_CONSTANT;
    ir_int_t signed_value = negative ? -(ir_int_t)magnitude
                                     : (ir_int_t)magnitude;
    if (signed_value < bounds.lo || signed_value > bounds.hi) {
        n00bb_report_integer_misfit(state, expression,
            CORE_STRING_STATIC("constant"), ir_interval_point(signed_value),
            target);
        return FOREIGN_CONSTANT_MISFIT;
    }
    *value = (uint64_t)(int64_t)signed_value;
    return FOREIGN_CONSTANT_OK;
}

static th_type *
foreign_element_type(parse_grammar_t *grammar,
                     core_translation_unit_t *unit, parse_sym_entry_t *field)
{
    th_type *type = field && field->decl_node
        ? declared_type_from_node(grammar, unit, field->decl_node) : nullptr;
    th_type *element = nullptr;
    if (type && n00bb_option_type_element(type, &element)) type = element;
    type = type ? th_resolve(type) : nullptr;
    return type && th_kind_of(type) == TH_INDIRECTION
        ? th_ind_referent(type) : nullptr;
}

static void
collect_union_arms(parse_grammar_t *grammar, parse_tree_t *node,
                   core_list_t(parse_tree_ptr_t) *arms)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (NT_IS(grammar, node, "union-arm")) {
        core_list_push(*arms, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_union_arms(grammar, core_tree_child(node, i), arms);
}

static bool
check_unnamed_union_arms(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         n00bb_tu_state_t *state, parse_scope_t *scope,
                         const core_dict_t *named, parse_tree_t *owner)
{
    parse_sym_entry_t *discriminant = member_field_named(scope,
        first_token_text(core_tree_child(owner, 2)));
    th_type *tag_type = discriminant && discriminant->decl_node
        ? declared_type_from_node(grammar, unit, discriminant->decl_node)
        : nullptr;
    core_list_t(parse_tree_ptr_t) arms = core_list_new(parse_tree_ptr_t);
    collect_union_arms(grammar, CHILD_NT(grammar, owner, "union-arms"),
                       &arms);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool ok = tag_type != nullptr;
    for (size_t i = 0; ok && i < arms.len; i++) {
        parse_tree_t *arm = arms.data[i];
        if (core_tree_num_children(arm) != 3) continue;
        uint64_t tag = 0;
        foreign_constant_status_t status = foreign_clause_constant(grammar,
            state, CHILD_NT(grammar, arm, "expression"), tag_type, &tag);
        if (status == FOREIGN_CONSTANT_MISFIT) {
            ok = false;
            break;
        }
        bool duplicate = core_dict_contains((core_dict_t *)named,
                                            (void *)(uintptr_t)tag)
            || !core_dict_add(&seen, tag, 1);
        core_string_t problem = status == FOREIGN_CONSTANT_NOT_CONSTANT
            ? CORE_STRING_STATIC(
                "a union arm's value must be a constant integer")
            : duplicate
            ? CORE_STRING_STATIC("two union arms have the same value")
            : (core_string_t){0};
        if (problem.data) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0037"),
                problem, link_source_location(arm));
            ok = false;
            break;
        }
    }
    core_dict_free(&seen);
    core_list_free(arms);
    return ok;
}

bool
cache_record_field_extents(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *declaration)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *record = parse_tree_sym(declaration);
    parse_scope_t *scope = record ? record->member_scope : nullptr;
    if (!state || !scope) return true;
    for (parse_sym_entry_t *field = scope->first_in_scope; field;
         field = field->next_in_scope) {
        parse_tree_t *clause = field->is_field && field->decl_node
            ? CHILD_NT(grammar, field->decl_node, "field-extent") : nullptr;
        if (!clause) continue;
        n00bb_field_extent_t *extent = core_arena_alloc(
            &state->semantic_values, sizeof(*extent));
        if (!extent) return false;
        bool counted = core_string_eq(first_token_text(clause),
                                      CORE_STRING_STATIC("count"));
        *extent = (n00bb_field_extent_t){
            .kind = counted ? N00BB_FIELD_EXTENT_COUNT
                            : N00BB_FIELD_EXTENT_UNTIL,
            .node = clause,
        };
        th_type *element = counted ? nullptr
            : foreign_element_type(grammar, unit, field);
        foreign_constant_status_t sentinel = counted ? FOREIGN_CONSTANT_OK
            : element ? foreign_clause_constant(grammar, state,
                  CHILD_NT(grammar, clause, "expression"), element,
                  &extent->sentinel)
            : FOREIGN_CONSTANT_NOT_CONSTANT;
        if (sentinel == FOREIGN_CONSTANT_MISFIT) return false;
        if (sentinel == FOREIGN_CONSTANT_NOT_CONSTANT) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0037"),
                CORE_STRING_STATIC("an 'until' sentinel must be a constant integer"),
                link_source_location(clause));
            return false;
        }
        if (counted) {
            core_string_t name = first_token_text(core_tree_child(clause, 1));
            extent->count_field = member_field_named(scope, name);
            if (!extent->count_field || extent->count_field == field) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "'count %.*s' names no other field of this record",
                    (int)name.u8_bytes, name.data);
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0037"),
                    core_string_borrow(message.data, message.byte_len),
                    link_source_location(clause));
                core_buffer_release(&message);
                return false;
            }
        }
        if (!parse_sym_set_binding(field, N00BB_FIELD_EXTENT_KEY, extent))
            return false;
    }
    core_list_t(parse_tree_ptr_t) groups = core_list_new(parse_tree_ptr_t);
    core_dict_t tags;
    core_dict_init(&tags, nullptr, nullptr);
    bool ok = true;
    for (parse_sym_entry_t *field = scope->first_in_scope; ok && field;
         field = field->next_in_scope) {
        parse_tree_t *arm = field->is_field && field->decl_node
            && NT_IS(grammar, field->decl_node, "union-arm")
            ? field->decl_node : nullptr;
        parse_tree_t *owner = arm ? parse_tree_parent(arm) : nullptr;
        while (owner && !NT_IS(grammar, owner, "field-union"))
            owner = parse_tree_parent(owner);
        if (!owner) continue;
        size_t group = 0;
        while (group < groups.len && groups.data[group] != owner) group++;
        if (group == groups.len) core_list_push(groups, owner);
        n00bb_record_layout_request_t *request = parse_sym_binding(record,
            CORE_STRING_STATIC("n00bb.record-layout-request"));
        core_string_t shape = !request || request->policy != N00BB_RECORD_C
            ? CORE_STRING_STATIC("a union needs a layout(c) record")
            : groups.len > 1
            ? CORE_STRING_STATIC("a record holds at most one union")
            : (core_string_t){};
        if (shape.data) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0037"),
                shape, link_source_location(owner));
            ok = false;
            break;
        }
        core_string_t name = first_token_text(core_tree_child(owner, 2));
        n00bb_field_union_t *member = core_arena_alloc(
            &state->semantic_values, sizeof(*member));
        ok = member != nullptr;
        if (!ok) break;
        *member = (n00bb_field_union_t){
            .group = (uint32_t)group + 1,
            .discriminant = member_field_named(scope, name),
            .node = arm,
        };
        bool plain = member->discriminant
            && !(member->discriminant->decl_node
                 && NT_IS(grammar, member->discriminant->decl_node,
                          "union-arm"));
        th_type *tag_type = plain && member->discriminant->decl_node
            ? declared_type_from_node(grammar, unit,
                                      member->discriminant->decl_node)
            : nullptr;
        foreign_constant_status_t tag = tag_type
            ? foreign_clause_constant(grammar, state,
                  CHILD_NT(grammar, arm, "expression"), tag_type,
                  &member->tag)
            : FOREIGN_CONSTANT_NOT_CONSTANT;
        if (plain && tag == FOREIGN_CONSTANT_MISFIT) {
            ok = false;
            break;
        }
        core_string_t problem = !plain
            ? CORE_STRING_STATIC(
                "'union by' names no plain field of this record")
            : tag == FOREIGN_CONSTANT_NOT_CONSTANT
            ? CORE_STRING_STATIC(
                "a union arm's value must be a constant integer")
            : !core_dict_add(&tags, member->tag, 1)
            ? CORE_STRING_STATIC("two union arms have the same value")
            : (core_string_t){};
        if (problem.data) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0037"),
                problem, link_source_location(arm));
            ok = false;
            break;
        }
        ok = parse_sym_set_binding(field, N00BB_FIELD_UNION_KEY, member)
            && (!record->type || n00bb_union_arm_register(
                th_hash(state->types, record->type), field->name,
                member->group));
    }
    for (size_t g = 0; ok && g < groups.len; g++)
        ok = check_unnamed_union_arms(grammar, unit, state, scope, &tags,
                                      groups.data[g]);
    core_dict_free(&tags);
    core_list_free(groups);
    return ok;
}

bool
register_record_semantic_shape(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *declaration)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_scope_t *scope = nullptr;
    for (parse_scope_t *candidate = state && state->annotations
            ? state->annotations->symtab->all_scopes : nullptr;
         candidate; candidate = candidate->all_next) {
        if (scope_in_value_namespace(candidate)
            && n00bb_field_scope_tag(candidate->scope_tag)
            && candidate->node == declaration) {
            scope = candidate;
            break;
        }
    }
    if (!state || !state->types || !scope) return false;
    bool object = NT_IS(grammar, declaration, "object-decl");
    parse_sym_entry_t *symbol = parse_tree_sym(declaration);
    n00bb_field_declaration_types_t types = {0};
    bool valid = n00bb_field_declaration_types(grammar, unit, scope, &types);
    if (valid) {
        valid = th_type_define(state->types, types.shape, types.record)
            && (!object || th_type_define(state->types, types.named,
                                          types.representation));
        if (!valid) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "conflicting record definitions for '%.*s'",
                (int)scope->name.u8_bytes, scope->name.data);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0005"),
                core_string_borrow(message.data, message.byte_len),
                link_source_location(declaration));
            core_buffer_release(&message);
        }
        if (!symbol || symbol->kind != PARSE_SYM_TYPEDEF) valid = false;
        else {
            symbol->type = types.base;
            symbol->adt_kind = object ? CORE_STRING_STATIC("object")
                                      : CORE_STRING_STATIC("record");
            parse_tree_set_type(declaration, symbol->type);
        }
    }
    return valid;
}

bool
n00bb_field_declaration_types(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_scope_t *scope,
                              n00bb_field_declaration_types_t *out)
{
    n00bb_tu_state_t *state = tu_state(unit);
    *out = (n00bb_field_declaration_types_t){0};
    if (!state || !state->types || !scope || !scope->node) return false;
    core_list_t(th_member) members = core_list_new(th_member);
    core_list_t(parse_sym_entry_ptr_t) fields =
        core_list_new(parse_sym_entry_ptr_t);
    record_fields_in_source_order(scope, &fields);
    bool valid = true;
    for (size_t i = 0; i < fields.len; i++) {
        parse_sym_entry_t *field = fields.data[i];
        th_type *field_type = declared_type_from_node(grammar, unit,
                                                       field->decl_node);
        if (!field_type) { valid = false; break; }
        core_list_push(members, ((th_member){
            .name = field->name,
            .type = field_type,
        }));
    }
    core_buffer_t qualified = {0};
    append_module_type_name(unit, scope->name, &qualified);
    parse_sym_entry_t *symbol = parse_tree_sym(scope->node);
    if (valid) {
        out->base = th_name(state->types, core_buffer_string(&qualified));
        out->named = record_template_type(state->types,
                                          core_buffer_string(&qualified),
                                          symbol);
        out->record = th_record(state->types, members.data, members.len);
        out->shape = out->named;
        out->representation = out->record;
    }
    if (valid && NT_IS(grammar, scope->node, "object-decl")) {
        core_buffer_t shape_name = {0};
        core_buffer_printf(&shape_name, "%.*s%s", (int)qualified.byte_len,
                           qualified.data, n00bb_object_shape_suffix);
        out->shape = record_template_type(state->types,
                                          core_buffer_string(&shape_name),
                                          symbol);
        out->representation = out->shape
            ? th_ind(state->types, 0, out->shape) : nullptr;
        core_buffer_release(&shape_name);
    }
    core_buffer_release(&qualified);
    core_list_free(fields);
    core_list_free(members);
    return valid && out->base && out->named && out->record && out->shape
        && out->representation;
}

th_type *
static_record_shape(n00bb_tu_state_t *state, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return nullptr;
    if (th_kind_of(type) == TH_RECORD) return type;
    if (!state || !state->types) return nullptr;
    th_type *shape = th_type_definition(state->types, type);
    if (shape) return shape;
    parse_sym_entry_t *symbol = state->unit
        ? type_symbol_in_loaded_imports(state->unit, state->types, type,
                                        nullptr)
        : nullptr;
    if (!symbol || !symbol->member_scope) return nullptr;
    core_list_t(th_member) members = core_list_new(th_member);
    bool variant_scope = core_string_eq(symbol->member_scope->scope_tag,
                                       CORE_STRING_STATIC("variant"));
    for (parse_sym_entry_t *field = symbol->member_scope->first_in_scope;
         field; field = field->next_in_scope)
        if ((field->is_field || (variant_scope
                && field->kind == PARSE_SYM_VARIABLE)) && field->type)
            core_list_push(members, ((th_member){
                .name = field->name,
                .type = field->type,
            }));
    shape = th_record(state->types, members.data, members.len);
    core_list_free(members);
    return shape;
}

bool
infer_anonymous_record(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_tu_state_t *state)
{
    parse_tree_t *fields = CHILD_NT(grammar, node, "named-field-list");
    if (!fields || !state || !state->types) return true;
    n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(fields);
    if (!cached) return false;
    parse_tree_t *argument = node;
    for (parse_tree_t *parent = parse_tree_parent(node);
         parent && !NT_IS(grammar, argument, "expression")
         && core_tree_num_children(parent) == 1;
         parent = parse_tree_parent(parent))
        argument = parent;
    bool open_fields = NT_IS(grammar, argument, "expression")
        && n00bb_contextual_method_argument(grammar, argument);
    core_list_t(th_member) members = core_list_new(th_member);
    for (size_t i = 0; i < cached->count; i++) {
        parse_tree_t *field = cached->nodes[i];
        if (!core_tree_num_children(field)) continue;
        parse_tree_t *value = core_tree_child(field,
            core_tree_num_children(field) - 1);
        th_type *open = open_fields && core_tree_num_children(field) == 3
            ? th_var(state->types) : nullptr;
        if (open && n00bb_literal_adopts_open_type(grammar, state, value,
                                                   open))
            parse_tree_set_type(field, open);
        parse_tree_t *name_node = core_tree_child(field, 0);
        parse_token_info_t *name_token = parse_node_token(name_node);
        if (!name_token || !core_string_eq(
                parse_terminal_name(grammar, parse_token_tid(name_token)),
                CORE_STRING_STATIC("IDENTIFIER"))) continue;
        core_string_t name = parse_tree_leaf_text(name_node);
        th_type *field_type = parse_tree_type(field);
        if (!field_type) field_type = th_var(state->types);
        core_list_push(members, ((th_member){
            .name = store_semantic_string(state, name),
            .type = field_type,
        }));
    }
    th_type *record = th_record(state->types, members.data, members.len);
    core_list_free(members);
    if (!record) return false;
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing, record);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else {
        parse_tree_set_type(node, record);
    }
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->record_literal_origin = node;
    return true;
}

static th_type *literal_union_shape(th_ctx *types, uint64_t record_hash,
                                    th_type *literal, th_type *definition);

bool
contextualize_record_literal(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state,
                             parse_tree_t *expression, th_type *expected)
{
    if (!expression || !expected) return true;
    th_type *payload = nullptr;
    if (n00bb_option_type_element(expected, &payload)) expected = payload;
    parse_tree_t *literal = n00bb_node_record_literal_origin(expression);
    parse_tree_t *fields = literal
        ? CHILD_NT(grammar, literal, "named-field-list") : nullptr;
    th_type *definition = static_record_shape(state,
        n00bb_object_type(state->types, expected)
            ? th_auto_deref(state->types, expected) : expected);
    if (!fields || !definition
        || th_kind_of(th_resolve(definition)) != TH_RECORD) return true;
    bool variant = false;
    (void)th_named_info(th_resolve(expected), nullptr, &variant, nullptr);
    n00bb_ordered_nodes_t *ordered = n00bb_node_ordered_nodes(fields);
    if (!variant && ordered) {
        th_member *named = ordered->count
            ? core_alloc_array(th_member, ordered->count) : nullptr;
        for (size_t i = 0; named && i < ordered->count; i++) {
            parse_tree_t *field = ordered->nodes[i];
            named[i] = (th_member){
                .name = field && core_tree_num_children(field)
                    ? parse_tree_leaf_text(core_tree_child(field, 0))
                    : CORE_STRING_STATIC(""),
                .type = th_name(state->types, CORE_STRING_STATIC("nil")),
            };
        }
        th_type *names = named || !ordered->count
            ? th_record(state->types, named, ordered->count) : nullptr;
        core_free(named);
        definition = names ? literal_union_shape(state->types,
            th_hash(state->types, expected), names, th_resolve(definition))
            : nullptr;
        if (!definition) return false;
    }
    if (!ordered || ordered->count != (variant ? 1 : th_record_len(definition)))
        return false;
    core_list_t(th_member) members = core_list_new(th_member);
    bool valid = true;
    for (size_t i = 0; i < ordered->count; i++) {
        parse_tree_t *field = ordered->nodes[i];
        if (!field || core_tree_num_children(field) < 3) {
            valid = false;
            break;
        }
        parse_tree_t *name_node = core_tree_child(field, 0);
        parse_tree_t *value_node = core_tree_child(field,
            core_tree_num_children(field) - 1);
        if (!core_tree_is_leaf(name_node) || !value_node
            || core_tree_is_leaf(value_node)) {
            valid = false;
            break;
        }
        core_string_t name = parse_tree_leaf_text(name_node);
        th_type *field_type = record_lookup_text(nullptr, definition, name);
        if (!field_type
            || !contextualize_record_literal(grammar, unit, state, value_node,
                                             field_type)) {
            valid = false;
            break;
        }
        th_type *actual_field = parse_tree_type(value_node);
        bool converted = false;
        if (!n00bb_type_materialize(grammar, state, value_node, actual_field,
                                    field_type, &converted)) {
            valid = false;
            break;
        }
        core_list_push(members, ((th_member){
            .name = store_semantic_string(state, name),
            .type = field_type,
        }));
        parse_tree_set_type(field, field_type);
    }
    th_type *record = valid && !variant
        ? th_record(state->types, members.data, members.len) : nullptr;
    if (!variant)
        valid = record && th_unify_probe(state->types, record, definition);
    core_list_free(members);
    if (!valid) return false;
    parse_sym_entry_t *owner_type = type_symbol_in_loaded_imports(
        unit, state->types, expected, nullptr);
    parse_scope_t *member_scope = owner_type ? owner_type->member_scope : nullptr;
    for (size_t i = 0; !variant && member_scope && i < ordered->count; i++) {
        parse_tree_t *field = ordered->nodes[i];
        core_string_t name = parse_tree_leaf_text(core_tree_child(field, 0));
        parse_sym_entry_t *symbol = parse_symtab_lookup_in(member_scope, name);
        if (!symbol || !symbol->is_field
            || !parse_tree_set_resolution_symbol(field, symbol)
            || !parse_sym_add_definition(symbol, field)) return false;
    }
    for (parse_tree_t *node = literal; node;
         node = parse_tree_parent(node)) {
        parse_tree_set_type(node, expected);
        if (node == expression) break;
    }
    return true;
}

static th_type *
named_field_expected_type(parse_grammar_t *grammar, th_ctx *types,
                          parse_tree_t *field, parse_tree_t *value)
{
    size_t count = core_tree_num_children(field);
    if (!count || core_tree_child(field, count - 1) != value
        || !core_tree_is_leaf(core_tree_child(field, 0))) return nullptr;
    core_string_t name = parse_tree_leaf_text(core_tree_child(field, 0));
    parse_tree_t *list = parse_tree_parent(field);
    while (list && NT_IS(grammar, parse_tree_parent(list),
                         "named-field-list"))
        list = parse_tree_parent(list);
    parse_tree_t *record = list && NT_IS(grammar, list, "named-field-list")
        ? parse_tree_parent(list) : nullptr;
    if (!record || !NT_IS(grammar, record, "tuple-or-paren")) return nullptr;
    th_type *outer = record_literal_expected_type(grammar, types, record);
    th_type *definition = outer ? th_type_definition(types, outer) : nullptr;
    definition = definition ? th_resolve(definition) : nullptr;
    return definition && th_kind_of(definition) == TH_RECORD
        ? record_lookup_text(nullptr, definition, name) : nullptr;
}

static th_type *
list_element_expected_type(parse_grammar_t *grammar, th_ctx *types,
                           parse_tree_t *elements)
{
    parse_tree_t *list = elements;
    while (list && NT_IS(grammar, list, "list-elems"))
        list = parse_tree_parent(list);
    if (!list || !NT_IS(grammar, list, "list-lit")) return nullptr;
    th_type *outer = parse_tree_type(list);
    if (!outer || th_kind_of(th_resolve(outer)) != TH_SEQUENCE)
        outer = record_literal_expected_type(grammar, types, list);
    outer = outer ? th_resolve(outer) : nullptr;
    return outer && th_kind_of(outer) == TH_SEQUENCE
        ? th_seq_elem(outer) : nullptr;
}

static th_type *
literal_context_type(parse_grammar_t *grammar, th_ctx *types,
                     parse_tree_t *literal)
{
    if (!literal) return nullptr;
    parse_tree_t *child = literal;
    for (parse_tree_t *parent = parse_tree_parent(child); parent;
         child = parent, parent = parse_tree_parent(parent)) {
        if (types && NT_IS(grammar, parent, "named-field"))
            return named_field_expected_type(grammar, types, parent, child);
        if (types && NT_IS(grammar, parent, "list-elems"))
            return list_element_expected_type(grammar, types, parent);
        if (NT_IS(grammar, parent, "sym-decl")) {
            parse_tree_t *type_spec = CHILD_NT(grammar, parent, "type-spec");
            return child == CHILD_NT(grammar, parent, "expression")
                && type_spec ? parse_tree_type(type_spec) : nullptr;
        }
        if (NT_IS(grammar, parent, "assign-stmt")) {
            if (core_tree_num_children(parent) != 3
                || core_tree_child(parent, 2) != child) return nullptr;
            parse_tree_t *target = core_tree_child(parent, 0);
            th_type *expected = parse_tree_type(target);
            parse_tree_t *index = n00bb_index_expression_node(grammar,
                                                               target);
            if (!index) return expected;
            th_type *receiver = parse_tree_type(core_tree_child(index, 0));
            th_type *resolved = receiver ? th_resolve(receiver) : nullptr;
            if (resolved && th_kind_of(resolved) == TH_SEQUENCE)
                return th_seq_elem(resolved);
            if (th_builtin_applied(resolved, TH_BUILTIN_ARRAY, 1)) {
                const th_arg *element = th_named_arg(resolved, 0);
                if (element && element->kind == TH_ARG_TYPE)
                    return element->type;
            }
            return expected;
        }
        if (parse_tree_is_group(parent)
            && NT_IS(grammar, parse_tree_parent(parent),
                     "list-elems")) continue;
        if (core_tree_num_children(parent) != 1
            || core_tree_child(parent, 0) != child) return nullptr;
    }
    return nullptr;
}

th_type *
record_literal_expected_type(parse_grammar_t *grammar, th_ctx *types,
                             parse_tree_t *literal)
{
    th_type *context = literal_context_type(grammar, types, literal);
    th_type *element = nullptr;
    return n00bb_option_type_element(context, &element) ? element : context;
}

bool
contextualize_record_fields_before_inference(parse_grammar_t *grammar,
                                              core_translation_unit_t *unit,
                                              n00bb_tu_state_t *state,
                                              parse_tree_t *literal)
{
    th_type *expected = record_literal_expected_type(grammar, state->types,
                                                     literal);
    th_type *definition = expected
        ? th_type_definition(state->types, expected) : nullptr;
    parse_tree_t *fields = CHILD_NT(grammar, literal, "named-field-list");
    n00bb_ordered_nodes_t *ordered = n00bb_node_ordered_nodes(fields);
    if (!definition || th_kind_of(th_resolve(definition)) != TH_RECORD
        || !ordered) return true;
    for (size_t i = 0; i < ordered->count; i++) {
        parse_tree_t *field = ordered->nodes[i];
        if (!field || core_tree_num_children(field) < 3) continue;
        parse_tree_t *value = core_tree_child(field,
            core_tree_num_children(field) - 1);
        if (!value || core_tree_is_leaf(value)) continue;
        core_string_t name = parse_tree_leaf_text(core_tree_child(field, 0));
        th_type *field_type = record_lookup_text(nullptr, definition, name);
        th_type *value_type = parse_tree_type(value);
        bool record_literal = n00bb_node_record_literal_origin(value);
        bool converted = false;
        if (field_type && value_type && !record_literal
            && n00bb_type_materialize(grammar, state, value, value_type,
                                      field_type, &converted)) {
            parse_tree_set_type(field, field_type);
            continue;
        }
        if (!record_literal) continue;
        if (!field_type || !contextualize_record_literal(grammar, unit, state,
                                                          value, field_type))
            return false;
        parse_tree_set_type(field, field_type);
    }
    return true;
}

core_string_t
call_keyword_name(parse_tree_t *argument)
{
    parse_tree_t *name = argument && !core_tree_is_leaf(argument)
        && core_tree_num_children(argument)
        ? core_tree_child(argument, 0) : nullptr;
    return name && core_tree_is_leaf(name) ? parse_tree_leaf_text(name)
                                           : (core_string_t){0};
}

static bool
index_is_method_receiver(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_tree_t *index_node)
{
    parse_tree_t *member = parse_tree_parent(index_node);
    parse_tree_t *call = member ? parse_tree_parent(member)
                                : nullptr;
    const parse_rule_t *rule = call ? parse_tree_rule(grammar, call)
                                    : nullptr;
    return rule && member_access_node(grammar, member) == member
        && core_tree_child(member, 0) == index_node
        && core_tree_child(call, 0) == member
        && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)
        && !call_receiver_is_module(grammar, unit, index_node);
}

bool
index_is_copyout_argument(parse_grammar_t *grammar,
                          core_translation_unit_t *unit,
                          parse_tree_t *index_node)
{
    parse_tree_t *argument = index_node;
    parse_tree_t *container = nullptr;
    for (parse_tree_t *at = index_node; at;
         at = parse_tree_parent(at)) {
        parse_tree_t *parent = parse_tree_parent(at);
        if (!parent) break;
        if (NT_IS(grammar, parent, "positional-args")
            || NT_IS(grammar, parent, "call-kw-arg")) {
            argument = at;
            container = parent;
            break;
        }
    }
    if (!container || n00bb_index_expression_node(grammar, argument)
            != index_node) return false;

    parse_tree_t *args = container;
    while (args && !NT_IS(grammar, args, "call-args"))
        args = parse_tree_parent(args);
    parse_tree_t *call = args ? parse_tree_parent(args) : nullptr;
    while (call && call_arguments_node(grammar, call) != args)
        call = parse_tree_parent(call);
    parse_tree_t *callee = call && core_tree_num_children(call)
        ? core_tree_child(call, 0) : nullptr;
    parse_sym_entry_t *function = callee
        ? bound_function_symbol(callee) : nullptr;
    if (!function || function->kind != PARSE_SYM_FUNCTION) return false;

    parse_tree_t *formals = CHILD_NT(grammar, function->decl_node,
                                     "func-type-signature");
    formals = CHILD_NT(grammar, formals, "param-decl");
    size_t parameter_index = 0;
    if (NT_IS(grammar, container, "call-kw-arg")) {
        core_string_t name = call_keyword_name(container);
        if (!function_parameter_named(grammar, function, formals, name,
                                      &parameter_index)) return false;
    }
    else {
        core_list_t(n00bb_tree_ptr_t) actuals =
            core_list_new(n00bb_tree_ptr_t);
        collect_positional_nodes(grammar, container, &actuals);
        size_t actual_index = SIZE_MAX;
        for (size_t i = 0; i < actuals.len; i++)
            if (actuals.data[i] == argument
                || node_is_within(index_node, actuals.data[i])) {
                actual_index = i;
                break;
            }
        parse_tree_t *member = member_access_node(grammar, callee);
        parse_tree_t *receiver = member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
        bool trait_call = member
            && !call_receiver_is_module(grammar, unit, receiver);
        parameter_index = actual_index == SIZE_MAX ? SIZE_MAX
            : actual_index + (trait_call ? 1 : 0);
        core_list_free(actuals);
        if (parameter_index == SIZE_MAX) return false;
    }
    th_mode mode = TH_IN;
    return function_parameter_info(grammar, function, formals,
               parameter_index, nullptr, nullptr, nullptr, &mode)
        && (mode == TH_OUT || mode == TH_INOUT);
}

void
remember_link_use_site(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       n00bb_tu_state_t *state, parse_tree_t *node)
{
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    bool varref = rule
        && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_VARREF);
    bool trait_reference = NT_IS(grammar, node, "trait-name");
    bool dotted_path = dotted_symbol_path_node(grammar, node);
    bool variant_case_candidate = false;
    core_string_t variant_case_name = {0};
    if (NT_IS(grammar, node, "primary")) {
        for (parse_tree_t *at = parse_tree_parent(node); at;
             at = parse_tree_parent(at)) {
            if (!NT_IS(grammar, at, "case-expr-item")) continue;
            parse_tree_t *expression = CHILD_NT(grammar, at, "expression");
            variant_case_candidate = !CHILD_NT(grammar, at, "range-sep")
                && expression && node_is_within(node, expression)
                && simple_identifier_expression(grammar, expression,
                                                &variant_case_name);
            break;
        }
    }
    bool type_reference = is_type_reference_node(grammar, node);
    core_string_t simple_name = {0};
    if (varref
        && !simple_identifier_expression(grammar, node, &simple_name))
        simple_member_chain_name(grammar, node, &simple_name);
    th_type *new_result = NT_IS(grammar, node, "new-expr")
        ? parse_tree_type(node) : nullptr;
    th_type *resolved_new_result = new_result ? th_resolve(new_result) : nullptr;
    bool new_result_is_variant = false;
    if (resolved_new_result)
        th_named_info(resolved_new_result, nullptr,
                      &new_result_is_variant, nullptr);
    parse_tree_t *init_target = n00bb_init_target(grammar, node);
    bool init_call = init_target != nullptr;
    bool constructor_call = init_call || (resolved_new_result
        && th_kind_of(resolved_new_result) != TH_SEQUENCE
        && th_kind_of(resolved_new_result) != TH_VAR
        && !new_result_is_variant);
    core_string_t dispatch_trait = {0}, dispatch_method = {0};
    bool index_dispatch = index_dispatch_for_node(
        grammar, node, &dispatch_trait, &dispatch_method);
    if (index_dispatch && parse_tree_is_index_address_access(node))
        return;
    bool operator_dispatch = binary_operator_dispatch_for_node(
        grammar, node, &dispatch_trait, &dispatch_method)
        || unary_operator_dispatch_for_node(grammar, node,
                                            &dispatch_trait,
                                            &dispatch_method);
    if (operator_dispatch
        && n00bb_flagset_operator_site(grammar, unit, state, node)) {
        operator_dispatch = false;
        dispatch_trait = (core_string_t){0};
        dispatch_method = (core_string_t){0};
    }
    if (!varref && !trait_reference && !variant_case_candidate
        && !constructor_call
        && !dotted_path && !type_reference && !index_dispatch
        && !operator_dispatch) return;
    if (link_use_site_for_node(state, node)) return;
    core_string_t member_name = dotted_path
        ? first_token_text(core_tree_child(node, 2))
        : type_reference ? bare_type_reference_name(grammar, node)
        : variant_case_candidate ? variant_case_name : simple_name;
    bool callback_call = dotted_path
        && n00bb_callback_function_type(grammar, unit, node) != nullptr;
    if (member_name.data)
        parse_tree_set_unresolved_name(node, member_name);
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    parse_sym_entry_t *symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : parse_tree_sym(node);
    if (!symbol) symbol = attached_symbol(grammar, unit, node);
    if (!symbol && !dotted_path && member_name.data) {
        core_translation_unit_t *imported = find_direct_import(unit,
                                                               member_name);
        parse_scope_t *imported_scope = imported
            ? module_scope(tu_state(imported)) : nullptr;
        if (imported_scope)
            parse_tree_set_resolution_scope(node, imported_scope);
    }
    if (symbol) parse_sym_add_use(symbol, node);
    parse_scope_t *scope = state->annotations
        ? parse_symtab_scope_for_node(state->annotations->symtab, node)
        : nullptr;
    parse_tree_t *receiver_node = init_call ? init_target
        : dotted_path ? core_tree_child(node, 0) : nullptr;
    while (receiver_node && !link_use_site_for_node(state, receiver_node)
           && !core_tree_is_leaf(receiver_node)
           && core_tree_num_children(receiver_node) == 1)
        receiver_node = core_tree_child(receiver_node, 0);
    parse_sym_entry_t *initializer_owner =
        module_variable_initializer_owner(grammar,
            state->annotations ? state->annotations->symtab : nullptr, node);
    size_t trait_path_part_count = 0;
    core_string_t *trait_path_parts = trait_reference
        ? cache_trait_name_parts(node, &trait_path_part_count) : nullptr;
    n00bb_link_use_site_t use_site = {
                                 .node = node,
                                 .receiver_node = receiver_node,
                                 .scope = scope,
                                 .symbol = symbol,
                                 .member_name = member_name,
                                 .trait_path_parts = trait_path_parts,
                                 .trait_path_part_count =
                                     trait_path_part_count,
                                 .function_owner =
                                     enclosing_function_symbol(grammar, node),
                                 .initializer_owner = initializer_owner,
                                 .dispatch_trait = dispatch_trait,
                                 .dispatch_method = dispatch_method,
                                 .dotted_path = dotted_path,
                                 .trait_reference = trait_reference,
                                 .variant_case_candidate =
                                     variant_case_candidate,
                                 .constructor_call = constructor_call,
                                 .init_call = init_call,
                                 .callback_call = callback_call,
                                 .operator_dispatch = operator_dispatch,
                                 .in_module_initializer =
                                     is_module_initializer_use(grammar, node,
                                                               initializer_owner)};
    if (!append_link_use_site(state, use_site)) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0004"),
            CORE_STRING_STATIC("could not retain source reference"),
            link_source_location(node));
        return;
    }
    bool copyout = index_dispatch
        && (index_is_compound_target(grammar, node)
            || index_is_copyout_argument(grammar, unit, node));
    bool receiver = index_dispatch && !copyout
        && index_is_method_receiver(grammar, unit, node);
    if ((copyout || receiver)
        && core_string_eq(dispatch_trait,
                          CORE_STRING_STATIC("Index_Read"))) {
        use_site.optional_dispatch = receiver;
        use_site.symbol = nullptr;
        use_site.reference = nullptr;
        use_site.dispatch_trait = CORE_STRING_STATIC("Index_Write");
        use_site.dispatch_method = CORE_STRING_STATIC("__write");
        use_site.resolved_trait_name = (core_string_t){};
        use_site.secondary_dispatch = true;
        if (!append_link_use_site(state, use_site))
            core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0004"),
                CORE_STRING_STATIC("could not retain source reference"),
                link_source_location(node));
    }
}

bool
n00bb_builtin_callback_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-call"));
}

bool
n00bb_builtin_option_function(parse_sym_entry_t *symbol)
{
    return symbol && (core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_is_nil"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_get"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_get_or_else")));
}

parse_sym_entry_t *
formal_symbol_named(parse_grammar_t *grammar, parse_tree_t *formals,
                   core_string_t name, size_t *index)
{
    if (!formals || !name.data) return nullptr;
    for (size_t i = 0;; i++) {
        size_t cursor = 0;
        parse_sym_entry_t *formal = formal_symbol_at(grammar, formals, i,
                                                      &cursor);
        if (!formal) break;
        if (core_string_eq(formal->name, name)) {
            if (index) *index = i;
            return formal;
        }
    }
    return nullptr;
}

bool
function_parameter_info(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                        parse_tree_t *formals, size_t index,
                        parse_sym_entry_t **formal_out, core_string_t *name,
                        uint8_t *flags, th_mode *mode)
{
    if (symbol && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-value-method"))) {
        th_type *signature = th_resolve(symbol->callable_contract
            ? symbol->callable_contract : symbol->type);
        const th_param *parameter = signature
            && th_kind_of(signature) == TH_FUNCTION
            ? th_func_param(signature, index) : nullptr;
        if (!parameter) return false;
        if (formal_out) *formal_out = nullptr;
        if (name)
            *name = index ? CORE_STRING_STATIC("other")
                          : CORE_STRING_STATIC("self");
        if (flags) *flags = 0;
        if (mode) *mode = parameter->mode;
        return true;
    }
    if (!formals && symbol) {
        parse_sym_entry_t *requirement = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"));
        parse_tree_t *signature = requirement
            ? CHILD_NT(grammar, requirement->decl_node,
                       "trait-func-type-signature") : nullptr;
        if (signature)
            formals = CHILD_NT(grammar, signature, "trait-param-decl");
    }
    th_type *callback_public = n00bb_builtin_callback_function(symbol)
        ? parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-public-signature")) : nullptr;
    if (n00bb_builtin_callback_function(symbol)
        && (!callback_public || !symbol->decl_node)) {
        th_type *signature = th_resolve(callback_public
            ? callback_public : symbol->type);
        const th_param *parameter = signature
            && th_kind_of(signature) == TH_FUNCTION
            ? th_func_param(signature, index) : nullptr;
        if (!parameter) return false;
        if (formal_out) *formal_out = nullptr;
        if (name) *name = (core_string_t){};
        if (flags) *flags = 0;
        if (mode) *mode = parameter->mode;
        return true;
    }
    if (n00bb_builtin_option_function(symbol)) {
        size_t count = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_get_or_else")) ? 2 : 1;
        if (index >= count) return false;
        if (formal_out) *formal_out = nullptr;
        if (name)
            *name = index ? CORE_STRING_STATIC("default")
                          : CORE_STRING_STATIC("self");
        if (flags) *flags = 0;
        if (mode) *mode = TH_IN;
        return true;
    }
    parse_sym_entry_t *formal = formal_symbol_at(grammar, formals, index,
                                                  &(size_t){0});
    if (formal) {
        if (formal_out) *formal_out = formal;
        if (name) *name = formal->name;
        if (flags) *flags = formal->is_optional ? 1u : 0u;
        if (mode) {
            if (NT_IS(grammar, formal->decl_node, "trait-obj-tref")) {
                th_type *signature = symbol->callable_contract
                    ? symbol->callable_contract
                    : symbol->type;
                signature = th_resolve(signature);
                if (th_kind_of(signature) == TH_BINDER)
                    signature = th_resolve(th_binder_body(signature));
                const th_param *parameter = signature
                    && th_kind_of(signature) == TH_FUNCTION
                    ? th_func_param(signature, index) : nullptr;
                *mode = parameter ? parameter->mode : TH_IN;
            }
            else *mode = formal->has_mode ? formal->mode : TH_IN;
        }
        return true;
    }
    uint8_t archived_mode = 0;
    bool archived = context_symbol(grammar, symbol)
        ? context_interface_parameter(symbol, index, name, flags,
                                      &archived_mode)
        : function_interface_parameter(symbol, index, name,
                                       flags, &archived_mode);
    if (!archived) return false;
    if (formal_out) *formal_out = nullptr;
    if (mode) *mode = (th_mode)archived_mode;
    return true;
}

bool
function_parameter_named(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                         parse_tree_t *formals, core_string_t name,
                         size_t *index)
{
    if (symbol && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-value-method"))) {
        size_t count = th_func_nparams(symbol->callable_contract
            ? symbol->callable_contract : symbol->type);
        if (count && core_string_eq(name, CORE_STRING_STATIC("self"))) {
            if (index) *index = 0;
            return true;
        }
        if (count > 1 && core_string_eq(name,
                CORE_STRING_STATIC("other"))) {
            if (index) *index = 1;
            return true;
        }
        return false;
    }
    if (!formals && symbol) {
        parse_sym_entry_t *requirement = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"));
        parse_tree_t *signature = requirement
            ? CHILD_NT(grammar, requirement->decl_node,
                       "trait-func-type-signature") : nullptr;
        if (signature)
            formals = CHILD_NT(grammar, signature, "trait-param-decl");
    }
    if (n00bb_builtin_callback_function(symbol)
        && !parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-public-signature"))) {
        th_type *signature = th_resolve(symbol->type);
        if (!signature || th_kind_of(signature) != TH_FUNCTION) return false;
        const th_kwparam *keyword = th_func_kw_lookup(signature, name);
        if (!keyword) return false;
        if (index) *index = th_func_nparams(signature)
            + (size_t)(keyword - th_func_kw(signature, 0));
        return true;
    }
    if (n00bb_builtin_option_function(symbol)) {
        if (core_string_eq(name, CORE_STRING_STATIC("self"))) {
            if (index) *index = 0;
            return true;
        }
        if (core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_get_or_else"))
            && core_string_eq(name, CORE_STRING_STATIC("default"))) {
            if (index) *index = 1;
            return true;
        }
        return false;
    }
    if (n00bb_builtin_reference_function(symbol)) {
        bool write = core_string_eq(symbol->name,
                                    CORE_STRING_STATIC("ref_write"));
        if ((write || core_string_eq(symbol->name,
                CORE_STRING_STATIC("ref_read")))
            && core_string_eq(name, CORE_STRING_STATIC("reference"))) {
            if (index) *index = 0;
            return true;
        }
        if (write && core_string_eq(name, CORE_STRING_STATIC("value"))) {
            if (index) *index = 1;
            return true;
        }
        return false;
    }
    parse_sym_entry_t *formal = formal_symbol_named(grammar, formals, name,
                                                    index);
    if (formal) return true;
    if (context_symbol(grammar, symbol))
        return context_interface_parameter_named(symbol, name, index);
    return function_interface_parameter_named(symbol, name, index);
}

parse_tree_t *
callable_formal_parameters(parse_grammar_t *grammar,
                           parse_sym_entry_t *symbol)
{
    parse_tree_t *declaration = symbol ? symbol->decl_node : nullptr;
    if (!declaration) return nullptr;
    if (NT_IS(grammar, declaration, "context-def"))
        return CHILD_NT(grammar, declaration, "param-decl");
    parse_tree_t *signature = CHILD_NT(grammar, declaration,
                                       "func-type-signature");
    if (signature) return CHILD_NT(grammar, signature, "param-decl");
    signature = CHILD_NT(grammar, declaration, "trait-func-type-signature");
    return signature ? CHILD_NT(grammar, signature, "trait-param-decl")
                     : nullptr;
}

th_type *
n00bb_atomic_element_type(th_type *type)
{
    type = th_builtin_applied(type, TH_BUILTIN_ATOMIC, 1);
    const th_arg *argument = type ? th_named_arg(type, 0) : nullptr;
    return argument && argument->kind == TH_ARG_TYPE
        ? argument->type : nullptr;
}

bool
n00bb_option_type_element(th_type *type, th_type **element)
{
    th_type *item = th_option_item(type);
    if (!item) return false;
    if (element) *element = item;
    return true;
}

th_type *
n00bb_operand_value_type(th_type *operand, th_type *other, bool comparison)
{
    th_type *resolved = operand ? th_resolve(operand) : nullptr;
    th_type *other_resolved = other ? th_resolve(other) : nullptr;
    th_type *option_item = resolved && !comparison
        ? th_option_item(resolved) : nullptr;
    if (option_item) return option_item;
    if (!resolved || th_kind_of(resolved) != TH_INDIRECTION) return operand;
    bool other_reference = other_resolved
        && (th_kind_of(other_resolved) == TH_INDIRECTION
            || th_is_nil(other_resolved));
    if (comparison && (!other_resolved || other_reference)) return operand;
    return th_ind_referent(resolved);
}

th_type *
n00bb_dereference_target(th_ctx *types, th_type *actual, th_type *wanted)
{
    actual = actual ? th_resolve(actual) : nullptr;
    wanted = wanted ? th_resolve(wanted) : nullptr;
    if (!actual || !wanted || th_kind_of(actual) != TH_INDIRECTION
        || th_kind_of(wanted) == TH_INDIRECTION) return nullptr;
    th_type *referent = th_ind_referent(actual);
    if (th_kind_of(wanted) == TH_VAR)
        return referent && th_resolve(referent) == wanted ? wanted : nullptr;
    if (th_unify_probe(types, referent, wanted))
        return th_is_closed(types, wanted) ? wanted : referent;
    if (is_integer_type(referent) && is_integer_type(wanted))
        return th_resolve(referent);
    th_type *element = th_option_item(wanted);
    return element && th_kind_of(th_resolve(element)) != TH_INDIRECTION
        && th_unify_probe(types, referent, element) ? element : nullptr;
}

bool
n00bb_dereferences_to(th_ctx *types, th_type *actual, th_type *wanted)
{
    return n00bb_dereference_target(types, actual, wanted) != nullptr;
}

bool
n00bb_practical_unsigned_to_signed(parse_grammar_t *grammar,
                                   n00bb_tu_state_t *state,
                                   parse_tree_t *source_node,
                                   th_type *source, th_type *target)
{
    uint64_t source_width = 0, target_width = 0;
    bool source_signed = true, target_signed = false;
    return n00bb_integer_representation(source, &source_width,
                                        &source_signed)
        && n00bb_integer_representation(target, &target_width,
                                        &target_signed)
        && source_width == 64 && target_width == 64
        && !source_signed && target_signed
        && !n00bb_range_proven_above_int64(grammar, state, source_node);
}

void
record_function_exit_fact(parse_grammar_t *grammar,
                          core_translation_unit_t *unit,
                          parse_tree_t *node, n00bb_tu_state_t *state)
{
    if (!state || !state->types || !node
        || (!NT_IS(grammar, node, "return-stmt")
            && !NT_IS(grammar, node, "fail-stmt"))) return;
    parse_tree_t *value = CHILD_NT(grammar, node, "expression");
    th_type *observed = value ? parse_tree_type(value) : nullptr;

    parse_tree_t *function = parse_tree_parent(node);
    while (function && !NT_IS(grammar, function, "func-def"))
        function = parse_tree_parent(function);
    parse_sym_entry_t *symbol = function
        ? parse_tree_sym(function) : nullptr;
    if (!symbol) return;

    static const core_string_t facts_key =
        CORE_STRING_STATIC("n00bb.function.return-facts");
    n00bb_function_return_facts_t *facts = parse_sym_binding(symbol,
                                                               facts_key);
    if (!facts) {
        facts = core_arena_alloc(&state->semantic_values, sizeof(*facts));
        if (!facts || !parse_sym_set_binding(symbol, facts_key, facts)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "could not retain function exit type facts"),
                link_source_location(node));
            return;
        }
        memset(facts, 0, sizeof(*facts));
        facts->returned_parameter = SIZE_MAX;
    }

    bool success_channel = NT_IS(grammar, node, "return-stmt");
    if (success_channel) {
        if (!core_arena_try_reserve_raw(&state->semantic_values,
                (void **)&facts->return_expressions,
                &facts->return_expression_capacity,
                facts->return_expression_count + 1,
                sizeof(*facts->return_expressions))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "could not retain function return-source facts"),
                link_source_location(node));
            return;
        }
        facts->return_expressions[facts->return_expression_count++] = value;

        if (!observed) return;

        th_type *signature = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.body-function-signature"));
        if (!signature) signature = symbol->type;
        th_type *expected = signature ? th_func_ret(signature) : nullptr;
        if (expected) {
            bool converted = false;
            core_diagnostics_t *diagnostics =
                core_translation_unit_diagnostics(unit);
            size_t before = core_diagnostics_count(diagnostics);
            th_type *materialized = n00bb_type_materialize(grammar, state,
                value, observed, expected, &converted);
            if (!materialized) {
                facts->conflict = true;
                facts->reported = true;
                if (core_diagnostics_count(diagnostics) == before) {
                    core_string_t actual = n00bb_print_type(state->types,
                                                            observed);
                    core_string_t wanted = n00bb_print_type(state->types,
                                                            expected);
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "return value of type %.*s does not convert to the declared return type %.*s",
                        (int)actual.u8_bytes, actual.data,
                        (int)wanted.u8_bytes, wanted.data);
                    core_diagnostics_report_at(diagnostics, unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0044"),
                        core_buffer_string(&message),
                        link_source_location(value));
                    core_buffer_release(&message);
                }
            }
            else {
                observed = materialized;
                if (!converted) parse_tree_set_type(value, observed);
            }
        }
    }

    if (!observed) return;

    th_type **channel = success_channel ? &facts->success : &facts->failure;
    if (!*channel) *channel = observed;
    else {
        th_type *unified = th_unify_in_place(state->types, *channel, observed);
        if (!unified) facts->conflict = true;
        else *channel = unified;
    }
}

bool
n00bb_literal_fits_scalar(ir_uint_t magnitude, bool negative,
                          const ir_scalar_t *scalar)
{
    if (!scalar || !scalar->width || scalar->width > N00BB_MAX_SIZED_BITS) return false;
    if (scalar->class == IR_CLASS_SINT) {
        ir_uint_t limit = (ir_uint_t)1 << (scalar->width - 1);
        return magnitude <= (negative ? limit : limit - 1);
    }
    if (scalar->class != IR_CLASS_UINT || (negative && magnitude))
        return false;
    return scalar->width == 128
        || magnitude <= (((ir_uint_t)1 << scalar->width) - 1);
}


static bool
n00bb_contextual_method_argument(parse_grammar_t *grammar,
                                 parse_tree_t *expression)
{
    parse_tree_t *parent = parse_tree_parent(expression);
    if (NT_IS(grammar, parent, "assign-stmt")
        && core_tree_num_children(parent) == 3
        && core_tree_child(parent, 2) == expression)
        return n00bb_index_expression_node(grammar,
            core_tree_child(parent, 0)) != nullptr;
    if (NT_IS(grammar, parent, "call-kw-arg"))
        parent = parse_tree_parent(parent);
    if (!NT_IS(grammar, parent, "positional-args")
        && !NT_IS(grammar, parent, "keyword-args")) return false;
    parse_tree_t *args = parse_tree_parent(parent);
    parse_tree_t *call = args ? parse_tree_parent(args) : nullptr;
    parse_tree_t *callee = NT_IS(grammar, args, "call-args")
        && NT_IS(grammar, call, "postfix-expr")
        && core_tree_num_children(call) ? core_tree_child(call, 0) : nullptr;
    return NT_IS(grammar, callee, "postfix-expr")
        && core_tree_num_children(callee) >= 3
        && core_tree_is_leaf(core_tree_child(callee, 1))
        && parse_tree_leaf_is(core_tree_child(callee, 1),
                              CORE_STRING_STATIC("."));
}

static bool
n00bb_scalar_repeats_earlier(const ir_profile_t *profile, size_t index)
{
    const ir_scalar_t *scalar = &profile->scalars[index];
    for (size_t i = 0; i < index; i++)
        if (profile->scalars[i].class == scalar->class
            && profile->scalars[i].width == scalar->width
            && profile->scalars[i].role == scalar->role) return true;
    return false;
}

static bool
n00bb_literal_candidate_scalar(const ir_profile_t *profile, size_t index)
{
    const ir_scalar_t *scalar = &profile->scalars[index];
    return !n00bb_scalar_repeats_earlier(profile, index)
        && (scalar->class == IR_CLASS_SINT || scalar->class == IR_CLASS_UINT)
        && (scalar->role == IR_ROLE_NONE || scalar->role == IR_ROLE_BYTE);
}

static bool
n00bb_constrain_literal_fit(parse_grammar_t *grammar,
                           n00bb_tu_state_t *state, parse_tree_t *origin,
                           parse_tree_t *node, th_type *type,
                           bool unsigned_only, bool option_alternatives)
{
    const ir_profile_t *profile = n00bb_type_profile();
    if (!origin || !node || !type || th_kind_of(th_resolve(type)) != TH_VAR)
        return false;
    ir_lit_t literal = {0};
    ir_interval_t range = {0};
    if (NT_IS(grammar, origin, "simple-lit")) {
        if (!n00bb_parse_switch_integer(first_token_text(origin), &literal))
            return false;
    }
    else if (n00bb_current_integer_range(origin, &range)
             && range.lo == range.hi) {
        literal.neg = range.lo < 0;
        literal.mag = range.lo < 0 ? (ir_uint_t)(-(range.lo + 1)) + 1
                                   : (ir_uint_t)range.lo;
    }
    else return false;
    bool negative = literal.neg;
    for (parse_tree_t *at = parse_tree_parent(origin);
         at && at != node; at = parse_tree_parent(at))
        if (NT_IS(grammar, at, "unary-expr")
            && core_tree_num_children(at) == 2
            && parse_tree_leaf_is(core_tree_child(at, 0),
                                  CORE_STRING_STATIC("-")))
            negative = !negative;
    if (n00bb_open_integer(type)) {
        if (unsigned_only && !n00bb_require_unsigned_integer(state, type))
            return false;
        th_type *seeded = n00bb_integer_literal_variable(state, origin,
            (ir_lit_t){.mag = literal.mag, .neg = negative && literal.mag});
        return seeded && th_unify_in_place(state->types, type, seeded);
    }
    size_t scalar_count = profile->n_scalars;
    size_t capacity = (scalar_count + 2 * N00BB_MAX_SIZED_BITS)
        * (option_alternatives ? 2 : 1);
    th_type **alternatives = core_arena_alloc(&state->semantic_values,
        capacity * sizeof(*alternatives));
    if (!alternatives) return false;
    size_t count = 0;
    for (size_t i = 0; i < scalar_count; i++) {
        if (!n00bb_literal_candidate_scalar(profile, i)
            || (unsigned_only && profile->scalars[i].class != IR_CLASS_UINT)
            || !n00bb_literal_fits_scalar(literal.mag, negative,
                                          &profile->scalars[i])) continue;
        th_type *candidate = th_name(state->types,
                                      profile->scalars[i].type_text);
        if (candidate) alternatives[count++] = candidate;
    }
    for (uint64_t width = 1; width <= N00BB_MAX_SIZED_BITS; width++) {
        for (size_t flavor = 0; flavor < (unsigned_only ? 1u : 2u);
             flavor++) {
            ir_scalar_t scalar = {
                .class = flavor ? IR_CLASS_SINT : IR_CLASS_UINT,
                .width = width,
            };
            if (!n00bb_literal_fits_scalar(literal.mag, negative, &scalar))
                continue;
            th_type *candidate = th_bitint(state->types, width, flavor != 0);
            if (!candidate) return false;
            alternatives[count++] = candidate;
        }
    }
    size_t values = count;
    for (size_t i = 0; option_alternatives && i < values; i++) {
        th_type *candidate = th_option(state->types, alternatives[i]);
        if (!candidate) return false;
        alternatives[count++] = candidate;
    }
    return th_var_constrain(state->types, type,
        (th_constraint){.kind = TH_CON_ONE_OF,
            .one_of = {.types = alternatives, .n = count}});
}

bool
n00bb_literal_adopts_open_type(parse_grammar_t *grammar,
                               n00bb_tu_state_t *state, parse_tree_t *node,
                               th_type *type)
{
    parse_tree_t *origin = node
        ? parse_tree_integer_literal_origin(node) : nullptr;
    if (!origin || n00bb_unsigned_literal_origin(origin)
        || !n00bb_constrain_literal_fit(grammar, state, origin, node, type,
                                        false, false)) return false;
    for (parse_tree_t *current = origin; current;
         current = parse_tree_parent(current)) {
        parse_tree_set_type(current, type);
        if (current == node) break;
    }
    return true;
}

bool
n00bb_constrain_integer_fit(n00bb_tu_state_t *state, th_type *type,
                            th_type *source, bool range_known,
                            ir_interval_t range)
{
    const ir_profile_t *profile = n00bb_type_profile();
    if (!state || !type || th_kind_of(th_resolve(type)) != TH_VAR)
        return false;
    size_t scalar_count = profile->n_scalars;
    size_t capacity = scalar_count + 2 * N00BB_MAX_SIZED_BITS;
    th_type **alternatives = core_arena_alloc(&state->semantic_values,
        capacity * sizeof(*alternatives));
    if (!alternatives) return false;
    size_t count = 0;
    for (size_t i = 0; i < scalar_count; i++) {
        if (!n00bb_literal_candidate_scalar(profile, i)) continue;
        th_type *candidate = th_name(state->types,
                                     profile->scalars[i].type_text);
        if (candidate
            && (n00bb_integer_type_promotes_to(source, candidate)
                || (range_known
                    && n00bb_integer_range_fits_type(candidate, range))))
            alternatives[count++] = candidate;
    }
    for (uint64_t width = 1; width <= N00BB_MAX_SIZED_BITS; width++) {
        for (size_t flavor = 0; flavor < 2; flavor++) {
            th_type *candidate = th_bitint(state->types, width,
                                           flavor != 0);
            if (!candidate) return false;
            if (n00bb_integer_type_promotes_to(source, candidate)
                || (range_known
                    && n00bb_integer_range_fits_type(candidate, range)))
                alternatives[count++] = candidate;
        }
    }
    return th_var_constrain(state->types, type,
        (th_constraint){.kind = TH_CON_ONE_OF,
            .one_of = {.types = alternatives, .n = count}});
}

static bool
folded_compound_origin(parse_tree_t *node)
{
    return node && !core_tree_is_leaf(node)
        && parse_tree_integer_literal_origin(node) == node
        && core_tree_num_children(node) == 3
        && !core_tree_is_leaf(core_tree_child(node, 1))
        && parse_tree_integer_literal_origin(core_tree_child(node, 0))
        && parse_tree_integer_literal_origin(core_tree_child(node, 2));
}

static bool
folded_operator_is_bitwise(parse_tree_t *node)
{
    core_string_t op = first_token_text(core_tree_child(node, 1));
    return core_string_eq(op, CORE_STRING_STATIC("&"))
        || core_string_eq(op, CORE_STRING_STATIC("|"))
        || core_string_eq(op, CORE_STRING_STATIC("^"))
        || core_string_eq(op, CORE_STRING_STATIC("<<"))
        || core_string_eq(op, CORE_STRING_STATIC(">>"));
}

bool
n00bb_unsigned_literal_origin(parse_tree_t *origin)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(origin);
    return facts && facts->unsigned_literal;
}

static bool
mark_unsigned_literal(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                      parse_tree_t *node, bool unsigned_literal)
{
    if (!unsigned_literal) return true;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->unsigned_literal = true;
    th_type *type = parse_tree_type(node);
    return !type || th_kind_of(th_resolve(type)) != TH_VAR
        || n00bb_constrain_literal_fit(grammar, state, node, node, type,
                                       true, false);
}

static void
type_folded_operands(n00bb_tu_state_t *state, parse_tree_t *node,
                     th_type *target)
{
    if (!folded_compound_origin(node)) return;
    core_string_t op = first_token_text(core_tree_child(node, 1));
    bool shift = core_string_eq(op, CORE_STRING_STATIC("<<"))
        || core_string_eq(op, CORE_STRING_STATIC(">>"));
    for (size_t side = 0; side < 2; side++) {
        parse_tree_t *operand = core_tree_child(node, side ? 2 : 0);
        parse_tree_t *origin = parse_tree_integer_literal_origin(operand);
        th_type *type = shift && side
            ? n00bb_shift_count_type(state->types, target) : target;
        if (!origin || !type) continue;
        for (parse_tree_t *at = origin; at;
             at = parse_tree_parent(at)) {
            parse_tree_set_type(at, type);
            if (at == operand) break;
        }
        type_folded_operands(state, origin, type);
    }
}

static bool
signature_variable(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                   parse_tree_t *node, th_type *type)
{
    parse_sym_entry_t *function = nullptr;
    for (parse_tree_t *at = parse_tree_parent(node); at && !function;
         at = parse_tree_parent(at))
        if (NT_IS(grammar, at, "func-def")) function = parse_tree_sym(at);
    th_type *signature = function ? parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.body-function-signature")) : nullptr;
    if (!signature && function) signature = function->type;
    th_type *resolved = th_resolve(type);
    return signature && resolved && th_kind_of(resolved) == TH_VAR
        && th_occurs(state->types, resolved, signature);
}

static bool
unify_literal_operand(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                      parse_tree_t *node, core_string_t trait,
                      core_string_t method)
{
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(method);
    bool comparison = core_string_eq(trait, CORE_STRING_STATIC("Equal"))
        || core_string_eq(trait, CORE_STRING_STATIC("Comparable"));
    bool shift = operator && operator->shift;
    bool assignment = NT_IS(grammar, node, "binop-assign-stmt");
    for (size_t side = 0; side < 2; side++) {
        parse_tree_t *operand = core_tree_child(node, side ? 2 : 0);
        parse_tree_t *other = core_tree_child(node, side ? 0 : 2);
        parse_tree_t *origin = parse_tree_integer_literal_origin(operand);
        if (!origin || parse_tree_integer_literal_origin(other))
            continue;
        th_type *other_type = parse_tree_type(other);
        bool unsigned_only = operator && operator->unsigned_only
            && !(shift && side == 1);
        if (!other_type || th_kind_of(th_resolve(other_type)) != TH_VAR) {
            if (unsigned_only && other_type)
                (void)n00bb_integer_literal_joins_type(state, operand,
                                                       other_type);
            continue;
        }
        if ((shift && side == 1) || n00bb_open_integer(other_type)
            || !signature_variable(grammar, state, node, other_type))
            continue;
        th_type *literal_type = shift ? th_var(state->types)
                                      : th_resolve(other_type);
        if (!literal_type || !n00bb_constrain_literal_fit(grammar, state,
                origin, operand, literal_type,
                unsigned_only || n00bb_unsigned_literal_origin(origin),
                comparison))
            return false;
        for (parse_tree_t *at = origin; at;
             at = parse_tree_parent(at)) {
            parse_tree_set_type(at, literal_type);
            if (at == operand) break;
        }
        type_folded_operands(state, origin, literal_type);
        if (comparison || assignment || (shift && side == 1)) continue;
        th_type *result = parse_tree_type(node);
        th_type *unified = result
            ? th_unify_in_place(state->types, result, literal_type)
            : literal_type;
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    return true;
}

static parse_tree_t *
untyped_const_initializer(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    parse_tree_t *declaration = symbol && symbol->kind == PARSE_SYM_VARIABLE
        ? symbol->decl_node : nullptr;
    parse_tree_t *statement = declaration;
    while (statement && !NT_IS(grammar, statement, "variable-decl"))
        statement = parse_tree_parent(statement);
    parse_tree_t *prefix = CHILD_NT(grammar, statement, "declaration-prefix");
    bool constant = core_string_eq(first_token_text(CHILD_NT(grammar, prefix,
        "binding-kind")), CORE_STRING_STATIC("const"));
    parse_tree_t *initializer = constant
            && NT_IS(grammar, declaration, "sym-decl")
            && !CHILD_NT(grammar, declaration, "type-spec")
        ? CHILD_NT(grammar, declaration, "expression") : nullptr;
    return initializer && parse_tree_integer_literal_origin(initializer)
        ? initializer : nullptr;
}

bool
n00bb_untyped_integer_const(parse_grammar_t *grammar,
                            parse_sym_entry_t *symbol,
                            int64_t *minimum, int64_t *maximum)
{
    const n00bb_untyped_integer_t *restored = symbol
        ? parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.untyped-integer"))
        : nullptr;
    if (restored) {
        *minimum = *maximum = restored->value;
        return true;
    }
    parse_tree_t *initializer = untyped_const_initializer(grammar, symbol);
    int64_t value = 0;
    if (!initializer || !n00bb_integer_point(initializer, &value))
        return false;
    *minimum = *maximum = value;
    return true;
}

bool
n00bb_untyped_integer_const_unsigned(parse_grammar_t *grammar,
                                     parse_sym_entry_t *symbol)
{
    const n00bb_untyped_integer_t *restored = symbol
        ? parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.untyped-integer"))
        : nullptr;
    if (restored) return restored->is_unsigned;
    parse_tree_t *initializer = untyped_const_initializer(grammar, symbol);
    return initializer && n00bb_unsigned_literal_origin(
        parse_tree_integer_literal_origin(initializer));
}

static parse_sym_entry_t *
integer_constant_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_node_resolution_t resolution = node && !core_tree_is_leaf(node)
        ? parse_tree_resolution(node) : (parse_node_resolution_t){0};
    return (NT_IS(grammar, node, "primary")
            || (NT_IS(grammar, node, "postfix-expr")
                && dotted_symbol_path_node(grammar, node)))
        && resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
}

bool
n00bb_integer_constant_reference(parse_grammar_t *grammar, parse_tree_t *node,
                                 int64_t *value)
{
    parse_sym_entry_t *symbol = integer_constant_symbol(grammar, node);
    return symbol && n00bb_untyped_integer_const(grammar, symbol, value,
                                                 value);
}

static bool
unsigned_integer_constant(parse_grammar_t *grammar, parse_tree_t *node)
{
    return n00bb_untyped_integer_const_unsigned(grammar,
        integer_constant_symbol(grammar, node));
}

static bool
folded_integer_operand(parse_tree_t *operand, int64_t *value)
{
    return operand && !core_tree_is_leaf(operand)
        && parse_tree_integer_literal_origin(operand)
        && n00bb_integer_point(operand, value);
}

static bool
fold_integer_operation(core_string_t op, int64_t left, int64_t right,
                       int64_t *value)
{
    if (core_string_eq(op, CORE_STRING_STATIC("+")))
        return !__builtin_add_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("-")))
        return !__builtin_sub_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("*")))
        return !__builtin_mul_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("/"))
        || core_string_eq(op, CORE_STRING_STATIC("%"))) {
        if (!right || (left == INT64_MIN && right == -1)) return false;
        *value = core_string_eq(op, CORE_STRING_STATIC("/"))
            ? left / right : left % right;
        return true;
    }
    if (left < 0 || right < 0) return false;
    if (core_string_eq(op, CORE_STRING_STATIC("<<"))) {
        if (right >= 63 || left > (INT64_MAX >> right)) return false;
        *value = left << right;
        return true;
    }
    if (core_string_eq(op, CORE_STRING_STATIC(">>"))) {
        *value = right >= 63 ? 0 : left >> right;
        return true;
    }
    if (core_string_eq(op, CORE_STRING_STATIC("&"))) *value = left & right;
    else if (core_string_eq(op, CORE_STRING_STATIC("|")))
        *value = left | right;
    else if (core_string_eq(op, CORE_STRING_STATIC("^")))
        *value = left ^ right;
    else return false;
    return true;
}

static bool
folded_integer_expression_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const core_string_t kinds[] = {
        CORE_STRING_STATIC("add-expr"), CORE_STRING_STATIC("mul-expr"),
        CORE_STRING_STATIC("shift-expr"), CORE_STRING_STATIC("bitand-expr"),
        CORE_STRING_STATIC("bitxor-expr"), CORE_STRING_STATIC("bitor-expr"),
    };
    if (!node || core_tree_is_leaf(node) || core_tree_num_children(node) != 3)
        return false;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (nt_is(grammar, node, kinds[i])) return true;
    return false;
}

static bool
fold_integer_literal_expression(parse_grammar_t *grammar, parse_tree_t *node,
                                int64_t *value)
{
    int64_t left = 0, right = 0;
    return folded_integer_expression_node(grammar, node)
        && folded_integer_operand(core_tree_child(node, 0), &left)
        && folded_integer_operand(core_tree_child(node, 2), &right)
        && fold_integer_operation(first_token_text(core_tree_child(node, 1)),
                                  left, right, value);
}

bool
n00bb_folded_integer_expression(parse_grammar_t *grammar, parse_tree_t *node,
                                int64_t *value)
{
    return folded_integer_expression_node(grammar, node)
        && parse_tree_integer_literal_origin(node) == node
        && n00bb_integer_point(node, value);
}

static bool
constant_integer_operand(parse_grammar_t *grammar, parse_tree_t *operand,
                         int64_t *value)
{
    return folded_integer_operand(operand, value)
        || n00bb_integer_constant_reference(grammar, operand, value);
}

static bool
report_zero_divisor(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                    parse_tree_t *node)
{
    if (core_tree_num_children(node) != 3) return false;
    core_string_t op = first_token_text(core_tree_child(node, 1));
    bool divide = core_string_eq(op, CORE_STRING_STATIC("/"))
        || core_string_eq(op, CORE_STRING_STATIC("/="));
    bool remainder = core_string_eq(op, CORE_STRING_STATIC("%"))
        || core_string_eq(op, CORE_STRING_STATIC("%="));
    int64_t divisor = 1;
    if ((!divide && !remainder)
        || (!NT_IS(grammar, node, "mul-expr")
            && !NT_IS(grammar, node, "binop-assign-stmt"))
        || !constant_integer_operand(grammar, core_tree_child(node, 2),
                                     &divisor)
        || divisor) return false;
    core_diagnostics_report_at(core_translation_unit_diagnostics(state->unit),
        state->unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0043"),
        divide ? CORE_STRING_STATIC("division by zero")
               : CORE_STRING_STATIC("remainder by zero"),
        link_source_location(node));
    return true;
}

static bool
fold_wide_integer_operation(core_string_t op, __int128 left, __int128 right,
                            __int128 *value)
{
    if (core_string_eq(op, CORE_STRING_STATIC("+")))
        return !__builtin_add_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("-")))
        return !__builtin_sub_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("*")))
        return !__builtin_mul_overflow(left, right, value);
    if (core_string_eq(op, CORE_STRING_STATIC("/"))
        || core_string_eq(op, CORE_STRING_STATIC("%"))) {
        if (!right) return false;
        *value = core_string_eq(op, CORE_STRING_STATIC("/"))
            ? left / right : left % right;
        return true;
    }
    if (core_string_eq(op, CORE_STRING_STATIC("<<"))) {
        if (left < 0 || right < 0 || right >= 126
            || left > ((__int128)1 << (126 - right))) return false;
        *value = left << right;
        return true;
    }
    return false;
}

static bool
report_constant_overflow(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                         parse_tree_t *node)
{
    int64_t left = 0, right = 0;
    __int128 value = 0;
    if (!folded_integer_expression_node(grammar, node)
        || !folded_integer_operand(core_tree_child(node, 0), &left)
        || !folded_integer_operand(core_tree_child(node, 2), &right)
        || !fold_wide_integer_operation(
            first_token_text(core_tree_child(node, 1)), left, right, &value))
        return false;
    th_type *target = n00bb_integer_literal_context(grammar, state, node);
    ir_interval_t bounds = {0};
    if (!target || !is_integer_type(target)
        || !n00bb_type_interval(target, &bounds)
        || ir_interval_contains(bounds, ir_interval_point(value)))
        return false;
    n00bb_report_integer_misfit(state, node, CORE_STRING_STATIC("constant"),
                                ir_interval_point(value), target);
    return true;
}

bool
n00bb_cache_contextual_integer(parse_grammar_t *grammar,
                               n00bb_tu_state_t *state, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    if (NT_IS(grammar, node, "binop-assign-stmt")) {
        if (report_zero_divisor(grammar, state, node)) return true;
        return !binary_operator_dispatch_for_node(grammar, node, &trait,
                                                  &method)
            || unify_literal_operand(grammar, state, node, trait, method);
    }
    if (!n00bb_integer_range_wrapper(grammar, node)
        && !NT_IS(grammar, node, "simple-lit")) return true;
    if (report_zero_divisor(grammar, state, node)
        || report_constant_overflow(grammar, state, node)) return true;
    if (NT_IS(grammar, node, "simple-lit")) {
        ir_lit_t literal = {0};
        if (n00bb_parse_switch_integer(first_token_text(node), &literal))
            parse_tree_set_integer_literal_origin(node, node);
        return true;
    }
    int64_t constant = 0;
    if (n00bb_integer_constant_reference(grammar, node, &constant)) {
        parse_tree_set_integer_literal_origin(node, node);
        return mark_unsigned_literal(grammar, state, node,
                                     unsigned_integer_constant(grammar, node));
    }
    if (fold_integer_literal_expression(grammar, node, &constant)) {
        parse_tree_set_integer_literal_origin(node, node);
        bool shift = NT_IS(grammar, node, "shift-expr");
        bool unsigned_literal = folded_operator_is_bitwise(node)
            || n00bb_unsigned_literal_origin(
                parse_tree_integer_literal_origin(core_tree_child(node, 0)))
            || (!shift && n00bb_unsigned_literal_origin(
                parse_tree_integer_literal_origin(core_tree_child(node, 2))));
        th_type *operand = parse_tree_type(core_tree_child(node, 0));
        operand = operand ? th_resolve(operand) : nullptr;
        int64_t count = 0;
        bool count_outside = shift
            && folded_integer_operand(core_tree_child(node, 2), &count)
            && (uint64_t)count >= n00bb_shift_receiver_width(operand);
        if (operand && is_integer_type(operand) && !count_outside
            && !n00bb_integer_range_fits_type(operand,
                                              ir_interval_point(constant))
            && n00bb_integer_literal_context(grammar, state, node)
            && n00bb_constant_misfits_type(state, node, operand))
            return true;
        return mark_unsigned_literal(grammar, state, node, unsigned_literal);
    }
    if (core_tree_num_children(node) == 3
        && binary_operator_dispatch_for_node(grammar, node, &trait, &method)
        && !unify_literal_operand(grammar, state, node, trait, method))
        return false;
    parse_tree_t *origin = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) {
            core_string_t text = parse_tree_leaf_text(child);
            if (core_string_eq(text, CORE_STRING_STATIC("+"))
                || core_string_eq(text, CORE_STRING_STATIC("-"))
                || core_string_eq(text, CORE_STRING_STATIC("("))
                || core_string_eq(text, CORE_STRING_STATIC(")")))
                continue;
            return true;
        }
        parse_tree_t *candidate = parse_tree_integer_literal_origin(child);
        if (!candidate || origin) return true;
        origin = candidate;
    }
    parse_tree_set_integer_literal_origin(node, origin);
    th_type *type = parse_tree_type(node)
        ? th_resolve(parse_tree_type(node)) : nullptr;
    if (origin && NT_IS(grammar, node, "expression")
        && type && th_kind_of(type) == TH_VAR
        && n00bb_contextual_method_argument(grammar, node))
        return n00bb_constrain_literal_fit(grammar, state, origin, node, type,
                                           false, false);

    return true;
}

static parse_tree_t *
value_construct_of_result(parse_grammar_t *grammar, parse_tree_t *value_stmt)
{
    for (parse_tree_t *at = parse_tree_parent(value_stmt); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "if-value-expr")
            || NT_IS(grammar, at, "try-value-expr")
            || NT_IS(grammar, at, "typecase-value-expr")) return at;
        if (NT_IS(grammar, at, "body-stmt")) return nullptr;
    }
    return nullptr;
}

th_type *
n00bb_integer_literal_context(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state, parse_tree_t *literal)
{
    parse_tree_t *child = literal;
    for (parse_tree_t *parent = parse_tree_parent(child);
         parent;
         child = parent, parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "value-stmt")) {
            parent = value_construct_of_result(grammar, parent);
            if (!parent) break;
            continue;
        }
        if (NT_IS(grammar, parent, "sym-decl")) {
            parse_tree_t *spec = CHILD_NT(grammar, parent, "type-spec");
            return spec ? parse_tree_type(spec) : nullptr;
        }
        if (NT_IS(grammar, parent, "assign-stmt"))
            return parse_tree_type(core_tree_child(parent, 0));
        if (NT_IS(grammar, parent, "binop-assign-stmt")
            && child == core_tree_child(parent, 2)) {
            const n00bb_binary_operator_t *op = n00bb_binary_operator_assigned(
                first_token_text(core_tree_child(parent, 1)));
            if (op && op->shift)
                return n00bb_shift_count_type(state->types,
                    parse_tree_type(core_tree_child(parent, 0)));
            return nullptr;
        }
        if (NT_IS(grammar, parent, "shift-expr")
            && core_tree_num_children(parent) == 3) {
            if (child != core_tree_child(parent, 0))
                return n00bb_shift_count_type(state->types,
                    parse_tree_type(core_tree_child(parent, 0)));
            continue;
        }
        if (NT_IS(grammar, parent, "bitand-expr")
            || NT_IS(grammar, parent, "bitxor-expr")
            || NT_IS(grammar, parent, "bitor-expr")
            || NT_IS(grammar, parent, "add-expr")
            || NT_IS(grammar, parent, "mul-expr"))
            continue;
        if (NT_IS(grammar, parent, "unary-expr")) {
            core_string_t op = first_token_text(parent);
            if (core_string_eq(op, CORE_STRING_STATIC("~"))
                || core_string_eq(op, CORE_STRING_STATIC("-"))
                || core_string_eq(op, CORE_STRING_STATIC("+"))
                || core_tree_num_children(parent) == 1) continue;
            return nullptr;
        }
        if (n00bb_integer_range_wrapper(grammar, parent)
            && (core_tree_num_children(parent) == 1
                || (NT_IS(grammar, parent, "tuple-or-paren")
                    && core_tree_num_children(parent) == 3
                    && child == core_tree_child(parent, 1)))) continue;
        return nullptr;
    }
    return nullptr;
}

bool
binary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_string_t *trait,
                                  core_string_t *method)
{
    if (core_tree_num_children(node) != 3)
        return false;
    if (NT_IS(grammar, node, "equality-expr")
        && NT_IS(grammar, core_tree_child(node, 1), "equality-op")) {
        *trait = CORE_STRING_STATIC("Equal");
        *method = CORE_STRING_STATIC("__equal");
        return true;
    }
    if (NT_IS(grammar, node, "compare-expr")
        && NT_IS(grammar, core_tree_child(node, 1), "compare-op")) {
        *trait = CORE_STRING_STATIC("Comparable");
        *method = CORE_STRING_STATIC("__cmp");
        return true;
    }
    core_string_t spelling = first_token_text(core_tree_child(node, 1));
    const n00bb_binary_operator_t *operator = nullptr;
    if (NT_IS(grammar, node, "binop-assign-stmt"))
        operator = n00bb_binary_operator_assigned(spelling);
    else if (NT_IS(grammar, node, "add-expr")
             || NT_IS(grammar, node, "mul-expr")
             || NT_IS(grammar, node, "shift-expr")
             || NT_IS(grammar, node, "bitand-expr")
             || NT_IS(grammar, node, "bitxor-expr")
             || NT_IS(grammar, node, "bitor-expr"))
        operator = n00bb_binary_operator_spelled(spelling);
    if (operator) {
        *trait = operator->trait;
        *method = operator->method;
        return true;
    }
    return false;
}

bool
unary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                 parse_tree_t *node,
                                 core_string_t *trait,
                                 core_string_t *method)
{
    if (!NT_IS(grammar, node, "unary-expr")
        || core_tree_num_children(node) != 2) return false;
    core_string_t spelling = first_token_text(core_tree_child(node, 0));
    if (core_string_eq(spelling, CORE_STRING_STATIC("-"))) {
        *trait = CORE_STRING_STATIC("Negate");
        *method = CORE_STRING_STATIC("__negate");
        return true;
    }
    if (core_string_eq(n00bb_canonical_logical_spelling(spelling),
                       CORE_STRING_STATIC("!"))) {
        *trait = CORE_STRING_STATIC("Not");
        *method = CORE_STRING_STATIC("__not");
        return true;
    }
    return false;
}

bool
cache_return_source_fact(parse_grammar_t *grammar,
                         parse_tree_t *node, n00bb_tu_state_t *state)
{
    static const core_string_t wrappers[] = {
        CORE_STRING_STATIC("expression"), CORE_STRING_STATIC("or-expr"),
        CORE_STRING_STATIC("and-expr"), CORE_STRING_STATIC("or-rhs"),
        CORE_STRING_STATIC("and-rhs"), CORE_STRING_STATIC("equality-expr"),
        CORE_STRING_STATIC("compare-expr"), CORE_STRING_STATIC("bitor-expr"),
        CORE_STRING_STATIC("bitxor-expr"), CORE_STRING_STATIC("bitand-expr"),
        CORE_STRING_STATIC("shift-expr"), CORE_STRING_STATIC("add-expr"),
        CORE_STRING_STATIC("mul-expr"), CORE_STRING_STATIC("unary-expr"),
        CORE_STRING_STATIC("postfix-expr"), CORE_STRING_STATIC("primary"),
        CORE_STRING_STATIC("tuple-or-paren"),
    };
    bool expression_node = false;
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
        expression_node |= nt_is(grammar, node, wrappers[i]);
    if (!state) return true;
    core_string_t dispatch_trait = {0}, dispatch_method = {0};
    bool slice_node = index_dispatch_for_node(grammar, node,
        &dispatch_trait, &dispatch_method)
        && core_string_eq(dispatch_trait, CORE_STRING_STATIC("Index_Slice"));
    if (!expression_node && !slice_node) return true;
    bool cast_source = NT_IS(grammar, node, "unary-expr")
        && core_tree_num_children(node) == 5
        && (parse_tree_leaf_is(core_tree_child(node, 0),
                               CORE_STRING_STATIC("cast"))
            || parse_tree_leaf_is(core_tree_child(node, 0),
                                  CORE_STRING_STATIC("unsafe_cast")));

    n00bb_return_source_t source = {0};
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    parse_sym_entry_t *symbol = resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : parse_tree_sym(node);
    if (slice_node)
        source = (n00bb_return_source_t){
            .kind = N00BB_RETURN_SOURCE_SLICE,
            .node = node,
        };
    else if (NT_IS(grammar, node, "postfix-expr")
             && core_tree_num_children(node) >= 3
             && (parse_tree_leaf_is(core_tree_child(node, 1),
                                   CORE_STRING_STATIC("."))
                 || (parse_tree_is_index_address_access(node)
                     && parse_tree_leaf_is(core_tree_child(node, 1),
                                          CORE_STRING_STATIC("["))))) {
        parse_tree_t *receiver = core_tree_child(node, 0);
        n00bb_return_source_t *base = n00bb_node_return_source(receiver);
        if (base && (base->kind == N00BB_RETURN_SOURCE_SYMBOL
                     || base->kind == N00BB_RETURN_SOURCE_CALL
                     || base->kind == N00BB_RETURN_SOURCE_PROJECTION))
            source = (n00bb_return_source_t){
                .kind = N00BB_RETURN_SOURCE_PROJECTION,
                .node = base->kind == N00BB_RETURN_SOURCE_PROJECTION
                    ? base->node : receiver,
            };
    }
    else if (symbol && !symbol->is_field
             && (symbol->kind == PARSE_SYM_VARIABLE
                   || symbol->kind == PARSE_SYM_PARAM)) {
        source = (n00bb_return_source_t){
            .kind = N00BB_RETURN_SOURCE_SYMBOL,
            .node = node,
            .symbol = symbol,
        };
    }
    else {
        const parse_rule_t *rule = parse_tree_rule(grammar, node);
        if (!cast_source && rule
            && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL))
            source = (n00bb_return_source_t){
                .kind = N00BB_RETURN_SOURCE_CALL,
                .node = node,
            };
    }

    bool transparent_unwrap = NT_IS(grammar, node, "postfix-expr")
        && core_tree_num_children(node) == 2
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC("!"));
    if (source.kind == N00BB_RETURN_SOURCE_NONE
        && NT_IS(grammar, node, "unary-expr")
        && core_string_eq(first_token_text(node), CORE_STRING_STATIC("ref"))) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *operand = core_tree_child(node, i);
            if (core_tree_is_leaf(operand)) continue;
            n00bb_return_source_t *field = n00bb_node_return_source(operand);
            if (field && field->kind == N00BB_RETURN_SOURCE_PROJECTION) {
                source = (n00bb_return_source_t){
                    .kind = N00BB_RETURN_SOURCE_PROJECTION_REFERENCE,
                    .node = field->node,
                };
            }
            break;
        }
    }
    if (source.kind == N00BB_RETURN_SOURCE_NONE
        && (core_tree_num_children(node) == 1 || transparent_unwrap
            || cast_source)) {
        n00bb_return_source_t *child = n00bb_node_return_source(
            core_tree_child(node, cast_source ? 4 : 0));
        if (child) {
            n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
            if (!facts) return false;
            facts->return_source = child;
            return true;
        }
    }
    if (source.kind == N00BB_RETURN_SOURCE_NONE) return true;
    n00bb_return_source_t *stored = core_arena_alloc(
        &state->semantic_values, sizeof(*stored));
    if (!stored) return false;
    *stored = source;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->return_source = stored;
    return true;
}

bool
cache_alias_edges(parse_grammar_t *grammar, parse_tree_t *node,
                  n00bb_tu_state_t *state)
{
    if (NT_IS(grammar, node, "typeof-expr")) return true;
    n00bb_alias_edge_t single = {0};
    bool direct = false;
    n00bb_return_source_t *source = n00bb_node_return_source(node);
    if (source && source->kind == N00BB_RETURN_SOURCE_PROJECTION)
        return true;
    if (NT_IS(grammar, node, "unary-expr")
        && core_string_eq(first_token_text(node), CORE_STRING_STATIC("ref"))) {
        single.kind = N00BB_ALIAS_REFERENCE;
        single.node = node;
        single.symbol = direct_ref_target(grammar, node);
        direct = true;
    }
    else if (source && source->node == node) {
        if (source->kind == N00BB_RETURN_SOURCE_SYMBOL) {
            single.kind = N00BB_ALIAS_SYMBOL;
            single.symbol = source->symbol;
            direct = true;
        }
        else if (source->kind == N00BB_RETURN_SOURCE_CALL
                 || source->kind == N00BB_RETURN_SOURCE_SLICE) {
            single.kind = N00BB_ALIAS_CALL;
            direct = true;
        }
        single.node = node;
    }
    if (direct) {
        parse_token_info_t *token = parse_tree_find_first_terminal(node);
        single.derived_at = token ? parse_token_start_byte(token) : 0;
        n00bb_alias_edges_t *result = core_arena_alloc(
            &state->semantic_values, sizeof(*result));
        n00bb_alias_edge_t *edge = core_arena_alloc(
            &state->semantic_values, sizeof(*edge));
        if (!result || !edge) return false;
        *edge = single;
        *result = (n00bb_alias_edges_t){ .edges = edge, .count = 1 };
        n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
        if (!facts) return false;
        facts->alias_edges = result;
        return true;
    }

    size_t count = 0;
    n00bb_alias_edges_t *only_child = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        n00bb_alias_edges_t *child = n00bb_node_alias_edges(
            core_tree_child(node, i));
        if (!child) continue;
        count += child->count;
        if (count == child->count) only_child = child;
        else only_child = nullptr;
    }
    if (!count) return true;
    if (only_child) {
        n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
        if (!facts) return false;
        facts->alias_edges = only_child;
        return true;
    }
    n00bb_alias_edges_t *result = core_arena_alloc(
        &state->semantic_values, sizeof(*result));
    n00bb_alias_edge_t *edges = core_arena_alloc(
        &state->semantic_values, count * sizeof(*edges));
    if (!result || !edges) return false;
    size_t at = 0;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        n00bb_alias_edges_t *child = n00bb_node_alias_edges(
            core_tree_child(node, i));
        if (!child) continue;
        memcpy(edges + at, child->edges, child->count * sizeof(*edges));
        at += child->count;
    }
    *result = (n00bb_alias_edges_t){ .edges = edges, .count = count };
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->alias_edges = result;
    return true;
}

parse_tree_t *
member_access_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (NT_IS(grammar, node, "postfix-expr")
        && core_tree_num_children(node) >= 3
        && core_tree_is_leaf(core_tree_child(node, 1))
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC(".")))
        return node;
    if (core_tree_num_children(node) == 1)
        return member_access_node(grammar, core_tree_child(node, 0));
    return nullptr;
}

static parse_tree_t *
index_assignment_statement(parse_grammar_t *grammar, parse_tree_t *node,
                           bool *through_receiver)
{
    *through_receiver = false;
    parse_tree_t *at = node;
    for (parse_tree_t *parent = parse_tree_parent(at); parent;
         at = parent, parent = parse_tree_parent(at)) {
        if ((NT_IS(grammar, parent, "assign-stmt")
             || NT_IS(grammar, parent, "binop-assign-stmt"))
            && core_tree_child(parent, 0) == at) return parent;
        if (NT_IS(grammar, parent, "postfix-expr")
            && core_tree_child(parent, 0) == at)
            *through_receiver = true;
        else if (core_tree_num_children(parent) != 1) return nullptr;
    }
    return nullptr;
}

bool
index_dispatch_for_node(parse_grammar_t *grammar, parse_tree_t *node,
                        core_string_t *trait, core_string_t *method)
{
    if (!NT_IS(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 4
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("["))) return false;
    if (NT_IS(grammar, core_tree_child(node, 2), "slice-contents")) {
        *trait = CORE_STRING_STATIC("Index_Slice");
        *method = CORE_STRING_STATIC("__slice");
    }
    else {
        *trait = CORE_STRING_STATIC("Index_Read");
        *method = CORE_STRING_STATIC("__read");
    }

    bool through_receiver = false;
    parse_tree_t *statement = index_assignment_statement(grammar, node,
                                                         &through_receiver);
    if (!through_receiver && NT_IS(grammar, statement, "assign-stmt")) {
        bool slice_write = NT_IS(grammar, core_tree_child(node, 2),
                                 "slice-contents");
        *trait = slice_write ? CORE_STRING_STATIC("Index_Slice_Write")
                             : CORE_STRING_STATIC("Index_Write");
        *method = slice_write ? CORE_STRING_STATIC("__write_slice")
                              : CORE_STRING_STATIC("__write");
        return true;
    }
    return true;
}

parse_tree_t *
n00bb_index_stored_value(parse_grammar_t *grammar, parse_tree_t *node)
{
    bool through_receiver = false;
    parse_tree_t *statement = index_assignment_statement(grammar, node,
                                                         &through_receiver);
    if (through_receiver || !NT_IS(grammar, statement, "assign-stmt")
        || core_tree_num_children(statement) < 3
        || !core_string_eq(first_token_text(core_tree_child(statement, 1)),
                           CORE_STRING_STATIC("="))) return nullptr;
    const parse_rule_t *rule = parse_tree_rule(grammar, statement);
    parse_annotation_t *assign = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS) : nullptr;
    return assign && parse_child_ref_is_set(assign->value_ref)
        ? parse_tree_resolve_child_ref(grammar, statement, assign->value_ref)
        : nullptr;
}

bool
n00bb_index_reference_storage(parse_grammar_t *grammar, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    if (!index_dispatch_for_node(grammar, node, &trait, &method)
        || core_string_eq(trait, CORE_STRING_STATIC("Index_Slice")))
        return false;
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        parse_tree_t *parent = parse_tree_parent(at);
        if (!parent) break;
        if (NT_IS(grammar, parent, "index-contents")
            || NT_IS(grammar, parent, "slice-contents")) return false;
        if (NT_IS(grammar, parent, "postfix-expr")
            && core_tree_child(parent, 0) != at) return false;
        if (!NT_IS(grammar, parent, "unary-expr")) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        parse_annotation_t *operator = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_OPERATOR)
            : nullptr;
        if (!operator && core_tree_num_children(parent) == 1) continue;
        return operator && core_string_eq(operator->op_kind,
            CORE_STRING_STATIC("ref"));
    }
    return false;
}

parse_tree_t *
n00bb_index_expression_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        core_string_t trait = {0}, method = {0};
        if (index_dispatch_for_node(grammar, node, &trait, &method)
            && !core_string_eq(trait, CORE_STRING_STATIC("Index_Slice")))
            return node;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

parse_tree_t *
n00bb_addressable_root_lvalue(parse_grammar_t *grammar,
                              parse_tree_t *target)
{
    for (;;) {
        parse_tree_t *member = member_access_node(grammar, target);
        if (member) {
            target = core_tree_num_children(member)
                ? core_tree_child(member, 0) : nullptr;
            continue;
        }
        parse_tree_t *index = n00bb_index_expression_node(grammar, target);
        if (index) {
            target = core_tree_num_children(index)
                ? core_tree_child(index, 0) : nullptr;
            continue;
        }
        break;
    }
    return n00bb_simple_lvalue(target);
}

typedef struct {
    bool constants;
    parse_sym_entry_t *in_parameter;
    core_list_t(parse_sym_entry_ptr_t) visited;
} origin_walk_t;

static parse_sym_entry_t *
view_origin_of_expression(parse_grammar_t *grammar,
                          core_translation_unit_t *unit, parse_tree_t *node,
                          origin_walk_t *walk);

static parse_sym_entry_t *
view_origin_of_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                      parse_sym_entry_t *symbol, origin_walk_t *walk)
{
    if (!symbol) return nullptr;
    for (size_t i = 0; i < walk->visited.len; i++)
        if (walk->visited.data[i] == symbol) return nullptr;
    core_list_push(walk->visited, symbol);
    if (symbol->kind == PARSE_SYM_PARAM) {
        if (symbol->has_mode && symbol->mode == TH_VIEW) return symbol;
        if (!walk->in_parameter && (!symbol->has_mode
                                    || symbol->mode == TH_IN))
            walk->in_parameter = symbol;
        return nullptr;
    }
    if (symbol->kind != PARSE_SYM_VARIABLE) return nullptr;
    if (walk->constants && symbol->mutability == PARSE_SYM_CONST)
        return symbol;
    if (!walk->constants && !scope_is_within_function(symbol->scope))
        return nullptr;
    for (size_t i = 0; i < symbol->definition_count; i++) {
        parse_tree_t *definition = symbol->definition_nodes[i];
        parse_tree_t *value = NT_IS(grammar, definition, "sym-decl")
            ? CHILD_NT(grammar, definition, "expression")
            : NT_IS(grammar, definition, "assign-stmt")
                && core_tree_num_children(definition) == 3
            ? core_tree_child(definition, 2) : nullptr;
        parse_sym_entry_t *origin = value
            ? view_origin_of_expression(grammar, unit, value, walk)
            : nullptr;
        if (origin) return origin;
    }
    return nullptr;
}

static parse_sym_entry_t *
origin_root_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_tree_t *node)
{
    parse_tree_t *reference = direct_ref_expression(grammar, node);
    if (reference) {
        parse_tree_t *operand = nullptr;
        for (size_t i = 0; i < core_tree_num_children(reference); i++)
            if (!core_tree_is_leaf(core_tree_child(reference, i)))
                operand = core_tree_child(reference, i);
        node = operand;
    }
    parse_tree_t *root = node ? n00bb_addressable_root_lvalue(grammar, node)
                              : nullptr;
    return root ? attached_symbol(grammar, unit, root) : nullptr;
}

static parse_sym_entry_t *
view_origin_of_expression(parse_grammar_t *grammar,
                          core_translation_unit_t *unit, parse_tree_t *node,
                          origin_walk_t *walk)
{
    return view_origin_of_symbol(grammar, unit,
        origin_root_symbol(grammar, unit, node), walk);
}

typedef struct {
    parse_sym_entry_t *origin;
    parse_sym_entry_t *in_parameter;
} origin_memo_t;

static parse_sym_entry_t *
guarded_origin(parse_grammar_t *grammar, core_translation_unit_t *unit,
               parse_tree_t *node, bool constants,
               parse_sym_entry_t **in_parameter)
{
    parse_sym_entry_t *symbol = origin_root_symbol(grammar, unit, node);
    core_string_t key = constants
        ? CORE_STRING_STATIC("n00bb.frozen-origin")
        : CORE_STRING_STATIC("n00bb.view-origin");
    bool local = symbol && symbol->kind == PARSE_SYM_VARIABLE
        && scope_is_within_function(symbol->scope);
    origin_memo_t *memo = local ? parse_sym_binding(symbol, key) : nullptr;
    origin_memo_t result = memo ? *memo : (origin_memo_t){0};
    if (!memo) {
        origin_walk_t walk = {
            .constants = constants,
            .visited = core_list_new(parse_sym_entry_ptr_t),
        };
        result.origin = view_origin_of_symbol(grammar, unit, symbol, &walk);
        result.in_parameter = walk.in_parameter;
        core_list_free(walk.visited);
        n00bb_tu_state_t *state = local ? tu_state(unit) : nullptr;
        memo = state ? core_arena_alloc(&state->semantic_values,
                                        sizeof(*memo)) : nullptr;
        if (memo) {
            *memo = result;
            if (!parse_sym_set_binding(symbol, key, memo)) memo = nullptr;
        }
    }
    if (in_parameter) *in_parameter = result.in_parameter;
    return result.origin;
}

parse_sym_entry_t *
n00bb_view_origin(parse_grammar_t *grammar, core_translation_unit_t *unit,
                  parse_tree_t *node, parse_sym_entry_t **in_parameter)
{
    return guarded_origin(grammar, unit, node, false, in_parameter);
}

parse_sym_entry_t *
n00bb_frozen_origin(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *node)
{
    return guarded_origin(grammar, unit, node, true, nullptr);
}

parse_sym_entry_t *
direct_ref_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_sym_entry_t *cached = parse_tree_reference_target(node);
    if (cached) return cached;
    if (NT_IS(grammar, node, "unary-expr")
        && core_string_eq(first_token_text(node), CORE_STRING_STATIC("ref"))) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *operand = core_tree_child(node, i);
            parse_tree_t *lvalue = n00bb_addressable_root_lvalue(grammar,
                                                                 operand);
            parse_node_resolution_t resolution = lvalue
                ? parse_tree_resolution(lvalue)
                : (parse_node_resolution_t){0};
            parse_sym_entry_t *symbol = resolution.kind
                    == PARSE_NODE_RESOLUTION_SYMBOL
                ? resolution.target.symbol : nullptr;
            if (symbol && (symbol->kind == PARSE_SYM_VARIABLE
                           || symbol->kind == PARSE_SYM_PARAM))
                return symbol;
        }
        return nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_sym_entry_t *symbol = direct_ref_target(
            grammar, core_tree_child(node, i));
        if (symbol) return symbol;
    }
    return nullptr;
}

static const struct {
    core_string_t source;
    core_string_t canonical;
} N00BB_CANONICAL_TYPE_NAMES[] = {
    {CORE_STRING_STATIC("i64"), CORE_STRING_STATIC("int")},
    {CORE_STRING_STATIC("u64"), CORE_STRING_STATIC("uint")},
    {CORE_STRING_STATIC("float"), CORE_STRING_STATIC("f64")},
    {CORE_STRING_STATIC("List"),
     CORE_STRING_STATIC("builtin.containers.list.List")},
    {CORE_STRING_STATIC("Set"),
     CORE_STRING_STATIC("builtin.containers.set.Set")},
    {CORE_STRING_STATIC("Dict"),
     CORE_STRING_STATIC("builtin.containers.dict.Dict")},
    {CORE_STRING_STATIC("View"),
     CORE_STRING_STATIC("builtin.containers.view.View")},
};

static const struct {
    core_string_t name;
    int arity;
} N00BB_GENERIC_TYPE_ARITIES[] = {
    {CORE_STRING_STATIC("Array"), 1},
    {CORE_STRING_STATIC("Array_Locking"), 1},
    {CORE_STRING_STATIC("List"), 1},
    {CORE_STRING_STATIC("List_Locking"), 1},
    {CORE_STRING_STATIC("Set"), 1},
    {CORE_STRING_STATIC("Set_Locking"), 1},
    {CORE_STRING_STATIC("View"), 1},
    {CORE_STRING_STATIC("Atomic"), 1},
    {CORE_STRING_STATIC("Ref"), 1},
    {CORE_STRING_STATIC("Dict"), 2},
    {CORE_STRING_STATIC("Dict_Locking"), 2},
    {CORE_STRING_STATIC("Foreign"), 1},
    {CORE_STRING_STATIC("Foreign_Fn"), 1},
    {CORE_STRING_STATIC("Tuple"), -2},
};

static const core_string_t N00BB_NAMED_BUILTIN_TYPES[] = {
    CORE_STRING_STATIC("Allocator"), CORE_STRING_STATIC("Any"),
    CORE_STRING_STATIC("byte"), CORE_STRING_STATIC("char"),
    CORE_STRING_STATIC("string"), CORE_STRING_STATIC("typespec"),
};

static int
generic_type_arity_string(core_string_t name)
{
    for (size_t i = 0; i < sizeof(N00BB_GENERIC_TYPE_ARITIES)
                            / sizeof(N00BB_GENERIC_TYPE_ARITIES[0]); i++)
        if (core_string_eq(name, N00BB_GENERIC_TYPE_ARITIES[i].name))
            return N00BB_GENERIC_TYPE_ARITIES[i].arity;
    return -1;
}

static core_string_t
canonical_type_name_entry(core_string_t name)
{
    for (size_t i = 0; i < sizeof(N00BB_CANONICAL_TYPE_NAMES)
                            / sizeof(N00BB_CANONICAL_TYPE_NAMES[0]); i++)
        if (core_string_eq(name, N00BB_CANONICAL_TYPE_NAMES[i].source))
            return N00BB_CANONICAL_TYPE_NAMES[i].canonical;
    return (core_string_t){};
}

core_string_t
canonical_builtin_type_name(core_string_t name)
{
    if (!name.data) return name;
    core_string_t canonical = canonical_type_name_entry(name);
    return canonical.data ? canonical : name;
}

parse_sym_entry_t *
attached_symbol(parse_grammar_t *grammar, core_translation_unit_t *unit,
               parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        return resolution.target.symbol;
    bool simple_name = NT_IS(grammar, node, "primary")
        || (NT_IS(grammar, node, "member-chain")
            && !dotted_symbol_path_node(grammar, node));
    if (resolution.kind == PARSE_NODE_RESOLUTION_SCOPE && !simple_name)
        return nullptr;
    if (resolution.kind != PARSE_NODE_RESOLUTION_SCOPE && parse_tree_sym(node))
        return parse_tree_sym(node);
    if (simple_name) {
        n00bb_tu_state_t *state = tu_state(unit);
        if (state && state->annotations && state->annotations->symtab)
            return parse_symtab_resolve_node(state->annotations->symtab,
                node, CORE_STRING_STATIC(""));
    }
    if (core_tree_num_children(node) == 1)
        return attached_symbol(grammar, unit, core_tree_child(node, 0));
    return nullptr;
}

bool
dotted_symbol_path_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)
        || core_tree_num_children(node) < 3
        || !core_tree_is_leaf(core_tree_child(node, 1))
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("."))) return false;
    return NT_IS(grammar, node, "member-chain")
        || NT_IS(grammar, node, "postfix-expr");
}

bool
is_type_reference_node_at(parse_grammar_t *grammar, parse_tree_t *node,
                          parse_tree_t *parent)
{
    if (!NT_IS(grammar, node, "member-chain")) return false;
    return NT_IS(grammar, parent, "base-tspec-core")
        || NT_IS(grammar, parent, "base-tspec");
}

bool
is_type_reference_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return is_type_reference_node_at(grammar, node, parse_tree_parent(node));
}

bool
inside_array_dimension(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at && !core_tree_is_leaf(at);
         at = parse_tree_parent(at))
        if (NT_IS(grammar, at, "array-dimension")) return true;
    return false;
}

core_string_t
bare_type_reference_name(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!is_type_reference_node(grammar, node)
        || dotted_symbol_path_node(grammar, node)
        || core_tree_num_children(node) != 1) return (core_string_t){0};
    parse_tree_t *identifier = core_tree_child(node, 0);
    return core_tree_is_leaf(identifier) ? first_token_text(identifier)
                                         : (core_string_t){0};
}

bool
bind_array_shape_alias_source(parse_grammar_t *grammar,
                              parse_tree_t *type_reference,
                              parse_sym_entry_t *alias)
{
    if (!type_reference || !alias || !alias->decl_node
        || !NT_IS(grammar, alias->decl_node, "alias-stmt")
        || !alias_declaration_is_transparent(alias->decl_node)) return true;
    for (parse_tree_t *at = type_reference; at;
         at = parse_tree_parent(at)) {
        if (core_tree_is_leaf(at)) continue;
        parse_sym_entry_t *owner = parse_tree_sym(at);
        if (!owner || owner->decl_node != at
            || (owner->kind != PARSE_SYM_VARIABLE
                && owner->kind != PARSE_SYM_PARAM
                && owner->kind != PARSE_SYM_FUNCTION
                && owner->kind != PARSE_SYM_TYPEDEF)) continue;
        parse_sym_entry_t *previous = parse_sym_binding(owner,
            CORE_STRING_STATIC("n00bb.array-shape-alias-source"));
        return (!previous || previous == alias)
            && parse_sym_set_binding(owner,
                CORE_STRING_STATIC("n00bb.array-shape-alias-source"), alias);
    }
    return true;
}

void
bind_declared_type_from_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "sym-decl")) return;
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    const parse_annotation_t *declares = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_DECLARES) : nullptr;
    if (!declares || !parse_child_ref_is_set(declares->type_ref)) return;

    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind != PARSE_NODE_RESOLUTION_SYMBOL
        || !resolution.target.symbol
        || resolution.target.symbol->kind != PARSE_SYM_VARIABLE) return;
    parse_tree_t *type_node = parse_tree_resolve_child_ref(grammar, node,
                                                            declares->type_ref);
    th_type *type = parse_tree_type(type_node);
    if (!type) return;
    resolution.target.symbol->type = type;
    parse_tree_set_type(node, type);
}


bool
builtin_type_name(core_string_t name)
{
    if (!name.data) return false;
    if (generic_type_arity_string(name) != -1) return true;
    for (size_t i = 0; i < sizeof(N00BB_NAMED_BUILTIN_TYPES)
                            / sizeof(N00BB_NAMED_BUILTIN_TYPES[0]); i++)
        if (core_string_eq(name, N00BB_NAMED_BUILTIN_TYPES[i])) return true;
    core_string_t canonical = canonical_type_name_entry(name);
    const ir_profile_t *profile = n00bb_type_profile();
    for (size_t i = 0; i < profile->n_scalars; i++)
        if (core_string_eq(name, profile->scalars[i].type_text)
            || (canonical.data
                && core_string_eq(canonical, profile->scalars[i].type_text)))
            return true;
    return false;
}

bool
is_named_type_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    return is_named_type_declaration(grammar, symbol)
        || (symbol && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-type")))
        || (symbol && symbol->kind == PARSE_SYM_TYPEDEF && !symbol->decl_node
            && parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.export-origin")));
}

bool
is_trait_type_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    return symbol && ((symbol->decl_node
        && NT_IS(grammar, symbol->decl_node, "trait-decl"))
        || ((parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.builtin-type"))
             || (!symbol->decl_node && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.export-origin"))))
            && core_string_eq(symbol->adt_kind,
                CORE_STRING_STATIC("trait"))));
}

static parse_sym_entry_t *
builtin_package_type(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                     core_string_t name)
{
    core_string_t package_name = CORE_STRING_STATIC("builtin");
    core_translation_unit_t *builtin = state->unit
        ? find_direct_import(state->unit, package_name) : nullptr;
    n00bb_package_declaration_t *package = builtin
        ? find_package_declaration(builtin, package_name) : nullptr;
    for (size_t i = 0; package && i < package->exports.len; i++) {
        n00bb_package_export_t *export = &package->exports.data[i];
        if (export->symbol && core_string_eq(export->export_name, name)) {
            parse_sym_entry_t *exported = canonical_symbol(export->symbol);
            if (is_named_type_symbol(grammar, exported)
                && !is_trait_type_symbol(grammar, exported)) return exported;
            continue;
        }
        core_string_t source = export->source_name;
        size_t leaf = source.u8_bytes;
        while (leaf && source.data[leaf - 1] != '.') leaf--;
        if (!leaf || !core_string_eq(export->export_name, name)
            || !core_string_eq(core_string_borrow(source.data + leaf,
                   source.u8_bytes - leaf), name)) continue;
        core_translation_unit_t *module = find_direct_import(builtin,
            core_string_borrow(source.data, leaf - 1));
        n00bb_tu_state_t *module_state = tu_state(module);
        parse_scope_t *types = module_state
            ? type_module_scope(module_state) : nullptr;
        parse_symtab_t *symtab = module_state && module_state->annotations
            ? module_state->annotations->symtab : nullptr;
        parse_sym_entry_t *symbol = types && symtab
            ? parse_symtab_lookup_scoped(symtab, types,
                CORE_STRING_STATIC("type"), name) : nullptr;
        if (is_named_type_symbol(grammar, symbol)
            && !is_trait_type_symbol(grammar, symbol)) return symbol;
    }
    return nullptr;
}

parse_sym_entry_t *
lookup_named_type_reference(parse_grammar_t *grammar,
                           n00bb_tu_state_t *state,
                           parse_symtab_t *symtab, parse_scope_t *scope,
                           core_string_t name)
{
    if (!grammar || !state || !symtab || !name.data) return nullptr;
    parse_sym_entry_t *symbol = scope
        ? parse_symtab_lookup_scoped(symtab, scope,
            CORE_STRING_STATIC("type"), name) : nullptr;
    if (!is_named_type_symbol(grammar, symbol)) {
        parse_scope_t *module_types = type_module_scope(state);
        symbol = module_types
            ? parse_symtab_lookup_scoped(symtab, module_types,
                CORE_STRING_STATIC("type"), name) : nullptr;
    }
    if (!is_named_type_symbol(grammar, symbol))
        symbol = parse_symtab_lookup_scoped(symtab, scope,
            CORE_STRING_STATIC("builtin"), name);
    if (!is_named_type_symbol(grammar, symbol))
        symbol = builtin_package_type(grammar, state, name);
    return is_named_type_symbol(grammar, symbol) ? symbol : nullptr;
}

th_type *
trait_type_reference(parse_grammar_t *grammar,
                     core_translation_unit_t *use_unit,
                     core_translation_unit_t *declaration_unit,
                     parse_sym_entry_t *symbol, th_type *existing)
{
    n00bb_tu_state_t *state = tu_state(use_unit);
    if (!state || !state->types || !declaration_unit
        || !is_trait_type_symbol(grammar, symbol)) return nullptr;
    core_buffer_t qualified = {0};
    if (symbol->decl_node)
        append_module_type_name(declaration_unit, symbol->name, &qualified);
    else core_buffer_printf(&qualified, "%.*s", (int)symbol->name.u8_bytes,
                            symbol->name.data);
    th_type *type = existing ? th_resolve(existing) : nullptr;
    if (!type || th_kind_of(type) != TH_VAR)
        type = qualified.data ? th_var(state->types) : nullptr;
    bool constrained = type && qualified.data;
    for (size_t i = 0; constrained && i < th_var_constraint_count(type); i++) {
        th_constraint constraint;
        if (th_var_constraint_at(type, i, &constraint)
            && constraint.kind == TH_CON_IMPLEMENTS
            && constraint.iface.data
            && core_string_eq(constraint.iface,
                              core_buffer_string(&qualified))) {
            core_buffer_release(&qualified);
            return type;
        }
    }
    core_string_t package = n00bb_package_id(use_unit);
    constrained = constrained && th_var_constrain(state->types, type,
        (th_constraint){.kind = TH_CON_IMPLEMENTS,
                        .iface = core_buffer_string(&qualified),
                        .has_scope = true,
                        .scope_module = use_unit->module.identifier,
                        .scope_package = package.data
                            ? package : CORE_STRING_STATIC("")});
    core_buffer_release(&qualified);
    return constrained ? type : nullptr;
}

void
resolve_local_type_reference(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node, parse_tree_t *parent)
{
    if (!is_type_reference_node_at(grammar, node, parent)
        || dotted_symbol_path_node(grammar, node)) return;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return;

    n00bb_link_use_site_t *site = link_use_site_for_node(state, node);
    parse_node_resolution_t previous = parse_tree_resolution(node);
    parse_scope_t *scope = previous.kind == PARSE_NODE_RESOLUTION_SCOPE
        ? previous.target.scope : site ? site->scope
        : parse_symtab_scope_for_node(symtab, node);
    if (!scope)
        scope = parse_symtab_current_scope(symtab, core_string_empty());
    if (scope) parse_tree_set_scope(node, scope);
    core_string_t name = site ? site->member_name
                              : bare_type_reference_name(grammar, node);
    parse_sym_entry_t *symbol = lookup_named_type_reference(grammar, state,
        symtab, scope, name);
    if (!symbol) {
        if (!name.data) return;
        th_type *pending = parse_tree_type(node);
        if (builtin_type_name(name)) {
            parse_tree_set_type(node, th_name(state->types,
                canonical_builtin_type_name(name)));
        }
        else if (!pending || th_kind_of(th_resolve(pending)) != TH_VAR) {
            parse_tree_set_type(node, th_var(state->types));
        }
        return;
    }
    if (previous.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && previous.target.symbol != symbol)
        parse_sym_remove_use(previous.target.symbol, node);
    if (!parse_tree_set_resolution_symbol(node, symbol)
        || !parse_sym_add_use(symbol, node)
        || !bind_array_shape_type_source(grammar, node, symbol)
        || !bind_array_shape_alias_source(grammar, node, symbol)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0013"),
            CORE_STRING_STATIC("could not bind the type reference node"),
            link_source_location(node));
    }
    if (is_trait_type_symbol(grammar, symbol)) {
        th_type *type = trait_type_reference(grammar, unit, unit, symbol,
                                             nullptr);
        if (type) parse_tree_set_type(node, type);
    }
    else if (symbol && symbol->decl_node
        && NT_IS(grammar, symbol->decl_node, "alias-stmt")
        && !alias_declaration_is_transparent(symbol->decl_node)) {
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) return;
        core_buffer_t qualified = {0};
        append_module_type_name(unit, symbol->name, &qualified);
        th_type *nominal = qualified.data
            ? th_name(state->types, core_buffer_string(&qualified)) : nullptr;
        core_buffer_release(&qualified);
        if (nominal) {
            symbol->type = nominal;
            parse_tree_set_type(symbol->decl_node, nominal);
            parse_tree_set_type(node, nominal);
        }
    }
    else if (symbol->type) {
        th_type *previous_type = parse_tree_type(node);
        th_type *resolved_type = previous_type
            ? th_unify_in_place(state->types, previous_type, symbol->type)
            : symbol->type;
        if (!resolved_type) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC(
                    "type reference conflicts with its declaration"),
                link_source_location(node));
            return;
        }
        parse_tree_set_type(node, resolved_type);
    }
}

bool
n00bb_is_nil_literal(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)
        && !NT_IS(grammar, node, "simple-lit")) {
        if (core_tree_num_children(node) != 1) return false;
        node = core_tree_child(node, 0);
    }
    return NT_IS(grammar, node, "simple-lit")
        && n00bb_nil_spelling(first_token_text(node));
}






parse_tree_t *
n00bb_simple_switch_literal(parse_grammar_t *grammar, parse_tree_t *node,
                            bool *negative, bool *saw_sign)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (NT_IS(grammar, node, "simple-lit")) return node;
    parse_tree_t *literal = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) {
            core_string_t spelling = parse_tree_leaf_text(child);
            if (*saw_sign || (!core_string_eq(spelling,
                                              CORE_STRING_STATIC("-"))
                && !core_string_eq(spelling, CORE_STRING_STATIC("+"))))
                return nullptr;
            *saw_sign = true;
            *negative = core_string_eq(spelling,
                                       CORE_STRING_STATIC("-"));
            continue;
        }
        parse_tree_t *found = n00bb_simple_switch_literal(
            grammar, child, negative, saw_sign);
        if (!found || literal) return nullptr;
        literal = found;
    }
    return literal;
}

bool
refine_integer_literal_type(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *node, n00bb_tu_state_t *state)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    if (!token) return true;
    core_string_t terminal = parse_terminal_name(grammar, parse_token_tid(token));
    if (!core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
        && !core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT")))
        return true;
    ir_lit_t literal = {0};
    if (literal_modifier(grammar, node).data
        || !n00bb_parse_switch_integer(first_token_text(node), &literal)) {
        return true;
    }
    bool negative = false;
    parse_tree_t *ancestor = parse_tree_parent(node);
    while (ancestor) {
        if (NT_IS(grammar, ancestor, "unary-expr")) {
            bool signed_negative = false;
            bool saw_sign = false;
            parse_tree_t *signed_literal = n00bb_simple_switch_literal(
                grammar, ancestor, &signed_negative, &saw_sign);
            if (signed_literal == node && saw_sign) {
                negative = signed_negative;
                break;
            }
        }
        ancestor = parse_tree_parent(ancestor);
    }
    if (negative && literal.mag > (ir_uint_t)1 << 127) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
            CORE_STRING_STATIC("no integer type holds this value"),
            link_source_location(node));
        return true;
    }
    literal.neg = negative;
    th_type *type = n00bb_integer_literal_variable(state, node, literal);
    if (!type) return false;
    parse_tree_set_type(node, type);
    return true;
}

bool
infer_new_expression_type(parse_grammar_t *grammar,
                          parse_tree_t *node, n00bb_tu_state_t *state)
{
    parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
    if (!type_node) return true;
    th_type *type = parse_tree_type(type_node);
    if (!type) return false;
    th_type *existing = parse_tree_type(node);
    if (existing) {
        th_type *unified = th_unify_in_place(state->types, existing, type);
        if (!unified) return false;
        parse_tree_set_type(node, unified);
    }
    else parse_tree_set_type(node, type);
    return true;
}
parse_tree_t *
top_level_statement_child(parse_grammar_t *grammar, parse_tree_t *wrapper)
{
    for (size_t i = 0; wrapper && i < core_tree_num_children(wrapper); i++) {
        parse_tree_t *child = core_tree_child(wrapper, i);
        if (parse_tree_nt_name(grammar, child).data) return child;
        if (!parse_tree_is_group(child)) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (parse_tree_nt_name(grammar, item).data) return item;
        }
    }
    return nullptr;
}
bool
cache_enum_item_value(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                      parse_tree_t *node)
{
    parse_tree_t *explicit_value = CHILD_NT(grammar, node, "enum-value");
    if (!explicit_value) return true;
    parse_tree_t *literal = CHILD_NT(grammar, explicit_value, "simple-lit");
    parse_tree_t *terminal_node = literal ? core_tree_child(literal, 0) : nullptr;
    parse_token_info_t *token = terminal_node && core_tree_is_leaf(terminal_node)
        ? core_tree_leaf_value(terminal_node) : nullptr;
    core_string_t terminal = token
        ? parse_terminal_name(grammar, parse_token_tid(token)) : (core_string_t){0};
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    ir_lit_t value = {0};
    if (!symbol || (!core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
            && !core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT")))
        || !n00bb_parse_switch_integer(parse_tree_leaf_text(terminal_node),
                                       &value)) return false;
    bool negative = core_tree_num_children(explicit_value) == 3;
    n00bb_enum_value_t *stored = core_arena_alloc(
        &state->semantic_values, sizeof(*stored));
    if (!stored) return false;
    *stored = (n00bb_enum_value_t){value.mag, negative && value.mag != 0};
    symbol->comptime_value = stored;
    symbol->has_comptime_value = true;
    return true;
}
bool
validate_package_body_statement(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node, parse_tree_t *parent)
{
    if (!NT_IS(grammar, node, "body-stmt")) return true;
    parse_tree_t *ancestor = parent;
    while (ancestor && !NT_IS(grammar, ancestor, "package-decl"))
        ancestor = parse_tree_parent(ancestor);
    if (!ancestor || CHILD_NT(grammar, node, "import-stmt")
        || CHILD_NT(grammar, node, "export-stmt")) return true;
    parse_tree_t *body = CHILD_NT(grammar, node, "body");
    n00bb_node_facts_t *facts = body ? n00bb_node_facts(body) : nullptr;
    if (facts && facts->selected_when_body) return true;
    package_diagnostic(unit, node, CORE_STRING_STATIC("N00BB-M0010"),
        CORE_STRING_STATIC(
            "a selected package branch may contain only imports, exports, and nested when statements"));
    return false;
}
void
cache_wrapped_symbol_resolution(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node)
{
    if (!grammar || !node || core_tree_is_leaf(node)) return;
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL))
        return;
    if (NT_IS(grammar, node, "primary")) {
        n00bb_tu_state_t *state = tu_state(unit);
        if (parse_tree_resolution(node).kind == PARSE_NODE_RESOLUTION_NONE
            && state && state->annotations && state->annotations->symtab)
            (void)parse_symtab_resolve_node(state->annotations->symtab,
                node, CORE_STRING_STATIC(""));
        if (parse_tree_resolution(node).kind
            == PARSE_NODE_RESOLUTION_SYMBOL)
            (void)parse_tree_mark_direct_symbol_reference(node);
        return;
    }
    const parse_annotation_t *emit = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_EMIT) : nullptr;
    bool identity = emit && core_string_eq(
        emit->emit_action, CORE_STRING_STATIC("identity"));
    bool parenthesized = NT_IS(grammar, node, "tuple-or-paren")
        && core_tree_num_children(node) == 3;
    if (!identity && !parenthesized) return;
    parse_tree_t *source = nullptr;
    if (parenthesized)
        source = core_tree_child(node, 1);
    else if (core_tree_num_children(node) == 1
        && !core_tree_is_leaf(core_tree_child(node, 0)))
        source = core_tree_child(node, 0);
    parse_node_resolution_t resolution = parse_tree_resolution(source);
    if (resolution.kind == PARSE_NODE_RESOLUTION_NONE
        && source && !core_tree_is_leaf(source)
        && parse_tree_sym(source))
        resolution = (parse_node_resolution_t){
            .kind = PARSE_NODE_RESOLUTION_SYMBOL,
            .target.symbol = parse_tree_sym(source),
        };
    if (parse_tree_resolution(node).kind == PARSE_NODE_RESOLUTION_NONE
        && resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        (void)parse_tree_set_resolution_symbol(node,
                                               resolution.target.symbol);
    else if (parse_tree_resolution(node).kind == PARSE_NODE_RESOLUTION_NONE
             && resolution.kind == PARSE_NODE_RESOLUTION_SCOPE)
        (void)parse_tree_set_resolution_scope(node, resolution.target.scope);
    if (source && parse_tree_is_direct_symbol_reference(source))
        (void)parse_tree_mark_direct_symbol_reference(node);
    parse_sym_entry_t *reference_target = source
        ? parse_tree_reference_target(source) : nullptr;
    if (reference_target)
        (void)parse_tree_set_reference_target(node, reference_target);
}


bool
bind_builtin_reference(parse_grammar_t *grammar,
                       n00bb_tu_state_t *state, parse_tree_t *node)
{
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    if (!rule || !parse_rule_find_annot(grammar, rule, PARSE_ANNOT_VARREF)
        || parse_tree_resolution(node).kind != PARSE_NODE_RESOLUTION_NONE)
        return true;
    parse_symtab_t *symtab = state->annotations->symtab;
    parse_scope_t *scope = parse_symtab_scope_for_node(symtab, node);
    core_string_t name = first_token_text(node);
    if (!scope || !name.data
        || parse_symtab_lookup_scoped(symtab, scope,
                                      CORE_STRING_STATIC(""), name))
        return true;
    parse_namespace_t *builtins = parse_symtab_ns(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_sym_entry_t *symbol = builtins && builtins->root
        ? parse_symtab_lookup_in(builtins->root, name) : nullptr;
    return !symbol || (parse_tree_set_resolution_symbol(node, symbol)
        && parse_tree_mark_direct_symbol_reference(node));
}

th_type *
expected_expression_child_type(parse_grammar_t *grammar,
                               parse_tree_t *parent, parse_tree_t *node,
                               th_type *expected)
{
    if (!parent || core_tree_is_leaf(node) || core_tree_is_leaf(parent))
        return nullptr;
    const parse_rule_t *rule = parse_tree_rule(grammar, parent);
    const parse_annotation_t *declares = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_DECLARES) : nullptr;
    if (declares && parse_child_ref_is_set(declares->type_ref)
        && NT_IS(grammar, node, "expression")
        && core_tree_child(parent, core_tree_num_children(parent) - 1)
            == node) {
        parse_tree_t *spec = parse_tree_resolve_child_ref(grammar, parent,
                                                          declares->type_ref);
        return parse_tree_type(spec);
    }
    if (!expected) return nullptr;
    const parse_annotation_t *emit = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_EMIT) : nullptr;
    bool identity = emit && core_string_eq(emit->emit_action,
                                            CORE_STRING_STATIC("identity"));
    if (!identity && !NT_IS(grammar, parent, "primary")
        && !NT_IS(grammar, parent, "literal-expr")) return nullptr;
    if (core_tree_num_children(parent) == 1
        && core_tree_child(parent, 0) == node)
        return expected;
    return nullptr;
}

bool
prepare_initial_symbols(core_translation_unit_t *unit, th_ctx *types)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !types) return false;
    if (state->annotations) return true;

    state->types = types;
    n00bb_register_value_coercions(state);
    state->annotations = parse_annot_result_new(nullptr);
    parse_symtab_set_owner_unit(state->annotations->symtab, unit);
    if (!th_register_method_resolver(types, n00bb_callback_method_signature,
                                     state)) return false;
    parse_annot_result_set_type_node_resolver(state->annotations,
        n00bb_resolve_type_node, unit);
    if (!register_builtin_symbols(state->annotations, state, types)
        || !register_native_declarations(state->annotations, types,
                                         n00bb_source_natives(unit)))
        return false;
    parse_annot_result_set_diagnostics(state->annotations,
        core_translation_unit_diagnostics(unit), unit);
    parse_annot_result_set_node_callback(state->annotations,
                                         initial_symbol_node,
                                         state->annotations);
    return true;
}

bool
collect_array_dimension_nodes(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state, parse_tree_t *node,
                              core_list_t(n00bb_tree_ptr_t) *dimensions)
{
    if (!grammar || !state || !node || !dimensions) return false;
    if (NT_IS(grammar, node, "base-tspec")) {
        n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(node);
        if (!cached) return false;
        for (size_t i = 0; i < cached->count; i++)
            core_list_push(*dimensions, cached->nodes[i]);
        return true;
    }
    if (NT_IS(grammar, node, "one-tspec")) {
        parse_tree_t *base = CHILD_NT(grammar, node, "base-tspec");
        return !base || collect_array_dimension_nodes(grammar, state, base,
                                                       dimensions);
    }
    if (NT_IS(grammar, node, "union-tspec")) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *child = core_tree_child(node, i);
            if (NT_IS(grammar, child, "one-tspec")) {
                if (!collect_array_dimension_nodes(grammar, state, child,
                                                    dimensions)) return false;
            }
            else if (parse_tree_is_group(child))
                for (size_t j = 0; j < core_tree_num_children(child); j++) {
                    parse_tree_t *item = core_tree_child(child, j);
                    if (NT_IS(grammar, item, "one-tspec")
                        && !collect_array_dimension_nodes(grammar, state, item,
                                                          dimensions))
                        return false;
                }
        }
        return true;
    }
    if (NT_IS(grammar, node, "type-spec")) {
        parse_tree_t *one = CHILD_NT(grammar, node, "one-tspec");
        return !one || collect_array_dimension_nodes(grammar, state, one,
                                                      dimensions);
    }
    if (NT_IS(grammar, node, "return-type")) {
        parse_tree_t *types = CHILD_NT(grammar, node, "union-tspec");
        return !types || collect_array_dimension_nodes(grammar, state, types,
                                                        dimensions);
    }
    return false;
}

static bool
collect_declared_array_dimensions(parse_grammar_t *grammar,
                                  n00bb_tu_state_t *state,
                                  parse_sym_entry_t *symbol,
                                  core_list_t(n00bb_tree_ptr_t) *dimensions)
{
    parse_tree_t *declaration = symbol_binding_node(symbol);
    parse_tree_t *type_spec = NT_IS(grammar, declaration, "type-spec")
        ? declaration : CHILD_NT(grammar, declaration, "type-spec");
    parse_tree_t *one = CHILD_NT(grammar, type_spec, "one-tspec");
    if (!one)
        one = CHILD_NT(grammar,
            CHILD_NT(grammar, type_spec, "union-tspec"), "one-tspec");
    parse_tree_t *base = CHILD_NT(grammar, one, "base-tspec");
    if (!base) return true;
    return collect_array_dimension_nodes(grammar, state, base, dimensions);
}

static bool
fold_static_operation(core_string_t operator, uint64_t left, uint64_t right,
                      uint64_t *value)
{
    if (core_string_eq(operator, CORE_STRING_STATIC("+")))
        return !__builtin_add_overflow(left, right, value);
    if (core_string_eq(operator, CORE_STRING_STATIC("-")))
        return !__builtin_sub_overflow(left, right, value);
    if (core_string_eq(operator, CORE_STRING_STATIC("*")))
        return !__builtin_mul_overflow(left, right, value);
    if (core_string_eq(operator, CORE_STRING_STATIC("/")))
        return right && ((*value = left / right), true);
    if (core_string_eq(operator, CORE_STRING_STATIC("%")))
        return right && ((*value = left % right), true);
    if (core_string_eq(operator, CORE_STRING_STATIC("<<")))
        return right < 64 && !(left >> (63 - right) >> 1)
            && ((*value = left << right), true);
    if (core_string_eq(operator, CORE_STRING_STATIC(">>")))
        return right < 64 && ((*value = left >> right), true);
    if (core_string_eq(operator, CORE_STRING_STATIC("&")))
        return (*value = left & right), true;
    if (core_string_eq(operator, CORE_STRING_STATIC("|")))
        return (*value = left | right), true;
    if (core_string_eq(operator, CORE_STRING_STATIC("^")))
        return (*value = left ^ right), true;
    return false;
}

static bool
fold_static_extent(parse_grammar_t *grammar, parse_tree_t *expression,
                   uint32_t depth, uint64_t *value)
{
    if (!expression || core_tree_is_leaf(expression) || depth > 64)
        return false;
    int64_t literal = 0;
    if (n00bb_range_simple_integer_literal(grammar, expression, &literal)) {
        *value = (uint64_t)literal;
        return literal >= 0;
    }
    parse_node_resolution_t resolution = parse_tree_resolution(expression);
    parse_sym_entry_t *symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    if (symbol && symbol->kind == PARSE_SYM_ENUM_CONST) {
        if (!symbol->has_comptime_value || !symbol->comptime_value)
            return false;
        ir_uint_t bits = 0;
        if (n00bb_flagset_member(symbol))
            bits = ((const n00bb_flagset_value_t *)
                        symbol->comptime_value)->mask;
        else {
            const n00bb_enum_value_t *member = symbol->comptime_value;
            if (member->negative) return false;
            bits = member->magnitude;
        }
        if (bits > UINT64_MAX) return false;
        *value = (uint64_t)bits;
        return true;
    }
    if (symbol && symbol->kind == PARSE_SYM_VARIABLE) {
        parse_tree_t *initializer = symbol->mutability == PARSE_SYM_CONST
                && symbol->decl_node
            ? CHILD_NT(grammar, symbol->decl_node, "expression") : nullptr;
        return fold_static_extent(grammar, initializer, depth + 1, value);
    }
    size_t children = core_tree_num_children(expression);
    if (children == 1)
        return fold_static_extent(grammar, core_tree_child(expression, 0),
                                  depth + 1, value);
    if (NT_IS(grammar, expression, "tuple-or-paren") && children == 3)
        return fold_static_extent(grammar, core_tree_child(expression, 1),
                                  depth + 1, value);
    if (NT_IS(grammar, expression, "unary-expr") && children == 5
        && core_string_eq(first_token_text(expression),
                          CORE_STRING_STATIC("cast")))
        return fold_static_extent(grammar, core_tree_child(expression, 4),
                                  depth + 1, value);
    uint64_t left = 0, right = 0;
    return children == 3
        && fold_static_extent(grammar, core_tree_child(expression, 0),
                              depth + 1, &left)
        && fold_static_extent(grammar, core_tree_child(expression, 2),
                              depth + 1, &right)
        && fold_static_operation(
            first_token_text(core_tree_child(expression, 1)), left, right,
            value);
}

bool
n00bb_static_integer_expression(parse_grammar_t *grammar,
                                parse_tree_t *expression, th_type *type,
                                uint64_t *value)
{
    if (n00bb_scalar_role_is(type, IR_ROLE_CHAR))
        return fold_static_extent(grammar, expression, 0, value)
            && *value <= 0x10ffff && (*value < 0xd800 || *value > 0xdfff);
    return is_integer_type(type)
        && fold_static_extent(grammar, expression, 0, value);
}

ir_shape_extent_t
array_dimension_extent(parse_grammar_t *grammar, parse_tree_t *expression)
{
    uint64_t value = 0;
    if (fold_static_extent(grammar, expression, 0, &value))
        return (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = value,
        };
    parse_scope_t *scope = parse_tree_scope(expression);
    return (ir_shape_extent_t){
        .kind = scope_is_within_function(scope) ? IR_SHAPE_EXTENT_RUNTIME
                                                : IR_SHAPE_EXTENT_UNRESOLVED,
        .expression = expression,
    };
}

bool
populate_unit_symbol_shapes(parse_grammar_t *grammar, ir_ctx_t *ir_context,
                            core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symbols = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!grammar || !ir_context || !unit || !symbols) return false;
    for (parse_scope_t *scope = symbols->all_scopes; scope;
         scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            th_type *type = symbol->type ? th_resolve(symbol->type) : nullptr;
            if (!type || th_kind_of(type) == TH_VAR) continue;
            bool complete = false;
            (void)array_shape_from_declaration(grammar, unit, ir_context, symbol,
                                                type, &complete);
            if (!complete) {
                if (!core_diagnostics_failed(
                        core_translation_unit_diagnostics(unit)))
                    core_diagnostics_report(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0045"),
                        CORE_STRING_STATIC(
                            "could not publish finalized array shape metadata"));
                return false;
            }
            if (symbol->kind == PARSE_SYM_FUNCTION
                && th_kind_of(type) == TH_FUNCTION) {
                parse_tree_t *signature = CHILD_NT(grammar,
                    symbol->decl_node, "func-type-signature");
                parse_tree_t *result = CHILD_NT(grammar, signature,
                                                "function-result");
                parse_tree_t *return_type = CHILD_NT(grammar, result,
                                                     "return-type");
                th_type *result_type = th_resolve(th_func_ret(type));
                uint32_t rank = 0;
                th_type *element = result_type;
                while (element && th_kind_of(element) == TH_SEQUENCE) {
                    element = th_resolve(th_seq_elem(element));
                    if (!element || rank == UINT32_MAX) return false;
                    rank++;
                }
                if (rank && return_type
                    && !parse_sym_binding(symbol,
                        CORE_STRING_STATIC("ir.return-shape"))) {
                    parse_sym_entry_t *alias_source = parse_sym_binding(
                        symbol, CORE_STRING_STATIC(
                            "n00bb.array-shape-alias-source"));
                    if (alias_source) {
                        bool source_complete = false;
                        th_type *source_type = alias_source->type
                            ? th_resolve(alias_source->type) : result_type;
                        ir_shape_t *shape = array_shape_from_declaration(
                            grammar, unit, ir_context, alias_source, source_type,
                            &source_complete);
                        if (!source_complete || !shape
                            || !parse_sym_set_binding(symbol,
                                CORE_STRING_STATIC("ir.return-shape"),
                                shape)) return false;
                    }
                    else {
                        ir_shape_t *shape = ir_shape_new(ir_context, element,
                                                         rank);
                        core_list_t(n00bb_tree_ptr_t) dimensions =
                            core_list_new(n00bb_tree_ptr_t);
                        bool ok = shape && collect_array_dimension_nodes(
                            grammar, state, return_type, &dimensions)
                            && dimensions.len <= rank;
                        for (uint32_t d = 0; ok && d < rank; d++)
                            ok = ir_shape_set_extent(shape, d,
                                (ir_shape_extent_t){
                                    .kind = IR_SHAPE_EXTENT_UNRESOLVED,
                                });
                        for (size_t d = 0; ok && d < dimensions.len; d++) {
                            parse_tree_t *expression = CHILD_NT(grammar,
                                dimensions.data[d], "expression");
                            if (expression)
                                ok = ir_shape_set_extent(shape, (uint32_t)d,
                                    array_dimension_extent(grammar,
                                                           expression));
                        }
                        uint32_t prefix = 0;
                        while (ok && prefix < dimensions.len
                            && CHILD_NT(grammar,
                                dimensions.data[prefix], "expression"))
                            prefix++;
                        ok = ok && ir_shape_set_storage(shape, prefix, true)
                            && parse_sym_set_binding(symbol,
                                CORE_STRING_STATIC("ir.return-shape"), shape);
                        core_list_free(dimensions);
                        if (!ok) return false;
                    }
                }
            }
        }
    }
    return true;
}

ir_shape_t *
array_shape_from_declaration(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             ir_ctx_t *ir_context,
                             parse_sym_entry_t *symbol,
                             th_type *semantic_type, bool *complete)
{
    if (complete) *complete = false;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!grammar || !state || !ir_context || !symbol || !semantic_type
        || !complete)
        return nullptr;
    ir_shape_t *existing = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.array-shape"));
    if (existing) {
        *complete = true;
        return existing;
    }
    th_type *dimension_type = th_resolve(semantic_type);
    uint32_t rank = 0;
    while (th_kind_of(dimension_type) == TH_SEQUENCE) {
        th_type *element = th_seq_elem(dimension_type);
        if (!element || rank == UINT32_MAX) return nullptr;
        dimension_type = th_resolve(element);
        rank++;
    }
    if (!rank) {
        *complete = true;
        return nullptr;
    }

    core_list_t(n00bb_tree_ptr_t) dimensions =
        core_list_new(n00bb_tree_ptr_t);
    if (!collect_declared_array_dimensions(grammar, state, symbol,
                                           &dimensions)
        || dimensions.len > rank) {
        core_list_free(dimensions);
        return nullptr;
    }
    parse_sym_entry_t *alias_source = dimensions.len == 0
        ? parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.array-shape-alias-source"))
        : nullptr;
    if (alias_source && alias_source != symbol) {
        bool source_complete = false;
        th_type *source_type = alias_source->type
            ? th_resolve(alias_source->type) : semantic_type;
        ir_shape_t *source_shape = array_shape_from_declaration(grammar,
            unit, ir_context, alias_source, source_type, &source_complete);
        core_list_free(dimensions);
        if (!source_complete) return nullptr;
        if (!source_shape) {
            *complete = true;
            return nullptr;
        }
        if (!parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("ir.array-shape"), source_shape))
            return nullptr;
        *complete = true;
        return source_shape;
    }

    ir_shape_t *shape = ir_shape_new(ir_context, dimension_type, rank);
    bool ok = shape != nullptr;
    for (uint32_t i = 0; ok && i < rank; i++)
        ok = ir_shape_set_extent(shape, i, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_UNRESOLVED,
        });
    for (size_t i = 0; ok && i < dimensions.len; i++) {
        parse_tree_t *expression = CHILD_NT(grammar,
            dimensions.data[i], "expression");
        if (!expression) continue;
        int64_t literal = 0;
        if (n00bb_range_simple_integer_literal(grammar, expression,
                                                &literal)
            && literal <= 0) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0080"),
                CORE_STRING_STATIC("array declaration bounds must be positive"),
                link_source_location(expression));
            ok = false;
            break;
        }
        ok = ir_shape_set_extent(shape, (uint32_t)i,
            array_dimension_extent(grammar, expression));
    }
    uint32_t flat_prefix_rank = 0;
    while (ok && flat_prefix_rank < dimensions.len) {
        parse_tree_t *dimension = dimensions.data[flat_prefix_rank];
        if (!CHILD_NT(grammar, dimension, "expression")) break;
        flat_prefix_rank++;
    }
    ok = ok && ir_shape_set_storage(shape, flat_prefix_rank, true);
    ok = ok && measure_static_shape_prefix(shape, flat_prefix_rank);
    core_list_free(dimensions);
    if (!ok || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"), shape)) return nullptr;
    *complete = true;
    return shape;
}

bool
measure_static_shape_prefix(ir_shape_t *shape, uint32_t flat_prefix_rank)
{
    bool ok = true;
    bool static_prefix = flat_prefix_rank != 0;
    uint64_t element_count = 1;
    for (uint32_t i = 0; ok && static_prefix && i < flat_prefix_rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) {
            ok = false;
            break;
        }
        if (extent.kind != IR_SHAPE_EXTENT_STATIC) {
            static_prefix = false;
            break;
        }
        ok = !__builtin_mul_overflow(element_count, extent.static_value,
                                     &element_count);
    }
    if (ok && static_prefix) {
        uint64_t stride = 1;
        for (uint32_t i = flat_prefix_rank; ok && i > 0; i--) {
            ir_shape_extent_t extent;
            ok = ir_shape_extent(shape, i - 1, &extent)
                && ir_shape_set_stride(shape, i - 1,
                    (ir_shape_measure_t){
                        .kind = IR_SHAPE_MEASURE_STATIC,
                        .static_value = stride,
                    });
            if (ok)
                ok = !__builtin_mul_overflow(stride, extent.static_value,
                                             &stride);
        }
        if (ok)
            ok = ir_shape_set_element_count(shape, (ir_shape_measure_t){
                .kind = IR_SHAPE_MEASURE_STATIC,
                .static_value = element_count,
            });
    }
    return ok;
}

static core_string_t
source_typevar_name(parse_tree_t *leaf)
{
    if (!leaf || !core_tree_is_leaf(leaf)) return (core_string_t){0};
    core_string_t spelling = parse_tree_leaf_text(leaf);
    if (!spelling.data || spelling.u8_bytes < 2 || spelling.data[0] != '\'')
        return (core_string_t){0};
    return core_string_borrow(spelling.data + 1, spelling.u8_bytes - 1);
}

th_type *
signature_typevar_for_node(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !node
        || !NT_IS(grammar, node, "tspec-tvar")
        || core_tree_num_children(node) != 1) return nullptr;
    core_string_t name = source_typevar_name(core_tree_child(node, 0));
    if (!name.data) return nullptr;

    parse_tree_t *owner_node = node;
    while (owner_node && !NT_IS(grammar, owner_node, "func-def")
           && !NT_IS(grammar, owner_node, "trait-decl")
           && !NT_IS(grammar, owner_node, "callback-decl")
           && !field_record_declaration(grammar, owner_node)
           && !NT_IS(grammar, owner_node, "variant-decl"))
        owner_node = parse_tree_parent(owner_node);
    parse_sym_entry_t *owner = owner_node
        ? parse_tree_sym(owner_node) : nullptr;
    if (!owner) return nullptr;

    n00bb_signature_typevars_t *variables =
        parse_sym_binding(owner, N00BB_SIGNATURE_TYPEVARS_KEY);
    if (!variables) {
        variables = core_arena_alloc(&state->semantic_values,
                                      sizeof(*variables));
        if (!variables || !parse_sym_set_binding(owner,
                                                  N00BB_SIGNATURE_TYPEVARS_KEY,
                                                  variables)) return nullptr;
        memset(variables, 0, sizeof(*variables));
    }

    for (size_t i = 0; i < variables->len; i++) {
        if (!core_string_eq(variables->data[i].name, name)) continue;
        th_type *type = variables->data[i].type;
        th_type *existing = parse_tree_type(node);
        if (existing && existing != type) {
            type = th_unify_in_place(state->types, existing, type);
            if (!type) return nullptr;
            variables->data[i].type = type;
        }
        parse_tree_set_type(node, type);
        return type;
    }

    th_type *type = parse_tree_type(node);
    if (!type || th_kind_of(th_resolve(type)) != TH_VAR)
        type = var_named_text(state->types, name);
    if (!type) return nullptr;
    if (!core_arena_try_reserve_raw(&state->semantic_values,
            (void **)&variables->data, &variables->cap,
            variables->len + 1, sizeof(*variables->data))) return nullptr;
    variables->data[variables->len++] = (n00bb_typevar_binding_t){name, type};
    parse_tree_set_type(node, type);
    return type;
}


th_type *
n00bb_resolve_type_node(th_ctx *types, parse_grammar_t *grammar,
                        parse_tree_t *node, void *context)
{
    core_translation_unit_t *unit = context;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->types != types || !node) return nullptr;
    th_type *cached = parse_tree_type(node);
    if (cached) return cached;
    if (NT_IS(grammar, node, "tspec-tvar"))
        return signature_typevar_for_node(grammar, unit, node);
    return nullptr;
}

void
record_cached_declared_function_signature(parse_grammar_t *grammar,
                                          core_translation_unit_t *unit,
                                          parse_tree_t *signature,
                                          n00bb_tu_state_t *state)
{
    parse_tree_t *function = parse_tree_parent(signature);
    parse_sym_entry_t *symbol = function
        ? parse_tree_sym(function) : nullptr;
    if (!symbol || parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.declared-function-signature"))) return;

    if (!state || !state->types) return;
    parse_tree_t *result = CHILD_NT(grammar, signature, "function-result");
    parse_tree_t *return_node = CHILD_NT(grammar, result, "return-type");
    parse_tree_t *error_node = CHILD_NT(grammar, result, "error-type");
    parse_tree_t *return_types = CHILD_NT(grammar, return_node, "union-tspec");
    parse_tree_t *error_type = CHILD_NT(grammar, error_node, "one-tspec");
    th_type *success = return_node
        ? return_types ? parse_tree_type(return_types) : nullptr
        : th_var(state->types);
    th_type *failure = error_node
        ? error_type ? parse_tree_type(error_type) : nullptr
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    n00bb_function_parameter_facts_t *facts = parse_sym_binding(symbol,
        N00BB_PARAMETER_FACTS_KEY);
    core_list_t(th_param) params = core_list_new(th_param);
    th_param rest = {0};
    bool has_rest = false;
    bool valid = success && failure
        && resolve_function_parameter_facts(grammar, state, facts,
            &params, &rest, &has_rest);
    core_list_t(n00bb_typevar_binding_t) variables =
        core_list_new(n00bb_typevar_binding_t);
    core_list_t(th_trait_requirement) declared =
        core_list_new(th_trait_requirement);
    signature_typevars_for_symbol(symbol, &variables);
    valid = valid && apply_signature_constraints(grammar, unit,
        CHILD_NT(grammar, signature, "where-clause"), state->types,
        &variables, &declared);
    th_type *signature_type = valid
        ? th_func_with_error(state->types, success, failure, params.data,
            params.len, has_rest ? &rest : nullptr, nullptr, 0)
        : nullptr;
    core_list_free(variables);
    core_list_free(params);
    if (!signature_type) {
        core_list_free(declared);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
            CORE_STRING_STATIC(
                "could not form the function signature from its cached type facts"),
            link_source_location(signature));
        return;
    }

    th_type *previous = symbol->type;
    th_type *unified = previous
        ? th_unify_in_place(state->types, previous, signature_type)
        : signature_type;
    if (!unified) {
        core_list_free(declared);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
            CORE_STRING_STATIC(
                "cached function signature conflicts with its symbol type"),
            link_source_location(signature));
        return;
    }
    bool declared_contract = return_node || error_node;
    if (declared_contract
        && (!parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.declared-function-signature"),
                unified)
            || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.declared-function-failure"),
                failure))) {
        core_list_free(declared);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not retain the cached function signature"),
            link_source_location(signature));
        return;
    }
    if (declared.len) {
        if (declared.len > SIZE_MAX / sizeof(th_trait_requirement)) {
            core_list_free(declared);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("too many declared where constraints"),
                link_source_location(signature));
            return;
        }
        n00bb_declared_where_t *facts = core_arena_alloc(
            &state->semantic_values, sizeof(*facts));
        th_trait_requirement *items = core_arena_alloc(
            &state->semantic_values, declared.len * sizeof(*items));
        if (!facts || !items) {
            core_list_free(declared);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not retain declared where constraints"),
                link_source_location(signature));
            return;
        }
        memcpy(items, declared.data, declared.len * sizeof(*items));
        *facts = (n00bb_declared_where_t){.data = items,
                                          .len = declared.len};
        if (!parse_sym_set_binding(symbol, N00BB_DECLARED_WHERE_KEY,
                                   facts)) {
            core_list_free(declared);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not bind declared where constraints"),
                link_source_location(signature));
            return;
        }
    }
    core_list_free(declared);
    propagate_symbol_type(symbol, state->types, previous, unified);
    symbol->error_type = failure;
    symbol->callable_contract = unified;
}

static bool
apply_one_signature_constraint(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *constraint_node,
                               th_ctx *types, th_type *variable,
                               core_list_t(th_trait_requirement) *declared)
{
    th_constraint constraint = {0};
    parse_tree_t *target = CHILD_NT(grammar, constraint_node, "one-tspec");
    if (target) {
        constraint.kind = TH_CON_EXCLUDES;
        constraint.target = parse_tree_type(target);
        if (!constraint.target) return false;
    }
    else {
        parse_tree_t *trait = CHILD_NT(grammar, constraint_node,
                                        "trait-name");
        core_translation_unit_t *owner = nullptr;
        parse_sym_entry_t *trait_symbol = nullptr;
        core_buffer_t name = {0};
        n00bb_link_use_site_t *trait_site = trait
            ? link_use_site_for_node(tu_state(unit), trait) : nullptr;
        if (!trait_site || !n00bb_resolve_trait_reference(grammar, unit,
                    trait_site,
                    &owner, &trait_symbol, &name)) {
            core_buffer_release(&name);
            return false;
        }
        if (trait_symbol && owner) {
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "%.*s.%.*s",
                (int)owner->module.identifier.u8_bytes,
                owner->module.identifier.data,
                (int)trait_symbol->name.u8_bytes,
                trait_symbol->name.data);
            core_buffer_release(&name);
            name = qualified;
        }
        constraint.kind = TH_CON_IMPLEMENTS;
        constraint.iface = core_buffer_string(&name);
        if (!th_var_constrain(types, variable, constraint)) {
            core_buffer_release(&name);
            return false;
        }
        if (declared) {
            n00bb_tu_state_t *state = tu_state(unit);
            char *copy = state ? core_arena_alloc(
                &state->semantic_values, name.byte_len + 1) : nullptr;
            if (!copy) {
                core_buffer_release(&name);
                return false;
            }
            if (name.byte_len) memcpy(copy, name.data, name.byte_len);
            core_list_push(*declared, ((th_trait_requirement){
                .subject = variable, .kind = TH_CON_IMPLEMENTS,
                .iface = core_string_borrow(copy, name.byte_len),
            }));
        }
        core_buffer_release(&name);
        return true;
    }
    if (!th_var_constrain(types, variable, constraint)) return false;
    if (declared)
        core_list_push(*declared, ((th_trait_requirement){
            .subject = variable, .kind = TH_CON_EXCLUDES,
            .target = constraint.target,
        }));
    return true;
}

static bool
apply_signature_constraint_tree(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node, th_ctx *types,
                                th_type *variable,
                                core_list_t(th_trait_requirement) *declared)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (NT_IS(grammar, node, "one-constraint"))
        return apply_one_signature_constraint(grammar, unit, node, types,
                                              variable, declared);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!apply_signature_constraint_tree(grammar, unit,
                core_tree_child(node, i), types, variable, declared))
            return false;
    return true;
}

static bool
apply_signature_where_tree(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node, th_ctx *types,
                           core_list_t(n00bb_typevar_binding_t) *variables,
                           core_list_t(th_trait_requirement) *declared)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (NT_IS(grammar, node, "type-constraint")) {
        if (core_tree_num_children(node) < 3) return false;
        core_string_t var_name = source_typevar_name(core_tree_child(node, 0));
        if (!var_name.data) return false;
        th_type *variable = type_variable_for_signature(types, variables,
                                                         var_name, nullptr,
                                                         nullptr);
        parse_tree_t *constraints = CHILD_NT(grammar, node,
                                              "constraint-list");
        return variable && constraints
            && apply_signature_constraint_tree(grammar, unit, constraints,
                                               types, variable, declared);
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!apply_signature_where_tree(grammar, unit,
                core_tree_child(node, i), types, variables, declared))
            return false;
    return true;
}

bool
apply_signature_constraints(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *where, th_ctx *types,
                            core_list_t(n00bb_typevar_binding_t) *variables,
                            core_list_t(th_trait_requirement) *declared)
{
    return apply_signature_where_tree(grammar, unit, where, types,
                                      variables, declared);
}

void
signature_typevars_for_symbol(parse_sym_entry_t *symbol,
                              core_list_t(n00bb_typevar_binding_t) *out)
{
    if (!symbol || !out) return;
    n00bb_signature_typevars_t *variables = parse_sym_binding(symbol,
        N00BB_SIGNATURE_TYPEVARS_KEY);
    for (size_t i = 0; variables && i < variables->len; i++)
        core_list_push(*out, variables->data[i]);
}

th_type *
type_variable_for_signature(th_ctx *types,
    core_list_t(n00bb_typevar_binding_t) *variables,
    core_string_t name, th_type *annotated, parse_tree_t *node)
{
    if (!name.data) return nullptr;
    if (variables) {
        for (size_t i = 0; i < variables->len; i++) {
            n00bb_typevar_binding_t *binding = &variables->data[i];
            if (!core_string_eq(binding->name, name)) continue;
            if (node) parse_tree_set_type(node, binding->type);
            return binding->type;
        }
    }
    th_type *type = variables ? th_var_named(types, name)
        : annotated ? annotated : th_var_named(types, name);
    if (type && node) parse_tree_set_type(node, type);
    if (type && variables)
        core_list_push(*variables, ((n00bb_typevar_binding_t){name, type}));
    return type;
}

void
record_function_signature(parse_grammar_t *grammar,
                           core_translation_unit_t *unit, parse_tree_t *func,
                           n00bb_tu_state_t *state, bool check_body)
{
    parse_sym_entry_t *symbol = parse_tree_sym(func);
    if (!symbol || !state->types) return;

    parse_tree_t *signature = CHILD_NT(grammar, func, "func-type-signature");
    parse_tree_t *params_node = CHILD_NT(grammar, signature, "param-decl");
    th_type *parsed_declared = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.declared-function-signature"));
    th_type *declared_failure = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.declared-function-failure"));
    n00bb_function_parameter_facts_t *parameter_facts = parse_sym_binding(
        symbol, N00BB_PARAMETER_FACTS_KEY);
    core_list_t(th_param) params = core_list_new(th_param);
    th_param rest = {0};
    bool has_rest = false;
    if (!resolve_function_parameter_facts(grammar, state, parameter_facts,
            &params, &rest, &has_rest)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC(
                "could not resolve the cached function parameter facts"),
            link_source_location(func));
        core_list_free(params);
        return;
    }
    th_type *success = nullptr;
    th_type *failure = nullptr;
    n00bb_function_return_facts_t *return_facts = parse_sym_binding(
        symbol, CORE_STRING_STATIC("n00bb.function.return-facts"));
    if (return_facts) {
        success = return_facts->success;
        failure = return_facts->failure;
        if (return_facts->conflict && !return_facts->reported)
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC(
                    "function exits produce incompatible success or failure types"),
                link_source_location(func));
    }
    bool has_success_value = success != nullptr;
    bool has_failure_value = failure != nullptr;
    if (!success) success = th_name(state->types, CORE_STRING_STATIC("nil"));
    if (!failure) failure = th_name(state->types, CORE_STRING_STATIC("nil"));

    bool refine_symbol_type = parsed_declared
        && parameter_facts
        && parameter_facts->has_inferred_parameter
        && symbol->type
        && th_kind_of(symbol->type) == TH_FUNCTION;
    th_type *declared = refine_symbol_type ? symbol->type : parsed_declared;
    th_type *inferred = th_func_with_error(state->types, success, failure,
        params.data, params.len, has_rest ? &rest : nullptr, nullptr, 0);
    if (declared && check_body && !has_success_value
        && th_kind_of(th_resolve(th_func_ret(declared))) == TH_VAR)
        th_unify_in_place(state->types, th_func_ret(declared),
                          th_name(state->types, CORE_STRING_STATIC("nil")));
    if (declared) {
        th_type *check_type = declared;
        th_type *check_inferred = inferred;
        if (check_type && !has_failure_value) {
            check_type = function_with_error_channel(state->types,
                                                      check_type, nullptr);
            check_inferred = function_with_error_channel(state->types,
                                                          inferred, nullptr);
        }
        th_type *unified = nullptr;
        if (!check_body || !has_success_value) unified = check_type;
        else unified = th_unify_in_place(state->types, check_type,
                                         check_inferred);
        bool matches = unified != nullptr;
        th_type *declared_error = declared_failure
            ? declared_failure : th_name(state->types, CORE_STRING_STATIC("nil"));
        th_type *check_error = declared_error;
        th_type *unified_error = nullptr;
        if (!check_body || !has_failure_value) unified_error = check_error;
        else unified_error = th_unify_in_place(state->types, check_error,
                                               failure);
        matches = matches && unified_error != nullptr;
        if (check_body && !matches
            && !(return_facts && return_facts->reported)) {
            core_buffer_t message = {0};
            core_string_t inferred_text = n00bb_print_type(state->types,
                                                           inferred);
            core_string_t declared_text = n00bb_print_type(state->types,
                                                           declared);
            core_buffer_printf(&message,
                "function body type '%.*s' does not match declared signature '%.*s'",
                (int)inferred_text.u8_bytes, inferred_text.data,
                (int)declared_text.u8_bytes, declared_text.data);
            parse_token_info_t *site = parse_tree_find_first_terminal(func);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
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
        if (refine_symbol_type) {
            symbol->type = has_failure_value ? unified
                : function_with_error_channel(state->types, unified,
                                              declared_error);
            symbol->error_type = has_failure_value ? unified_error
                                                   : declared_error;
        }
        else {
            symbol->type = declared;
            symbol->error_type = declared_error;
        }
    }
    else {
        if (symbol->type) {
            th_type *unified = th_unify_in_place(state->types, symbol->type,
                                                 inferred);
            if (unified) symbol->type = unified;
        }
        else symbol->type = inferred;

        if (symbol->error_type) {
            th_type *unified = th_unify_in_place(state->types,
                symbol->error_type, failure);
            if (unified) symbol->error_type = unified;
        }
        else symbol->error_type = failure;
    }

    core_list_free(params);
    if (core_string_eq(symbol->name, CORE_STRING_STATIC("__init"))) {
        th_type *constructor_type = symbol->type;
        constructor_type = constructor_type
            ? th_resolve(constructor_type) : nullptr;
        size_t receiver_index = 0;
        parse_sym_entry_t *receiver = formal_symbol_at(
            grammar, params_node, 0, &receiver_index);
        bool object_receiver = receiver
            && n00bb_object_type_declared(grammar, unit, receiver);
        if (!constructor_type
            || th_kind_of(constructor_type) != TH_FUNCTION
            || !receiver
            || (!core_string_eq(receiver->direction,
                                CORE_STRING_STATIC("modify"))
                && !(object_receiver
                     && core_string_eq(receiver->direction,
                                       CORE_STRING_STATIC("in"))))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0017"),
                object_receiver
                    ? CORE_STRING_STATIC(
                        "__init must take its constructed object as the first in or modify parameter")
                    : CORE_STRING_STATIC(
                        "__init must take its constructed value as the first modify parameter"),
                link_source_location(func));
        }
        const th_param *receiver_parameter = constructor_type
            && th_kind_of(constructor_type) == TH_FUNCTION
                ? th_func_param(constructor_type, 0) : nullptr;
        bool receiver_is_variant = false;
        if (receiver_parameter && receiver_parameter->type)
            th_named_info(th_resolve(receiver_parameter->type), nullptr,
                          &receiver_is_variant, nullptr);
        if (receiver_is_variant) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0022"),
                CORE_STRING_STATIC(
                    "variant types cannot define __init; initialize a selected arm through its payload type"),
                link_source_location(func));
        }
        if (!th_is_nil(symbol->error_type)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0049"),
                CORE_STRING_STATIC(
                    "__init cannot declare or infer a failure channel; use a fallible factory function"),
                link_source_location(func));
        }
        th_type *constructor_result = constructor_type
            && th_kind_of(constructor_type) == TH_FUNCTION
                ? th_func_ret(constructor_type) : nullptr;
        if (!th_is_nil(constructor_result)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0050"),
                CORE_STRING_STATIC(
                    "__init must return no value; use a factory function to produce a value"),
                link_source_location(func));
        }
    }
    if (core_string_eq(symbol->name, CORE_STRING_STATIC("__finalize"))) {
        th_type *finalizer_type = symbol->type
            ? th_resolve(symbol->type) : nullptr;
        size_t receiver_index = 0;
        parse_sym_entry_t *receiver = formal_symbol_at(
            grammar, params_node, 0, &receiver_index);
        uint8_t receiver_attrs = 0;
        bool has_receiver = finalizer_type
            && th_kind_of(finalizer_type) == TH_FUNCTION
            && th_func_nparams(finalizer_type) == 1
            && th_func_nkw(finalizer_type) == 0
            && !th_func_rest(finalizer_type)
            && receiver
            && core_string_eq(receiver->direction,
                CORE_STRING_STATIC("consume"))
            && receiver->type
            && th_ind_attrs(th_resolve(receiver->type), &receiver_attrs)
            && !(receiver_attrs & TH_NULLABLE);
        if (!has_receiver) {
            core_buffer_t message = {0};
            th_type *receiver_type = receiver && receiver->type
                ? th_resolve(receiver->type) : nullptr;
            th_type *element = nullptr;
            bool is_optional = receiver_type
                && (n00bb_option_type_element(receiver_type, &element)
                    || (th_ind_attrs(receiver_type, &receiver_attrs)
                        && (receiver_attrs & TH_NULLABLE)));
            bool is_reference = receiver_type
                && th_ind_attrs(receiver_type, &receiver_attrs);
            core_string_t shown = receiver_type
                ? n00bb_print_type(tu_state(unit)->types, receiver_type)
                : (core_string_t){0};
            if (!finalizer_type || th_kind_of(finalizer_type) != TH_FUNCTION
                || th_func_nparams(finalizer_type) != 1
                || th_func_nkw(finalizer_type) || th_func_rest(finalizer_type)
                || !receiver)
                core_buffer_printf(&message,
                    "__finalize must take exactly one parameter, the"
                    " `consume` receiver it finalizes");
            else if (!core_string_eq(receiver->direction,
                                     CORE_STRING_STATIC("consume")))
                core_buffer_printf(&message,
                    "the receiver of __finalize must be declared `consume`");
            else if (!is_optional && !is_reference)
                core_buffer_printf(&message,
                    "the receiver of __finalize must be a Ref to the value"
                    " being finalized, not %.*s",
                    (int)shown.u8_bytes, shown.data);
            else
                core_buffer_printf(&message,
                    "the receiver of __finalize cannot be optional (%.*s);"
                    " a finalizer always runs on a live value",
                    (int)shown.u8_bytes, shown.data);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0031"),
                core_buffer_string(&message), link_source_location(func));
            core_buffer_release(&message);
        }
        if (!th_is_nil(symbol->error_type)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0032"),
                CORE_STRING_STATIC("__finalize cannot fail"),
                link_source_location(func));
        }
        th_type *result = finalizer_type
            && th_kind_of(finalizer_type) == TH_FUNCTION
                ? th_func_ret(finalizer_type) : nullptr;
        if (!th_is_nil(result)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0033"),
                CORE_STRING_STATIC("__finalize must return no value"),
                link_source_location(func));
        }
    }
    (void)unit;
}

parse_sym_entry_t *
formal_symbol_at(parse_grammar_t *grammar, parse_tree_t *node,
                size_t target, size_t *index)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (NT_IS(grammar, node, "pos-param")
        || NT_IS(grammar, node, "default-param")
        || NT_IS(grammar, node, "trait-obj-tref")
        || NT_IS(grammar, node, "vargs-param")) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if ((*index)++ == target) return symbol;
        return nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_sym_entry_t *found = formal_symbol_at(grammar,
            core_tree_child(node, i), target, index);
        if (found) return found;
    }
    return nullptr;
}

th_type *
function_with_error_channel(th_ctx *types, th_type *function,
                            th_type *error_type)
{
    if (!types || !function || th_kind_of(function) != TH_FUNCTION
        ) return nullptr;
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nparams(function); i++)
        core_list_push(params, *th_func_param(function, i));
    for (size_t i = 0; i < th_func_nkw(function); i++)
        core_list_push(keywords, *th_func_kw(function, i));
    th_type *result = th_func_with_error(types, th_func_ret(function),
        error_type, params.data, params.len, th_func_rest(function),
        keywords.data, keywords.len);
    core_list_free(params);
    core_list_free(keywords);
    return result;
}

static core_string_t
print_user_type(th_ctx *types, th_type *type, bool constraints)
{
    th_print_options_t options = {
        .sequence = CORE_STRING_STATIC("Array"),
        .reference = CORE_STRING_STATIC("Ref"),
        .option_suffix = CORE_STRING_STATIC("?"),
        .omit_constraints = !constraints,
        .omit_nil_failure = true,
    };
    return th_print_with_options(types, type, &options);
}

core_string_t
n00bb_print_type_brief(th_ctx *types, th_type *type)
{
    return print_user_type(types, type, false);
}

core_string_t
n00bb_print_type(th_ctx *types, th_type *type)
{
    return print_user_type(types, type, true);
}

void
record_context_signature(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_tree_t *context, n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = parse_tree_sym(context);
    if (!symbol || !state || !state->types) return;
    parse_tree_t *spec = CHILD_NT(grammar, context, "context-spec");
    parse_tree_t *context_body = CHILD_NT(grammar, context, "context-body");
    parse_tree_t *context_hooks = CHILD_NT(grammar, context_body,
                                            "context-hooks");
    static const core_string_t hook_names[] = {
        CORE_STRING_STATIC("context-enter"),
        CORE_STRING_STATIC("context-exit"),
        CORE_STRING_STATIC("context-fail"),
        CORE_STRING_STATIC("context-ensure"),
    };
    for (size_t hook_index = 0;; hook_index++) {
        parse_tree_t *wrapper = parse_tree_get_nth_nt_child(
            context_hooks, (int32_t)hook_index);
        if (!wrapper) break;
        if (!NT_IS(grammar, wrapper, "context-hook")) continue;
        for (size_t i = 0; i < sizeof(hook_names) / sizeof(hook_names[0]);
             i++) {
            parse_tree_t *hook = child_nt(grammar, wrapper, hook_names[i]);
            core_string_t role = n00bb_context_hook_role(hook_names[i]);
            if (hook && (!role.data
                         || !parse_sym_set_binding(symbol, role, hook))) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0003"),
                    CORE_STRING_STATIC(
                        "could not cache a context hook on its declaration symbol"),
                    link_source_location(hook));
                return;
            }
        }
    }
    parse_tree_t *provides = CHILD_NT(grammar, spec, "provides-clause");
    parse_tree_t *error = CHILD_NT(grammar, spec, "error-type");
    parse_tree_t *fail_hook = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.ir.context.fail"));
    if (fail_hook && !error) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0005"),
            CORE_STRING_STATIC("a context fail hook requires a declared context failure type"));
        return;
    }

    core_list_t(th_param) constructor_params = core_list_new(th_param);
    th_param constructor_rest = {0};
    bool constructor_has_rest = false;
    n00bb_function_parameter_facts_t *parameter_facts = parse_sym_binding(
        symbol, N00BB_PARAMETER_FACTS_KEY);
    bool params_ok = resolve_function_parameter_facts(grammar, state,
        parameter_facts, &constructor_params, &constructor_rest,
        &constructor_has_rest);
    th_type *provided_type = provides
        ? parse_tree_type(CHILD_NT(grammar, provides, "type-spec"))
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    th_type *failure_type = error
        ? parse_tree_type(CHILD_NT(grammar, error, "one-tspec"))
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    th_type *signature = params_ok && provided_type && failure_type
        ? th_func_with_error(state->types, provided_type, failure_type,
            constructor_params.data, constructor_params.len,
            constructor_has_rest ? &constructor_rest : nullptr, nullptr, 0)
        : nullptr;
    if (!signature) {
        core_list_free(constructor_params);
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
            CORE_STRING_STATIC("could not form context constructor signature from its declaration"));
        return;
    }
    symbol->type = signature;
    symbol->error_type = failure_type;
    core_list_free(constructor_params);
}

void
record_context_hook_signature(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *hook, n00bb_tu_state_t *state)
{
    if (!hook || !state || !state->types) return;
    int hook_index = NT_IS(grammar, hook, "context-enter") ? 0
        : NT_IS(grammar, hook, "context-exit") ? 1
        : NT_IS(grammar, hook, "context-fail") ? 2
        : NT_IS(grammar, hook, "context-ensure") ? 3 : -1;
    if (hook_index < 0) return;
    parse_tree_t *context = parse_tree_parent(hook);
    while (context && !NT_IS(grammar, context, "context-def"))
        context = parse_tree_parent(context);
    parse_sym_entry_t *symbol = context
        ? parse_tree_sym(context) : nullptr;
    th_type *context_type = symbol && symbol->type
        ? th_resolve(symbol->type) : nullptr;
    if (!symbol || !context_type || th_kind_of(context_type) != TH_FUNCTION)
        return;
    th_type *provided_type = th_func_ret(context_type);
    th_type *failure_type = symbol->error_type;
    n00bb_function_parameter_facts_t *parameter_facts = parse_sym_binding(
        symbol, N00BB_PARAMETER_FACTS_KEY);
    core_list_t(th_param) hook_params = core_list_new(th_param);
    th_param rest = {0};
    bool has_rest = false;
    bool valid = resolve_function_parameter_facts(grammar, state,
        parameter_facts, &hook_params, &rest, &has_rest);

    parse_tree_t *hook_param_list = CHILD_NT(grammar, hook,
                                              "context-hook-params");
    parse_tree_t *hook_param = CHILD_NT(grammar, hook_param_list, "pos-param");
    if (hook_param) {
        parse_tree_t *hook_type_node = CHILD_NT(grammar, hook_param,
                                                 "type-spec");
        th_type *parameter_type = hook_type_node
            ? parse_tree_type(hook_type_node)
            : hook_index == 2 ? failure_type : provided_type;
        if (!parameter_type) valid = false;
        else {
            parse_sym_entry_t *parameter_symbol =
                parse_tree_sym(hook_param);
            if (parameter_symbol) {
                th_type *previous = parameter_symbol->type;
                if (previous && !th_unify_in_place(state->types, previous,
                                                    parameter_type))
                    valid = false;
                else propagate_symbol_type(parameter_symbol, state->types,
                                           previous, parameter_type);
            }
            parse_tree_set_type(hook_param, parameter_type);
            core_list_push(hook_params, ((th_param){
                .mode = TH_IN, .type = parameter_type,
            }));
        }
    }
    th_type *hook_return = hook_index == 0 ? provided_type
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    th_type *hook_failure = hook_index == 3
        ? th_name(state->types, CORE_STRING_STATIC("nil")) : failure_type;
    th_type *hook_type = valid && hook_return && hook_failure
        ? th_func_with_error(state->types, hook_return, hook_failure,
            hook_params.data, hook_params.len,
            has_rest ? &rest : nullptr, nullptr, 0) : nullptr;
    core_list_free(hook_params);
    if (!hook_type) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC(
                "could not form context hook signature from its declaration"));
        return;
    }
    parse_tree_set_type(hook, hook_type);
}

n00bb_link_use_site_t *
link_use_site_for_node(n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!state || !node) return nullptr;
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts && facts->first_link_use_site
        ? &state->link_use_sites.data[facts->first_link_use_site - 1]
        : nullptr;
}

bool
append_link_use_site(n00bb_tu_state_t *state, n00bb_link_use_site_t site)
{
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, site.node);
    if (!facts) return false;
    size_t index = state->link_use_sites.len;
    if (!core_list_try_push(state->link_use_sites, site)) return false;
    if (!facts->first_link_use_site) facts->first_link_use_site = index + 1;
    if (facts->last_link_use_site)
        state->link_use_sites.data[facts->last_link_use_site - 1]
            .next_for_node = index + 1;
    facts->last_link_use_site = index + 1;
    return true;
}

n00bb_link_use_site_t *
link_use_site_for_dispatch(n00bb_tu_state_t *state, parse_tree_t *node,
                           core_string_t trait)
{
    if (!state || !node) return nullptr;
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    for (size_t slot = facts ? facts->first_link_use_site : 0; slot;
         slot = state->link_use_sites.data[slot - 1].next_for_node)
        if (core_string_eq(state->link_use_sites.data[slot - 1]
                               .dispatch_trait, trait))
            return &state->link_use_sites.data[slot - 1];
    return nullptr;
}

th_type *
explicit_trait_method_signature(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *method, th_ctx *types,
                                th_type **failure)
{
    parse_tree_t *signature = CHILD_NT(grammar, method,
                                       "trait-func-type-signature");
    parse_tree_t *params = CHILD_NT(grammar, signature, "trait-param-decl");
    parse_tree_t *receiver = CHILD_NT(grammar, params, "trait-obj-tref");
    parse_tree_t *result = CHILD_NT(grammar, signature, "function-result");
    parse_tree_t *success = CHILD_NT(grammar, result, "return-type");
    parse_tree_t *error = CHILD_NT(grammar, result, "error-type");
    if (!params || !receiver) return nullptr;
    core_list_t(th_param) parameters = core_list_new(th_param);
    parse_tree_t *receiver_type_node = CHILD_NT(grammar, receiver,
                                                 "tspec-tvar");
    th_type *receiver_type = receiver_type_node
        ? parse_tree_type(receiver_type_node) : nullptr;
    parse_sym_entry_t *receiver_symbol = parse_tree_sym(receiver);
    if (receiver_symbol && receiver_type) {
        th_type *bound = receiver_symbol->type
            ? th_unify_in_place(types, receiver_symbol->type, receiver_type)
            : receiver_type;
        if (!bound) return nullptr;
        receiver_symbol->type = bound;
        parse_tree_set_type(receiver, bound);
        receiver_type = bound;
    }
    th_mode receiver_mode = receiver_symbol && receiver_symbol->has_mode
        ? receiver_symbol->mode : TH_IN;
    bool collected = receiver_type != nullptr;
    if (collected)
        core_list_push(parameters, ((th_param){
            .mode = receiver_mode,
            .type = receiver_type,
        }));
    th_param rest = {0};
    bool has_rest = false;
    parse_sym_entry_t *method_symbol = parse_tree_sym(method);
    n00bb_function_parameter_facts_t *parameter_facts = method_symbol
        ? parse_sym_binding(method_symbol,
            N00BB_PARAMETER_FACTS_KEY) : nullptr;
    collected = collected && resolve_function_parameter_facts(grammar,
        tu_state(unit), parameter_facts, &parameters, &rest, &has_rest);
    th_type *return_type = success
        ? parse_tree_type(CHILD_NT(grammar, success, "union-tspec"))
        : th_name(types, CORE_STRING_STATIC("nil"));
    th_type *failure_type = error
        ? parse_tree_type(CHILD_NT(grammar, error, "one-tspec"))
        : th_name(types, CORE_STRING_STATIC("nil"));
    th_type *method_type = collected && return_type && failure_type
        ? th_func_with_error(types, return_type, failure_type,
            parameters.data, parameters.len, has_rest ? &rest : nullptr,
            nullptr, 0) : nullptr;
    core_list_free(parameters);
    if (failure) *failure = failure_type;
    return method_type;
}

static bool
parameter_index_in(parse_grammar_t *grammar, parse_tree_t *node,
                  parse_tree_t *target, size_t *index)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if (NT_IS(grammar, node, "pos-param")
        || NT_IS(grammar, node, "default-param")
        || NT_IS(grammar, node, "vargs-param")) {
        if (node == target) return true;
        (*index)++;
        return false;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (parameter_index_in(grammar, core_tree_child(node, i), target,
                               index)) return true;
    return false;
}


void
bind_declared_parameter(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node,
                        n00bb_tu_state_t *state)
{
    if (!state || !state->types || !node || core_tree_is_leaf(node)) return;
    parse_tree_t *callable = parse_tree_parent(node);
    while (callable && !NT_IS(grammar, callable, "func-def")
           && !NT_IS(grammar, callable, "callback-role")
           && !NT_IS(grammar, callable, "context-def"))
        callable = parse_tree_parent(callable);
    if (!callable) return;
    parse_tree_t *declared_type = CHILD_NT(grammar, node, "type-spec");
    parse_sym_entry_t *parameter = parse_tree_sym(node);
    bool variadic = NT_IS(grammar, node, "vargs-param");
    parse_sym_entry_t *callable_symbol = parse_tree_sym(callable);

    if (declared_type && parameter) {
        th_type *declared = parse_tree_type(declared_type);
        if (variadic && declared) {
            if (parameter->has_mode && parameter->mode == TH_INOUT)
                declared = th_ind(state->types, 0, declared);
            declared = th_array(state->types, declared);
        }
        th_type *binding_type = parameter->type && declared
            ? th_unify_in_place(state->types, parameter->type, declared)
            : parameter->type ? parameter->type : declared;
        if (!binding_type) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC(
                    "parameter symbol type does not match its declared type"),
                link_source_location(node));
            return;
        }
        parameter->type = binding_type;
        parse_tree_set_type(node, binding_type);
        return;
    }
    if (!callable_symbol || !callable_symbol->type) return;
    th_type *function_type = callable_symbol->type;
    if (!function_type || th_kind_of(function_type) != TH_FUNCTION) return;
    size_t index = 0;
    parse_tree_t *params = NT_IS(grammar, callable, "func-def")
        ? CHILD_NT(grammar, CHILD_NT(grammar, callable, "func-type-signature"),
                   "param-decl")
        : NT_IS(grammar, callable, "callback-role")
            ? CHILD_NT(grammar, CHILD_NT(grammar, callable,
                                         "func-type-signature"),
                       "param-decl")
        : CHILD_NT(grammar, callable, "param-decl");
    if (!parameter_index_in(grammar, params, node, &index)) return;
    const th_param *declared = variadic
        ? th_func_rest(function_type)
        : th_func_param(function_type, index);
    if (!declared || !parameter) return;
    th_type *binding_type = declared->type;
    if (variadic && binding_type) {
        if (parameter->has_mode && parameter->mode == TH_INOUT)
            binding_type = th_ind(state->types, 0, binding_type);
        binding_type = th_array(state->types, binding_type);
    }
    if (!binding_type) return;
    if (parameter->type) {
        th_type *unified = th_unify_in_place(state->types, parameter->type,
                                             binding_type);
        if (!unified) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("parameter declaration type does not match function signature"));
            return;
        }
        parameter->type = unified;
    }
    else parameter->type = binding_type;
    parse_tree_set_type(node, parameter->type);
}

static bool
callback_type_equal(th_ctx *types, th_type *left, th_type *right)
{
    return left && right
        && th_hash(types, th_resolve(left)) == th_hash(types,
                                                       th_resolve(right));
}

static bool
callback_signature_tail_matches(th_ctx *types, th_type *handler,
                                size_t skipped, th_type *call)
{
    handler = handler ? th_resolve(handler) : nullptr;
    call = call ? th_resolve(call) : nullptr;
    if (!handler || !call || th_kind_of(handler) != TH_FUNCTION
        || th_kind_of(call) != TH_FUNCTION
        || th_func_nparams(handler) != th_func_nparams(call) + skipped
        || th_func_nkw(handler) != th_func_nkw(call)
        || !callback_type_equal(types, th_func_ret(handler),
                                th_func_ret(call))
        || !callback_type_equal(types, th_func_error(handler),
                                th_func_error(call))) return false;
    for (size_t i = 0; i < th_func_nparams(call); i++) {
        const th_param *actual = th_func_param(handler, i + skipped);
        const th_param *visible = th_func_param(call, i);
        if (!actual || !visible || actual->mode != visible->mode
            || !callback_type_equal(types, actual->type, visible->type))
            return false;
    }
    const th_param *actual_rest = th_func_rest(handler);
    const th_param *visible_rest = th_func_rest(call);
    if ((actual_rest != nullptr) != (visible_rest != nullptr)
        || (actual_rest && (actual_rest->mode != visible_rest->mode
            || !callback_type_equal(types, actual_rest->type,
                                    visible_rest->type)))) return false;
    for (size_t i = 0; i < th_func_nkw(call); i++) {
        const th_kwparam *actual = th_func_kw(handler, i);
        const th_kwparam *visible = th_func_kw(call, i);
        if (!actual || !visible || actual->mode != visible->mode
            || !core_string_eq(actual->name, visible->name)
            || !callback_type_equal(types, actual->type, visible->type))
            return false;
    }
    return true;
}

void
record_callback_role_signature(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *role,
                               n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = parse_tree_sym(role);
    parse_tree_t *signature = CHILD_NT(grammar, role,
                                        "func-type-signature");
    n00bb_function_parameter_facts_t *facts = symbol
        ? parse_sym_binding(symbol,
            N00BB_PARAMETER_FACTS_KEY) : nullptr;
    core_list_t(th_param) parameters = core_list_new(th_param);
    th_param rest = {0};
    bool has_rest = false;
    bool valid = symbol && signature && state && state->types
        && resolve_function_parameter_facts(grammar, state, facts,
            &parameters, &rest, &has_rest);
    parse_tree_t *result = CHILD_NT(grammar, signature, "function-result");
    parse_tree_t *return_node = CHILD_NT(grammar, result, "return-type");
    parse_tree_t *error_node = CHILD_NT(grammar, result, "error-type");
    parse_tree_t *return_type = CHILD_NT(grammar, return_node,
                                         "union-tspec");
    parse_tree_t *error_type = CHILD_NT(grammar, error_node, "one-tspec");
    th_type *success = return_node
        ? return_type ? parse_tree_type(return_type) : nullptr
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    th_type *failure = error_node
        ? error_type ? parse_tree_type(error_type) : nullptr
        : th_name(state->types, CORE_STRING_STATIC("nil"));
    th_type *contract = valid && success && failure
        ? th_func_with_error(state->types, success, failure,
            parameters.data, parameters.len, has_rest ? &rest : nullptr,
            nullptr, 0) : nullptr;
    core_list_free(parameters);
    if (!contract) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0023"),
            CORE_STRING_STATIC("could not form the callback role signature"),
            link_source_location(role));
        return;
    }
    symbol->type = contract;
    symbol->error_type = failure;
    symbol->callable_contract = th_env_signature_contract(
        state->types, contract, failure);
    parse_tree_set_type(role, contract);
}

static parse_sym_entry_t *
callback_role_symbol(parse_sym_entry_t *callback, core_string_t name)
{
    for (parse_sym_entry_t *role = callback && callback->member_scope
            ? callback->member_scope->first_in_scope : nullptr;
         role; role = role->next_in_scope)
        if (role->kind == PARSE_SYM_FUNCTION
            && core_string_eq(role->name, name))
            return role;
    return nullptr;
}

th_type *
callback_setup_dispatch_contract(th_ctx *types, th_type *setup_type)
{
    th_type *setup = setup_type ? th_resolve(setup_type) : nullptr;
    size_t parameter_count = setup && th_kind_of(setup) == TH_FUNCTION
        ? th_func_nparams(setup) : 0;
    if (!types || !parameter_count) return nullptr;
    th_param *parameters = core_alloc_array(th_param, parameter_count);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    if (!parameters) return nullptr;
    for (size_t i = 0; i < parameter_count; i++)
        parameters[i] = *th_func_param(setup, i);
    parameters[0].type = th_var(types);
    for (size_t i = 0; i < th_func_nkw(setup); i++)
        core_list_push(keywords, *th_func_kw(setup, i));
    th_type *signature = th_func_with_error(types, th_func_ret(setup),
        th_func_error(setup), parameters, parameter_count,
        th_func_rest(setup), keywords.data, keywords.len);
    core_free(parameters);
    core_list_free(keywords);
    return signature ? th_env_signature_contract(types, signature,
        th_func_error(signature)) : nullptr;
}

void
finalize_callback_contract(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *declaration,
                           n00bb_tu_state_t *state)
{
    (void)grammar;
    parse_sym_entry_t *symbol = parse_tree_sym(declaration);
    if (!symbol || !state || !state->types) return;
    core_buffer_t qualified = {0};
    append_module_type_name(unit, symbol->name, &qualified);
    th_type *nominal = qualified.data
        ? th_name(state->types, core_buffer_string(&qualified)) : nullptr;
    core_buffer_release(&qualified);
    if (!nominal) return;
    symbol->type = nominal;
    symbol->adt_kind = CORE_STRING_STATIC("callback");
    parse_tree_set_type(declaration, nominal);

    parse_sym_entry_t *call = callback_role_symbol(symbol,
        CORE_STRING_STATIC("call"));
    parse_sym_entry_t *handler = callback_role_symbol(symbol,
        CORE_STRING_STATIC("handler"));
    parse_sym_entry_t *setup = callback_role_symbol(symbol,
        CORE_STRING_STATIC("setup"));
    th_type *call_type = call ? call->type : nullptr;
    th_type *handler_type = handler ? handler->type : nullptr;
    bool stateful = setup != nullptr;
    th_type *state_type = nullptr;
    bool valid = call_type && handler_type;
    if (valid && !stateful)
        valid = callback_signature_tail_matches(state->types, handler_type,
                                                0, call_type);
    if (valid && stateful) {
        const th_param *state_parameter = th_func_param(
            th_resolve(handler_type), 0);
        state_type = state_parameter ? state_parameter->type : nullptr;
        valid = state_parameter && state_parameter->mode == TH_INOUT
            && callback_signature_tail_matches(state->types, handler_type,
                                                1, call_type);
        th_type *setup_type = setup->type ? th_resolve(setup->type) : nullptr;
        const th_param *handler_ref = setup_type
            && th_kind_of(setup_type) == TH_FUNCTION
            ? th_func_param(setup_type, 0) : nullptr;
        const th_param *state_ref = setup_type
            && th_kind_of(setup_type) == TH_FUNCTION
            ? th_func_param(setup_type, 1) : nullptr;
        th_type *handler_referent = handler_ref
            ? th_ind_referent(th_resolve(handler_ref->type)) : nullptr;
        th_type *state_referent = state_ref
            ? th_ind_referent(th_resolve(state_ref->type)) : nullptr;
        valid = valid && handler_ref && state_ref
            && callback_type_equal(state->types, handler_referent,
                                   handler_type)
            && callback_type_equal(state->types, state_referent, state_type)
            && th_is_nil(th_func_ret(setup_type));
    }
    if (!valid) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0023"),
            CORE_STRING_STATIC(
                "callback call, handler, and setup roles do not form a valid contract"),
            link_source_location(declaration));
        return;
    }
    n00bb_callback_contract_t *contract = core_arena_alloc(
        &state->semantic_values, sizeof(*contract));
    if (!contract) return;
    *contract = (n00bb_callback_contract_t){
        .call = call,
        .handler = handler,
        .setup = setup,
        .state_type = state_type,
        .stateful = stateful,
    };
    if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-contract"), contract))
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0023"),
            CORE_STRING_STATIC("could not retain the callback contract"),
            link_source_location(declaration));
}

bool
resolve_function_parameter_facts(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state,
                                 n00bb_function_parameter_facts_t *facts,
                                 core_list_t(th_param) *params,
                                 th_param *rest, bool *has_rest)
{
    for (size_t i = 0; facts && i < facts->param_count; i++) {
        n00bb_function_parameter_ref_t *fact = &facts->params[i];
        parse_tree_t *node = fact->node;
        if (!node) return false;
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        parse_tree_t *owner = parse_tree_parent(node);
        while (owner && !NT_IS(grammar, owner, "func-def")
               && !NT_IS(grammar, owner, "trait-spec")
               && !NT_IS(grammar, owner, "callback-role")
               && !NT_IS(grammar, owner, "context-def"))
            owner = parse_tree_parent(owner);
        parse_sym_entry_t *function = owner
            ? parse_tree_sym(owner) : nullptr;
        bool body_local = function && NT_IS(grammar, owner, "func-def")
            && parse_tree_type(node)
            && parse_sym_binding(function,
                CORE_STRING_STATIC("n00bb.body-function-signature"));
        th_type *type = body_local ? parse_tree_type(node)
            : symbol ? symbol->type : nullptr;
        if (!type) type = parse_tree_type(node);
        if (!type && symbol) type = symbol->type;
        if (!type) type = th_var(state->types);
        if (!type) return false;
        th_comb comb;
        if (!fact->variadic && th_sum_comb(th_resolve(type), &comb)
            && comb == TH_UNION) {
            if (!fact->generic_union_type) {
                size_t count = th_sum_len(type);
                th_type **alternatives = core_arena_alloc(
                    &state->semantic_values, count * sizeof(*alternatives));
                th_type *variable = th_var(state->types);
                if (!alternatives || !variable) return false;
                for (size_t j = 0; j < count; j++)
                    alternatives[j] = th_sum_arm(type, j);
                if (!th_var_constrain(state->types, variable,
                    (th_constraint){.kind = TH_CON_ONE_OF,
                        .one_of = {.types = alternatives, .n = count}}))
                    return false;
                fact->generic_union_type = variable;
            }
            th_type *previous = symbol ? symbol->type : nullptr;
            type = fact->generic_union_type;
            parse_tree_set_type(node, type);
            if (symbol) propagate_symbol_type(symbol, state->types,
                                              previous, type);
        }
        th_mode mode = symbol && symbol->has_mode ? symbol->mode : TH_IN;
        if (fact->variadic && fact->has_type_node) {
            parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
            if (type_node && parse_tree_type(type_node))
                type = parse_tree_type(type_node);
        }
        else if (fact->variadic && symbol && symbol->type) {
            th_type *resolved = th_resolve(symbol->type);
            if (th_kind_of(resolved) == TH_SEQUENCE) {
                type = th_seq_elem(resolved);
                if (mode == TH_INOUT && th_ind_referent(type))
                    type = th_ind_referent(type);
            }
        }
        if (!body_local && !fact->variadic && parse_tree_type(node)
            && parse_tree_type(node) != type) {
            th_type *unified = th_unify_in_place(state->types, type,
                                                 parse_tree_type(node));
            if (unified) type = unified;
        }
        if (fact->variadic) {
            th_type *binding = symbol && symbol->type
                ? th_resolve(symbol->type) : nullptr;
            th_type *element = mode == TH_INOUT
                ? th_ind(state->types, 0, type) : type;
            if (symbol && (fact->has_type_node || !binding
                           || th_kind_of(binding) != TH_SEQUENCE))
                symbol->type = th_array(state->types, element);
            parse_tree_set_type(node, symbol ? symbol->type
                                    : th_array(state->types, element));
            *rest = (th_param){.mode = mode, .type = type};
            *has_rest = true;
        }
        else {
            parse_tree_set_type(node, type);
            if (symbol) propagate_symbol_type(symbol, state->types,
                symbol->type, type);
            core_list_push(*params, ((th_param){
                .mode = mode,
                .type = type,
            }));
        }
    }
    return true;
}

void
bind_function_body_signature(core_translation_unit_t *unit,
                             parse_tree_t *signature,
                             n00bb_tu_state_t *state)
{
    parse_tree_t *function = parse_tree_parent(signature);
    parse_sym_entry_t *symbol = function
        ? parse_tree_sym(function) : nullptr;
    n00bb_signature_typevars_t *variables = symbol
        ? parse_sym_binding(symbol,
            N00BB_SIGNATURE_TYPEVARS_KEY) : nullptr;
    if (!symbol || !variables || !variables->len || !symbol->type) return;

    th_type *body_type = th_type_clone(state->types, symbol->type);
    if (!body_type || th_kind_of(body_type) != TH_FUNCTION) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC(
                "could not copy the function signature for its body"),
            link_source_location(signature));
        return;
    }

    th_type **declaration_vars = nullptr;
    th_type **body_vars = nullptr;
    size_t declaration_count = th_free_vars(state->types, symbol->type,
        &declaration_vars);
    size_t body_count = th_free_vars(state->types, body_type, &body_vars);
    bool mapped = declaration_count == body_count;
    for (size_t i = 0; mapped && i < declaration_count; i++) {
        core_string_t original = th_var_name(declaration_vars[i]);
        core_string_t copy = th_var_name(body_vars[i]);
        mapped = original.data ? copy.data && core_string_eq(original, copy)
                               : !copy.data;
    }
    if (!mapped) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not map body type variables to their declaration"),
            link_source_location(signature));
        return;
    }
    n00bb_body_typevar_map_t *typevar_map = core_arena_alloc(
        &state->semantic_values, sizeof(*typevar_map));
    if (!typevar_map || !parse_sym_set_binding(symbol,
            N00BB_BODY_TYPEVAR_MAP_KEY, typevar_map)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not retain the body type-variable map"),
            link_source_location(signature));
        return;
    }
    *typevar_map = (n00bb_body_typevar_map_t){
        .declaration = declaration_vars,
        .body = body_vars,
        .count = body_count,
    };

    n00bb_function_parameter_facts_t *facts = parse_sym_binding(symbol,
        N00BB_PARAMETER_FACTS_KEY);
    for (size_t i = 0; facts && i < facts->param_count; i++) {
        n00bb_function_parameter_ref_t *fact = &facts->params[i];
        const th_param *formal = fact->variadic
            ? th_func_rest(body_type)
            : th_func_param(body_type, i);
        th_type *type = formal ? formal->type : nullptr;
        if (fact->variadic && type) type = th_array(state->types, type);
        parse_sym_entry_t *parameter_symbol = fact->node
            ? parse_tree_sym(fact->node) : nullptr;
        if (!parameter_symbol || !type) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "could not bind a body-local type to a declared parameter"),
                link_source_location(fact->node ? fact->node : signature));
            return;
        }
        th_type *previous = parameter_symbol->type;
        parameter_symbol->type = type;
        parse_tree_set_type(fact->node, type);
        propagate_symbol_type(parameter_symbol, state->types, previous, type);
    }
    if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.body-function-signature"), body_type))
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not retain the body-local function signature"),
            link_source_location(signature));
}

core_list_decl(th_member);
typedef struct {
    ir_uint_t mask;
} n00bb_flagset_eval_t;

void
append_module_type_name(core_translation_unit_t *unit, core_string_t name,
                        core_buffer_t *buffer)
{
    core_buffer_printf(buffer, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes, unit->module.identifier.data,
        (int)name.u8_bytes, name.data);
}

bool
finalize_variant_symbol_type(parse_grammar_t *grammar, parse_tree_t *node,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    parse_scope_t *members = symbol ? symbol->member_scope : nullptr;
    if (!symbol || !members || !state || !state->types) return false;

    core_list_t(th_member) arms = core_list_new(th_member);
    core_list_t(parse_sym_entry_ptr_t) ordered =
        variant_arms_in_source_order(grammar, members);
    for (size_t i = 0; i < ordered.len; i++) {
        parse_sym_entry_t *arm = ordered.data[i];
        if (!arm->type) {
            core_list_free(ordered);
            core_list_free(arms);
            return false;
        }
        core_list_push(arms, ((th_member){
            .name = arm->name,
            .type = arm->type,
        }));
    }
    core_list_free(ordered);

    core_buffer_t qualified = {0};
    append_module_type_name(unit, symbol->name, &qualified);
    core_string_t qualified_name = core_buffer_string(&qualified);
    th_type *template = qualified.data
        ? variant_template_type(state->types, qualified_name, symbol)
        : nullptr;
    th_type *definition = template
        ? th_record(state->types, arms.data, arms.len) : nullptr;
    th_type *type = definition && th_type_define(state->types, template,
        definition) ? th_variant(state->types, qualified_name, nullptr, 0)
        : nullptr;
    core_list_free(arms);
    core_buffer_release(&qualified);
    if (!type) return false;
    symbol->type = type;
    parse_tree_set_type(node, type);
    return true;
}


bool
finalize_enum_symbol_type(parse_grammar_t *grammar, parse_tree_t *node,
                          core_translation_unit_t *unit,
                          n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    if (!symbol || !unit || !state || !state->types) return false;
    core_buffer_t qualified = {0};
    append_module_type_name(unit, symbol->name, &qualified);
    th_type *type = qualified.data
        ? th_name(state->types, core_buffer_string(&qualified)) : nullptr;
    core_buffer_release(&qualified);
    if (!type) return false;
    symbol->type = type;
    symbol->adt_kind = CORE_STRING_STATIC("enum");
    parse_tree_set_type(node, type);
    for (parse_sym_entry_t *item = symbol->member_scope
            ? symbol->member_scope->first_in_scope : nullptr;
         item; item = item->next_in_scope) {
        if (!item->decl_node
            || !NT_IS(grammar, item->decl_node, "enum-item"))
            continue;
        item->kind = PARSE_SYM_ENUM_CONST;
        item->type = type;
        parse_tree_set_type(item->decl_node, type);
    }
    return true;
}

parse_tree_t *
flagset_declaration_ancestor(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at && !core_tree_is_leaf(at);
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "flagset-stmt")) return at;
    }
    return nullptr;
}

bool
finalize_flagset_symbol_type(parse_grammar_t *grammar, parse_tree_t *node,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state)
{
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    if (!symbol || !unit || !state || !state->types) return false;
    if (!symbol->type) {
        core_buffer_t qualified = {0};
        append_module_type_name(unit, symbol->name, &qualified);
        symbol->type = qualified.data
            ? th_name(state->types, core_buffer_string(&qualified)) : nullptr;
        core_buffer_release(&qualified);
    }
    if (!symbol->type) return false;
    parse_tree_set_type(node, symbol->type);

    for (parse_sym_entry_t *item = symbol->member_scope
            ? symbol->member_scope->first_in_scope : nullptr;
         item; item = item->next_in_scope) {
        if (!item->decl_node
            || !NT_IS(grammar, item->decl_node, "flagset-item")) continue;
        item->kind = PARSE_SYM_ENUM_CONST;
        item->type = symbol->type;
        item->mutability = PARSE_SYM_CONST;
        parse_sym_set_phases(item, PARSE_SYM_PHASE_BOTH,
                             PARSE_SYM_PHASE_NONE,
                             PARSE_SYM_PHASE_COMPTIME);
        parse_sym_set_storage_duration(item, PARSE_SYM_STORAGE_COMPTIME);
        parse_tree_set_type(item->decl_node, symbol->type);
    }
    n00bb_flagset_info_t *info = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.flagset-info"));
    if (!info) {
        info = core_arena_alloc(&state->semantic_values, sizeof(*info));
        if (!info) return false;
        *info = (n00bb_flagset_info_t){0};
        if (!parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.flagset-info"), info)) return false;
    }
    return true;
}


static bool
flagset_node_mask(n00bb_tu_state_t *state, parse_tree_t *node,
                  ir_uint_t *mask)
{
    n00bb_flagset_eval_t *value = state && node
        ? core_dict_get(&state->flagset_values, node, nullptr) : nullptr;
    if (!value) return false;
    if (mask) *mask = value->mask;
    return true;
}

static bool
cache_flagset_node_mask(n00bb_tu_state_t *state, parse_tree_t *node,
                        ir_uint_t mask)
{
    n00bb_flagset_eval_t *value = core_arena_alloc(
        &state->semantic_values, sizeof(*value));
    if (!value) return false;
    *value = (n00bb_flagset_eval_t){.mask = mask};
    core_dict_put(&state->flagset_values, node, value);
    return true;
}

static core_string_t
flagset_operator_spelling(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    core_option_t(core_string_t) value = token
        ? parse_token_value(token) : core_option_none(core_string_t);
    return core_option_is_set(value) ? core_option_get(value)
                                     : (core_string_t){0};
}

bool
cache_flagset_semantics(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        n00bb_tu_state_t *state, parse_tree_t *node,
                        core_buffer_t *reason)
{
    parse_tree_t *declaration = flagset_declaration_ancestor(grammar, node);
    if (!declaration) return true;
    if (!finalize_flagset_symbol_type(grammar, declaration, unit, state))
        return false;
    parse_sym_entry_t *owner = parse_tree_sym(declaration);
    n00bb_flagset_info_t *info = owner ? parse_sym_binding(owner,
        CORE_STRING_STATIC("n00bb.flagset-info")) : nullptr;
    if (!owner || !info) return false;
    if (node == declaration && !info->atomic_count) {
        core_buffer_puts(reason,
            "a flagset needs at least one primitive flag");
        return false;
    }

    if (NT_IS(grammar, node, "flagset-name")) {
        parse_tree_t *name_node = core_tree_num_children(node) == 1
            ? core_tree_child(node, 0) : nullptr;
        parse_sym_entry_t *symbol = name_node && core_tree_is_leaf(name_node)
            ? parse_symtab_lookup_in(owner->member_scope,
                                     parse_tree_leaf_text(name_node))
            : nullptr;
        n00bb_flagset_value_t *value = symbol && symbol->has_comptime_value
            ? symbol->comptime_value : nullptr;
        if (!value && name_node && core_tree_is_leaf(name_node)) {
            core_string_t name = parse_tree_leaf_text(name_node);
            core_buffer_printf(reason,
                "'%.*s' is not an earlier flag of this flagset",
                (int)name.u8_bytes, name.data);
        }
        if (!value || !symbol->type
            || !parse_tree_set_resolution_symbol(node, symbol)) return false;
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, value->mask);
    }
    if (NT_IS(grammar, node, "flagset-primary")) {
        ir_uint_t mask = 0;
        if (core_tree_num_children(node) == 1) {
            parse_tree_t *child = core_tree_child(node, 0);
            if (!core_tree_is_leaf(child)
                && !NT_IS(grammar, child, "nil-keyword")
                && !flagset_node_mask(state, child, &mask)) return false;
        }
        else if (core_tree_num_children(node) == 3
                 && !flagset_node_mask(state, core_tree_child(node, 1),
                                       &mask)) return false;
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, mask);
    }
    if (NT_IS(grammar, node, "flagset-unary")) {
        size_t count = core_tree_num_children(node);
        parse_tree_t *operand = core_tree_child(node, count - 1);
        ir_uint_t mask = 0;
        if (!flagset_node_mask(state, operand, &mask)) return false;
        if (count == 2) mask = info->all_mask ^ mask;
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, mask);
    }
    bool operation = NT_IS(grammar, node, "flagset-union")
        || NT_IS(grammar, node, "flagset-xor")
        || NT_IS(grammar, node, "flagset-intersection");
    if (operation) {
        size_t count = core_tree_num_children(node);
        ir_uint_t mask = 0;
        if (!count || !flagset_node_mask(state, core_tree_child(node, 0),
                                         &mask)) return false;
        for (size_t i = 1; i + 1 < count; i += 2) {
            ir_uint_t right = 0;
            core_string_t op = flagset_operator_spelling(
                core_tree_child(node, i));
            if (!flagset_node_mask(state, core_tree_child(node, i + 1),
                                   &right)) return false;
            if (core_string_eq(op, CORE_STRING_STATIC("+"))
                || core_string_eq(op, CORE_STRING_STATIC("|"))) mask |= right;
            else if (core_string_eq(op, CORE_STRING_STATIC("-")))
                mask &= ~right;
            else if (core_string_eq(op, CORE_STRING_STATIC("^"))) mask ^= right;
            else if (core_string_eq(op, CORE_STRING_STATIC("&"))) mask &= right;
            else return false;
        }
        mask &= info->all_mask;
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, mask);
    }
    if (NT_IS(grammar, node, "flagset-value")) {
        ir_uint_t mask = 0;
        if (core_tree_num_children(node) != 1
            || !flagset_node_mask(state, core_tree_child(node, 0), &mask))
            return false;
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, mask);
    }
    if (NT_IS(grammar, node, "flagset-item")) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if (!symbol) return false;
        bool atomic = core_tree_num_children(node) == 1;
        ir_uint_t mask = 0;
        uint32_t order = info->atomic_count;
        int32_t declaration_start = core_tree_node_value(node).start;
        if (atomic) {
            if (order >= 128) {
                core_buffer_puts(reason,
                    "a flagset holds at most 128 primitive flags");
                return false;
            }
            mask = (ir_uint_t)1 << order;
        }
        else if (!flagset_node_mask(state,
                     core_tree_child(node, 2), &mask)) return false;
        for (parse_sym_entry_t *item = owner->member_scope->first_in_scope;
             item; item = item->next_in_scope) {
            if (!item->decl_node || item->decl_node == node
                || core_tree_node_value(item->decl_node).start
                       >= declaration_start) continue;
            n00bb_flagset_value_t *prior = item->has_comptime_value
                ? item->comptime_value : nullptr;
            if (prior && prior->mask == mask) {
                core_buffer_printf(reason,
                    "'%.*s' has the same value as '%.*s'; each flagset "
                    "value must be distinct",
                    (int)symbol->name.u8_bytes, symbol->name.data,
                    (int)item->name.u8_bytes, item->name.data);
                return false;
            }
        }
        n00bb_flagset_value_t *value = core_arena_alloc(
            &state->semantic_values, sizeof(*value));
        if (!value) return false;
        *value = (n00bb_flagset_value_t){
            .mask = mask,
            .declaration_order = order,
            .atomic = atomic,
        };
        symbol->comptime_value = value;
        symbol->has_comptime_value = true;
        if (atomic) {
            info->all_mask |= mask;
            info->atomic_count++;
        }
        parse_tree_set_type(node, owner->type);
        return cache_flagset_node_mask(state, node, mask);
    }
    return true;
}


bool
n00bb_parse_switch_integer(core_string_t text, ir_lit_t *literal)
{
    unsigned base = 10;
    bool hex = false;
    size_t i = 0;
    if (text.u8_bytes >= 2 && text.data[0] == '0') {
        if (text.data[1] == 'x' || text.data[1] == 'X') {
            base = 16;
            hex = true;
            i = 2;
        }
        else if (text.data[1] == 'b' || text.data[1] == 'B') {
            base = 2;
            i = 2;
        }
        else if (text.data[1] == 'o' || text.data[1] == 'O') {
            base = 8;
            i = 2;
        }
    }
    ir_uint_t value = 0;
    bool digit_seen = false;
    for (; i < text.u8_bytes; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '_') continue;
        unsigned digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : base;
        if (digit >= base || value > (~(ir_uint_t)0 - digit) / base)
            return false;
        value = value * base + digit;
        digit_seen = true;
    }
    if (!digit_seen) return false;
    *literal = (ir_lit_t){.mag = value, .hex = hex};
    return true;
}

parse_tree_t *
n00bb_simple_lvalue(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (parse_tree_resolution(node).kind
            == PARSE_NODE_RESOLUTION_SYMBOL) return node;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

parse_sym_entry_t *
simple_expression_symbol(parse_tree_t *node)
{
    parse_tree_t *lvalue = n00bb_simple_lvalue(node);
    parse_node_resolution_t resolution = lvalue
        ? parse_tree_resolution(lvalue) : (parse_node_resolution_t){0};
    parse_sym_entry_t *symbol = resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM)
        ? symbol : nullptr;
}

parse_sym_entry_t *
simple_expression_bound_symbol(parse_tree_t *node)
{
    parse_tree_t *lvalue = n00bb_simple_lvalue(node);
    parse_node_resolution_t resolution = lvalue
        ? parse_tree_resolution(lvalue) : (parse_node_resolution_t){0};
    parse_sym_entry_t *symbol = resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol
        : lvalue ? parse_tree_sym(lvalue) : nullptr;
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM
                      || symbol->kind == PARSE_SYM_FUNCTION)
        ? symbol : nullptr;
}

bool
n00bb_default_references(parse_tree_t *node, parse_sym_entry_t *symbol)
{
    if (!node || !symbol) return false;
    for (size_t i = 0; i < symbol->use_count; i++)
        if (node_is_within(symbol->use_nodes[i], node)) return true;
    return false;
}

void
context_default_dependencies(parse_tree_t *expr,
                             core_list_t(parse_sym_entry_ptr_t) *parameters,
                             core_list_t(parse_sym_entry_ptr_t) *out)
{
    if (!expr) return;
    core_list_t(parse_sym_entry_ptr_t) ordered =
        core_list_new(parse_sym_entry_ptr_t);
    core_list_t(size_t) offsets = core_list_new(size_t);
    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        if (!parameter || !n00bb_default_references(expr, parameter))
            continue;
        size_t offset = SIZE_MAX;
        for (size_t u = 0; u < parameter->use_count; u++) {
            parse_tree_t *use = parameter->use_nodes[u];
            if (!node_is_within(use, expr)) continue;
            parse_token_info_t *token = parse_tree_find_first_terminal(use);
            if (token && parse_token_start_byte(token) < offset)
                offset = parse_token_start_byte(token);
        }
        size_t at = ordered.len;
        while (at && offsets.data[at - 1] > offset) at--;
        core_list_insert(ordered, at, parameter);
        core_list_insert(offsets, at, offset);
    }
    for (size_t i = 0; i < ordered.len; i++)
        core_list_push(*out, ordered.data[i]);
    core_list_free(ordered);
    core_list_free(offsets);
}

th_type *
context_default_ir_type(th_ctx *types, th_type *context_type,
                        core_list_t(parse_sym_entry_ptr_t) *dependencies,
                        th_type *result_type)
{
    th_type *signature = context_type ? th_resolve(context_type) : nullptr;
    if (!types || !signature || th_kind_of(signature) != TH_FUNCTION
        || !result_type) return nullptr;
    core_list_t(th_param) parameters = core_list_new(th_param);
    bool valid = true;
    for (size_t i = 0; valid && i < dependencies->len; i++) {
        parse_sym_entry_t *dependency = dependencies->data[i];
        valid = dependency && dependency->type;
        if (valid) core_list_push(parameters, ((th_param){
            .mode = TH_IN,
            .type = dependency->type,
        }));
    }
    th_type *failure = th_func_error(signature);
    if (!failure) failure = th_name(types, CORE_STRING_STATIC("nil"));
    th_type *function_type = valid && failure
        ? th_func_with_error(types, result_type, failure,
                             parameters.data, parameters.len, nullptr,
                             nullptr, 0)
        : nullptr;
    core_list_free(parameters);
    return function_type;
}

parse_sym_entry_t *
n00bb_atomic_member_lvalue_root(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *target)
{
    parse_sym_entry_t *symbol = n00bb_member_chain_root(grammar, unit, target);
    return symbol && n00bb_atomic_element_type(symbol->type) ? symbol : nullptr;
}

bool
n00bb_reference_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (!NT_IS(grammar, at, "unary-expr")) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        parse_annotation_t *operator = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_OPERATOR)
            : nullptr;
        if (!operator || !core_string_eq(operator->op_kind,
                CORE_STRING_STATIC("ref"))) continue;
        for (size_t i = 0; i < core_tree_num_children(at); i++) {
            parse_tree_t *operand = core_tree_child(at, i);
            if (!node_is_within(node, operand)) continue;
            for (parse_tree_t *path = node; path && path != operand;
                 path = parse_tree_parent(path)) {
                parse_tree_t *parent = parse_tree_parent(path);
                core_string_t trait = {0}, method = {0};
                if (parent && index_dispatch_for_node(grammar, parent,
                                                       &trait, &method)) {
                    parse_tree_t *receiver = core_tree_num_children(parent)
                        ? core_tree_child(parent, 0) : nullptr;
                    if (!node_is_within(node, receiver)) return false;
                }
            }
            return true;
        }
    }
    return false;
}

parse_sym_entry_t *
n00bb_atomic_load_symbol(parse_grammar_t *grammar,
                         core_translation_unit_t *unit, parse_tree_t *node)
{
    parse_sym_entry_t *symbol = NT_IS(grammar, node, "primary")
        ? attached_symbol(grammar, unit, node) : nullptr;
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM)
        && n00bb_atomic_element_type(symbol->type)
        && !n00bb_reference_target(grammar, node)
        && !n00bb_assignment_target(grammar, node, nullptr)
        ? symbol : nullptr;
}

void
n00bb_apply_atomic_load(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node)
{
    parse_sym_entry_t *symbol = n00bb_atomic_load_symbol(grammar, unit, node);
    if (symbol)
        parse_tree_set_type(node, n00bb_atomic_element_type(symbol->type));
}

parse_sym_entry_t *
n00bb_member_chain_root(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node)
{
    parse_tree_t *access = member_access_node(grammar, node);
    if (!access) return nullptr;
    while (access) {
        node = core_tree_child(access, 0);
        access = member_access_node(grammar, node);
    }
    parse_tree_t *root = n00bb_simple_lvalue(node);
    return root ? attached_symbol(grammar, unit, root) : nullptr;
}

bool
n00bb_member_path(parse_grammar_t *grammar, core_translation_unit_t *unit,
                  parse_tree_t *node, parse_sym_entry_t **root_out,
                  core_buffer_t *path)
{
    parse_tree_t *access = member_access_node(grammar, node);
    parse_tree_t *bare = access ? nullptr : n00bb_simple_lvalue(node);
    parse_sym_entry_t *root = access
        ? n00bb_member_chain_root(grammar, unit, node)
        : bare ? attached_symbol(grammar, unit, bare) : nullptr;
    if (!root || (root->kind != PARSE_SYM_VARIABLE
                  && root->kind != PARSE_SYM_PARAM)) return false;
    core_list_t(parse_tree_ptr_t) steps = core_list_new(parse_tree_ptr_t);
    for (; access; access = member_access_node(grammar,
                                core_tree_child(access, 0)))
        core_list_push(steps, access);
    core_buffer_printf(path, "%.*s", (int)root->name.u8_bytes,
                       root->name.data);
    bool valid = true;
    for (size_t i = steps.len; valid && i > 0; i--) {
        parse_tree_t *field = core_tree_child(steps.data[i - 1], 2);
        valid = field && core_tree_is_leaf(field);
        if (valid) {
            core_string_t name = parse_tree_leaf_text(field);
            core_buffer_printf(path, ".%.*s", (int)name.u8_bytes, name.data);
        }
    }
    core_list_free(steps);
    if (valid) *root_out = root;
    return valid;
}

static bool
type_may_contain_reference_alias_inner(th_ctx *types, th_type *type,
                                      core_list_t(th_type_ptr_t) *active)
{
    if (!type) return true;
    type = th_resolve(type);
    if (!type) return true;
    for (size_t i = 0; i < active->len; i++)
        if (active->data[i] == type) return true;
    switch (th_kind_of(type)) {
    case TH_INDIRECTION:
    case TH_RECORD:
    case TH_SUM:
    case TH_VAR:
        return true;
    case TH_SEQUENCE:
        return true;
    case TH_NAMED: {
        size_t argument_count = 0;
        if (!th_named_info(type, nullptr, nullptr, &argument_count))
            return true;
        th_builtin_kind_t builtin = th_builtin_kind_of(type);
        if (builtin == TH_BUILTIN_ARRAY || builtin == TH_BUILTIN_STRING)
            return true;
        if (argument_count) {
            core_list_push(*active, type);
            bool result = false;
            for (size_t i = 0; i < argument_count; i++) {
                const th_arg *argument = th_named_arg(type, i);
                if (!argument || argument->kind != TH_ARG_TYPE
                    || type_may_contain_reference_alias_inner(types,
                                                    argument->type, active)) {
                    result = true;
                    break;
                }
            }
            active->len--;
            return result;
        }
        if (n00bb_scalar_of(type)) return false;
        th_type *definition = types
            ? th_type_definition(types, type) : nullptr;
        if (definition) {
            core_list_push(*active, type);
            bool result = type_may_contain_reference_alias_inner(types,
                                                    definition, active);
            active->len--;
            return result;
        }
        return true;
    }
    case TH_BITINT:
    case TH_FUNCTION:
        return false;
    default:
        return true;
    }
}

static bool
type_reaches_shared_state_inner(th_ctx *types, th_type *type,
                                core_list_t(th_type_ptr_t) *active)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return true;
    for (size_t i = 0; i < active->len; i++)
        if (active->data[i] == type) return false;
    switch (th_kind_of(type)) {
    case TH_BITINT:
    case TH_FUNCTION:
        return false;
    case TH_RECORD:
    case TH_SUM: {
        core_list_push(*active, type);
        bool result = false;
        size_t count = th_kind_of(type) == TH_RECORD ? th_record_len(type)
                                                     : th_sum_len(type);
        for (size_t i = 0; !result && i < count; i++) {
            th_type *child = th_kind_of(type) == TH_RECORD
                ? (th_record_member(type, i)
                    ? th_record_member(type, i)->type : nullptr)
                : th_sum_arm(type, i);
            result = type_reaches_shared_state_inner(types, child, active);
        }
        active->len--;
        return result;
    }
    case TH_NAMED: {
        if (n00bb_scalar_of(type)) return false;
        th_builtin_kind_t builtin = th_builtin_kind_of(type);
        if (builtin == TH_BUILTIN_STRING) return false;
        if (builtin == TH_BUILTIN_ARRAY) return true;
        size_t argument_count = 0;
        th_named_info(type, nullptr, nullptr, &argument_count);
        th_type *definition = types ? th_type_definition(types, type)
                                    : nullptr;
        core_list_push(*active, type);
        bool result = false;
        if (definition)
            result = type_reaches_shared_state_inner(types, definition,
                                                     active);
        else
            for (size_t i = 0; !result && i < argument_count; i++) {
                const th_arg *argument = th_named_arg(type, i);
                result = !argument || argument->kind != TH_ARG_TYPE
                    || type_reaches_shared_state_inner(types, argument->type,
                                                       active);
            }
        active->len--;
        return result;
    }
    default:
        return true;
    }
}

bool
n00bb_type_reaches_shared_state(th_ctx *types, th_type *type)
{
    core_list_t(th_type_ptr_t) active = {0};
    bool result = type_reaches_shared_state_inner(types, type, &active);
    core_list_free(active);
    return result;
}

bool
n00bb_unit_type_reaches_shared_state(n00bb_tu_state_t *state, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!state || !type) return true;
    uintptr_t cached = (uintptr_t)core_dict_get(&state->shared_state_types,
                                                type, nullptr);
    if (cached) return cached == 2;
    bool result = n00bb_type_reaches_shared_state(state->types, type);
    core_dict_put(&state->shared_state_types, type,
                  (void *)(uintptr_t)(result ? 2 : 1));
    return result;
}

bool
type_may_contain_reference_alias(th_ctx *types, th_type *type)
{
    core_list_t(th_type_ptr_t) active = {0};
    bool result = type_may_contain_reference_alias_inner(types, type,
                                                         &active);
    core_list_free(active);
    return result;
}

static const ir_scalar_t *
n00bb_integer_scalar(th_type *type)
{
    const ir_scalar_t *scalar = n00bb_scalar_of(type);
    return scalar && (scalar->class == IR_CLASS_SINT
                      || scalar->class == IR_CLASS_UINT)
        && scalar->role == IR_ROLE_NONE ? scalar : nullptr;
}

const ir_scalar_t *
n00bb_scalar_of(th_type *type)
{
    const th_builtin_type_t *builtin = th_builtin_of(type);
    return builtin ? builtin->scalar : nullptr;
}

bool
n00bb_scalar_class_is(th_type *type, ir_class_t class)
{
    const ir_scalar_t *scalar = n00bb_scalar_of(type);
    return scalar && scalar->class == class;
}

bool
n00bb_scalar_role_is(th_type *type, ir_role_t role)
{
    const ir_scalar_t *scalar = n00bb_scalar_of(type);
    return scalar && scalar->role == role;
}

bool
n00bb_integer_representation(th_type *type, uint64_t *width,
                             bool *is_signed)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || !width || !is_signed) return false;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    if (th_bitint_info(type, width, &flavor)) {
        *is_signed = flavor == TH_BITINT_SIGNED;
        return flavor == TH_BITINT_SIGNED || flavor == TH_BITINT_UNSIGNED;
    }
    const ir_scalar_t *scalar = n00bb_integer_scalar(type);
    if (!scalar) return false;
    *width = scalar->width;
    *is_signed = scalar->class == IR_CLASS_SINT;
    return true;
}

th_type *
n00bb_integer_type_of_width(th_ctx *types, uint64_t width, bool is_signed)
{
    static const core_string_t signed_names[] = {
        CORE_STRING_STATIC("i8"), CORE_STRING_STATIC("i16"),
        CORE_STRING_STATIC("i32"), CORE_STRING_STATIC("i64"),
        CORE_STRING_STATIC("i128"),
    };
    static const core_string_t unsigned_names[] = {
        CORE_STRING_STATIC("u8"), CORE_STRING_STATIC("u16"),
        CORE_STRING_STATIC("u32"), CORE_STRING_STATIC("u64"),
        CORE_STRING_STATIC("u128"),
    };
    for (size_t i = 0; i < N00BB_INTEGER_WIDTHS; i++)
        if (((uint64_t)8 << i) == width)
            return th_name(types, canonical_builtin_type_name(
                is_signed ? signed_names[i] : unsigned_names[i]));
    return nullptr;
}

th_type *
n00bb_common_integer_type(th_ctx *types, th_type *left, th_type *right)
{
    if (n00bb_integer_type_promotes_to(left, right)) return right;
    if (n00bb_integer_type_promotes_to(right, left)) return left;
    ir_interval_t left_range = {0}, right_range = {0};
    if (!n00bb_type_interval(left, &left_range)
        || !n00bb_type_interval(right, &right_range)) return nullptr;
    ir_interval_t joined = ir_interval_join(left_range, right_range);
    for (size_t i = 0; i < N00BB_INTEGER_WIDTHS; i++)
        if (ir_interval_fits(joined, (uint64_t)8 << i, true))
            return n00bb_integer_type_of_width(types, (uint64_t)8 << i, true);
    return nullptr;
}

bool
n00bb_integer_type_promotes_to(th_type *source, th_type *target)
{
    uint64_t source_width = 0, target_width = 0;
    bool source_signed = false, target_signed = false;
    return n00bb_integer_representation(source, &source_width,
                                        &source_signed)
        && n00bb_integer_representation(target, &target_width,
                                        &target_signed)
        && (source_signed == target_signed
            ? source_width <= target_width
            : !source_signed && target_signed
                && source_width < target_width);
}

bool
n00bb_integer_range_fits_type(th_type *type, ir_interval_t range)
{
    ir_interval_t bounds = {0};
    return n00bb_type_interval(type, &bounds)
        && ir_interval_contains(bounds, range);
}

static parse_tree_t *
float_literal_leaf(parse_tree_t *node, th_type *retype)
{
    while (node && !core_tree_is_leaf(node)) {
        if (retype) parse_tree_set_type(node, retype);
        size_t children = core_tree_num_children(node);
        if (children == 1) node = core_tree_child(node, 0);
        else if (children == 2
                 && (parse_tree_leaf_is(core_tree_child(node, 0),
                                        CORE_STRING_STATIC("-"))
                     || parse_tree_leaf_is(core_tree_child(node, 0),
                                           CORE_STRING_STATIC("+"))))
            node = core_tree_child(node, 1);
        else if (children == 3
                 && parse_tree_leaf_is(core_tree_child(node, 0),
                                       CORE_STRING_STATIC("("))
                 && parse_tree_leaf_is(core_tree_child(node, 2),
                                       CORE_STRING_STATIC(")")))
            node = core_tree_child(node, 1);
        else return nullptr;
    }
    parse_token_info_t *token = node ? parse_node_token(node) : nullptr;
    if (!token || core_option_is_set(parse_token_postfix_modifier(token)))
        return nullptr;
    core_string_t text = parse_tree_leaf_text(node);
    if (!text.u8_bytes || text.data[0] < '0' || text.data[0] > '9'
        || memchr(text.data, 'x', text.u8_bytes)
        || memchr(text.data, 'X', text.u8_bytes)) return nullptr;
    return memchr(text.data, '.', text.u8_bytes)
            || memchr(text.data, 'e', text.u8_bytes)
            || memchr(text.data, 'E', text.u8_bytes)
        ? node : nullptr;
}

bool
n00bb_contextual_float_literal(parse_tree_t *node, th_type *target)
{
    th_type *current_type = node ? parse_tree_type(node) : nullptr;
    if (!float_literal_leaf(node, nullptr) || !is_float_type(target)
        || (current_type && !is_float_type(current_type))) return false;
    float_literal_leaf(node, target);
    return true;
}

bool
n00bb_float_literal_adopts(parse_tree_t *node, th_type *operand)
{
    if (n00bb_contextual_float_literal(node, operand)) return true;
    th_type *current_type = node ? parse_tree_type(node) : nullptr;
    if (!operand || th_kind_of(th_resolve(operand)) != TH_VAR
        || !float_literal_leaf(node, nullptr)
        || (current_type && !is_float_type(current_type))) return false;
    float_literal_leaf(node, operand);
    return true;
}

static bool
unsigned_literal_misplaced(parse_tree_t *node, th_type *expected)
{
    parse_tree_t *origin = node ? parse_tree_integer_literal_origin(node)
                                : nullptr;
    return origin && n00bb_unsigned_literal_origin(origin)
        && !n00bb_bitwise_type(expected);
}

bool
n00bb_integer_literal_joins_type(n00bb_tu_state_t *state,
                                 parse_tree_t *node, th_type *target)
{
    parse_tree_t *origin = node
        ? parse_tree_integer_literal_origin(node) : nullptr;
    if (!origin) return n00bb_contextual_float_literal(node, target);
    th_type *open = parse_tree_type(node);
    if (n00bb_open_integer(open))
        return target
            && (n00bb_open_integer(target)
                || ((is_integer_type(target)
                     || n00bb_scalar_role_is(target, IR_ROLE_BYTE))
                    && !unsigned_literal_misplaced(node, target)))
            && th_unify_in_place(state->types, open, target) != nullptr;
    return open && target && th_resolve(open) == th_resolve(target);
}

static bool
integer_value_promotes(parse_tree_t *node, th_type *actual,
                       th_type *expected)
{
    ir_interval_t range = {0}, bounds = {0};
    if (!n00bb_type_interval(actual, &range)
        || !n00bb_type_interval(expected, &bounds)
        || unsigned_literal_misplaced(node, expected)) return false;
    if (node) (void)n00bb_current_integer_range(node, &range);
    return ir_interval_meet(range, bounds, nullptr);
}

static bool
record_has_member(th_type *record, core_string_t name)
{
    for (size_t i = 0; i < th_record_len(record); i++) {
        const th_member *member = th_record_member(record, i);
        if (member && member->name.data
            && core_string_eq(member->name, name))
            return true;
    }
    return false;
}

static th_type *
literal_union_shape(th_ctx *types, uint64_t record_hash, th_type *literal,
                    th_type *definition)
{
    size_t count = th_record_len(definition);
    th_member *kept = count ? core_alloc_array(th_member, count) : nullptr;
    if (count && !kept) return nullptr;
    size_t used = 0;
    bool unions = false, valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        const th_member *member = th_record_member(definition, i);
        uint32_t group = member && member->name.data
            ? n00bb_union_arm_group(record_hash, member->name) : 0;
        if (group) {
            unions = true;
            if (!record_has_member(literal, member->name)) continue;
            for (size_t j = 0; valid && j < used; j++)
                valid = n00bb_union_arm_group(record_hash,
                                              kept[j].name) != group;
        }
        if (valid) kept[used++] = *member;
    }
    th_type *shape = !valid ? nullptr
        : unions ? th_record(types, kept, used) : definition;
    core_free(kept);
    return shape;
}

static th_type *
record_literal_as_named(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *node, th_type *actual,
                        th_type *expected)
{
    if (!node || core_tree_is_leaf(node)
        || th_kind_of(th_resolve(actual)) != TH_RECORD
        || th_kind_of(th_resolve(expected)) != TH_NAMED) return nullptr;
    if (grammar && state->unit)
        return contextualize_record_literal(grammar, state->unit, state, node,
                                            expected) ? expected : nullptr;
    th_type *definition = th_type_definition(state->types, expected);
    if (!definition || th_kind_of(th_resolve(definition)) != TH_RECORD)
        return nullptr;
    th_type *shape = literal_union_shape(state->types,
        th_hash(state->types, expected), th_resolve(actual),
        th_resolve(definition));
    if (shape == th_resolve(definition)
        && !n00bb_node_record_literal_origin(node)) return nullptr;
    bool reduced = shape && shape != th_resolve(definition);
    if (!shape || (!reduced && !th_unify_probe(state->types, actual, shape))
        || !th_unify_in_place(state->types, actual, shape)) return nullptr;
    if (reduced)
        for (parse_tree_t *current = n00bb_node_record_literal_origin(node);
             current; current = parse_tree_parent(current)) {
            parse_tree_set_type(current, expected);
            if (current == node) break;
        }
    return expected;
}

static parse_tree_t *
contextual_sequence_literal(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *literal = node;
    while (grammar && literal && !core_tree_is_leaf(literal)
           && !NT_IS(grammar, literal, "list-lit")
           && core_tree_num_children(literal) == 1)
        literal = core_tree_child(literal, 0);
    return grammar && literal && !core_tree_is_leaf(literal)
        && NT_IS(grammar, literal, "list-lit")
        && !literal_modifier(grammar, literal).data ? literal : nullptr;
}

static th_type *
sequence_literal_as_array(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                          parse_tree_t *node, th_type *expected)
{
    th_type *array = expected ? th_resolve(expected) : nullptr;
    parse_tree_t *literal = contextual_sequence_literal(grammar, node);
    if (!literal || !array || th_kind_of(array) != TH_SEQUENCE
        || !th_builtin_applied(parse_tree_type(literal),
                               TH_BUILTIN_CONTAINER_LIST, 1)) return nullptr;
    th_type *element = th_seq_elem(array);
    core_list_t(parse_tree_ptr_t) items = core_list_new(parse_tree_ptr_t);
    parse_tree_t *elements = CHILD_NT(grammar, literal, "list-elems");
    bool fits = !elements || collect_ordered_children(elements, &items);
    for (size_t i = 0; fits && i < items.len; i++) {
        th_type *item = parse_tree_type(items.data[i]);
        fits = n00bb_integer_literal_joins_type(state, items.data[i], element)
            || (item && th_unify_probe(state->types, item, element));
    }
    for (size_t i = 0; fits && i < items.len; i++)
        fits = th_unify_in_place(state->types,
            parse_tree_type(items.data[i]), element) != nullptr;
    core_list_free(items);
    if (!fits) return nullptr;
    for (parse_tree_t *current = literal; current;
         current = parse_tree_parent(current)) {
        parse_tree_set_type(current, expected);
        if (current == node) break;
    }
    n00bb_node_facts_t *facts = n00bb_node_facts(literal);
    if (facts) facts->literal_site = nullptr;
    return expected;
}

static th_type *
union_arm_for(th_ctx *types, th_type *actual, th_type *expected)
{
    if (th_builtin_kind(types, expected) == TH_BUILTIN_ANY) return expected;
    th_comb comb;
    if (!th_sum_comb(th_resolve(expected), &comb) || comb != TH_UNION)
        return nullptr;
    for (size_t i = 0; i < th_sum_len(expected); i++)
        if (th_unify_probe(types, actual, th_sum_arm(expected, i)))
            return expected;
    return nullptr;
}

bool
n00bb_value_coerces(th_ctx *types, th_type *from, th_type *to)
{
    from = from ? th_resolve(from) : nullptr;
    to = to ? th_resolve(to) : nullptr;
    if (!types || !from || !to || th_kind_of(from) != TH_NAMED
        || th_kind_of(to) != TH_NAMED || !th_is_closed(types, from)
        || !th_is_closed(types, to)
        || th_hash(types, from) == th_hash(types, to)) return false;
    core_string_t from_name = {}, to_name = {};
    size_t from_args = 0, to_args = 0;
    return th_named_info(from, &from_name, nullptr, &from_args)
        && th_named_info(to, &to_name, nullptr, &to_args)
        && !from_args && !to_args
        && core_string_eq(from_name,
               core_string_borrow(n00bb_buffer_type_name,
                                  sizeof(n00bb_buffer_type_name) - 1))
        && core_string_eq(to_name, CORE_STRING_STATIC("string"));
}

static bool
named_arguments_want_buffer(th_ctx *types, th_type *expected, th_type *actual)
{
    core_string_t expected_name = {}, actual_name = {};
    size_t expected_args = 0, actual_args = 0;
    if (!th_named_info(expected, &expected_name, nullptr, &expected_args)
        || !th_named_info(actual, &actual_name, nullptr, &actual_args)
        || expected_args != actual_args
        || !core_string_eq(expected_name, actual_name)) return false;
    for (size_t i = 0; i < expected_args; i++) {
        const th_arg *want = th_named_arg(expected, i);
        const th_arg *have = th_named_arg(actual, i);
        if (want && have && want->kind == TH_ARG_TYPE
            && have->kind == TH_ARG_TYPE
            && n00bb_string_where_buffer_expected(types, want->type,
                                                  have->type)) return true;
    }
    return false;
}

static bool
record_fields_want_buffer(th_ctx *types, th_type *expected, th_type *actual)
{
    for (size_t i = 0; i < th_record_len(expected); i++) {
        const th_member *member = th_record_member(expected, i);
        th_type *field = member
            ? th_record_lookup(actual, member->name) : nullptr;
        if (field && n00bb_string_where_buffer_expected(types, member->type,
                                                        field)) return true;
    }
    return false;
}

bool
n00bb_string_where_buffer_expected(th_ctx *types, th_type *expected,
                                   th_type *actual)
{
    if (!types || !expected || !actual) return false;
    if (n00bb_value_coerces(types, expected, actual)) return true;
    expected = th_resolve(expected);
    actual = th_resolve(actual);
    th_type *expected_item = nullptr, *actual_item = nullptr;
    if (n00bb_option_type_element(expected, &expected_item))
        return n00bb_string_where_buffer_expected(types, expected_item,
            n00bb_option_type_element(actual, &actual_item)
                ? actual_item : actual);
    th_kind kind = th_kind_of(expected);
    if (kind == TH_NAMED && th_kind_of(actual) != TH_NAMED) {
        th_type *definition = th_type_definition(types, expected);
        return definition && definition != expected
            && n00bb_string_where_buffer_expected(types, definition, actual);
    }
    if (kind != th_kind_of(actual)) return false;
    switch (kind) {
    case TH_SEQUENCE:
        return n00bb_string_where_buffer_expected(types, th_seq_elem(expected),
                                                  th_seq_elem(actual));
    case TH_NAMED:
        return named_arguments_want_buffer(types, expected, actual);
    case TH_RECORD:
        return record_fields_want_buffer(types, expected, actual);
    default:
        return false;
    }
}

static th_type *
literal_entry_expected_type(th_type *expected, bool dictionary, size_t index)
{
    expected = th_resolve(expected);
    if (th_kind_of(expected) == TH_SEQUENCE) return th_seq_elem(expected);
    const th_arg *argument = th_kind_of(expected) == TH_NAMED
        ? th_named_arg(expected, dictionary ? index & 1u : 0) : nullptr;
    return argument && argument->kind == TH_ARG_TYPE ? argument->type
                                                     : nullptr;
}

parse_tree_t *
n00bb_string_where_buffer_site(parse_grammar_t *grammar, th_ctx *types,
                               parse_tree_t *node, th_type *expected)
{
    parse_tree_t *literal = node;
    while (literal && !core_tree_is_leaf(literal)
           && !NT_IS(grammar, literal, "list-lit")
           && !NT_IS(grammar, literal, "dict-or-set-lit")
           && core_tree_num_children(literal) == 1)
        literal = core_tree_child(literal, 0);
    parse_tree_t *entries = NT_IS(grammar, literal, "list-lit")
        ? CHILD_NT(grammar, literal, "list-elems")
        : NT_IS(grammar, literal, "dict-or-set-lit")
        ? CHILD_NT(grammar, literal, "set-entries") : nullptr;
    parse_tree_t *dictionary = NT_IS(grammar, literal, "dict-or-set-lit")
        ? CHILD_NT(grammar, literal, "dict-entries") : nullptr;
    if (dictionary) entries = dictionary;
    core_list_t(parse_tree_ptr_t) items = core_list_new(parse_tree_ptr_t);
    parse_tree_t *site = node;
    if (entries && collect_ordered_children(entries, &items))
        for (size_t i = 0; site == node && i < items.len; i++) {
            th_type *want = literal_entry_expected_type(expected,
                                                        dictionary, i);
            parse_tree_t *item = items.data[i];
            if (want && !core_tree_is_leaf(item)
                && n00bb_string_where_buffer_expected(types, want,
                       parse_tree_type(item)))
                site = n00bb_string_where_buffer_site(grammar, types, item,
                                                      want);
        }
    core_list_free(items);
    return site;
}

static bool
n00bb_coercion_hook(th_ctx *types, th_type *from, th_type *to, void *user)
{
    (void)user;
    return n00bb_value_coerces(types, from, to);
}

th_type *
n00bb_value_coercion_target(th_ctx *types, th_type *from)
{
    th_type *view = th_name(types, CORE_STRING_STATIC("string"));
    return n00bb_value_coerces(types, from, view) ? view : nullptr;
}

bool
n00bb_blanket_receiver(th_type *contract)
{
    th_type *signature = contract ? th_resolve(contract) : nullptr;
    const th_param *receiver = signature
        && th_kind_of(signature) == TH_FUNCTION
        && th_func_nparams(signature)
        ? th_func_param(signature, 0) : nullptr;
    return receiver && receiver->type
        && th_kind_of(th_resolve(receiver->type)) == TH_VAR;
}

static th_type *
n00bb_receiver_view_hook(th_ctx *types, th_type *from, void *user)
{
    (void)types;
    (void)user;
    return from ? th_option_item(th_resolve(from)) : nullptr;
}

static th_type *
n00bb_coercion_target_hook(th_ctx *types, th_type *from, void *user)
{
    (void)user;
    th_type *item = from ? th_option_item(th_resolve(from)) : nullptr;
    if (item) return item;
    return n00bb_value_coercion_target(types, from);
}

static bool
call_result_node(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)
           && core_tree_num_children(node) == 1)
        node = core_tree_child(node, 0);
    return node && !core_tree_is_leaf(node)
        && core_tree_num_children(node) >= 3
        && core_tree_is_leaf(core_tree_child(node, 1))
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC("("));
}

static bool
n00bb_conversion_hook(th_ctx *types, th_type *from, th_type *to, void *site,
                      void *context, void *user)
{
    parse_grammar_t *grammar = context;
    n00bb_tu_state_t *state = user;
    for (parse_tree_t *at = site; at; at = parse_tree_parent(at)) {
        parse_scope_t *scope = parse_tree_scope(at);
        if (!scope || !scope->owner_unit) continue;
        state = tu_state(scope->owner_unit);
        break;
    }
    if (!state || state->types != types) return false;
    if (n00bb_open_integer(from) && n00bb_open_integer(to))
        return th_unify_in_place(types, from, to) != nullptr;
    if (th_is_nil(to) || (site && n00bb_node_record_literal_origin(site)))
        return true;
    if (site && ((!th_is_closed(types, from) && call_result_node(site))
            || th_kind_of(th_resolve(from)) == TH_VAR
            || (th_kind_of(th_resolve(to)) == TH_VAR && !th_is_nil(from)))
        && n00bb_dereference_target(types, from, th_resolve(to))
            != th_resolve(to)) {
        core_list_push(state->deferred_conversions,
            ((n00bb_deferred_conversion_t){site, from, to}));
        if (n00bb_open_integer(to) && !n00bb_open_integer(from))
            n00bb_integer_copy(state, site,
                (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, to, nullptr},
                site);
        if (n00bb_open_integer(from) && is_integer_type(to))
            n00bb_integer_boundary(state, site, to);
        return true;
    }
    if (!th_is_closed(types, from) || !th_is_closed(types, to)) {
        th_type *resolved = th_resolve(to);
        if (th_is_nil(from) && th_kind_of(resolved) == TH_VAR) return true;
        th_type *element = nullptr;
        bool option_target = n00bb_option_type_element(resolved, &element);
        if (th_is_nil(from) && option_target) return true;
        if (!option_target && th_kind_of(resolved) != TH_VAR
            && n00bb_dereference_target(types, from, resolved)) {
            th_type *referent = th_ind_referent(th_resolve(from));
            if (n00bb_open_integer(referent) && is_integer_type(resolved)) {
                if (site) n00bb_integer_boundary(state, site, resolved);
                return true;
            }
            return th_unify_in_place(types, referent, resolved) != nullptr;
        }
        if (option_target && element && th_kind_of(from) != TH_VAR
            && !n00bb_option_type_element(from, nullptr))
            return th_unify_probe(types, from, element);
        return th_kind_of(resolved) == TH_VAR
            && n00bb_dereference_target(types, from, resolved) == resolved;
    }
    bool converted = false;
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(state->unit);
    size_t reported = core_diagnostics_count(diagnostics);
    if (n00bb_type_materialize(grammar, state, site, from, to, &converted))
        return true;
    if (!site) return false;
    if (core_diagnostics_count(diagnostics) == reported)
        n00bb_report_conversion_error(state->unit, site, types, from, to);
    return true;
}

void
n00bb_register_value_coercions(n00bb_tu_state_t *state)
{
    th_register_coercion(state->types, n00bb_coercion_hook, nullptr);
    th_register_coercion_target(state->types, n00bb_coercion_target_hook);
    th_register_receiver_view(state->types, n00bb_receiver_view_hook);
    th_register_conversion(state->types, n00bb_conversion_hook, state);
    n00bb_register_integer_class(state);
}

static void
collect_value_results(parse_grammar_t *grammar, parse_tree_t *construct,
                      parse_tree_t *at,
                      core_list_t(parse_tree_ptr_t) *results)
{
    if (!at || core_tree_is_leaf(at)) return;
    if (NT_IS(grammar, at, "value-stmt")) {
        parse_tree_t *expression = CHILD_NT(grammar, at, "expression");
        if (expression
            && value_construct_of_result(grammar, at) == construct)
            core_list_push(*results, expression);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(at); i++)
        collect_value_results(grammar, construct, core_tree_child(at, i),
                              results);
}

static bool
contextual_value_results(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                         parse_tree_t *node, th_type *target)
{
    parse_tree_t *construct = node;
    while (grammar && construct && !core_tree_is_leaf(construct)
           && !NT_IS(grammar, construct, "if-value-expr")
           && !NT_IS(grammar, construct, "try-value-expr")
           && !NT_IS(grammar, construct, "typecase-value-expr")
           && core_tree_num_children(construct) == 1)
        construct = core_tree_child(construct, 0);
    if (!grammar || !construct || core_tree_is_leaf(construct)
        || core_tree_num_children(construct) == 1
        || !is_integer_type(target)) return false;
    core_list_t(parse_tree_ptr_t) results = core_list_new(parse_tree_ptr_t);
    collect_value_results(grammar, construct, construct, &results);
    bool contextual = results.len > 0;
    for (size_t i = 0; contextual && i < results.len; i++) {
        th_type *result = parse_tree_type(results.data[i]);
        contextual = (result && th_is_closed(state->types, result)
                      && th_hash(state->types, result)
                          == th_hash(state->types, target))
            || n00bb_integer_literal_joins_type(state, results.data[i],
                                                target)
            || contextual_value_results(grammar, state, results.data[i],
                                        target);
    }
    core_list_free(results);
    if (!contextual) return false;
    for (parse_tree_t *at = construct; at;
         at = parse_tree_parent(at)) {
        parse_tree_set_type(at, target);
        if (at == node) break;
    }
    return true;
}

bool
n00bb_constant_misfits_type(n00bb_tu_state_t *state, parse_tree_t *node,
                      th_type *expected)
{
    th_type *target = is_integer_type(expected) ? expected : nullptr;
    th_type *element = nullptr;
    if (!target && n00bb_option_type_element(expected, &element)
        && is_integer_type(element)) target = element;
    ir_interval_t range = {0};
    if (!target || !node || !parse_tree_integer_literal_origin(node)
        || !n00bb_current_integer_range(node, &range)
        || n00bb_integer_range_fits_type(target, range))
        return false;
    n00bb_report_integer_misfit(state, node, CORE_STRING_STATIC("constant"),
                                range, target);
    return true;
}

void
n00bb_report_integer_misfit(n00bb_tu_state_t *state, parse_tree_t *node,
                            core_string_t noun, ir_interval_t range,
                            th_type *target)
{
    core_string_t name = th_print(state->types, target);
    core_buffer_t message = {0};
    core_buffer_puts(&message, "integer ");
    core_buffer_append_string(&message, noun);
    if (range.lo != range.hi) core_buffer_puts(&message, "s ");
    else core_buffer_puts(&message, " ");
    if (range.lo == ir_int_max) core_buffer_puts(&message, "above ");
    ir_int_append(&message, range.lo);
    if (range.lo != range.hi) {
        core_buffer_puts(&message, "..");
        ir_int_append(&message, range.hi);
        core_buffer_puts(&message, " never fit ");
    }
    else core_buffer_puts(&message, " does not fit ");
    core_buffer_printf(&message, "%.*s", (int)name.u8_bytes, name.data);
    core_diagnostics_report_at(
        core_translation_unit_diagnostics(state->unit), state->unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0041"),
        core_buffer_string(&message), link_source_location(node));
    core_buffer_release(&message);
}

static uint64_t
narrowing_site_key(uint32_t line, uint32_t column)
{
    return ((uint64_t)line << 32) | column;
}

bool
n00bb_integer_narrowing_reported(n00bb_tu_state_t *state, uint32_t line,
                                 uint32_t column)
{
    return state && line && core_dict_get(&state->integer_narrowing_sites,
        (void *)(uintptr_t)narrowing_site_key(line, column), nullptr);
}

void
n00bb_report_integer_narrowing(n00bb_tu_state_t *state, parse_tree_t *node,
                               ir_interval_t range, th_type *target)
{
    core_diag_location_t location = link_source_location(node);
    core_dict_put(&state->integer_narrowing_sites,
        (void *)(uintptr_t)narrowing_site_key(location.line, location.column),
        node);
    core_string_t name = th_print(state->types, target);
    core_buffer_t message = {0};
    core_buffer_puts(&message, "integer values ");
    ir_int_append(&message, range.lo);
    core_buffer_puts(&message, "..");
    ir_int_append(&message, range.hi);
    core_buffer_printf(&message,
        " may not fit %.*s; the conversion traps at runtime when they do not",
        (int)name.u8_bytes, name.data);
    core_diagnostics_report_at(
        core_translation_unit_diagnostics(state->unit), state->unit,
        CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-W0017"),
        core_buffer_string(&message), location);
    core_buffer_release(&message);
}

th_type *
n00bb_type_materialize(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_tree_t *node, th_type *actual, th_type *expected,
                       bool *converted)
{
    *converted = false;
    if (!state || !actual || !expected) return nullptr;
    th_ctx *types = state->types;
    th_type *element = n00bb_atomic_element_type(actual);
    if (element && !n00bb_atomic_element_type(expected)) actual = element;
    element = n00bb_atomic_element_type(expected);
    if (element && !n00bb_atomic_element_type(actual)) {
        bool inner = false;
        th_type *materialized = n00bb_type_materialize(grammar, state, node,
            actual, element, &inner);
        *converted = materialized != nullptr;
        return materialized ? expected : nullptr;
    }
    if (n00bb_integer_literal_joins_type(state, node, expected)
        || contextual_value_results(grammar, state, node, expected))
        return expected;
    if (n00bb_constant_misfits_type(state, node, expected)) return nullptr;
    *converted = true;
    if (n00bb_value_coerces(types, actual, expected)) return expected;
    if (n00bb_string_where_buffer_expected(types, expected, actual)) {
        *converted = false;
        if (!node) return nullptr;
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(state->unit), state->unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0036"),
            core_string_borrow(n00bb_string_to_buffer_text,
                               sizeof(n00bb_string_to_buffer_text) - 1),
            link_source_location(grammar
                ? n00bb_string_where_buffer_site(grammar, types, node,
                                                 expected)
                : node));
        return nullptr;
    }
    if (integer_value_promotes(node,actual, expected)) {
        n00bb_integer_boundary(state, node, expected);
        return expected;
    }
    th_type *dereferenced = n00bb_dereference_target(types, actual, expected);
    if (dereferenced && th_resolve(dereferenced) != th_resolve(expected)
        && is_integer_type(dereferenced))
        return n00bb_type_materialize(grammar, state, node, dereferenced,
                                      expected, converted);
    if (dereferenced) return expected;
    ir_interval_t value_range = {0}, target_range = {0};
    if (node && !unsigned_literal_misplaced(node, expected)
        && n00bb_type_interval(actual, &value_range)
        && n00bb_type_interval(expected, &target_range)) {
        (void)n00bb_current_integer_range(node, &value_range);
        n00bb_report_integer_misfit(state, node, CORE_STRING_STATIC("value"),
                                    value_range, expected);
        *converted = false;
        return nullptr;
    }
    if (n00bb_option_type_element(expected, &element)
        && !n00bb_option_type_element(actual, nullptr)
        && (th_is_nil(actual) || th_unify_probe(types, actual, element)
            || n00bb_integer_literal_joins_type(state, node, element)))
        return expected;
    if (n00bb_option_type_element(expected, &element)
        && !n00bb_option_type_element(actual, nullptr)
        && integer_value_promotes(node,actual, element)) {
        n00bb_integer_boundary(state, node, element);
        return expected;
    }
    th_type *materialized = union_arm_for(types, actual, expected);
    if (materialized && !th_unify_probe(types, actual, expected))
        return materialized;
    *converted = false;
    materialized = record_literal_as_named(grammar, state, node, actual,
                                           expected);
    if (materialized) return materialized;
    materialized = sequence_literal_as_array(grammar, state, node, expected);
    if (materialized) return materialized;
    th_type *option_element = nullptr;
    if (n00bb_option_type_element(actual, &option_element)
        && !n00bb_option_type_element(expected, nullptr)
        && th_kind_of(th_resolve(expected)) != TH_VAR
        && th_unify_probe(types, option_element, expected)
        && th_unify_in_place(types, option_element, expected))
        return expected;
    if (n00bb_open_integer(actual) && is_integer_type(expected))
        n00bb_integer_boundary(state, node, expected);
    return th_unify_in_place(types, actual, expected);
}

bool
is_integer_type(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    return type && (th_kind_of(type) == TH_BITINT
                    || n00bb_integer_scalar(type) != nullptr);
}

void
cache_type_syntax_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return;
    parse_tree_t *source = nullptr;
    if (NT_IS(grammar, node, "type-spec"))
        source = CHILD_NT(grammar, node, "one-tspec");
    else if (NT_IS(grammar, node, "one-tspec"))
        source = CHILD_NT(grammar, node, "base-tspec");
    else if (NT_IS(grammar, node, "base-tspec")) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *child = core_tree_child(node, i);
            if (NT_IS(grammar, child, "array-dimensions")) return;
            if (NT_IS(grammar, child, "base-tspec-core")) source = child;
        }
    }
    else if (NT_IS(grammar, node, "base-tspec-core")) {
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *child = core_tree_child(node, i);
            if (NT_IS(grammar, child, "tspec-application")) return;
            if (NT_IS(grammar, child, "member-chain")) source = child;
        }
    }
    if (source && !parse_tree_type(node)
        && parse_tree_type(source))
        parse_tree_set_type(node, parse_tree_type(source));
}

bool
append_type_argument_pack(th_type *pack, core_list_t(th_arg) *arguments)
{
    if (!pack || !arguments
        || th_builtin_kind_of(pack) != TH_BUILTIN_TUPLE) return false;
    for (size_t i = 0; i < th_named_nargs(pack); i++) {
        const th_arg *argument = th_named_arg(pack, i);
        if (!argument || argument->kind != TH_ARG_TYPE) return false;
        core_list_push(*arguments, *argument);
    }
    return true;
}

th_type *
type_argument_pack(parse_grammar_t *grammar, th_ctx *types,
                  parse_tree_t *node)
{
    if (!grammar || !types || !NT_IS(grammar, node, "tspec-list"))
        return nullptr;
    core_list_t(th_type_ptr_t) arguments = core_list_new(th_type_ptr_t);
    bool valid = true;
    for (size_t i = 0; valid && i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "union-tspec")) {
            th_type *type = parse_tree_type(child);
            if (!type) valid = false;
            else core_list_push(arguments, type);
        }
        else if (NT_IS(grammar, child, "tspec-list")) {
            core_list_t(th_arg) tail = core_list_new(th_arg);
            valid = append_type_argument_pack(
                parse_tree_type(child), &tail);
            for (size_t j = 0; valid && j < tail.len; j++) {
                if (tail.data[j].kind != TH_ARG_TYPE) valid = false;
                else core_list_push(arguments, tail.data[j].type);
            }
            core_list_free(tail);
        }
    }
    th_type *pack = valid && arguments.len
        ? th_tuple(types, arguments.data, arguments.len) : nullptr;
    core_list_free(arguments);
    return pack;
}

th_type *
function_parameter_pack(parse_grammar_t *grammar, th_ctx *types,
                        parse_tree_t *node)
{
    if (!grammar || !types || !NT_IS(grammar, node, "tspec-func-params"))
        return nullptr;
    core_list_t(th_param) positional = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    core_list_t(core_string_t) owned_names = core_list_new(core_string_t);
    th_param rest = {0};
    bool has_rest = false;
    bool valid = true;
    for (size_t i = 0; valid && i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        size_t count = parse_tree_is_group(child)
            ? core_tree_num_children(child) : 1;
        for (size_t j = 0; valid && j < count; j++) {
            parse_tree_t *item = parse_tree_is_group(child)
                ? core_tree_child(child, j) : child;
            if (NT_IS(grammar, item, "union-tspec")
                || NT_IS(grammar, item, "tspec-func-param")) {
                parse_tree_t *type_node = NT_IS(grammar, item,
                        "tspec-func-param")
                    ? CHILD_NT(grammar, item, "union-tspec") : item;
                th_type *type = type_node
                    ? parse_tree_type(type_node) : nullptr;
                if (!type) valid = false;
                else {
                    core_string_t spelling = first_token_text(item);
                    th_mode mode = core_string_eq(
                            spelling, CORE_STRING_STATIC("out")) ? TH_OUT
                        : core_string_eq(
                            spelling, CORE_STRING_STATIC("modify")) ? TH_INOUT
                        : core_string_eq(
                            spelling, CORE_STRING_STATIC("consume")) ? TH_CONSUME
                        : core_string_eq(
                            spelling, CORE_STRING_STATIC("view")) ? TH_VIEW
                        : TH_IN;
                    core_list_push(positional, ((th_param){
                        .mode = mode, .type = type,
                    }));
                }
            }
            else if (NT_IS(grammar, item, "tspec-func-params")) {
                th_type *tail = th_resolve(
                    parse_tree_type(item));
                if (!tail || th_kind_of(tail) != TH_FUNCTION) {
                    valid = false;
                    continue;
                }
                for (size_t k = 0; k < th_func_nparams(tail); k++) {
                    const th_param *parameter = th_func_param(tail, k);
                    if (!parameter) valid = false;
                    else core_list_push(positional, *parameter);
                }
                const th_param *tail_rest = th_func_rest(tail);
                if (tail_rest) {
                    if (has_rest) valid = false;
                    else {
                        rest = *tail_rest;
                        has_rest = true;
                    }
                }
                for (size_t k = 0; valid && k < th_func_nkw(tail); k++) {
                    const th_kwparam *keyword = th_func_kw(tail, k);
                    if (!keyword) valid = false;
                    else core_list_push(keywords, *keyword);
                }
            }
            else if (NT_IS(grammar, item, "tspec-vargs")) {
                parse_tree_t *type_node = CHILD_NT(grammar, item,
                                                    "union-tspec");
                if (has_rest || !type_node
                    || !parse_tree_type(type_node)) valid = false;
                else {
                    rest = (th_param){
                        .mode = TH_IN,
                        .type = parse_tree_type(type_node),
                    };
                    has_rest = true;
                }
            }
            else if (NT_IS(grammar, item, "tspec-kargs")) {
                for (size_t k = 0; valid
                     && k < core_tree_num_children(item); k++) {
                    parse_tree_t *field_group = core_tree_child(item, k);
                    size_t field_count =
                        parse_tree_is_group(field_group)
                            ? core_tree_num_children(field_group) : 1;
                    for (size_t m = 0; valid && m < field_count; m++) {
                        parse_tree_t *field =
                            parse_tree_is_group(field_group)
                                ? core_tree_child(field_group, m)
                                : field_group;
                        if (!NT_IS(grammar, field, "tspec-field")) continue;
                        parse_tree_t *type_node = CHILD_NT(grammar, field,
                                                            "union-tspec");
                        parse_tree_t *name_node = core_tree_num_children(field)
                            ? core_tree_child(field, 0) : nullptr;
                        core_string_t name = name_node
                            && core_tree_is_leaf(name_node)
                                ? parse_tree_leaf_text(name_node)
                                : (core_string_t){0};
                        if (!name.data || !name.u8_bytes || !type_node
                            || !parse_tree_type(type_node)) {
                            valid = false;
                            break;
                        }
                        core_string_t stable_name = core_string_dup(name);
                        if (!stable_name.data) {
                            valid = false;
                            break;
                        }
                        core_list_push(owned_names, stable_name);
                        core_list_push(keywords, ((th_kwparam){
                            .name = stable_name,
                            .mode = TH_IN,
                            .type = parse_tree_type(type_node),
                        }));
                    }
                }
            }
        }
    }
    th_type *nil = th_name(types, CORE_STRING_STATIC("nil"));
    th_type *pack = valid && nil
        ? th_func_with_error(types, nil, nil, positional.data,
            positional.len, has_rest ? &rest : nullptr, keywords.data,
            keywords.len) : nullptr;
    for (size_t i = 0; i < owned_names.len; i++)
        core_free(owned_names.data[i].data);
    core_list_free(owned_names);
    core_list_free(positional);
    core_list_free(keywords);
    return pack;
}

static bool
array_dimensions_have_valid_order(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *base_type)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_ordered_nodes_t *dimensions = state
        ? n00bb_node_ordered_nodes(base_type) : nullptr;
    if (!dimensions) return false;
    bool saw_unsized = false;
    for (size_t j = 0; j < dimensions->count; j++) {
        parse_tree_t *expression = CHILD_NT(grammar,
            dimensions->nodes[j], "expression");
        if (!expression) {
            saw_unsized = true;
        }
        else {
            if (saw_unsized)
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR,
                    CORE_STRING_STATIC("N00BB-A0001"),
                    CORE_STRING_STATIC(
                        "sized array dimensions must precede unsized dimensions"),
                    link_source_location(dimensions->nodes[j]));
            th_type *type = parse_tree_type(expression);
            if (!is_integer_type(type))
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR,
                    CORE_STRING_STATIC("N00BB-A0002"),
                    CORE_STRING_STATIC(
                        "array dimension expression must have an integer type"),
                    link_source_location(expression));
        }
    }
    return true;
}

bool
validate_cached_array_dimensions(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 n00bb_tu_state_t *state)
{
    if (!state) return false;
    for (size_t i = 0; i < state->array_type_nodes.len; i++)
        if (!array_dimensions_have_valid_order(grammar, unit,
                state->array_type_nodes.data[i])) return false;
    return true;
}

n00bb_node_facts_t *
n00bb_node_facts(parse_tree_t *node)
{
    return node && !core_tree_is_leaf(node)
        ? parse_tree_frontend_data(node) : nullptr;
}

n00bb_ordered_nodes_t *
n00bb_node_ordered_nodes(parse_tree_t *node)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts ? facts->ordered_nodes : nullptr;
}

n00bb_litmod_site_t *
n00bb_node_literal_site(parse_tree_t *node)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts ? facts->literal_site : nullptr;
}

parse_tree_t *
n00bb_node_record_literal_origin(parse_tree_t *node)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts ? facts->record_literal_origin : nullptr;
}

th_type *
n00bb_reference_definition(th_ctx *types, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *definition = type && types && th_kind_of(type) == TH_NAMED
        ? th_type_definition(types, type) : nullptr;
    definition = definition ? th_resolve(definition) : nullptr;
    return definition && th_kind_of(definition) == TH_INDIRECTION
        ? definition : nullptr;
}

static bool
object_definition(th_type *type, th_type *definition)
{
    th_type *shape = definition ? th_resolve(th_ind_referent(definition))
                                : nullptr;
    core_string_t name = {0}, shape_name = {0};
    core_string_t suffix = CORE_STRING_STATIC(n00bb_object_shape_suffix);
    return shape && th_named_info(th_resolve(type), &name, nullptr, nullptr)
        && th_named_info(shape, &shape_name, nullptr, nullptr)
        && shape_name.u8_bytes == name.u8_bytes + suffix.u8_bytes
        && core_string_starts_with(shape_name, name)
        && core_string_ends_with(shape_name, suffix);
}

bool
n00bb_object_type(th_ctx *types, th_type *type)
{
    return object_definition(type, n00bb_reference_definition(types, type));
}

th_type *
n00bb_dereferenced_type(th_ctx *types, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    for (;;) {
        th_type *definition = n00bb_reference_definition(types, type);
        if (object_definition(type, definition)) return type;
        if (definition) type = definition;
        th_type *option_item = type ? th_option_item(type) : nullptr;
        if (option_item) {
            type = th_resolve(option_item);
            continue;
        }
        if (!type || th_kind_of(type) != TH_INDIRECTION) return type;
        type = th_resolve(th_ind_referent(type));
    }
}

parse_sym_entry_t *
bound_function_symbol(parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    parse_sym_entry_t *symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : parse_tree_sym(node);
    if (symbol) return symbol->kind == PARSE_SYM_FUNCTION ? symbol : nullptr;
    if (core_tree_num_children(node) == 1)
        return bound_function_symbol(core_tree_child(node, 0));
    return nullptr;
}

n00bb_node_facts_t *
n00bb_ensure_node_facts(n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!state || !node || core_tree_is_leaf(node)) return nullptr;
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    if (!facts) {
        facts = core_arena_alloc(&state->semantic_values, sizeof(*facts));
        if (facts) parse_tree_set_frontend_data(node, facts);
    }
    return facts;
}

n00bb_return_source_t *
n00bb_node_return_source(parse_tree_t *node)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts ? facts->return_source : nullptr;
}

n00bb_alias_edges_t *
n00bb_node_alias_edges(parse_tree_t *node)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    return facts ? facts->alias_edges : nullptr;
}

bool
alias_declaration_is_transparent(parse_tree_t *declaration)
{
    for (size_t i = 0; declaration && !core_tree_is_leaf(declaration)
         && i < core_tree_num_children(declaration); i++) {
        parse_tree_t *child = core_tree_child(declaration, i);
        if (core_tree_is_leaf(child)
            && parse_tree_leaf_is(child, CORE_STRING_STATIC("alias")))
            return true;
    }
    return false;
}

th_type *
declared_type_from_node(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node)
{
    (void)unit;
    if (!node) return nullptr;
    if (!NT_IS(grammar, node, "alias-stmt")) {
        th_type *attached = parse_tree_type(node);
        if (attached) return attached;
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if (!symbol) {
            parse_node_resolution_t resolution = parse_tree_resolution(node);
            if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
                symbol = resolution.target.symbol;
        }
        if (symbol && symbol->type) return symbol->type;
    }
    parse_tree_t *type_spec = NT_IS(grammar, node, "type-spec") ? node
        : CHILD_NT(grammar, node, "type-spec");
    if (type_spec) return parse_tree_type(type_spec);
    return parse_tree_type(node);
}

bool
record_fields_in_source_order(parse_scope_t *scope,
                              core_list_t(parse_sym_entry_ptr_t) *fields)
{
    core_list_t(int32_t) starts = core_list_new(int32_t);
    for (parse_sym_entry_t *field = scope ? scope->first_in_scope : nullptr;
         field; field = field->next_in_scope) {
        if (!field->is_field) continue;
        int32_t start = field->decl_node
            ? core_tree_node_value(field->decl_node).start : INT32_MAX;
        size_t at = fields->len;
        while (at && starts.data[at - 1] > start) at--;
        core_list_insert(*fields, at, field);
        core_list_insert(starts, at, start);
    }
    core_list_free(starts);
    return true;
}

bool
callback_protocol_name(core_string_t type_name, core_string_t marker,
                       core_buffer_t *out)
{
    int64_t dot = type_name.data ? core_string_rfind_byte(type_name, '.')
                                 : -1;
    if (dot < 0 || (size_t)dot + 1 >= type_name.u8_bytes || !marker.data
        || !out) return false;
    size_t prefix = (size_t)dot + 1;
    core_buffer_append(out, type_name.data, prefix);
    core_buffer_append_string(out, marker);
    core_buffer_append(out, type_name.data + prefix,
                       type_name.u8_bytes - prefix);
    return out->data != nullptr;
}

bool
context_hook_name(parse_sym_entry_t *symbol, core_string_t suffix,
                  core_buffer_t *out)
{
    if (!symbol || !symbol->name.data || !suffix.data || !out)
        return false;
    core_buffer_printf(out, "__n00bb_context_%.*s_%.*s",
        (int)symbol->name.u8_bytes, symbol->name.data,
        (int)suffix.u8_bytes, suffix.data);
    return out->data != nullptr;
}

bool
context_default_name(parse_sym_entry_t *symbol, size_t index,
                     core_buffer_t *out)
{
    if (!symbol || !symbol->name.data || !out) return false;
    core_buffer_printf(out, "__n00bb_context_%.*s_default_%zu",
        (int)symbol->name.u8_bytes, symbol->name.data, index);
    return out->data != nullptr;
}

core_string_t
n00bb_context_hook_role(core_string_t name)
{
    if (!name.data) return (core_string_t){};
    if (core_string_eq(name, CORE_STRING_STATIC("context-enter")))
        return CORE_STRING_STATIC("n00bb.ir.context.enter");
    if (core_string_eq(name, CORE_STRING_STATIC("context-exit")))
        return CORE_STRING_STATIC("n00bb.ir.context.exit");
    if (core_string_eq(name, CORE_STRING_STATIC("context-fail")))
        return CORE_STRING_STATIC("n00bb.ir.context.fail");
    if (core_string_eq(name, CORE_STRING_STATIC("context-ensure")))
        return CORE_STRING_STATIC("n00bb.ir.context.ensure");
    return (core_string_t){};
}

bool
node_is_within(parse_tree_t *node, parse_tree_t *ancestor)
{
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at))
        if (at == ancestor) return true;
    return false;
}

bool
exported_type_shape(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_sym_entry_t *symbol,
                    th_env_type_shape_kind_t *shape_kind,
                    th_type **shape)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (shape_kind) *shape_kind = TH_ENV_TYPE_SHAPE_NONE;
    if (shape) *shape = nullptr;
    n00bb_interface_type_shape_t *recorded = symbol && !symbol->decl_node
        && state && state->interface_module
        ? parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.interface-type-shape"))
        : nullptr;
    if (recorded && shape_kind && shape) {
        *shape_kind = recorded->kind;
        *shape = recorded->shape;
        return true;
    }
    if (!state || !state->types || !symbol || !symbol->decl_node ||
        !shape_kind || !shape) return false;

    parse_tree_t *declaration = symbol->decl_node;
    if (NT_IS(grammar, declaration, "alias-stmt")) {
        *shape_kind = alias_declaration_is_transparent(declaration)
            ? TH_ENV_TYPE_SHAPE_ALIAS : TH_ENV_TYPE_SHAPE_NOMINAL;
        *shape = declared_type_from_node(grammar, unit, declaration);
        return *shape != nullptr;
    }

    parse_scope_t *members = symbol->member_scope;
    if (!members) {
        core_string_t tag = NT_IS(grammar, declaration, "type-decl")
            ? CORE_STRING_STATIC("type")
            : NT_IS(grammar, declaration, "record-decl")
            ? CORE_STRING_STATIC("record")
            : NT_IS(grammar, declaration, "object-decl")
            ? CORE_STRING_STATIC("object")
            : NT_IS(grammar, declaration, "variant-decl")
            ? CORE_STRING_STATIC("variant")
            : NT_IS(grammar, declaration, "enum-stmt")
            ? CORE_STRING_STATIC("enum") : (core_string_t){};
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             tag.data && scope; scope = scope->all_next) {
            if (scope->node == declaration && scope->scope_tag.data &&
                core_string_eq(scope->scope_tag, tag)) {
                members = scope;
                break;
            }
        }
    }
    if (field_record_declaration(grammar, declaration)) {
        core_list_t(th_member) members_out = core_list_new(th_member);
        core_list_t(parse_sym_entry_ptr_t) fields =
            core_list_new(parse_sym_entry_ptr_t);
        record_fields_in_source_order(members, &fields);
        bool valid = true;
        for (size_t i = 0; i < fields.len; i++) {
            parse_sym_entry_t *field = fields.data[i];
            th_type *field_type = declared_type_from_node(grammar, unit,
                                                           field->decl_node);
            if (!field_type) { valid = false; break; }
            core_list_push(members_out, ((th_member){
                .name = field->name,
                .type = field_type,
            }));
        }
        *shape_kind = NT_IS(grammar, declaration, "object-decl")
            ? TH_ENV_TYPE_SHAPE_OBJECT : TH_ENV_TYPE_SHAPE_RECORD;
        *shape = valid
            ? th_record(state->types, members_out.data, members_out.len)
            : nullptr;
        core_list_free(fields);
        core_list_free(members_out);
        return *shape != nullptr;
    }
    if (NT_IS(grammar, declaration, "variant-decl") && members) {
        core_list_t(th_member) arms = core_list_new(th_member);
        core_list_t(parse_sym_entry_ptr_t) ordered =
            variant_arms_in_source_order(grammar, members);
        bool valid = true;
        for (size_t i = 0; i < ordered.len; i++) {
            parse_sym_entry_t *arm = ordered.data[i];
            th_type *payload = declared_type_from_node(grammar, unit,
                                                        arm->decl_node);
            if (!payload) { valid = false; break; }
            core_list_push(arms, ((th_member){
                .name = arm->name,
                .type = payload,
            }));
        }
        *shape_kind = TH_ENV_TYPE_SHAPE_VARIANT;
        *shape = valid ? th_record(state->types, arms.data, arms.len) : nullptr;
        core_list_free(ordered);
        core_list_free(arms);
        return *shape != nullptr;
    }
    if (NT_IS(grammar, declaration, "enum-stmt") && members) {
        core_list_t(th_member) items = core_list_new(th_member);
        for (parse_sym_entry_t *item = members->first_in_scope; item;
             item = item->next_in_scope) {
            if (!item->decl_node || !item->type ||
                !parse_tree_nt_name(grammar, item->decl_node).data ||
                !NT_IS(grammar, item->decl_node, "enum-item")) continue;
            core_list_push(items, ((th_member){
                .name = item->name,
                .type = item->type,
            }));
        }
        *shape_kind = TH_ENV_TYPE_SHAPE_ENUM;
        *shape = th_record(state->types, items.data, items.len);
        core_list_free(items);
        return *shape != nullptr;
    }
    if (NT_IS(grammar, declaration, "flagset-stmt") && members) {
        core_list_t(th_member) items = core_list_new(th_member);
        for (parse_sym_entry_t *item = members->first_in_scope; item;
             item = item->next_in_scope) {
            if (!item->decl_node || !item->type
                || !NT_IS(grammar, item->decl_node, "flagset-item")) continue;
            core_list_push(items, ((th_member){
                .name = item->name,
                .type = item->type,
            }));
        }
        *shape_kind = TH_ENV_TYPE_SHAPE_BITSET;
        *shape = th_record(state->types, items.data, items.len);
        core_list_free(items);
        return *shape != nullptr;
    }
    return true;
}

bool
is_reference_symbol(parse_sym_entry_t *symbol)
{
    if (!symbol || !symbol->type) return false;
    th_type *type = th_resolve(symbol->type);
    return type && th_kind_of(type) == TH_INDIRECTION;
}

static n00bb_function_parameter_facts_t *
function_parameter_facts(parse_grammar_t *grammar, parse_tree_t *node);

void
collect_ir_parameters(parse_grammar_t *grammar, parse_tree_t *node,
                      core_list_t(parse_sym_entry_ptr_t) *parameters,
                      parse_sym_entry_t **rest)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (NT_IS(grammar, node, "func-type-signature"))
        node = CHILD_NT(grammar, node, "param-decl");
    if (!node) return;
    if (NT_IS(grammar, node, "context-hook-params")) {
        parse_tree_t *parameter = CHILD_NT(grammar, node, "pos-param");
        if (parameter)
            core_list_push(*parameters, parse_tree_sym(parameter));
        return;
    }
    bool trait_parameters = NT_IS(grammar, node, "trait-param-decl");
    if (!trait_parameters && !NT_IS(grammar, node, "param-decl")) return;
    if (trait_parameters) {
        parse_tree_t *receiver = CHILD_NT(grammar, node, "trait-obj-tref");
        if (receiver)
            core_list_push(*parameters, parse_tree_sym(receiver));
    }
    n00bb_function_parameter_facts_t *facts =
        function_parameter_facts(grammar, node);
    for (size_t i = 0; facts && i < facts->param_count; i++) {
        n00bb_function_parameter_ref_t *fact = &facts->params[i];
        parse_sym_entry_t *symbol = fact->node
            ? parse_tree_sym(fact->node) : nullptr;
        if (fact->variadic) *rest = symbol;
        else core_list_push(*parameters, symbol);
    }
}

bool
select_storage_classes(core_list_t(core_translation_unit_ptr_t) *units)
{
    if (!units) return false;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                bool local_value = (symbol->kind == PARSE_SYM_VARIABLE
                                    || symbol->kind == PARSE_SYM_PARAM)
                    && symbol->type;
                bool dynamic_duration = false;
                n00bb_storage_decision_t decision = n00bb_storage_decide(
                    &(n00bb_storage_facts_t){
                        .is_typed_variable = local_value,
                        .has_lexical_lifetime =
                            scope_has_lexical_lifetime(scope),
                        .is_reference = is_reference_symbol(symbol),
                        .required = symbol->storage_class_required,
                        .requested = symbol->requested_storage_class,
                        .selected = symbol->selected_storage_class,
                    }, &dynamic_duration);
                if (decision >= N00BB_STORAGE_INVALID_TARGET) {
                    core_string_t failure =
                        decision == N00BB_STORAGE_INVALID_TARGET
                        ? CORE_STRING_STATIC(
                            "storage class requirement does not refer to a typed variable")
                        : decision == N00BB_STORAGE_MISSING_CLASS
                            ? CORE_STRING_STATIC(
                                "required dynamic storage class has no name")
                            : CORE_STRING_STATIC(
                                "selected storage class conflicts with the required class");
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0046"),
                        failure,
                        symbol->decl_node
                            ? link_source_location(symbol->decl_node)
                            : (core_diag_location_t){0});
                    return false;
                }
                else if (decision == N00BB_STORAGE_EXPLICIT) {
                    parse_sym_set_storage_class(
                        symbol, symbol->requested_storage_class, true,
                        symbol->requested_storage_class);
                    if (dynamic_duration)
                        parse_sym_set_storage_duration(
                            symbol, PARSE_SYM_STORAGE_DYNAMIC);
                }
                else if (decision == N00BB_STORAGE_RUNTIME_DYNAMIC) {
                    if (dynamic_duration)
                        parse_sym_set_storage_duration(
                            symbol, PARSE_SYM_STORAGE_DYNAMIC);
                }
            }
        }
    }
    return true;
}


bool
bind_array_shape_type_source(parse_grammar_t *grammar,
                             parse_tree_t *type_reference,
                             parse_sym_entry_t *symbol)
{
    if (!type_reference || core_tree_is_leaf(type_reference) || !symbol)
        return true;
    parse_tree_t *core = parse_tree_parent(type_reference);
    if (!NT_IS(grammar, core, "base-tspec-core")
        || core_tree_num_children(core) != 1
        || core_tree_child(core, 0) != type_reference) return true;
    parse_tree_t *base = parse_tree_parent(core);
    return !NT_IS(grammar, base, "base-tspec")
        || parse_tree_set_resolution_symbol(base, symbol);
}

typedef struct {
    core_string_t name;
    core_list_t(parse_sym_entry_ptr_t) symbols;
} type_symbol_bucket_t;

typedef struct {
    th_ctx *types;
    th_type *wanted;
    core_string_t name;
    size_t args;
    uint64_t hash;
    bool hashed;
} type_symbol_query_t;

static bool
type_symbol_name(parse_sym_entry_t *symbol, core_string_t *name)
{
    return symbol->type && symbol->kind == PARSE_SYM_TYPEDEF
        && !alias_declaration_is_transparent(
               canonical_symbol(symbol)->decl_node)
        && th_named_info(th_resolve(symbol->type), name, nullptr, nullptr);
}

static bool
type_symbol_matches(type_symbol_query_t *query, parse_sym_entry_t *symbol)
{
    if (!query->hashed) {
        query->hash = th_hash(query->types, query->wanted);
        query->hashed = true;
    }
    n00bb_signature_typevars_t *variables = parse_sym_binding(
        symbol, N00BB_SIGNATURE_TYPEVARS_KEY);
    return th_hash(query->types, th_resolve(symbol->type)) == query->hash
        || (symbol->member_scope && variables
            && variables->len == query->args);
}

static parse_namespace_t *
type_namespace(n00bb_tu_state_t *state)
{
    return state && state->annotations
        ? parse_symtab_ns(state->annotations->symtab,
                          CORE_STRING_STATIC("type")) : nullptr;
}

void
n00bb_index_type_symbols(n00bb_tu_state_t *state)
{
    parse_namespace_t *type_ns = type_namespace(state);
    if (!type_ns || state->type_symbols_indexed) return;
    core_dict_init_string(&state->type_symbol_index);
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        if (scope->ns != type_ns) continue;
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            core_string_t name = {};
            if (!type_symbol_name(symbol, &name)) continue;
            type_symbol_bucket_t *bucket = core_dict_get(
                &state->type_symbol_index, &name, nullptr);
            if (!bucket) {
                bucket = core_alloc(type_symbol_bucket_t);
                bucket->name = name;
                bucket->symbols = core_list_new(parse_sym_entry_ptr_t);
                core_dict_put(&state->type_symbol_index, &bucket->name,
                              bucket);
            }
            core_list_push(bucket->symbols, symbol);
        }
    }
    state->type_symbols_indexed = true;
}

static void
free_type_symbol_index(n00bb_tu_state_t *state)
{
    if (!state->type_symbols_indexed) return;
    core_dict_t *index = &state->type_symbol_index;
    for (size_t i = 0; i < index->capacity; i++) {
        if (index->buckets[i].state != _CORE_BUCKET_OCCUPIED) continue;
        type_symbol_bucket_t *bucket = index->values[i];
        core_list_free(bucket->symbols);
        core_free(bucket);
    }
    core_dict_free(index);
    state->type_symbols_indexed = false;
}

static parse_sym_entry_t *
unit_type_symbol(n00bb_tu_state_t *state, type_symbol_query_t *query)
{
    if (state && state->semantic_annotation_complete
        && state->type_symbols_indexed) {
        type_symbol_bucket_t *bucket = core_dict_get(
            &state->type_symbol_index, &query->name, nullptr);
        for (size_t i = 0; bucket && i < bucket->symbols.len; i++)
            if (type_symbol_matches(query, bucket->symbols.data[i]))
                return bucket->symbols.data[i];
        return nullptr;
    }
    parse_namespace_t *type_ns = type_namespace(state);
    for (parse_scope_t *scope = type_ns
            ? state->annotations->symtab->all_scopes : nullptr;
         scope; scope = scope->all_next) {
        if (scope->ns != type_ns) continue;
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            core_string_t name = {};
            if (type_symbol_name(symbol, &name)
                && core_string_eq(name, query->name)
                && type_symbol_matches(query, symbol))
                return symbol;
        }
    }
    return nullptr;
}

parse_sym_entry_t *
type_symbol_in_loaded_imports(core_translation_unit_t *root, th_ctx *types,
                              th_type *wanted,
                              core_translation_unit_t **owner_unit_out)
{
    if (owner_unit_out) *owner_unit_out = nullptr;
    type_symbol_query_t query = {.types = types, .wanted = wanted};
    if (!th_named_info(wanted, &query.name, nullptr, &query.args))
        return nullptr;
    core_list_t(core_translation_unit_ptr_t) pending =
        core_list_new(core_translation_unit_ptr_t);
    core_dict_t visited = {0};
    core_dict_init(&visited, core_hash_word, nullptr);
    core_list_push(pending, root);
    parse_sym_entry_t *found = nullptr;
    for (size_t at = 0; at < pending.len && !found; at++) {
        core_translation_unit_t *unit = pending.data[at];
        if (!core_dict_add(&visited, unit, unit)) continue;
        found = unit_type_symbol(tu_state(unit), &query);
        if (found && owner_unit_out) *owner_unit_out = unit;
        for (size_t i = 0; !found && i < unit->import_count; i++)
            core_list_push(pending, unit->imports[i]);
    }
    core_dict_free(&visited);
    core_list_free(pending);
    return found;
}

n00bb_callback_contract_t *
n00bb_callback_contract_for_type(n00bb_tu_state_t *state, th_type *type)
{
    if (!state || !state->unit || !state->types || !type) return nullptr;
    core_string_t name = {};
    if (!th_named_info(th_resolve(type), &name, nullptr, nullptr))
        return nullptr;
    th_type *base = name.data ? th_name(state->types, name) : nullptr;
    parse_sym_entry_t *symbol = type_symbol_in_loaded_imports(
        state->unit, state->types, base, nullptr);
    symbol = canonical_symbol(symbol);
    return symbol ? parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.callback-contract")) : nullptr;
}

th_type *
n00bb_specialize_callback_role(th_ctx *types,
    n00bb_callback_contract_t *contract, th_type *receiver, th_type *role)
{
    if (!types || !contract || !receiver || !role) return nullptr;
    size_t argument_count = 0;
    if (!th_named_info(th_resolve(receiver), nullptr, nullptr,
                       &argument_count)) return nullptr;
    th_type **handler_variables = nullptr;
    size_t variable_count = th_free_vars(types, contract->handler->type,
                                         &handler_variables);
    if (argument_count != variable_count) return nullptr;
    th_type **role_variables = nullptr;
    size_t role_count = th_free_vars(types, role, &role_variables);
    if (!role_count) return th_type_clone(types, role);
    th_type **replacements = core_alloc_array(th_type *, role_count);
    if (!replacements) return nullptr;
    bool matched = true;
    for (size_t i = 0; matched && i < role_count; i++) {
        core_string_t role_name = th_var_name(role_variables[i]);
        matched = false;
        for (size_t j = 0; j < variable_count; j++) {
            core_string_t handler_name = th_var_name(handler_variables[j]);
            const th_arg *argument = th_named_arg(receiver, j);
            if (!role_name.data || !handler_name.data || !argument
                || argument->kind != TH_ARG_TYPE
                || !core_string_eq(role_name, handler_name)) continue;
            replacements[i] = argument->type;
            matched = true;
            break;
        }
    }
    th_type *specialized = matched
        ? th_type_clone_replacing(types, role,
            (const th_type *const *)role_variables, replacements, role_count)
        : nullptr;
    core_free(replacements);
    return specialized;
}

th_type *
n00bb_callback_method_signature(th_ctx *types, th_type *receiver,
                                core_string_t name, void *context)
{
    if (!types || !receiver
        || !core_string_eq(name, CORE_STRING_STATIC("call"))) return nullptr;
    th_type *resolved = th_resolve(receiver);
    th_type *function = resolved && th_kind_of(resolved) == TH_INDIRECTION
        ? th_resolve(th_ind_referent(resolved)) : nullptr;
    n00bb_tu_state_t *state = context;
    if (!function || th_kind_of(function) != TH_FUNCTION) {
        n00bb_callback_contract_t *contract =
            n00bb_callback_contract_for_type(state, resolved);
        function = contract && contract->call
            ? n00bb_specialize_callback_role(types, contract, resolved,
                                             contract->call->type) : nullptr;
    }
    if (function && th_kind_of(function) == TH_BINDER)
        function = th_instantiate(types, function);
    if (!function || th_kind_of(function) != TH_FUNCTION) return nullptr;

    core_list_t(th_param) parameters = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    core_list_push(parameters, ((th_param){.mode = TH_IN,
                                           .type = receiver}));
    for (size_t i = 0; i < th_func_nparams(function); i++)
        core_list_push(parameters, *th_func_param(function, i));
    for (size_t i = 0; i < th_func_nkw(function); i++)
        core_list_push(keywords, *th_func_kw(function, i));
    th_type *signature = th_func_with_error(types, th_func_ret(function),
        th_func_error(function), parameters.data, parameters.len,
        th_func_rest(function), keywords.data, keywords.len);
    core_list_free(parameters);
    core_list_free(keywords);
    return signature;
}

bool
n00bb_bind_callback_call(n00bb_tu_state_t *state, n00bb_link_use_site_t *site)
{
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    parse_namespace_t *builtins = symtab
        ? parse_symtab_ns(symtab, CORE_STRING_STATIC("builtin")) : nullptr;
    if (!symtab || !builtins || !builtins->root) return false;
    th_type *receiver_type = site->receiver_node
        ? parse_tree_type(site->receiver_node) : nullptr;
    th_type *signature = n00bb_callback_method_signature(
        state->types, receiver_type, CORE_STRING_STATIC("call"), state);
    if (!signature && receiver_type) {
        receiver_type = n00bb_dereferenced_type(state->types, receiver_type);
        signature = n00bb_callback_method_signature(state->types,
            receiver_type, CORE_STRING_STATIC("call"), state);
    }
    n00bb_callback_contract_t *callback_contract =
        n00bb_callback_contract_for_type(state, receiver_type);
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__callback_call_%zu",
                       (size_t)(site - state->link_use_sites.data));
    parse_scope_t *saved = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             builtins->root);
    parse_sym_entry_t *symbol = signature && name.data
        ? parse_symtab_add(symtab, CORE_STRING_STATIC("builtin"),
            core_buffer_string(&name), PARSE_SYM_FUNCTION,
            nullptr) : nullptr;
    core_buffer_release(&name);
    if (saved)
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"), saved);
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.callback-call"), symbol)) return false;
    symbol->type = signature;
    symbol->callable_contract = signature;
    symbol->is_synthetic = true;
    if (callback_contract && callback_contract->call) {
        symbol->decl_node = callback_contract->call->decl_node;
        if (!parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.callback-public-signature"),
                callback_contract->call->type)) return false;
    }
    symbol->visibility = CORE_STRING_STATIC("public");
    parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
    site->callback_call = true;
    site->symbol = symbol;
    return parse_tree_set_resolution_symbol(site->node, symbol)
        && parse_sym_add_use(symbol, site->node);
}

bool
prepare_callback_call_symbols(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return false;
    for (size_t i = 0; i < state->link_use_sites.len; i++) {
        n00bb_link_use_site_t *site = &state->link_use_sites.data[i];
        if (site->callback_call && !n00bb_bind_callback_call(state, site))
            return false;
    }
    return true;
}

parse_sym_entry_t *
enclosing_function_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *parent = node; parent;
         parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "func-def"))
            return parse_tree_sym(parent);
    }
    return nullptr;
}

parse_sym_entry_t *
link_effect_owner_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_sym_entry_t *function = enclosing_function_symbol(grammar, node);
    if (function) return function;
    for (parse_tree_t *parent = node; parent;
         parent = parse_tree_parent(parent)) {
        if (!NT_IS(grammar, parent, "context-def")) continue;
        parse_sym_entry_t *context = parse_tree_sym(parent);
        return context_symbol(grammar, context) ? context : nullptr;
    }
    return nullptr;
}

bool
context_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    return symbol && ((symbol->decl_node
        && NT_IS(grammar, symbol->decl_node, "context-def"))
        || core_string_eq(symbol->adt_kind, CORE_STRING_STATIC("context")));
}

bool
is_module_global_symbol(parse_grammar_t *grammar,
                        parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_VARIABLE && symbol->scope
        && !context_symbol(grammar, symbol)
        && (core_string_eq(symbol->scope->scope_tag,
                           CORE_STRING_STATIC("module"))
            || core_string_eq(symbol->scope->scope_tag,
                              CORE_STRING_STATIC("package-interface")));
}

parse_tree_t *
simple_identifier_primary(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!grammar || !node) return nullptr;
    while (!core_tree_is_leaf(node) && !NT_IS(grammar, node, "primary")) {
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return NT_IS(grammar, node, "primary") ? node : nullptr;
}

bool
simple_identifier_expression(parse_grammar_t *grammar, parse_tree_t *node,
                             core_string_t *name)
{
    parse_tree_t *primary = simple_identifier_primary(grammar, node);
    if (!primary || core_tree_num_children(primary) != 1) return false;
    parse_tree_t *identifier = core_tree_child(primary, 0);
    parse_token_info_t *token = parse_node_token(identifier);
    if (!token || !core_string_eq(parse_terminal_name(grammar, parse_token_tid(token)),
                                  CORE_STRING_STATIC("IDENTIFIER")))
        return false;
    if (name) *name = parse_tree_leaf_text(identifier);
    return true;
}

static bool compiling_build_time;
static bool compiling_literal_stage;

void
n00bb_set_build_time(bool build_time)
{
    compiling_build_time = build_time;
}

void
n00bb_set_literal_stage(bool literal_stage)
{
    compiling_literal_stage = literal_stage;
}

bool
n00bb_literal_stage(void)
{
    return compiling_build_time && compiling_literal_stage;
}

size_t
n00bb_initializer_kinds(void)
{
    return n00bb_literal_stage() ? 1 : 2;
}

parse_sym_phase_t
n00bb_compile_phase(void)
{
    return compiling_build_time ? PARSE_SYM_PHASE_COMPTIME
                                : PARSE_SYM_PHASE_RUNTIME;
}

bool
scope_is_within_function(parse_scope_t *scope)
{
    for (parse_scope_t *at = scope; at; at = at->parent)
        if (core_string_eq(at->scope_tag,
                           CORE_STRING_STATIC("function"))
            || core_string_eq(at->scope_tag,
                              CORE_STRING_STATIC("context-hook")))
            return true;
    return false;
}

bool
scope_is_within(parse_scope_t *scope, parse_scope_t *ancestor)
{
    for (parse_scope_t *at = scope; at; at = at->parent)
        if (at == ancestor) return true;
    return false;
}

bool
scope_has_lexical_lifetime(parse_scope_t *scope)
{
    static const core_string_t local_scopes[] = {
        CORE_STRING_STATIC("block"), CORE_STRING_STATIC("comptime"),
        CORE_STRING_STATIC("context"), CORE_STRING_STATIC("context-hook"),
        CORE_STRING_STATIC("for"), CORE_STRING_STATIC("function"),
        CORE_STRING_STATIC("if"), CORE_STRING_STATIC("switch"),
        CORE_STRING_STATIC("try"), CORE_STRING_STATIC("try-error"),
        CORE_STRING_STATIC("typecase"), CORE_STRING_STATIC("while"),
        CORE_STRING_STATIC("with"),
    };
    for (parse_scope_t *at = scope; at; at = at->parent) {
        for (size_t i = 0; i < sizeof(local_scopes) / sizeof(local_scopes[0]);
             i++)
            if (core_string_eq(at->scope_tag, local_scopes[i])) return true;
        if (core_string_eq(at->scope_tag, CORE_STRING_STATIC("module")))
            return false;
    }
    return false;
}

parse_scope_t *
lexical_function_scope(parse_scope_t *scope)
{
    for (parse_scope_t *at = scope; at; at = at->parent) {
        if (core_string_eq(at->scope_tag,
                           CORE_STRING_STATIC("function"))
            || core_string_eq(at->scope_tag,
                              CORE_STRING_STATIC("context-hook")))
            return at;
        if (core_string_eq(at->scope_tag, CORE_STRING_STATIC("module")))
            return nullptr;
    }
    return nullptr;
}

static void
apply_decl_binding(parse_grammar_t *grammar, parse_tree_t *node,
                   parse_sym_mutability_t mutability, bool threaded)
{
    if (!node || !NT_IS(grammar, node, "sym-decl")) return;
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    if (!symbol) return;
    symbol->mutability = mutability;
    symbol->is_thread_local = threaded;
    bool function_local = scope_is_within_function(symbol->scope);
    parse_sym_set_storage_duration(symbol, threaded
        ? PARSE_SYM_STORAGE_THREAD
        : function_local ? PARSE_SYM_STORAGE_AUTOMATIC
                         : PARSE_SYM_STORAGE_MODULE);
    parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
        mutability == PARSE_SYM_MUTABLE ? PARSE_SYM_PHASE_BOTH
        : mutability == PARSE_SYM_CONST ? PARSE_SYM_PHASE_COMPTIME
                                        : PARSE_SYM_PHASE_NONE,
        PARSE_SYM_PHASE_BOTH);
    if (!CHILD_NT(grammar, node, "type-spec"))
        parse_sym_mark_type_inferred(symbol);
}

static parse_scope_t *
inferred_binding_scope(parse_scope_t *scope)
{
    for (parse_scope_t *s = scope; s; s = s->parent) {
        if (core_string_eq(s->scope_tag, CORE_STRING_STATIC("function"))
            || core_string_eq(s->scope_tag,
                              CORE_STRING_STATIC("context-hook"))
            || core_string_eq(s->scope_tag, CORE_STRING_STATIC("module")))
            return s;
    }
    return scope;
}

static void
apply_declaration_defaults(parse_visit_t *visit)
{
    parse_sym_entry_t *declared = parse_tree_sym(visit->node);
    if (declared && declared->decl_node == visit->node
        && declared->kind == PARSE_SYM_TYPEDEF)
        parse_sym_set_phases(declared, PARSE_SYM_PHASE_BOTH,
            PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
    if (declared && declared->decl_node == visit->node) {
        if (!declared->visibility.u8_bytes
            && (NT_IS(visit->grammar, visit->node, "func-def")
                || field_record_declaration(visit->grammar, visit->node)
                || NT_IS(visit->grammar, visit->node, "enum-stmt")
                || NT_IS(visit->grammar, visit->node, "flagset-stmt")
                || NT_IS(visit->grammar, visit->node, "variant-decl")
                || NT_IS(visit->grammar, visit->node, "trait-decl")
                || NT_IS(visit->grammar, visit->node, "alias-stmt")
                || NT_IS(visit->grammar, visit->node, "callback-decl")
                || NT_IS(visit->grammar, visit->node, "context-def")))
            declared->visibility = CORE_STRING_STATIC("public");
        if (field_record_declaration(visit->grammar, visit->node)
            && core_string_eq(declared->visibility,
                              CORE_STRING_STATIC("public")))
            for (parse_sym_entry_t *member = declared->member_scope
                     ? declared->member_scope->first_in_scope : nullptr;
                 member; member = member->next_in_scope)
                if (member->is_field && !member->visibility.u8_bytes)
                    member->visibility = CORE_STRING_STATIC("public");
    }
}

void
declared_symbol_node(parse_visit_t *visit, bool entering, void *context)
{
    (void)context;
    if (!entering && visit->pn) apply_declaration_defaults(visit);
}

void
initial_symbol_node(parse_visit_t *visit, bool entering, void *context)
{
    if (entering) return;
    parse_annot_result_t *annotations = context;
    if (!visit->pn) return;
    parse_sym_entry_t *declared = parse_tree_sym(visit->node);
    apply_declaration_defaults(visit);
    if (NT_IS(visit->grammar, visit->node, "sym-decl")
        && declared && declared->decl_node == visit->node
        && declared->shadowed
        && declared->shadowed->kind != PARSE_SYM_MODULE
        && declared->scope == declared->shadowed->scope) {
        core_diagnostics_report_at(
            annotations->diagnostics, annotations->diagnostic_unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-N0001"),
            CORE_STRING_STATIC("duplicate declaration in the same scope"),
            link_source_location(visit->node));
        return;
    }

    if (NT_IS(visit->grammar, visit->node, "sym-decl")) {
        parse_tree_t *declaration = parse_tree_parent(visit->node);
        while (declaration && !NT_IS(visit->grammar, declaration,
                                    "variable-decl"))
            declaration = parse_tree_parent(declaration);
        parse_tree_t *prefix = CHILD_NT(visit->grammar, declaration,
                                        "declaration-prefix");
        if (prefix) {
            parse_tree_t *kind = CHILD_NT(visit->grammar, prefix,
                                          "binding-kind");
            parse_tree_t *storage = CHILD_NT(visit->grammar, prefix,
                                             "storage-class");
            core_string_t spelling = first_token_text(kind);
            parse_sym_mutability_t mutability =
                core_string_eq(spelling, CORE_STRING_STATIC("const"))
                    ? PARSE_SYM_CONST
                    : core_string_eq(spelling, CORE_STRING_STATIC("let"))
                        ? PARSE_SYM_IMMUTABLE : PARSE_SYM_MUTABLE;
            bool threaded = core_string_eq(first_token_text(storage),
                                           CORE_STRING_STATIC("thread"));
            apply_decl_binding(visit->grammar, visit->node,
                               mutability, threaded);
            parse_sym_entry_t *symbol = declared;
            parse_sym_entry_t *previous = symbol ? symbol->shadowed : nullptr;
            if (previous && previous->kind == PARSE_SYM_VARIABLE
                && previous->mutability == PARSE_SYM_IMMUTABLE
                && previous->scope != symbol->scope
                && scope_is_within_function(previous->scope)
                && scope_is_within(symbol->scope, previous->scope)) {
                bool used_before = false;
                for (size_t i = 0; i < previous->use_count && !used_before; i++) {
                    parse_tree_t *use = previous->use_nodes[i];
                    if (core_tree_node_value(use).start
                        >= core_tree_node_value(visit->node).start) continue;
                    for (parse_tree_t *parent = use; parent;
                         parent = parse_tree_parent(parent))
                        if (parent == symbol->scope->node) {
                            used_before = true;
                            break;
                        }
                }
                core_diagnostics_report_at(
                    annotations->diagnostics, annotations->diagnostic_unit,
                    used_before ? CORE_DIAG_FATAL : CORE_DIAG_WARNING,
                    used_before ? CORE_STRING_STATIC("N00BB-N0002")
                                : CORE_STRING_STATIC("N00BB-W0007"),
                    used_before
                        ? CORE_STRING_STATIC("cannot shadow a let binding already used in this block")
                        : CORE_STRING_STATIC("nested declaration shadows an outer let binding"),
                    link_source_location(visit->node));
            }
        }
    }
    else if (NT_IS(visit->grammar, visit->node, "pos-param")
             || NT_IS(visit->grammar, visit->node, "default-param")
             || NT_IS(visit->grammar, visit->node, "vargs-param")) {
        if (declared) {
            declared->is_optional = NT_IS(visit->grammar, visit->node,
                                                "default-param");
            declared->is_variadic = NT_IS(visit->grammar, visit->node,
                                                "vargs-param");
            if (declared->has_mode
                && (th_mode_produces_output(declared->mode)
                    || declared->mode == TH_CONSUME))
                declared->mutability = PARSE_SYM_MUTABLE;
            parse_sym_set_phases(declared, PARSE_SYM_PHASE_BOTH,
                declared->mutability == PARSE_SYM_MUTABLE
                    ? PARSE_SYM_PHASE_BOTH : PARSE_SYM_PHASE_NONE,
                PARSE_SYM_PHASE_NONE);
            parse_sym_set_storage_duration(declared,
                                           PARSE_SYM_STORAGE_AUTOMATIC);
        }
        if (declared
            && !CHILD_NT(visit->grammar, visit->node, "type-spec"))
            parse_sym_mark_type_inferred(declared);
    }
    else if (NT_IS(visit->grammar, visit->node, "func-def")) {
        if (declared) {
            parse_sym_set_phases(declared, PARSE_SYM_PHASE_BOTH,
                PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_NONE);
            parse_sym_mark_type_inferred(declared);
        }
    }
    else if (NT_IS(visit->grammar, visit->node, "context-manager")
             && declared) {
        parse_sym_set_phases(declared, PARSE_SYM_PHASE_BOTH,
                             PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_BOTH);
    }
    else if ((NT_IS(visit->grammar, visit->node, "for-binding")
              || NT_IS(visit->grammar, visit->node, "try-error-binding"))
             && declared) {
        parse_sym_entry_t *symbol = declared;
        if (NT_IS(visit->grammar, visit->node, "for-binding"))
            symbol->mutability = PARSE_SYM_IMMUTABLE;
        bool for_binding = NT_IS(visit->grammar, visit->node, "for-binding");
        parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
            !for_binding && symbol->mutability == PARSE_SYM_MUTABLE
                ? PARSE_SYM_PHASE_BOTH : PARSE_SYM_PHASE_NONE,
            for_binding ? PARSE_SYM_PHASE_BOTH : PARSE_SYM_PHASE_NONE);
        if (for_binding) {
            parse_sym_set_storage_duration(symbol,
                                           PARSE_SYM_STORAGE_AUTOMATIC);
            if (!CHILD_NT(visit->grammar, visit->node, "type-spec"))
                parse_sym_mark_type_inferred(symbol);
            if (core_string_eq(first_token_text(visit->node),
                               CORE_STRING_STATIC("let")))
                parse_sym_set_binding(symbol,
                    CORE_STRING_STATIC("n00bb.explicit-loop-let"), symbol);
        }
        else {
            parse_sym_set_storage_duration(symbol,
                                           PARSE_SYM_STORAGE_AUTOMATIC);
        }
        if (!parse_sym_add_definition(symbol, visit->node))
            core_diagnostics_report_at(
                annotations->diagnostics,
                annotations->diagnostic_unit, CORE_DIAG_ERROR,
                CORE_STRING_STATIC("N00BB-L0013"),
                CORE_STRING_STATIC(
                    "could not record the binding definition site"),
                link_source_location(visit->node));
    }
    else if (NT_IS(visit->grammar, visit->node,
                   "visible-inferred-assign")) {
        parse_tree_t *assignment = CHILD_NT(visit->grammar, visit->node,
                                             "assign-stmt");
        if (assignment) {
            parse_node_resolution_t resolution =
                parse_tree_resolution(assignment);
            parse_sym_entry_t *symbol = resolution.kind
                    == PARSE_NODE_RESOLUTION_SYMBOL
                ? resolution.target.symbol : nullptr;
            if (symbol && parse_tree_set_resolution_symbol(visit->node,
                                                            symbol))
                symbol->visibility = first_token_text(visit->node);
        }
    }
    else if (NT_IS(visit->grammar, visit->node, "assign-stmt")) {
        parse_tree_t *target = core_tree_child(visit->node, 0);
        core_string_t name = {0};
        if (!simple_identifier_expression(visit->grammar, target, &name))
            return;
        parse_namespace_t *ns = parse_symtab_ns(annotations->symtab,
                                                 CORE_STRING_STATIC(""));
        parse_scope_t *current = ns ? ns->current : nullptr;
        if (!name.data || !current
            || parse_symtab_lookup_scoped(annotations->symtab, current,
                CORE_STRING_STATIC(""), name)) return;

        parse_scope_t *destination = inferred_binding_scope(current);
        parse_symtab_enter_scope(annotations->symtab, CORE_STRING_STATIC(""),
                                 destination);
        parse_sym_entry_t *symbol = parse_symtab_add(annotations->symtab,
            CORE_STRING_STATIC(""), name, PARSE_SYM_VARIABLE, nullptr);
        parse_symtab_enter_scope(annotations->symtab, CORE_STRING_STATIC(""),
                                 current);
        if (symbol) {
            if (!parse_sym_add_definition(symbol, visit->node)) return;
            symbol->mutability = PARSE_SYM_MUTABLE;
            parse_sym_set_storage_duration(symbol,
                scope_is_within_function(current)
                    ? PARSE_SYM_STORAGE_AUTOMATIC : PARSE_SYM_STORAGE_MODULE);
            parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                PARSE_SYM_PHASE_BOTH, PARSE_SYM_PHASE_BOTH);
            parse_tree_t *value = core_tree_child(visit->node, 2);
            if (value && !core_tree_is_leaf(value))
                symbol->type = parse_tree_type(value);
            parse_sym_mark_type_inferred(symbol);
            parse_tree_set_resolution_symbol(visit->node, symbol);
        }
    }
}

parse_tree_t *
symbol_binding_node(parse_sym_entry_t *symbol)
{
    if (!symbol) return nullptr;
    if (symbol->decl_node) return symbol->decl_node;
    return symbol->definition_count ? symbol->definition_nodes[0] : nullptr;
}

bool
symbol_has_definition(parse_sym_entry_t *symbol, parse_tree_t *node)
{
    if (!symbol || !node) return false;
    for (size_t i = 0; i < symbol->definition_count; i++)
        if (symbol->definition_nodes[i] == node) return true;
    return false;
}

bool
scope_in_value_namespace(parse_scope_t *scope)
{
    return scope && scope->ns
        && core_string_eq(scope->ns->ns_name, CORE_STRING_STATIC(""));
}

bool
is_named_type_declaration(parse_grammar_t *grammar,
                          parse_sym_entry_t *symbol)
{
    return symbol && symbol->decl_node
        && (field_record_declaration(grammar, symbol->decl_node)
            || NT_IS(grammar, symbol->decl_node, "alias-stmt")
            || NT_IS(grammar, symbol->decl_node, "enum-stmt")
            || NT_IS(grammar, symbol->decl_node, "flagset-stmt")
            || NT_IS(grammar, symbol->decl_node, "variant-decl")
            || NT_IS(grammar, symbol->decl_node, "callback-decl")
            || NT_IS(grammar, symbol->decl_node, "trait-decl"));
}

bool
field_record_declaration(parse_grammar_t *grammar, parse_tree_t *node)
{
    return NT_IS(grammar, node, "type-decl")
        || NT_IS(grammar, node, "record-decl")
        || NT_IS(grammar, node, "object-decl");
}

bool
n00bb_field_scope_tag(core_string_t tag)
{
    return core_string_eq(tag, CORE_STRING_STATIC("type"))
        || core_string_eq(tag, CORE_STRING_STATIC("record"))
        || core_string_eq(tag, CORE_STRING_STATIC("object"));
}

bool
n00bb_object_type_declared(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *value)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    if ((!type || th_kind_of(type) != TH_NAMED) && value && value->decl_node)
        type = declared_type_from_node(grammar, unit, value->decl_node);
    type = type ? th_resolve(type) : nullptr;
    parse_sym_entry_t *symbol = state && type
        && th_kind_of(type) == TH_NAMED
        ? type_symbol_in_loaded_imports(unit, state->types, type, nullptr)
        : nullptr;
    symbol = symbol ? canonical_symbol(symbol) : nullptr;
    return symbol && ((symbol->decl_node
                       && NT_IS(grammar, symbol->decl_node, "object-decl"))
                      || core_string_eq(symbol->adt_kind,
                                        CORE_STRING_STATIC("object")));
}

static void
free_tu_state(void *data)
{
    n00bb_tu_state_t *state = data;
    if (!state) return;
    for (size_t i = 0; i < state->link_use_sites.len; i++)
        core_free(state->link_use_sites.data[i].trait_path_parts);
    core_list_free(state->link_use_sites);
    for (size_t i = 0; i < state->dispatch_records.len; i++) {
        core_free(state->dispatch_records.data[i].trait.data);
        core_free(state->dispatch_records.data[i].method.data);
    }
    core_list_free(state->dispatch_records);
    core_list_free(state->literal_backed_consts);
    core_list_free(state->import_sources);
    core_list_free(state->path_imports);
    core_list_free(state->restricted_imports);
    core_list_free(state->brought_names);
    core_list_free(state->deferred_conversions);
    core_list_free(state->integer_seeds);
    core_list_free(state->integer_edges);
    core_list_free(state->integer_guards);
    core_list_free(state->integer_shift_sites);
    core_dict_free(&state->integer_shift_seen);
    core_list_free(state->module_top_level_statements);
    core_list_free(state->literal_constants);
    core_list_free(state->array_type_nodes);
    for (size_t i = 0; i < state->literal_modifiers.len; i++) {
        core_free(state->literal_modifiers.data[i].modifier.data);
        core_free(state->literal_modifiers.data[i].kind.data);
    }
    core_list_free(state->literal_modifiers);
    for (size_t i = 0; i < state->package_declarations.len; i++)
        core_list_free(state->package_declarations.data[i].exports);
    core_list_free(state->package_declarations);
    core_list_free(state->import_aliases);
    core_dict_free(&state->import_alias_index);
    core_list_free(state->literal_site_nodes);
    core_list_free(state->shape_literal_nodes);
    core_dict_free(&state->flagset_values);
    core_dict_free(&state->shared_state_types);
    core_dict_free(&state->integer_edge_sites);
    core_dict_free(&state->integer_range_nodes);
    core_dict_free(&state->integer_tracked_symbols);
    core_dict_free(&state->declarations_typed_ahead);
    free_type_symbol_index(state);
    n00bb_range_state_release(state);
    core_arena_free(&state->semantic_values);
    parse_annot_result_free(state->annotations);
    if (state->lex) {
        parse_pwz_free(state->lex->parser);
        scan_token_stream_free(state->lex->tokens);
        core_free(state->lex);
    }
    core_free(state);
}

n00bb_tu_state_t *
n00bb_tu_state_new(core_translation_unit_t *unit, n00bb_lex_result_t *lex,
                   th_ctx *types)
{
    n00bb_tu_state_t *state = core_alloc(n00bb_tu_state_t);
    memset(state, 0, sizeof(*state));
    core_arena_init(&state->semantic_values, 1024);
    core_dict_init(&state->shared_state_types, nullptr, nullptr);
    state->link_use_sites = core_list_new(n00bb_link_use_site_t);
    state->module_top_level_statements = core_list_new(parse_tree_ptr_t);
    state->literal_constants = core_list_new(n00bb_literal_constant_t);
    state->literal_backed_consts =
        core_list_new(n00bb_literal_backed_const_t);
    state->array_type_nodes = core_list_new(parse_tree_ptr_t);
    state->literal_modifiers = core_list_new(n00bb_user_litmod_t);
    state->package_declarations = core_list_new(n00bb_package_declaration_t);
    state->import_aliases = core_list_new(n00bb_import_alias_t);
    core_dict_init_string(&state->import_alias_index);
    state->literal_site_nodes = core_list_new(parse_tree_ptr_t);
    state->shape_literal_nodes = core_list_new(parse_tree_ptr_t);
    core_dict_init(&state->flagset_values, nullptr, nullptr);
    core_dict_init(&state->integer_edge_sites, nullptr, nullptr);
    core_dict_init(&state->integer_range_nodes, nullptr, nullptr);
    core_dict_init(&state->integer_shift_seen, nullptr, nullptr);
    core_dict_init(&state->integer_tracked_symbols, nullptr, nullptr);
    core_dict_init(&state->declarations_typed_ahead, nullptr, nullptr);
    core_dict_init(&state->integer_symbol_ranges.index, nullptr, nullptr);
    core_dict_init(&state->integer_range_invalidated_symbols, nullptr, nullptr);
    core_dict_init(&state->integer_solved_symbols, nullptr, nullptr);
    core_dict_init(&state->integer_boundary_sites, nullptr, nullptr);
    core_dict_init(&state->integer_narrowing_sites, nullptr, nullptr);
    core_dict_init(&state->integer_reference_taken, nullptr, nullptr);
    state->lex = lex;
    state->unit = unit;
    state->types = types;
    state->interface_module = n00bb_source_file_is_interface(&unit->source);
    core_translation_unit_set_user_data(unit, state, free_tu_state);
    return state;
}

static parse_scope_t *
module_scope_in_namespace(n00bb_tu_state_t *state,
                          core_string_t namespace_name)
{
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    parse_namespace_t *ns = symtab
        ? parse_symtab_ns(symtab, namespace_name) : nullptr;
    if (!ns || !ns->root) return nullptr;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next)
        if (scope->parent == ns->root
            && core_string_eq(scope->scope_tag,
                              CORE_STRING_STATIC("module")))
            return scope;
    return nullptr;
}

parse_scope_t *
module_scope(n00bb_tu_state_t *state)
{
    return module_scope_in_namespace(state, CORE_STRING_STATIC(""));
}

parse_scope_t *
type_module_scope(n00bb_tu_state_t *state)
{
    return module_scope_in_namespace(state, CORE_STRING_STATIC("type"));
}

core_list_t(parse_scope_ptr_t)
module_scopes_in_namespace(n00bb_tu_state_t *state,
                           core_string_t namespace_name)
{
    core_list_t(parse_scope_ptr_t) scopes = core_list_new(parse_scope_ptr_t);
    if (!state || !state->annotations || !state->annotations->symtab)
        return scopes;
    parse_symtab_t *symtab = state->annotations->symtab;
    parse_namespace_t *ns = parse_symtab_ns(symtab, namespace_name);
    if (!ns || !ns->root) return scopes;
    core_list_push(scopes, ns->root);
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (scope->parent == ns->root
            && core_string_eq(scope->scope_tag,
                              CORE_STRING_STATIC("module")))
            core_list_push(scopes, scope);
    }
    return scopes;
}

core_list_t(parse_scope_ptr_t)
module_scopes(n00bb_tu_state_t *state)
{
    return module_scopes_in_namespace(state, CORE_STRING_STATIC(""));
}

core_translation_unit_t *
find_direct_import(core_translation_unit_t *unit, core_string_t module)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_translation_unit_t *alias = state
        ? core_dict_get(&state->import_alias_index, &module, nullptr)
        : nullptr;
    return alias ? alias : core_translation_unit_find_import(unit, module);
}

n00bb_package_declaration_t *
find_package_declaration(core_translation_unit_t *unit, core_string_t name)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (size_t i = 0; state && i < state->package_declarations.len; i++)
        if (core_string_eq(state->package_declarations.data[i].name, name))
            return &state->package_declarations.data[i];
    return nullptr;
}

core_diag_location_t
link_source_location(parse_tree_t *node)
{
    parse_token_info_t *token = node
        ? parse_tree_find_first_terminal(node) : nullptr;
    return token ? (core_diag_location_t){
        .line = parse_token_line(token),
        .column = parse_token_column(token),
        .end_line = parse_token_endline(token),
        .end_column = parse_token_endcol(token),
        .has_location = true,
    } : (core_diag_location_t){0};
}

parse_sym_entry_t *
canonical_symbol(parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *origin = parse_sym_binding_origin(symbol);
    while (origin) {
        symbol = origin;
        origin = parse_sym_binding_origin(symbol);
    }
    return symbol;
}

parse_sym_entry_t *
n00bb_scope_named_next(parse_scope_t *scope, core_string_t name,
                       parse_sym_entry_t *previous)
{
    parse_sym_entry_t *next = previous ? previous->shadowed
        : scope ? parse_symtab_lookup_in(scope, name) : nullptr;
    return next && next->scope == scope && core_string_eq(next->name, name)
        ? next : nullptr;
}

bool
n00bb_same_declaration_listed(const core_list_t(parse_sym_entry_ptr_t) *list,
                              parse_sym_entry_t *symbol)
{
    parse_sym_entry_t *origin = canonical_symbol(symbol);
    for (size_t i = 0; i < list->len; i++)
        if (canonical_symbol(list->data[i]) == origin) return true;
    return false;
}

bool
symbol_is_public_declaration(parse_sym_entry_t *symbol)
{
    if (!symbol) return false;
    if (core_string_eq(symbol->visibility, CORE_STRING_STATIC("public")))
        return true;
    if (symbol->visibility.u8_bytes) return false;
    return symbol->kind == PARSE_SYM_FUNCTION
        || symbol->kind == PARSE_SYM_TYPEDEF
        || symbol->kind == PARSE_SYM_TAG;
}

th_env_visibility_t
symbol_visibility(parse_sym_entry_t *symbol)
{
    if (!symbol) return TH_ENV_PRIVATE;
    if (parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.external-export"))
        || core_string_eq(symbol->visibility,
            CORE_STRING_STATIC("public")))
        return TH_ENV_PUBLIC;
    if (symbol_is_public_declaration(symbol))
        return TH_ENV_PUBLIC;
    return TH_ENV_PRIVATE;
}

static bool
append_dotted_name(parse_grammar_t *grammar, parse_tree_t *node,
                   core_buffer_t *name)
{
    if (!node) return false;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        if (!token || !core_string_eq(parse_terminal_name(grammar, parse_token_tid(token)),
                                      CORE_STRING_STATIC("IDENTIFIER")))
            return true;
        if (name->byte_len) core_buffer_putc(name, '.');
        core_string_t text = parse_tree_leaf_text(node);
        core_buffer_append(name, text.data, text.u8_bytes);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)
            && parse_tree_leaf_is(child, CORE_STRING_STATIC(".")))
            continue;
        if (!append_dotted_name(grammar, child, name)) return false;
    }
    return true;
}

static core_string_t
package_node_name(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                  parse_tree_t *node)
{
    parse_tree_t *dotted = CHILD_NT(grammar, node, "dotted-name");
    core_buffer_t name = {0};
    if (!dotted || !append_dotted_name(grammar, dotted, &name)
        || !name.byte_len) {
        core_buffer_release(&name);
        return (core_string_t){0};
    }
    char *stored = core_arena_alloc(&state->semantic_values,
                                    name.byte_len + 1);
    if (!stored) {
        core_buffer_release(&name);
        return (core_string_t){0};
    }
    memcpy(stored, name.data, name.byte_len);
    stored[name.byte_len] = '\0';
    core_string_t result = core_string_borrow(stored, name.byte_len);
    core_buffer_release(&name);
    return result;
}

core_string_t
store_semantic_string(n00bb_tu_state_t *state, core_string_t text)
{
    char *stored = state && text.data
        ? core_arena_alloc(&state->semantic_values, text.u8_bytes + 1)
        : nullptr;
    if (!stored) return (core_string_t){0};
    memcpy(stored, text.data, text.u8_bytes);
    stored[text.u8_bytes] = '\0';
    return core_string_borrow(stored, text.u8_bytes);
}

bool
cache_package_export_syntax(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *package_node = nullptr;
    for (parse_tree_t *parent = parse_tree_parent(node);
         parent; parent = parse_tree_parent(parent))
        if (NT_IS(grammar, parent, "package-decl")) {
            package_node = parent;
            break;
        }
    n00bb_package_declaration_t *package = nullptr;
    for (size_t i = 0; state && package_node
            && i < state->package_declarations.len; i++)
        if (state->package_declarations.data[i].node == package_node) {
            package = &state->package_declarations.data[i];
            break;
        }
    parse_tree_t *dotted = CHILD_NT(grammar, node, "dotted-name");
    core_buffer_t source = {0};
    if (!package || !dotted
        || !append_dotted_name(grammar, dotted, &source)
        || !source.byte_len) {
        core_buffer_release(&source);
        return false;
    }
    core_string_t source_name = core_string_borrow(source.data,
                                                    source.byte_len);
    parse_tree_t *alias = CHILD_NT(grammar, node, "export-alias");
    core_string_t export_name = {0};
    if (alias && core_tree_num_children(alias) >= 2)
        export_name = first_token_text(core_tree_child(alias, 1));
    if (!export_name.data) {
        size_t start = source.byte_len;
        while (start && source.data[start - 1] != '.') start--;
        export_name = core_string_borrow(source.data + start,
                                         source.byte_len - start);
    }
    core_string_t stored_source = store_semantic_string(state, source_name);
    core_string_t stored_export = store_semantic_string(state, export_name);
    bool subpackage = false;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)
            && parse_tree_leaf_is(child, CORE_STRING_STATIC("package"))) {
            subpackage = true;
            break;
        }
    }
    core_buffer_release(&source);
    if (!stored_source.data || !stored_export.data) return false;
    for (size_t i = 0; i < package->exports.len; i++)
        if (package->exports.data[i].node == node) return true;
    parse_tree_t *grantee = CHILD_NT(grammar, node, "export-grantee");
    core_buffer_t grantee_name = {0};
    if (grantee && !append_dotted_name(grammar,
            CHILD_NT(grammar, grantee, "dotted-name"), &grantee_name)) {
        core_buffer_release(&grantee_name);
        return false;
    }
    core_string_t stored_grantee = grantee_name.byte_len
        ? store_semantic_string(state, core_string_borrow(grantee_name.data,
                                                          grantee_name.byte_len))
        : (core_string_t){0};
    core_buffer_release(&grantee_name);
    if (grantee && !stored_grantee.data) return false;
    core_list_push(package->exports, ((n00bb_package_export_t){
        .source_name = stored_source,
        .export_name = stored_export,
        .grantee = stored_grantee,
        .node = node,
        .renamed = alias != nullptr,
        .subpackage = subpackage,
    }));
    return true;
}

bool
cache_package_declaration_syntax(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !node || !NT_IS(grammar, node, "package-decl"))
        return false;
    core_string_t name = package_node_name(grammar, state, node);
    if (!name.data) return false;
    for (size_t i = 0; i < state->package_declarations.len; i++) {
        if (!core_string_eq(state->package_declarations.data[i].name,
                            name)) continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "package interface '%.*s' is declared more than once",
            (int)name.u8_bytes, name.data);
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-M0001"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(node));
        core_buffer_release(&message);
        return false;
    }
    core_list_push(state->package_declarations,
        ((n00bb_package_declaration_t){
            .name = name,
            .node = node,
            .exports = core_list_new(n00bb_package_export_t),
        }));
    return true;
}

bool
package_tree_may_declare(parse_grammar_t *grammar, parse_tree_t *tree,
                         core_string_t name)
{
    if (!tree || core_tree_is_leaf(tree)) return false;
    if (NT_IS(grammar, tree, "package-decl")) {
        parse_tree_t *dotted = CHILD_NT(grammar, tree, "dotted-name");
        core_buffer_t declared = {0};
        bool matches = dotted && append_dotted_name(grammar, dotted,
                                                     &declared)
            && core_string_eq(core_string_borrow(declared.data,
                                                  declared.byte_len), name);
        core_buffer_release(&declared);
        return matches;
    }
    for (size_t i = 0; i < core_tree_num_children(tree); i++)
        if (package_tree_may_declare(grammar, core_tree_child(tree, i),
                                     name)) return true;
    return false;
}

bool
package_tree_syntax_index(parse_grammar_t *grammar, parse_tree_t *tree,
                          core_buffer_t *names)
{
    if (!tree || core_tree_is_leaf(tree)) return true;
    if (NT_IS(grammar, tree, "package-decl")) {
        parse_tree_t *dotted = CHILD_NT(grammar, tree, "dotted-name");
        core_buffer_t name = {0};
        bool ok = dotted && append_dotted_name(grammar, dotted, &name)
            && name.byte_len;
        if (ok) core_buffer_append(names, name.data, name.byte_len);
        core_buffer_release(&name);
        if (!ok) return false;
        core_buffer_putc(names, '\n');
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(tree); i++)
        if (!package_tree_syntax_index(grammar, core_tree_child(tree, i),
                                       names)) return false;
    return true;
}

bool
n00bb_tu_remember_selected_when_body(void *opaque,
    core_translation_unit_t *unit, parse_tree_t *body)
{
    (void)opaque;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !body) return false;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, body);
    if (!facts) return false;
    facts->selected_when_body = true;
    return true;
}

bool
n00bb_tu_remember_build_time_use(void *opaque,
    core_translation_unit_t *unit, parse_tree_t *node)
{
    (void)opaque;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !node) return false;
    if (!state->build_time_use.has_location)
        state->build_time_use = link_source_location(node);
    return true;
}

void
propagate_tree_type_to_wrappers(parse_tree_t *node, th_ctx *types,
                                th_type *previous, th_type *type)
{
    for (parse_tree_t *parent = node
             ? parse_tree_parent(node) : nullptr;
         parent; parent = parse_tree_parent(parent)) {
        th_type *parent_type = parse_tree_type(parent);
        if (!parent_type || !previous
            || (th_resolve(parent_type) != th_resolve(previous)
                && (!th_is_closed(types, parent_type)
                    || !th_is_closed(types, previous)
                    || th_hash(types, parent_type) != th_hash(types, previous))))
            break;
        parse_tree_t *typed_child = nullptr;
        for (size_t i = 0; i < core_tree_num_children(parent); i++) {
            parse_tree_t *child = core_tree_child(parent, i);
            if (!core_tree_is_leaf(child)
                && parse_tree_type(child)) {
                if (typed_child) { typed_child = nullptr; break; }
                typed_child = child;
            }
        }
        if (!typed_child || typed_child != node)
            break;
        parse_tree_set_type(parent, type);
        node = parent;
    }
}

void
propagate_symbol_type(parse_sym_entry_t *symbol, th_ctx *types,
                      th_type *previous, th_type *type)
{
    if (!symbol || !types || !type) return;
    for (size_t i = 0; previous && i < symbol->use_count; i++) {
        parse_tree_t *use = symbol->use_nodes[i];
        if (!use || core_tree_is_leaf(use)) continue;
        parse_node_resolution_t resolution = parse_tree_resolution(use);
        if (resolution.kind != PARSE_NODE_RESOLUTION_SYMBOL
            || resolution.target.symbol != symbol) continue;
        th_type *use_type = parse_tree_type(use);
        if (use_type && (th_resolve(use_type) == th_resolve(previous)
            || (th_is_closed(types, use_type) && th_is_closed(types, previous)
                && th_hash(types, use_type) == th_hash(types, previous)))) {
            parse_tree_set_type(use, type);
            propagate_tree_type_to_wrappers(use, types, previous, type);
        }
    }
    symbol->type = type;
}

void
finalize_module_inference(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->inference_finalized
        || !state->semantic_annotation_complete
        || !state->annotations || !state->types) return;

    parse_annot_resolve_undefined(state->annotations);
    state->inference_finalized = true;
}

typedef struct {
    parse_sym_entry_t *type;
    parse_sym_entry_t *value;
} n00bb_type_value_collision_t;

static bool
find_type_value_collision(parse_sym_entry_t *symbol, void *context)
{
    n00bb_type_value_collision_t *collision = context;
    if (symbol != collision->type && symbol->scope && symbol->scope->ns
        && core_string_eq(symbol->scope->ns->ns_name,
                          CORE_STRING_STATIC(""))) {
        collision->value = symbol;
        return false;
    }
    return true;
}

bool
validate_type_value_namespaces(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return false;
    parse_namespace_t *types = parse_symtab_ns(
        symtab, CORE_STRING_STATIC("type"));
    if (!types) return false;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (scope->ns != types) continue;
        for (parse_sym_entry_t *type = scope->first_in_scope; type;
             type = type->next_in_scope) {
            if (type->kind != PARSE_SYM_TYPEDEF) continue;
            n00bb_type_value_collision_t collision = {.type = type};
            parse_symtab_visit_name_across_namespaces(
                symtab, scope, type->name, find_type_value_collision,
                &collision);
            if (!collision.value) continue;
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "type name '%.*s' conflicts with a value name in the same scope",
                (int)type->name.u8_bytes, type->name.data);
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0006"),
                core_buffer_string(&message),
                link_source_location(type->decl_node));
            core_buffer_release(&message);
            return false;
        }
    }
    return true;
}

th_type *
n00bb_literal_site_type(th_ctx *types)
{
    return th_name(types,
                   CORE_STRING_STATIC("builtin.litmod_site.Literal_Site"));
}

bool
n00bb_litmod_takes_site(th_ctx *types, th_type *signature)
{
    signature = signature ? th_resolve(signature) : nullptr;
    const th_param *site = signature
        && th_kind_of(signature) == TH_FUNCTION
        && th_func_nparams(signature) == 2
        ? th_func_param(signature, 1) : nullptr;
    return site && th_mode_is_in(site->mode)
        && th_hash(types, th_resolve(site->type))
            == th_hash(types, n00bb_literal_site_type(types));
}

bool
validate_user_litmod_signatures(parse_grammar_t *grammar,
                                core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->literal_modifiers.len) return true;
    bool valid = true;
    for (size_t i = 0; i < state->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *registration =
            &state->literal_modifiers.data[i];
        if (!registration->declaration && registration->function) continue;
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            n00bb_user_litmod_t *prior = &state->literal_modifiers.data[j];
            if (prior->declaration == registration->declaration) {
                registration->function = prior->function;
                seen = true;
                break;
            }
        }
        if (seen) continue;
        parse_sym_entry_t *symbol = registration->declaration
            ? parse_tree_sym(registration->declaration) : nullptr;
        th_type *signature = symbol && symbol->type
            ? th_resolve(symbol->type) : nullptr;
        parse_tree_t *declaration = symbol ? symbol->decl_node : nullptr;
        parse_tree_t *syntax = CHILD_NT(grammar, declaration,
                                        "func-type-signature");
        parse_tree_t *result_syntax = CHILD_NT(grammar, syntax,
                                               "function-result");
        n00bb_function_parameter_facts_t *facts = symbol
            ? parse_sym_binding(symbol,
                N00BB_PARAMETER_FACTS_KEY)
            : nullptr;
        bool no_payload = core_string_eq(registration->kind,
                                         CORE_STRING_STATIC("nil"));
        bool takes_site = core_string_eq(registration->kind,
                                         CORE_STRING_STATIC("fence"))
            && n00bb_litmod_takes_site(state->types, signature);
        size_t arity = no_payload ? 0 : takes_site ? 2 : 1;
        bool shape_valid = symbol && symbol->kind == PARSE_SYM_FUNCTION
            && declaration && CHILD_NT(grammar, declaration,
                                         "litmod-prelude")
            && CHILD_NT(grammar, result_syntax, "return-type")
            && CHILD_NT(grammar, result_syntax, "error-type")
            && facts && !facts->has_inferred_parameter
            && facts->param_count == arity
            && (no_payload || (!facts->params[0].variadic
                && facts->params[0].node
                && parse_tree_sym(facts->params[0].node)
                && !parse_tree_sym(facts->params[0].node)->is_optional))
            && signature && th_kind_of(signature) == TH_FUNCTION
            && th_func_nparams(signature) == arity
            && !th_func_rest(signature) && !th_func_nkw(signature);
        const th_param *param = shape_valid && !no_payload
            ? th_func_param(signature, 0) : nullptr;
        th_type *payload = param ? th_resolve(param->type) : nullptr;
        shape_valid = shape_valid
            && (no_payload
                || (param && th_mode_is_in(param->mode) && payload))
            && th_func_ret(signature)
            && th_func_error(signature);
        bool raw = core_string_eq(registration->kind,
                              CORE_STRING_STATIC("quotes"))
            || core_string_eq(registration->kind,
                              CORE_STRING_STATIC("int"))
            || core_string_eq(registration->kind,
                              CORE_STRING_STATIC("hex"))
            || core_string_eq(registration->kind,
                              CORE_STRING_STATIC("float"))
            || core_string_eq(registration->kind,
                              CORE_STRING_STATIC("fence"));
        bool boolean = core_string_eq(registration->kind,
                                      CORE_STRING_STATIC("bool"));
        bool dictionary = core_string_eq(registration->kind,
                                         CORE_STRING_STATIC("dict"))
            || core_string_eq(registration->kind,
                              CORE_STRING_STATIC("record"));
        if (shape_valid && raw) {
            shape_valid = th_hash(state->types, payload)
                == th_hash(state->types, th_name(state->types, CORE_STRING_STATIC("string")));
        }
        else if (shape_valid && boolean) {
            shape_valid = th_hash(state->types, payload)
                == th_hash(state->types, th_name(state->types, CORE_STRING_STATIC("bool")));
        }
        else if (shape_valid && !no_payload) {
            shape_valid = th_kind_of(payload) == TH_SEQUENCE;
            if (shape_valid && dictionary) {
                th_type *element = th_resolve(th_seq_elem(payload));
                shape_valid = th_builtin_applied(element,
                                                       TH_BUILTIN_TUPLE, 2)
                    != nullptr;
                if (shape_valid && core_string_eq(registration->kind,
                                    CORE_STRING_STATIC("record"))) {
                    const th_arg *first = th_named_arg(element, 0);
                    shape_valid = first && first->kind == TH_ARG_TYPE
                        && th_hash(state->types, th_resolve(first->type))
                            == th_hash(state->types,
                                th_name(state->types, CORE_STRING_STATIC("string")));
                }
            }
        }
        if (shape_valid) {
            th_type *failure = th_resolve(th_func_error(signature));
            th_type *result = th_resolve(th_func_ret(signature));
            shape_valid = th_hash(state->types, failure)
                == th_hash(state->types, th_name(state->types, CORE_STRING_STATIC("string")))
                && (core_string_eq(registration->kind,
                    CORE_STRING_STATIC("fence"))
                    || th_hash(state->types, result)
                        != th_hash(state->types,
                            th_name(state->types, CORE_STRING_STATIC("nil"))));
        }
        if (!shape_valid) {
            core_buffer_t *message = core_buffer_empty();
            core_buffer_printf(message,
                "literal modifier '%.*s' requires %s"
                "an explicit return type "
                "(non-nil except for fences), and fail: string",
                (int)registration->modifier.u8_bytes,
                registration->modifier.data,
                no_payload ? "no parameters, "
                           : "one explicitly typed shape-compatible input "
                             "(a fence may add site: Literal_Site), ");
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0029"),
                core_string_borrow(message->data, message->byte_len),
                registration->location);
            core_buffer_free(message);
            valid = false;
        }
        else if (!parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.litmod.declaration"),
            registration->declaration)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0029"),
                CORE_STRING_STATIC("could not retain the literal modifier declaration"),
                registration->location);
            valid = false;
        }
        else registration->function = symbol;
    }
    return valid;
}

uint64_t
n00bb_shift_receiver_width(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    uint64_t width = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    if (type && th_bitint_info(type, &width, &flavor))
        return flavor == TH_BITINT_SIGNED ? 0 : width;
    bool is_signed = false;
    return n00bb_integer_representation(type, &width, &is_signed)
        && !is_signed ? width : 0;
}

bool
n00bb_bitwise_type(th_type *type)
{
    return n00bb_shift_receiver_width(type) != 0;
}

bool
n00bb_unsigned_shift_count_type(th_type *type)
{
    uint64_t width = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    if (type && th_bitint_info(th_resolve(type), &width, &flavor))
        return flavor == TH_BITINT_UNSIGNED && width != 0;
    return n00bb_shift_receiver_width(type) != 0;
}

th_type *
n00bb_shift_count_type(th_ctx *types, th_type *receiver)
{
    uint64_t width = 0;
    bool is_signed = false;
    th_type *resolved = receiver ? th_resolve(receiver) : nullptr;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    if (!resolved || (!th_bitint_info(resolved, &width, &flavor)
            && !n00bb_integer_representation(resolved, &width, &is_signed)))
        return nullptr;
    if (!types || !width) return nullptr;
    uint64_t bits = 0;
    for (uint64_t maximum = width - 1; maximum; maximum >>= 1) bits++;
    return th_bitint(types, bits ? bits : 1, false);
}


static n00bb_function_parameter_facts_t *
function_parameter_facts(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *function = parse_tree_parent(node);
    while (function && !NT_IS(grammar, function, "func-def")
           && !NT_IS(grammar, function, "trait-spec")
           && !NT_IS(grammar, function, "callback-role")
           && !NT_IS(grammar, function, "context-def"))
        function = parse_tree_parent(function);
    parse_sym_entry_t *symbol = function
        ? parse_tree_sym(function) : nullptr;
    if (!symbol) return nullptr;
    return parse_sym_binding(symbol, N00BB_PARAMETER_FACTS_KEY);
}

bool
record_function_parameter_fact(parse_grammar_t *grammar,
                               parse_tree_t *node, n00bb_tu_state_t *state)
{
    bool variadic = NT_IS(grammar, node, "vargs-param");
    if (!variadic && !NT_IS(grammar, node, "pos-param")
        && !NT_IS(grammar, node, "default-param")) return true;
    n00bb_function_parameter_facts_t *facts =
        function_parameter_facts(grammar, node);
    parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
    if (!facts) return true;
    parse_tree_t *owner = parse_tree_parent(node);
    while (owner && !NT_IS(grammar, owner, "func-def")
           && !NT_IS(grammar, owner, "trait-spec")
           && !NT_IS(grammar, owner, "callback-role")
           && !NT_IS(grammar, owner, "context-def"))
        owner = parse_tree_parent(owner);
    if (owner && NT_IS(grammar, owner, "context-def")) {
        parse_tree_t *context_child = node;
        while (context_child
               && parse_tree_parent(context_child) != owner)
            context_child = parse_tree_parent(context_child);
        if (!NT_IS(grammar, context_child, "param-decl")) return true;
    }
    if (!core_arena_try_reserve_raw(&state->semantic_values,
            (void **)&facts->params, &facts->param_capacity,
            facts->param_count + 1, sizeof(*facts->params))) return false;
    facts->params[facts->param_count++] = (n00bb_function_parameter_ref_t){
        .node = node,
        .variadic = variadic,
        .has_type_node = type_node != nullptr,
    };
    facts->has_inferred_parameter = facts->has_inferred_parameter
        || type_node == nullptr;
    return true;
}


static void
probe_static_literal(parse_grammar_t *grammar, parse_tree_t *node,
                     n00bb_static_literal_probe_t *probe)
{
    static const core_string_t wrappers[] = {
        CORE_STRING_STATIC("expression"), CORE_STRING_STATIC("or-expr"),
        CORE_STRING_STATIC("and-expr"), CORE_STRING_STATIC("or-rhs"),
        CORE_STRING_STATIC("and-rhs"), CORE_STRING_STATIC("equality-expr"),
        CORE_STRING_STATIC("compare-expr"), CORE_STRING_STATIC("bitor-expr"),
        CORE_STRING_STATIC("bitxor-expr"), CORE_STRING_STATIC("bitand-expr"),
        CORE_STRING_STATIC("shift-expr"), CORE_STRING_STATIC("add-expr"),
        CORE_STRING_STATIC("mul-expr"), CORE_STRING_STATIC("unary-expr"),
        CORE_STRING_STATIC("postfix-expr"), CORE_STRING_STATIC("primary"),
    };
    while (node && probe->valid) {
        if (NT_IS(grammar, node, "simple-lit")
            || NT_IS(grammar, node, "list-lit")
            || NT_IS(grammar, node, "dict-or-set-lit")
            || NT_IS(grammar, node, "tuple-or-paren")) {
            probe->literal = node;
            probe->literal_count = 1;
            probe->valid = !n00bb_node_literal_site(node);
            return;
        }
        bool transparent = false;
        for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
            transparent = transparent || nt_is(grammar, node, wrappers[i]);
        if (!transparent) {
            probe->valid = false;
            return;
        }
        parse_tree_t *child = nullptr;
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *item = core_tree_child(node, i);
            if (core_tree_is_leaf(item)) {
                core_string_t text = parse_tree_leaf_text(item);
                if (core_string_eq(text, CORE_STRING_STATIC("-")))
                    probe->negative = true;
                else if (!core_string_eq(text, CORE_STRING_STATIC("+"))) {
                    probe->valid = false;
                    return;
                }
                continue;
            }
            if (child) {
                probe->valid = false;
                return;
            }
            child = item;
        }
        node = child;
    }
    probe->valid = false;
}

n00bb_static_literal_probe_t
find_static_literal(parse_grammar_t *grammar, parse_tree_t *node)
{
    n00bb_static_literal_probe_t probe = {
        .valid = node != nullptr,
    };
    probe_static_literal(grammar, node, &probe);
    if (probe.literal_count != 1) probe.valid = false;
    return probe;
}
parse_tree_t *
call_arguments_node(parse_grammar_t *grammar, parse_tree_t *call)
{
    const parse_rule_t *rule = parse_tree_rule(grammar, call);
    const parse_annotation_t *annotation = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL) : nullptr;
    if (annotation && parse_child_ref_is_set(annotation->value_ref)) {
        parse_tree_t *arguments = parse_tree_resolve_child_ref(
            grammar, call, annotation->value_ref);
        if (arguments) return arguments;
    }
    return CHILD_NT(grammar, call, "call-args");
}

void
collect_positional_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                         core_list_t(n00bb_tree_ptr_t) *arguments)
{
    if (!NT_IS(grammar, node, "positional-args")) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
    {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "expression")) {
            core_list_push(*arguments, child);
            continue;
        }
        if (!parse_tree_is_group(child)) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (NT_IS(grammar, item, "expression"))
                core_list_push(*arguments, item);
        }
    }
}

static void
cache_trait_group_parts(parse_tree_t *node,
                        core_list_t(core_string_t) *parts)
{
    if (!node || core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) {
            core_string_t text = parse_tree_leaf_text(child);
            if (text.data && text.u8_bytes
                && !core_string_eq(text, CORE_STRING_STATIC(".")))
                core_list_push(*parts, text);
            continue;
        }
        parse_nt_node_t *child_node = &core_tree_node_value(child);
        if (!child_node->group_top && !child_node->group_item) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (core_tree_is_leaf(item)) {
                core_string_t text = parse_tree_leaf_text(item);
                if (text.data && text.u8_bytes
                    && !core_string_eq(text, CORE_STRING_STATIC(".")))
                    core_list_push(*parts, text);
                continue;
            }
            if (!core_tree_node_value(item).group_item
                && !parse_tree_is_group(item)) continue;
            for (size_t k = 0; k < core_tree_num_children(item); k++) {
                parse_tree_t *leaf = core_tree_child(item, k);
                if (!core_tree_is_leaf(leaf)) continue;
                core_string_t text = parse_tree_leaf_text(leaf);
                if (text.data && text.u8_bytes
                    && !core_string_eq(text, CORE_STRING_STATIC(".")))
                    core_list_push(*parts, text);
            }
        }
    }
}

core_string_t *
cache_trait_name_parts(parse_tree_t *node, size_t *count)
{
    *count = 0;
    core_list_t(core_string_t) parts = core_list_new(core_string_t);
    cache_trait_group_parts(node, &parts);
    core_string_t *result = parts.len
        ? core_alloc_array(core_string_t, parts.len) : nullptr;
    if (result) {
        memcpy(result, parts.data, parts.len * sizeof(*result));
        *count = parts.len;
    }
    core_list_free(parts);
    return result;
}

parse_tree_t *
n00bb_init_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "postfix-expr")
        || core_tree_num_children(node) < 3
        || !parse_tree_leaf_is(core_tree_child(node, 1),
                               CORE_STRING_STATIC("("))) return nullptr;
    core_string_t name = {0};
    if (!simple_identifier_expression(grammar, core_tree_child(node, 0),
                                      &name)
        || !core_string_eq(name, CORE_STRING_STATIC("init"))) return nullptr;
    parse_tree_t *arguments = call_arguments_node(grammar, node);
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(grammar,
        CHILD_NT(grammar, arguments, "positional-args"), &positional);
    parse_tree_t *target = positional.len ? positional.data[0] : nullptr;
    core_list_free(positional);
    return target;
}
static bool
primitive_ordering_type(th_type *type)
{
    return is_integer_type(type) || n00bb_open_integer(type)
        || is_float_type(type);
}

bool
is_float_type(th_type *type)
{
    return n00bb_scalar_class_is(type, IR_CLASS_FLOAT);
}

bool
primitive_operator_site(n00bb_tu_state_t *state, n00bb_link_use_site_t *site)
{
    if (!site || !site->operator_dispatch
        || core_tree_num_children(site->node) == 0) return false;
    parse_tree_t *receiver = operator_receiver_node(site);
    if (core_tree_is_leaf(receiver)) return false;
    th_type *receiver_type = parse_tree_type(receiver);
    th_type *resolved_receiver = receiver_type
        ? th_resolve(receiver_type) : nullptr;
    if (resolved_receiver && th_kind_of(resolved_receiver) == TH_INDIRECTION
        && state && state->types && site->dispatch_method.data
        && !core_string_eq(site->dispatch_trait, CORE_STRING_STATIC("Equal"))
        && !th_method_lookup(state->types, receiver_type,
                             site->dispatch_method))
        receiver_type = th_ind_referent(resolved_receiver);
    if (core_string_eq(site->dispatch_trait,
                       CORE_STRING_STATIC("Not")))
        return n00bb_scalar_class_is(receiver_type, IR_CLASS_BOOL);
    if (core_string_eq(site->dispatch_trait,
                       CORE_STRING_STATIC("Negate")))
        return primitive_ordering_type(receiver_type);
    bool equality = core_string_eq(site->dispatch_trait,
                                   CORE_STRING_STATIC("Equal"));
    bool ordering = core_string_eq(site->dispatch_trait,
                                   CORE_STRING_STATIC("Comparable"));
    if (equality || ordering) {
        if (primitive_ordering_type(receiver_type)) return true;
        if (!equality) return false;
        if (receiver_type && th_ind_referent(th_resolve(receiver_type)))
            return true;
        if (state && state->link_analysis
            && n00bb_enum_type_symbol(state->link_analysis, receiver_type))
            return true;
        th_builtin_kind_t kind = th_builtin_kind_of(receiver_type);
        if (n00bb_scalar_class_is(receiver_type, IR_CLASS_BOOL)
            || kind == TH_BUILTIN_NIL || kind == TH_BUILTIN_TYPESPEC
            || n00bb_scalar_role_is(receiver_type, IR_ROLE_CHAR)
            || n00bb_scalar_role_is(receiver_type, IR_ROLE_BYTE))
            return true;
        parse_tree_t *other = core_tree_child(site->node, 2);
        return !core_tree_is_leaf(other)
            && th_builtin_kind_of(parse_tree_type(other))
                == TH_BUILTIN_NIL;
    }
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(site->dispatch_method);
    if (!operator) return false;
    if (operator->unsigned_only)
        return n00bb_bitwise_type(receiver_type)
            || n00bb_open_integer(receiver_type);
    return primitive_ordering_type(receiver_type);
}

bool
n00bb_flagset_type(core_translation_unit_t *unit, n00bb_tu_state_t *state,
                   th_type *type, parse_sym_entry_t **symbol_out)
{
    parse_sym_entry_t *symbol = unit && state && state->types && type
        ? type_symbol_in_loaded_imports(unit, state->types,
                                        th_resolve(type), nullptr)
        : nullptr;
    parse_scope_t *scope = symbol ? symbol->member_scope : nullptr;
    if (symbol_out) *symbol_out = symbol;
    return scope && core_string_eq(scope->scope_tag,
                                   CORE_STRING_STATIC("flagset"));
}

bool
n00bb_flagset_operator_site(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            n00bb_tu_state_t *state,
                            parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    bool binary = binary_operator_dispatch_for_node(grammar, node,
                                                     &trait, &method);
    bool unary = !binary && unary_operator_dispatch_for_node(grammar, node,
                                                             &trait, &method);
    if (!binary && !unary) return false;
    parse_tree_t *receiver = core_tree_child(node, unary ? 1 : 0);
    th_type *type = receiver && !core_tree_is_leaf(receiver)
        ? parse_tree_type(receiver) : nullptr;
    parse_sym_entry_t *receiver_symbol = attached_symbol(grammar, unit,
                                                          receiver);
    parse_tree_t *inner = receiver;
    while (inner && !core_tree_is_leaf(inner)
           && core_tree_num_children(inner) == 1)
        inner = core_tree_child(inner, 0);
    bool receiver_is_flagset = n00bb_flagset_member(receiver_symbol)
        || (receiver_symbol && n00bb_flagset_type(unit, state,
                                                  receiver_symbol->type,
                                                  nullptr))
        || n00bb_flagset_type(unit, state, type, nullptr)
        || (inner && inner != node && !core_tree_is_leaf(inner)
            && core_tree_num_children(inner) > 1
            && n00bb_flagset_operator_site(grammar, unit, state, inner));
    if (!receiver_is_flagset) return false;
    return core_string_eq(trait, CORE_STRING_STATIC("Add"))
        || core_string_eq(trait, CORE_STRING_STATIC("Subtract"))
        || core_string_eq(trait, CORE_STRING_STATIC("Bit_And"))
        || core_string_eq(trait, CORE_STRING_STATIC("Bit_Or"))
        || core_string_eq(trait, CORE_STRING_STATIC("Bit_Xor"))
        || core_string_eq(trait, CORE_STRING_STATIC("Equal"))
        || core_string_eq(trait, CORE_STRING_STATIC("Not"));
}

bool
refine_flagset_operator_result(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state,
                               parse_tree_t *node)
{
    if (NT_IS(grammar, node, "unary-expr")
        && core_tree_num_children(node) == 2
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("~"))) {
        th_type *operand = parse_tree_type(core_tree_child(node, 1));
        th_type *result = parse_tree_type(node);
        if (operand && result
            && (is_integer_type(operand)
                || th_kind_of(th_resolve(operand)) == TH_VAR
                || n00bb_flagset_type(unit, state, operand, nullptr))
            && th_unify_in_place(state->types, operand, result)) return true;
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
            CORE_STRING_STATIC("~ requires a flagset or integer operand"),
            link_source_location(node));
        return false;
    }
    if (!n00bb_flagset_operator_site(grammar, unit, state, node)) return true;
    bool unary = core_tree_num_children(node) == 2;
    parse_tree_t *left = core_tree_child(node, unary ? 1 : 0);
    th_type *left_type = parse_tree_type(left);
    th_type *right_type = unary ? nullptr
        : parse_tree_type(core_tree_child(node, 2));
    core_string_t spelling = first_token_text(
        core_tree_child(node, unary ? 0 : 1));
    bool boolean = core_string_eq(n00bb_canonical_logical_spelling(spelling),
                                  CORE_STRING_STATIC("!"))
        || core_string_eq(spelling, CORE_STRING_STATIC("=="))
        || core_string_eq(spelling, CORE_STRING_STATIC("!="));
    th_type *expected = boolean ? th_name(state->types, CORE_STRING_STATIC("bool")) : left_type;
    th_type *result = parse_tree_type(node);
    if (!result || th_is_bottom(result)) {
        parse_tree_set_type(node, expected);
        result = expected;
    }
    bool valid = left_type && expected && result
        && (unary || (right_type
            && th_unify_in_place(state->types, left_type, right_type)))
        && th_unify_in_place(state->types, result, expected);
    if (!valid) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
            CORE_STRING_STATIC(
                "flagset operator operands and result have incompatible types"),
            link_source_location(node));
        return false;
    }
    if (core_string_eq(spelling, CORE_STRING_STATIC("|")))
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-FLAGSET-OR"),
            CORE_STRING_STATIC(
                "use + for flagset union; | is accepted for migration"),
            link_source_location(node));
    return true;
}

static bool
option_compared_with_value(core_translation_unit_t *unit,
                           n00bb_tu_state_t *state, parse_tree_t *node,
                           parse_tree_t *left, th_type *left_type,
                           th_type *right_type)
{
    th_type *left_resolved = left_type ? th_resolve(left_type) : nullptr;
    th_type *right_resolved = right_type ? th_resolve(right_type) : nullptr;
    if (!left_resolved || !right_resolved
        || th_is_nil(left_resolved) || th_is_nil(right_resolved))
        return false;
    bool left_option = th_option_item(left_resolved) != nullptr;
    bool right_option = th_option_item(right_resolved) != nullptr;
    if (!left_option && !right_option) return false;
    parse_tree_t *right = core_tree_child(node, 2);
    bool name_left = left_option
        && !(right_option && parse_tree_integer_literal_origin(left));
    parse_tree_t *option = name_left ? left : right;
    core_string_t text = n00bb_node_source_text(unit, option,
        CORE_STRING_STATIC("this operand"));
    core_string_t shown = n00bb_print_type(state->types,
        name_left ? left_resolved : right_resolved);
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "'%.*s' is %.*s, and an option can only be compared with nil; "
        "check it for nil, or take its value first",
        (int)text.u8_bytes, text.data, (int)shown.u8_bytes, shown.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
        unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0045"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(node));
    core_buffer_release(&message);
    return true;
}

bool
refine_intrinsic_binary_result(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    if (!binary_operator_dispatch_for_node(grammar, node,
                                           &trait, &method)
        || core_dict_contains(&state->integer_edge_sites, node)) return true;
    const n00bb_binary_operator_t *compound =
        NT_IS(grammar, node, "binop-assign-stmt")
        ? n00bb_binary_operator_method(method) : nullptr;
    th_type *compound_target = compound
        ? parse_tree_type(core_tree_child(node, 0)) : nullptr;
    if (compound && !compound->shift
        && (is_integer_type(compound_target)
            || n00bb_open_integer(compound_target))) return true;
    n00bb_link_use_site_t site = {
        .node = node,
        .dispatch_trait = trait,
        .dispatch_method = method,
        .operator_dispatch = true,
    };
    bool comparison = core_string_eq(trait, CORE_STRING_STATIC("Equal"))
        || core_string_eq(trait, CORE_STRING_STATIC("Comparable"));
    parse_tree_t *left = core_tree_child(node, 0);
    parse_tree_t *right = core_tree_child(node, 2);
    th_type *left_type = parse_tree_type(left);
    th_type *right_type = parse_tree_type(right);
    if (comparison && option_compared_with_value(unit, state, node, left,
                                                 left_type, right_type))
        return false;
    if (!primitive_operator_site(state, &site)) return true;
    th_type *result = parse_tree_type(node);
    if (comparison && !is_integer_type(left_type)
        && !n00bb_open_integer(left_type)
        && !is_integer_type(right_type) && !n00bb_open_integer(right_type)
        && !(left_type && th_ind_referent(th_resolve(left_type)))
        && !(right_type && th_ind_referent(th_resolve(right_type))))
        return true;
    th_type *declared_left = left_type;
    left_type = n00bb_operand_value_type(left_type, right_type, comparison);
    right_type = n00bb_operand_value_type(right_type, declared_left,
                                          comparison);
    if (!comparison && result && declared_left != left_type
        && th_resolve(result) == th_resolve(declared_left)) {
        result = left_type;
        parse_tree_set_type(node, result);
    }
    if (comparison && (!result || th_is_bottom(result))) {
        result = th_name(state->types, CORE_STRING_STATIC("bool"));
        parse_tree_set_type(node, result);
    }
    if (!comparison && (!result || th_is_bottom(result))) {
        parse_tree_set_type(node, left_type);
        result = left_type;
    }
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(method);
    bool shift = operator && operator->shift;
    if (operator && (operator->unsigned_only || shift)
        && n00bb_open_integer(left_type)
        && !n00bb_require_unsigned_integer(state, left_type))
        return false;
    if (shift && !n00bb_bitwise_type(left_type)
        && !n00bb_open_integer(left_type)) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
            CORE_STRING_STATIC(
                "built-in shifts require an unsigned or bit left operand"),
            link_source_location(node));
        return false;
    }
    if (shift) {
        ir_interval_t count = {0};
        uint64_t width = n00bb_shift_receiver_width(left_type);
        parse_tree_t *literal_origin =
            parse_tree_integer_literal_origin(right);
        th_type *count_type = literal_origin && right_type
            && th_kind_of(th_resolve(right_type)) == TH_VAR
            ? n00bb_shift_count_type(state->types, left_type) : nullptr;
        if (count_type
            && n00bb_integer_literal_joins_type(state, right, count_type))
            right_type = parse_tree_type(right);
        bool known_range = n00bb_current_integer_range(right, &count);
        if (!known_range && literal_origin)
            known_range = n00bb_current_integer_range(literal_origin,
                                                       &count);
        if (n00bb_open_integer(left_type)
            && core_dict_add(&state->integer_shift_seen, node, 1))
            core_list_push(state->integer_shift_sites, node);
        if (known_range
            && (count.lo < 0
                || (count.lo >= (ir_int_t)width
                    && !n00bb_open_integer(left_type)))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
                CORE_STRING_STATIC("shift count is outside the receiver width"),
                link_source_location(right));
            return false;
        }
        if (n00bb_open_integer(right_type)
            && !n00bb_require_unsigned_integer(state, right_type))
            return false;
        if (!right_type || (!n00bb_unsigned_shift_count_type(right_type)
                            && !n00bb_open_integer(right_type))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
                CORE_STRING_STATIC(
                    "built-in shift count must be unsigned"),
                link_source_location(node));
            return false;
        }
        if (n00bb_open_integer(left_type)) {
            th_type *widened = n00bb_integer_result_variable(state, node,
                operator->op, left, right);
            if (!widened) return false;
            parse_tree_set_type(node, widened);
            return true;
        }
    }
    bool left_open = n00bb_open_integer(left_type);
    bool right_open = n00bb_open_integer(right_type);
    bool left_pending = left_type && !left_open
        && th_kind_of(th_resolve(left_type)) == TH_VAR;
    bool right_pending = right_type && !right_open
        && th_kind_of(th_resolve(right_type)) == TH_VAR;
    if (!comparison && !shift && operator && (left_open || right_open)
        && (left_open || left_pending) && (right_open || right_pending)) {
        th_type *existing = result ? th_resolve(result) : nullptr;
        if (n00bb_open_integer(existing)
            && existing != th_resolve(left_type)
            && existing != th_resolve(right_type)) return true;
        if (operator->unsigned_only && right_open
            && !n00bb_require_unsigned_integer(state, right_type))
            return false;
        th_type *widened = n00bb_integer_result_variable(state, node,
            operator->op, left, right);
        if (!widened) return false;
        if (existing && th_kind_of(existing) == TH_VAR
            && existing != th_resolve(left_type)
            && existing != th_resolve(right_type)
            && !th_unify_in_place(state->types, widened, existing))
            return false;
        parse_tree_set_type(node, widened);
        return true;
    }
    if (!comparison && !shift && operator && (left_open || right_open)
        && is_integer_type(left_type) != is_integer_type(right_type)) {
        th_type *declared = is_integer_type(left_type) ? left_type
                                                        : right_type;
        th_type *current = result ? th_resolve(result) : nullptr;
        if (current && th_kind_of(current) == TH_VAR
            && !n00bb_open_integer(current)
            && !th_unify_in_place(state->types, current, declared))
            return false;
        parse_tree_set_type(node, declared);
        n00bb_integer_node_result(state, node, operator->op, left, right);
        return true;
    }
    if (!comparison && !shift && is_integer_type(left_type)
        && is_integer_type(right_type)
        && th_resolve(left_type) != th_resolve(right_type)) {
        th_type *wider = n00bb_common_integer_type(state->types, left_type,
                                                   right_type);
        th_type *current = result ? th_resolve(result) : nullptr;
        if (current && is_integer_type(current)
            && ((wider && n00bb_integer_type_promotes_to(wider, current))
                || th_hash(state->types, current)
                    == th_hash(state->types, right_type))) {
            if (operator)
                n00bb_integer_node_result(state, node, operator->op, left,
                                          right);
            return true;
        }
        if (wider && (!current || current == th_resolve(left_type)
                      || th_kind_of(current) == TH_VAR)) {
            if (current && th_kind_of(current) == TH_VAR
                && !th_unify_in_place(state->types, current, wider))
                return false;
            parse_tree_set_type(node, wider);
            n00bb_integer_node_result(state, node, operator->op, left, right);
            return true;
        }
    }
    bool left_integer = is_integer_type(left_type)
        || n00bb_open_integer(left_type);
    bool right_integer = is_integer_type(right_type)
        || n00bb_open_integer(right_type);
    if (comparison && left_integer && right_integer
        && th_resolve(left_type) != th_resolve(right_type))
        return th_unify_in_place(state->types, result,
            th_name(state->types, CORE_STRING_STATIC("bool"))) != nullptr;
    if (!shift && left_type && right_type
        && left_type != right_type) {
        if (n00bb_integer_literal_joins_type(state, right, left_type)
            || n00bb_float_literal_adopts(right, left_type)) {
            right_type = parse_tree_type(right);
        }
        else {
            th_type *previous_left = left_type;
            if (n00bb_integer_literal_joins_type(state, left, right_type)
                || n00bb_float_literal_adopts(left, right_type)) {
                left_type = parse_tree_type(left);
                if (result == previous_left) {
                    result = left_type;
                    parse_tree_set_type(node, result);
                }
            }
        }
    }
    th_type *operand_type = left_type;
    bool compatible_operands = shift || (left_type && right_type
        && th_unify_probe(state->types, left_type, right_type)
        && th_unify_in_place(state->types, left_type, right_type));
    if (!compatible_operands && left_type && right_type) {
        if (n00bb_practical_unsigned_to_signed(grammar, state, right,
                                               right_type, left_type))
            compatible_operands = true;
        else if (n00bb_practical_unsigned_to_signed(grammar, state, left,
                                                    left_type, right_type)) {
            operand_type = right_type;
            compatible_operands = true;
        }
    }
    if (compatible_operands && !comparison && result == left_type
        && operand_type != left_type) {
        result = operand_type;
        parse_tree_set_type(node, result);
    }
    bool valid = left_type && right_type && result
        && compatible_operands
        && th_unify_in_place(state->types, result,
            comparison ? th_name(state->types, CORE_STRING_STATIC("bool")) : operand_type);
    if (valid && !comparison && operator && is_integer_type(operand_type))
        n00bb_integer_node_result(state, node, operator->op, left, right);
    if (valid) return true;
    core_buffer_t message = {0};
    core_string_t left_text = left_type
        ? th_print(state->types, left_type) : CORE_STRING_STATIC("<missing>");
    core_string_t right_text = right_type
        ? th_print(state->types, right_type) : CORE_STRING_STATIC("<missing>");
    core_string_t result_text = result
        ? th_print(state->types, result) : CORE_STRING_STATIC("<missing>");
    core_string_t op_text = core_tree_num_children(node) == 3
        ? first_token_text(core_tree_child(node, 1))
        : CORE_STRING_STATIC("?");
    th_type *produced = comparison
        ? th_name(state->types, CORE_STRING_STATIC("bool")) : operand_type;
    core_string_t produced_text = produced
        ? th_print(state->types, produced) : CORE_STRING_STATIC("<missing>");
    if (!compatible_operands)
        core_buffer_printf(&message,
            "operator '%.*s' needs operands of the same type; got %.*s and %.*s",
            (int)op_text.u8_bytes, op_text.data,
            (int)left_text.u8_bytes, left_text.data,
            (int)right_text.u8_bytes, right_text.data);
    else
        core_buffer_printf(&message,
            "operator '%.*s' produces %.*s, which does not match its use as %.*s",
            (int)op_text.u8_bytes, op_text.data,
            (int)produced_text.u8_bytes, produced_text.data,
            (int)result_text.u8_bytes, result_text.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
        unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0045"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(node));
    core_buffer_release(&message);
    return false;
}

bool
record_generic_operator_failure(parse_grammar_t *grammar,
                                n00bb_tu_state_t *state, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    bool binary = binary_operator_dispatch_for_node(grammar, node,
                                                    &trait, &method);
    if (!binary && !unary_operator_dispatch_for_node(grammar, node,
                                                     &trait, &method))
        return true;
    if (core_string_eq(trait, CORE_STRING_STATIC("Equal"))
        || core_string_eq(trait, CORE_STRING_STATIC("Comparable")))
        return true;
    parse_sym_entry_t *function = nullptr;
    for (parse_tree_t *at = parse_tree_parent(node); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "try-stmt")
            || NT_IS(grammar, at, "try-value-expr")) return true;
        if (NT_IS(grammar, at, "func-def")) {
            function = parse_tree_sym(at);
            break;
        }
    }
    th_type *body = function ? parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.body-function-signature")) : nullptr;
    th_type *failure = body ? th_func_error(body) : nullptr;
    if (!failure || th_kind_of(th_resolve(failure)) != TH_VAR) return true;
    parse_tree_t *receiver_node = core_tree_child(node, binary ? 0 : 1);
    th_type *receiver = parse_tree_type(receiver_node);
    if (!receiver || th_kind_of(th_resolve(receiver)) != TH_VAR) return true;
    th_type *result = parse_tree_type(node);
    return th_method_obligation_set_error(state->types, receiver,
                                          method, result, failure);
}

static bool
constrain_primitive_numeric(n00bb_tu_state_t *state, th_type *type)
{
    const ir_profile_t *profile = n00bb_type_profile();
    type = type ? th_resolve(type) : nullptr;
    if (!type) return false;
    if (th_kind_of(type) != TH_VAR)
        return is_integer_type(type) || is_float_type(type);
    size_t capacity = profile->n_scalars + 1;
    th_type **alternatives = core_arena_alloc(&state->semantic_values,
        capacity * sizeof(*alternatives));
    if (!alternatives) return false;
    size_t count = 0;
    for (size_t i = 0; i + 1 < capacity; i++) {
        th_type *candidate = th_name(state->types, profile->scalars[i].type_text);
        if (is_integer_type(candidate)
            || profile->scalars[i].class == IR_CLASS_FLOAT)
            alternatives[count++] = candidate;
    }
    th_type *sized_integer = th_var(state->types);
    if (!sized_integer || !th_var_constrain(state->types, sized_integer,
            (th_constraint){.kind = TH_CON_KIND, .type_kind = TH_BITINT}))
        return false;
    alternatives[count++] = sized_integer;
    return th_var_constrain(state->types, type,
        (th_constraint){.kind = TH_CON_ONE_OF,
            .one_of = {.types = alternatives, .n = count}});
}

bool
refine_intrinsic_unary_result(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (NT_IS(grammar, node, "unary-expr")
        && core_tree_num_children(node) == 2
        && parse_tree_leaf_is(core_tree_child(node, 0), CORE_STRING_STATIC("+"))) {
        th_type *operand = parse_tree_type(core_tree_child(node, 1));
        if (constrain_primitive_numeric(state, operand)) return true;
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
            CORE_STRING_STATIC("unary + requires a primitive numeric operand"),
            link_source_location(node));
        return false;
    }
    core_string_t trait = {0}, method = {0};
    if (!unary_operator_dispatch_for_node(grammar, node,
                                          &trait, &method)) return true;
    n00bb_link_use_site_t site = {
        .node = node,
        .dispatch_trait = trait,
        .dispatch_method = method,
        .operator_dispatch = true,
    };
    th_type *operand = parse_tree_type(core_tree_child(node, 1));
    if (core_string_eq(trait, CORE_STRING_STATIC("Negate"))
        && n00bb_open_integer(operand)
        && !NT_IS(grammar, parse_tree_integer_literal_origin(
                       core_tree_child(node, 1)), "simple-lit")) {
        if (core_dict_contains(&state->integer_edge_sites, node)) return true;
        th_type *negated = n00bb_integer_result_variable(state, node,
            IR_OP_NEG, core_tree_child(node, 1), nullptr);
        th_type *current = parse_tree_type(node);
        if (!negated || (current && th_kind_of(th_resolve(current)) == TH_VAR
                && !n00bb_open_integer(current)
                && !th_unify_in_place(state->types, current, negated)))
            return false;
        parse_tree_set_type(node, negated);
        return true;
    }
    if (!primitive_operator_site(state, &site)) return true;
    th_type *expected = core_string_eq(trait, CORE_STRING_STATIC("Not"))
        ? th_name(state->types, CORE_STRING_STATIC("bool")) : operand;
    th_type *result = parse_tree_type(node);
    if (!result || th_is_bottom(result)) {
        parse_tree_set_type(node, expected);
        result = expected;
    }
    if (operand && expected && result
        && th_unify_in_place(state->types, result, expected)) {
        parse_tree_t *negated = core_tree_child(node, 1);
        if (core_string_eq(trait, CORE_STRING_STATIC("Negate"))
            && is_integer_type(operand)
            && !NT_IS(grammar, parse_tree_integer_literal_origin(negated),
                      "simple-lit"))
            n00bb_integer_node_result(state, node, IR_OP_NEG, negated,
                                      nullptr);
        return true;
    }
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
        unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
        CORE_STRING_STATIC(
            "built-in unary operator operand and result have incompatible types"),
        link_source_location(node));
    return false;
}
bool
index_is_compound_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    bool through_receiver = false;
    return NT_IS(grammar, index_assignment_statement(grammar, node,
                                                     &through_receiver),
                 "binop-assign-stmt");
}

th_type *
n00bb_callback_function_type(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *member)
{
    if (!member_access_node(grammar, member)
        || core_tree_num_children(member) < 3
        || !core_string_eq(first_token_text(core_tree_child(member, 2)),
                           CORE_STRING_STATIC("call"))) return nullptr;
    parse_tree_t *receiver = core_tree_child(member, 0);
    th_type *type = receiver && !core_tree_is_leaf(receiver)
        ? parse_tree_type(receiver) : nullptr;
    n00bb_tu_state_t *state = tu_state(unit);
    type = type && state ? n00bb_dereferenced_type(state->types, type) : nullptr;
    if (type && th_kind_of(type) == TH_FUNCTION) return type;
    n00bb_callback_contract_t *contract =
        n00bb_callback_contract_for_type(state, type);
    return contract && contract->call
        ? n00bb_specialize_callback_role(state->types, contract, type,
                                         contract->call->type) : nullptr;
}

parse_tree_t *
operator_receiver_node(n00bb_link_use_site_t *site)
{
    if (!site || !site->node || !core_tree_num_children(site->node))
        return nullptr;
    bool unary = core_tree_num_children(site->node) == 2
        && (core_string_eq(site->dispatch_trait,
                           CORE_STRING_STATIC("Negate"))
            || core_string_eq(site->dispatch_trait,
                              CORE_STRING_STATIC("Not")));
    return core_tree_child(site->node, unary ? 1 : 0);
}
bool
module_initializer_declaration(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const core_string_t declarations[] = {
        CORE_STRING_STATIC("func-def"), CORE_STRING_STATIC("context-def"),
        CORE_STRING_STATIC("enum-stmt"), CORE_STRING_STATIC("import-stmt"),
        CORE_STRING_STATIC("alias-stmt"), CORE_STRING_STATIC("type-decl"),
        CORE_STRING_STATIC("record-decl"), CORE_STRING_STATIC("object-decl"),
        CORE_STRING_STATIC("variant-decl"),
        CORE_STRING_STATIC("trait-decl"), CORE_STRING_STATIC("callback-decl"),
        CORE_STRING_STATIC("package-decl"), CORE_STRING_STATIC("when-stmt"),
    };
    for (size_t i = 0; i < sizeof(declarations) / sizeof(declarations[0]);
         i++)
        if (nt_is(grammar, node, declarations[i])) return true;
    return false;
}

bool
is_module_initializer_use(parse_grammar_t *grammar, parse_tree_t *node,
                          parse_sym_entry_t *initializer_owner)
{
    if (initializer_owner) return true;
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "func-def")
            || NT_IS(grammar, at, "context-def")) return false;
        if (!NT_IS(grammar, at, "top-level-stmt")) continue;
        for (size_t i = 0; i < core_tree_num_children(at); i++) {
            parse_tree_t *statement = core_tree_child(at, i);
            if (NT_IS(grammar, statement, "variable-decl")) return false;
            if (parse_tree_nt_name(grammar, statement).data)
                return !module_initializer_declaration(grammar, statement);
        }
    }
    return false;
}

bool
initializer_node_is_reference_operand(parse_grammar_t *grammar,
                                       parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (!NT_IS(grammar, at, "unary-expr")
            || core_tree_num_children(at) < 2) continue;
        parse_tree_t *operator = core_tree_child(at, 0);
        parse_tree_t *operand = core_tree_child(at, 1);
        if (operator && core_tree_is_leaf(operator)
            && core_string_eq(parse_tree_leaf_text(operator),
                              CORE_STRING_STATIC("ref"))
            && node_is_within(node, operand)) return true;
    }
    return false;
}

bool
initializer_node_is_reference_store_target(parse_grammar_t *grammar,
                                        core_translation_unit_t *unit,
                                        parse_tree_t *node,
                                        parse_sym_entry_t *symbol)
{
    if (!node || !symbol) return false;
    for (parse_tree_t *at = parse_tree_parent(node); at;
         at = parse_tree_parent(at)) {
        if (!NT_IS(grammar, at, "unary-expr")
            || core_tree_num_children(at) < 2) continue;
        parse_tree_t *operator = core_tree_child(at, 0);
        parse_tree_t *operand = core_tree_child(at, 1);
        parse_tree_t *lvalue = n00bb_simple_lvalue(operand);
        if (!operator || !core_tree_is_leaf(operator)
            || !core_string_eq(parse_tree_leaf_text(operator),
                               CORE_STRING_STATIC("ref"))
            || !lvalue || !node_is_within(node, lvalue)
            || canonical_symbol(attached_symbol(grammar, unit, lvalue))
                != canonical_symbol(symbol)) continue;
        for (parse_tree_t *call = parse_tree_parent(at); call;
             call = parse_tree_parent(call)) {
            parse_tree_t *args = CHILD_NT(grammar, call, "call-args");
            if (!args || !node_is_within(at, args)) continue;
            parse_tree_t *callee = core_tree_num_children(call)
                ? core_tree_child(call, 0) : nullptr;
            parse_sym_entry_t *function = bound_function_symbol(callee);
            if (!function) continue;
            const n00bb_axiom_t *axiom = n00bb_axiom_of_symbol(function);
            if (!axiom || !(axiom->writes & N00BB_AXIOM_PARAM(0))
                || (axiom->roles & N00BB_AXIOM_READS_TARGET)) continue;
            parse_tree_t *positional = CHILD_NT(grammar, args,
                                                "positional-args");
            core_list_t(n00bb_tree_ptr_t) actuals =
                core_list_new(n00bb_tree_ptr_t);
            if (positional)
                collect_positional_nodes(grammar, positional, &actuals);
            bool direct_target = actuals.len
                && node_is_within(at, actuals.data[0]);
            core_list_free(actuals);
            if (direct_target) return true;
        }
    }
    return false;
}

bool
initializer_node_is_out_argument(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node)
{
    for (parse_tree_t *call = node
             ? parse_tree_parent(node) : nullptr;
         call; call = parse_tree_parent(call)) {
        parse_tree_t *args = CHILD_NT(grammar, call, "call-args");
        if (!args || !node_is_within(node, args)) continue;
        parse_tree_t *callee = core_tree_num_children(call)
            ? core_tree_child(call, 0) : nullptr;
        parse_sym_entry_t *symbol = bound_function_symbol(callee);
        if (!symbol) return false;
        parse_tree_t *formals = CHILD_NT(grammar, symbol->decl_node,
                                         "func-type-signature");
        formals = CHILD_NT(grammar, formals, "param-decl");
        parse_tree_t *member = member_access_node(grammar, callee);
        parse_tree_t *receiver = member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
        size_t formal_offset = member && receiver
            && !call_receiver_is_module(grammar, unit, receiver)
                ? 1 : 0;

        parse_tree_t *positional = CHILD_NT(grammar, args,
                                             "positional-args");
        core_list_t(n00bb_tree_ptr_t) actuals =
            core_list_new(n00bb_tree_ptr_t);
        if (positional)
            collect_positional_nodes(grammar, positional, &actuals);
        for (size_t i = 0; i < actuals.len; i++) {
            if (!node_is_within(node, actuals.data[i])) continue;
            th_mode mode = TH_IN;
            bool found = function_parameter_info(grammar, symbol, formals,
                i + formal_offset, nullptr, nullptr, nullptr, &mode);
            core_list_free(actuals);
            return found && mode == TH_OUT;
        }
        core_list_free(actuals);

        parse_tree_t *keywords = CHILD_NT(grammar, args, "keyword-args");
        core_list_t(n00bb_tree_ptr_t) named =
            core_list_new(n00bb_tree_ptr_t);
        if (keywords) collect_keyword_nodes(grammar, keywords, &named);
        for (size_t i = 0; i < named.len; i++) {
            parse_tree_t *expression = CHILD_NT(grammar, named.data[i],
                                                  "expression");
            if (!expression || !node_is_within(node, expression)) continue;
            core_string_t name = call_keyword_name(named.data[i]);
            size_t index = 0;
            th_mode mode = TH_IN;
            bool found = name.data
                && function_parameter_named(grammar, symbol, formals, name,
                                             &index)
                && function_parameter_info(grammar, symbol, formals, index,
                    nullptr, nullptr, nullptr, &mode);
            core_list_free(named);
            return found && mode == TH_OUT;
        }
        core_list_free(named);
        return false;
    }
    return false;
}

parse_sym_entry_t *
module_variable_initializer_owner(parse_grammar_t *grammar,
                                  parse_symtab_t *symtab,
                                  parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "func-def")) return nullptr;
        bool declaration = NT_IS(grammar, at, "sym-decl");
        bool assignment = NT_IS(grammar, at, "assign-stmt");
        if (!declaration && !assignment) continue;
        parse_sym_entry_t *symbol = parse_tree_sym(at);
        parse_scope_t *scope = symtab
            ? parse_symtab_scope_for_node(symtab, node) : nullptr;
        if (!scope && symtab)
            scope = parse_symtab_scope_for_node(symtab, at);
        if (!symbol && assignment && symtab
            && scope && core_tree_num_children(at) > 0) {
            parse_tree_t *target = core_tree_child(at, 0);
            parse_tree_t *lvalue = n00bb_simple_lvalue(target);
            symbol = lvalue
                ? parse_symtab_resolve_node(symtab, lvalue,
                                            core_string_empty()) : nullptr;
            if (symbol && symbol->kind != PARSE_SYM_VARIABLE) symbol = nullptr;
        }
        parse_tree_t *expression = nullptr;
        if (declaration) {
            expression = CHILD_NT(grammar, at, "expression");
        }
        else {
            const parse_rule_t *rule = parse_tree_rule(grammar, at);
            parse_annotation_t *assign = rule
                ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
                : nullptr;
            expression = assign
                ? parse_tree_resolve_child_ref(grammar, at,
                                               assign->value_ref)
                : nullptr;
        }
        parse_tree_t *branch = node;
        while (branch && parse_tree_parent(branch) != at)
            branch = parse_tree_parent(branch);
        bool owned_binding = declaration ? symbol && symbol->decl_node == at
            : symbol && symbol_has_definition(symbol, at)
                && symbol_binding_node(symbol) == at;
        return owned_binding && symbol->scope
                && expression == branch
                && core_string_eq(symbol->scope->scope_tag,
                                  CORE_STRING_STATIC("module"))
            ? symbol : nullptr;
    }
    return nullptr;
}
void
validate_builtin_generic_type_arity(parse_grammar_t *grammar,
                                   parse_tree_t *node,
                                   core_translation_unit_t *unit,
                                   th_type *type)
{
    if (!grammar || !node || !unit || !type) return;
    core_string_t name = {};
    size_t argument_count = 0;
    if (!type || !th_named_info(th_resolve(type), &name, nullptr,
                                &argument_count)) return;
    int expected = builtin_generic_type_arity(name);
    if (expected < 0 || argument_count == (size_t)expected) return;

    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "built-in type '%.*s' requires %d type argument%s, but %zu provided",
        (int)name.u8_bytes, name.data, expected,
        expected == 1 ? "" : "s", argument_count);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_FATAL,
        CORE_STRING_STATIC("N00BB-T0016"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(node));
    core_buffer_release(&message);
}

bool
n00bb_flagset_member(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_ENUM_CONST
        && symbol->owner_type && symbol->owner_type->member_scope
        && core_string_eq(symbol->owner_type->member_scope->scope_tag,
                          CORE_STRING_STATIC("flagset"));
}

bool
n00bb_flagset_instance_access(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *node,
                              parse_sym_entry_t *member)
{
    if (!n00bb_flagset_member(member) || !node
        || core_tree_num_children(node) < 3) return false;
    parse_tree_t *receiver = core_tree_child(node, 0);
    parse_node_resolution_t resolution = parse_tree_resolution(receiver);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && resolution.target.symbol == member->owner_type) return false;
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_link_use_site_t *receiver_site = state
        ? link_use_site_for_node(state, receiver) : nullptr;
    if (receiver_site && receiver_site->symbol == member->owner_type)
        return false;
    if (!core_tree_is_leaf(receiver)
        && core_string_eq(parse_tree_unresolved_name(receiver),
                          member->owner_type->name)) return false;
    parse_sym_entry_t *receiver_symbol = attached_symbol(grammar, unit,
                                                          receiver);
    if (receiver_symbol == member->owner_type) return false;
    th_type *receiver_type = receiver && !core_tree_is_leaf(receiver)
        ? parse_tree_type(receiver) : nullptr;
    if ((!receiver_type || th_kind_of(th_resolve(receiver_type)) == TH_VAR)
        && receiver_symbol) receiver_type = receiver_symbol->type;
    return state && receiver_type && member->owner_type->type
        && th_hash(state->types, th_resolve(receiver_type))
            == th_hash(state->types, th_resolve(member->owner_type->type));
}
static const n00bb_literal_registration_t N00BB_LITERAL_HANDLERS[] = {
    {CORE_STRING_STATIC("array"), N00BB_LITERAL_SEQUENCE_SHAPE,
     N00BB_LITERAL_ARRAY, CORE_STRING_STATIC("array")},
    {CORE_STRING_STATIC("a"), N00BB_LITERAL_SEQUENCE_SHAPE,
     N00BB_LITERAL_ARRAY, CORE_STRING_STATIC("array")},
    {CORE_STRING_STATIC("binary"), N00BB_LITERAL_STRING_SHAPE,
     N00BB_LITERAL_BINARY, CORE_STRING_STATIC("binary")},
    {CORE_STRING_STATIC("b"), N00BB_LITERAL_STRING_SHAPE,
     N00BB_LITERAL_BINARY, CORE_STRING_STATIC("binary")},
    {CORE_STRING_STATIC("u32"), N00BB_LITERAL_STRING_SHAPE,
     N00BB_LITERAL_U32, CORE_STRING_STATIC("u32")},
};

const n00bb_literal_registration_t *
n00bb_literal_handler(core_string_t prefix, n00bb_literal_shape_t shape)
{
    for (size_t i = 0; i < sizeof(N00BB_LITERAL_HANDLERS)
                            / sizeof(N00BB_LITERAL_HANDLERS[0]); i++) {
        const n00bb_literal_registration_t *registration =
            &N00BB_LITERAL_HANDLERS[i];
        if (registration->shape == shape
            && core_string_eq(prefix, registration->prefix))
            return registration;
    }
    return nullptr;
}

bool
n00bb_literal_prefix_registered(core_string_t prefix)
{
    for (size_t i = 0; i < sizeof(N00BB_LITERAL_HANDLERS)
                            / sizeof(N00BB_LITERAL_HANDLERS[0]); i++)
        if (core_string_eq(prefix, N00BB_LITERAL_HANDLERS[i].prefix))
            return true;
    return false;
}

bool
register_user_litmod(void *context, core_translation_unit_t *unit,
                     parse_tree_t *declaration, core_string_t modifier,
                     core_string_t kind)
{
    (void)context;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !modifier.u8_bytes || !declaration)
        return false;
    n00bb_literal_shape_t shape =
        core_string_eq(kind, CORE_STRING_STATIC("quotes"))
            ? N00BB_LITERAL_STRING_SHAPE
        : core_string_eq(kind, CORE_STRING_STATIC("sequence"))
            ? N00BB_LITERAL_SEQUENCE_SHAPE
        : N00BB_LITERAL_BRACE_SHAPE;
    if ((core_string_eq(kind, CORE_STRING_STATIC("quotes"))
            || core_string_eq(kind, CORE_STRING_STATIC("sequence"))
            || core_string_eq(kind, CORE_STRING_STATIC("dict"))
            || core_string_eq(kind, CORE_STRING_STATIC("set")))
        && n00bb_literal_handler(modifier, shape)) return false;
    for (size_t i = 0; i < state->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *existing = &state->literal_modifiers.data[i];
        if (core_string_eq(existing->modifier, modifier)
            && core_string_eq(existing->kind, kind)) return false;
    }
    n00bb_user_litmod_t registration = {
        .modifier = core_string_from_raw(modifier.data,
                                         (int64_t)modifier.u8_bytes),
        .kind = core_string_from_raw(kind.data, (int64_t)kind.u8_bytes),
        .declaration = declaration,
        .module = unit->module.identifier,
    };
    parse_token_info_t *token = parse_tree_find_first_terminal(declaration);
    if (token) registration.location = (core_diag_location_t){
        .line = parse_token_line(token),
        .column = parse_token_column(token),
        .end_line = parse_token_endline(token),
        .end_column = parse_token_endcol(token),
        .has_location = true,
    };
    if (!registration.modifier.data || !registration.kind.data) {
        core_free(registration.modifier.data);
        core_free(registration.kind.data);
        return false;
    }
    core_list_push(state->literal_modifiers, registration);
    return true;
}
static n00bb_user_litmod_t *
local_user_litmod(n00bb_tu_state_t *state, core_string_t modifier,
                  core_string_t kind)
{
    if (!state) return nullptr;
    for (size_t i = 0; i < state->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *registration =
            &state->literal_modifiers.data[i];
        if (core_string_eq(registration->modifier, modifier)
            && core_string_eq(registration->kind, kind))
            return registration;
    }
    return nullptr;
}

static n00bb_user_litmod_t *imported_user_litmod(n00bb_tu_state_t *state,
                                                 core_string_t modifier,
                                                 core_string_t kind,
                                                 bool *ambiguous);

bool
report_shadowed_litmods(core_translation_unit_t *unit,
                        n00bb_tu_state_t *state)
{
    bool valid = true;
    for (size_t i = 0; state && i < state->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *local = &state->literal_modifiers.data[i];
        n00bb_user_litmod_t *visible = imported_user_litmod(state,
            local->modifier, local->kind, nullptr);
        if (!visible) continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "literal modifier '%.*s' for %.*s literals is already declared in module '%.*s'",
            (int)local->modifier.u8_bytes, local->modifier.data,
            (int)local->kind.u8_bytes, local->kind.data,
            (int)visible->module.u8_bytes, visible->module.data);
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-PP0001"),
            core_string_borrow(message.data, message.byte_len),
            local->location);
        core_buffer_release(&message);
        valid = false;
    }
    return valid;
}

n00bb_user_litmod_t *
user_litmod(n00bb_tu_state_t *state, core_string_t modifier,
            core_string_t kind, bool *ambiguous)
{
    if (ambiguous) *ambiguous = false;
    n00bb_user_litmod_t *local = local_user_litmod(state, modifier, kind);
    return local ? local : imported_user_litmod(state, modifier, kind,
                                                ambiguous);
}

typedef bool (*litmod_visitor_t)(n00bb_user_litmod_t *registration,
                                 void *context);

static bool
visit_exported_litmods(n00bb_package_export_t *export, core_string_t kind,
                       litmod_visitor_t visit, void *context)
{
    n00bb_tu_state_t *source = tu_state(export->source_unit);
    for (size_t i = 0; source && i < source->literal_modifiers.len; i++) {
        n00bb_user_litmod_t *exported = &source->literal_modifiers.data[i];
        if (!core_string_eq(exported->kind, kind)) continue;
        parse_sym_entry_t *handler = exported->declaration
            ? parse_tree_sym(exported->declaration) : exported->function;
        if (!handler || (!export->module_export
                && handler != canonical_symbol(export->symbol)))
            continue;
        if (!visit(exported, context)) return false;
    }
    return true;
}

static bool
visit_imported_litmods(n00bb_tu_state_t *state, core_string_t kind,
                       litmod_visitor_t visit, void *context)
{
    if (!state || !state->unit) return true;
    for (size_t i = 0; i < core_translation_unit_import_count(state->unit);
         i++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            state->unit, i);
        n00bb_tu_state_t *imported_state = tu_state(imported);
        for (size_t alias_ix = 0; imported_state
                && alias_ix < state->import_aliases.len; alias_ix++) {
            n00bb_import_alias_t *alias = &state->import_aliases.data[alias_ix];
            if (alias->unit != imported) continue;
            for (size_t package_ix = 0;
                 package_ix < imported_state->package_declarations.len;
                 package_ix++) {
                n00bb_package_declaration_t *package =
                    &imported_state->package_declarations.data[package_ix];
                if (!core_string_eq(package->name, alias->name)) continue;
                for (size_t export_ix = 0; export_ix < package->exports.len;
                     export_ix++) {
                    n00bb_package_export_t *export =
                        &package->exports.data[export_ix];
                    if (export->source_unit && !visit_exported_litmods(
                            export, kind, visit, context))
                        return false;
                }
            }
        }
    }
    return true;
}

typedef struct {
    core_string_t modifier;
    n00bb_user_litmod_t *found;
    bool ambiguous;
} imported_litmod_search_t;

static bool
match_imported_litmod(n00bb_user_litmod_t *registration, void *context)
{
    imported_litmod_search_t *search = context;
    if (!core_string_eq(registration->modifier, search->modifier))
        return true;
    if (search->found && search->found != registration) {
        search->ambiguous = true;
        return false;
    }
    search->found = registration;
    return true;
}

static n00bb_user_litmod_t *
imported_user_litmod(n00bb_tu_state_t *state, core_string_t modifier,
                     core_string_t kind, bool *ambiguous)
{
    imported_litmod_search_t search = {.modifier = modifier};
    visit_imported_litmods(state, kind, match_imported_litmod, &search);
    if (ambiguous) *ambiguous = search.ambiguous;
    return search.ambiguous ? nullptr : search.found;
}

static bool
collect_visible_litmod(n00bb_user_litmod_t *registration, void *context)
{
    core_list_t(n00bb_user_litmod_ptr_t) *found = context;
    for (size_t i = 0; i < found->len; i++)
        if (found->data[i] == registration) return true;
    core_list_push(*found, registration);
    return true;
}

core_string_t
n00bb_literal_shape_trait(core_string_t kind)
{
    return core_string_eq(kind, CORE_STRING_STATIC("sequence"))
            ? CORE_STRING_STATIC("Sequence_Like")
        : core_string_eq(kind, CORE_STRING_STATIC("set"))
            ? CORE_STRING_STATIC("Set_Like")
        : core_string_eq(kind, CORE_STRING_STATIC("dict"))
            ? CORE_STRING_STATIC("Dict_Like")
        : (core_string_t){0};
}

core_list_t(n00bb_user_litmod_ptr_t)
n00bb_visible_litmods(n00bb_tu_state_t *state, core_string_t kind)
{
    core_list_t(n00bb_user_litmod_ptr_t) found =
        core_list_new(n00bb_user_litmod_ptr_t);
    for (size_t i = 0; state && i < state->literal_modifiers.len; i++)
        if (core_string_eq(state->literal_modifiers.data[i].kind, kind))
            collect_visible_litmod(&state->literal_modifiers.data[i], &found);
    visit_imported_litmods(state, kind, collect_visible_litmod, &found);
    return found;
}

core_string_t
literal_modifier(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node) return (core_string_t){0};
    parse_tree_t *last = node;
    while (last && !core_tree_is_leaf(last)) {
        size_t count = core_tree_num_children(last);
        last = count ? core_tree_child(last, count - 1) : nullptr;
    }
    parse_token_info_t *token = parse_node_token(last);
    if (token && core_option_is_set(parse_token_postfix_modifier(token)))
        return core_option_get(parse_token_postfix_modifier(token));
    return NT_IS(grammar, node, "embed-lit") ? CORE_STRING_STATIC("string")
                                             : (core_string_t){0};
}

bool
dict_only_empty_literal(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "dict-or-set-lit")
        || core_tree_num_children(node) != 3) return false;
    parse_token_info_t *marker = parse_node_token(core_tree_child(node, 1));
    return marker && parse_token_tid(marker) == ':';
}

bool
literal_modifier_terminal(parse_grammar_t *grammar, parse_tree_t *leaf,
                          parse_tree_t *parent)
{
    parse_token_info_t *token = parse_node_token(leaf);
    if (!token || !parent) return false;
    core_string_t terminal = parse_terminal_name(grammar, parse_token_tid(token));
    if (NT_IS(grammar, parent, "simple-lit"))
        return core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
            || core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT"))
            || core_string_eq(terminal, CORE_STRING_STATIC("FLOAT_LIT"))
            || core_string_eq(terminal, CORE_STRING_STATIC("STRING_LIT"))
            || n00bb_nil_spelling(first_token_text(leaf));
    if (NT_IS(grammar, parent, "bool-lit"))
        return parse_tree_leaf_is(leaf, CORE_STRING_STATIC("true"))
            || parse_tree_leaf_is(leaf, CORE_STRING_STATIC("false"));
    if (NT_IS(grammar, parent, "embed-lit"))
        return core_string_eq(terminal, CORE_STRING_STATIC("EMBED"));
    if (NT_IS(grammar, parent, "list-lit"))
        return parse_tree_leaf_is(leaf, CORE_STRING_STATIC("]"));
    if (NT_IS(grammar, parent, "dict-or-set-lit"))
        return parse_tree_leaf_is(leaf, CORE_STRING_STATIC("}"));
    return NT_IS(grammar, parent, "tuple-or-paren")
        && parse_tree_leaf_is(leaf, CORE_STRING_STATIC(")"));
}
static void
push_ungrouped_children(parse_tree_t *node,
                        core_list_t(parse_tree_ptr_t) *children)
{
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (parse_tree_is_group(child))
            push_ungrouped_children(child, children);
        else core_list_push(*children, child);
    }
}

bool
cache_static_literal_node(parse_grammar_t *grammar,
                          n00bb_tu_state_t *state, parse_tree_t *node)
{
    bool is_static = NT_IS(grammar, node, "simple-lit")
        || NT_IS(grammar, node, "embed-lit");
    if (!is_static && NT_IS(grammar, node, "named-field")) {
        parse_tree_t *value = CHILD_NT(grammar, node, "expression");
        is_static = value
            && n00bb_node_facts(value)
            && n00bb_node_facts(value)->static_literal;
    }
    if (!is_static && core_tree_num_children(node) == 1) {
        parse_tree_t *child = core_tree_child(node, 0);
        is_static = !core_tree_is_leaf(child)
            && n00bb_node_facts(child)
            && n00bb_node_facts(child)->static_literal;
    }
    else if (!is_static && (NT_IS(grammar, node, "list-elems")
             || NT_IS(grammar, node, "list-lit")
             || NT_IS(grammar, node, "dict-entry")
             || NT_IS(grammar, node, "dict-entries")
             || NT_IS(grammar, node, "set-entries")
             || NT_IS(grammar, node, "dict-or-set-lit")
             || NT_IS(grammar, node, "expr-comma-list")
             || NT_IS(grammar, node, "named-field-list")
             || NT_IS(grammar, node, "tuple-or-paren"))) {
        is_static = true;
        core_list_t(parse_tree_ptr_t) children =
            core_list_new(parse_tree_ptr_t);
        push_ungrouped_children(node, &children);
        for (size_t i = 0; i < children.len; i++) {
            parse_tree_t *child = children.data[i];
            if (!core_tree_is_leaf(child)
                && (!n00bb_node_facts(child)
                    || !n00bb_node_facts(child)->static_literal)) {
                is_static = false;
                break;
            }
        }
        core_list_free(children);
    }
    if (!is_static) return true;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->static_literal = true;
    return true;
}

static bool
store_ordered_children(n00bb_tu_state_t *state, parse_tree_t *node,
                       core_list_t(parse_tree_ptr_t) *items)
{
    n00bb_ordered_nodes_t *cached = core_arena_alloc(&state->semantic_values,
                                                      sizeof(*cached));
    parse_tree_t **nodes = items->len ? core_arena_alloc(
        &state->semantic_values, items->len * sizeof(*nodes)) : nullptr;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts || !cached || (items->len && !nodes)) return false;
    if (items->len) memcpy(nodes, items->data, items->len * sizeof(*nodes));
    *cached = (n00bb_ordered_nodes_t){.nodes = nodes, .count = items->len};
    facts->ordered_nodes = cached;
    return true;
}

bool
cache_ordered_literal_children(parse_grammar_t *grammar,
                               n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!grammar || !state || !node) return false;
    bool list_node = NT_IS(grammar, node, "list-elems")
        || NT_IS(grammar, node, "set-entries")
        || NT_IS(grammar, node, "dict-entries")
        || NT_IS(grammar, node, "dict-entry")
        || NT_IS(grammar, node, "expr-comma-list")
        || NT_IS(grammar, node, "named-field-list")
        || NT_IS(grammar, node, "tuple-or-paren");
    if (!list_node) return true;

    core_list_t(parse_tree_ptr_t) children = core_list_new(parse_tree_ptr_t);
    push_ungrouped_children(node, &children);
    core_list_t(parse_tree_ptr_t) items = core_list_new(parse_tree_ptr_t);
    for (size_t i = 0; i < children.len; i++)
    {
        parse_tree_t *child = children.data[i];
        bool value_node = NT_IS(grammar, child, "expression")
            || NT_IS(grammar, child, "named-field");
        if (value_node) {
            core_list_push(items, child);
            continue;
        }
        bool list_child = NT_IS(grammar, child, "list-elems")
            || NT_IS(grammar, child, "set-entries")
            || NT_IS(grammar, child, "dict-entries")
            || NT_IS(grammar, child, "dict-entry")
            || NT_IS(grammar, child, "expr-comma-list")
            || NT_IS(grammar, child, "named-field-list");
        if (list_child) {
            n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(child);
            if (!cached) {
                core_list_free(children);
                core_list_free(items);
                return false;
            }
            for (size_t j = 0; j < cached->count; j++)
                core_list_push(items, cached->nodes[j]);
        }
    }

    bool ok = store_ordered_children(state, node, &items);
    core_list_free(children);
    core_list_free(items);
    return ok;
}

bool
cache_ordered_array_dimensions(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state, parse_tree_t *node)
{
    bool dimensions_node = NT_IS(grammar, node, "array-dimensions");
    bool base_node = NT_IS(grammar, node, "base-tspec");
    if (!dimensions_node && !base_node) return true;

    core_list_t(parse_tree_ptr_t) items = core_list_new(parse_tree_ptr_t);
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (dimensions_node && NT_IS(grammar, child, "array-dimension")) {
            core_list_push(items, child);
            continue;
        }
        if (base_node && NT_IS(grammar, child, "array-dimensions")) {
            n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(child);
            if (!cached) {
                core_list_free(items);
                return false;
            }
            for (size_t j = 0; j < cached->count; j++)
                core_list_push(items, cached->nodes[j]);
            continue;
        }
        if (!parse_tree_is_group(child)) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (dimensions_node && NT_IS(grammar, item, "array-dimension")) {
                core_list_push(items, item);
            }
            else if (base_node
                     && NT_IS(grammar, item, "array-dimensions")) {
                n00bb_ordered_nodes_t *cached = n00bb_node_ordered_nodes(item);
                if (!cached) {
                    core_list_free(items);
                    return false;
                }
                for (size_t k = 0; k < cached->count; k++)
                    core_list_push(items, cached->nodes[k]);
            }
        }
    }

    bool ok = store_ordered_children(state, node, &items);
    if (ok && base_node) core_list_push(state->array_type_nodes, node);
    core_list_free(items);
    return ok;
}
static parse_sym_entry_t *
typecase_subject_symbol(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        parse_tree_t *case_block)
{
    parse_tree_t *typecase = parse_tree_parent(case_block);
    while (typecase && !NT_IS(grammar, typecase, "typecase-stmt")
           && !NT_IS(grammar, typecase, "typecase-value-expr"))
        typecase = parse_tree_parent(typecase);
    parse_tree_t *expression = typecase
        ? CHILD_NT(grammar, typecase, "expression") : nullptr;
    parse_tree_t *subject = n00bb_simple_lvalue(expression);
    parse_sym_entry_t *symbol = subject
        ? attached_symbol(grammar, unit, subject) : nullptr;
    return symbol && (symbol->kind == PARSE_SYM_VARIABLE
                      || symbol->kind == PARSE_SYM_PARAM)
        ? symbol : nullptr;
}

bool
cache_typecase_arm_types(parse_grammar_t *grammar,
                         n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "type-spec-list")) return true;
    parse_tree_t *arm = parse_tree_parent(node);
    if (!arm || (!NT_IS(grammar, arm, "typecase-case-block")
                 && !NT_IS(grammar, arm,
                           "typecase-value-case-block"))) return true;
    core_list_t(th_type_ptr_t) types = core_list_new(th_type_ptr_t);
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "type-spec")) {
            th_type *type = parse_tree_type(child);
            if (!type) goto invalid;
            core_list_push(types, type);
            continue;
        }
        if (!parse_tree_is_group(child)) continue;
        for (size_t j = 0; j < core_tree_num_children(child); j++) {
            parse_tree_t *item = core_tree_child(child, j);
            if (!NT_IS(grammar, item, "type-spec")) continue;
            th_type *type = parse_tree_type(item);
            if (!type) goto invalid;
            core_list_push(types, type);
        }
    }
    n00bb_typecase_arm_types_t *cached = core_arena_alloc(
        &state->semantic_values, sizeof(*cached));
    th_type **values = types.len ? core_arena_alloc(
        &state->semantic_values, types.len * sizeof(*values)) : nullptr;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, arm);
    if (!facts || !cached || (types.len && !values)) {
        core_list_free(types);
        return false;
    }
    if (types.len) memcpy(values, types.data, types.len * sizeof(*values));
    *cached = (n00bb_typecase_arm_types_t){
        .types = values, .count = types.len,
    };
    facts->typecase_arm_types = cached;
    core_list_free(types);
    return true;
invalid:
    core_list_free(types);
    return true;
}

void
prepare_typecase_arm(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     parse_tree_t *case_block)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_sym_entry_t *subject = state
        ? typecase_subject_symbol(grammar, unit, case_block)
        : nullptr;
    n00bb_node_facts_t *facts = n00bb_node_facts(case_block);
    n00bb_typecase_arm_types_t *types = state && facts
        ? facts->typecase_arm_types : nullptr;
    th_type *narrowed = types && types->count
        ? types->count == 1 ? types->types[0]
            : th_sum(state->types, TH_UNION, types->types, types->count)
        : nullptr;
    if (!subject) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-TYPECASE"),
            CORE_STRING_STATIC("typecase requires a variable subject"));
        return;
    }
    if (!narrowed) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-TYPECASE"),
            CORE_STRING_STATIC("typecase arm contains an invalid type specification"));
        return;
    }
    parse_symtab_t *symtab = state->annotations->symtab;
    parse_tree_t *body = CHILD_NT(grammar, case_block, "body");
    if (!body) body = CHILD_NT(grammar, case_block, "value-body");
    parse_scope_t *lexical_scope = body
        ? parse_symtab_scope_for_node(symtab, body) : nullptr;
    parse_scope_t *scope = parse_symtab_scope_in_namespace(
        symtab, lexical_scope, CORE_STRING_STATIC(""));
    parse_sym_entry_t *shadow = scope
        ? parse_symtab_lookup_in(scope, subject->name) : nullptr;
    if (shadow) {
        if (shadow->shadowed != subject
            || parse_sym_binding(shadow,
                CORE_STRING_STATIC("n00bb.typecase-subject")) != subject) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_ERROR,
                CORE_STRING_STATIC("N00BB-TYPECASE"),
                CORE_STRING_STATIC(
                    "typecase subject conflicts with another branch refinement"));
            return;
        }
    }
    else {
        parse_scope_t *previous = parse_symtab_current_scope(
            symtab, CORE_STRING_STATIC(""));
        if (!scope || !previous) return;
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC(""), scope);
        shadow = parse_symtab_add(symtab, CORE_STRING_STATIC(""),
            subject->name, subject->kind, nullptr);
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC(""), previous);
        if (!shadow) return;
        shadow->type = narrowed;
        shadow->mutability = subject->mutability;
        shadow->direction = subject->direction;
        shadow->pass_by = subject->pass_by;
        shadow->has_mode = subject->has_mode;
        shadow->mode = subject->mode;
        shadow->type_is_inferred = subject->type_is_inferred;
        shadow->is_optional = subject->is_optional;
        shadow->is_variadic = subject->is_variadic;
        shadow->storage_duration = subject->storage_duration;
        shadow->shadowed = subject;
        if (!parse_sym_set_binding_origin(shadow, subject))
            return;
        parse_sym_set_phases(shadow, subject->phases,
            subject->mutable_phases, subject->initializer_phases);
        if (!parse_sym_set_binding(shadow,
                CORE_STRING_STATIC("n00bb.typecase-subject"), subject))
            return;
    }
    shadow->type = narrowed;
    parse_tree_set_resolution_symbol(case_block, shadow);
    if (NT_IS(grammar, case_block, "typecase-case-block"))
        parse_tree_set_type(case_block, narrowed);
}

bool
apply_typecase_narrowing(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!NT_IS(grammar, node, "primary")) return true;
    parse_sym_entry_t *symbol = parse_tree_sym(node);
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        symbol = resolution.target.symbol;
    if (!symbol) return true;
    for (parse_tree_t *parent = parse_tree_parent(node); parent;
         parent = parse_tree_parent(parent)) {
        if (!NT_IS(grammar, parent, "typecase-case-block")
            && !NT_IS(grammar, parent,
                      "typecase-value-case-block")) continue;
        parse_sym_entry_t *shadow = parse_tree_sym(parent);
        if (!shadow || shadow == symbol || shadow->shadowed != symbol
            || !parse_sym_binding(shadow,
                CORE_STRING_STATIC("n00bb.typecase-subject")))
            return true;
        (void)parse_sym_remove_use(symbol, node);
        if (!parse_tree_set_resolution_symbol(node, shadow)
            || !parse_sym_add_use(shadow, node)) return false;
        parse_tree_set_type(node, shadow->type);
        return true;
    }
    return true;
}
bool
simple_member_chain_name(parse_grammar_t *grammar, parse_tree_t *node,
                         core_string_t *name)
{
    if (!NT_IS(grammar, node, "member-chain")
        || core_tree_num_children(node) != 1) return false;
    parse_tree_t *identifier = core_tree_child(node, 0);
    parse_token_info_t *token = parse_node_token(identifier);
    if (!token || !core_string_eq(parse_terminal_name(grammar, parse_token_tid(token)),
                                  CORE_STRING_STATIC("IDENTIFIER")))
        return false;
    if (name) *name = parse_tree_leaf_text(identifier);
    return true;
}
static bool
member_chain_is_name(parse_grammar_t *grammar, parse_tree_t *node,
                     core_string_t name)
{
    core_string_t text = {0};
    if (!simple_member_chain_name(grammar, node, &text)) return false;
    return core_string_eq(text, name);
}

int
builtin_generic_type_arity(core_string_t name)
{
    if (!name.data) return -1;
    return generic_type_arity_string(canonical_builtin_type_name(name));
}

static th_type *
apply_type_arguments(th_ctx *types, core_string_t name,
                     const th_arg *arguments, size_t count)
{
    if (!types || !name.data) return nullptr;
    int expected = builtin_generic_type_arity(name);
    if (expected != -1 && expected != -2 && count && !arguments)
        return nullptr;
    for (size_t i = 0; expected != -1 && i < count; i++)
        if (arguments[i].kind != TH_ARG_TYPE) return nullptr;
    core_string_t canonical = canonical_builtin_type_name(name);
    if (core_string_eq(canonical, CORE_STRING_STATIC("Array")) && count == 1
        && arguments[0].kind == TH_ARG_TYPE)
        return th_array(types, arguments[0].type);
    if (core_string_eq(canonical, CORE_STRING_STATIC("Ref")) && count == 1
        && arguments[0].kind == TH_ARG_TYPE)
        return th_ind(types, 0, arguments[0].type);
    if (core_string_eq(canonical, CORE_STRING_STATIC("Foreign"))
        && count == 1 && arguments[0].kind == TH_ARG_TYPE)
        return th_ind(types, TH_FOREIGN, arguments[0].type);
    if (core_string_eq(canonical, CORE_STRING_STATIC("Foreign_Fn"))
        && count == 1 && arguments[0].kind == TH_ARG_TYPE
        && th_kind_of(th_resolve(arguments[0].type)) == TH_FUNCTION)
        return th_ind(types, TH_FOREIGN, arguments[0].type);
    if (core_string_eq(canonical, CORE_STRING_STATIC("Tuple"))) {
        th_type **elements = count
            ? core_alloc_array(th_type *, count) : nullptr;
        bool valid = !count || elements;
        for (size_t i = 0; valid && i < count; i++) {
            valid = arguments[i].kind == TH_ARG_TYPE;
            if (valid) elements[i] = arguments[i].type;
        }
        th_type *tuple = valid ? th_tuple(types, elements, count) : nullptr;
        core_free(elements);
        return tuple;
    }
    return th_apply(types, canonical, arguments, count);
}

static bool
has_direct_leaf_text(parse_tree_t *node, core_string_t text)
{
    if (!node) return false;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child) && parse_tree_leaf_is(child, text))
            return true;
    }
    return false;
}

static th_type *
compose_base_type_core(parse_grammar_t *grammar, core_translation_unit_t *unit,
                      parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !NT_IS(grammar, node, "base-tspec-core"))
        return nullptr;
    parse_tree_t *type_node = nullptr;
    parse_tree_t *application = CHILD_NT(grammar, node, "tspec-application");
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "member-chain")
            || NT_IS(grammar, child, "sized-int-tspec")
            || NT_IS(grammar, child, "tspec-tvar")
            || NT_IS(grammar, child, "tspec-func")
            || NT_IS(grammar, child, "union-tspec")) {
            type_node = child;
            break;
        }
        if ((core_tree_is_leaf(child) || NT_IS(grammar, child, "nil-keyword"))
            && n00bb_nil_spelling(first_token_text(child)))
            return th_name(state->types, CORE_STRING_STATIC("nil"));
    }
    if (!type_node) return nullptr;
    if (NT_IS(grammar, type_node, "tspec-func")) {
        parse_tree_t *parameters = CHILD_NT(grammar, type_node,
                                             "tspec-func-params");
        parse_tree_t *result = CHILD_NT(grammar, type_node,
            "tspec-function-result");
        parse_tree_t *return_node = CHILD_NT(grammar, result, "return-type");
        parse_tree_t *error_node = CHILD_NT(grammar, result, "error-type");
        parse_tree_t *grouped_union = CHILD_NT(grammar, parameters,
                                                "union-tspec");
        bool has_union_operator = grouped_union
            && core_tree_num_children(grouped_union) > 1;
        bool no_result = (!return_node
                          || !core_tree_num_children(return_node))
            && (!error_node || !core_tree_num_children(error_node));
        if (no_result && has_union_operator)
            return parse_tree_type(grouped_union);
    }
    th_type *base = parse_tree_type(type_node);
    parse_tree_t *base_parent = parse_tree_parent(node);
    if (!application && NT_IS(grammar, type_node, "member-chain")
        && member_chain_is_name(grammar, type_node,
                                CORE_STRING_STATIC("Tuple"))
        && !CHILD_NT(grammar, base_parent, "array-dimensions"))
        return th_tuple(state->types, nullptr, 0);
    if (NT_IS(grammar, type_node, "sized-int-tspec")) {
        parse_tree_t *size = CHILD_NT(grammar, type_node, "integer-size");
        ir_lit_t width = {0};
        core_string_t prefix = first_token_text(type_node);
        if (!size || !n00bb_parse_switch_integer(first_token_text(size),
                                                  &width)
            || !width.mag || width.mag > N00BB_MAX_SIZED_BITS) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0028"),
                CORE_STRING_STATIC("sized integer and bit widths must be between 1 and 128"),
                link_source_location(type_node));
            return nullptr;
        }
        bool is_signed = core_string_eq(prefix, CORE_STRING_STATIC("i"));
        if (!is_signed
            && !core_string_eq(prefix, CORE_STRING_STATIC("u"))
            && !core_string_eq(prefix, CORE_STRING_STATIC("bits")))
            return nullptr;
        return core_string_eq(prefix, CORE_STRING_STATIC("bits"))
            ? th_bits(state->types, (uint64_t)width.mag)
            : th_bitint(state->types, (uint64_t)width.mag, is_signed);
    }
    if (!application || !NT_IS(grammar, type_node, "member-chain"))
        return base;
    core_string_t name = {};
    bool is_variant = false;
    if (!base || !th_named_info(th_resolve(base), &name,
                                &is_variant, nullptr)) return nullptr;
    parse_tree_t *list = CHILD_NT(grammar, application, "tspec-list");
    core_list_t(th_arg) arguments = core_list_new(th_arg);
    bool valid = list && append_type_argument_pack(
        parse_tree_type(list), &arguments);
    th_type *applied = valid
        ? is_variant
            ? th_variant(state->types, name, arguments.data, arguments.len)
            : apply_type_arguments(state->types, name, arguments.data,
                                   arguments.len)
        : nullptr;
    core_list_free(arguments);
    return applied;
}

th_type *
compose_type_syntax_node(parse_grammar_t *grammar,
                         core_translation_unit_t *unit, parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->types || !node || core_tree_is_leaf(node))
        return nullptr;
    if (NT_IS(grammar, node, "one-tspec")) {
        parse_tree_t *base = CHILD_NT(grammar, node, "base-tspec");
        th_type *element = base ? parse_tree_type(base) : nullptr;
        if (!element || !has_direct_leaf_text(node,
                CORE_STRING_STATIC("?"))) return element;
        return th_option(state->types, element);
    }
    if (NT_IS(grammar, node, "tspec-list"))
        return type_argument_pack(grammar, state->types, node);
    if (NT_IS(grammar, node, "tspec-func-param")) {
        parse_tree_t *type = CHILD_NT(grammar, node, "union-tspec");
        return type ? parse_tree_type(type) : nullptr;
    }
    if (NT_IS(grammar, node, "tspec-func-params"))
        return function_parameter_pack(grammar, state->types, node);
    if (NT_IS(grammar, node, "tspec-func")) {
        parse_tree_t *parameters = CHILD_NT(grammar, node,
                                             "tspec-func-params");
        core_list_t(th_param) params = core_list_new(th_param);
        core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
        th_param rest = {0};
        bool has_rest = false;
        th_type *parameter_shape = parameters
            ? th_resolve(parse_tree_type(parameters)) : nullptr;
        bool valid = !parameters || (parameter_shape
            && th_kind_of(parameter_shape) == TH_FUNCTION);
        for (size_t i = 0; valid && parameter_shape
             && i < th_func_nparams(parameter_shape); i++) {
            const th_param *parameter = th_func_param(parameter_shape, i);
            if (!parameter) valid = false;
            else core_list_push(params, *parameter);
        }
        const th_param *rest_parameter = parameter_shape
            ? th_func_rest(parameter_shape) : nullptr;
        if (rest_parameter) {
            rest = *rest_parameter;
            has_rest = true;
        }
        for (size_t i = 0; valid && parameter_shape
             && i < th_func_nkw(parameter_shape); i++) {
            const th_kwparam *keyword = th_func_kw(parameter_shape, i);
            if (!keyword) valid = false;
            else core_list_push(keywords, *keyword);
        }
        parse_tree_t *result = CHILD_NT(grammar, node,
            "tspec-function-result");
        parse_tree_t *return_node = CHILD_NT(grammar, result, "return-type");
        parse_tree_t *error_node = CHILD_NT(grammar, result, "error-type");
        parse_tree_t *return_types = CHILD_NT(grammar, return_node,
                                               "union-tspec");
        parse_tree_t *error_type = CHILD_NT(grammar, error_node, "one-tspec");
        th_type *success = return_node
            ? return_types ? parse_tree_type(return_types) : nullptr
            : th_name(state->types, CORE_STRING_STATIC("nil"));
        th_type *failure = error_node
            ? error_type ? parse_tree_type(error_type) : nullptr
            : th_name(state->types, CORE_STRING_STATIC("nil"));
        th_type *function = valid && success && failure
            ? th_func_with_error(state->types, success, failure,
                params.data, params.len, has_rest ? &rest : nullptr,
                keywords.data, keywords.len) : nullptr;
        core_list_free(params);
        core_list_free(keywords);
        return function;
    }
    if (NT_IS(grammar, node, "tspec-application")) return nullptr;
    if (NT_IS(grammar, node, "base-tspec-core"))
        return compose_base_type_core(grammar, unit, node);
    if (NT_IS(grammar, node, "base-tspec")) {
        parse_tree_t *core = CHILD_NT(grammar, node, "base-tspec-core");
        th_type *type = core ? parse_tree_type(core) : nullptr;
        n00bb_ordered_nodes_t *dimensions = n00bb_node_ordered_nodes(node);
        if (dimensions && dimensions->count) {
            parse_tree_t *name = CHILD_NT(grammar, core, "member-chain");
            if (!member_chain_is_name(grammar, name,
                                      CORE_STRING_STATIC("Array"))
                || !CHILD_NT(grammar, core, "tspec-application")) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0030"),
                    CORE_STRING_STATIC(
                        "array syntax is Array[Element][size]: the first "
                        "square brackets contain a type, and dimensions "
                        "follow (for example, Array[Node][513])"),
                    link_source_location(node));
                return nullptr;
            }
        }
        for (size_t i = 1; type && dimensions && i < dimensions->count; i++)
            type = th_array(state->types, type);
        return type;
    }
    if (NT_IS(grammar, node, "union-tspec")) {
        core_list_t(th_type_ptr_t) arms = core_list_new(th_type_ptr_t);
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *child = core_tree_child(node, i);
            size_t count = parse_tree_is_group(child)
                ? core_tree_num_children(child) : 1;
            for (size_t j = 0; j < count; j++) {
                parse_tree_t *arm = parse_tree_is_group(child)
                    ? core_tree_child(child, j) : child;
                if (!NT_IS(grammar, arm, "one-tspec")) continue;
                th_type *type = parse_tree_type(arm);
                if (!type) {
                    core_list_free(arms);
                    return nullptr;
                }
                if (th_is_nil(type) && core_tree_num_children(node) > 1) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0029"),
                        CORE_STRING_STATIC(
                            "nil cannot be an anonymous union arm; use T?"),
                        link_source_location(arm));
                    core_list_free(arms);
                    return nullptr;
                }
                core_list_push(arms, type);
            }
        }
        th_type *type = arms.len == 1 ? arms.data[0]
            : arms.len ? th_sum(state->types, TH_UNION, arms.data, arms.len)
            : nullptr;
        core_list_free(arms);
        return type;
    }
    return nullptr;
}

void
propagate_resolved_type_reference(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *node, th_type *type)
{
    parse_tree_t *child = node;
    parse_tree_t *parent = parse_tree_parent(child);
    for (; parent; child = parent,
         parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "base-tspec-core")) {
            if (CHILD_NT(grammar, parent, "tspec-application")) return;
        }
        else if (NT_IS(grammar, parent, "base-tspec")) {
            if (CHILD_NT(grammar, parent, "array-dimensions")) return;
        }
        else if (NT_IS(grammar, parent, "one-tspec")) {
            if (has_direct_leaf_text(parent, CORE_STRING_STATIC("?")))
                return;
        }
        else if (!NT_IS(grammar, parent, "type-spec")) break;
        th_type *existing = parse_tree_type(parent);
        n00bb_tu_state_t *state = tu_state(unit);
        if (existing && state && state->types) {
            th_type *unified = th_unify_in_place(state->types, existing,
                                                 type);
            if (!unified) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                    CORE_STRING_STATIC(
                        "resolved type reference conflicts with its cached type"),
                    link_source_location(parent));
                return;
            }
            type = unified;
        }
        parse_tree_set_type(parent, type);
    }

    if (parent && NT_IS(grammar, parent, "vargs-param")) return;
    if (parent && (NT_IS(grammar, parent, "pos-param")
                   || NT_IS(grammar, parent, "default-param"))) {
        parse_sym_entry_t *parameter = parse_tree_sym(parent);
        n00bb_tu_state_t *state = tu_state(unit);
        if (!parameter || !state || !state->types) return;
        th_type *unified = parameter->type
            ? th_unify_in_place(state->types, parameter->type, type) : type;
        if (!unified) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC(
                    "resolved parameter type conflicts with its inferred type"),
                link_source_location(parent));
            return;
        }
        parameter->type = unified;
        parse_tree_set_type(parent, unified);
    }
}

void
refresh_cached_type_path(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = parse_tree_parent(at)) {
        if (core_tree_is_leaf(at)) continue;
        if (NT_IS(grammar, at, "tspec-application")) continue;
        bool type_node = NT_IS(grammar, at, "type-spec")
            || NT_IS(grammar, at, "one-tspec")
            || NT_IS(grammar, at, "base-tspec")
            || NT_IS(grammar, at, "base-tspec-core")
            || NT_IS(grammar, at, "union-tspec")
            || NT_IS(grammar, at, "tspec-list")
            || NT_IS(grammar, at, "tspec-func-param")
            || NT_IS(grammar, at, "tspec-func-params")
            || NT_IS(grammar, at, "tspec-func");
        if (type_node) {
            parse_tree_set_type(at, nullptr);
            cache_type_syntax_node(grammar, at);
            th_type *composed = compose_type_syntax_node(grammar, unit, at);
            if (composed) parse_tree_set_type(at, composed);
            if (NT_IS(grammar, at, "type-spec") && composed) {
                parse_tree_t *parent = parse_tree_parent(at);
                parse_sym_entry_t *field = parent
                    && NT_IS(grammar, parent, "type-decl-one")
                    ? parse_tree_sym(parent) : nullptr;
                if (field && field->is_field) {
                    n00bb_tu_state_t *state = tu_state(unit);
                    th_type *bound = field->type
                        ? th_unify_in_place(state->types, field->type,
                                            composed)
                        : composed;
                    if (!bound) {
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(unit), unit,
                            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                            CORE_STRING_STATIC(
                                "field type conflicts with its resolved declaration"),
                            link_source_location(parent));
                        return;
                    }
                    field->type = bound;
                    parse_tree_set_type(parent, bound);
                }
            }
        }
        else if (at != node && !parse_tree_is_group(at))
            break;
    }
}

