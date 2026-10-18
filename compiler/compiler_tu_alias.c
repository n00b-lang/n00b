#include "compiler_tu.h"

static bool
type_holds_reference(th_type *type)
{
    th_type *held = type ? th_resolve(type) : nullptr;
    while (held && th_builtin_kind_of(held) == TH_BUILTIN_OPTION) {
        const th_arg *payload = th_named_arg(held, 0);
        held = payload && payload->kind == TH_ARG_TYPE
            ? th_resolve(payload->type) : nullptr;
    }
    return !held || th_kind_of(held) == TH_INDIRECTION
        || th_kind_of(held) == TH_VAR;
}

static bool
alias_component_add(core_list_t(parse_sym_entry_ptr_t) *symbols,
                    parse_sym_entry_t *symbol)
{
    if (!symbols || !symbol) return false;
    for (size_t i = 0; i < symbols->len; i++)
        if (symbols->data[i] == symbol) return true;
    core_list_push(*symbols, symbol);
    return true;
}

static bool
local_alias_component_is_closed(parse_grammar_t *grammar,
                                parse_sym_entry_t *root,
                                parse_scope_t *owner,
                                core_list_t(parse_sym_entry_ptr_t) *component)
{
    if (!grammar || !root || !owner || !component) return false;
    core_list_push(*component, root);
    for (size_t i = 0; i < component->len; i++) {
        parse_sym_entry_t *symbol = component->data[i];
        if (!symbol || !symbol->type) return false;
        if (is_module_global_symbol(grammar, symbol)) continue;
        if (symbol->lifetime.escapes
            || (symbol->kind != PARSE_SYM_VARIABLE
             && symbol->kind != PARSE_SYM_PARAM)
            || lexical_function_scope(symbol->scope) != owner) return false;
        for (size_t direction = 0; direction < 2; direction++) {
            parse_sym_alias_t *edges = direction ? symbol->aliased_by
                                                  : symbol->aliases;
            size_t edge_count = direction ? symbol->aliased_by_count
                                          : symbol->alias_count;
            for (size_t j = 0; j < edge_count; j++) {
                parse_sym_alias_t *edge = &edges[j];
                if (!edge->other || edge->escapes
                    || edge->kind == PARSE_SYM_ALIAS_MAY
                    || !edge->other->type
                    || !alias_component_add(component, edge->other))
                    return false;
                if (!is_module_global_symbol(grammar, edge->other)
                    && lexical_function_scope(edge->other->scope) != owner)
                    return false;
            }
        }
    }
    return true;
}


bool
finalize_alias_free_local_summaries(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return false;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        if (!scope_has_lexical_lifetime(scope)) continue;
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            bool local_value = (symbol->kind == PARSE_SYM_VARIABLE
                                || symbol->kind == PARSE_SYM_PARAM)
                && symbol->type;
            if (!local_value) continue;
            if (!type_may_contain_reference_alias(state->types,
                                                  symbol->type)) {
                parse_sym_set_alias_analysis_complete(symbol, true);
                continue;
            }
            if (symbol->lifetime.escapes
                || (!symbol->alias_count && !symbol->aliased_by_count))
                continue;
            parse_scope_t *owner = lexical_function_scope(symbol->scope);
            core_list_t(parse_sym_entry_ptr_t) component =
                core_list_new(parse_sym_entry_ptr_t);
            bool complete = local_alias_component_is_closed(
                grammar, symbol, owner, &component);
            if (complete)
                for (size_t i = 0; i < component.len; i++)
                    parse_sym_set_alias_analysis_complete(
                        component.data[i], true);
            core_list_free(component);
        }
    }
    return true;
}

static bool
reference_is_simple_assignment(parse_grammar_t *grammar,
                               parse_tree_t *reference)
{
    for (parse_tree_t *at = reference; at;
         at = parse_tree_parent(at)) {
        if (nt_is(grammar, at, "assign-stmt")
            || nt_is(grammar, at, "sym-decl")) {
            const parse_rule_t *rule = parse_tree_rule(grammar, at);
            parse_annotation_t *assign = rule
                ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
                : nullptr;
            parse_tree_t *value = assign
                ? parse_tree_resolve_child_ref(grammar, at,
                                               assign->value_ref) : nullptr;
            return value && direct_ref_expression(grammar, value)
                && node_is_within(reference, value);
        }
        if (nt_is(grammar, at, "func-def")
            || nt_is(grammar, at, "context-def")) return false;
    }
    return false;
}

static bool
reference_is_call_argument(parse_grammar_t *grammar,
                           parse_tree_t *reference)
{
    for (parse_tree_t *at = reference; at;
         at = parse_tree_parent(at)) {
        if (nt_is(grammar, at, "func-def")
            || nt_is(grammar, at, "context-def")) return false;
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        parse_annotation_t *call = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL) : nullptr;
        if (call && parse_child_ref_is_set(call->type_ref)) {
            parse_tree_t *arguments = parse_tree_resolve_child_ref(
                grammar, at, call->type_ref);
            if (node_is_within(reference, arguments)) return true;
        }
    }
    return false;
}

static void
mark_expression_symbols_may_escape(n00bb_tu_state_t *state,
                                   parse_tree_t *node)
{
    n00bb_alias_edges_t *facts = state && node
        ? n00bb_node_escape_edges(node) : nullptr;
    for (size_t i = 0; facts && i < facts->count; i++) {
        parse_sym_entry_t *symbol = facts->edges[i].symbol;
        if (symbol && (symbol->kind == PARSE_SYM_VARIABLE
                       || symbol->kind == PARSE_SYM_PARAM))
            symbol->lifetime.escapes = true;
    }
}

void
mark_alias_symbol_may_escape(th_ctx *types, parse_sym_entry_t *symbol)
{
    if (!symbol || (!is_reference_symbol(symbol) && !symbol->alias_count
        && !type_may_contain_reference_alias(types, symbol->type)))
        return;
    core_list_t(parse_sym_entry_ptr_t) work =
        core_list_new(parse_sym_entry_ptr_t);
    core_list_push(work, symbol);
    for (size_t at = 0; at < work.len; at++) {
        parse_sym_entry_t *current = work.data[at];
        current->lifetime.escapes = true;
        for (size_t i = 0; i < current->alias_count; i++) {
            parse_sym_alias_t edge = current->aliases[i];
            if (!edge.other) continue;
            edge.other->lifetime.escapes = true;
            parse_symtab_add_alias(current, edge.other, edge.kind,
                                   edge.derived_at, true);
            bool seen = false;
            for (size_t j = 0; j < work.len; j++)
                seen |= work.data[j] == edge.other;
            if (!seen) core_list_push(work, edge.other);
        }
        for (size_t i = 0; i < current->aliased_by_count; i++) {
            parse_sym_alias_t edge = current->aliased_by[i];
            if (!edge.other) continue;
            edge.other->lifetime.escapes = true;
            parse_symtab_add_alias(edge.other, current, edge.kind,
                                   edge.derived_at, true);
            bool seen = false;
            for (size_t j = 0; j < work.len; j++)
                seen |= work.data[j] == edge.other;
            if (!seen) core_list_push(work, edge.other);
        }
    }
    core_list_free(work);
}

void
mark_referent_value_may_escape(th_ctx *types,
                               parse_sym_entry_t *symbol)
{
    if (!symbol) return;
    th_type *held = symbol->type ? th_resolve(symbol->type) : nullptr;
    if (held && th_kind_of(held) == TH_INDIRECTION)
        held = th_ind_referent(held);
    if (type_may_contain_reference_alias(types, held))
        symbol->lifetime.referent_value_escapes = true;
    core_list_t(parse_sym_entry_ptr_t) work =
        core_list_new(parse_sym_entry_ptr_t);
    for (size_t i = 0; i < symbol->alias_count; i++) {
        parse_sym_entry_t *target = symbol->aliases[i].other;
        if (target && target != symbol) core_list_push(work, target);
    }
    for (size_t at = 0; at < work.len; at++) {
        parse_sym_entry_t *current = work.data[at];
        current->lifetime.escapes = true;
        for (size_t i = 0; i < current->alias_count; i++) {
            parse_sym_entry_t *target = current->aliases[i].other;
            if (!target || target == symbol) continue;
            bool seen = false;
            for (size_t j = 0; j < work.len; j++)
                seen |= work.data[j] == target;
            if (!seen) core_list_push(work, target);
        }
    }
    core_list_free(work);
}

parse_sym_entry_t *
cached_argument_value_source(n00bb_tu_state_t *state, parse_tree_t *argument)
{
    n00bb_alias_edges_t *facts = state && argument
        ? n00bb_node_alias_edges(argument) : nullptr;
    if (!facts || facts->count != 1) return nullptr;
    n00bb_alias_edge_t *edge = &facts->edges[0];
    return edge->kind == N00BB_ALIAS_REFERENCE
        || edge->kind == N00BB_ALIAS_SYMBOL ? edge->symbol : nullptr;
}

void
mark_expression_aliases_may_escape(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *result = state && node
        ? n00bb_node_alias_edges(node) : nullptr;
    if (result && result->count == 1
        && result->edges[0].kind == N00BB_ALIAS_CALL) {
        parse_tree_t *call = result->edges[0].node;
        parse_sym_entry_t *callee = call && core_tree_num_children(call)
            ? bound_function_symbol(core_tree_child(call, 0))
            : nullptr;
        if (n00bb_builtin_reference_function(callee)
            && core_string_eq(callee->name,
                              CORE_STRING_STATIC("ref_read"))) {
            parse_tree_t *arguments = call_arguments_node(grammar, call);
            core_list_t(n00bb_tree_ptr_t) positional =
                core_list_new(n00bb_tree_ptr_t);
            collect_positional_nodes(grammar,
                child_nt(grammar, arguments, "positional-args"),
                &positional);
            parse_tree_t *argument = positional.len
                ? positional.data[0] : nullptr;
            parse_sym_entry_t *referent = cached_argument_value_source(
                state, argument);
            core_list_free(positional);
            if (referent) {
                mark_referent_value_may_escape(state->types, referent);
                return;
            }
        }
    }
    n00bb_alias_edges_t *facts = state && node
        ? n00bb_node_escape_edges(node) : nullptr;
    if (!facts) return;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        parse_sym_entry_t *symbol = edge->symbol;
        if (!symbol) continue;
        if (edge->kind == N00BB_ALIAS_REFERENCE) {
            symbol->lifetime.escapes = true;
            mark_referent_value_may_escape(state->types, symbol);
            continue;
        }
        if (type_may_contain_reference_alias(state->types, symbol->type))
            mark_referent_value_may_escape(state->types, symbol);
    }
}

parse_tree_t *
aggregate_literal_root(parse_grammar_t *grammar, parse_tree_t *expression)
{
    parse_tree_t *node = expression;
    while (node && !core_tree_is_leaf(node)) {
        if (nt_is(grammar, node, "list-lit")
            || nt_is(grammar, node, "dict-or-set-lit")) return node;
        if (nt_is(grammar, node, "tuple-or-paren")) {
            if (child_nt(grammar, node, "named-field-list")) return node;
            for (size_t i = 0; i < core_tree_num_children(node); i++)
                if (core_tree_is_leaf(core_tree_child(node, i))
                    && parse_tree_leaf_is(core_tree_child(node, i),
                                          CORE_STRING_STATIC(",")))
                    return node;
            return nullptr;
        }
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

bool
record_aggregate_aliases(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         parse_tree_t *node, parse_sym_entry_t *holder)
{
    (void)grammar;
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *facts = state
        ? n00bb_node_alias_edges(node) : nullptr;
    if (!facts) return true;
    bool complete = true;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t edge = facts->edges[i];
        if (edge.kind == N00BB_ALIAS_CALL) continue;
        parse_sym_entry_t *source = edge.symbol;
        if (edge.kind == N00BB_ALIAS_REFERENCE) {
            if (!source) { complete = false; continue; }
            bool escapes = scope_has_lexical_lifetime(source->scope)
                && !scope_is_within(holder->scope, source->scope);
            complete &= parse_symtab_add_alias(holder, source,
                PARSE_SYM_ALIAS_MAY, edge.derived_at, escapes);
            continue;
        }
        bool unresolved = source && source->type
            && th_kind_of(th_resolve(source->type)) == TH_VAR;
        if (!source || (!is_reference_symbol(source)
                        && !source->alias_count && !unresolved)) continue;
        if (!source->alias_count) {
            complete &= parse_symtab_add_alias(holder, source,
                PARSE_SYM_ALIAS_MAY, edge.derived_at,
                source->lifetime.escapes);
            continue;
        }
        for (size_t j = 0; j < source->alias_count; j++) {
            const parse_sym_alias_t *alias = parse_sym_alias_at(source, j);
            if (!alias || !alias->other) { complete = false; continue; }
            bool escapes = alias->escapes
                || (scope_has_lexical_lifetime(alias->other->scope)
                    && !scope_is_within(holder->scope, alias->other->scope));
            complete &= parse_symtab_add_alias(holder, alias->other,
                PARSE_SYM_ALIAS_MAY, alias->derived_at, escapes);
        }
    }
    return complete;
}

static void
mark_return_value_aliases_may_escape(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *facts = state && node
        ? n00bb_node_alias_edges(node) : nullptr;
    if (!facts) return;
    th_type *returned = parse_tree_type(node);
    if (returned && !type_may_contain_reference_alias(state->types, returned))
        return;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        parse_sym_entry_t *source = edge->symbol;
        bool unresolved = source && source->type
            && th_kind_of(th_resolve(source->type)) == TH_VAR;
        if ((edge->kind == N00BB_ALIAS_CALL
             && type_may_contain_reference_alias(state->types,
                 parse_tree_type(edge->node)))
            || edge->kind == N00BB_ALIAS_REFERENCE
            || (source && (is_reference_symbol(source)
                           || source->alias_count || unresolved)))
            mark_expression_aliases_may_escape(grammar, unit, edge->node);
    }
}

void
record_reference_facts(parse_grammar_t *grammar,
                       core_translation_unit_t *unit, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return;

    n00bb_tu_state_t *state = tu_state(unit);
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    if (nt_is(grammar, node, "return-stmt"))
        mark_return_value_aliases_may_escape(
            grammar, unit, child_nt(grammar, node, "expression"));

    if (nt_is(grammar, node, "unary-expr")
        && core_string_eq(first_token_text(node), CORE_STRING_STATIC("ref"))
        && !reference_is_simple_assignment(grammar, node)
        && !reference_is_call_argument(grammar, node)) {
        parse_sym_entry_t *target = direct_ref_target(grammar, node);
        if (target) target->lifetime.escapes = true;
        else
            mark_expression_symbols_may_escape(state, node);
    }

    bool declaration = nt_is(grammar, node, "sym-decl");
    bool assignment = nt_is(grammar, node, "assign-stmt")
        || nt_is(grammar, node, "binop-assign-stmt");
    if (!declaration && !assignment) return;
    parse_annotation_t *assign = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS) : nullptr;
    parse_tree_t *target_node = assignment && assign
        ? parse_tree_resolve_child_ref(grammar, node, assign->name_ref)
        : nullptr;
    parse_tree_t *value_node = assign
        ? parse_tree_resolve_child_ref(grammar, node, assign->value_ref)
        : nullptr;
    parse_tree_t *target_lvalue = n00bb_simple_lvalue(target_node);
    parse_sym_entry_t *holder = declaration
        ? parse_tree_sym(node)
        : target_lvalue && !member_access_node(grammar, target_node)
            ? parse_tree_sym(target_lvalue) : nullptr;
    if (!holder || (holder->kind != PARSE_SYM_VARIABLE
                    && holder->kind != PARSE_SYM_PARAM)) {
        mark_expression_aliases_may_escape(grammar, unit, value_node);
        return;
    }
    if (value_node) parse_sym_add_definition(holder, node);
    parse_tree_t *direct_reference = direct_ref_expression(grammar,
                                                            value_node);
    parse_sym_entry_t *referent = direct_reference
        ? direct_ref_target(grammar, direct_reference) : nullptr;
    parse_token_info_t *site = parse_tree_find_first_terminal(value_node);
    uint64_t derivation = site ? site->start_byte : 0;
    if (referent) {
        if (holder == referent) return;
        bool outlives_target = scope_has_lexical_lifetime(referent->scope)
            && !scope_is_within(holder->scope, referent->scope);
        parse_symtab_add_alias(holder, referent, PARSE_SYM_ALIAS_DERIVED,
                               derivation, outlives_target);
        return;
    }

    parse_tree_t *aggregate = aggregate_literal_root(grammar, value_node);
    if (aggregate) {
        if (!record_aggregate_aliases(grammar, unit, aggregate, holder))
            holder->lifetime.escapes = true;
        if (holder->lifetime.escapes && holder->alias_count)
            mark_alias_symbol_may_escape(state->types, holder);
        return;
    }

    n00bb_alias_edges_t *value_aliases = state && value_node
        ? n00bb_node_alias_edges(value_node) : nullptr;
    th_type *value_type = value_node
        ? parse_tree_type(value_node) : nullptr;
    if (value_aliases
        && n00bb_callback_contract_for_type(state, value_type)) {
        if (!record_aggregate_aliases(grammar, unit, value_node, holder))
            holder->lifetime.escapes = true;
        if (holder->lifetime.escapes && holder->alias_count)
            mark_alias_symbol_may_escape(state->types, holder);
        return;
    }

    n00bb_return_source_t *value_source = state && value_node
        ? n00bb_node_return_source(value_node) : nullptr;
    if (value_source && value_source->kind == N00BB_RETURN_SOURCE_PROJECTION)
        return;

    parse_sym_entry_t *source = simple_expression_symbol(value_node);
    bool unresolved_source_type = source && source->type
        && th_kind_of(th_resolve(source->type)) == TH_VAR;
    if (!source || source == holder
        || (!is_reference_symbol(source) && !source->alias_count
            && !unresolved_source_type)) {
        for (size_t i = 0; value_aliases && i < value_aliases->count; i++)
            if (value_aliases->edges[i].kind == N00BB_ALIAS_CALL) return;
        mark_expression_aliases_may_escape(grammar, unit, value_node);
        return;
    }
    if (is_reference_symbol(source) && !unresolved_source_type
        && holder->type && !type_holds_reference(holder->type)) {
        for (size_t i = 0; i < parse_sym_alias_count(source); i++) {
            const parse_sym_alias_t *known = parse_sym_alias_at(source, i);
            parse_sym_entry_t *referent = known ? known->other : nullptr;
            if (!referent) {
                holder->lifetime.escapes = true;
                continue;
            }
            for (size_t j = 0; j < parse_sym_alias_count(referent); j++) {
                const parse_sym_alias_t *content =
                    parse_sym_alias_at(referent, j);
                if (!content || !content->other) {
                    holder->lifetime.escapes = true;
                    continue;
                }
                if (content->other == holder) continue;
                bool outlives = scope_has_lexical_lifetime(
                        content->other->scope)
                    && !scope_is_within(holder->scope, content->other->scope);
                parse_symtab_add_alias(holder, content->other, content->kind,
                    derivation, content->escapes || outlives);
            }
        }
        if (!source->alias_count
            && type_may_contain_reference_alias(state->types, holder->type))
            parse_sym_set_alias_analysis_complete(holder, false);
        return;
    }
    if (source->lifetime.escapes) holder->lifetime.escapes = true;
    for (size_t i = 0; i < parse_sym_alias_count(source); i++) {
        const parse_sym_alias_t *known = parse_sym_alias_at(source, i);
        if (!known || !known->other) {
            holder->lifetime.escapes = true;
            continue;
        }
        bool outlives_target = scope_has_lexical_lifetime(known->other->scope)
            && !scope_is_within(holder->scope, known->other->scope);
        parse_symtab_add_alias(holder, known->other, known->kind,
            derivation, known->escapes || source->lifetime.escapes
                || outlives_target);
    }
    if (!source->alias_count) {
        bool outlives_source = scope_has_lexical_lifetime(source->scope)
            && !scope_is_within(holder->scope, source->scope);
        parse_symtab_add_alias(holder, source, PARSE_SYM_ALIAS_MAY,
            derivation, source->lifetime.escapes || outlives_source);
    }
}
