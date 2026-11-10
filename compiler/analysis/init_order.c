#include "lower/lower_internal.h"

static bool
instruction_loads_global(ir_places_t *places, const ir_instr_t *instruction,
                         const ir_global_t *global);
static bool
uses_global_value(const ir_instr_t *instruction, const ir_global_t *global);
static void
add_rule_member(core_list_t(n00bb_ir_func_ptr_t) *members, core_dict_t *seen,
                ir_func_t *function);
static bool
global_read_site(ir_summary_facts_t *facts, ir_places_t *places,
                 const ir_splace_t *place, ir_instr_t *instruction);
static core_translation_unit_t *
global_owner_unit(n00bb_link_analysis_t *analysis, const ir_global_t *global);
static bool
initializers_known(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit);
static const ir_global_t *
path_global(const ir_spath_t *path);
static const ir_global_t *
entry_read_global(const ir_summary_t *summary, uint32_t index);
static bool
initialized_dynamically(n00bb_link_analysis_t *analysis,
                        const n00bb_initializer_read_t *read);
static const n00bb_initializer_read_t *
blocking_read(n00bb_link_analysis_t *analysis, core_link_unit_t *link,
              const core_list_t(n00bb_initializer_read_t) *reads,
              core_translation_unit_t *candidate, size_t scc,
              core_dict_t *selected);
static void
report_initializer_cycle(core_link_unit_t *link, size_t scc,
                         core_dict_t *selected,
                         const n00bb_initializer_read_t *read);

static bool
instruction_loads_global(ir_places_t *places, const ir_instr_t *instruction,
                         const ir_global_t *global)
{
    uint32_t first = 0;
    uint32_t count = places
        ? ir_places_accesses_of(places, instruction, &first) : 0;
    for (uint32_t a = 0; a < count; a++) {
        const ir_access_t *access = ir_places_access_at(places, first + a);
        if (access->kind != IR_ACCESS_LOAD) continue;
        const uint32_t *ids = ir_places_access_places(places, access);
        for (uint32_t p = 0; p < access->count; p++) {
            const ir_place_t *place = ir_place_at(places, ids[p]);
            if ((place->kind == IR_PLACE_GLOBAL
                 || place->kind == IR_PLACE_THREAD)
                && place->root && place->root->kind == IR_VAL_GLOBAL
                && ir_global_identity(place->root->global) == global)
                return true;
        }
    }
    return false;
}

static bool
uses_global_value(const ir_instr_t *instruction, const ir_global_t *global)
{
    if (instruction->op == IR_OP_ADDRESS_OF) return false;
    uint32_t first = instruction->op == IR_OP_GLOBAL_STORE
        || instruction->op == IR_OP_STORE ? 1 : 0;
    for (uint32_t o = first; o < instruction->n_operands; o++) {
        const ir_value_t *value = ir_operand_value(&instruction->operands[o]);
        if (value && value->kind == IR_VAL_GLOBAL && value->global
            && ir_global_identity(value->global) == global) return true;
    }
    return false;
}

core_translation_unit_t *
unit_for_ir_module(n00bb_link_analysis_t *analysis, ir_module_t *module)
{
    if (!module) return nullptr;
    core_translation_unit_t *unit = analysis->unit_of_module_ready
        ? core_dict_get(&analysis->unit_of_module, module, nullptr) : nullptr;
    if (unit && unit_module(tu_state(unit)) == module) return unit;
    for (size_t u = 0; u < analysis->units.len; u++) {
        if (unit_module(tu_state(analysis->units.data[u])) != module)
            continue;
        if (!analysis->unit_of_module_ready)
            core_dict_init(&analysis->unit_of_module, nullptr, nullptr);
        analysis->unit_of_module_ready = true;
        core_dict_put(&analysis->unit_of_module, module,
                      analysis->units.data[u]);
        return analysis->units.data[u];
    }
    return nullptr;
}

core_translation_unit_t *
n00bb_unit_named(n00bb_link_analysis_t *analysis, core_string_t name)
{
    for (size_t u = 0; name.data && u < analysis->units.len; u++)
        if (core_string_eq(analysis->units.data[u]->module.identifier, name))
            return analysis->units.data[u];
    return nullptr;
}

size_t
rule_group(n00bb_rule_facts_t *rules, const ir_func_t *function)
{
    bool found = false;
    void *group = core_dict_get(&rules->groups, function->owner, &found);
    return found ? (size_t)(uintptr_t)group - 1 : rules->count - 1;
}

static void
add_rule_member(core_list_t(n00bb_ir_func_ptr_t) *members, core_dict_t *seen,
                ir_func_t *function)
{
    bool found = false;
    if (!function || !function->n_blocks) return;
    core_dict_get(seen, function, &found);
    if (found) return;
    core_dict_put(seen, function, 1);
    core_list_push(*members, function);
}

static core_list_t(n00bb_ir_func_ptr_t)
rule_members(n00bb_link_analysis_t *analysis,
             const core_list_t(n00bb_ir_func_ptr_t) *entries)
{
    core_list_t(n00bb_ir_func_ptr_t) members =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, core_hash_word, nullptr);
    for (size_t f = 0; f < analysis->emitted_functions.len; f++)
        add_rule_member(&members, &seen, analysis->emitted_functions.data[f]);
    for (size_t u = 0; u < analysis->units.len; u++) {
        ir_module_t *module = unit_module(tu_state(analysis->units.data[u]));
        if (module)
            add_rule_member(&members, &seen, ir_func_find(module,
                CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)));
    }
    for (size_t f = 0; entries && f < entries->len; f++)
        add_rule_member(&members, &seen, entries->data[f]);
    for (size_t i = 0; i < analysis->assert_sites.len; i++)
        add_rule_member(&members, &seen, analysis->assert_sites.data[i].function);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        for (size_t i = 0; i < state->result_defaults.len; i++) {
            ir_instr_t *write = n00bb_declared_result_default(
                state->result_defaults.data[i]);
            if (write) add_rule_member(&members, &seen, write->block->owner);
        }
    }
    core_dict_free(&seen);
    return members;
}

void
rule_facts_begin(n00bb_link_analysis_t *analysis, core_link_unit_t *link,
                 const core_list_t(n00bb_ir_func_ptr_t) *entries)
{
    n00bb_rule_facts_t *rules = core_alloc(n00bb_rule_facts_t);
    rules->link = link;
    rules->summaries = analysis->summaries;
    core_dict_init(&rules->groups, core_hash_word, nullptr);
    rules->count = core_link_unit_scc_count(link) + 1;
    for (size_t g = 0; g + 1 < rules->count; g++)
        for (size_t m = 0; m < core_link_unit_scc_member_count(link, g); m++) {
            ir_module_t *module = unit_module(tu_state(
                core_link_unit_scc_member(link, g, m)));
            if (module) core_dict_put(&rules->groups, module, g + 1);
        }
    core_list_t(n00bb_ir_func_ptr_t) members = rule_members(analysis, entries);
    rules->starts = core_alloc_array(size_t, rules->count + 1);
    rules->members = core_alloc_array(ir_func_t *, members.len + 1);
    rules->facts = core_alloc_array(ir_summary_facts_t *, rules->count);
    rules->explained = core_alloc_array(ir_summary_facts_t *, rules->count);
    for (size_t f = 0; f < members.len; f++)
        rules->starts[rule_group(rules, members.data[f]) + 1]++;
    for (size_t g = 0; g < rules->count; g++)
        rules->starts[g + 1] += rules->starts[g];
    size_t *fill = core_alloc_array(size_t, rules->count);
    for (size_t f = 0; f < members.len; f++) {
        size_t group = rule_group(rules, members.data[f]);
        rules->members[rules->starts[group] + fill[group]++] = members.data[f];
    }
    core_free(fill);
    core_list_free(members);
    analysis->rule_facts = rules;
}

void
rule_facts_end(n00bb_link_analysis_t *analysis)
{
    n00bb_rule_facts_t *rules = analysis->rule_facts;
    if (!rules) return;
    for (size_t g = 0; g < rules->count; g++) {
        ir_summary_facts_free(rules->facts[g]);
        ir_summary_facts_free(rules->explained[g]);
    }
    core_free(rules->starts);
    core_free(rules->members);
    core_free(rules->facts);
    core_free(rules->explained);
    core_dict_free(&rules->groups);
    core_free(rules);
    analysis->rule_facts = nullptr;
}

ir_summary_facts_t *
group_facts(n00bb_link_analysis_t *analysis, const ir_func_t *function,
            bool provenance)
{
    n00bb_rule_facts_t *rules = analysis->rule_facts;
    if (!rules || rules->summaries != analysis->summaries) return nullptr;
    size_t group = rule_group(rules, function);
    ir_summary_facts_t **slot = provenance ? &rules->explained[group]
                                           : &rules->facts[group];
    if (*slot) return *slot;
    size_t count = rules->starts[group + 1] - rules->starts[group];
    double start = n00bb_profile_seconds();
    *slot = ir_summary_facts_build(analysis->summaries,
                                   rules->members + rules->starts[group],
                                   count, provenance);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE summary-facts %.3f group %zu functions %zu "
                "facts %lld iterations %lld\n",
            n00bb_profile_seconds() - start, group, count,
            (long long)core_datalog_total_facts(
                ir_summary_facts_engine(*slot)),
            (long long)core_datalog_iterations(
                ir_summary_facts_engine(*slot)));
    return *slot;
}

static bool
global_read_site(ir_summary_facts_t *facts, ir_places_t *places,
                 const ir_splace_t *place, ir_instr_t *instruction)
{
    if (uses_global_value(instruction, place->global)) return true;
    if (instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE)
        return ir_summary_facts_call_reads_entry(facts, instruction, place);
    return instruction_loads_global(places, instruction, place->global);
}

ir_instr_t *
first_uninitialized_read(n00bb_link_analysis_t *analysis, ir_func_t *function,
                         const ir_splace_t *place, uint8_t entry_state)
{
    ir_summary_facts_t *facts = group_facts(analysis, function, false);
    size_t scope = ir_analysis_scope_begin();
    ir_places_t *places = ir_func_places(function);
    ir_summary_state_query_t *query = nullptr;
    ir_instr_t *found = nullptr;
    for (uint32_t b = 0; !found && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t n = 0; !found && n < block->n_instrs; n++) {
            ir_instr_t *instruction = block->instrs[n];
            if (!global_read_site(facts, places, place, instruction))
                continue;
            if (!query)
                query = ir_summary_state_query(analysis->summaries, function,
                                               place, entry_state);
            if (ir_summary_state_query_at(query, instruction)
                & IR_PATH_UNPASSED)
                found = instruction;
        }
    }
    ir_summary_state_query_free(query);
    ir_analysis_scope_end(scope);
    return found;
}

bool
initializer_units_share_cycle(core_link_unit_t *link,
                              core_translation_unit_t *left,
                              core_translation_unit_t *right)
{
    size_t left_scc, right_scc;
    if (!left || !right || !core_link_unit_scc_of(link, left, &left_scc)
        || !core_link_unit_scc_of(link, right, &right_scc)
        || left_scc != right_scc) return false;
    if (core_link_unit_scc_member_count(link, left_scc) > 1) return true;
    for (size_t i = 0; i < core_translation_unit_import_count(left); i++)
        if (core_translation_unit_import(left, i) == left) return true;
    return false;
}

void
index_module_globals(n00bb_link_analysis_t *analysis, core_dict_t *out)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    || !is_module_global_symbol(analysis->grammar, symbol))
                    continue;
                ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                symbol);
                if (global && ir_global_identity(global) == global)
                    core_dict_add(out, global, symbol);
            }
    }
}

static core_translation_unit_t *
global_owner_unit(n00bb_link_analysis_t *analysis, const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    ir_module_t *owner = identity ? identity->owner : nullptr;
    core_translation_unit_t *unit = unit_for_ir_module(analysis, owner);
    return unit || !owner ? unit
        : n00bb_unit_named(analysis, ir_module_name(owner));
}

static bool
initializers_known(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit)
{
    return !tu_state(unit)->interface_module
        || interface_body_module(analysis, unit->module.identifier);
}

static const ir_global_t *
path_global(const ir_spath_t *path)
{
    return (path->root == IR_SPLACE_GLOBAL || path->root == IR_SPLACE_THREAD)
            && path->global
        ? ir_global_identity((ir_global_t *)path->global) : nullptr;
}

static const ir_global_t *
entry_read_global(const ir_summary_t *summary, uint32_t index)
{
    const ir_spath_t  *path   = ir_summary_path_at(summary, index);
    const ir_global_t *global = path->flags & IR_SPATH_READS_ENTRY
        ? path_global(path) : nullptr;
    for (uint32_t i = 0; global && i < index; i++) {
        const ir_spath_t *earlier = ir_summary_path_at(summary, i);
        if ((earlier->flags & IR_SPATH_READS_ENTRY)
            && path_global(earlier) == global)
            return nullptr;
    }
    return global;
}

bool
collect_initializer_reads(n00bb_link_analysis_t *analysis,
                          core_list_t(n00bb_initializer_read_t) *reads)
{
    core_dict_t symbols;
    core_dict_init(&symbols, nullptr, nullptr);
    index_module_globals(analysis, &symbols);
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        ir_module_t *module = unit_module(tu_state(unit));
        ir_func_t *initializer = module ? n00bb_cached_body(ir_func_find(
            module, CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))) : nullptr;
        ir_summary_t *summary = initializer
            ? ir_summary_of(analysis->summaries, initializer) : nullptr;
        for (uint32_t i = 0; summary
             && i < ir_summary_path_count(summary); i++) {
            const ir_global_t *identity = entry_read_global(summary, i);
            if (!identity || identity->has_static_initializer) continue;
            bool found = false;
            parse_sym_entry_t *symbol = core_dict_get(&symbols,
                (void *)identity, &found);
            core_translation_unit_t *target = found
                ? global_owner_unit(analysis, identity) : nullptr;
            if (!target || !initializers_known(analysis, target)) continue;
            ir_global_t *global = (ir_global_t *)identity;
            ir_splace_t  place  = ir_splace_global(global);
            ir_instr_t  *read   = first_uninitialized_read(
                analysis, initializer, &place, IR_PATH_UNPASSED);
            if (read)
                core_list_push(*reads, ((n00bb_initializer_read_t){
                    .source = unit, .target = target, .symbol = symbol,
                    .global = global, .initializer = initializer,
                    .read = read,
                }));
        }
    }
    core_dict_free(&symbols);
    return true;
}

bool
span_covers(ir_span_t span, uint32_t line, uint32_t column)
{
    return (line > span.start_line
            || (line == span.start_line && column >= span.start_col))
        && (line < span.end_line
            || (line == span.end_line && column <= span.end_col));
}

core_diag_location_t
initializer_read_location(const n00bb_initializer_read_t *read)
{
    ir_span_t span = read->read->span;
    n00bb_tu_state_t *state = tu_state(read->source);
    for (size_t i = 0; state && i < state->link_use_sites.len; i++) {
        n00bb_link_use_site_t *use = &state->link_use_sites.data[i];
        parse_token_info_t *token = use->symbol == read->symbol
                && use->in_module_initializer
            ? parse_tree_find_first_terminal(use->node) : nullptr;
        if (!token || !span_covers(span, parse_token_line(token),
                                   parse_token_column(token))) continue;
        return (core_diag_location_t){
            .line = parse_token_line(token),
            .column = parse_token_column(token),
            .end_line = parse_token_endline(token),
            .end_column = parse_token_endcol(token),
            .has_location = true,
        };
    }
    return (core_diag_location_t){
        .line = span.start_line, .column = span.start_col,
        .end_line = span.end_line, .end_column = span.end_col,
        .has_location = true,
    };
}

bool
summary_writes_global(const ir_summary_t *summary, const ir_global_t *global)
{
    global = global ? ir_global_identity((ir_global_t *)global) : nullptr;
    for (uint32_t i = 0; summary && i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if ((path->flags & IR_SPATH_WRITE) && path_global(path) == global)
            return true;
    }
    return false;
}

static bool
initialized_dynamically(n00bb_link_analysis_t *analysis,
                        const n00bb_initializer_read_t *read)
{
    const ir_global_t *global = ir_global_identity(read->global);
    if (!global || global->has_static_initializer
        || ir_global_storage_duration(global) == IR_STORAGE_DURATION_THREAD)
        return false;
    ir_module_t *module = unit_module(tu_state(read->target));
    ir_func_t *initializer = module ? n00bb_cached_body(ir_func_find(module,
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))) : nullptr;
    return initializer
        && summary_writes_global(ir_summary_of(analysis->summaries,
                                               initializer), global);
}

static const n00bb_initializer_read_t *
blocking_read(n00bb_link_analysis_t *analysis, core_link_unit_t *link,
              const core_list_t(n00bb_initializer_read_t) *reads,
              core_translation_unit_t *candidate, size_t scc,
              core_dict_t *selected)
{
    for (size_t r = 0; r < reads->len; r++) {
        const n00bb_initializer_read_t *read = &reads->data[r];
        size_t target_scc = 0;
        if (read->source != candidate || read->target == candidate
            || core_dict_contains(selected, read->target)
            || !core_link_unit_scc_of(link, read->target, &target_scc)
            || target_scc != scc
            || !initialized_dynamically(analysis, read))
            continue;
        return read;
    }
    return nullptr;
}

static void
report_initializer_cycle(core_link_unit_t *link, size_t scc,
                         core_dict_t *selected,
                         const n00bb_initializer_read_t *read)
{
    core_buffer_t modules = {0};
    size_t listed = 0;
    for (size_t m = 0; m < core_link_unit_scc_member_count(link, scc); m++) {
        core_translation_unit_t *member = core_link_unit_scc_member(link,
                                                                    scc, m);
        if (core_dict_contains(selected, member)) continue;
        core_buffer_printf(&modules, "%s`%.*s`", listed++ ? ", " : "",
                           (int)member->module.identifier.u8_bytes,
                           member->module.identifier.data);
    }
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(read->source);
    n00bb_report(diagnostics, read->source, N00B_DIAG_MODULE_INIT_CYCLE,
        initializer_read_location(read), N00B_TEXT_MODULES_READ_EACH_OTHER_S,
        N00BB_ARGS(ARG_TEXT(core_buffer_string(&modules))));
    n00bb_report_label(diagnostics,
        N00B_TEXT_READS_BEFORE_MODULE_INITIALIZES_IT,
        N00BB_ARGS(ARG_TEXT(read->symbol->name),
                   ARG_TEXT(read->target->module.identifier)));
    n00bb_report_help(diagnostics, N00B_TEXT_MOVE_THE_READ_INTO_A,
        N00BB_NO_ARGS);
    core_buffer_release(&modules);
}

bool
order_initializers(core_link_unit_t *link, n00bb_link_analysis_t *analysis,
                   const core_list_t(n00bb_initializer_read_t) *reads,
                   core_list_t(core_translation_unit_ptr_t) *order)
{
    core_dict_t selected;
    core_dict_init(&selected, nullptr, nullptr);
    bool ok = true;
    for (size_t scc = 0; ok && scc < core_link_unit_scc_count(link); scc++) {
        size_t members = core_link_unit_scc_member_count(link, scc);
        for (size_t step = 0; ok && step < members; step++) {
            core_translation_unit_t *choice = nullptr;
            const n00bb_initializer_read_t *blocked = nullptr;
            for (size_t m = 0; !choice && m < members; m++) {
                core_translation_unit_t *candidate =
                    core_link_unit_scc_member(link, scc, m);
                if (!candidate || core_dict_contains(&selected, candidate))
                    continue;
                const n00bb_initializer_read_t *read = blocking_read(
                    analysis, link, reads, candidate, scc, &selected);
                if (read) blocked = read;
                else choice = candidate;
            }
            if (!choice) {
                if (blocked)
                    report_initializer_cycle(link, scc, &selected, blocked);
                ok = false;
                continue;
            }
            core_dict_add(&selected, choice, choice);
            core_list_push(*order, choice);
        }
    }
    core_dict_free(&selected);
    return ok && order->len == analysis->units.len;
}
