#include <string.h>

#include "compiler_link.h"
#include "compiler_link_lifetime.h"

static const uint8_t *
interface_function_lifetime_metadata(n00bb_link_analysis_t *analysis,
                                     const char *qualified_name,
                                     size_t *size)
{
    if (size) *size = 0;
    const th_env *environment = analysis
        ? analysis->interface_environment : nullptr;
    for (size_t i = 0; environment
         && i < th_env_metadata_count(environment); i++) {
        th_env_metadata_view_t metadata = {0};
        if (!th_env_metadata_at(environment, i, &metadata)
            || metadata.owner_kind != TH_ENV_METADATA_FUNCTION
            || strcmp(metadata.owner_name, qualified_name)
            || strcmp(metadata.key, "n00b.function-lifetime")
            || metadata.version != 1) continue;
        if (size) *size = metadata.size;
        return metadata.data;
    }
    for (size_t e = 0; analysis && e < analysis->linked_interface_count; e++) {
        const th_env *environment = analysis->linked_interfaces[e];
        for (size_t i = 0; i < th_env_metadata_count(environment); i++) {
            th_env_metadata_view_t metadata = {0};
            if (!th_env_metadata_at(environment, i, &metadata)
                || metadata.owner_kind != TH_ENV_METADATA_FUNCTION
                || strcmp(metadata.owner_name, qualified_name)
                || strcmp(metadata.key, "n00b.function-lifetime")
                || metadata.version != 1) continue;
            if (size) *size = metadata.size;
            return metadata.data;
        }
    }
    return nullptr;
}

static bool
decode_function_lifetime_effects(const uint8_t *data, size_t size,
                                 parse_sym_entry_t *function,
                                 n00bb_function_lifetime_effects_t *out)
{
    th_type *signature = function && function->type
        ? th_resolve(function->type) : nullptr;
    size_t expected = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_nparams(signature) + (th_func_rest(signature) != nullptr)
        : 0;
    if (!data || !out || size < 16 || memcmp(data, "N0FL", 4)
        || data[4] != 1 || !signature || th_kind_of(signature) != TH_FUNCTION)
        return false;
    uint32_t count = read_u32_le(data + 5);
    if (count != expected || count > size - 16) return false;
    size_t offset = 9 + count;
    uint8_t return_kind = data[offset++];
    uint32_t return_parameter = read_u32_le(data + offset);
    offset += 4;
    uint16_t global_size = (uint16_t)data[offset]
        | (uint16_t)data[offset + 1] << 8;
    offset += 2;
    if (return_kind > N00BB_RETURN_ALIAS_FRESH_EXTERNAL
        || offset + global_size != size
        || ((return_kind == N00BB_RETURN_ALIAS_PARAMETER)
            && return_parameter >= count)
        || (return_kind != N00BB_RETURN_ALIAS_PARAMETER
            && return_parameter != UINT32_MAX)
        || ((return_kind == N00BB_RETURN_ALIAS_GLOBAL) != (global_size != 0)))
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (data[9 + i] > N00BB_PARAM_EFFECT_BOTH_ESCAPE) return false;
    *out = (n00bb_function_lifetime_effects_t){
        .parameter_effects = data + 9,
        .parameter_count = count,
        .return_kind = (n00bb_return_alias_kind_t)return_kind,
        .return_parameter = return_parameter,
        .return_global = global_size ? (const char *)data + offset : nullptr,
        .return_global_size = global_size,
    };
    return true;
}

bool
function_lifetime_effects_for_symbol(n00bb_link_analysis_t *analysis,
                                     parse_sym_entry_t *function,
                                     n00bb_function_lifetime_effects_t *out)
{
    if (!analysis || !function || !out) return false;
    if (n00bb_builtin_byte_offset_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 2,
            .return_kind = N00BB_RETURN_ALIAS_PARAMETER,
            .return_parameter = 0,
        };
        return true;
    }
    if (n00bb_builtin_byte_diff_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 2,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_gc_memory_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 3,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_volatile_load_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 1,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_prefetch_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 1,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_atomic_rmw_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 2,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_atomic_cas_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = 3,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_atomic_access_function(function)) {
        static const uint8_t effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = effects,
            .parameter_count = core_string_eq(function->name,
                    CORE_STRING_STATIC("__atomic_load"))
                || core_string_eq(function->name,
                    CORE_STRING_STATIC("__atomic_load_relaxed"))
                || core_string_eq(function->name,
                    CORE_STRING_STATIC("__atomic_load_acquire")) ? 1 : 2,
            .return_kind = N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_native_function(function)) {
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_count = th_func_nparams(function->type),
            .return_kind = core_string_eq(function->name,
                CORE_STRING_STATIC("__os_map_zeroed"))
                ? N00BB_RETURN_ALIAS_FRESH_EXTERNAL
                : N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    if (n00bb_builtin_reference_function(function)) {
        static const uint8_t read_effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
        };
        static const uint8_t write_effects[] = {
            N00BB_PARAM_EFFECT_NO_ESCAPE,
            N00BB_PARAM_EFFECT_MAY_ESCAPE,
        };
        bool read = core_string_eq(function->name,
                                   CORE_STRING_STATIC("ref_read"));
        *out = (n00bb_function_lifetime_effects_t){
            .parameter_effects = read ? read_effects : write_effects,
            .parameter_count = read ? 1 : 2,
            .return_kind = read ? N00BB_RETURN_ALIAS_UNKNOWN
                                : N00BB_RETURN_ALIAS_NONE,
            .return_parameter = UINT32_MAX,
        };
        return true;
    }
    core_translation_unit_t *unit = unit_for_symbol(analysis->grammar,
        &analysis->units, function);
    if (!unit) return false;
    core_buffer_t qualified = {0};
    core_buffer_printf(&qualified, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes,
        unit->module.identifier.data,
        (int)function->name.u8_bytes, function->name.data);
    size_t size = 0;
    const uint8_t *data = interface_function_lifetime_metadata(analysis,
        qualified.data, &size);
    bool ok = data && decode_function_lifetime_effects(data, size, function,
                                                       out);
    core_buffer_release(&qualified);
    return ok;
}

typedef struct {
    parse_sym_entry_t *function;
    core_translation_unit_t *unit;
    n00bb_function_return_facts_t *facts;
    parse_sym_entry_t **formals;
    bool *possible;
    size_t formal_count;
} n00bb_return_candidate_t;
core_list_decl(n00bb_return_candidate_t);

bool simple_returned_global(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    parse_tree_t *expression,
    core_list_t(parse_sym_entry_ptr_t) *visited,
    core_buffer_t *global_name);

static n00bb_return_candidate_t *
return_candidate_for_symbol(core_list_t(n00bb_return_candidate_t) *candidates,
                            parse_sym_entry_t *function)
{
    for (size_t i = 0; i < candidates->len; i++)
        if (candidates->data[i].function == function)
            return &candidates->data[i];
    return nullptr;
}

static bool
returned_source_matches_formal(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis,
    core_list_t(n00bb_return_candidate_t) *candidates,
    n00bb_return_candidate_t *owner, parse_tree_t *expression,
    size_t formal_index)
{
    if (!expression || formal_index >= owner->formal_count)
        return false;
    n00bb_tu_state_t *state = tu_state(owner->unit);
    n00bb_return_source_t *source = state
        ? n00bb_node_return_source(expression) : nullptr;
    if (!source) return false;
    if (source->kind == N00BB_RETURN_SOURCE_PROJECTION_REFERENCE) {
        parse_tree_t *receiver = source->node;
        th_type *type = receiver
            ? th_resolve(core_tree_node_value(receiver).type) : nullptr;
        return type && th_kind_of(type) == TH_INDIRECTION
            && returned_source_matches_formal(grammar, analysis, candidates,
                owner, receiver, formal_index);
    }
    if (source->kind == N00BB_RETURN_SOURCE_SYMBOL) {
        parse_sym_entry_t *symbol = source->symbol;
        if (symbol == owner->formals[formal_index]) return true;
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
            || (symbol->mutability != PARSE_SYM_IMMUTABLE
                && symbol->mutability != PARSE_SYM_CONST)
            || symbol->definition_count != 1) return false;
        parse_tree_t *initializer = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.initializer-expression"));
        return initializer && initializer != expression
            && returned_source_matches_formal(grammar, analysis, candidates,
                owner, initializer, formal_index);
    }

    parse_tree_t *call = source->kind == N00BB_RETURN_SOURCE_CALL
            || source->kind == N00BB_RETURN_SOURCE_SLICE
        ? source->node : nullptr;
    if (!call || !core_tree_num_children(call)) return false;
    core_string_t index_trait = {0}, index_method = {0};
    if (index_dispatch_for_node(grammar, call, &index_trait, &index_method)) {
        return core_string_eq(index_trait,
                CORE_STRING_STATIC("Index_Slice"))
            && returned_source_matches_formal(grammar, analysis, candidates,
                owner, core_tree_child(call, 0), formal_index);
    }

    parse_tree_t *member = member_access_node(grammar, call);
    parse_sym_entry_t *callee = source->symbol;
    if (!callee || callee->kind != PARSE_SYM_FUNCTION) return false;
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    bool trait_dispatch = member
        && !call_receiver_is_module(grammar, owner->unit, receiver);
    n00bb_return_candidate_t *target = return_candidate_for_symbol(
        candidates, callee);
    if (target) {
        for (size_t i = 0; i < target->formal_count; i++) {
            if (!target->possible[i]) continue;
            parse_tree_t *argument = call_argument_for_parameter(grammar,
                call, callee, i, trait_dispatch);
            if (returned_source_matches_formal(grammar, analysis,
                    candidates, owner, argument, formal_index))
                return true;
        }
        return false;
    }
    if (callee->decl_node && nt_is(grammar, callee->decl_node, "func-def"))
        return false;
    n00bb_function_lifetime_effects_t effects = {0};
    if (!function_lifetime_effects_for_symbol(analysis, callee, &effects)
        || effects.return_kind != N00BB_RETURN_ALIAS_PARAMETER) return false;
    parse_tree_t *argument = call_argument_for_parameter(grammar, call,
        callee, effects.return_parameter, trait_dispatch);
    return returned_source_matches_formal(grammar, analysis, candidates,
        owner, argument, formal_index);
}

static void
compute_returned_parameters(parse_grammar_t *grammar,
                            n00bb_link_analysis_t *analysis)
{
    static const core_string_t facts_key =
        CORE_STRING_STATIC("n00bb.function.return-facts");
    core_list_t(n00bb_return_candidate_t) candidates =
        core_list_new(n00bb_return_candidate_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) continue;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION
                    || !symbol->decl_node
                    || !nt_is(grammar, symbol->decl_node, "func-def")
                    || return_candidate_for_symbol(&candidates, symbol))
                    continue;
                n00bb_function_return_facts_t *facts = parse_sym_binding(
                    symbol, facts_key);
                if (!facts || !facts->return_expression_count) continue;
                parse_tree_t *signature = child_nt(grammar,
                    symbol->decl_node, "func-type-signature");
                parse_tree_t *params = child_nt(grammar, signature,
                                                "param-decl");
                core_list_t(parse_sym_entry_ptr_t) formals =
                    core_list_new(parse_sym_entry_ptr_t);
                parse_sym_entry_t *rest = nullptr;
                collect_ir_parameters(grammar, params, &formals, &rest);
                if (formals.len) {
                    n00bb_return_candidate_t candidate = {
                        .function = symbol,
                        .unit = unit,
                        .facts = facts,
                        .formals = core_alloc_array(parse_sym_entry_t *,
                                                    formals.len),
                        .possible = core_alloc_array(bool, formals.len),
                        .formal_count = formals.len,
                    };
                    memcpy(candidate.formals, formals.data,
                           formals.len * sizeof(*candidate.formals));
                    for (size_t i = 0; i < formals.len; i++)
                        candidate.possible[i] = true;
                    core_list_push(candidates, candidate);
                }
                core_list_free(formals);
                (void)rest;
            }
        }
    }

    bool changed;
    do {
        changed = false;
        for (size_t f = 0; f < candidates.len; f++) {
            n00bb_return_candidate_t *candidate = &candidates.data[f];
            for (size_t i = 0; i < candidate->formal_count; i++) {
                if (!candidate->possible[i]) continue;
                for (size_t r = 0;
                     r < candidate->facts->return_expression_count; r++) {
                    if (returned_source_matches_formal(grammar, analysis,
                            &candidates, candidate,
                            candidate->facts->return_expressions[r], i))
                        continue;
                    candidate->possible[i] = false;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);

    for (size_t f = 0; f < candidates.len; f++) {
        n00bb_return_candidate_t *candidate = &candidates.data[f];
        size_t selected = SIZE_MAX;
        for (size_t i = 0; i < candidate->formal_count; i++) {
            if (!candidate->possible[i]) continue;
            if (selected != SIZE_MAX) {
                selected = SIZE_MAX;
                break;
            }
            selected = i;
        }
        candidate->facts->returned_parameter = selected;
    }
    for (size_t i = 0; i < candidates.len; i++) {
        core_free(candidates.data[i].formals);
        core_free(candidates.data[i].possible);
    }
    core_list_free(candidates);
    analysis->returned_parameters_computed = true;
}

bool
function_returned_parameter(parse_grammar_t *grammar,
                            n00bb_link_analysis_t *analysis,
                            parse_sym_entry_t *function,
                            size_t *formal_index)
{
    if (!grammar || !analysis || !function) return false;
    if (!function->decl_node
        || !nt_is(grammar, function->decl_node, "func-def")) {
        n00bb_function_lifetime_effects_t effects = {0};
        if (!function_lifetime_effects_for_symbol(analysis, function,
                                                   &effects)
            || effects.return_kind != N00BB_RETURN_ALIAS_PARAMETER)
            return false;
        if (formal_index) *formal_index = effects.return_parameter;
        return true;
    }
    if (!analysis->returned_parameters_computed)
        compute_returned_parameters(grammar, analysis);
    n00bb_function_return_facts_t *facts = parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.function.return-facts"));
    if (!facts || !facts->return_expression_count
        || facts->returned_parameter == SIZE_MAX) return false;
    if (formal_index) *formal_index = facts->returned_parameter;
    return true;
}

static bool
tree_depth_from(parse_tree_t *node, parse_tree_t *ancestor, size_t *depth)
{
    size_t count = 0;
    for (parse_tree_t *at = node; at;
         at = core_tree_node_value(at).parent) {
        if (at == ancestor) {
            if (depth) *depth = count;
            return true;
        }
        count++;
    }
    return false;
}

static size_t
tree_child_index(parse_tree_t *parent, parse_tree_t *child)
{
    for (size_t i = 0; i < core_tree_num_children(parent); i++)
        if (core_tree_child(parent, i) == child) return i;
    return SIZE_MAX;
}

static bool
call_precedes_in_walk(parse_tree_t *candidate, parse_tree_t *current,
                      parse_tree_t *root)
{
    size_t candidate_depth = 0, current_depth = 0;
    if (!tree_depth_from(candidate, root, &candidate_depth)
        || !tree_depth_from(current, root, &current_depth)) return false;

    parse_tree_t *a = candidate, *b = current;
    size_t original_candidate_depth = candidate_depth;
    size_t original_current_depth = current_depth;
    while (candidate_depth > current_depth) {
        a = core_tree_node_value(a).parent;
        candidate_depth--;
    }
    while (current_depth > candidate_depth) {
        b = core_tree_node_value(b).parent;
        current_depth--;
    }
    if (a == b)
        return original_candidate_depth < original_current_depth;
    while (core_tree_node_value(a).parent
           != core_tree_node_value(b).parent) {
        a = core_tree_node_value(a).parent;
        b = core_tree_node_value(b).parent;
        if (!a || !b) return false;
    }
    return tree_child_index(core_tree_node_value(a).parent, a)
        > tree_child_index(core_tree_node_value(b).parent, b);
}

static parse_tree_t *
find_call_expression(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    parse_tree_t *expression)
{
    (void)grammar;
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *facts = state && expression
        ? n00bb_node_alias_edges(expression) : nullptr;
    if (!state || !expression) return nullptr;

    parse_tree_t *best = nullptr;
    for (size_t i = 0; facts && i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        if (edge->kind != N00BB_ALIAS_CALL) continue;
        if (!best || call_precedes_in_walk(edge->node, best, expression))
            best = edge->node;
    }
    for (size_t i = 0; i < state->link_use_sites.len; i++) {
        n00bb_link_use_site_t *site = &state->link_use_sites.data[i];
        if (!core_string_eq(site->dispatch_trait,
                            CORE_STRING_STATIC("Index_Slice"))
            || !node_is_within(site->node, expression)) continue;
        if (!best || call_precedes_in_walk(site->node, best, expression))
            best = site->node;
    }
    return best;
}

static bool record_returned_call_alias(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    parse_sym_entry_t *holder, parse_tree_t *expression,
    core_list_t(parse_tree_ptr_t) *active);
static bool record_call_aggregate_aliases(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    parse_sym_entry_t *holder, parse_tree_t *expression,
    core_list_t(parse_tree_ptr_t) *active);


static bool
call_argument_aliases(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_tree_t *argument, parse_sym_entry_t *holder)
{
    if (!argument || !holder) return false;
    if (core_string_eq(first_token_text(argument),
                       CORE_STRING_STATIC("ref"))) {
        parse_sym_entry_t *referent = direct_ref_target(grammar, argument);
        if (!referent) return false;
        bool escapes = scope_has_lexical_lifetime(referent->scope)
            && !scope_is_within(holder->scope, referent->scope);
        parse_token_info_t *site = parse_tree_find_first_terminal(argument);
        return parse_symtab_add_alias(holder, referent, PARSE_SYM_ALIAS_MAY,
            site ? site->start_byte : 0, escapes);
    }

    parse_sym_entry_t *source = simple_expression_symbol(argument);
    if (source && (is_reference_symbol(source) || source->alias_count
                   || (source->type && th_kind_of(
                       th_resolve(source->type)) == TH_VAR))) {
        if (!source->alias_count) {
            parse_token_info_t *site = parse_tree_find_first_terminal(argument);
            return parse_symtab_add_alias(holder, source,
                PARSE_SYM_ALIAS_MAY, site ? site->start_byte : 0,
                source->lifetime.escapes);
        }
        for (size_t i = 0; i < source->alias_count; i++) {
            const parse_sym_alias_t *alias = parse_sym_alias_at(source, i);
            if (!alias || !alias->other) return false;
            bool escapes = alias->escapes
                || (scope_has_lexical_lifetime(alias->other->scope)
                    && !scope_is_within(holder->scope, alias->other->scope));
            if (!parse_symtab_add_alias(holder, alias->other, alias->kind,
                    alias->derived_at, escapes)) return false;
        }
        return true;
    }

    th_type *holder_type = holder->type ? th_resolve(holder->type) : nullptr;
    const char *holder_name = nullptr;
    size_t holder_name_size = 0;
    bool holder_is_variant = false;
    if (source && (source->kind == PARSE_SYM_VARIABLE
                   || source->kind == PARSE_SYM_PARAM)
        && holder_type && th_named_info(holder_type, &holder_name,
            &holder_name_size, &holder_is_variant, nullptr)
        && !holder_is_variant && holder_name_size == 4
        && !memcmp(holder_name, "view", 4)) {
        parse_token_info_t *site = parse_tree_find_first_terminal(argument);
        bool escapes = scope_has_lexical_lifetime(source->scope)
            && !scope_is_within(holder->scope, source->scope);
        return parse_symtab_add_alias(holder, source,
            PARSE_SYM_ALIAS_DERIVED, site ? site->start_byte : 0, escapes);
    }

    parse_tree_t *aggregate = aggregate_literal_root(grammar, argument);
    return aggregate && record_aggregate_aliases(grammar, unit, aggregate,
                                                   holder);
}

static bool
record_function_aggregate_aliases(parse_grammar_t *grammar,
                                  n00bb_link_analysis_t *analysis,
                                  core_translation_unit_t *source_unit,
                                  parse_tree_t *node,
                                  parse_sym_entry_t *holder,
                                  parse_sym_entry_t *function,
                                  core_translation_unit_t *call_unit,
                                  parse_tree_t *call,
                                  core_list_t(parse_tree_ptr_t) *active)
{
    n00bb_tu_state_t *state = tu_state(source_unit);
    n00bb_alias_edges_t *facts = state
        ? n00bb_node_alias_edges(node) : nullptr;
    if (!facts) return true;
    parse_tree_t *signature = child_nt(grammar, function->decl_node,
                                       "func-type-signature");
    parse_tree_t *param_decl = child_nt(grammar, signature, "param-decl");
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, param_decl, &formals, &rest);
    parse_tree_t *member = member_access_node(grammar,
        core_tree_num_children(call) ? core_tree_child(call, 0) : nullptr);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    bool trait_dispatch = member
        && !call_receiver_is_module(grammar, call_unit, receiver);
    bool complete = true;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        if (edge->kind == N00BB_ALIAS_CALL) {
            th_type *result_type = core_tree_node_value(edge->node).type;
            if (type_may_contain_reference_alias(state->types, result_type)
                && !record_returned_call_alias(grammar, analysis, source_unit,
                    holder, edge->node, active)) {
                holder->lifetime.escapes = true;
                complete = false;
            }
            continue;
        }
        if (edge->kind == N00BB_ALIAS_REFERENCE) {
            complete &= record_aggregate_aliases(grammar, source_unit,
                                                  edge->node, holder);
            continue;
        }
        size_t index = SIZE_MAX;
        for (size_t j = 0; j < formals.len; j++)
            if (formals.data[j] == edge->symbol) { index = j; break; }
        if (rest == edge->symbol) index = formals.len;
        if (index != SIZE_MAX) {
            parse_tree_t *argument = call_argument_for_parameter(grammar,
                call, function, index, trait_dispatch);
            complete &= argument && call_argument_aliases(grammar,
                call_unit, argument, holder);
        }
        else
            complete &= record_aggregate_aliases(grammar, source_unit,
                                                  edge->node, holder);
    }
    core_list_free(formals);
    return complete;
}


static bool
record_returned_call_alias(parse_grammar_t *grammar,
                           n00bb_link_analysis_t *analysis,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *holder,
                           parse_tree_t *expression,
                           core_list_t(parse_tree_ptr_t) *active);

static bool
record_function_return_aliases(parse_grammar_t *grammar,
                               n00bb_link_analysis_t *analysis,
                               parse_sym_entry_t *holder,
                               parse_sym_entry_t *function,
                               core_translation_unit_t *call_unit,
                               parse_tree_t *call,
                               core_list_t(parse_tree_ptr_t) *active);

static bool
record_function_return_aliases(parse_grammar_t *grammar,
                               n00bb_link_analysis_t *analysis,
                               parse_sym_entry_t *holder,
                               parse_sym_entry_t *function,
                               core_translation_unit_t *call_unit,
                               parse_tree_t *call,
                               core_list_t(parse_tree_ptr_t) *active)
{
    if (!function || !function->decl_node
        || !nt_is(grammar, function->decl_node, "func-def")) return false;
    core_translation_unit_t *owner = unit_for_symbol(grammar,
        &analysis->units, function);
    if (!owner) return false;
    n00bb_function_return_facts_t *facts = parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.function.return-facts"));
    if (!facts || !facts->return_expression_count) return false;
    bool complete = true;
    for (size_t i = 0; i < facts->return_expression_count; i++) {
        parse_tree_t *expression = facts->return_expressions[i];
        if (!expression) { complete = false; continue; }
        parse_tree_t *aggregate = aggregate_literal_root(grammar, expression);
        n00bb_tu_state_t *owner_state = tu_state(owner);
        n00bb_alias_edges_t *aliases = owner_state
            ? n00bb_node_alias_edges(expression)
            : nullptr;
        bool callback_value = owner_state && aliases
            && n00bb_callback_contract_for_type(owner_state,
                core_tree_node_value(expression).type);
        if (aggregate || callback_value)
            complete &= record_function_aggregate_aliases(grammar, analysis,
                owner, aggregate ? aggregate : expression, holder, function,
                call_unit, call,
                active);
        else
            complete &= record_returned_call_alias(grammar, analysis, owner,
                holder, expression, active);
    }
    return complete;
}

parse_sym_entry_t *
global_symbol_from_qualified_name(parse_grammar_t *grammar,
                                 n00bb_link_analysis_t *analysis,
                                 const char *name, size_t name_size)
{
    if (!grammar || !analysis || !name || !name_size) return nullptr;
    size_t dot = name_size;
    while (dot && name[dot - 1] != '.') dot--;
    if (!dot || dot == name_size) return nullptr;
    core_string_t module = core_string_borrow(name, dot - 1);
    core_string_t root = core_string_borrow(name + dot,
                                             name_size - dot);
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        if (!core_string_eq(unit->module.identifier, module)) continue;
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        parse_scope_t *scope = module_scope(state);
        parse_sym_entry_t *symbol = symtab && scope
            ? parse_symtab_lookup_scoped(symtab, scope,
                CORE_STRING_STATIC(""), root) : nullptr;
        return symbol && symbol->kind == PARSE_SYM_VARIABLE
            && is_module_global_symbol(grammar, symbol) ? symbol : nullptr;
    }
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        parse_scope_t *scope = module_scope(state);
        for (parse_sym_entry_t *symbol = scope ? scope->first_in_scope : nullptr;
             symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE
                || !is_module_global_symbol(grammar, symbol)) continue;
            n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.export-origin"));
            if (origin && origin->declaration.u8_bytes == name_size
                && !memcmp(origin->declaration.data, name, name_size))
                return symbol;
        }
    }
    return nullptr;
}

static bool
record_returned_call_alias_body(parse_grammar_t *grammar,
                          n00bb_link_analysis_t *analysis,
                          core_translation_unit_t *unit,
                          parse_sym_entry_t *holder,
                          parse_tree_t *expression,
                          core_list_t(parse_tree_ptr_t) *active)
{
    if (!holder || !expression) return false;
    if (core_string_eq(first_token_text(expression),
                       CORE_STRING_STATIC("ref"))) {
        parse_sym_entry_t *referent = direct_ref_target(grammar, expression);
        if (!referent) return false;
        bool escapes = scope_has_lexical_lifetime(referent->scope)
            && !scope_is_within(holder->scope, referent->scope);
        parse_token_info_t *site = parse_tree_find_first_terminal(expression);
        return parse_symtab_add_alias(holder, referent,
            PARSE_SYM_ALIAS_DERIVED, site ? site->start_byte : 0,
            escapes);
    }

    parse_tree_t *aggregate = aggregate_literal_root(grammar, expression);
    if (aggregate)
        return record_call_aggregate_aliases(grammar, analysis, unit, holder,
                                               aggregate, active);

    parse_tree_t *call = find_call_expression(grammar, unit, expression);
    if (call) {
        if (!core_tree_num_children(call)) return false;
        core_string_t index_trait = {0}, index_method = {0};
        if (index_dispatch_for_node(grammar, call, &index_trait,
                                    &index_method)
            && core_string_eq(index_trait,
                              CORE_STRING_STATIC("Index_Slice")))
            return call_argument_aliases(grammar,
                unit, core_tree_child(call, 0), holder);
        parse_tree_t *member = member_access_node(grammar,
            core_tree_num_children(call) ? core_tree_child(call, 0) : nullptr);
        parse_tree_t *receiver = member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
        bool trait_dispatch = member
            && !call_receiver_is_module(grammar, unit, receiver);
        if (trait_dispatch && analysis->may_link_further) return false;
        parse_sym_entry_t *callee = bound_function_symbol(
            trait_dispatch ? call : core_tree_child(call, 0));
        if (trait_dispatch && !callee)
            callee = attached_symbol(grammar, unit, member);
        if (trait_dispatch && !callee) {
            n00bb_tu_state_t *state = tu_state(unit);
            for (size_t i = 0; state && i < state->link_use_sites.len; i++) {
                n00bb_link_use_site_t *site = &state->link_use_sites.data[i];
                if (!site->symbol || !node_is_within(site->node, member)
                    || !n00bb_is_trait_dispatch_reference(grammar, unit, site->node))
                    continue;
                callee = site->symbol;
                break;
            }
        }
        if (!callee || callee->kind != PARSE_SYM_FUNCTION) return false;
        if (!callee->decl_node
            || !nt_is(grammar, callee->decl_node, "func-def")) {
            n00bb_function_lifetime_effects_t effects = {0};
            if (!function_lifetime_effects_for_symbol(analysis, callee,
                                                       &effects)) return false;
            if (effects.return_kind == N00BB_RETURN_ALIAS_NONE
                || effects.return_kind == N00BB_RETURN_ALIAS_FRESH_EXTERNAL)
                return true;
            if (effects.return_kind == N00BB_RETURN_ALIAS_GLOBAL) {
                parse_sym_entry_t *global = global_symbol_from_qualified_name(
                    grammar, analysis, effects.return_global,
                    effects.return_global_size);
                if (!global) return false;
                parse_token_info_t *site =
                    parse_tree_find_first_terminal(expression);
                return parse_symtab_add_alias(holder, global,
                    PARSE_SYM_ALIAS_DERIVED,
                    site ? site->start_byte : 0, false);
            }
            if (effects.return_kind != N00BB_RETURN_ALIAS_PARAMETER)
                return false;
            parse_tree_t *argument = call_argument_for_parameter(grammar,
                call, callee, effects.return_parameter, trait_dispatch);
            return argument && record_returned_call_alias(grammar, analysis,
                unit, holder, argument, active);
        }
        size_t parameter_index = 0;
        if (!function_returned_parameter(grammar, analysis, callee,
                                         &parameter_index))
            return record_function_return_aliases(grammar, analysis, holder,
                callee, unit, call, active);
        parse_tree_t *argument = call_argument_for_parameter(grammar, call,
            callee, parameter_index, trait_dispatch);
        if (argument && call_argument_aliases(grammar, unit, argument,
                                               holder))
            return true;
        return record_returned_call_alias(grammar, analysis, unit, holder,
                                          argument, active);
    }

    parse_sym_entry_t *source = simple_expression_symbol(expression);
    if (source && is_reference_symbol(source)) {
        if (!source->alias_count) return false;
        for (size_t i = 0; i < source->alias_count; i++) {
            const parse_sym_alias_t *alias = parse_sym_alias_at(source, i);
            if (!alias || !alias->other) return false;
            bool outlives_target = scope_has_lexical_lifetime(
                alias->other->scope)
                && !scope_is_within(holder->scope, alias->other->scope);
            if (!parse_symtab_add_alias(holder, alias->other, alias->kind,
                    alias->derived_at, alias->escapes || outlives_target))
                return false;
        }
        return true;
    }

    if (source && source->alias_count) {
        for (size_t i = 0; i < source->alias_count; i++) {
            const parse_sym_alias_t *alias = parse_sym_alias_at(source, i);
            if (!alias || !alias->other) return false;
            bool outlives_target = scope_has_lexical_lifetime(
                alias->other->scope)
                && !scope_is_within(holder->scope, alias->other->scope);
            if (!parse_symtab_add_alias(holder, alias->other, alias->kind,
                    alias->derived_at, alias->escapes || outlives_target))
                return false;
        }
        return true;
    }

    return false;
}

static bool
record_returned_call_alias(parse_grammar_t *grammar,
                           n00bb_link_analysis_t *analysis,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *holder,
                           parse_tree_t *expression,
                           core_list_t(parse_tree_ptr_t) *active)
{
    if (!expression || !active) return false;
    for (size_t i = 0; i < active->len; i++)
        if (active->data[i] == expression) return false;
    core_list_push(*active, expression);
    bool complete = record_returned_call_alias_body(grammar, analysis,
        unit, holder, expression, active);
    active->len--;
    return complete;
}

static bool
record_call_aggregate_aliases(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    parse_sym_entry_t *holder, parse_tree_t *expression,
    core_list_t(parse_tree_ptr_t) *active)
{
    if (!expression) return false;
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *facts = state
        ? n00bb_node_alias_edges(expression) : nullptr;
    if (!facts) return true;
    bool complete = true;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        if (edge->kind == N00BB_ALIAS_CALL) {
            th_type *result_type = core_tree_node_value(edge->node).type;
            if (type_may_contain_reference_alias(state->types, result_type))
                complete &= record_returned_call_alias(grammar, analysis,
                    unit, holder, edge->node, active);
        }
        else
            complete &= record_aggregate_aliases(grammar, unit,
                                                  edge->node, holder);
    }
    return complete;
}

void
record_link_unit_call_result_aliases(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) continue;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *holder = scope->first_in_scope; holder;
                 holder = holder->next_in_scope) {
                if ((holder->kind != PARSE_SYM_VARIABLE
                     && holder->kind != PARSE_SYM_PARAM)) continue;
                for (size_t d = 0; d < holder->definition_count; d++) {
                    parse_tree_t *definition = holder->definition_nodes[d];
                    const parse_rule_t *rule = parse_tree_rule(
                        analysis->grammar, definition);
                    parse_annotation_t *assignment = rule
                        ? parse_rule_find_annot(analysis->grammar, rule,
                                                PARSE_ANNOT_ASSIGNS)
                        : nullptr;
                    parse_tree_t *expression = assignment
                        ? parse_tree_resolve_child_ref(analysis->grammar,
                            definition, assignment->value_ref) : nullptr;
                    if (!expression
                        || !find_call_expression(analysis->grammar, unit,
                                                 expression)) continue;
                    core_list_t(parse_tree_ptr_t) active =
                        core_list_new(parse_tree_ptr_t);
                    bool summarized = record_returned_call_alias(
                        analysis->grammar, analysis, unit, holder,
                        expression, &active);
                    core_list_free(active);
                    if (summarized && holder->type
                        && type_may_contain_reference_alias(state->types,
                                                            holder->type))
                        parse_sym_set_alias_analysis_complete(holder, true);
                    if (!summarized && holder->type
                        && type_may_contain_reference_alias(state->types,
                                                            holder->type))
                        parse_sym_set_alias_analysis_complete(holder, false);
                    if (!summarized && is_reference_symbol(holder))
                        holder->lifetime.escapes = true;
                }
                bool escaping_alias = false;
                for (size_t a = 0; a < holder->alias_count; a++)
                    escaping_alias |= holder->aliases[a].escapes;
                if ((holder->lifetime.escapes || escaping_alias)
                    && holder->alias_count)
                    mark_alias_symbol_may_escape(state->types, holder);
            }
        }
    }
}

static parse_sym_entry_t *
source_function_parameter(parse_grammar_t *grammar,
                          parse_sym_entry_t *function, size_t index)
{
    if (!function || !function->decl_node
        || !nt_is(grammar, function->decl_node, "func-def")) return nullptr;
    n00bb_function_parameter_facts_t *facts = parse_sym_binding(function,
        N00BB_PARAMETER_FACTS_KEY);
    parse_tree_t *node = facts && index < facts->param_count
        ? facts->params[index].node : nullptr;
    return node ? core_tree_node_value(node).sym : nullptr;
}

static bool
source_parameter_may_escape(n00bb_link_analysis_t *analysis,
                            parse_grammar_t *grammar,
                            parse_sym_entry_t *function, size_t index)
{
    if (!function) return true;
    if (n00bb_builtin_native_function(function))
        return index >= th_func_nparams(function->type);
    if (!function->decl_node
        || !nt_is(grammar, function->decl_node, "func-def")) {
        n00bb_function_lifetime_effects_t effects = {0};
        if (!function_lifetime_effects_for_symbol(analysis, function,
                                                   &effects)
            || index >= effects.parameter_count) return true;
        uint8_t effect = effects.parameter_effects[index];
        return effect == N00BB_PARAM_EFFECT_UNKNOWN
            || effect == N00BB_PARAM_EFFECT_MAY_ESCAPE
            || effect == N00BB_PARAM_EFFECT_BOTH_ESCAPE;
    }
    parse_sym_entry_t *formal = source_function_parameter(grammar, function,
                                                           index);
    if (!formal) return true;

    if (formal->lifetime.escapes) return true;
    for (size_t i = 0; i < formal->alias_count; i++)
        if (formal->aliases[i].escapes) return true;
    for (size_t i = 0; i < formal->aliased_by_count; i++)
        if (formal->aliased_by[i].escapes) return true;

    return false;
}

static bool
source_parameter_referent_value_may_escape(n00bb_link_analysis_t *analysis,
                                           parse_grammar_t *grammar,
                                           parse_sym_entry_t *function,
                                           size_t index)
{
    if (!function) return true;
    if (n00bb_builtin_native_function(function)) return false;
    if (function->decl_node
        && nt_is(grammar, function->decl_node, "func-def")) {
        parse_sym_entry_t *formal = source_function_parameter(grammar,
                                                               function, index);
        return !formal || formal->lifetime.referent_value_escapes;
    }
    n00bb_function_lifetime_effects_t effects = {0};
    if (!function_lifetime_effects_for_symbol(analysis, function, &effects)
        || index >= effects.parameter_count
        || !effects.parameter_effects) return true;
    uint8_t effect = effects.parameter_effects[index];
    return effect == N00BB_PARAM_EFFECT_UNKNOWN
        || effect == N00BB_PARAM_EFFECT_REFERENT_VALUE_ESCAPE
        || effect == N00BB_PARAM_EFFECT_BOTH_ESCAPE;
}

static bool function_returned_global_inner(
    parse_grammar_t *grammar, n00bb_link_analysis_t *analysis,
    core_translation_unit_t *unit, parse_sym_entry_t *function,
    core_list_t(parse_sym_entry_ptr_t) *visited,
    core_buffer_t *global_name);

static bool
append_qualified_global_name(parse_grammar_t *grammar,
                             n00bb_link_analysis_t *analysis,
                             parse_sym_entry_t *global,
                             core_buffer_t *name)
{
    core_translation_unit_t *unit = global
        ? unit_for_symbol(grammar, &analysis->units, global) : nullptr;
    if (!grammar || !analysis || !unit || !global->name.data || !name)
        return false;
    core_buffer_printf(name, "%.*s.%.*s",
        (int)unit->module.identifier.u8_bytes,
        unit->module.identifier.data,
        (int)global->name.u8_bytes, global->name.data);
    return name->byte_len != 0;
}

bool
simple_returned_global(parse_grammar_t *grammar,
                      n00bb_link_analysis_t *analysis,
                      core_translation_unit_t *unit,
                      parse_tree_t *expression,
                      core_list_t(parse_sym_entry_ptr_t) *visited,
                      core_buffer_t *global_name)
{
    if (!expression || !global_name) return false;
    if (core_string_eq(first_token_text(expression),
                       CORE_STRING_STATIC("ref"))) {
        parse_sym_entry_t *target = direct_ref_target(grammar, expression);
        return target && is_module_global_symbol(grammar, target)
            && append_qualified_global_name(grammar, analysis, target,
                                              global_name);
    }
    parse_sym_entry_t *symbol = simple_expression_bound_symbol(expression);
    if (symbol && is_module_global_symbol(grammar, symbol))
        return append_qualified_global_name(grammar, analysis, symbol,
                                            global_name);
    parse_sym_entry_t *global = nullptr;
    if (symbol)
        for (size_t i = 0; i < parse_sym_alias_count(symbol); i++) {
            const parse_sym_alias_t *alias = parse_sym_alias_at(symbol, i);
            if (!alias || !alias->other
                || !is_module_global_symbol(grammar, alias->other))
                return false;
            if (global && global != alias->other) return false;
            global = alias->other;
        }
    if (global)
        return append_qualified_global_name(grammar, analysis, global,
                                            global_name);

    parse_tree_t *call = find_call_expression(grammar, unit, expression);
    if (!call || !core_tree_num_children(call)) return false;
    parse_tree_t *member = member_access_node(grammar, call);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    bool trait_dispatch = member
        && !call_receiver_is_module(grammar, unit, receiver);
    if (trait_dispatch && analysis->may_link_further) return false;
    parse_sym_entry_t *callee = bound_function_symbol(
        trait_dispatch ? call : core_tree_child(call, 0));
    if (!callee || callee->kind != PARSE_SYM_FUNCTION) return false;
    if (!callee->decl_node
        || !nt_is(grammar, callee->decl_node, "func-def")) {
        n00bb_function_lifetime_effects_t effects = {0};
        if (!function_lifetime_effects_for_symbol(analysis, callee, &effects))
            return false;
        if (effects.return_kind == N00BB_RETURN_ALIAS_GLOBAL) {
            core_buffer_append(global_name, effects.return_global,
                               effects.return_global_size);
            return true;
        }
        if (effects.return_kind != N00BB_RETURN_ALIAS_PARAMETER)
            return false;
        parse_tree_t *argument = call_argument_for_parameter(grammar, call,
            callee, effects.return_parameter, trait_dispatch);
        return simple_returned_global(grammar, analysis, unit, argument,
            visited, global_name);
    }

    if (function_returned_global_inner(grammar, analysis,
            unit_for_symbol(grammar, &analysis->units, callee), callee,
            visited, global_name)) return true;
    size_t parameter_index = 0;
    if (!function_returned_parameter(grammar, analysis, callee,
                                     &parameter_index)) return false;
    parse_tree_t *argument = call_argument_for_parameter(grammar, call,
        callee, parameter_index, trait_dispatch);
    return simple_returned_global(grammar, analysis, unit, argument,
                                  visited, global_name);
}

static bool
function_returned_global_inner(parse_grammar_t *grammar,
                               n00bb_link_analysis_t *analysis,
                               core_translation_unit_t *unit,
                               parse_sym_entry_t *function,
                               core_list_t(parse_sym_entry_ptr_t) *visited,
                               core_buffer_t *global_name)
{
    if (!grammar || !analysis || !unit || !function
        || !function->decl_node
        || !nt_is(grammar, function->decl_node, "func-def")) return false;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == function) return false;
    core_list_push(*visited, function);
    n00bb_function_return_facts_t *facts = parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.function.return-facts"));
    core_buffer_t expected = {0};
    bool found = facts && facts->return_expression_count != 0;
    for (size_t i = 0; found && i < facts->return_expression_count; i++) {
        core_buffer_t current = {0};
        found = simple_returned_global(grammar, analysis, unit,
            facts->return_expressions[i], visited, &current)
            && (!expected.byte_len
                || (expected.byte_len == current.byte_len
                    && !memcmp(expected.data, current.data,
                               current.byte_len)));
        if (found && !expected.byte_len)
            core_buffer_append(&expected, current.data, current.byte_len);
        core_buffer_release(&current);
    }
    visited->len--;
    found = found && expected.byte_len != 0;
    if (found) core_buffer_append(global_name, expected.data,
                                  expected.byte_len);
    core_buffer_release(&expected);
    return found;
}

static bool
function_returned_global(parse_grammar_t *grammar,
                         n00bb_link_analysis_t *analysis,
                         core_translation_unit_t *unit,
                         parse_sym_entry_t *function,
                         core_buffer_t *global_name)
{
    core_list_t(parse_sym_entry_ptr_t) visited =
        core_list_new(parse_sym_entry_ptr_t);
    bool found = function_returned_global_inner(grammar, analysis, unit,
        function, &visited, global_name);
    core_list_free(visited);
    return found;
}

static bool
function_returns_fresh_external_inner(
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *function,
    core_list_t(parse_sym_entry_ptr_t) *active)
{
    if (!analysis || !function) return false;
    if (n00bb_builtin_native_function(function))
        return core_string_eq(function->name,
                              CORE_STRING_STATIC("__os_map_zeroed"));
    for (size_t i = 0; i < active->len; i++)
        if (active->data[i] == function) return false;
    core_translation_unit_t *unit = unit_for_symbol(analysis->grammar,
        &analysis->units, function);
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    n00bb_function_return_facts_t *facts = parse_sym_binding(function,
        CORE_STRING_STATIC("n00bb.function.return-facts"));
    if (!state || !facts || !facts->return_expression_count) return false;
    core_list_push(*active, function);
    bool fresh = true;
    for (size_t i = 0; fresh && i < facts->return_expression_count; i++) {
        n00bb_return_source_t *source = n00bb_node_return_source(
            facts->return_expressions[i]);
        n00bb_link_use_site_t *site = source
            ? link_use_site_for_node(state, source->node) : nullptr;
        parse_sym_entry_t *callee = source && source->symbol
            ? source->symbol : site ? site->symbol : nullptr;
        fresh = source && source->kind == N00BB_RETURN_SOURCE_CALL
            && function_returns_fresh_external_inner(analysis, callee,
                                                     active);
    }
    active->len--;
    return fresh;
}

static bool
function_returns_fresh_external(n00bb_link_analysis_t *analysis,
                                parse_sym_entry_t *function)
{
    core_list_t(parse_sym_entry_ptr_t) active =
        core_list_new(parse_sym_entry_ptr_t);
    bool fresh = function_returns_fresh_external_inner(analysis, function,
                                                       &active);
    core_list_free(active);
    return fresh;
}

static bool
append_function_lifetime_effects(n00bb_link_analysis_t *analysis,
                                 th_env *environment,
                                 core_translation_unit_t *unit,
                                 parse_sym_entry_t *function)
{
    parse_grammar_t *grammar = analysis ? analysis->grammar : nullptr;
    th_type *signature = function && function->type
        ? th_resolve(function->type) : nullptr;
    if (!grammar || !environment || !unit || !function || !signature
        || th_kind_of(signature) != TH_FUNCTION) return false;
    parse_tree_t *signature_node = child_nt(grammar, function->decl_node,
                                            "func-type-signature");
    parse_tree_t *param_decl = child_nt(grammar, signature_node,
                                        "param-decl");
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, param_decl, &parameters, &rest);
    size_t count = parameters.len + (rest != nullptr);
    bool ok = count == th_func_nparams(signature)
        + (th_func_rest(signature) != nullptr) && count <= UINT32_MAX;
    core_buffer_t metadata = {0};
    if (ok) {
        core_buffer_append(&metadata, "N0FL", 4);
        core_buffer_putc(&metadata, 1);
        append_u32_le(&metadata, (uint32_t)count);
    }
    for (size_t i = 0; ok && i < count; i++) {
        parse_sym_entry_t *parameter = i < parameters.len
            ? parameters.data[i] : rest;
        if (!parameter || !parameter->type) {
            ok = false;
            break;
        }
        bool escapes = source_parameter_may_escape(analysis, grammar,
                                                    function, i);
        bool referent_value_escapes =
            parameter->lifetime.referent_value_escapes;
        n00bb_tu_state_t *state = tu_state(unit);
        bool aliasable = type_may_contain_reference_alias(
            state ? state->types : nullptr, parameter->type);
        bool empty_alias_effects = !parameter->alias_count
            && !parameter->aliased_by_count && !parameter->lifetime.escapes;
        bool complete = !aliasable || parameter->alias_analysis_complete
            || parameter->use_count == 0 || empty_alias_effects;
        n00bb_param_effect_t effect = !complete
            ? N00BB_PARAM_EFFECT_UNKNOWN
            : escapes && referent_value_escapes
                ? N00BB_PARAM_EFFECT_BOTH_ESCAPE
                : escapes ? N00BB_PARAM_EFFECT_MAY_ESCAPE
                : referent_value_escapes
                    ? N00BB_PARAM_EFFECT_REFERENT_VALUE_ESCAPE
                    : N00BB_PARAM_EFFECT_NO_ESCAPE;
        core_buffer_putc(&metadata, (char)effect);
    }

    n00bb_return_alias_kind_t return_kind = N00BB_RETURN_ALIAS_NONE;
    uint32_t return_parameter = UINT32_MAX;
    size_t returned_parameter = 0;
    if (ok && function_returned_parameter(grammar, analysis, function,
                                           &returned_parameter)) {
        if (returned_parameter > UINT32_MAX) ok = false;
        else {
            return_kind = N00BB_RETURN_ALIAS_PARAMETER;
            return_parameter = (uint32_t)returned_parameter;
        }
    }
    else if (ok && type_may_contain_reference_alias(
        tu_state(unit)->types, th_func_ret(signature))) {
        core_buffer_t global_name = {0};
        bool has_global = function_returned_global(grammar, analysis, unit,
            function, &global_name);
        if (has_global) {
            if (!global_name.byte_len || global_name.byte_len > UINT16_MAX)
                ok = false;
            else {
                return_kind = N00BB_RETURN_ALIAS_GLOBAL;
                core_buffer_putc(&metadata,
                                 (char)N00BB_RETURN_ALIAS_GLOBAL);
                append_u32_le(&metadata, UINT32_MAX);
                core_buffer_putc(&metadata,
                    (char)(global_name.byte_len & 0xff));
                core_buffer_putc(&metadata,
                    (char)(global_name.byte_len >> 8));
                core_buffer_append(&metadata, global_name.data,
                                   global_name.byte_len);
            }
        }
        else return_kind = function_returns_fresh_external(analysis, function)
            ? N00BB_RETURN_ALIAS_FRESH_EXTERNAL
            : N00BB_RETURN_ALIAS_UNKNOWN;
        core_buffer_release(&global_name);
    }
    if (ok && return_kind != N00BB_RETURN_ALIAS_GLOBAL) {
        core_buffer_putc(&metadata, (char)return_kind);
        append_u32_le(&metadata, return_parameter);
        core_buffer_putc(&metadata, 0);
        core_buffer_putc(&metadata, 0);
    }
    core_buffer_t qualified = {0};
    if (ok)
        ok = append_environment_symbol_name(grammar, unit, function,
                                               &qualified);
    if (ok)
        ok = th_env_register_metadata(environment, TH_ENV_METADATA_FUNCTION,
            qualified.data, "n00b.function-lifetime", 1, metadata.data,
            metadata.byte_len);
    core_buffer_release(&qualified);
    core_buffer_release(&metadata);
    core_list_free(parameters);
    return ok;
}

th_env *
build_function_lifetime_environment(n00bb_link_analysis_t *analysis)
{
    if (!analysis || !analysis->export_environment) return nullptr;
    const th_env *inputs[] = {analysis->export_environment};
    core_diagnostics_t *diagnostics = analysis->units.len
        ? core_translation_unit_diagnostics(analysis->units.data[0]) : nullptr;
    th_env *environment = th_env_compose_report(inputs, 1, diagnostics,
                                                nullptr);
    bool ok = environment != nullptr;
    for (size_t u = 0; ok && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) {
            ok = false;
            break;
        }
        for (parse_scope_t *scope = symtab->all_scopes; ok && scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)
                || !core_string_eq(scope->scope_tag,
                                   CORE_STRING_STATIC("module"))) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; ok && symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type
                    || !symbol->decl_node
                    || !nt_is(analysis->grammar, symbol->decl_node,
                              "func-def")) continue;
                ok = append_function_lifetime_effects(analysis, environment,
                                                       unit, symbol);
            }
        }
    }
    if (ok) ok = register_package_interfaces(analysis, environment);
    if (!ok) {
        th_env_free(environment);
        return nullptr;
    }
    return environment;
}

static void
record_call_argument_escape_facts_once(n00bb_link_analysis_t *analysis)
{
    parse_grammar_t *grammar = analysis->grammar;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        core_dict_foreach(state->annotations->cf_labels, item) {
            parse_cf_label_t *label = item->value;
            if (!label || label->kind != PARSE_CF_CALL || !label->cond
                || core_tree_is_leaf(label->cond)) continue;
            parse_tree_t *member = member_access_node(grammar, label->cond);
            parse_tree_t *receiver = member && core_tree_num_children(member)
                ? core_tree_child(member, 0) : nullptr;
            bool module_call = member
                && call_receiver_is_module(grammar, unit, receiver);
            bool trait_dispatch = member && !module_call;
            if (trait_dispatch && analysis->may_link_further) {
                if (receiver)
                    mark_expression_aliases_may_escape(grammar, unit, receiver);
                parse_tree_t *args = call_arguments_node(grammar, label->self);
                mark_expression_aliases_may_escape(grammar, unit, args);
                continue;
            }
            parse_sym_entry_t *function = bound_function_symbol(label->cond);
            bool source_function = function && function->decl_node
                && nt_is(grammar, function->decl_node, "func-def");
            n00bb_function_lifetime_effects_t interface_effects = {0};
            bool has_interface_effects = !source_function && function
                && function_lifetime_effects_for_symbol(analysis, function,
                                                        &interface_effects);
            if (!source_function && !has_interface_effects) {
                if (member && receiver)
                    mark_expression_aliases_may_escape(grammar, unit, receiver);
                parse_tree_t *args = call_arguments_node(grammar, label->self);
                mark_expression_aliases_may_escape(grammar, unit, args);
                continue;
            }

            core_list_t(parse_sym_entry_ptr_t) formals =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            size_t fixed_count = 0;
            bool has_rest = false;
            if (source_function) {
                parse_tree_t *signature = child_nt(grammar,
                    function->decl_node, "func-type-signature");
                parse_tree_t *param_decl = child_nt(grammar, signature,
                                                    "param-decl");
                collect_ir_parameters(grammar, param_decl, &formals, &rest);
                fixed_count = formals.len;
                has_rest = rest != nullptr;
            }
            else {
                th_type *signature = function->type
                    ? th_resolve(function->type) : nullptr;
                if (!signature || th_kind_of(signature) != TH_FUNCTION) {
                    core_list_free(formals);
                    if (member && receiver)
                        mark_expression_aliases_may_escape(grammar, unit, receiver);
                    parse_tree_t *args = call_arguments_node(grammar,
                                                             label->self);
                    mark_expression_aliases_may_escape(grammar, unit, args);
                    continue;
                }
                fixed_count = th_func_nparams(signature);
                has_rest = th_func_rest(signature) != nullptr;
                if (fixed_count + (has_rest ? 1 : 0)
                    != interface_effects.parameter_count) {
                    core_list_free(formals);
                    if (member && receiver)
                        mark_expression_aliases_may_escape(grammar, unit, receiver);
                    parse_tree_t *args = call_arguments_node(grammar,
                                                             label->self);
                    mark_expression_aliases_may_escape(grammar, unit, args);
                    continue;
                }
            }
            for (size_t i = 0; i < fixed_count; i++) {
                bool address_escapes = source_parameter_may_escape(
                    analysis, grammar, function, i);
                bool value_escapes =
                    source_parameter_referent_value_may_escape(
                        analysis, grammar, function, i);
                if (!address_escapes && !value_escapes) continue;
                parse_tree_t *argument = call_argument_for_parameter(
                    grammar, label->self, function, i, trait_dispatch);
                if (address_escapes)
                    mark_expression_aliases_may_escape(grammar, unit,
                                                       argument);
                if (value_escapes) {
                    parse_sym_entry_t *referent = cached_argument_value_source(
                        state, argument);
                    if (referent)
                        mark_referent_value_may_escape(state->types,
                                                        referent);
                    else
                        mark_expression_aliases_may_escape(grammar, unit,
                                                           argument);
                }
            }
            if (has_rest
                && (source_parameter_may_escape(analysis, grammar,
                                                function, fixed_count)
                    || source_parameter_referent_value_may_escape(
                        analysis, grammar, function, fixed_count))) {
                parse_tree_t *args = call_arguments_node(grammar, label->self);
                mark_expression_aliases_may_escape(grammar, unit, args);
            }
            core_list_free(formals);
        }
    }
}

static size_t
escape_fact_count(n00bb_link_analysis_t *analysis)
{
    size_t escaped = 0;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) continue;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                escaped += symbol->lifetime.escapes
                    + symbol->lifetime.referent_value_escapes;
            }
        }
    }
    return escaped;
}

bool
record_call_argument_escape_facts(n00bb_link_analysis_t *analysis)
{
    for (;;) {
        size_t before = escape_fact_count(analysis);
        record_call_argument_escape_facts_once(analysis);
        size_t after = escape_fact_count(analysis);
        if (after == before) return true;
        if (after < before) break;
    }

    if (analysis->units.len) {
        core_translation_unit_t *unit = analysis->units.data[0];
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0010"),
            CORE_STRING_STATIC(
                "call escape facts did not stabilize across the LinkUnit"));
    }
    return false;
}
