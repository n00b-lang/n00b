#include "compiler_marks.h"
#include "cached_body.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/pdg.h"
#include "ir/pdg_memory.h"
#include "ir/sites.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "th/typehash.h"

enum { MARK_PARAMS = 64 };

typedef const ir_instr_t *instr_ptr_t;
core_list_decl(instr_ptr_t);

typedef struct {
    uint64_t marks;
    uint64_t params;
} taint_t;

typedef struct {
    const ir_value_t *argument;
    ir_block_t       *ok;
    uint64_t          marks;
} validation_t;
core_list_decl(validation_t);

typedef struct {
    const ir_value_t *base;
    const ir_value_t *value;
    const ir_instr_t *at;
} write_t;
core_list_decl(write_t);

typedef struct {
    uint64_t                  result_marks;
    uint64_t                  result_params;
    uint64_t                 *rejects;
    const ir_instr_t        **sinks;
    core_list_t(validation_t) validations;
} function_facts_t;

typedef struct {
    n00bb_marks_t *marks;
    core_dict_t    facts;
    bool           changed;
} marks_state_t;

typedef struct {
    marks_state_t        *state;
    ir_func_t            *function;
    function_facts_t     *facts;
    core_dict_t           values;
    ir_doms_t            *doms;
    ir_pdg_memory_t      *memssa;
    core_list_t(write_t)  writes;
    bool                  reporting;
    bool                  changed;
} function_state_t;

static uint64_t
rule_marks(const core_list_t(n00bb_mark_rule_t) *rules,
           const ir_func_t *function, uint32_t param)
{
    uint64_t marks = 0;
    for (size_t r = 0; function && r < rules->len; r++)
        if (rules->data[r].function == function
            && (param == UINT32_MAX || rules->data[r].param == param))
            marks |= rules->data[r].marks;
    return marks;
}

static function_facts_t *
facts_of(marks_state_t *state, const ir_func_t *function)
{
    bool found = false;
    function_facts_t *facts = function
        ? core_dict_get(&state->facts, (void *)function, &found) : nullptr;
    return found ? facts : nullptr;
}

static uint64_t
carried(const n00bb_marks_t *marks, th_type *type, core_string_t field)
{
    uint64_t found = 0;
    for (int depth = 0; marks->carriers.len && type && depth < 4; depth++) {
        th_type *resolved = th_resolve(type);
        core_string_t name = {0};
        bool named = th_named_info(resolved, &name, nullptr, nullptr);
        for (size_t c = 0; named && c < marks->carriers.len; c++) {
            const n00bb_mark_carrier_t *carrier = &marks->carriers.data[c];
            if (core_string_eq(carrier->type, name)
                && core_string_eq(carrier->field, field))
                found |= carrier->marks;
        }
        type = th_ind_referent(resolved);
    }
    return found;
}

static uint64_t
seeded(function_state_t *fs, const ir_value_t *value)
{
    return value->kind == IR_VAL_PARAM
        ? rule_marks(&fs->state->marks->seeds, fs->function, value->index) : 0;
}

static uint64_t
field_marks(const n00bb_marks_t *marks, const ir_instr_t *in)
{
    if ((in->op != IR_OP_FIELD && in->op != IR_OP_EXTRACT)
        || in->n_operands != 2 || in->operands[1].kind != IR_OPD_NAME)
        return 0;
    const ir_value_t *base = ir_operand_value(&in->operands[0]);
    return carried(marks, base ? base->type : nullptr, in->operands[1].name);
}

static taint_t
stored_taint(function_state_t *fs, const ir_value_t *value)
{
    bool found = false;
    taint_t *taint = value
        ? core_dict_get(&fs->values, (void *)value, &found) : nullptr;
    taint_t result = found ? *taint : (taint_t){0};
    if (!value) return result;
    result.marks |= carried(fs->state->marks, value->type, (core_string_t){0})
        | seeded(fs, value);
    if (value->kind == IR_VAL_PARAM && value->index < MARK_PARAMS)
        result.params |= 1ull << value->index;
    return result;
}

static void
merge(taint_t *into, taint_t from)
{
    into->marks |= from.marks;
    into->params |= from.params;
}

static void
record(function_state_t *fs, const ir_value_t *value, taint_t taint)
{
    if (!value || (!taint.marks && !taint.params)) return;
    bool found = false;
    taint_t *old = core_dict_get(&fs->values, (void *)value, &found);
    if (!found) {
        old = core_alloc(taint_t);
        *old = taint;
        core_dict_put(&fs->values, (void *)value, old);
        fs->changed = true;
        return;
    }
    if ((old->marks | taint.marks) == old->marks
        && (old->params | taint.params) == old->params) return;
    merge(old, taint);
    fs->changed = true;
}

typedef struct {
    core_list_t(instr_ptr_t) stores;
    bool                     known;
} reaching_t;

static bool
collect_store(void *context, const ir_memdef_t *def)
{
    reaching_t *reaching = context;
    if (def->kind != IR_MEMDEF_STORE || !def->instr) {
        reaching->known = false;
        return false;
    }
    core_list_push(reaching->stores, def->instr);
    return true;
}

static bool
slot_address(const ir_value_t *slot)
{
    const ir_instr_t *alloc = slot && slot->kind == IR_VAL_INSTR
        ? slot->def : nullptr;
    return alloc && (alloc->op == IR_OP_ALLOCA || alloc->op == IR_OP_ALLOC);
}

static bool
slot_load(const ir_value_t *value)
{
    const ir_instr_t *def = value && value->kind == IR_VAL_INSTR
        ? value->def : nullptr;
    return def && def->op == IR_OP_LOAD && def->n_operands
        && slot_address(ir_operand_value(&def->operands[0]));
}

static reaching_t
reaching_stores(function_state_t *fs, const ir_instr_t *load)
{
    reaching_t reaching = {.stores = core_list_new(instr_ptr_t),
                           .known = true};
    if (!fs->memssa
        || !ir_pdg_memory_visit_clobbers(fs->memssa,
               ir_pdg_memory_access_of(fs->memssa, load, IR_ACCESS_LOAD),
               collect_store, &reaching))
        reaching.known = false;
    return reaching;
}

static bool
same_stores(const reaching_t *a, const reaching_t *b)
{
    if (!a->known || !b->known || a->stores.len != b->stores.len) return false;
    for (size_t i = 0; i < a->stores.len; i++) {
        bool found = false;
        for (size_t j = 0; !found && j < b->stores.len; j++)
            found = a->stores.data[i] == b->stores.data[j];
        if (!found) return false;
    }
    return true;
}

static const ir_value_t *
derived_root(const ir_value_t *value, bool addresses)
{
    for (int depth = 0; value && depth < 32; depth++) {
        const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def
                                                           : nullptr;
        bool derived = def && def->n_operands
            && (def->op == IR_OP_COPY
                || (addresses && (def->op == IR_OP_FIELD
                                  || def->op == IR_OP_INDEX
                                  || def->op == IR_OP_ADDRESS_OF)));
        const ir_value_t *source = derived
            ? ir_operand_value(&def->operands[0]) : nullptr;
        if (!source) return value;
        value = source;
    }
    return value;
}

static bool
same_value(function_state_t *fs, const ir_value_t *a, const ir_value_t *b)
{
    if (a == b) return true;
    const ir_value_t *root = derived_root(a, false);
    if (root && root->kind != IR_VAL_LOCAL && root == derived_root(b, false))
        return true;
    if (!slot_load(a) || !slot_load(b)
        || ir_operand_value(&a->def->operands[0])
               != ir_operand_value(&b->def->operands[0]))
        return false;
    reaching_t left = reaching_stores(fs, a->def);
    reaching_t right = reaching_stores(fs, b->def);
    bool same = same_stores(&left, &right);
    core_list_free(left.stores);
    core_list_free(right.stores);
    return same;
}

static uint64_t
cleared(function_state_t *fs, const ir_value_t *value, ir_block_t *block)
{
    uint64_t marks = 0;
    for (size_t v = 0; value && v < fs->facts->validations.len; v++) {
        const validation_t *validation = &fs->facts->validations.data[v];
        if ((marks | validation->marks) != marks
            && ir_dominates(fs->doms, validation->ok, block)
            && same_value(fs, validation->argument, value))
            marks |= validation->marks;
    }
    return marks;
}

static taint_t
read_value(function_state_t *fs, const ir_value_t *value, ir_block_t *block)
{
    taint_t taint = stored_taint(fs, value);
    if (taint.marks) taint.marks &= ~cleared(fs, value, block);
    return taint;
}

static ir_block_t *
ok_block(const ir_instr_t *call)
{
    ir_instr_t *branch = call->block ? ir_block_terminator(call->block)
                                     : nullptr;
    bool success_first = false;
    return ir_call_outcome_branch(branch, &success_first) == call
        ? ir_succ_at(branch, success_first ? 0 : 1) : nullptr;
}

static void
collect_validations(n00bb_marks_t *marks, ir_func_t *function,
                    core_list_t(validation_t) *validations)
{
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            ir_func_t *callee = n00bb_direct_call_target(in);
            for (size_t r = 0; callee && r < marks->clears.len; r++) {
                const n00bb_mark_rule_t *rule = &marks->clears.data[r];
                ir_block_t *ok = rule->function == callee
                        && rule->param + 1 < in->n_operands
                    ? ok_block(in) : nullptr;
                if (ok)
                    core_list_push(*validations, ((validation_t){
                        .argument = ir_operand_value(
                            &in->operands[rule->param + 1]),
                        .ok = ok, .marks = rule->marks}));
            }
        }
}

static bool
flows_from(const ir_summary_t *summary, ir_sflow_table_t table,
           uint32_t flow, uint32_t param)
{
    for (uint32_t s = 0; s < ir_summary_flow_source_count(summary, table);
         s++) {
        const ir_spath_t *source = ir_summary_flow_source(summary, table, s);
        if (source->root == IR_SPLACE_PARAM && source->param == param
            && (ir_summary_flow_has(summary, table, flow, s, IR_SFLOW_DATA)
                || ir_summary_flow_has(summary, table, flow, s,
                                       IR_SFLOW_COPY)
                || ir_summary_flow_has(summary, table, flow, s,
                                       IR_SFLOW_ALIAS)))
            return true;
    }
    return false;
}

static bool
param_reaches_result(const ir_summary_t *summary, uint32_t param)
{
    for (uint32_t f = 0;
         f < ir_summary_flow_count(summary, IR_SFLOW_RESULTS); f++)
        if (flows_from(summary, IR_SFLOW_RESULTS, f, param)) return true;
    return false;
}

static ir_summary_t *
summary_of(marks_state_t *state, ir_func_t *callee)
{
    return callee && state->marks->summaries
        ? ir_summary_of(state->marks->summaries, callee) : nullptr;
}

static bool
passes_through(marks_state_t *state, ir_func_t *callee,
               const function_facts_t *facts, uint32_t param)
{
    if (!callee) return true;
    if (facts)
        return param < MARK_PARAMS && (facts->result_params >> param & 1);
    ir_summary_t *summary = summary_of(state, callee);
    return !summary || param_reaches_result(summary, param);
}

static void
write_memory(function_state_t *fs, const ir_value_t *address,
             const ir_value_t *value, taint_t taint, const ir_instr_t *at)
{
    const ir_value_t *base = derived_root(address, true);
    if (!base || (!taint.marks && !taint.params)) return;
    record(fs, base, taint);
    if (fs->reporting)
        core_list_push(fs->writes, ((write_t){
            .base = base, .value = value, .at = at}));
}

static void
store_flows(function_state_t *fs, ir_instr_t *call, ir_func_t *callee,
            ir_block_t *block)
{
    ir_summary_t *summary = summary_of(fs->state, callee);
    for (uint32_t f = 0;
         summary && f < ir_summary_flow_count(summary, IR_SFLOW_STORES);
         f++) {
        const ir_spath_t *to = ir_summary_flow_to(summary, IR_SFLOW_STORES,
                                                  f);
        if (to->root != IR_SPLACE_PARAM || to->param + 1 >= call->n_operands)
            continue;
        for (uint32_t a = 1; a < call->n_operands; a++) {
            const ir_value_t *argument = ir_operand_value(&call->operands[a]);
            if (argument && a - 1 != to->param
                && flows_from(summary, IR_SFLOW_STORES, f, a - 1))
                write_memory(fs,
                    ir_operand_value(&call->operands[to->param + 1]),
                    argument, read_value(fs, argument, block), call);
        }
    }
}

typedef struct {
    const ir_instr_t *origin;
    const ir_value_t *carrier;
    bool              by_type;
    const ir_instr_t *field;
    const ir_instr_t *via;
    core_dict_t       seen;
} trace_t;

static bool trace_value(function_state_t *fs, trace_t *trace,
                        const ir_value_t *value, uint64_t bit);

static bool
trace_call(function_state_t *fs, trace_t *trace, const ir_instr_t *call,
           uint64_t bit)
{
    marks_state_t *state = fs->state;
    ir_func_t *callee = n00bb_direct_call_target(call);
    function_facts_t *facts = facts_of(state, callee);
    if ((rule_marks(&state->marks->sources, callee, UINT32_MAX) & bit)
        || (facts && (facts->result_marks & bit))) {
        trace->origin = call;
        return true;
    }
    for (uint32_t a = 1; a < call->n_operands; a++) {
        const ir_value_t *argument = ir_operand_value(&call->operands[a]);
        if (!argument || !passes_through(state, callee, facts, a - 1)
            || !(read_value(fs, argument, call->block).marks & bit))
            continue;
        if (!trace->via) trace->via = call;
        if (trace_value(fs, trace, argument, bit)) return true;
    }
    return false;
}

static bool
trace_value(function_state_t *fs, trace_t *trace, const ir_value_t *value,
            uint64_t bit)
{
    if (!value || !core_dict_add(&trace->seen, (void *)value, (void *)value))
        return false;
    for (size_t w = 0; w < fs->writes.len; w++) {
        const write_t *write = &fs->writes.data[w];
        if (write->base != value
            || !(stored_taint(fs, write->value).marks & bit)) continue;
        if (write->at->op != IR_OP_STORE && !trace->via) trace->via = write->at;
        if (trace_value(fs, trace, write->value, bit)) return true;
    }
    n00bb_marks_t *marks = fs->state->marks;
    bool by_seed = (seeded(fs, value) & bit) != 0;
    if (by_seed || (carried(marks, value->type, (core_string_t){0}) & bit)) {
        trace->carrier = value;
        trace->by_type = !by_seed;
        return true;
    }
    const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def : nullptr;
    if (!def) return false;
    if (field_marks(marks, def) & bit) {
        trace->field = def;
        return true;
    }
    if (def->op == IR_OP_CALL || def->op == IR_OP_CALLBACK_INVOKE)
        return trace_call(fs, trace, def, bit);
    if (slot_load(value)) {
        reaching_t reaching = reaching_stores(fs, def);
        bool found = false;
        for (size_t s = 0; !found && reaching.known && s < reaching.stores.len;
             s++) {
            const ir_instr_t *store = reaching.stores.data[s];
            found = store->n_operands == 2
                && trace_value(fs, trace,
                               ir_operand_value(&store->operands[1]), bit);
        }
        core_list_free(reaching.stores);
        return found;
    }
    for (uint32_t o = 0; o < def->n_operands; o++) {
        const ir_value_t *operand = ir_operand_value(&def->operands[o]);
        if (operand && (stored_taint(fs, operand).marks & bit)
            && trace_value(fs, trace, operand, bit))
            return true;
    }
    return false;
}

static void
report(function_state_t *fs, ir_instr_t *call, ir_func_t *callee,
       uint32_t param, uint64_t violating, uint64_t declared,
       const function_facts_t *callee_facts, const ir_value_t *argument)
{
    n00bb_marks_t *marks = fs->state->marks;
    trace_t trace = {0};
    core_dict_init(&trace.seen, nullptr, nullptr);
    (void)trace_value(fs, &trace, argument, violating & -violating);
    core_dict_free(&trace.seen);
    marks->report(marks->context, &(n00bb_mark_violation_t){
        .call = call, .caller = fs->function, .callee = callee,
        .param = param, .marks = violating, .argument = argument,
        .origin = trace.origin, .carrier = trace.carrier,
        .by_type = trace.by_type,
        .field = trace.field,
        .via = trace.via != trace.origin ? trace.via : nullptr,
        .sink = declared & violating ? nullptr
            : callee_facts->sinks[param]});
}

static void
check_sink(function_state_t *fs, ir_instr_t *call, ir_func_t *callee,
           const function_facts_t *callee_facts, uint32_t param,
           taint_t argument_taint, const ir_value_t *argument)
{
    n00bb_marks_t *marks = fs->state->marks;
    uint64_t declared = rule_marks(&marks->rejects, callee, param);
    uint64_t inferred = callee_facts && param < callee->n_params
        ? callee_facts->rejects[param] : 0;
    uint64_t rejected = declared | inferred;
    if (!rejected) return;
    for (uint32_t p = 0; p < fs->function->n_params && p < MARK_PARAMS; p++) {
        if (!(argument_taint.params >> p & 1)
            || (fs->facts->rejects[p] | rejected) == fs->facts->rejects[p])
            continue;
        fs->facts->rejects[p] |= rejected;
        if (!fs->facts->sinks[p]) fs->facts->sinks[p] = call;
        fs->state->changed = true;
    }
    uint64_t violating = argument_taint.marks & rejected;
    if (fs->reporting && violating && marks->report)
        report(fs, call, callee, param, violating, declared, callee_facts,
               argument);
}

static n00bb_mark_question_t *
question_at(n00bb_marks_t *marks, const ir_instr_t *call)
{
    ir_func_t *callee = n00bb_direct_call_target(call);
    if (!callee || !core_string_starts_with(callee->name,
                                            CORE_STRING_STATIC(N00BB_MARK_QUERY)))
        return nullptr;
    for (size_t q = 0; marks->questions && q < marks->questions->len; q++)
        if (marks->questions->data[q].call == call)
            return &marks->questions->data[q];
    return nullptr;
}

static void
transfer_call(function_state_t *fs, ir_instr_t *in, ir_block_t *block)
{
    marks_state_t *state = fs->state;
    n00bb_mark_question_t *question = fs->reporting
        ? question_at(state->marks, in) : nullptr;
    if (question && in->n_operands > 1)
        question->answer = (read_value(fs, ir_operand_value(&in->operands[1]),
                                       block).marks & question->marks) != 0;
    ir_func_t *callee = n00bb_direct_call_target(in);
    function_facts_t *callee_facts = facts_of(state, callee);
    taint_t result = {
        .marks = rule_marks(&state->marks->sources, callee, UINT32_MAX)
            | (callee_facts ? callee_facts->result_marks : 0),
    };
    for (uint32_t a = 1; a < in->n_operands; a++) {
        const ir_value_t *argument = ir_operand_value(&in->operands[a]);
        if (!argument) continue;
        taint_t taint = read_value(fs, argument, block);
        if (passes_through(state, callee, callee_facts, a - 1))
            merge(&result, taint);
        if (callee)
            check_sink(fs, in, callee, callee_facts, a - 1, taint, argument);
    }
    if (callee) store_flows(fs, in, callee, block);
    result.marks &= ~rule_marks(&state->marks->clears, callee, UINT32_MAX);
    record(fs, in->dest, result);
}

static void
transfer_load(function_state_t *fs, ir_instr_t *in, ir_block_t *block)
{
    taint_t result = {0};
    if (!slot_load(in->dest)) {
        for (uint32_t o = 0; o < in->n_operands; o++)
            merge(&result, read_value(fs, ir_operand_value(&in->operands[o]),
                                      block));
        record(fs, in->dest, result);
        return;
    }
    reaching_t reaching = reaching_stores(fs, in);
    for (size_t s = 0; reaching.known && s < reaching.stores.len; s++) {
        const ir_instr_t *store = reaching.stores.data[s];
        if (store->n_operands == 2)
            merge(&result, read_value(fs, ir_operand_value(&store->operands[1]),
                                      store->block));
    }
    core_list_free(reaching.stores);
    record(fs, in->dest, result);
}

static void
transfer(function_state_t *fs, ir_instr_t *in, ir_block_t *block)
{
    if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE) {
        transfer_call(fs, in, block);
        return;
    }
    if (in->op == IR_OP_LOAD) {
        transfer_load(fs, in, block);
        return;
    }
    if (in->op == IR_OP_STORE && in->n_operands == 2) {
        const ir_value_t *address = ir_operand_value(&in->operands[0]);
        const ir_value_t *value = ir_operand_value(&in->operands[1]);
        if (!slot_address(address))
            write_memory(fs, address, value, read_value(fs, value, block), in);
        return;
    }
    taint_t result = {0};
    for (uint32_t o = 0; o < in->n_operands; o++)
        merge(&result, read_value(fs, ir_operand_value(&in->operands[o]),
                                  block));
    result.marks |= field_marks(fs->state->marks, in);
    if (in->op == IR_OP_RETURN) {
        function_facts_t *facts = fs->facts;
        if ((facts->result_marks | result.marks) != facts->result_marks
            || (facts->result_params | result.params)
                   != facts->result_params) {
            facts->result_marks |= result.marks;
            facts->result_params |= result.params;
            fs->state->changed = true;
        }
        return;
    }
    record(fs, in->dest, result);
}

static void
sweep(function_state_t *fs)
{
    ir_func_t *function = fs->function;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
            transfer(fs, function->blocks[b]->instrs[i],
                     function->blocks[b]);
}

static void
analyze_function(marks_state_t *state, ir_func_t *function, bool reporting)
{
    function_state_t fs = {
        .state = state, .function = function,
        .facts = facts_of(state, function),
        .doms = ir_func_doms(function),
        .memssa = ir_func_pdg(function)
            ? ir_pdg_memory(ir_func_pdg(function)) : nullptr,
        .writes = core_list_new(write_t),
    };
    core_dict_init(&fs.values, nullptr, nullptr);
    do {
        fs.changed = false;
        sweep(&fs);
    } while (fs.changed);
    if (reporting) {
        fs.reporting = true;
        sweep(&fs);
    }
    core_dict_foreach(&fs.values, item) core_free(item->value);
    core_dict_free(&fs.values);
    core_list_free(fs.writes);
}

static bool
calls_into(marks_state_t *state, ir_func_t *function)
{
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
            if (facts_of(state, n00bb_direct_call_target(
                    function->blocks[b]->instrs[i])))
                return true;
    return false;
}

static void
add_facts(marks_state_t *state, ir_func_t *function,
          core_list_t(n00bb_ir_func_ptr_t) *cone)
{
    function_facts_t *facts = core_alloc(function_facts_t);
    facts->validations = core_list_new(validation_t);
    facts->rejects = core_alloc_array(uint64_t, function->n_params);
    facts->sinks = core_alloc_array(const ir_instr_t *, function->n_params);
    core_dict_put(&state->facts, function, facts);
    if (!function->n_blocks) return;
    collect_validations(state->marks, function, &facts->validations);
    core_list_push(*cone, function);
}

static void
add_rule_functions(marks_state_t *state,
                   const core_list_t(n00bb_mark_rule_t) *rules,
                   core_list_t(n00bb_ir_func_ptr_t) *cone)
{
    for (size_t r = 0; r < rules->len; r++)
        if (!facts_of(state, rules->data[r].function))
            add_facts(state, rules->data[r].function, cone);
}

void
n00bb_check_marks(n00bb_marks_t *marks)
{
    marks_state_t state = {.marks = marks};
    core_dict_init(&state.facts, nullptr, nullptr);
    core_list_t(n00bb_ir_func_ptr_t) cone = core_list_new(n00bb_ir_func_ptr_t);
    add_rule_functions(&state, &marks->sources, &cone);
    add_rule_functions(&state, &marks->clears, &cone);
    add_rule_functions(&state, &marks->rejects, &cone);
    add_rule_functions(&state, &marks->seeds, &cone);
    for (size_t q = 0; marks->questions && q < marks->questions->len; q++) {
        ir_instr_t *call = marks->questions->data[q].call;
        ir_func_t *owner = call && call->block ? call->block->owner : nullptr;
        if (owner && !facts_of(&state, owner)) add_facts(&state, owner, &cone);
    }
    bool everywhere = marks->carriers.len != 0;
    for (size_t f = 0; everywhere && f < marks->functions.len; f++)
        if (marks->functions.data[f]->n_blocks
            && !facts_of(&state, marks->functions.data[f]))
            add_facts(&state, marks->functions.data[f], &cone);
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t f = 0; f < marks->functions.len; f++) {
            ir_func_t *function = marks->functions.data[f];
            if (function->n_blocks && !facts_of(&state, function)
                && calls_into(&state, function)) {
                add_facts(&state, function, &cone);
                grew = true;
            }
        }
    }
    do {
        state.changed = false;
        for (size_t f = 0; f < cone.len; f++)
            analyze_function(&state, cone.data[f], false);
    } while (state.changed);
    for (size_t f = 0; f < cone.len; f++)
        analyze_function(&state, cone.data[f], true);
    core_dict_foreach(&state.facts, item) {
        function_facts_t *facts = item->value;
        core_free(facts->rejects);
        core_free(facts->sinks);
        core_list_free(facts->validations);
        core_free(facts);
    }
    core_dict_free(&state.facts);
    core_list_free(cone);
}
