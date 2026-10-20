#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/summary.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(ir_saccess_t);
core_list_decl(ir_sinput_t);
core_list_decl(ir_soutput_t);
core_list_decl(uint32_t);
typedef ir_func_t *summary_func_ptr_t;
core_list_decl(summary_func_ptr_t);
typedef ir_summary_t *summary_ptr_t;
core_list_decl(summary_ptr_t);

struct ir_summary_t {
    const ir_func_t           *function;
    uint32_t                   flags;
    core_list_t(ir_saccess_t)  accesses;
    core_list_t(ir_sinput_t)   inputs;
    core_list_t(ir_soutput_t)  outputs;
    core_list_t(uint32_t)      dependences;
};

struct ir_summaries_t {
    const ir_summary_hooks_t *hooks;
    core_list_t(summary_ptr_t) owned;
    core_dict_t               by_function;
};

bool
ir_splace_eq(const ir_splace_t *a, const ir_splace_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case IR_SPLACE_PARAM:
        return a->param == b->param;
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return a->global == b->global;
    case IR_SPLACE_HEAP:
        return a->type == b->type
            || (a->type && b->type && th_resolve(a->type) == th_resolve(b->type));
    case IR_SPLACE_UNKNOWN:
        return true;
    }
    return false;
}

ir_summary_t *
ir_summary_new(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = core_alloc(ir_summary_t);
    summary->function    = function;
    summary->accesses    = core_list_new(ir_saccess_t);
    summary->inputs      = core_list_new(ir_sinput_t);
    summary->outputs     = core_list_new(ir_soutput_t);
    summary->dependences = core_list_new(uint32_t);
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

static void
summary_free(ir_summary_t *summary)
{
    core_list_free(summary->accesses);
    core_list_free(summary->inputs);
    core_list_free(summary->outputs);
    core_list_free(summary->dependences);
    core_free(summary);
}

void
ir_summary_set_flags(ir_summary_t *summary, uint32_t flags)
{
    summary->flags |= flags;
}

void
ir_summary_add_access(ir_summary_t *summary, ir_splace_t place, uint8_t flags)
{
    for (size_t i = 0; i < summary->accesses.len; i++)
        if (ir_splace_eq(&summary->accesses.data[i].place, &place)) {
            summary->accesses.data[i].flags |= flags;
            return;
        }
    core_list_push(summary->accesses,
                   ((ir_saccess_t){.place = place, .flags = flags}));
}

ir_summary_t *
ir_summary_conservative(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_new(summaries, function);
    summary->flags = IR_SUMMARY_MAY_FAIL | IR_SUMMARY_MAY_TRAP
        | IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_BLOCK
        | IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_IO
        | IR_SUMMARY_MAY_COLLECT;
    ir_summary_add_access(summary, (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                          IR_SACCESS_READ | IR_SACCESS_WRITE
                              | IR_SACCESS_READS_ENTRY);
    return summary;
}

const ir_func_t *ir_summary_function(const ir_summary_t *s) { return s->function; }
uint32_t ir_summary_flags(const ir_summary_t *s) { return s ? s->flags : 0; }
uint32_t ir_summary_access_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->accesses.len : 0;
}
const ir_saccess_t *ir_summary_access_at(const ir_summary_t *s, uint32_t i)
{
    return s && i < s->accesses.len ? &s->accesses.data[i] : nullptr;
}
uint32_t ir_summary_input_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->inputs.len : 0;
}
const ir_sinput_t *ir_summary_input_at(const ir_summary_t *s, uint32_t i)
{
    return s && i < s->inputs.len ? &s->inputs.data[i] : nullptr;
}
uint32_t ir_summary_output_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->outputs.len : 0;
}
const ir_soutput_t *ir_summary_output_at(const ir_summary_t *s, uint32_t i)
{
    return s && i < s->outputs.len ? &s->outputs.data[i] : nullptr;
}
const uint32_t *ir_summary_dependences(const ir_summary_t *s)
{
    return s ? s->dependences.data : nullptr;
}

ir_summary_t *
ir_summary_of(ir_summaries_t *summaries, const ir_func_t *function)
{
    bool found = false;
    void *summary = summaries
        ? core_dict_get(&summaries->by_function, (void *)function, &found)
        : nullptr;
    return found ? summary : nullptr;
}

static bool
portable_place(const ir_func_t *f, const ir_place_t *place, ir_splace_t *out)
{
    switch (place->kind) {
    case IR_PLACE_LOCAL:
        return false;
    case IR_PLACE_PARAM:
        for (uint32_t i = 0; i < f->n_params; i++)
            if (f->params[i] == place->root) {
                *out = (ir_splace_t){.kind = IR_SPLACE_PARAM, .param = i,
                                     .type = place->type};
                return true;
            }
        *out = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
        return true;
    case IR_PLACE_GLOBAL:
    case IR_PLACE_THREAD: {
        const ir_global_t *global = place->root && place->root->global
            ? ir_global_identity(place->root->global) : nullptr;
        *out = (ir_splace_t){
            .kind = place->kind == IR_PLACE_THREAD ? IR_SPLACE_THREAD
                                                    : IR_SPLACE_GLOBAL,
            .global = global, .type = place->type};
        return global != nullptr;
    }
    case IR_PLACE_HEAP:
        *out = (ir_splace_t){.kind = IR_SPLACE_HEAP, .type = place->type};
        return true;
    case IR_PLACE_UNKNOWN:
        *out = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
        return true;
    }
    return false;
}

static bool
addressable(const ir_func_t *f, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    if (!type) return false;
    if (th_kind_of(type) == TH_INDIRECTION || th_kind_of(type) == TH_SEQUENCE)
        return true;
    if (th_kind_of(type) == TH_NAMED && f && f->owner) {
        th_type *definition = ir_type_definition(f->owner, type);
        definition = definition ? th_resolve(definition) : nullptr;
        return definition && (th_kind_of(definition) == TH_INDIRECTION
                              || th_kind_of(definition) == TH_SEQUENCE);
    }
    return th_kind_of(type) == TH_VAR;
}

static bool
argument_places(ir_places_t *places, const ir_func_t *f, const ir_value_t *arg,
                const uint32_t **ids, uint32_t *count)
{
    *ids   = nullptr;
    *count = 0;
    return arg && addressable(f, arg) && ir_places_of(places, arg, ids, count);
}

typedef struct {
    ir_summaries_t                *summaries;
    core_list_t(summary_func_ptr_t) targets;
    bool                           unknown;
} call_targets_t;

static void
collect_target(void *sink, ir_func_t *target)
{
    call_targets_t *targets = sink;
    core_list_push(targets->targets, target);
}

static bool
resolve_targets(ir_summaries_t *summaries, const ir_instr_t *call,
                call_targets_t *out)
{
    out->summaries = summaries;
    out->targets   = core_list_new(summary_func_ptr_t);
    out->unknown   = false;
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->call_targets
        && hooks->call_targets(hooks->context, call, collect_target, out))
        return true;
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands
        ? ir_operand_value(&call->operands[0]) : nullptr;
    if (callee && callee->kind == IR_VAL_FUNC && callee->owner) {
        core_list_push(out->targets, callee->owner);
        return true;
    }
    out->unknown = true;
    return true;
}

static void
adjust_flags(ir_summaries_t *summaries, ir_summary_t *summary)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->adjust)
        summary->flags = hooks->adjust(hooks->context, summary->function,
                                       summary->flags);
}

static ir_summary_t *
target_summary(ir_summaries_t *summaries, ir_func_t *target)
{
    ir_summary_t *summary = ir_summary_of(summaries, target);
    if (summary) return summary;
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->axiom) summary = hooks->axiom(hooks->context,
                                                      summaries, target);
    if (!summary) summary = ir_summary_conservative(summaries, target);
    adjust_flags(summaries, summary);
    core_dict_put(&summaries->by_function, target, summary);
    return summary;
}

static bool
is_call(const ir_instr_t *in)
{
    return in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE;
}

enum {
    FIELD_DEPTH  = 4,
    FIELD_LEAVES = 64,
};

typedef struct {
    core_string_t names[FIELD_DEPTH];
    uint32_t      depth;
} field_leaf_t;
core_list_decl(field_leaf_t);

static th_type *
record_of(const ir_func_t *f, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (type && th_kind_of(type) == TH_NAMED && f->owner) {
        th_type *definition = ir_type_definition(f->owner, type);
        type = definition ? th_resolve(definition) : nullptr;
    }
    return type && th_kind_of(type) == TH_RECORD && th_record_len(type)
        ? type : nullptr;
}

static bool
collect_leaves(const ir_func_t *f, th_type *type, field_leaf_t prefix,
               core_list_t(field_leaf_t) *out)
{
    th_type *record = prefix.depth < FIELD_DEPTH ? record_of(f, type)
                                                 : nullptr;
    if (!record) {
        if (out->len >= FIELD_LEAVES) return false;
        core_list_push(*out, prefix);
        return true;
    }
    for (size_t i = 0; i < th_record_len(record); i++) {
        const th_member *member = th_record_member(record, i);
        field_leaf_t     next   = prefix;
        next.names[next.depth++] = member->name;
        if (!collect_leaves(f, member->type, next, out)) return false;
    }
    return true;
}

static core_list_t(field_leaf_t)
place_leaves(const ir_func_t *f, const ir_splace_t *place)
{
    core_list_t(field_leaf_t) leaves = core_list_new(field_leaf_t);
    if (!collect_leaves(f, place->type, (field_leaf_t){0}, &leaves)) {
        leaves.len = 0;
        core_list_push(leaves, ((field_leaf_t){0}));
    }
    return leaves;
}

typedef struct {
    ir_summaries_t     *summaries;
    ir_func_t          *f;
    ir_places_t        *places;
    const ir_splace_t  *target;
    const field_leaf_t *leaf;
} flow_query_t;

static bool
argument_is(flow_query_t *q, const ir_instr_t *call, uint32_t param,
            const ir_splace_t *want)
{
    uint32_t operand = param + 1;
    if (operand >= call->n_operands) return false;
    const ir_value_t *arg = ir_operand_value(&call->operands[operand]);
    const uint32_t   *ids = nullptr;
    uint32_t          n   = 0;
    if (!argument_places(q->places, q->f, arg, &ids, &n) || n != 1)
        return false;
    ir_splace_t mapped;
    return portable_place(q->f, ir_place_at(q->places, ids[0]), &mapped)
        && mapped.kind != IR_SPLACE_HEAP && mapped.kind != IR_SPLACE_UNKNOWN
        && ir_splace_eq(&mapped, want);
}

static bool
named_place(const ir_splace_t *place)
{
    return place->kind != IR_SPLACE_HEAP && place->kind != IR_SPLACE_UNKNOWN;
}

static uint8_t
target_access_flags(flow_query_t *q, const ir_instr_t *call,
                    const ir_summary_t *s)
{
    uint8_t flags = 0;
    for (size_t a = 0; q->target && a < s->accesses.len; a++) {
        const ir_saccess_t *access = &s->accesses.data[a];
        bool matches = access->place.kind == IR_SPLACE_PARAM
            ? argument_is(q, call, access->place.param, q->target)
            : named_place(&access->place)
                && ir_splace_eq(&access->place, q->target);
        if (matches) flags |= access->flags;
    }
    return flags;
}

static uint8_t
written(uint8_t flags, uint8_t must)
{
    if (flags & must) return IR_PATH_PASSED;
    if (flags & IR_SACCESS_WRITE) return IR_PATH_UNPASSED | IR_PATH_PASSED;
    return IR_PATH_UNPASSED;
}

static uint8_t
callee_effect(const ir_summary_t *s, uint8_t flags, ir_path_outcome_t outcome)
{
    bool diverges = (s->flags & IR_SUMMARY_DIVERGES) != 0;
    uint8_t success = diverges || (s->flags & IR_SUMMARY_NO_RETURN)
        ? 0 : written(flags, IR_SACCESS_MUST_SUCCESS);
    uint8_t failure = !diverges && (s->flags & IR_SUMMARY_MAY_FAIL)
        ? written(flags, IR_SACCESS_MUST_FAILURE) : 0;
    switch (outcome) {
    case IR_PATH_SUCCESS:
        return success;
    case IR_PATH_FAILURE:
        return failure;
    case IR_PATH_ANY:
        break;
    }
    return success | failure;
}

static uint8_t
call_effect(flow_query_t *q, const ir_instr_t *call, ir_path_outcome_t outcome)
{
    call_targets_t targets;
    resolve_targets(q->summaries, call, &targets);
    uint8_t effect = targets.unknown || !targets.targets.len
        ? IR_PATH_UNPASSED : 0;
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *s = target_summary(q->summaries, targets.targets.data[t]);
        effect |= callee_effect(s, target_access_flags(q, call, s), outcome);
    }
    core_list_free(targets.targets);
    return effect;
}

static bool
stores_target(flow_query_t *q, const ir_instr_t *in)
{
    uint32_t first = 0;
    uint32_t count = q->target ? ir_places_accesses_of(q->places, in, &first)
                               : 0;
    for (uint32_t a = first; a < first + count; a++) {
        const ir_access_t *access = ir_places_access_at(q->places, a);
        if (access->kind != IR_ACCESS_STORE
            || !access->whole || access->count != 1) continue;
        ir_splace_t mapped;
        if (portable_place(q->f, ir_place_at(q->places,
                ir_places_access_places(q->places, access)[0]), &mapped)
            && named_place(&mapped) && ir_splace_eq(&mapped, q->target))
            return true;
    }
    return false;
}

static bool
field_store_covers(flow_query_t *q, const ir_instr_t *in)
{
    if (!q->target || !q->leaf || !q->leaf->depth || in->op != IR_OP_STORE
        || !in->n_operands) return false;
    core_string_t     path[FIELD_DEPTH];
    uint32_t          depth   = 0;
    const ir_value_t *address = ir_operand_value(&in->operands[0]);
    while (address && address->def && address->def->op == IR_OP_FIELD
           && address->def->n_operands == 2
           && address->def->operands[1].kind == IR_OPD_NAME) {
        if (depth == FIELD_DEPTH) return false;
        path[depth++] = address->def->operands[1].name;
        address       = ir_operand_value(&address->def->operands[0]);
    }
    if (!depth || depth > q->leaf->depth) return false;
    for (uint32_t i = 0; i < depth; i++)
        if (!core_string_eq(path[depth - 1 - i], q->leaf->names[i]))
            return false;
    const uint32_t *ids = nullptr;
    uint32_t        n   = 0;
    ir_splace_t     mapped;
    return address && ir_places_of(q->places, address, &ids, &n) && n == 1
        && portable_place(q->f, ir_place_at(q->places, ids[0]), &mapped)
        && named_place(&mapped) && ir_splace_eq(&mapped, q->target);
}

static bool
writes_target(flow_query_t *q, const ir_instr_t *in)
{
    return stores_target(q, in) || field_store_covers(q, in);
}

static uint8_t
flow_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    flow_query_t *q = context;
    if (is_call(in)) return call_effect(q, in, outcome);
    return writes_target(q, in) ? IR_PATH_PASSED : IR_PATH_UNPASSED;
}

static uint8_t
combine_leaf(uint8_t combined, uint8_t leaf, bool first)
{
    if (first) return leaf;
    if (!combined || !leaf) return 0;
    uint8_t passed = combined & leaf & IR_PATH_PASSED;
    return ((combined | leaf) & IR_PATH_UNPASSED) | passed;
}

uint8_t
ir_summary_effect(const ir_summary_t *summary, const ir_splace_t *place,
                  ir_path_outcome_t outcome)
{
    if (!summary) return IR_PATH_UNPASSED;
    uint8_t flags = 0;
    for (size_t a = 0; a < summary->accesses.len; a++)
        if (named_place(&summary->accesses.data[a].place)
            && ir_splace_eq(&summary->accesses.data[a].place, place))
            flags |= summary->accesses.data[a].flags;
    return callee_effect(summary, flags, outcome);
}

static bool
arm_dominates(ir_doms_t *doms, ir_block_t *arm, ir_block_t *from)
{
    return arm && ir_pred_count(doms, arm) == 1 && ir_dominates(doms, arm, from);
}

static ir_path_outcome_t
call_arm(ir_doms_t *doms, const ir_instr_t *call, ir_block_t *from)
{
    ir_instr_t *terminator      = ir_block_terminator(call->block);
    bool        true_is_success = false;
    if (ir_call_outcome_branch(terminator, &true_is_success) != call)
        return IR_PATH_ANY;
    ir_block_t *on_true  = ir_succ_of(&terminator->operands[1]);
    ir_block_t *on_false = ir_succ_of(&terminator->operands[2]);
    ir_block_t *success  = true_is_success ? on_true : on_false;
    ir_block_t *failure  = true_is_success ? on_false : on_true;
    if (success == failure) return IR_PATH_ANY;
    if (arm_dominates(doms, success, from)) return IR_PATH_SUCCESS;
    if (arm_dominates(doms, failure, from)) return IR_PATH_FAILURE;
    return IR_PATH_ANY;
}

typedef struct {
    uint32_t    def;
    uint8_t     effect;
    ir_block_t *from;
} walk_item_t;
core_list_decl(walk_item_t);

static uint8_t state_before_leaf(flow_query_t *q, ir_memssa_t *memssa,
                                 ir_doms_t *doms, uint8_t entry,
                                 const ir_instr_t *in);

uint8_t
ir_summary_state_before(ir_summaries_t *summaries, ir_func_t *f,
                        const ir_splace_t *place, uint8_t entry,
                        const ir_instr_t *in)
{
    size_t       scope  = ir_analysis_scope_begin();
    ir_memssa_t *memssa = ir_func_memssa(f);
    ir_doms_t   *doms   = ir_func_doms(f);
    flow_query_t q      = {.summaries = summaries, .f = f,
                           .places = ir_func_places(f), .target = place};
    uint8_t state = memssa && doms && q.places
        ? state_before_leaf(&q, memssa, doms, entry, in) : 0;
    if (state & IR_PATH_UNPASSED) {
        core_list_t(field_leaf_t) leaves = place_leaves(f, place);
        for (size_t l = 0; leaves.data[0].depth && l < leaves.len; l++) {
            q.leaf = &leaves.data[l];
            state  = combine_leaf(state,
                state_before_leaf(&q, memssa, doms, entry, in), l == 0);
        }
        core_list_free(leaves);
    }
    ir_analysis_scope_end(scope);
    return state;
}

static uint8_t
state_before_leaf(flow_query_t *q, ir_memssa_t *memssa, ir_doms_t *doms,
                  uint8_t entry, const ir_instr_t *in)
{
    uint32_t start = ir_memssa_def_before(memssa, in);
    uint8_t  state = 0;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_t(walk_item_t) work = core_list_new(walk_item_t);
    if (start != UINT32_MAX)
        core_list_push(work, ((walk_item_t){.def = start,
                                            .effect = IR_PATH_UNPASSED,
                                            .from = in->block}));
    while (work.len) {
        walk_item_t item = work.data[--work.len];
        uintptr_t key = ((uintptr_t)item.def << 34)
            | ((uintptr_t)item.from->index << 2) | item.effect;
        if (!core_dict_add(&seen, (void *)(key + 1), (void *)(key + 1)))
            continue;
        const ir_memdef_t *def    = ir_memssa_def(memssa, item.def);
        uint8_t            effect = item.effect;
        switch (def->kind) {
        case IR_MEMDEF_ENTRY:
            state |= ir_path_apply(entry, effect);
            continue;
        case IR_MEMDEF_PHI:
            for (uint32_t i = 0; i < ir_memssa_phi_count(memssa, item.def);
                 i++) {
                ir_block_t *pred = ir_pred_at(doms, def->block, i);
                core_list_push(work, ((walk_item_t){
                    .def    = ir_memssa_phi_incoming(memssa, item.def, i),
                    .effect = effect,
                    .from   = pred ? pred : def->block}));
            }
            continue;
        case IR_MEMDEF_STORE:
            if (writes_target(q, def->instr)) effect = IR_PATH_PASSED;
            break;
        case IR_MEMDEF_CALL:
            if (is_call(def->instr))
                effect = ir_path_apply(call_effect(q, def->instr,
                    call_arm(doms, def->instr, item.from)), effect);
            break;
        }
        if (effect == IR_PATH_PASSED) state |= IR_PATH_PASSED;
        else if (effect)
            core_list_push(work, ((walk_item_t){
                .def = ir_memssa_def_prev(memssa, item.def),
                .effect = effect, .from = item.from}));
    }
    core_list_free(work);
    core_dict_free(&seen);
    return state;
}

ir_splace_t
ir_splace_global(const ir_global_t *global)
{
    bool thread = ir_global_storage_duration(global)
        == IR_STORAGE_DURATION_THREAD;
    return (ir_splace_t){
        .kind   = thread ? IR_SPLACE_THREAD : IR_SPLACE_GLOBAL,
        .global = ir_global_identity(global),
        .type   = global->type};
}

static void
compose_call(ir_summaries_t *summaries, ir_summary_t *summary, ir_func_t *f,
             ir_places_t *places, const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    if (targets.unknown) {
        summary->flags |= IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
            | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
            | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT;
        ir_summary_add_access(summary, (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                              IR_SACCESS_READ | IR_SACCESS_WRITE
                                  | IR_SACCESS_READS_ENTRY);
        core_list_free(targets.targets);
        return;
    }
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(summaries,
                                              targets.targets.data[t]);
        summary->flags |= callee->flags
            & (IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
               | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
               | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
               | IR_SUMMARY_ALLOCATES_DIRECT);
        for (size_t a = 0; a < callee->accesses.len; a++) {
            const ir_saccess_t *access = &callee->accesses.data[a];
            uint8_t flags = access->flags
                & (IR_SACCESS_READ | IR_SACCESS_WRITE | IR_SACCESS_READS_ENTRY);
            if (access->place.kind != IR_SPLACE_PARAM) {
                ir_summary_add_access(summary, access->place, flags);
                continue;
            }
            uint32_t operand = access->place.param + 1;
            const ir_value_t *arg = operand < call->n_operands
                ? ir_operand_value(&call->operands[operand]) : nullptr;
            const uint32_t *ids = nullptr;
            uint32_t        n   = 0;
            if (arg && !addressable(f, arg)) continue;
            if (!argument_places(places, f, arg, &ids, &n)) {
                ir_summary_add_access(summary,
                    (ir_splace_t){.kind = IR_SPLACE_UNKNOWN}, flags);
                continue;
            }
            for (uint32_t p = 0; p < n; p++) {
                ir_splace_t mapped;
                if (portable_place(f, ir_place_at(places, ids[p]), &mapped))
                    ir_summary_add_access(summary, mapped, flags);
            }
        }
    }
    core_list_free(targets.targets);
}

static uint32_t
allocation_flags(const ir_func_t *f, const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_ALLOC:
        return ir_allocation_has_only_local_uses(f, in)
            ? 0
            : IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_COLLECT
                | IR_SUMMARY_ALLOCATES_DIRECT;
    case IR_OP_ALLOC_USING:
    case IR_OP_ARRAY_NEW:
    case IR_OP_ARRAY_NEW_USING:
    case IR_OP_BOX:
        return IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_BUFFER_BORROW_END:
        return IR_SUMMARY_MAY_COLLECT;
    default:
        return 0;
    }
}


static void summarize_dependences(ir_summaries_t *summaries,
                                  ir_summary_t *summary, ir_func_t *f);

enum {
    UNPASSED_RETURN = 1u << 0,
    UNPASSED_FAIL   = 1u << 1,
};

static uint8_t
exit_unpassed(const ir_path_exits_t *exits)
{
    return ((exits->returned & IR_PATH_UNPASSED) ? UNPASSED_RETURN : 0)
        | ((exits->failed & IR_PATH_UNPASSED) ? UNPASSED_FAIL : 0);
}

static uint8_t
unpassed_exits(flow_query_t *q, ir_func_t *f, const ir_path_exits_t *whole)
{
    uint8_t unpassed = exit_unpassed(whole);
    if (!unpassed) return 0;
    core_list_t(field_leaf_t) leaves = place_leaves(f, q->target);
    if (leaves.data[0].depth) {
        unpassed = 0;
        for (size_t l = 0; l < leaves.len; l++) {
            ir_path_exits_t exits;
            q->leaf = &leaves.data[l];
            unpassed |= ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, q, &exits)
                ? exit_unpassed(&exits) : UNPASSED_RETURN | UNPASSED_FAIL;
        }
        q->leaf = nullptr;
    }
    core_list_free(leaves);
    return unpassed;
}

static void
summarize_function(ir_summaries_t *summaries, ir_summary_t *summary,
                   ir_func_t *f)
{
    size_t scope = ir_analysis_scope_begin();
    summary->flags = 0;
    summary->accesses.len = 0;
    ir_places_t *places = ir_func_places(f);
    ir_sites_t  *sites  = ir_func_sites(f);
    ir_memssa_t *memssa = ir_func_memssa(f);
    for (uint32_t a = 0; places && a < ir_places_access_count(places); a++) {
        const ir_access_t *access = ir_places_access_at(places, a);
        const uint32_t    *ids    = ir_places_access_places(places, access);
        bool entry = access->kind == IR_ACCESS_LOAD && memssa
            && ir_memssa_reads_entry(memssa, a);
        for (uint32_t p = 0; p < access->count; p++) {
            ir_splace_t mapped;
            if (!portable_place(f, ir_place_at(places, ids[p]), &mapped))
                continue;
            uint8_t flags = access->kind == IR_ACCESS_LOAD
                ? IR_SACCESS_READ | (entry ? IR_SACCESS_READS_ENTRY : 0)
                : IR_SACCESS_WRITE;
            ir_summary_add_access(summary, mapped, flags);
        }
    }
    for (uint32_t s = 0; sites && s < ir_sites_count(sites); s++) {
        const ir_site_t *site = ir_site_at(sites, s);
        if (site->kind == IR_SITE_TRAP) summary->flags |= IR_SUMMARY_MAY_TRAP;
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (is_call(in)) compose_call(summaries, summary, f, places, in);
            summary->flags |= allocation_flags(f, in);
        }
    }
    flow_query_t    q = {.summaries = summaries, .f = f, .places = places};
    ir_path_exits_t exits;
    if (places && ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, &q, &exits)) {
        if (!exits.returned) summary->flags |= IR_SUMMARY_NO_RETURN;
        if (exits.failed) summary->flags |= IR_SUMMARY_MAY_FAIL;
        if (!exits.returned && !exits.failed)
            summary->flags |= IR_SUMMARY_DIVERGES;
    }
    else summary->flags |= IR_SUMMARY_MAY_FAIL;
    for (size_t a = 0; places && a < summary->accesses.len; a++) {
        ir_saccess_t *access = &summary->accesses.data[a];
        if (!(access->flags & IR_SACCESS_WRITE) || !named_place(&access->place))
            continue;
        ir_splace_t target = access->place;
        q.target = &target;
        q.leaf   = nullptr;
        if (!ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, &q, &exits))
            continue;
        uint8_t unpassed = unpassed_exits(&q, f, &exits);
        if (!(unpassed & UNPASSED_RETURN))
            access->flags |= IR_SACCESS_MUST_SUCCESS;
        if (!(unpassed & UNPASSED_FAIL))
            access->flags |= IR_SACCESS_MUST_FAILURE;
    }
    adjust_flags(summaries, summary);
    summarize_dependences(summaries, summary, f);
    ir_analysis_scope_end(scope);
}

typedef struct {
    ir_summaries_t *summaries;
    ir_summary_t   *summary;
    ir_func_t      *f;
    ir_places_t    *places;
    ir_memssa_t    *memssa;
    ir_pdg_t       *pdg;
    core_dict_t     index;
    uint32_t        instrs;
    uint32_t        words;
    uint32_t       *access_first;
    uint32_t       *access_count;
    uint64_t       *data;
    uint64_t       *control;
    uint64_t       *place_stores;
    bool            changed;
} deps_t;

static uint64_t *
row(deps_t *d, uint64_t *table, uint32_t index)
{
    return table + (size_t)index * d->words;
}

static bool
merge(deps_t *d, uint64_t *into, const uint64_t *from)
{
    bool grew = false;
    for (uint32_t w = 0; w < d->words; w++) {
        uint64_t next = into[w] | from[w];
        grew |= next != into[w];
        into[w] = next;
    }
    return grew;
}

static void
set_bit(deps_t *d, uint64_t *into, uint32_t bit)
{
    uint64_t mask = 1ull << (bit % 64);
    if (!(into[bit / 64] & mask)) {
        into[bit / 64] |= mask;
        d->changed = true;
    }
}

static uint32_t
input_of(ir_summary_t *summary, ir_sinput_t input)
{
    for (size_t i = 0; i < summary->inputs.len; i++) {
        ir_sinput_t *have = &summary->inputs.data[i];
        if (have->kind != input.kind) continue;
        if ((input.kind == IR_SINPUT_VALUE || input.kind == IR_SINPUT_REFERENT)
                ? have->param == input.param
                : input.kind == IR_SINPUT_UNKNOWN
                      || ir_splace_eq(&have->place, &input.place))
            return (uint32_t)i;
    }
    core_list_push(summary->inputs, input);
    return (uint32_t)(summary->inputs.len - 1);
}

static uint32_t
instr_index(deps_t *d, const ir_instr_t *in)
{
    bool found = false;
    uint32_t index = (uint32_t)(uintptr_t)core_dict_get(&d->index, (void *)in,
                                                        &found);
    return found ? index - 1 : UINT32_MAX;
}

static uint32_t
entry_input(deps_t *d, uint32_t place)
{
    ir_splace_t mapped;
    if (!portable_place(d->f, ir_place_at(d->places, place), &mapped))
        return UINT32_MAX;
    if (mapped.kind == IR_SPLACE_PARAM)
        return input_of(d->summary, (ir_sinput_t){
            .kind = IR_SINPUT_REFERENT, .param = mapped.param});
    if (mapped.kind == IR_SPLACE_UNKNOWN)
        return input_of(d->summary, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    return input_of(d->summary, (ir_sinput_t){.kind = IR_SINPUT_PLACE,
                                              .place = mapped});
}

typedef struct {
    deps_t            *d;
    uint64_t          *into;
    const ir_access_t *load;
    const uint32_t    *load_places;
} clobber_sink_t;

static bool
gather_clobber(void *context, const ir_memdef_t *def)
{
    clobber_sink_t *sink = context;
    deps_t         *d    = sink->d;
    if (def->kind == IR_MEMDEF_ENTRY) {
        for (uint32_t p = 0; p < sink->load->count; p++) {
            uint32_t bit = entry_input(d, sink->load_places[p]);
            if (bit != UINT32_MAX && bit < d->words * 64)
                set_bit(d, sink->into, bit);
        }
        return true;
    }
    uint32_t index = def->instr ? instr_index(d, def->instr) : UINT32_MAX;
    if (index != UINT32_MAX && merge(d, sink->into, row(d, d->data, index)))
        d->changed = true;
    return true;
}

static void
call_dependences(deps_t *d, ir_instr_t *call, uint64_t *into)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    bool unknown = targets.unknown;
    for (size_t t = 0; !unknown && t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        if (!callee->outputs.len && !callee->inputs.len
            && (callee->flags & IR_SUMMARY_CALLS_UNKNOWN))
            unknown = true;
    }
    for (uint32_t o = 1; o < call->n_operands; o++) {
        ir_value_t *value = ir_operand_value(&call->operands[o]);
        if (!value) continue;
        if (value->kind == IR_VAL_PARAM)
            for (uint32_t i = 0; i < d->f->n_params; i++)
                if (d->f->params[i] == value)
                    set_bit(d, into, input_of(d->summary, (ir_sinput_t){
                        .kind = IR_SINPUT_VALUE, .param = i}));
        if (value->kind == IR_VAL_INSTR && value->def) {
            uint32_t index = instr_index(d, value->def);
            if (index != UINT32_MAX
                && merge(d, into, row(d, d->data, index)))
                d->changed = true;
        }
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument_places(d->places, d->f, value, &ids, &n))
            for (uint32_t p = 0; p < n; p++) {
                uint32_t bit = entry_input(d, ids[p]);
                if (bit != UINT32_MAX && bit < d->words * 64)
                    set_bit(d, into, bit);
                if (merge(d, into, row(d, d->place_stores, ids[p])))
                    d->changed = true;
            }
    }
    if (unknown)
        set_bit(d, into, input_of(d->summary,
                                  (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN}));
    core_list_free(targets.targets);
}

static void
instruction_dependences(deps_t *d, ir_instr_t *in, uint32_t index)
{
    uint64_t *into = row(d, d->data, index);
    for (uint32_t e = 0; d->pdg && e < ir_pdg_in_count(d->pdg, in); e++) {
        const ir_dep_edge_t *edge = ir_pdg_in_at(d->pdg, in, e);
        uint32_t from = edge->kind == IR_DEP_DATA
            ? instr_index(d, edge->from) : UINT32_MAX;
        if (from != UINT32_MAX && merge(d, into, row(d, d->data, from)))
            d->changed = true;
    }
    for (uint32_t o = 0; o < in->n_operands; o++) {
        ir_value_t *value = ir_operand_value(&in->operands[o]);
        if (!value || value->kind != IR_VAL_PARAM) continue;
        for (uint32_t i = 0; i < d->f->n_params; i++)
            if (d->f->params[i] == value)
                set_bit(d, into, input_of(d->summary, (ir_sinput_t){
                    .kind = IR_SINPUT_VALUE, .param = i}));
    }
    uint32_t end = d->access_first[index] + d->access_count[index];
    for (uint32_t a = d->access_first[index]; a < end; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind != IR_ACCESS_LOAD) continue;
        clobber_sink_t sink = {.d = d, .into = into, .load = access,
            .load_places = ir_places_access_places(d->places, access)};
        if (!d->memssa
            || !ir_memssa_visit_clobbers(d->memssa, a, gather_clobber, &sink))
            set_bit(d, into, input_of(d->summary,
                                      (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN}));
    }
    if (is_call(in)) call_dependences(d, in, into);
}

static void
block_control(deps_t *d, ir_block_t *block, uint64_t *into)
{
    for (uint32_t c = 0; d->pdg && c < ir_controllers_of(d->pdg, block); c++) {
        ir_block_t *controller = ir_controller_at(d->pdg, block, c);
        ir_instr_t *branch = controller ? ir_block_terminator(controller)
                                        : nullptr;
        uint32_t index = branch ? instr_index(d, branch) : UINT32_MAX;
        if (index == UINT32_MAX) continue;
        if (merge(d, into, row(d, d->data, index))) d->changed = true;
        if (merge(d, into, row(d, d->control, index))) d->changed = true;
    }
}

static bool
call_writes_place(deps_t *d, ir_instr_t *call, const ir_splace_t *place)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    bool writes = targets.unknown;
    for (size_t t = 0; !writes && t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        for (size_t a = 0; !writes && a < callee->accesses.len; a++) {
            const ir_saccess_t *access = &callee->accesses.data[a];
            if (!(access->flags & IR_SACCESS_WRITE)) continue;
            if (access->place.kind != IR_SPLACE_PARAM) {
                writes = access->place.kind == IR_SPLACE_UNKNOWN
                    || ir_splace_eq(&access->place, place);
                continue;
            }
            uint32_t operand = access->place.param + 1;
            const ir_value_t *arg = operand < call->n_operands
                ? ir_operand_value(&call->operands[operand]) : nullptr;
            const uint32_t *ids = nullptr;
            uint32_t        n   = 0;
            if (arg && !addressable(d->f, arg)) continue;
            if (!argument_places(d->places, d->f, arg, &ids, &n)) {
                writes = true;
                continue;
            }
            for (uint32_t p = 0; !writes && p < n; p++) {
                ir_splace_t mapped;
                writes = portable_place(d->f, ir_place_at(d->places, ids[p]),
                                        &mapped)
                    && ir_splace_eq(&mapped, place);
            }
        }
    }
    core_list_free(targets.targets);
    return writes;
}

static void
add_output(deps_t *d, ir_soutput_kind_t kind, ir_splace_t place,
           const uint64_t *data, const uint64_t *control)
{
    ir_summary_t *s = d->summary;
    ir_soutput_t  out = {.kind = kind, .place = place};
    out.data_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < s->inputs.len; bit++)
        if (data[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.data_count    = (uint32_t)s->dependences.len - out.data_first;
    out.control_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < s->inputs.len; bit++)
        if (control[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.control_count = (uint32_t)s->dependences.len - out.control_first;
    core_list_push(s->outputs, out);
}

static void
collect_outputs(deps_t *d)
{
    uint64_t *data    = core_alloc_array(uint64_t, d->words);
    uint64_t *control = core_alloc_array(uint64_t, d->words);
    for (int kind = IR_SOUTPUT_RESULT; kind <= IR_SOUTPUT_FAILURE; kind++) {
        memset(data, 0, d->words * sizeof(uint64_t));
        memset(control, 0, d->words * sizeof(uint64_t));
        bool any = false;
        ir_op_t op = kind == IR_SOUTPUT_RESULT ? IR_OP_RETURN : IR_OP_FAIL;
        for (uint32_t b = 0; b < d->f->n_blocks; b++)
            for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = d->f->blocks[b]->instrs[i];
                uint32_t index = in->op == op ? instr_index(d, in)
                                              : UINT32_MAX;
                if (index == UINT32_MAX) continue;
                any = true;
                merge(d, data, row(d, d->data, index));
                merge(d, control, row(d, d->control, index));
            }
        if (any) add_output(d, (ir_soutput_kind_t)kind,
                            (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                            data, control);
    }
    for (size_t a = 0; a < d->summary->accesses.len; a++) {
        const ir_saccess_t *access = &d->summary->accesses.data[a];
        if (!(access->flags & IR_SACCESS_WRITE)) continue;
        memset(data, 0, d->words * sizeof(uint64_t));
        memset(control, 0, d->words * sizeof(uint64_t));
        for (uint32_t p = 0; p < ir_places_count(d->places); p++) {
            ir_splace_t mapped;
            if (portable_place(d->f, ir_place_at(d->places, p), &mapped)
                && ir_splace_eq(&mapped, &access->place))
                merge(d, data, row(d, d->place_stores, p));
        }
        for (uint32_t b = 0; b < d->f->n_blocks; b++)
            for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = d->f->blocks[b]->instrs[i];
                uint32_t index = is_call(in)
                        && call_writes_place(d, in, &access->place)
                    ? instr_index(d, in) : UINT32_MAX;
                if (index != UINT32_MAX) {
                    merge(d, data, row(d, d->data, index));
                    merge(d, control, row(d, d->control, index));
                }
            }
        for (uint32_t a = 0; a < ir_places_access_count(d->places); a++) {
            const ir_access_t *store = ir_places_access_at(d->places, a);
            const uint32_t    *ids   = ir_places_access_places(d->places,
                                                               store);
            uint32_t           index = instr_index(d, store->instr);
            bool               hits  = false;
            for (uint32_t p = 0; !hits && p < store->count; p++) {
                ir_splace_t mapped;
                hits = portable_place(d->f, ir_place_at(d->places, ids[p]),
                                      &mapped)
                    && ir_splace_eq(&mapped, &access->place);
            }
            if (store->kind == IR_ACCESS_STORE && hits && index != UINT32_MAX)
                merge(d, control, row(d, d->control, index));
        }
        add_output(d, IR_SOUTPUT_PLACE, access->place, data, control);
    }
    core_free(data);
    core_free(control);
}

static void
summarize_dependences(ir_summaries_t *summaries, ir_summary_t *summary,
                      ir_func_t *f)
{
    summary->inputs.len      = 0;
    summary->outputs.len     = 0;
    summary->dependences.len = 0;
    deps_t d = {.summaries = summaries, .summary = summary, .f = f,
                .places = ir_func_places(f), .memssa = ir_func_memssa(f),
                .pdg = ir_func_pdg(f)};
    if (!d.places) return;
    core_dict_init(&d.index, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            core_dict_put(&d.index, in, (void *)(uintptr_t)(++d.instrs));
            for (uint32_t o = 1; is_call(in) && o < in->n_operands; o++) {
                const uint32_t *ids = nullptr;
                uint32_t        n   = 0;
                ir_value_t     *arg = ir_operand_value(&in->operands[o]);
                argument_places(d.places, f, arg, &ids, &n);
            }
        }
    d.access_first = core_alloc_array(uint32_t, d.instrs + 1);
    d.access_count = core_alloc_array(uint32_t, d.instrs + 1);
    for (uint32_t a = 0, index = 0, at = 0; at < f->n_blocks; at++)
        for (uint32_t i = 0; i < f->blocks[at]->n_instrs; i++, index++) {
            d.access_first[index] = a;
            while (a < ir_places_access_count(d.places)
                   && ir_places_access_at(d.places, a)->instr
                          == f->blocks[at]->instrs[i])
                a++;
            d.access_count[index] = a - d.access_first[index];
        }
    for (uint32_t i = 0; i < f->n_params; i++) {
        input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_VALUE, .param = i});
        input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_REFERENT,
                                        .param = i});
    }
    for (uint32_t p = 0; p < ir_places_count(d.places); p++)
        entry_input(&d, p);
    input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    d.words   = (uint32_t)((summary->inputs.len + 63) / 64 + 1);
    d.data    = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.control = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.place_stores = core_alloc_array(uint64_t,
        (size_t)(ir_places_count(d.places) + 1) * d.words);
    for (uint32_t round = 0; round < 256; round++) {
        d.changed = false;
        uint32_t index = 0;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++, index++) {
                instruction_dependences(&d, block->instrs[i], index);
                block_control(&d, block, row(&d, d.control, index));
            }
        }
        for (uint32_t a = 0; a < ir_places_access_count(d.places); a++) {
            const ir_access_t *access = ir_places_access_at(d.places, a);
            if (access->kind != IR_ACCESS_STORE) continue;
            uint32_t        from = instr_index(&d, access->instr);
            const uint32_t *ids  = ir_places_access_places(d.places, access);
            for (uint32_t p = 0; from != UINT32_MAX && p < access->count; p++)
                if (merge(&d, row(&d, d.place_stores, ids[p]),
                          row(&d, d.data, from)))
                    d.changed = true;
        }
        if (!d.changed) break;
    }
    collect_outputs(&d);
    core_free(d.access_first);
    core_free(d.access_count);
    core_free(d.data);
    core_free(d.control);
    core_free(d.place_stores);
    core_dict_free(&d.index);
}

static bool
same_summary(const ir_summary_t *a, const core_list_t(ir_saccess_t) *before,
             uint32_t flags)
{
    if (a->flags != flags || a->accesses.len != before->len) return false;
    for (size_t i = 0; i < before->len; i++) {
        bool found = false;
        for (size_t j = 0; !found && j < a->accesses.len; j++)
            found = ir_splace_eq(&a->accesses.data[j].place,
                                 &before->data[i].place)
                && a->accesses.data[j].flags == before->data[i].flags;
        if (!found) return false;
    }
    return true;
}

typedef struct {
    ir_summaries_t                *summaries;
    core_dict_t                    index;
    core_dict_t                    lowlink;
    core_dict_t                    on_stack;
    core_list_t(summary_func_ptr_t) stack;
    uint32_t                       counter;
} tarjan_t;

static void direct_callees(ir_summaries_t *summaries, ir_func_t *f,
                           core_list_t(summary_func_ptr_t) *out);

static void
solve_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *summary = ir_summary_new(summaries, members[i]);
        summary->flags = IR_SUMMARY_NO_RETURN | IR_SUMMARY_DIVERGES;
        core_dict_put(&summaries->by_function, members[i], summary);
    }
    bool cyclic = count > 1;
    for (size_t i = 0; !cyclic && i < count; i++) {
        core_list_t(summary_func_ptr_t) callees =
            core_list_new(summary_func_ptr_t);
        direct_callees(summaries, members[i], &callees);
        for (size_t c = 0; c < callees.len; c++)
            cyclic |= callees.data[c] == members[i];
        core_list_free(callees);
    }
    for (uint32_t round = 0; round < 64; round++) {
        bool changed = false;
        for (size_t i = 0; i < count; i++) {
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *next    = ir_summary_new(nullptr, members[i]);
            summarize_function(summaries, next, members[i]);
            if (!same_summary(next, &summary->accesses, summary->flags))
                changed = true;
            ir_summary_t previous = *summary;
            *summary = *next;
            *next    = previous;
            summary_free(next);
        }
        if (!cyclic || !changed) break;
    }
}

static void
direct_callees(ir_summaries_t *summaries, ir_func_t *f,
               core_list_t(summary_func_ptr_t) *out)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (!is_call(in)) continue;
            call_targets_t targets;
            resolve_targets(summaries, in, &targets);
            for (size_t t = 0; t < targets.targets.len; t++)
                if (targets.targets.data[t]->n_blocks)
                    core_list_push(*out, targets.targets.data[t]);
            core_list_free(targets.targets);
        }
}

static uint32_t
dict_u32(core_dict_t *dict, const void *key, bool *found)
{
    return (uint32_t)(uintptr_t)core_dict_get(dict, (void *)key, found);
}

typedef struct {
    ir_func_t                      *f;
    core_list_t(summary_func_ptr_t) callees;
    size_t                          next;
} frame_t;
core_list_decl(frame_t);

static void
strongconnect(tarjan_t *t, ir_func_t *root)
{
    core_list_t(frame_t) frames = core_list_new(frame_t);
    frame_t first = {.f = root, .callees = core_list_new(summary_func_ptr_t)};
    direct_callees(t->summaries, root, &first.callees);
    core_dict_put(&t->index, root, (void *)(uintptr_t)(t->counter + 1));
    core_dict_put(&t->lowlink, root, (void *)(uintptr_t)(t->counter + 1));
    t->counter++;
    core_list_push(t->stack, root);
    core_dict_put(&t->on_stack, root, root);
    core_list_push(frames, first);
    while (frames.len) {
        frame_t *frame = &frames.data[frames.len - 1];
        if (frame->next < frame->callees.len) {
            ir_func_t *callee = frame->callees.data[frame->next++];
            bool found = false;
            dict_u32(&t->index, callee, &found);
            if (!found) {
                frame_t next = {.f = callee,
                                .callees = core_list_new(summary_func_ptr_t)};
                direct_callees(t->summaries, callee, &next.callees);
                core_dict_put(&t->index, callee,
                              (void *)(uintptr_t)(t->counter + 1));
                core_dict_put(&t->lowlink, callee,
                              (void *)(uintptr_t)(t->counter + 1));
                t->counter++;
                core_list_push(t->stack, callee);
                core_dict_put(&t->on_stack, callee, callee);
                core_list_push(frames, next);
            }
            else if (core_dict_contains(&t->on_stack, callee)) {
                uint32_t low = dict_u32(&t->lowlink, frame->f, &found);
                uint32_t idx = dict_u32(&t->index, callee, &found);
                if (idx < low)
                    core_dict_put(&t->lowlink, frame->f,
                                  (void *)(uintptr_t)idx);
            }
            continue;
        }
        ir_func_t *f = frame->f;
        bool found = false;
        uint32_t low = dict_u32(&t->lowlink, f, &found);
        uint32_t idx = dict_u32(&t->index, f, &found);
        if (low == idx) {
            core_list_t(summary_func_ptr_t) members =
                core_list_new(summary_func_ptr_t);
            ir_func_t *member;
            do {
                member = t->stack.data[--t->stack.len];
                core_dict_remove(&t->on_stack, member);
                core_list_push(members, member);
            } while (member != f);
            solve_component(t->summaries, members.data, members.len);
            core_list_free(members);
        }
        core_list_free(frame->callees);
        frames.len--;
        if (frames.len) {
            ir_func_t *parent = frames.data[frames.len - 1].f;
            uint32_t parent_low = dict_u32(&t->lowlink, parent, &found);
            if (low < parent_low)
                core_dict_put(&t->lowlink, parent, (void *)(uintptr_t)low);
        }
    }
    core_list_free(frames);
}

ir_summaries_t *
ir_summaries_compute(ir_func_t *const *functions, size_t count,
                     const ir_summary_hooks_t *hooks)
{
    ir_summaries_t *summaries = core_alloc(ir_summaries_t);
    summaries->hooks = hooks;
    summaries->owned = core_list_new(summary_ptr_t);
    core_dict_init(&summaries->by_function, nullptr, nullptr);
    tarjan_t t = {.summaries = summaries,
                  .stack = core_list_new(summary_func_ptr_t)};
    core_dict_init(&t.index, nullptr, nullptr);
    core_dict_init(&t.lowlink, nullptr, nullptr);
    core_dict_init(&t.on_stack, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) {
        bool found = false;
        dict_u32(&t.index, functions[i], &found);
        if (!found && functions[i]->n_blocks) strongconnect(&t, functions[i]);
    }
    core_dict_free(&t.index);
    core_dict_free(&t.lowlink);
    core_dict_free(&t.on_stack);
    core_list_free(t.stack);
    return summaries;
}

void
ir_summaries_free(ir_summaries_t *summaries)
{
    if (!summaries) return;
    for (size_t i = 0; i < summaries->owned.len; i++)
        summary_free(summaries->owned.data[i]);
    core_list_free(summaries->owned);
    core_dict_free(&summaries->by_function);
    core_free(summaries);
}
