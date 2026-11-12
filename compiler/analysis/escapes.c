#include "lower/lower_internal.h"

static void
merge_call_return(void *context, const ir_func_t *target,
                  const ir_summary_t *summary);
static bool
mark_external_call_return(n00bb_link_analysis_t *analysis,
                          ir_func_t *function, ir_instr_t *call);
static bool
span_within(ir_span_t inner, ir_span_t outer);
static bool
rule_step_seen(n00bb_rule_chain_t *chain, const ir_summary_step_t *step);
static core_string_t
describe_callee(const ir_func_t *callee);
static core_string_t
describe_summary_place(const ir_splace_t *place);
static core_string_t
describe_flag(core_string_t flag);

bool
build_escape_facts(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++)
            core_list_push(roots, ir_func_at(state->ir_module, f));
    }
    bind_summary_hooks(analysis, true);
    n00bb_push_summary_roots(analysis, &roots);
    ir_summaries_free(analysis->escapes);
    analysis->summary_phase = N00BB_PHASE_ESCAPES;
    analysis->escapes = ir_escapes_compute(roots.data, roots.len,
                                           &analysis->summary_hooks);
    analysis->summary_phase = 0;
    ir_analysis_bind_summaries(analysis->escapes);
    core_list_free(roots);
    return analysis->escapes != nullptr;
}

bool
mark_diverging_calls(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        ir_module_t *module = state->ir_module;
        for (size_t f = 0; f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                    ir_instr_t *call = function->blocks[b]->instrs[i];
                    if ((call->op != IR_OP_CALL
                         && call->op != IR_OP_CALLBACK_INVOKE)
                        || !ir_summary_call_diverges(analysis->escapes, call))
                        continue;
                    if (!ir_annot_set(analysis->ir_context,
                            &call->annots, (ir_annot_t){
                                .key = CORE_STRING_STATIC("ir.diverges"),
                                .kind = IR_ANNOT_BOOL, .b = true}))
                        return false;
                }
        }
    }
    return true;
}

static void
merge_call_return(void *context, const ir_func_t *target,
                  const ir_summary_t *summary)
{
    (void)target;
    call_return_t *outcome  = context;
    ir_sreturn_t   returned = ir_summary_return(summary);
    bool same = returned.kind == outcome->returned.kind
        && returned.param == outcome->returned.param
        && returned.global == outcome->returned.global;
    if (outcome->targets++ && !same)
        returned = (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    outcome->returned = returned;
}

static bool
mark_external_call_return(n00bb_link_analysis_t *analysis,
                          ir_func_t *function, ir_instr_t *call)
{
    ir_value_t *result = call->dest_kind == IR_DEST_VALUE ? call->dest
                                                          : nullptr;
    if (!result || !type_may_contain_reference_alias(
            ir_module_types(function->owner), result->type)) return true;
    call_return_t outcome = {0};
    if (!ir_summary_call_callees(analysis->escapes, call, merge_call_return,
                                 &outcome)
        || !outcome.targets
        || outcome.returned.kind != IR_SRETURN_FRESH_EXTERNAL) return true;
    return ir_value_set_external_writable_storage(analysis->ir_context,
                                                  result);
}

bool
mark_external_call_returns(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        ir_module_t *module = state->ir_module;
        for (size_t f = 0; f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                    ir_instr_t *in = function->blocks[b]->instrs[i];
                    if ((in->op == IR_OP_CALL
                         || in->op == IR_OP_CALLBACK_INVOKE)
                        && !mark_external_call_return(analysis, function, in))
                        return false;
                }
        }
    }
    return true;
}

core_string_t
ir_value_source_name(const ir_value_t *value)
{
    if (!value) return (core_string_t){0};
    if (value->local && value->local->source_name.u8_bytes
        && !value->local->synthetic)
        return value->local->source_name;
    if (value->global) return n00bb_name_leaf(value->global->name);
    return (core_string_t){0};
}

typedef ir_local_t *local_ptr_t;
core_list_decl(local_ptr_t);

static bool
finalizes_local(const ir_instr_t *in)
{
    const ir_block_t *block = in->block;
    const ir_instr_t *next = in->op == IR_OP_ADDRESS_OF && in->dest
        && in->index + 1 < block->n_instrs ? block->instrs[in->index + 1]
                                           : nullptr;
    return next && next->op == IR_OP_CONSUME && next->n_operands == 1
        && ir_operand_value(&next->operands[0]) == in->dest;
}

static bool
reads_local(const ir_instr_t *in, const ir_local_t *local)
{
    if (finalizes_local(in)) return false;
    for (uint32_t o = 0; o < in->n_operands; o++) {
        if (in->op == IR_OP_CONSUME && o == 1) continue;
        if (ir_operand_local(&in->operands[o]) == local)
            return true;
    }
    return false;
}

static bool
writes_local(const ir_instr_t *in, const ir_local_t *local)
{
    return in->dest_kind == IR_DEST_LOCAL && in->dest_local == local;
}

static ir_local_t *
copied_local(const ir_instr_t *write)
{
    const ir_value_t *value = write->op == IR_OP_COPY && write->n_operands
        ? ir_operand_value(&write->operands[0]) : nullptr;
    for (uint32_t depth = 0; value && value->kind == IR_VAL_INSTR
         && value->def && depth < 32; depth++) {
        ir_instr_t *def = value->def;
        if (def->op != IR_OP_COPY && def->op != IR_OP_NOMINAL_CAST
            && def->op != IR_OP_CAST) return nullptr;
        ir_local_t *local = def->n_operands
            ? ir_operand_local(&def->operands[0]) : nullptr;
        if (local) return local;
        value = def->n_operands ? ir_operand_value(&def->operands[0])
                                : nullptr;
    }
    return nullptr;
}

static void
collect_aliases(ir_func_t *function, ir_local_t *consumed,
                core_list_t(local_ptr_t) *aliases)
{
    th_ctx *types = ir_module_types(function->owner);
    core_list_push(*aliases, consumed);
    if (!n00bb_type_reaches_shared_state(types, consumed->type)) return;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                ir_local_t *source = in->dest_kind == IR_DEST_LOCAL
                    ? copied_local(in) : nullptr;
                bool from_alias = false, known = false;
                for (size_t a = 0; a < aliases->len; a++) {
                    from_alias |= aliases->data[a] == source;
                    known |= aliases->data[a] == in->dest_local;
                }
                if (!from_alias || known) continue;
                core_list_push(*aliases, in->dest_local);
                grew = true;
            }
    }
}

static const ir_instr_t *
read_after(const ir_instr_t *consume, const ir_local_t *local)
{
    core_dict_t visited;
    core_dict_init(&visited, nullptr, nullptr);
    core_list_t(n00bb_ir_block_ptr_t) pending =
        core_list_new(n00bb_ir_block_ptr_t);
    const ir_instr_t *found = nullptr;
    ir_block_t *block = consume->block;
    uint32_t start = consume->index + 1;
    while (block && !found) {
        bool stopped = false;
        for (uint32_t i = start; !stopped && !found && i < block->n_instrs;
             i++) {
            const ir_instr_t *in = block->instrs[i];
            if (reads_local(in, local) && ir_span_known(in->span)
                && !in->span.implied)
                found = in;
            else stopped = writes_local(in, local);
        }
        ir_instr_t *end = stopped || found ? nullptr
                                           : ir_block_terminator(block);
        for (uint32_t s = 0; end && s < ir_succ_count(end); s++) {
            ir_block_t *next = ir_succ_at(end, s);
            if (next && core_dict_add(&visited, next, next))
                core_list_push(pending, next);
        }
        block = pending.len ? pending.data[--pending.len] : nullptr;
        start = 0;
    }
    core_dict_free(&visited);
    core_list_free(pending);
    return found;
}

static bool
check_use_after_consume(core_translation_unit_t *unit, ir_func_t *function,
                        const ir_instr_t *consume, core_dict_t *reported)
{
    bool moved = consume->n_operands == 1
        && ir_annot_get((ir_annots_t *)&consume->annots,
                        CORE_STRING_STATIC("n00bb.move"));
    ir_local_t *consumed = consume->n_operands == 2
        ? ir_operand_local(&consume->operands[1])
        : moved ? ir_operand_local(&consume->operands[0]) : nullptr;
    if (!consumed || consumed->synthetic || !consumed->source_name.u8_bytes)
        return true;
    core_list_t(local_ptr_t) aliases = core_list_new(local_ptr_t);
    collect_aliases(function, consumed, &aliases);
    bool ok = true;
    for (size_t a = 0; a < aliases.len; a++) {
        ir_local_t *local = aliases.data[a];
        const ir_instr_t *read = local->synthetic ? nullptr
            : read_after(consume, local);
        if (!read || !core_dict_add(reported, (void *)read, (void *)read))
            continue;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        n00bb_report(diagnostics, unit, N00B_DIAG_USE_AFTER_CONSUME,
            n00bb_span_location(read->span),
            moved ? N00B_TEXT_USED_AFTER_MOVE : N00B_TEXT_USED_AFTER_CONSUME,
            N00BB_ARGS(ARG_TEXT(local->source_name)));
        n00bb_report_secondary(diagnostics, nullptr,
            n00bb_span_location(consume->span),
            moved ? N00B_TEXT_IS_MOVED_HERE : N00B_TEXT_IS_CONSUMED_HERE,
            N00BB_ARGS(ARG_TEXT(consumed->source_name)));
        n00bb_report_help(diagnostics, moved
            ? N00B_TEXT_A_MOVED_VALUE_CANNOT_BE
            : N00B_TEXT_A_CONSUMED_VALUE_CANNOT_BE, N00BB_NO_ARGS);
        ok = false;
    }
    core_list_free(aliases);
    return ok;
}

static bool
check_place_copy(core_translation_unit_t *unit, ir_func_t *function,
                 ir_instr_t *in)
{
    ir_annot_t *copy = ir_annot_get(&in->annots,
        CORE_STRING_STATIC("n00bb.place_copy"));
    if (!copy || copy->kind != IR_ANNOT_STR || !in->dest
        || !n00bb_type_finalized(unit, in->dest->type))
        return true;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t type_text = n00bb_print_type(
        ir_module_types(function->owner), in->dest->type);
    n00bb_report(diagnostics, unit, N00B_DIAG_FINALIZED_COPY,
        n00bb_span_location(in->span), N00B_TEXT_FINALIZED_VALUE_COPIED,
        N00BB_ARGS(ARG_TEXT(type_text), ARG_TEXT(copy->s)));
    n00bb_report_help(diagnostics, N00B_TEXT_MOVE_THE_WHOLE_VARIABLE,
                      N00BB_NO_ARGS);
    return false;
}

bool
validate_consumed_bindings(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->ir_module || state->interface_module) continue;
        ir_diags_t *diagnostics = attach_ir_diags(analysis->ir_context,
            nullptr, unit, nullptr);
        if (!diagnostics) return false;
        ir_module_t *module = state->ir_module;
        core_dict_t reported;
        core_dict_init(&reported, nullptr, nullptr);
        for (size_t f = 0; f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                for (uint32_t i = 0; i < block->n_instrs; i++) {
                    ir_instr_t *in = block->instrs[i];
                    ok = check_place_copy(unit, function, in) && ok;
                    if (in->op == IR_OP_CONSUME)
                        ok = check_use_after_consume(unit, function, in,
                                                     &reported) && ok;
                    if (in->op != IR_OP_CONSUME || in->n_operands != 2
                        || in->operands[1].kind != IR_OPD_VALUE
                        || !n00bb_binding_escapes(analysis->escapes,
                               function, in->operands[1].value)) continue;
                    core_string_t name = ir_value_source_name(
                        in->operands[1].value);
                    core_diagnostics_t *destination =
                        core_translation_unit_diagnostics(unit);
                    if (name.u8_bytes)
                        n00bb_report(destination, unit,
                            N00B_DIAG_CONSUME_ESCAPED, n00bb_span_location(in->span),
                            N00B_TEXT_CANNOT_BE_CONSUMED_HERE_A,
                            N00BB_ARGS(ARG_TEXT(name)));
                    else
                        n00bb_report(destination, unit,
                            N00B_DIAG_CONSUME_ESCAPED, n00bb_span_location(in->span),
                            N00B_TEXT_THIS_VALUE_CANNOT_BE_CONSUMED,
                            N00BB_NO_ARGS);
                    n00bb_report_help(destination,
                        N00B_TEXT_PASS_A_COPY_INSTEAD_OR,
                        N00BB_NO_ARGS);
                    ok = false;
                }
            }
        }
        core_dict_free(&reported);
        if (ir_diags_failed(diagnostics)) ok = false;
    }
    return ok;
}

static bool
span_within(ir_span_t inner, ir_span_t outer)
{
    return inner.file == outer.file && inner.file >= 0
        && inner.start_byte >= outer.start_byte
        && inner.end_byte <= outer.end_byte;
}

static bool
initializes_own_storage(const ir_instr_t *call, ir_span_t span)
{
    const ir_func_t *callee = call->n_operands > 1 ? ir_call_function(call)
                                                   : nullptr;
    const ir_value_t *target = callee
            && core_string_eq(callee->name, CORE_STRING_STATIC("__init"))
        ? ir_operand_value(&call->operands[1]) : nullptr;
    for (uint32_t depth = 0; target && target->kind == IR_VAL_INSTR
         && target->def && depth < 8; depth++) {
        const ir_instr_t *def = target->def;
        if (def->op == IR_OP_ALLOC || def->op == IR_OP_ALLOCA)
            return span_within(def->span, span);
        if ((def->op != IR_OP_NOMINAL_CAST && def->op != IR_OP_COPY)
            || !def->n_operands) return false;
        target = ir_operand_value(&def->operands[0]);
    }
    return false;
}

const ir_instr_t *
assert_side_effect_call(ir_summary_facts_t *facts, n00bb_assert_site_t *site)
{
    ir_func_t *function = site->function;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            if ((in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                && span_within(in->span, site->span)
                && !initializes_own_storage(in, site->span)
                && ir_summary_facts_side_effect(facts, in))
                return in;
        }
    return nullptr;
}

void
report_chain(core_diagnostics_t *diagnostics,
             const core_translation_unit_t *unit, n00bb_diag_t id,
             core_diag_location_t at, n00bb_text_t plain,
             n00bb_text_t explained, n00bb_rule_chain_t *chain)
{
    n00bb_report(diagnostics, unit, id, at,
        chain->clauses.len ? explained : plain,
        N00BB_ARGS(ARG_CLAUSES(chain->clauses.data, chain->clauses.len)));
    for (size_t c = 0; c < chain->clauses.len; c++)
        core_free(chain->clauses.data[c].data);
    core_list_free(chain->clauses);
}

static bool
rule_step_seen(n00bb_rule_chain_t *chain, const ir_summary_step_t *step)
{
    size_t clauses = chain->clauses.len;
    size_t count = clauses < 16 ? clauses : 16;
    for (size_t s = 0; s < count; s++) {
        const ir_summary_step_t *seen = &chain->seen[s];
        if (seen->kind == step->kind && seen->callee == step->callee
            && seen->param == step->param
            && core_string_eq(seen->name, step->name)
            && seen->place.kind == step->place.kind
            && seen->place.global == step->place.global
            && seen->place.param == step->place.param) return true;
    }
    if (clauses < 16) chain->seen[clauses] = *step;
    return false;
}

static core_string_t
describe_callee(const ir_func_t *callee)
{
    if (callee)
        return n00bb_text(N00B_TEXT_QUOTED_NAME,
                          N00BB_ARGS(ARG_TEXT(callee->name)));
    return n00bb_text(N00B_TEXT_UNSEEN_CODE, N00BB_NO_ARGS);
}

static core_string_t
describe_summary_place(const ir_splace_t *place)
{
    switch (place->kind) {
    case IR_SPLACE_PARAM:
        return n00bb_text(N00B_TEXT_PLACE_ARGUMENT,
                          N00BB_ARGS(ARG_NUMBER(place->param + 1)));
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        if (place->global)
            return n00bb_text(place->kind == IR_SPLACE_THREAD
                    ? N00B_TEXT_PLACE_THREAD_GLOBAL : N00B_TEXT_PLACE_GLOBAL,
                N00BB_ARGS(ARG_TEXT(n00bb_name_leaf(place->global->name))));
        break;
    case IR_SPLACE_HEAP:
        return n00bb_text(N00B_TEXT_PLACE_HEAP, N00BB_NO_ARGS);
    case IR_SPLACE_UNKNOWN:
    case IR_SPLACE_SOURCE:
        break;
    }
    return n00bb_text(N00B_TEXT_PLACE_UNOWNED, N00BB_NO_ARGS);
}

static core_string_t
describe_flag(core_string_t flag)
{
    static const struct {
        uint64_t bit;
        n00bb_text_t text;
    } phrases[] = {
        {IR_SUMMARY_DIVERGES, N00B_TEXT_FLAG_DIVERGES},
        {IR_SUMMARY_CALLS_UNKNOWN, N00B_TEXT_FLAG_CALLS_UNKNOWN},
        {IR_SUMMARY_MAY_FAIL, N00B_TEXT_FLAG_MAY_FAIL},
        {IR_SUMMARY_MAY_TRAP, N00B_TEXT_FLAG_MAY_TRAP},
        {IR_SUMMARY_MAY_BLOCK, N00B_TEXT_FLAG_MAY_BLOCK},
        {IR_SUMMARY_MAY_IO, N00B_TEXT_FLAG_MAY_IO},
        {IR_SUMMARY_MAY_OUTPUT, N00B_TEXT_FLAG_MAY_OUTPUT},
        {IR_SUMMARY_MAY_INPUT, N00B_TEXT_FLAG_MAY_INPUT},
        {IR_SUMMARY_CALLS_FINALIZERS, N00B_TEXT_FLAG_CALLS_FINALIZERS},
        {IR_SUMMARY_STARTS_THREADS, N00B_TEXT_FLAG_STARTS_THREADS},
    };
    uint64_t bits = 0;
    if (ir_summary_effect_lookup(flag, &bits))
        for (size_t p = 0; p < sizeof(phrases) / sizeof(*phrases); p++)
            if (phrases[p].bit == bits)
                return n00bb_text(phrases[p].text, N00BB_NO_ARGS);
    return n00bb_text(N00B_TEXT_FLAG_OTHER, N00BB_ARGS(ARG_TEXT(flag)));
}

bool
describe_rule_step(void *context, const ir_summary_step_t *step)
{
    n00bb_rule_chain_t *chain = context;
    if (step->kind == IR_SUMMARY_STEP_RULE
        || step->kind == IR_SUMMARY_STEP_DISCARDED
        || rule_step_seen(chain, step)) return true;
    core_string_t detail = {0};
    n00bb_text_t clause = N00B_TEXT_CLAUSE_OUT_ARGUMENT;
    switch (step->kind) {
    case IR_SUMMARY_STEP_OUT_ARGUMENT:
        break;
    case IR_SUMMARY_STEP_CALLS:
    case IR_SUMMARY_STEP_CALLS_UNSEEN:
        clause = chain->ignored ? N00B_TEXT_CLAUSE_CHANGES_NOTHING
                                : N00B_TEXT_CLAUSE_CALLS;
        break;
    case IR_SUMMARY_STEP_FLAG:
        clause = N00B_TEXT_CLAUSE_FLAG;
        detail = describe_flag(step->name);
        break;
    case IR_SUMMARY_STEP_ACCESS: {
        bool write = core_string_eq(step->name, CORE_STRING_STATIC("write"));
        clause = write ? N00B_TEXT_CLAUSE_WRITES : N00B_TEXT_CLAUSE_READS;
        detail = describe_summary_place(&step->place);
        if (write && !chain->writer && step->callee
            && (step->place.kind == IR_SPLACE_GLOBAL
                || step->place.kind == IR_SPLACE_THREAD)) {
            chain->writer = step->callee;
            chain->written = step->place.global;
        }
        break;
    }
    case IR_SUMMARY_STEP_ESCAPE:
        clause = N00B_TEXT_CLAUSE_ESCAPES;
        break;
    case IR_SUMMARY_STEP_ADDRESSABLE:
        clause = N00B_TEXT_CLAUSE_ADDRESSABLE;
        break;
    case IR_SUMMARY_STEP_PLACE:
        clause = N00B_TEXT_CLAUSE_PLACE_VISIBLE;
        detail = describe_summary_place(&step->place);
        break;
    case IR_SUMMARY_STEP_RETURNS:
        clause = N00B_TEXT_CLAUSE_RETURNS_ARGUMENT;
        break;
    case IR_SUMMARY_STEP_DEPENDS:
        clause = N00B_TEXT_CLAUSE_DEPENDS;
        break;
    case IR_SUMMARY_STEP_RULE:
    case IR_SUMMARY_STEP_DISCARDED:
        break;
    }
    core_string_t callee = describe_callee(step->callee);
    core_list_push(chain->clauses, n00bb_text(clause,
        N00BB_ARGS(ARG_TEXT(callee), ARG_TEXT(detail),
                   ARG_NUMBER(step->param + 1))));
    core_free(callee.data);
    core_free(detail.data);
    return true;
}

bool
call_diverges_by_rule(void *context, const ir_instr_t *call)
{
    dead_code_context_t *dead = context;
    return dead->facts && ir_summary_facts_call_diverges(dead->facts, call);
}
