#include "lower_internal.h"
#include "comptime.h"

static bool
field_extents_hold(core_list_t(core_translation_unit_ptr_t) *units);
static int
compare_functions(const void *left, const void *right);
static void
report_view_candidate(core_translation_unit_t *unit,
                      parse_sym_entry_t *function, parse_sym_entry_t *parameter);
static bool
view_candidate(n00bb_tu_state_t *state, const n00bb_flow_t *flow,
               parse_sym_entry_t *parameter, uint32_t index);
static void
report_view_candidates(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       ir_summaries_t *summaries);
static bool
reintern_unit_types(core_list_t(core_translation_unit_ptr_t) *units);

static void
n00bb_mark_synthetic_locals(ir_module_t *module)
{
    for (size_t i = 0; module && i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at(module, i);
        for (uint32_t l = 0; function && l < function->n_locals; l++)
            function->locals[l]->synthetic = function->locals[l]->synthetic
                || !function->locals[l]->source_name.data;
    }
}

bool
n00bb_dump_ir(const n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; analysis && i < analysis->units.len; i++) {
        core_string_t text = ir_module_print(
            unit_module(tu_state(analysis->units.data[i])));
        if (!text.data) return false;
        fwrite(text.data, 1, text.u8_bytes, stdout);
        core_free(text.data);
    }
    return analysis != nullptr;
}

static bool
field_extents_hold(core_list_t(core_translation_unit_ptr_t) *units)
{
    bool ok = true;
    for (size_t u = 0; u < units->len; u++) {
        core_translation_unit_t *unit = units->data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                n00bb_field_union_t *member = symbol->is_field
                    ? parse_sym_binding(symbol, N00BB_FIELD_UNION_KEY)
                    : nullptr;
                uint64_t width = 0;
                bool is_signed = false;
                if (member && (!n00bb_integer_representation(
                        th_resolve(member->discriminant->type), &width,
                        &is_signed)
                    || (width != 8 && width != 16 && width != 32))) {
                    core_string_t type_text = n00bb_print_type(state->types,
                        member->discriminant->type);
                    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                        N00B_DIAG_UNION_DISCRIMINANT_TYPE,
                        link_source_location(member->node),
                        N00B_TEXT_THE_DISCRIMINANT_IS_BUT_A,
                        N00BB_ARGS(ARG_TEXT(member->discriminant->name),
                                   ARG_TEXT(type_text)));
                    ok = false;
                }
                n00bb_field_extent_t *extent = symbol->is_field
                    ? parse_sym_binding(symbol, N00BB_FIELD_EXTENT_KEY)
                    : nullptr;
                if (!extent) continue;
                if (!foreign_pointer_type(symbol->type)) {
                    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                        N00B_DIAG_EXTENT_NEEDS_FOREIGN,
                        link_source_location(extent->node),
                        N00B_TEXT_FIELD_HAS_AN_EXTENT_CLAUSE,
                        N00BB_ARGS(ARG_TEXT(symbol->name)));
                    ok = false;
                }
                else if (extent->count_field && !is_integer_type(
                        th_resolve(extent->count_field->type))) {
                    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                        N00B_DIAG_EXTENT_COUNT_TYPE,
                        link_source_location(extent->node),
                        N00B_TEXT_THE_COUNT_FIELD_OF_MUST,
                        N00BB_ARGS(ARG_TEXT(extent->count_field->name),
                                   ARG_TEXT(symbol->name)));
                    ok = false;
                }
            }
    }
    return ok;
}

static int
compare_functions(const void *left, const void *right)
{
    uintptr_t a = (uintptr_t)*(const ir_func_t *const *)left;
    uintptr_t b = (uintptr_t)*(const ir_func_t *const *)right;
    return a < b ? -1 : a > b;
}

static core_list_t(n00bb_ir_func_ptr_t)
taken_functions(ir_module_t *module)
{
    core_list_t(n00bb_ir_func_ptr_t) taken =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *instruction
                    = function->blocks[b]->instrs[i];
                for (uint32_t o = 0; o < instruction->n_operands; o++) {
                    const ir_value_t *value
                        = ir_value_operand(instruction, o);
                    if (value && value->kind == IR_VAL_FUNC && value->owner
                        && (o || instruction->op != IR_OP_CALL))
                        core_list_push(taken, value->owner);
                }
            }
    }
    if (taken.len)
        qsort(taken.data, taken.len, sizeof(*taken.data), compare_functions);
    return taken;
}

static void
report_view_candidate(core_translation_unit_t *unit,
                      parse_sym_entry_t *function, parse_sym_entry_t *parameter)
{
    n00bb_report(core_translation_unit_diagnostics(unit), unit,
        N00B_DIAG_PARAMETER_COULD_BE_VIEW,
        link_source_location(parameter->decl_node),
        N00B_TEXT_PARAMETER_COULD_BE_VIEW_NEVER,
        N00BB_ARGS(ARG_TEXT(parameter->name), ARG_TEXT(function->name)));
}

static bool
view_candidate(n00bb_tu_state_t *state, const n00bb_flow_t *flow,
               parse_sym_entry_t *parameter, uint32_t index)
{
    th_mode mode = parameter->has_mode ? parameter->mode : TH_IN;
    return mode == TH_IN && index < 64 && parameter->type
        && !parse_sym_binding(parameter, n00bb_view_blocked_binding)
        && n00bb_unit_type_reaches_shared_state(state, parameter->type)
        && !((flow->mutates[N00BB_FLOW_SELF]
              | flow->mutates[N00BB_FLOW_CONTENTS]
              | flow->escapes[N00BB_FLOW_SELF]
              | flow->escapes[N00BB_FLOW_CONTENTS]
              | flow->leaks[N00BB_FLOW_SELF]
              | flow->leaks[N00BB_FLOW_CONTENTS])
             >> index & 1);
}

static void
report_view_candidates(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       ir_summaries_t *summaries)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_list_t(n00bb_ir_func_ptr_t) taken = {0};
    bool scanned = false;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    for (size_t s = 0; s < scopes.len; s++)
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type
                || !function_symbol_at(grammar, symbol->decl_node)
                || core_string_starts_with(symbol->name,
                                           CORE_STRING_STATIC("__"))
                || parse_sym_binding(symbol,
                       CORE_STRING_STATIC("n00bb.external-export")))
                continue;
            ir_func_t *function = link_ir_function_for_symbol(
                state->link_analysis, symbol);
            ir_summary_t *summary = function && function->n_blocks
                ? ir_summary_of(summaries, function) : nullptr;
            if (!summary) continue;
            n00bb_flow_t flow;
            n00bb_summary_flow(summary, &flow);
            if (flow.opaque) continue;
            core_list_t(parse_sym_entry_ptr_t) parameters =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            collect_ir_parameters(grammar, child_nt(grammar,
                symbol->decl_node,
                CORE_STRING_STATIC("func-type-signature")),
                &parameters, &rest);
            bool receiver = parameters.len && core_string_eq(
                parameters.data[0]->name, CORE_STRING_STATIC("self"));
            uint32_t input = 0;
            for (size_t p = 0; !receiver && p < parameters.len; p++) {
                parse_sym_entry_t *parameter = parameters.data[p];
                th_mode mode = parameter->has_mode ? parameter->mode
                                                   : TH_IN;
                if (!th_mode_accepts_input(mode)) continue;
                if (!view_candidate(state, &flow, parameter, input++))
                    continue;
                if (!scanned) {
                    taken = taken_functions(state->ir_module);
                    scanned = true;
                }
                if (taken.len && bsearch(&function, taken.data, taken.len,
                                         sizeof(*taken.data),
                                         compare_functions))
                    break;
                report_view_candidate(unit, symbol, parameter);
            }
            core_list_free(parameters);
        }
    core_list_free(scopes);
    core_list_free(taken);
}

n00bb_lower_context_t *
n00bb_solved_lower_new(n00bb_link_analysis_t *analysis,
                       parse_grammar_t *grammar,
                       core_translation_unit_t *unit)
{
    n00bb_lower_context_t *lower = core_alloc(n00bb_lower_context_t);
    if (lower)
        *lower = (n00bb_lower_context_t){
            .grammar = grammar,
            .ir_context = analysis->ir_context,
            .units = &analysis->units,
            .unit = unit,
            .solved = true,
        };
    return lower;
}

static bool
reintern_unit_types(core_list_t(core_translation_unit_ptr_t) *units)
{
    bool ok = true;
    for (size_t i = 0; i < units->len; i++) {
        ir_module_t *module = tu_state(units->data[i])->ir_module;
        ok = (!module || ir_module_reintern_types(module)) && ok;
    }
    return ok;
}

static bool finish_deferred_lowering(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *units, bool ok);
static bool analyze_lowered_program(
    parse_grammar_t *grammar, core_link_unit_t *link,
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *unit_list, bool ok,
    bool dump_control_flow, bool dump_pdg, bool dump_summaries,
    double profile_start);

bool
n00bb_analyze_program(parse_grammar_t *grammar, core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis, bool dump_control_flow,
                      bool dump_pdg, bool dump_summaries)
{
    core_list_t(core_translation_unit_ptr_t) units =
        core_list_new(core_translation_unit_ptr_t);
    for (size_t i = 0; link && i < core_link_unit_translation_unit_count(link);
         i++)
        core_list_push(units, core_link_unit_translation_unit(link, i));
    return analyze_lowered_program(grammar, link, analysis, &units,
                                   units.len != 0, dump_control_flow,
                                   dump_pdg, dump_summaries,
                                   n00bb_profile_seconds());
}

bool
lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                     n00bb_link_analysis_t *analysis,
                     ir_ctx_t *ir_context,
                     core_diagnostics_t *diagnostics,
                     bool dump_ir,
                     bool dump_control_flow, bool dump_pdg,
                     bool dump_summaries, bool dump_layout)
{
    double profile_start = n00bb_profile_seconds();
    if (analysis) analysis->ir_context = ir_context;
    core_link_contribution_view_t semantic_analysis;
    core_link_contribution_view_t bootstrap_analysis;
    core_list_t(core_translation_unit_ptr_t) units =
        core_list_new(core_translation_unit_ptr_t);
    bool ok = link && core_link_unit_graph_is_finalized(link)
        && analysis
        && core_link_unit_require_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.semantic-ready"), 1,
            &semantic_analysis)
        && core_link_unit_require_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.bootstrap-pipeline-ran"), 1,
            &bootstrap_analysis)
        && core_link_unit_run_stage(link, N00BB_LINK_STAGE_IR_BINDINGS)
        && core_link_unit_collect(link);
    for (size_t i = 0; ok
         && i < core_link_unit_translation_unit_count(link); i++)
        core_list_push(units,
            core_link_unit_translation_unit(link, i));
    ok = ok && units.len
        && field_extents_hold(&units);
    for (size_t i = 0; ok && i < units.len; i++) {
        n00bb_tu_state_t *state = tu_state(units.data[i]);
        if (!state) ok = false;
        else {
            state->link = link;
            state->link_analysis = analysis;
        }
    }
    ir_lowerer_t *lowerer = ir_lowerer_new(ir_context);
    ok = ok && lowerer
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("binary"), n00bb_lower_binary,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("nil_literal"),
                               n00bb_lower_nil_literal, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("cast"), n00bb_lower_cast, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("unsafe_cast"),
                               n00bb_lower_cast,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("typeof"), n00bb_lower_typeof,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("effect_query"),
                               n00bb_lower_effect_query, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("bool_not"), n00bb_lower_bool_not,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("complement"),
                               n00bb_lower_complement, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("negate"), n00bb_lower_negate,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("string_literal"),
                               n00bb_lower_string_literal, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("char_literal"),
                               n00bb_lower_char_literal, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("sequence_literal"),
                               n00bb_lower_sequence_literal, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("brace_literal"),
                               n00bb_lower_brace_literal, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("array_new"),
                               n00bb_lower_array_new, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("callback_create"),
                               n00bb_lower_callback_create, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("compound_assign"),
                               n00bb_lower_compound_assignment, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("module_member"),
                               n00bb_lower_module_member, nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("return"), n00bb_lower_return,
                               nullptr)
        && ir_lowerer_register(lowerer, CORE_STRING_STATIC("call"),
                               n00bb_lower_call, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_BRANCH,
                                     n00bb_lower_branch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_LOOP,
                                     n00bb_lower_loop_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SWITCH,
                                     n00bb_lower_switch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_JUMP,
                                     n00bb_lower_jump_annotation, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("postfix-expr"), n00bb_lower_variant_payload,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer, CORE_STRING_STATIC("place"),
                                        n00bb_lower_variant_payload, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("embed-lit"), n00bb_lower_scalar_litmod,
            nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_CALL,
                                     n00bb_mark_call_lvalues, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("simple-lit"), n00bb_skip_i128_min_magnitude,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("simple-lit"), n00bb_lower_scalar_litmod,
            nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_DECLARES,
                                     n00bb_lower_declare_local, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_ASSIGNS,
                                     n00bb_check_assignment, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_VARREF,
                                     n00bb_initialize_default_reference,
                                     nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_VARREF,
                                     n00bb_lower_atomic_reference, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_branch_body, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_typecase_scope, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_try_scope, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("new-expr"), n00bb_mark_constructor_lvalues, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("assert-stmt"), n00bb_lower_assert_statement, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("expression-stmt"), n00bb_lower_effect_check,
        nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("effect-decl"), n00bb_lower_effect_declaration,
        nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("mark-decl"), n00bb_lower_mark_declaration,
        nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("expression"), n00bb_capture_compound_target, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("place"), n00bb_capture_compound_target,
            nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("case-expr-item"), n00bb_prune_case_name, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("case-expr-list"), n00bb_enter_case_patterns,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("body-stmt"),
            n00bb_resume_unreachable_statement, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("top-level-stmt"),
            n00bb_resume_unreachable_statement, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("and-rhs"), n00bb_lower_branch_body, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("or-rhs"), n00bb_lower_branch_body, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        CORE_STRING_STATIC("expr-comma-list"), n00bb_lower_tuple_parts, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("named-field-list"), n00bb_lower_named_field_parts, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("tuple-or-paren"), n00bb_lower_tuple_literal, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("variable-decl"), n00bb_lower_pattern, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("assign-stmt"), n00bb_lower_pattern, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("body-stmt"), n00bb_lower_statement_scope,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("top-level-stmt"), n00bb_lower_statement_scope,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("expression"), n00bb_lower_statement_scope,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("or-expr"), n00bb_lower_statement_scope,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("and-expr"), n00bb_lower_statement_scope,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("postfix-expr"), n00bb_lower_field_receiver,
            nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            CORE_STRING_STATIC("place"), n00bb_lower_field_receiver,
            nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                         n00bb_lower_with_scope, nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_OPERATOR,
                                         n00bb_lower_operator_annotation,
                                         nullptr);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-register %.3f\n",
            n00bb_profile_seconds() - profile_start);
    size_t *marks = core_alloc_array(size_t, units.len + 1);
    for (size_t i = 0; marks && i < units.len; i++)
        marks[i] = core_diagnostics_count(
            core_translation_unit_diagnostics(units.data[i]));
    bool bodies_ok = ok;
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        bool unit_ok = true;
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type
                    || !function_symbol_at(grammar, symbol->decl_node))
                    continue;
                if (lower_function_body(lowerer, grammar, unit, symbol,
                                        ir_context, &units)) continue;
                ir_func_t *function = link_ir_function_for_symbol(
                    state->link_analysis, symbol);
                unit_ok = n00bb_unit_failed(unit)
                    && (!function
                        || exclude_function(state->link_analysis, unit,
                                            function))
                    && unit_ok;
            }
        }
        core_list_free(scopes);
        bodies_ok = bodies_ok && unit_ok;
    }
    ok = bodies_ok;
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-functions %.3f\n",
            n00bb_profile_seconds() - profile_start);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (!context_symbol(grammar, symbol)) continue;
                static const core_string_t hooks[] = {
                    CORE_STRING_STATIC("context-enter"),
                    CORE_STRING_STATIC("context-exit"),
                    CORE_STRING_STATIC("context-fail"),
                    CORE_STRING_STATIC("context-ensure"),
                };
                for (size_t h = 0; ok
                    && h < sizeof(hooks) / sizeof(hooks[0]); h++) {
                    core_string_t role = n00bb_context_hook_role(hooks[h]);
                    parse_tree_t *hook = symbol->decl_node && role.data
                        ? parse_sym_binding(symbol, role) : nullptr;
                    if (hook)
                        ok = lower_context_hook(lowerer, grammar, unit,
                            symbol, hook, ir_context, &units);
                }
                core_list_t(parse_sym_entry_ptr_t) parameters =
                    core_list_new(parse_sym_entry_ptr_t);
                parse_sym_entry_t *rest = nullptr;
                collect_ir_parameters(grammar,
                    child_nt(grammar, symbol->decl_node, CORE_STRING_STATIC("param-decl")),
                    &parameters, &rest);
                for (size_t p = 0; ok && p < parameters.len; p++)
                    if (parameters.data[p]->is_optional)
                        ok = lower_context_default(lowerer, grammar, unit,
                            symbol, p, ir_context, &units);
                core_list_free(parameters);
                (void)rest;
            }
        }
        core_list_free(scopes);
    }
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-contexts %.3f\n",
            n00bb_profile_seconds() - profile_start);
    core_list_t(core_translation_unit_ptr_t) init_order =
        core_list_new(core_translation_unit_ptr_t);
    ok = ok && n00bb_generate_value_methods(analysis);
    if (ok && !lower_all_module_initializers(analysis, lowerer, grammar,
                                             &units, ir_context))
        ok = explained_by_errors(&units);
    n00bb_typing_t typing = {0};
    bool profile = n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE);
    double typing_start = profile ? n00bb_profile_seconds() : 0;
    if (ok && !n00bb_infer_types(analysis, units.data, units.len, &typing))
        ok = typing.recovered && explained_by_errors(&units);
    ok = ok && reintern_unit_types(&units);
    double rewrite_start = profile ? n00bb_profile_seconds() : 0;
    if (ok && !n00bb_rewrite_typed_ir(analysis, grammar, &typing))
        ok = explained_by_errors(&units);
    ok = ok && reintern_unit_types(&units)
        && n00bb_generate_dispatch_adapters(analysis);
    if (ok && !n00bb_check_initialized_fields(analysis))
        ok = explained_by_errors(&units);
    if (ok && !n00bb_check_variant_arms(analysis))
        ok = explained_by_errors(&units);
    n00bb_typing_free(&typing);
    if (profile)
        fprintf(stderr, "PROFILE lower-infer %.3f lower-rewrite %.3f\n",
            rewrite_start - typing_start,
            n00bb_profile_seconds() - rewrite_start);
    ok = ok && publish_declared_aggregate_layouts(analysis)
        && publish_interned_variant_layouts(analysis)
        && build_reachable_functions(analysis, &units, diagnostics);

    bool members_bound = ok && bind_field_member_uses(analysis);
    ok = ok && members_bound;
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-initializers %.3f\n",
            n00bb_profile_seconds() - profile_start);

    exclude_failed_functions(analysis, &units, marks);
    if (ok) ok = build_escape_facts(analysis)
        && check_lease_storage(analysis)
        && report_bookkeeping_writes(analysis)
        && mark_external_call_returns(analysis)
        && mark_diverging_calls(analysis);
    ir_policy_t *verify_policy = ok ? ir_policy_new(ir_context) : nullptr;
    if (ok) ok = verify_policy && ir_policy_set(verify_policy,
        CORE_STRING_STATIC("IR-E0023"), IR_POLICY_ALLOW);
    bool verified_all = ok;
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_diags_t *diagnostics = attach_ir_diags(ir_context, verify_policy,
                                                  unit, nullptr);
        ir_verify_failures_t failures = {0};
        ir_props_t properties = ir_verify_with_failures(module, diagnostics,
                                                        &failures);
        ir_props_t verified = IR_PROP_PARSED | IR_PROP_TYPED
            | IR_PROP_VERIFIED;
        bool clean = !failures.count && !failures.module;
        bool contained = failures.count && !failures.module
            && core_diagnostics_failed(core_translation_unit_diagnostics(unit));
        for (size_t f = 0; contained && f < failures.count; f++)
            contained = exclude_function(analysis, unit,
                                         failures.functions[f]);
        ir_verify_failures_release(&failures);
        if (contained) properties = ir_verify(module, nullptr);
        verified_all = (contained || clean)
            && (properties & verified) == verified && verified_all;
    }
    ok = verified_all;
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-verify %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok) {
        ir_policy_t *policy = ir_policy_new(ir_context);
        ok = policy
            && ir_policy_set(policy, CORE_STRING_STATIC("IR-W0027"), IR_POLICY_DENY)
            && ir_policy_set(policy, CORE_STRING_STATIC("IR-W0030"), IR_POLICY_ALLOW)
            && ir_policy_set(policy, CORE_STRING_STATIC("IR-W0031"), IR_POLICY_WARN);
        bool analyzed = ok;
        for (size_t i = 0; ok && i < units.len; i++) {
            core_translation_unit_t *unit = units.data[i];
            ir_diags_t *diagnostics = attach_ir_diags(ir_context, policy,
                                                      unit, nullptr);
            n00bb_mark_synthetic_locals(tu_state(unit)->ir_module);
            analyzed = ir_analyze(tu_state(unit)->ir_module, diagnostics)
                && analyzed;
        }
        ok = analyzed || explained_by_errors(&units);
    }
    exclude_failed_functions(analysis, &units, marks);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-analyze %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok) ok = validate_consumed_bindings(analysis)
        || explained_by_errors(&units);
    exclude_failed_functions(analysis, &units, marks);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-consume %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok) ok = place_unit_allocations(analysis)
        || explained_by_errors(&units);
    exclude_failed_functions(analysis, &units, marks);
    static const ir_pass_t *const lowering_passes[] = {
        &ir_pass_promote,
        &ir_pass_structure,
        &ir_pass_layout,
    };
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        bool sliced = n00bb_buffer_slice_pass(tu_state(unit)->ir_module,
            analysis->escapes, attach_ir_diags(ir_context, nullptr, unit,
                                               nullptr));
        if (sliced) report_view_candidates(grammar, unit, analysis->escapes);
        ok = sliced || explained_by_errors(&units);
    }
    exclude_failed_functions(analysis, &units, marks);
    core_free(marks);
    if (ok && dump_ir) ok = n00bb_dump_ir(analysis);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        ir_diags_t *diagnostics = attach_ir_diags(ir_context, nullptr, unit,
                                                  nullptr);
        for (size_t p = 0; ok
             && p < sizeof(lowering_passes) / sizeof(lowering_passes[0]);
             p++) {
            ir_props_t unmet = 0;
            ir_pass_result_t result = ir_pass_run(lowering_passes[p],
                module, nullptr, &unmet, diagnostics);
            if (result != IR_PASS_OK) {
                if (result == IR_PASS_UNMET) {
                    n00bb_internal_error(
                        core_translation_unit_diagnostics(unit), unit,
                        (core_diag_location_t){0}, "optimizing the program",
                        "the prerequisites of pass `%.*s` are not satisfied",
                        (int)lowering_passes[p]->name.u8_bytes,
                        lowering_passes[p]->name.data);
                }
                ok = false;
            }
        }
    }
    if (ok) n00bb_comptime_drop_declarations(analysis);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-passes %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok && dump_layout)
        for (size_t i = 0; i < units.len; i++)
            dump_layout_for_unit(grammar, units.data[i]);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        for (size_t f = 0; ok && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            size_t scope = ir_analysis_scope_begin();
            bool built = ir_func_pdg(function) != nullptr;
            ir_analysis_scope_end(scope);
            if (function->n_blocks && !built) {
                n00bb_internal_error(
                    core_translation_unit_diagnostics(unit), unit,
                    n00bb_span_location(function->span), "analyzing the program",
                    "could not build the dependence graph of `%.*s`",
                    (int)function->name.u8_bytes, function->name.data);
                ok = false;
            }
        }
    }
    core_list_free(init_order);
    analysis->members_bound = members_bound;
    if (ok && analysis->defer_program_analysis)
        return finish_deferred_lowering(link, analysis, &units, ok);
    return analyze_lowered_program(grammar, link, analysis, &units, ok,
                                   dump_control_flow, dump_pdg,
                                   dump_summaries, profile_start);
}

static bool
finish_deferred_lowering(core_link_unit_t *link,
                         n00bb_link_analysis_t *analysis,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         bool ok)
{
    bool lowered = ok;
    for (size_t i = 0; i < units->len; i++)
        if (core_diagnostics_failed(
                core_translation_unit_diagnostics(units->data[i])))
            ok = false;
    if (link && core_link_unit_has_fatal_diagnostics(link)) ok = false;
    analysis->lowered_with_errors = lowered && !ok;
    for (size_t i = 0; i < units->len; i++)
        ir_module_release_analyses(tu_state(units->data[i])->ir_module);
    core_list_free(*units);
    return ok;
}

static bool
analyze_lowered_program(parse_grammar_t *grammar, core_link_unit_t *link,
                        n00bb_link_analysis_t *analysis,
                        core_list_t(core_translation_unit_ptr_t) *unit_list,
                        bool ok, bool dump_control_flow, bool dump_pdg,
                        bool dump_summaries, double profile_start)
{
    core_list_t(core_translation_unit_ptr_t) units = *unit_list;
    core_list_t(core_translation_unit_ptr_t) init_order =
        core_list_new(core_translation_unit_ptr_t);
    bool members_bound = analysis->members_bound;
    n00bb_runtime_init_analysis_t runtime_init = {0};
    bool analyze_runtime_init = ok;
    if (analyze_runtime_init)
        ok = runtime_init_entries(grammar, analysis, &runtime_init);
    core_list_t(n00bb_ir_func_ptr_t) initializer_functions =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < units.len; i++) {
        ir_module_t *module = unit_module(tu_state(units.data[i]));
        ir_func_t *initializer = module ? n00bb_cached_body(ir_func_find(
            module, CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))) : nullptr;
        if (initializer) core_list_push(initializer_functions, initializer);
    }
    if (ok) ok = build_function_summaries(analysis, &initializer_functions,
                                          &runtime_init.entry_functions)
        && fold_function_effects(analysis);
    if (ok) {
        bool marks = check_marks(analysis);
        bool checks = evaluate_effect_checks(analysis);
        analysis->mark_questions.len = 0;
        analysis->effect_checks_failed = !checks || !marks;
    }
    core_list_free(initializer_functions);
    rule_facts_begin(analysis, link, &runtime_init.entry_functions);
    core_list_t(n00bb_initializer_read_t) initializer_reads =
        core_list_new(n00bb_initializer_read_t);
    if (ok) ok = collect_initializer_reads(analysis, &initializer_reads)
        && order_initializers(link, analysis, &initializer_reads,
                              &init_order);
    if (ok) {
        analysis->initializer_order.len = 0;
        for (size_t i = 0; i < init_order.len; i++)
            core_list_push(analysis->initializer_order, init_order.data[i]);
    }
    if (ok && analyze_runtime_init)
        ok = runtime_init_analysis_build(&init_order, &initializer_reads,
                                         &runtime_init);
    runtime_init.summaries = analysis->summaries;
    if (ok) {
        ir_analysis_read_only_begin();
        bool frozen = validate_frozen_writes(analysis);
        bool overflow = diagnose_overflow(analysis);
        bool ranges = diagnose_ranges(analysis);
        ir_analysis_read_only_end();
        warn_unbounded_recursion(analysis);
        warn_dead_stores(analysis);
        warn_unused_params(analysis);
        warn_rule_diagnostics(analysis);
        warn_shared_global_races(analysis, &runtime_init.entry_functions);
        ok = (frozen && overflow && ranges) || explained_by_errors(&units);
    }
    if (members_bound) n00bb_report_unused_items(analysis);
    if (ok) {
        bool returns = validate_missing_returns(analysis);
        bool asserts = validate_assert_side_effects(analysis);
        ok = ((returns && asserts) || explained_by_errors(&units))
            && n00bb_publish_function_summaries(analysis)
            && publish_declarations(analysis);
    }
    size_t program_scope = ir_analysis_scope_begin();
    if (ok && analyze_runtime_init && !build_program_graph(analysis,
                                                           &runtime_init)) {
        n00bb_internal_error(core_translation_unit_diagnostics(units.data[0]),
            units.data[0], (core_diag_location_t){}, "analyzing the program",
            "could not build the program's dependence graph");
        ok = false;
    }
    if (ok && analyze_runtime_init)
        ok = validate_uninitialized_global_reads(link, analysis,
            &runtime_init, &init_order, &initializer_reads)
            || explained_by_errors(&units);
    core_list_free(initializer_reads);
    if (ok && analyze_runtime_init) {
        ir_analysis_read_only_begin();
        ok = validate_thread_function_reads(link, analysis, &runtime_init,
                                            true)
            || explained_by_errors(&units);
        ir_analysis_read_only_end();
    }
    if (analyze_runtime_init) release_program_graph(analysis, &runtime_init);
    ir_analysis_scope_end(program_scope);
    rule_facts_end(analysis);
    if (analyze_runtime_init) runtime_init_analysis_release(&runtime_init);
    if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
        fprintf(stderr, "PROFILE lower-summaries %.3f\n",
            n00bb_profile_seconds() - profile_start);

    core_list_free(init_order);

    bool analyzed = ok;
    for (size_t i = 0; i < units.len; i++) {
        if (core_diagnostics_failed(
                core_translation_unit_diagnostics(units.data[i])))
            ok = false;
    }
    if (link && core_link_unit_has_fatal_diagnostics(link)) ok = false;
    analysis->lowered_with_errors = analyzed && !ok;

    if (ok) {
        for (size_t i = 0; i < units.len; i++) {
            if (dump_control_flow || dump_pdg)
                dump_ir_analysis(tu_state(units.data[i])->ir_module,
                                 dump_control_flow, dump_pdg);
        }
        if (dump_summaries) dump_function_summaries(analysis);
        if (analysis->effects_of.data
            && !dump_effects_of(analysis, units.data[0]))
            ok = false;
        if (analysis->effects_query.data
            && !dump_effects_query(analysis, units.data[0]))
            ok = false;
        for (size_t i = 0; analysis->dump_ranges && i < units.len; i++)
            dump_unit_ranges(units.data[i]);
        for (size_t f = 0; dump_control_flow
                 && f < analysis->reachable_functions.len; f++) {
            ir_func_t *function = analysis->reachable_functions.data[f];
            core_string_t owner = ir_module_name(function->owner);
            printf("reachable @%.*s.%.*s\n",
                (int)owner.u8_bytes, owner.data,
                (int)function->name.u8_bytes, function->name.data);
        }
    }
    for (size_t i = 0; i < units.len; i++)
        ir_module_release_analyses(tu_state(units.data[i])->ir_module);
    core_list_free(units);
    return ok;
}

bool
n00bb_register_callback_representation(ir_module_t *module, th_type *type)
{
    if (!module || !type) return false;
    if (ir_type_definition(module, type)) return true;
    core_string_t name = {};
    size_t arguments = 0;
    if (!th_named_info(th_resolve(type), &name, nullptr, &arguments)
        || !arguments) return false;
    th_type *base = name.data
        ? th_name(ir_module_types(module), name) : nullptr;
    th_type *definition = base ? ir_type_definition(module, base) : nullptr;
    return definition && ir_type_define(module, type, definition);
}

bool
n00bb_static_value_to_ir_initializer(const n00bb_static_value_t *value,
                                    th_type *type,
                                    n00bb_tu_state_t *state,
                                    ir_global_init_value_t *out,
                                    size_t depth)
{
    if (!value || !type || !state || !out) return false;
    type = th_resolve(type);
    if (!type) return false;
    if (n00bb_atomic_element_type(type))
        return n00bb_static_value_to_ir_initializer(value,
            n00bb_atomic_element_type(type), state, out, depth + 1);
    th_type *option_element = th_option_item(type);
    if (value->kind != N00BB_STATIC_VALUE_NIL && option_element)
        return n00bb_static_value_to_ir_initializer(value, option_element,
                                                    state, out, depth + 1);
    *out = (ir_global_init_value_t){0};
    switch (value->kind) {
    case N00BB_STATIC_VALUE_BOOL:
        out->kind = IR_GLOBAL_INIT_BOOL;
        out->boolean = value->as.boolean;
        return true;
    case N00BB_STATIC_VALUE_INT:
        out->kind = IR_GLOBAL_INIT_INT;
        out->integer.neg = value->as.integer < 0;
        out->integer.mag = value->as.integer < 0
            ? (ir_uint_t)(-(value->as.integer + 1)) + 1
            : (ir_uint_t)value->as.integer;
        return true;
    case N00BB_STATIC_VALUE_FLOAT:
        out->kind = IR_GLOBAL_INIT_FLOAT;
        out->floating = value->as.real;
        return true;
    case N00BB_STATIC_VALUE_STRING:
        out->kind = IR_GLOBAL_INIT_STRING;
        out->string = value->as.string;
        return true;
    case N00BB_STATIC_VALUE_NIL:
        if (!th_is_nil(type) && !th_option_item(type)) return false;
        out->kind = IR_GLOBAL_INIT_NIL;
        return true;
    case N00BB_STATIC_VALUE_VARIANT: {
        bool variant = false;
        th_type *shape = static_record_shape(state, type);
        if (!th_named_info(type, nullptr, &variant, nullptr)
            || !variant || !shape || th_kind_of(shape) != TH_RECORD
            || !value->as.variant.payload) return false;
        for (size_t i = 0; i < th_record_len(shape); i++) {
            const th_member *arm = th_record_member(shape, i);
            if (!arm || !core_string_eq(value->as.variant.arm, arm->name))
                continue;
            ir_global_init_value_t *payload = core_arena_alloc(
                &state->semantic_values, sizeof(*payload));
            if (!payload || !n00bb_static_value_to_ir_initializer(
                    value->as.variant.payload, arm->type, state, payload,
                    depth + 1)) return false;
            payload->member = arm->name;
            out->kind = IR_GLOBAL_INIT_VARIANT;
            out->aggregate.items = payload;
            out->aggregate.count = 1;
            return true;
        }
        return false;
    }
    case N00BB_STATIC_VALUE_ADDRESS: {
        parse_sym_entry_t *target = value->as.address;
        th_type *referent = th_kind_of(type) == TH_INDIRECTION
            ? th_ind_referent(type) : nullptr;
        if (!target || !target->owner_unit || !referent || !state->ir_module)
            return false;
        core_buffer_t name = {0};
        append_module_symbol_name(target->owner_unit, target, &name);
        ir_global_t *global = ir_global_find(state->ir_module,
                                             core_buffer_string(&name));
        core_buffer_release(&name);
        ir_value_t *symbol = global
            ? ir_global_address(state->ir_module, global) : nullptr;
        if (!symbol) return false;
        out->kind = IR_GLOBAL_INIT_SYMBOL;
        out->symbol = symbol;
        return true;
    }
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD:
        break;
    }

    size_t count = value->as.aggregate.count;
    if (count && !value->as.aggregate.items) return false;
    ir_global_init_value_t *items = count
        ? core_arena_alloc(&state->semantic_values, count * sizeof(*items))
        : nullptr;
    if (count && !items) return false;
    if (count) memset(items, 0, count * sizeof(*items));
    out->aggregate.items = items;
    out->aggregate.count = count;

    if (value->kind == N00BB_STATIC_VALUE_SEQUENCE) {
        th_type *element = static_sequence_element_type(type);
        if (!element) return false;
        out->kind = IR_GLOBAL_INIT_SEQUENCE;
        for (size_t i = 0; i < count; i++)
            if (!n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], element, state,
                    &items[i], depth + 1)) return false;
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_TUPLE) {
        if (!th_builtin_applied(type, TH_BUILTIN_TUPLE, count)) return false;
        out->kind = IR_GLOBAL_INIT_TUPLE;
        for (size_t i = 0; i < count; i++) {
            th_type *element = th_tuple_element(type, i, nullptr);
            if (!element || !n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], element, state,
                    &items[i], depth + 1)) return false;
        }
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_RECORD) {
        th_type *shape = static_record_shape(state, type);
        if (!shape || th_kind_of(shape) != TH_RECORD
            || count != th_record_len(shape)) return false;
        out->kind = IR_GLOBAL_INIT_RECORD;
        for (size_t i = 0; i < count; i++) {
            const th_member *member = th_record_member(shape, i);
            if (!member || !n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], member->type, state,
                    &items[i], depth + 1)) return false;
            items[i].member = member->name;
        }
        return true;
    }
    return false;
}
