#include "compiler_link.h"

bool
n00bb_collect_initializer_order(
    core_link_unit_t *link, core_list_t(core_translation_unit_ptr_t) *order)
{
    core_link_contribution_view_t contribution;
    if (!core_link_unit_require_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC(N00BB_INITIALIZER_ORDER_DOMAIN), 1,
            &contribution)) return false;
    const n00bb_initializer_order_t *initializer_order =
        contribution.payload;
    if (!initializer_order) return false;
    for (size_t i = 0; i < initializer_order->count; i++)
        core_list_push(*order, initializer_order->units[i]);
    return initializer_order->count
        == core_link_unit_translation_unit_count(link);
}

static bool
add_initializer_use_facts(core_link_unit_t *link, uint32_t producer_stage,
                          core_link_dependency_state_t state,
                          core_link_declaration_t *source,
                          core_link_declaration_t *target,
                          core_translation_unit_t *unit,
                          parse_tree_t *site,
                          core_diag_location_t location,
                          core_string_t provenance,
                          uint32_t provenance_version)
{
    return core_link_unit_add_dependency(link,
            N00BB_LINK_DEPENDENCY_INITIALIZER_USE, producer_stage, state,
            source, target, unit, site, location, provenance,
            provenance_version, nullptr)
        && core_link_unit_add_dependency(link,
            N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER, producer_stage, state,
            source, target, unit, site, location, provenance,
            provenance_version, nullptr);
}

bool
initializer_units_share_cycle(core_link_unit_t *link,
                               core_translation_unit_t *left,
                               core_translation_unit_t *right)
{
    if (!left || !right) return false;
    size_t left_scc, right_scc;
    if (!core_link_unit_scc_of(link, left, &left_scc)
        || !core_link_unit_scc_of(link, right, &right_scc)
        || left_scc != right_scc) return false;
    if (core_link_unit_scc_member_count(link, left_scc) > 1) return true;
    for (size_t import = 0;
         import < core_translation_unit_import_count(left); import++)
        if (core_translation_unit_import(left, import) == left)
            return true;
    return false;
}

bool
publish_link_initializer_uses(core_link_unit_t *link,
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
            if (site->intrinsic_operator) continue;
            if (!site->in_module_initializer) continue;
            if (primitive_operator_site(state, site)) continue;
            if (dotted_receiver_prefix_node(analysis->grammar, site->node)
                || module_member_is_call(analysis->grammar, site->node))
                continue;
            if (n00bb_assignment_target(analysis->grammar, site->node,
                                       nullptr)) continue;
            if (initializer_node_is_out_argument(analysis->grammar, unit,
                                                 site->node)) continue;
            if (initializer_node_is_reference_store_target(analysis->grammar,
                    unit, site->node, site->symbol)) continue;
            if (is_type_reference_node(analysis->grammar, site->node))
                continue;
            parse_sym_entry_t *symbol = canonical_symbol(site->symbol);
            if (n00bb_builtin_function(symbol)) continue;
            if (symbol && (symbol->kind == PARSE_SYM_MODULE
                    || symbol->kind == PARSE_SYM_ENUM_CONST
                    || symbol->kind == PARSE_SYM_TYPEDEF
                    || symbol->kind == PARSE_SYM_TAG))
                continue;
            if (symbol && symbol->kind == PARSE_SYM_VARIABLE
                && !is_module_global_symbol(analysis->grammar, symbol))
                continue;

            bool relevant_kind = symbol
                && (symbol->kind == PARSE_SYM_FUNCTION
                    || (symbol->kind == PARSE_SYM_VARIABLE
                        && is_module_global_symbol(analysis->grammar,
                                                   symbol)));
            core_link_declaration_t *target = relevant_kind
                ? link_declaration_for_symbol(analysis, symbol) : nullptr;
            bool external = target
                && core_link_declaration_is_external_requirement(target);
            parse_sym_entry_t *initializer_owner = site->initializer_owner;
            if (!initializer_owner)
                initializer_owner = module_variable_initializer_owner(
                    analysis->grammar, state->annotations->symtab,
                    site->node);
            if (site->callback_call) continue;
            core_link_declaration_t *source = initializer_owner
                ? link_declaration_for_symbol(analysis, initializer_owner)
                : nullptr;
            bool resolved = relevant_kind && target
                && (!initializer_owner || source);
            if (!resolved) complete = false;
            core_link_dependency_state_t dependency_state = external
                ? CORE_LINK_DEPENDENCY_EXTERNAL
                : resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                           : CORE_LINK_DEPENDENCY_UNAVAILABLE;
            if (!add_initializer_use_facts(link, producer_stage,
                    dependency_state, source, resolved ? target : nullptr,
                    unit, site->node, link_source_location(site->node),
                    core_source_file_path(&unit->source), 1))
                return false;
        }
    }

    size_t call_dependency_count = core_link_unit_dependency_count(link);
    for (size_t d = 0; d < call_dependency_count; d++) {
        core_link_dependency_view_t call;
        if (!core_link_unit_dependency_at(link, d, &call)) return false;
        if ((call.kind != N00BB_LINK_DEPENDENCY_CALL
                && call.kind != N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                && call.kind != N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            || !call.source_unit || !call.source_site) continue;
        parse_sym_entry_t *callee = link_dependency_callee_symbol(
            analysis, &call);
        if (call.state == CORE_LINK_DEPENDENCY_EXTERNAL
            && (link_dependency_reference_builtin(&call, nullptr)
                || n00bb_builtin_function(callee))) continue;
        parse_tree_t *site = (parse_tree_t *)call.source_site;
        n00bb_tu_state_t *source_state = tu_state(call.source_unit);
        parse_sym_entry_t *owner_symbol = module_variable_initializer_owner(
            analysis->grammar,
            source_state && source_state->annotations
                ? source_state->annotations->symtab : nullptr,
            site);
        if (!is_module_initializer_use(analysis->grammar, site,
                                       owner_symbol)) continue;
        core_link_declaration_t *source = owner_symbol
            ? link_declaration_for_symbol(analysis, owner_symbol) : nullptr;
        bool external = call.state == CORE_LINK_DEPENDENCY_EXTERNAL;
        bool resolved = (call.state == CORE_LINK_DEPENDENCY_RESOLVED
                || external)
            && call.to && (!owner_symbol || source);
        if (n00bb_builtin_callback_function(
                link_dependency_callee_symbol(analysis, &call)))
            continue;
        if (!resolved) complete = false;
        if (!add_initializer_use_facts(link, producer_stage,
                external ? CORE_LINK_DEPENDENCY_EXTERNAL
                : resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                           : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                source, resolved ? call.to : nullptr, call.source_unit, site,
                call.source_location, call.provenance,
                call.provenance_version))
            return false;
        for (size_t e = 0; e < call_dependency_count; e++) {
            core_link_dependency_view_t effect;
            if (!core_link_unit_dependency_at(link, e, &effect))
                return false;
            bool global_effect = effect_summary_kind(effect.kind)
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY
                || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN;
            if (!global_effect || effect.from != call.to) continue;
            if (effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
                || effect.kind
                    == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY)
                continue;
            if (effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN)
                continue;
            bool effect_resolved = effect.state == CORE_LINK_DEPENDENCY_RESOLVED
                && effect.to;
            if (!effect_resolved) complete = false;
            if (!add_initializer_use_facts(link, producer_stage,
                    effect_resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                                    : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    source, effect.to, call.source_unit,
                    (parse_tree_t *)call.source_site, call.source_location,
                    call.provenance, call.provenance_version))
                return false;
        }
    }
    *state_out = complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                          : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}

typedef struct {
    core_link_declaration_t *declaration;
    size_t parent;
    uint8_t color;
} n00bb_initializer_graph_node_t;
core_list_decl(n00bb_initializer_graph_node_t);

bool
has_initializer_write_candidate(core_link_unit_t *link,
                                n00bb_link_analysis_t *analysis,
                                parse_sym_entry_t *symbol)
{
    return n00bb_module_initializer_writes(analysis, symbol)
        || n00bb_function_writes_global(link, analysis, symbol);
}

static core_translation_unit_t *
unit_owning_node(n00bb_link_analysis_t *analysis, parse_tree_t *node)
{
    while (node && parse_tree_parent(node))
        node = parse_tree_parent(node);
    for (size_t i = 0; node && i < analysis->units.len; i++)
        if (n00bb_tu_parsed_tree(tu_state(analysis->units.data[i])) == node)
            return analysis->units.data[i];
    return nullptr;
}

bool
n00bb_function_writes_global(core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             parse_sym_entry_t *symbol)
{
    core_link_declaration_t *target =
        link_declaration_for_symbol(analysis, symbol);
    for (size_t i = 0; target
         && i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) continue;
        bool writes = edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
            || edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
            || edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS
            || edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY
            || edge.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY
            || edge.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY;
        if (writes && edge.to == target) return true;
    }
    return false;
}

bool
n00bb_module_initializer_writes(n00bb_link_analysis_t *analysis,
                                parse_sym_entry_t *symbol)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        if (!state) continue;
        for (size_t site = 0; site < state->link_use_sites.len; site++) {
            n00bb_link_use_site_t *use = &state->link_use_sites.data[site];
            if (use->symbol == symbol && use->in_module_initializer
                && (n00bb_assignment_target(analysis->grammar, use->node,
                                            nullptr)
                    || initializer_node_is_out_argument(analysis->grammar,
                                                        analysis->units.data[i],
                                                        use->node)))
                return true;
        }
    }
    for (size_t u = 0; u < symbol->use_count; u++) {
        parse_tree_t *use = symbol->use_nodes[u];
        if (enclosing_function_symbol(analysis->grammar, use)) continue;
        core_translation_unit_t *owner = unit_owning_node(analysis, use);
        if (owner && n00bb_use_writes_root(owner, analysis->grammar, use,
                                           nullptr)) return true;
    }
    return false;
}

static bool
initializer_graph_add_node(
    parse_grammar_t *grammar,
    core_list_t(n00bb_initializer_graph_node_t) *nodes,
    core_dict_t *positions,
    core_link_declaration_t *declaration)
{
    if (!dynamic_initializer_declaration(grammar, declaration)
        || core_dict_contains(positions, declaration))
        return true;
    core_list_push(*nodes, ((n00bb_initializer_graph_node_t){
        .declaration = declaration,
    }));
    core_dict_add(positions, declaration, nodes->len);
    return true;
}

static void
append_initializer_cycle_member(core_buffer_t *chain, core_buffer_t *modules,
                                core_list_t(core_string_t) *seen,
                                core_link_declaration_t *declaration)
{
    core_string_t module = core_link_declaration_module(declaration);
    if (chain->byte_len) core_buffer_puts(chain, " -> ");
    append_initializer_declaration_name(chain, declaration);
    for (size_t i = 0; i < seen->len; i++)
        if (core_string_eq(seen->data[i], module)) return;
    core_list_push(*seen, module);
    core_buffer_printf(modules, "%s'%.*s'", modules->byte_len ? ", " : "",
                       (int)module.u8_bytes, module.data);
}

static void
report_initializer_cycle(core_list_t(n00bb_initializer_graph_node_t) *nodes,
                         size_t from, size_t target,
                         core_link_dependency_view_t *edge)
{
    size_t length = 1;
    for (size_t at = from; at != target; at = nodes->data[at].parent)
        length++;
    size_t *members = core_alloc_array(size_t, length);
    members[0] = target;
    size_t index = length;
    for (size_t at = from; at != target; at = nodes->data[at].parent)
        members[--index] = at;
    core_buffer_t chain = {0};
    core_buffer_t modules = {0};
    core_list_t(core_string_t) seen = core_list_new(core_string_t);
    for (size_t i = 0; i < length; i++)
        append_initializer_cycle_member(&chain, &modules, &seen,
            nodes->data[members[i]].declaration);
    append_initializer_cycle_member(&chain, &modules, &seen,
                                    nodes->data[target].declaration);
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "initializer dependency cycle requires reading an uninitialized global: %.*s (module%s %.*s)",
        (int)chain.byte_len, chain.data ? chain.data : "",
        seen.len == 1 ? "" : "s", (int)modules.byte_len,
        modules.data ? modules.data : "");
    core_diagnostics_report_at(
        core_translation_unit_diagnostics(edge->source_unit),
        edge->source_unit, CORE_DIAG_ERROR,
        CORE_STRING_STATIC("N00BB-L0040"),
        core_string_borrow(message.data, message.byte_len),
        edge->source_location);
    core_buffer_release(&message);
    core_buffer_release(&chain);
    core_buffer_release(&modules);
    core_list_free(seen);
    core_free(members);
}

static bool
initializer_graph_cycle_visit(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis,
    core_list_t(n00bb_initializer_graph_node_t) *nodes,
    core_dict_t *positions,
    size_t node_index, bool *reported)
{
    nodes->data[node_index].color = 1;
    core_link_declaration_t *source = nodes->data[node_index].declaration;
    for (size_t i = 0; i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) return false;
        if (edge.kind != N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER
            || edge.state != CORE_LINK_DEPENDENCY_RESOLVED
            || edge.from != source || !edge.to
            || !dynamic_initializer_declaration(analysis->grammar, edge.to))
            continue;
        size_t target = (size_t)(uintptr_t)core_dict_get(
            positions, edge.to, nullptr);
        if (!target) continue;
        target--;
        uint8_t color = nodes->data[target].color;
        if (color == 1) {
            if (!*reported && edge.source_unit) {
                report_initializer_cycle(nodes, node_index, target, &edge);
                *reported = true;
            }
            continue;
        }
        if (!color) nodes->data[target].parent = node_index;
        if (!color && !initializer_graph_cycle_visit(link, analysis, nodes,
                positions, target, reported)) return false;
    }
    nodes->data[node_index].color = 2;
    return true;
}

bool
validate_initializer_cycles(core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            core_link_contribution_state_t *state_out)
{
    bool complete = true;
    bool failed = false;
    core_link_dependency_view_t unavailable = {0};
    core_list_t(n00bb_initializer_graph_node_t) nodes =
        core_list_new(n00bb_initializer_graph_node_t);
    core_dict_t positions;
    core_dict_init(&positions, nullptr, nullptr);
    bool ok = true;
    for (size_t i = 0; ok && i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        ok = core_link_unit_dependency_at(link, i, &edge);
        if (!ok || edge.kind != N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER)
            continue;
        if (edge.state != CORE_LINK_DEPENDENCY_RESOLVED) {
            complete = false;
            if (!unavailable.kind) unavailable = edge;
        }
        ok = initializer_graph_add_node(analysis->grammar, &nodes,
                                        &positions, edge.from)
            && initializer_graph_add_node(analysis->grammar, &nodes,
                                          &positions, edge.to);
    }
    bool reported = false;
    for (size_t i = 0; ok && i < nodes.len; i++)
        if (!nodes.data[i].color)
            ok = initializer_graph_cycle_visit(link, analysis, &nodes,
                                               &positions, i, &reported);
    failed |= reported;
    core_dict_free(&positions);
    core_list_free(nodes);
    if (!ok) return false;
    if (!complete && !failed && unavailable.kind) {
        core_string_t source = unavailable.from
            ? core_link_declaration_key(unavailable.from)
            : CORE_STRING_STATIC("<module initializer>");
        core_string_t target = unavailable.to
            ? core_link_declaration_key(unavailable.to)
            : CORE_STRING_STATIC("<unknown>");
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "initializer dependency from '%.*s' to '%.*s' is not resolved",
            (int)source.u8_bytes, source.data,
            (int)target.u8_bytes, target.data);
        core_diagnostics_report_at(unavailable.source_unit
                ? core_translation_unit_diagnostics(
                    unavailable.source_unit)
                : core_link_unit_diagnostics(link),
            unavailable.source_unit, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-L0045"),
            core_string_borrow(message.data, message.byte_len),
            unavailable.source_location);
        core_buffer_release(&message);
    }
    *state_out = failed ? CORE_LINK_CONTRIBUTION_FAILED
        : complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                   : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}
