#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/layout.h"
#include "ir/summary.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(ir_saccess_t);
core_list_decl(ir_sfield_t);
core_list_decl(ir_strap_site_t);
core_list_decl(ir_sinput_t);
core_list_decl(ir_soutput_t);
core_list_decl(uint32_t);
core_list_decl(uint8_t);
typedef ir_func_t *summary_func_ptr_t;
core_list_decl(summary_func_ptr_t);
typedef ir_block_t *summary_block_ptr_t;
core_list_decl(summary_block_ptr_t);
typedef ir_summary_t *summary_ptr_t;
core_list_decl(summary_ptr_t);

struct ir_summary_t {
    const ir_func_t           *function;
    uint32_t                   flags;
    core_list_t(ir_saccess_t)  accesses;
    core_list_t(ir_sinput_t)   inputs;
    core_list_t(ir_soutput_t)  outputs;
    core_list_t(uint32_t)      dependences;
    core_list_t(uint8_t)       escapes;
    bool                       all_escape;
    ir_sreturn_t               returned;
    uint8_t                    traps;
    core_list_t(ir_strap_site_t) trap_sites;
    ir_sflow_t                 flow;
    core_list_t(ir_sfield_t)   fields;
};

void
ir_sflow_release(ir_sflow_t *flow)
{
    core_free(flow->carried);
    flow->carried   = nullptr;
    flow->n_carried = 0;
}

ir_sflow_t
ir_sflow_opaque(void)
{
    ir_sflow_t flow = {
        .receives = UINT64_MAX, .receives_fresh = true,
        .receives_nonlocal = true, .writes_nonlocal = true, .opaque = true,
    };
    for (int kind = 0; kind < IR_SFLOW_KINDS; kind++)
        flow.writes[kind] = flow.escapes[kind] = flow.leaks[kind]
            = UINT64_MAX;
    return flow;
}

bool
ir_sflow_eq(const ir_sflow_t *a, const ir_sflow_t *b)
{
    return !memcmp(a->writes, b->writes, sizeof(a->writes))
        && !memcmp(a->escapes, b->escapes, sizeof(a->escapes))
        && !memcmp(a->leaks, b->leaks, sizeof(a->leaks))
        && a->receives == b->receives
        && a->receives_fresh == b->receives_fresh
        && a->receives_nonlocal == b->receives_nonlocal
        && a->writes_nonlocal == b->writes_nonlocal
        && a->opaque == b->opaque && a->n_carried == b->n_carried
        && (!a->n_carried
            || !memcmp(a->carried, b->carried,
                       a->n_carried * sizeof(*a->carried)));
}

const ir_sflow_t *
ir_summary_flow(const ir_summary_t *summary)
{
    return summary ? &summary->flow : nullptr;
}

void
ir_summary_set_flow(ir_summary_t *summary, const ir_sflow_t *flow)
{
    ir_sflow_release(&summary->flow);
    summary->flow = *flow;
    summary->flow.carried = flow->n_carried
        ? core_alloc_array(ir_scarried_t, flow->n_carried) : nullptr;
    if (flow->n_carried)
        memcpy(summary->flow.carried, flow->carried,
               flow->n_carried * sizeof(*flow->carried));
}

static uint8_t
trap_kind(const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_TRAP:
        return IR_STRAP_EXPLICIT;
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_SET:
        return IR_STRAP_BOUNDS;
    case IR_OP_OPTION_GET:
        return IR_STRAP_UNWRAP;
    default:
        return IR_STRAP_ARITHMETIC;
    }
}

struct ir_summaries_t {
    const ir_summary_hooks_t *hooks;
    core_list_t(summary_ptr_t) owned;
    core_dict_t               by_function;
    core_dict_t               names;
    bool                      escapes_only;
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
    summary->escapes     = core_list_new(uint8_t);
    summary->fields      = core_list_new(ir_sfield_t);
    summary->trap_sites  = core_list_new(ir_strap_site_t);
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

static void
summary_free(ir_summary_t *summary)
{
    core_list_free(summary->trap_sites);
    core_list_free(summary->escapes);
    core_list_free(summary->accesses);
    core_list_free(summary->inputs);
    core_list_free(summary->outputs);
    core_list_free(summary->dependences);
    core_list_free(summary->fields);
    ir_sflow_release(&summary->flow);
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
    summary->all_escape = true;
    summary->traps      = IR_STRAP_ALL;
    summary->returned   = (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    summary->flow       = ir_sflow_opaque();
    return summary;
}

uint8_t
ir_summary_param_escape(const ir_summary_t *summary, uint32_t param)
{
    if (!summary || summary->all_escape)
        return IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT;
    return param < summary->escapes.len ? summary->escapes.data[param] : 0;
}

void
ir_summary_set_param_escape(ir_summary_t *summary, uint32_t param,
                            uint8_t flags)
{
    while (summary->escapes.len <= param) core_list_push(summary->escapes, 0);
    summary->escapes.data[param] |= flags;
}

ir_sreturn_t
ir_summary_return(const ir_summary_t *summary)
{
    return summary ? summary->returned
                   : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
}

void
ir_summary_set_return(ir_summary_t *summary, ir_sreturn_t returned)
{
    summary->returned = returned;
}

const ir_func_t *ir_summary_function(const ir_summary_t *s) { return s->function; }
uint32_t ir_summary_flags(const ir_summary_t *s) { return s ? s->flags : 0; }
uint8_t ir_summary_trap_kinds(const ir_summary_t *s)
{
    return s ? s->traps : IR_STRAP_ALL;
}
void ir_summary_set_trap_kinds(ir_summary_t *s, uint8_t kinds)
{
    s->traps |= kinds & IR_STRAP_ALL;
}
uint32_t ir_summary_trap_site_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->trap_sites.len : 0;
}
const ir_strap_site_t *ir_summary_trap_site_at(const ir_summary_t *s,
                                               uint32_t i)
{
    return s && i < s->trap_sites.len ? &s->trap_sites.data[i] : nullptr;
}
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

ir_summary_t *
ir_summary_for(ir_summaries_t *summaries, ir_func_t *function)
{
    return function ? target_summary(summaries, function)
                    : ir_summary_conservative(summaries, nullptr);
}

uint64_t
ir_summary_effect_bits(const ir_summary_t *summary)
{
    uint64_t bits = summary->flags & 0xffffu;
    bits |= (uint64_t)summary->traps << IR_SEFFECT_TRAPS_SHIFT;
    for (size_t a = 0; a < summary->accesses.len; a++) {
        const ir_saccess_t *access = &summary->accesses.data[a];
        bool read  = access->flags & IR_SACCESS_READ;
        bool write = access->flags & IR_SACCESS_WRITE;
        if (access->place.kind != IR_SPLACE_PARAM) {
            if (read) bits |= IR_SEFFECT_READS_NONLOCAL;
            if (write) bits |= IR_SEFFECT_WRITES_NONLOCAL;
            continue;
        }
        uint32_t param = access->place.param < 16 ? access->place.param : 15;
        if (read) bits |= UINT64_C(1) << (IR_SEFFECT_READS_PARAM_SHIFT + param);
        if (write)
            bits |= UINT64_C(1) << (IR_SEFFECT_WRITES_PARAM_SHIFT + param);
    }
    return bits;
}

static bool is_call(const ir_instr_t *in);

uint8_t
ir_summary_call_escape(ir_summaries_t *summaries, const ir_instr_t *call,
                       uint32_t param)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    uint8_t flags = targets.unknown
        ? IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT : 0;
    for (size_t t = 0; !targets.unknown && t < targets.targets.len; t++)
        flags |= ir_summary_param_escape(
            target_summary(summaries, targets.targets.data[t]), param);
    core_list_free(targets.targets);
    return flags;
}

bool
ir_summary_call_diverges(ir_summaries_t *summaries, const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    bool diverges = !targets.unknown && targets.targets.len;
    for (size_t t = 0; diverges && t < targets.targets.len; t++)
        diverges = (target_summary(summaries, targets.targets.data[t])->flags
                    & IR_SUMMARY_DIVERGES) != 0;
    core_list_free(targets.targets);
    return diverges;
}

const ir_summary_t *
ir_summary_entries(ir_summaries_t *summaries, uint32_t flag)
{
    const ir_summary_hooks_t *hooks = summaries ? summaries->hooks : nullptr;
    return hooks && hooks->entries
        ? hooks->entries(hooks->context, summaries, flag) : nullptr;
}

static bool
unseen_reaches(ir_summaries_t *summaries, const ir_splace_t *place)
{
    if (place->kind != IR_SPLACE_GLOBAL && place->kind != IR_SPLACE_THREAD)
        return true;
    const ir_global_t *global = place->global
        ? ir_global_identity(place->global) : nullptr;
    const ir_summary_hooks_t *hooks = summaries ? summaries->hooks : nullptr;
    return !global || !hooks || !hooks->global_reachable
        || hooks->global_reachable(hooks->context, global);
}

static bool
access_touches(ir_summaries_t *summaries, const ir_splace_t *access,
               const ir_splace_t *place)
{
    return access->kind == IR_SPLACE_UNKNOWN
        ? unseen_reaches(summaries, place)
        : ir_splace_eq(access, place);
}

bool
ir_summary_place_unseen(ir_summaries_t *summaries, const ir_splace_t *place)
{
    return unseen_reaches(summaries, place);
}

bool
ir_summary_call_callees(ir_summaries_t *summaries, const ir_instr_t *call,
                        ir_summary_callee_fn visit, void *context)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    for (size_t t = 0; t < targets.targets.len; t++)
        visit(context, targets.targets.data[t],
              target_summary(summaries, targets.targets.data[t]));
    core_list_free(targets.targets);
    return !targets.unknown;
}

static void
check_pure_callee(void *context, const ir_func_t *target,
                  const ir_summary_t *summary)
{
    (void)target;
    const uint32_t effects = IR_SUMMARY_MAY_FAIL | IR_SUMMARY_MAY_BLOCK
        | IR_SUMMARY_DIVERGES | IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_IO
        | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS;
    bool *pure = context;
    *pure = *pure && !(summary->flags & effects);
    for (size_t a = 0; *pure && a < summary->accesses.len; a++)
        *pure = !(summary->accesses.data[a].flags & IR_SACCESS_WRITE);
}

bool
ir_summary_call_pure(ir_summaries_t *summaries, const ir_instr_t *call)
{
    bool pure = summaries != nullptr;
    return pure
        && ir_summary_call_callees(summaries, call, check_pure_callee, &pure)
        && pure;
}

static bool
place_meets(ir_module_t *module, const ir_place_t *place,
            const ir_splace_t *other)
{
    if (place->kind == IR_PLACE_LOCAL)
        return place->exposed && other->kind == IR_SPLACE_UNKNOWN;
    if (place->kind == IR_PLACE_UNKNOWN || other->kind == IR_SPLACE_UNKNOWN)
        return true;
    bool named = place->kind == IR_PLACE_GLOBAL
        || place->kind == IR_PLACE_THREAD;
    bool other_named = other->kind == IR_SPLACE_GLOBAL
        || other->kind == IR_SPLACE_THREAD;
    if (named && other_named) {
        const ir_global_t *global = place->root && place->root->global
            ? ir_global_identity(place->root->global) : nullptr;
        return !global || !other->global
            || global == ir_global_identity(other->global);
    }
    if (place->kind == IR_PLACE_HEAP && place->root
        && other->kind != IR_SPLACE_HEAP) return false;
    return !place->type || !other->type
        || ir_places_types_overlap(module, place->type, other->type);
}

typedef struct {
    ir_summaries_t   *summaries;
    const ir_func_t  *f;
    ir_places_t      *places;
    const ir_instr_t *call;
    const uint32_t   *ids;
    uint32_t          count;
} write_query_t;

static bool
access_writes(write_query_t *q, const ir_splace_t *place, bool arguments)
{
    if (place->kind == IR_SPLACE_PARAM && arguments) {
        uint32_t operand = place->param + 1;
        const ir_value_t *arg = operand < q->call->n_operands
            ? ir_operand_value(&q->call->operands[operand]) : nullptr;
        if (!arg) return true;
        if (!addressable(q->f, arg)) return false;
        const uint32_t *written = nullptr;
        uint32_t        n       = 0;
        if (!argument_places(q->places, q->f, arg, &written, &n)) return true;
        for (uint32_t w = 0; w < n; w++)
            for (uint32_t i = 0; i < q->count; i++)
                if (ir_places_may_alias(q->places, written[w], q->ids[i]))
                    return true;
        return false;
    }
    ir_splace_t other = place->kind == IR_SPLACE_PARAM
        ? (ir_splace_t){.kind = IR_SPLACE_UNKNOWN} : *place;
    for (uint32_t i = 0; i < q->count; i++)
        if (place_meets(q->f->owner, ir_place_at(q->places, q->ids[i]),
                        &other))
            return true;
    return false;
}

static bool
summary_writes(write_query_t *q, const ir_summary_t *summary, bool follow)
{
    if (!summary) return true;
    for (size_t a = 0; a < summary->accesses.len; a++)
        if ((summary->accesses.data[a].flags & IR_SACCESS_WRITE)
            && access_writes(q, &summary->accesses.data[a].place, follow))
            return true;
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; follow && e < sizeof(entries) / sizeof(*entries); e++)
        if ((summary->flags & entries[e])
            && summary_writes(q, ir_summary_entries(q->summaries, entries[e]),
                              false))
            return true;
    return false;
}

bool
ir_summary_call_writes(ir_summaries_t *summaries, const ir_func_t *f,
                       ir_places_t *places, const ir_instr_t *call,
                       uint32_t place)
{
    if (!summaries || !f || !places || !call) return true;
    if (call->op != IR_OP_CALL && call->op != IR_OP_CALLBACK_INVOKE)
        return true;
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    write_query_t q = {
        .summaries = summaries, .f = f, .places = places, .call = call,
        .ids = &place, .count = 1,
    };
    bool writes = targets.unknown;
    for (size_t t = 0; !writes && t < targets.targets.len; t++)
        writes = summary_writes(&q, target_summary(summaries,
                                                   targets.targets.data[t]),
                                true);
    core_list_free(targets.targets);
    return writes;
}

bool
ir_summary_argument_addressable(const ir_func_t *function,
                                const ir_value_t *argument)
{
    return addressable(function, argument);
}

bool
ir_summary_place_of(const ir_func_t *function, const ir_place_t *place,
                    ir_splace_t *out)
{
    return portable_place(function, place, out);
}

static bool
call_returns_parameter(ir_summaries_t *summaries, const ir_instr_t *call,
                       uint32_t param)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    bool returns = targets.unknown;
    for (size_t t = 0; !targets.unknown && t < targets.targets.len; t++) {
        ir_sreturn_t returned = ir_summary_return(
            target_summary(summaries, targets.targets.data[t]));
        returns |= returned.kind == IR_SRETURN_UNKNOWN
            || (returned.kind == IR_SRETURN_PARAM
                && returned.param == param);
    }
    core_list_free(targets.targets);
    return returns;
}

typedef struct {
    const ir_value_t *value;
    bool              storage;
} escape_value_t;
core_list_decl(escape_value_t);

typedef struct {
    ir_summaries_t              *summaries;
    const ir_func_t             *function;
    bool                         returns_escape;
    bool                         returned;
    bool                         referent;
    uint32_t                     param;
    core_list_t(escape_value_t)  values;
} escape_walk_t;

static bool holds_reference(const ir_func_t *f, const ir_value_t *value);

static bool
tracked(const escape_walk_t *walk, const ir_value_t *value, bool storage)
{
    for (size_t i = 0; i < walk->values.len; i++)
        if (walk->values.data[i].value == value
            && walk->values.data[i].storage == storage) return true;
    return false;
}

static bool
derived(const escape_walk_t *walk, const ir_value_t *value)
{
    return tracked(walk, value, false);
}

static void
track(escape_walk_t *walk, const ir_value_t *value, bool storage)
{
    if (!value || tracked(walk, value, storage)) return;
    if (!storage && !holds_reference(walk->function, value)) return;
    core_list_push(walk->values,
                   ((escape_value_t){.value = value, .storage = storage}));
}

static void
derive(escape_walk_t *walk, const ir_value_t *value)
{
    track(walk, value, false);
}

static const ir_value_t *used_value(const ir_operand_t *operand);

static bool
inline_elements(const ir_func_t *f, const ir_value_t *base)
{
    const ir_instr_t *field = base->kind == IR_VAL_INSTR ? base->def : nullptr;
    if (!field || field->op != IR_OP_FIELD || field->n_operands != 2
        || field->operands[0].kind != IR_OPD_VALUE
        || field->operands[1].kind != IR_OPD_NAME
        || !field->operands[0].value) return false;
    th_type *record = th_ind_referent(field->operands[0].value->type);
    const ir_final_layout_t *layout = record
        ? ir_layout_final(f->owner, record, ir_module_target(f->owner))
        : nullptr;
    for (size_t i = 0; layout && i < layout->n_members; i++)
        if (layout->members[i].name.data
            && core_string_eq(layout->members[i].name,
                              field->operands[1].name))
            return layout->members[i].inline_element_count != 0;
    return !layout;
}

static bool
positional_argument(const ir_instr_t *call, uint32_t index)
{
    if (index == 0) return false;
    for (uint32_t i = 1; i <= index; i++) {
        ir_opdkind_t kind = call->operands[i].kind;
        if (kind != IR_OPD_VALUE && kind != IR_OPD_FRESH
            && kind != IR_OPD_OUT) return false;
    }
    return true;
}

static bool
returns_value(const ir_func_t *f)
{
    th_type *success = th_func_ret(f->type);
    return success && ir_machine_of(f->owner, success).class != IR_CLASS_VOID;
}

static bool
return_operand_escapes(escape_walk_t *walk, uint32_t index)
{
    const ir_func_t *f = walk->function;
    uint32_t first_output = returns_value(f) ? 1 : 0;
    if (index < first_output) {
        walk->returned = true;
        return walk->returns_escape;
    }
    uint32_t output = first_output;
    for (size_t i = 0; i < th_func_nparams(f->type); i++) {
        const th_param *formal = th_func_param(f->type, i);
        if (!formal || !th_mode_produces_output(formal->mode)) continue;
        if (output++ == index)
            return walk->returns_escape || i != walk->param;
    }
    return true;
}

static bool
hold_in_local(escape_walk_t *walk, const ir_value_t *target)
{
    for (unsigned depth = 0; target && depth < 32; depth++) {
        const ir_instr_t *def = target->kind == IR_VAL_INSTR ? target->def
                                                             : nullptr;
        if (!def) return false;
        switch (def->op) {
        case IR_OP_FIELD:
        case IR_OP_INDEX:
        case IR_OP_OFFSET:
        case IR_OP_BITCAST:
        case IR_OP_COPY:
            target = def->n_operands ? used_value(&def->operands[0])
                                     : nullptr;
            continue;
        case IR_OP_ADDRESS_OF: {
            const ir_value_t *local = def->n_operands
                ? used_value(&def->operands[0]) : nullptr;
            if (!local || (local->kind != IR_VAL_LOCAL
                           && local->kind != IR_VAL_PARAM)) return false;
            if (!derived(walk, local))
                core_list_push(walk->values,
                    ((escape_value_t){.value = local, .storage = false}));
            return true;
        }
        case IR_OP_ALLOCA:
        case IR_OP_ALLOC:
            track(walk, target, true);
            return true;
        default:
            return false;
        }
    }
    return false;
}

static const ir_value_t *
used_value(const ir_operand_t *operand)
{
    switch (operand->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
    case IR_OPD_ARG:
    case IR_OPD_OUT:
    case IR_OPD_FRESH:
        return operand->value;
    case IR_OPD_PHI_ARM:
        return operand->phi.value;
    case IR_OPD_NAMED_ARG:
        return operand->named.value;
    default:
        return nullptr;
    }
}

static bool
use_escapes(escape_walk_t *walk, const ir_instr_t *use, uint32_t index)
{
    const ir_operand_t *operand = &use->operands[index];
    switch (use->op) {
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_LENGTH:
    case IR_OP_SEQUENCE_SHAPE_CHECK:
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_LT:
    case IR_OP_LE:
    case IR_OP_GT:
    case IR_OP_GE:
    case IR_OP_TYPE_OF:
    case IR_OP_TYPE_TEST:
    case IR_OP_OPTION_IS_NIL:
    case IR_OP_VARIANT_TAG:
    case IR_OP_PREFETCH:
    case IR_OP_MEMCMP:
    case IR_OP_BRANCH:
    case IR_OP_SWITCH:
        return false;
    case IR_OP_STORE:
        return index != 0 && !hold_in_local(walk, used_value(&use->operands[0]));
    case IR_OP_SEQUENCE_SET:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
        return index != 0;
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
        if (index == 1) walk->referent = true;
        return index > 1;
    case IR_OP_MEMSET:
        return index > 1;
    case IR_OP_INDEX:
        if (index == 0 && use->dest && operand->value
            && th_kind_of(th_resolve(operand->value->type)) == TH_INDIRECTION
            && th_kind_of(th_resolve(th_ind_referent(operand->value->type)))
                == TH_SEQUENCE
            && !inline_elements(walk->function, operand->value)) {
            if (ir_address_escapes(walk->summaries, walk->function,
                                   use->dest))
                walk->referent = true;
            return false;
        }
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_FIELD:
    case IR_OP_OFFSET:
    case IR_OP_BITCAST:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_PHI:
    case IR_OP_SELECT:
    case IR_OP_EXTRACT:
    case IR_OP_EXTRACT_INDEX:
    case IR_OP_INSERT:
    case IR_OP_AGGREGATE:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_OPTION_GET_OR_ELSE:
    case IR_OP_VARIANT_MAKE:
    case IR_OP_VARIANT_PAYLOAD:
    case IR_OP_TYPE_NARROW:
    case IR_OP_BOX:
    case IR_OP_CONSUME:
        if (use->op == IR_OP_SELECT && index == 0) return false;
        if (use->op == IR_OP_CONSUME && index != 0) return false;
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_COPY:
        if (use->dest_local) {
            if (!use->dest_local->value) return true;
            derive(walk, use->dest_local->value);
            return false;
        }
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_ADDRESS_OF:
        if (!use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_PARAM_PRESENT:
    case IR_OP_ENUM_VALUE:
        return false;
    case IR_OP_CAST:
        return !operand->value || ir_machine_of(walk->function->owner,
            operand->value->type).class == IR_CLASS_PTR;
    case IR_OP_CALL:
        if (!positional_argument(use, index)) return true;
        uint8_t escape = ir_summary_call_escape(walk->summaries, use,
                                                index - 1);
        if (escape & IR_SESCAPE_ADDRESS) return true;
        if (escape & IR_SESCAPE_REFERENT) walk->referent = true;
        if (use->dest
            && call_returns_parameter(walk->summaries, use, index - 1))
            derive(walk, use->dest);
        return false;
    case IR_OP_RETURN:
        return return_operand_escapes(walk, index);
    default:
        return true;
    }
}

static bool
storage_use_escapes(escape_walk_t *walk, const ir_instr_t *use,
                    uint32_t index)
{
    switch (use->op) {
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET:
        if (use->dest) derive(walk, use->dest);
        return false;
    case IR_OP_STORE:
    case IR_OP_SEQUENCE_SET:
    case IR_OP_MEMSET:
        return index != 0;
    case IR_OP_SEQUENCE_LENGTH:
    case IR_OP_SEQUENCE_SHAPE_CHECK:
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_PREFETCH:
    case IR_OP_MEMCMP:
        return false;
    case IR_OP_CONSUME:
        return index == 0;
    case IR_OP_FIELD:
    case IR_OP_INDEX:
    case IR_OP_OFFSET:
    case IR_OP_BITCAST:
    case IR_OP_PHI:
    case IR_OP_SELECT:
        if (use->op == IR_OP_SELECT && index == 0) return false;
        if (!use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_COPY:
        if (use->dest_local || !use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_CALL:
        if (!positional_argument(use, index)) return true;
        if (ir_summary_call_escape(walk->summaries, use, index - 1))
            return true;
        if (use->dest
            && call_returns_parameter(walk->summaries, use, index - 1))
            track(walk, use->dest, true);
        return false;
    default:
        return true;
    }
}

static bool
walk_escapes(escape_walk_t *walk, const ir_value_t *start)
{
    derive(walk, start);
    const ir_func_t *f = walk->function;
    for (size_t cursor = 0; cursor < walk->values.len; cursor++) {
        const ir_value_t *value = walk->values.data[cursor].value;
        bool storage = walk->values.data[cursor].storage;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            const ir_block_t *block = f->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                const ir_instr_t *use = block->instrs[i];
                for (uint32_t k = 0; k < use->n_operands; k++)
                    if (used_value(&use->operands[k]) == value
                        && (storage ? storage_use_escapes(walk, use, k)
                                    : use_escapes(walk, use, k)))
                        return true;
            }
        }
    }
    return false;
}

bool
ir_address_escapes(ir_summaries_t *summaries, const ir_func_t *function,
                   const ir_value_t *address)
{
    escape_walk_t walk = {
        .summaries = summaries, .function = function, .returns_escape = true,
        .param = UINT32_MAX, .values = core_list_new(escape_value_t),
    };
    bool escapes = walk_escapes(&walk, address);
    core_list_free(walk.values);
    return escapes;
}

static bool
holds_reference(const ir_func_t *f, const ir_value_t *value)
{
    if (!value || !value->type) return true;
    switch (ir_machine_of(f->owner, value->type).class) {
    case IR_CLASS_SINT:
    case IR_CLASS_UINT:
    case IR_CLASS_FLOAT:
    case IR_CLASS_BOOL:
    case IR_CLASS_VOID:
        return false;
    default:
        return true;
    }
}

static bool
referent_escapes(ir_summaries_t *summaries, const ir_func_t *f,
                 const escape_walk_t *walk)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        const ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *use = block->instrs[i];
            if ((use->op != IR_OP_LOAD && use->op != IR_OP_SEQUENCE_GET)
                || !use->n_operands || !use->dest
                || !derived(walk, used_value(&use->operands[0]))
                || !holds_reference(f, use->dest))
                continue;
            if (ir_address_escapes(summaries, f, use->dest)) return true;
        }
    }
    return false;
}

static ir_sreturn_t
join_return(ir_sreturn_t left, ir_sreturn_t right)
{
    if (left.kind == IR_SRETURN_NONE) return right;
    if (right.kind == IR_SRETURN_NONE) return left;
    if (left.kind == right.kind && left.param == right.param
        && left.global == right.global) return left;
    return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
}

static ir_sreturn_t
returned_source(ir_summaries_t *summaries, const ir_func_t *f,
                const ir_value_t *value, const escape_walk_t *walks)
{
    for (uint32_t p = 0; p < f->n_params; p++)
        if (derived(&walks[p], value))
            return (ir_sreturn_t){.kind = IR_SRETURN_PARAM, .param = p};
    if (!value || th_kind_of(th_resolve(value->type)) != TH_INDIRECTION)
        return (ir_sreturn_t){.kind = IR_SRETURN_NONE};
    const ir_instr_t *def = value->kind == IR_VAL_INSTR ? value->def
                                                        : nullptr;
    if (def && def->op == IR_OP_ADDRESS_OF && def->n_operands == 1) {
        const ir_value_t *target = used_value(&def->operands[0]);
        if (target && target->kind == IR_VAL_GLOBAL && target->global)
            return (ir_sreturn_t){.kind = IR_SRETURN_GLOBAL,
                                  .global = target->global};
    }
    if (def && is_call(def)) {
        call_targets_t targets;
        resolve_targets(summaries, def, &targets);
        bool fresh = !targets.unknown && targets.targets.len;
        for (size_t t = 0; fresh && t < targets.targets.len; t++)
            fresh = ir_summary_return(target_summary(summaries,
                targets.targets.data[t])).kind == IR_SRETURN_FRESH_EXTERNAL;
        core_list_free(targets.targets);
        if (fresh) return (ir_sreturn_t){.kind = IR_SRETURN_FRESH_EXTERNAL};
    }
    return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
}

static void
summarize_escapes(ir_summaries_t *summaries, ir_summary_t *summary,
                  const ir_func_t *f)
{
    summary->escapes.len = 0;
    escape_walk_t *walks = f->n_params
        ? core_alloc_array(escape_walk_t, f->n_params) : nullptr;
    for (uint32_t p = 0; p < f->n_params; p++) {
        walks[p] = (escape_walk_t){
            .summaries = summaries, .function = f, .param = p,
            .values = core_list_new(escape_value_t),
        };
        uint8_t flags = 0;
        if (!holds_reference(f, f->params[p])) {
            ir_summary_set_param_escape(summary, p, 0);
            continue;
        }
        if (walk_escapes(&walks[p], f->params[p]))
            flags |= IR_SESCAPE_ADDRESS;
        else if (walks[p].referent
                 || referent_escapes(summaries, f, &walks[p]))
            flags |= IR_SESCAPE_REFERENT;
        ir_summary_set_param_escape(summary, p, flags);
    }
    ir_sreturn_t returned = {.kind = IR_SRETURN_NONE};
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        const ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_RETURN || !in->n_operands || !returns_value(f)
                || in->operands[0].kind != IR_OPD_VALUE) continue;
            returned = join_return(returned, returned_source(summaries, f,
                in->operands[0].value, walks));
        }
    }
    summary->returned = returned;
    for (uint32_t p = 0; p < f->n_params; p++)
        core_list_free(walks[p].values);
    core_free(walks);
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
            : access->place.kind == IR_SPLACE_UNKNOWN
            ? unseen_reaches(q->summaries, q->target)
            : named_place(&access->place)
                && ir_splace_eq(&access->place, q->target);
        if (matches)
            flags |= access->place.kind == IR_SPLACE_UNKNOWN
                ? access->flags & (IR_SACCESS_READ | IR_SACCESS_WRITE
                                   | IR_SACCESS_READS_ENTRY)
                : access->flags;
    }
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; q->target && e < sizeof(entries) / sizeof(*entries);
         e++) {
        if (!(s->flags & entries[e])) continue;
        const ir_summary_t *entry = ir_summary_entries(q->summaries,
                                                       entries[e]);
        if (!entry) {
            if (unseen_reaches(q->summaries, q->target))
                flags |= IR_SACCESS_READ | IR_SACCESS_WRITE
                    | IR_SACCESS_READS_ENTRY;
            continue;
        }
        for (size_t a = 0; a < entry->accesses.len; a++)
            if (entry->accesses.data[a].place.kind != IR_SPLACE_PARAM
                && access_touches(q->summaries,
                                  &entry->accesses.data[a].place, q->target))
                flags |= entry->accesses.data[a].flags
                    & (IR_SACCESS_READ | IR_SACCESS_WRITE
                       | IR_SACCESS_READS_ENTRY);
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

static field_leaf_t
read_path(const ir_splace_t *place, const ir_instr_t *in)
{
    field_leaf_t path = {0};
    core_string_t names[FIELD_DEPTH];
    uint32_t      depth = 0;
    ir_op_t       step  = in->op == IR_OP_LOAD ? IR_OP_FIELD : IR_OP_EXTRACT;
    if ((in->op != IR_OP_LOAD && in->op != IR_OP_EXTRACT) || !in->n_operands)
        return path;
    if (in->op == IR_OP_EXTRACT) {
        if (in->n_operands != 2 || in->operands[1].kind != IR_OPD_NAME)
            return path;
        names[depth++] = in->operands[1].name;
    }
    const ir_value_t *address = ir_operand_value(&in->operands[0]);
    while (address && address->kind == IR_VAL_INSTR && address->def
           && address->def->op == step && address->def->n_operands == 2
           && address->def->operands[1].kind == IR_OPD_NAME) {
        if (depth == FIELD_DEPTH) return (field_leaf_t){0};
        names[depth++] = address->def->operands[1].name;
        address = ir_operand_value(&address->def->operands[0]);
    }
    if (!depth || !address || address->kind != IR_VAL_GLOBAL
        || !address->global
        || ir_global_identity(address->global) != place->global)
        return path;
    for (uint32_t i = 0; i < depth; i++)
        path.names[i] = names[depth - 1 - i];
    path.depth = depth;
    return path;
}

static bool
leaf_under(const field_leaf_t *leaf, const field_leaf_t *path)
{
    uint32_t common = leaf->depth < path->depth ? leaf->depth : path->depth;
    for (uint32_t i = 0; i < common; i++)
        if (!core_string_eq(leaf->names[i], path->names[i])) return false;
    return true;
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
        field_leaf_t read = read_path(place, in);
        core_list_t(field_leaf_t) leaves = place_leaves(f, place);
        bool first = true;
        for (size_t l = 0; leaves.data[0].depth && l < leaves.len; l++) {
            if (!leaf_under(&leaves.data[l], &read)) continue;
            q.leaf = &leaves.data[l];
            state  = combine_leaf(state,
                state_before_leaf(&q, memssa, doms, entry, in), first);
            first  = false;
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
        case IR_MEMDEF_ELEMENT_PHI:
            continue;
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
    if (call->op == IR_OP_CALLBACK_INVOKE)
        summary->flags |= IR_SUMMARY_CALLS_CALLBACKS;
    if (targets.unknown) {
        summary->flags |= IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
            | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
            | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT;
        summary->traps |= IR_STRAP_ALL;
        ir_summary_add_access(summary, (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                              IR_SACCESS_READ | IR_SACCESS_WRITE
                                  | IR_SACCESS_READS_ENTRY);
        core_list_free(targets.targets);
        return;
    }
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(summaries,
                                              targets.targets.data[t]);
        summary->traps |= callee->traps;
        summary->flags |= callee->flags
            & (IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
               | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
               | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
               | IR_SUMMARY_ALLOCATES_DIRECT | IR_SUMMARY_CALLS_FINALIZERS
               | IR_SUMMARY_STARTS_THREADS | IR_SUMMARY_REINTERPRETS
               | IR_SUMMARY_CALLS_CALLBACKS);
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
    case IR_OP_ARRAY_NEW:
        return in->dest && th_is_closed(ir_module_types(f->owner),
                                        in->dest->type)
            ? IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_COLLECT
            : IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_ALLOC_USING:
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
    summary->flags = ir_reinterpretations_note_function(nullptr, f)
        ? IR_SUMMARY_REINTERPRETS : 0;
    summary->accesses.len = 0;
    ir_places_t *places = ir_func_places(f);
    ir_sites_t  *sites  = ir_func_sites(f);
    ir_memssa_t *memssa = ir_func_memssa(f);
    ir_ranges_t *prove_ranges = (!summaries->hooks
                                 || !summaries->hooks->defer_trap_proofs)
        && (ir_module_props(f->owner) & IR_PROP_SSA)
        ? ir_func_ranges(f) : nullptr;
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
        if (site->kind != IR_SITE_TRAP
            || (prove_ranges && ir_range_trap_proven(prove_ranges,
                                                     site->instr))) continue;
        uint8_t kind = trap_kind(site->instr);
        summary->flags |= IR_SUMMARY_MAY_TRAP;
        summary->traps |= kind;
        core_list_push(summary->trap_sites, ((ir_strap_site_t){
            .kind   = kind,
            .line   = site->instr->span.start_line,
            .column = site->instr->span.start_col,
        }));
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
    summarize_escapes(summaries, summary, f);
    summarize_dependences(summaries, summary, f);
    ir_analysis_scope_end(scope);
}

typedef struct {
    core_list_t(uint32_t) bits;
    core_list_t(uint32_t) rows;
    core_list_t(uint32_t) stores;
    core_list_t(uint32_t) loads;
    core_list_t(uint32_t) parts;
    bool                  built;
} recipe_t;

typedef recipe_t *recipe_ptr_t;
core_list_decl(recipe_ptr_t);

typedef struct {
    ir_splace_t place;
    uint32_t    part;
} place_part_t;
core_list_decl(place_part_t);

typedef struct {
    ir_splace_t   place;
    ir_summary_t *callee;
    int32_t       output;
    uint32_t      owner;
    uint32_t      part;
} call_output_t;
core_list_decl(call_output_t);

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
    uint64_t       *loads;
    uint32_t       *place_bits;
    recipe_t       *call_recipes;
    uint32_t       *slot_of;
    uint32_t       *slot_places;
    uint32_t        slots;
    uint32_t        defs;
    uint32_t       *order;
    uint32_t       *call_slots;
    uint64_t       *state;
    core_list_t(recipe_ptr_t) parts;
    core_dict_t     call_parts;
    core_dict_t     inputs;
    core_dict_t     input_parts;
    core_list_t(place_part_t) place_parts;
    core_list_t(call_output_t) *call_outputs;
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
                : input.kind == IR_SINPUT_CALL
                ? core_string_eq(have->callee, input.callee)
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

static bool same_input(const ir_sinput_t *a, const ir_sinput_t *b);

static void *
input_key(const ir_sinput_t *input)
{
    uint64_t key = (uint64_t)input->kind + 1;
    switch (input->kind) {
    case IR_SINPUT_VALUE:
    case IR_SINPUT_REFERENT:
        key |= (uint64_t)(input->param + 1) << 8;
        break;
    case IR_SINPUT_PLACE: {
        const void *identity = input->place.kind == IR_SPLACE_HEAP
            ? (input->place.type ? (void *)th_resolve(input->place.type)
                                 : nullptr)
            : (const void *)input->place.global;
        key ^= ((uint64_t)input->place.kind << 4)
            ^ ((uint64_t)(uintptr_t)identity * 0x9e3779b97f4a7c15ull);
        break;
    }
    case IR_SINPUT_CALL:
        key ^= (uint64_t)(uintptr_t)input->callee.data * 0x9e3779b97f4a7c15ull;
        break;
    case IR_SINPUT_UNKNOWN:
        break;
    }
    return (void *)(uintptr_t)(key ? key : 1);
}

static uint32_t
deps_input(deps_t *d, ir_sinput_t input)
{
    void     *key   = input_key(&input);
    uintptr_t known = (uintptr_t)core_dict_get(&d->inputs, key, nullptr);
    if (known && same_input(&d->summary->inputs.data[known - 1], &input))
        return (uint32_t)(known - 1);
    uint32_t index = input_of(d->summary, input);
    if (!known) core_dict_put(&d->inputs, key, (void *)(uintptr_t)(index + 1));
    return index;
}

static uint32_t
entry_input(deps_t *d, uint32_t place)
{
    if (d->place_bits) return d->place_bits[place];
    ir_splace_t mapped;
    if (!portable_place(d->f, ir_place_at(d->places, place), &mapped))
        return UINT32_MAX;
    if (mapped.kind == IR_SPLACE_PARAM)
        return deps_input(d, (ir_sinput_t){
            .kind = IR_SINPUT_REFERENT, .param = mapped.param});
    if (mapped.kind == IR_SPLACE_UNKNOWN)
        return deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    return deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_PLACE,
                                       .place = mapped});
}


static void
recipe_free(recipe_t *r)
{
    core_list_free(r->bits);
    core_list_free(r->rows);
    core_list_free(r->stores);
    core_list_free(r->loads);
    core_list_free(r->parts);
}

static void
recipe_add(core_list_t(uint32_t) *list, uint32_t value)
{
    if (!list->data) *list = core_list_new(uint32_t);
    if (list->len && list->data[list->len - 1] == value) return;
    core_list_push(*list, value);
}

static void
apply_recipe(deps_t *d, const recipe_t *r, uint64_t *into)
{
    for (size_t i = 0; i < r->bits.len; i++) set_bit(d, into, r->bits.data[i]);
    for (size_t i = 0; i < r->rows.len; i++)
        if (merge(d, into, row(d, d->data, r->rows.data[i]))) d->changed = true;
    for (size_t i = 0; i < r->stores.len; i++)
        if (merge(d, into, row(d, d->place_stores, r->stores.data[i])))
            d->changed = true;
    for (size_t i = 0; i < r->loads.len; i++)
        if (merge(d, into, row(d, d->loads, r->loads.data[i])))
            d->changed = true;
    for (size_t i = 0; i < r->parts.len; i++)
        apply_recipe(d, d->parts.data[r->parts.data[i]], into);
}

static uint32_t
new_part(deps_t *d)
{
    core_list_push(d->parts, core_alloc(recipe_t));
    return (uint32_t)(d->parts.len - 1);
}

static void
set_input(deps_t *d, recipe_t *into, ir_sinput_t input)
{
    uint32_t bit = deps_input(d, input);
    if (bit >= d->words * 64)
        bit = deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    recipe_add(&into->bits, bit);
}

static void
place_dependences(deps_t *d, uint32_t place, recipe_t *into)
{
    uint32_t bit = entry_input(d, place);
    if (bit != UINT32_MAX && bit < d->words * 64) recipe_add(&into->bits, bit);
    recipe_add(&into->stores, place);
}

static void
storage_operand_dependences(deps_t *d, const ir_instr_t *call,
                            const ir_value_t *storage, recipe_t *into)
{
    uint32_t index = instr_index(d, call);
    if (index == UINT32_MAX) return;
    uint32_t end = d->access_first[index] + d->access_count[index];
    for (uint32_t a = d->access_first[index]; a < end; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        const uint32_t    *ids    = ir_places_access_places(d->places, access);
        if (access->kind == IR_ACCESS_LOAD && access->direct
            && access->count == 1
            && ir_place_at(d->places, ids[0])->root == storage)
            recipe_add(&into->loads, a);
    }
}

static void
value_dependences(deps_t *d, const ir_instr_t *call, const ir_value_t *value,
                  recipe_t *into)
{
    if (!value) return;
    if (value->kind == IR_VAL_LOCAL || value->kind == IR_VAL_GLOBAL)
        storage_operand_dependences(d, call, value, into);
    if (value->kind == IR_VAL_PARAM)
        for (uint32_t i = 0; i < d->f->n_params; i++)
            if (d->f->params[i] == value)
                set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_VALUE,
                                                 .param = i});
    if (value->kind == IR_VAL_INSTR && value->def) {
        uint32_t index = instr_index(d, value->def);
        if (index != UINT32_MAX) recipe_add(&into->rows, index);
    }
}

static void
argument_dependences(deps_t *d, const ir_instr_t *call, recipe_t *into)
{
    for (uint32_t o = 1; o < call->n_operands; o++) {
        ir_value_t *value = ir_operand_value(&call->operands[o]);
        value_dependences(d, call, value, into);
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument_places(d->places, d->f, value, &ids, &n))
            for (uint32_t p = 0; p < n; p++)
                place_dependences(d, ids[p], into);
    }
}

static bool
opaque_summary(const ir_summary_t *callee)
{
    return !callee->outputs.len && !callee->inputs.len
        && (!callee->function || !callee->function->n_blocks);
}

static bool
conservative_summary(const ir_summary_t *callee)
{
    return opaque_summary(callee)
        && (callee->flags & (IR_SUMMARY_CALLS_UNKNOWN
                             | IR_SUMMARY_CALLS_FINALIZERS
                             | IR_SUMMARY_STARTS_THREADS));
}

static core_string_t
callee_name(deps_t *d, const ir_func_t *target)
{
    bool found = false;
    core_string_t *known = core_dict_get(&d->summaries->names, (void *)target,
                                         &found);
    if (found) return *known;
    core_buffer_t text   = {0};
    core_string_t module = ir_module_name(target->owner);
    core_buffer_append(&text, module.data, module.u8_bytes);
    core_buffer_append(&text, ".", 1);
    core_buffer_append(&text, target->name.data, target->name.u8_bytes);
    core_string_t *name = ir_alloc(d->f->owner->ctx, sizeof(core_string_t));
    *name = ir_strdup(d->f->owner->ctx, core_buffer_string(&text));
    core_buffer_release(&text);
    core_dict_put(&d->summaries->names, (void *)target, name);
    return *name;
}

static void
opaque_dependences(deps_t *d, const ir_instr_t *call, ir_summary_t *callee,
                   recipe_t *into)
{
    argument_dependences(d, call, into);
    if (conservative_summary(callee) || !callee->function)
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    else
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_CALL,
            .callee = callee_name(d, callee->function)});
}

static bool
splace_matches(ir_module_t *module, const ir_splace_t *a,
               const ir_splace_t *b)
{
    if (a->kind == IR_SPLACE_UNKNOWN || b->kind == IR_SPLACE_UNKNOWN)
        return true;
    if (a->kind != b->kind) return false;
    if (a->kind == IR_SPLACE_HEAP)
        return !a->type || !b->type
            || ir_places_types_overlap(module, a->type, b->type);
    return ir_splace_eq(a, b);
}

static uint32_t
place_input_part(deps_t *d, const ir_splace_t *place)
{
    for (size_t i = 0; i < d->place_parts.len; i++)
        if (ir_splace_eq(&d->place_parts.data[i].place, place))
            return d->place_parts.data[i].part;
    uint32_t part = new_part(d);
    for (uint32_t p = 0; p < ir_places_count(d->places); p++) {
        ir_splace_t mapped;
        if (portable_place(d->f, ir_place_at(d->places, p), &mapped)
            && splace_matches(d->f->owner, &mapped, place))
            place_dependences(d, p, d->parts.data[part]);
    }
    core_list_push(d->place_parts, ((place_part_t){*place, part}));
    return part;
}

static void
map_callee_input(deps_t *d, const ir_instr_t *call,
                 const ir_sinput_t *input, recipe_t *into)
{
    ir_value_t *argument = input->param + 1 < call->n_operands
        ? ir_operand_value(&call->operands[input->param + 1]) : nullptr;
    switch (input->kind) {
    case IR_SINPUT_VALUE:
        value_dependences(d, call, argument, into);
        break;
    case IR_SINPUT_REFERENT: {
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument_places(d->places, d->f, argument, &ids, &n))
            for (uint32_t p = 0; p < n; p++) place_dependences(d, ids[p], into);
        else if (argument && addressable(d->f, argument))
            set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
        break;
    }
    case IR_SINPUT_PLACE:
        set_input(d, into, *input);
        recipe_add(&into->parts, place_input_part(d, &input->place));
        break;
    case IR_SINPUT_CALL:
        set_input(d, into, *input);
        break;
    case IR_SINPUT_UNKNOWN:
        set_input(d, into, *input);
        break;
    }
}

static void
map_callee_output(deps_t *d, const ir_instr_t *call, ir_summary_t *callee,
                  const ir_soutput_t *output, recipe_t *into)
{
    core_dict_t *mapped = core_dict_get(&d->input_parts, callee, nullptr);
    if (!mapped) {
        mapped = core_alloc(core_dict_t);
        core_dict_init(mapped, nullptr, nullptr);
        core_dict_put(&d->input_parts, callee, mapped);
    }
    uint64_t call_key = (uint64_t)(instr_index(d, call) + 1) << 32;
    const uint32_t ranges[2][2] = {
        {output->data_first, output->data_count},
        {output->control_first, output->control_count},
    };
    for (int r = 0; r < 2; r++)
        for (uint32_t i = ranges[r][0]; i < ranges[r][0] + ranges[r][1]; i++) {
            uint32_t input = callee->dependences.data[i];
            if (input >= callee->inputs.len) continue;
            void     *key   = (void *)(uintptr_t)(call_key | input);
            uintptr_t known = (uintptr_t)core_dict_get(mapped, key, nullptr);
            if (!known) {
                uint32_t part = new_part(d);
                map_callee_input(d, call, &callee->inputs.data[input],
                                 d->parts.data[part]);
                known = part + 1;
                core_dict_put(mapped, key, (void *)known);
            }
            recipe_add(&into->parts, (uint32_t)(known - 1));
        }
}

static bool
output_writes(deps_t *d, const ir_instr_t *call, const ir_splace_t *written,
              const ir_splace_t *target, const uint32_t *ids, uint32_t n)
{
    if (written->kind == IR_SPLACE_UNKNOWN) return true;
    if (written->kind != IR_SPLACE_PARAM) {
        if (target && splace_matches(d->f->owner, written, target))
            return true;
        for (uint32_t p = 0; p < n; p++) {
            ir_splace_t mapped;
            if (portable_place(d->f, ir_place_at(d->places, ids[p]), &mapped)
                && splace_matches(d->f->owner, written, &mapped))
                return true;
        }
        return false;
    }
    ir_value_t *argument = written->param + 1 < call->n_operands
        ? ir_operand_value(&call->operands[written->param + 1]) : nullptr;
    const uint32_t *argument_ids = nullptr;
    uint32_t        count        = 0;
    if (!argument_places(d->places, d->f, argument, &argument_ids, &count))
        return argument && addressable(d->f, argument);
    for (uint32_t a = 0; a < count; a++) {
        for (uint32_t p = 0; p < n; p++)
            if (argument_ids[a] == ids[p]) return true;
        ir_splace_t mapped;
        if (target && portable_place(d->f, ir_place_at(d->places,
                                                       argument_ids[a]),
                                     &mapped)
            && splace_matches(d->f->owner, &mapped, target))
            return true;
    }
    return false;
}

enum { CALL_OUTPUT_UNKNOWN = -2, CALL_OUTPUT_OPAQUE = -1 };

static void
add_call_output(core_list_t(call_output_t) *entries, ir_splace_t written,
                ir_summary_t *callee, int32_t output, uint32_t owner)
{
    core_list_push(*entries, ((call_output_t){.place = written,
        .callee = callee, .output = output, .owner = owner}));
}

static core_list_t(call_output_t) *
call_output_entries(deps_t *d, const ir_instr_t *call)
{
    core_list_t(call_output_t) *entries =
        &d->call_outputs[instr_index(d, call)];
    if (entries->data) return entries;
    *entries = core_list_new(call_output_t);
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    if (targets.unknown)
        add_call_output(entries, (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                        nullptr, CALL_OUTPUT_UNKNOWN,
                        (uint32_t)entries->len);
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        if (opaque_summary(callee)) {
            uint32_t owner = (uint32_t)entries->len;
            if (conservative_summary(callee))
                add_call_output(entries,
                                (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                                callee, CALL_OUTPUT_OPAQUE, owner);
            for (size_t a = 0; a < callee->accesses.len; a++)
                if (callee->accesses.data[a].flags & IR_SACCESS_WRITE)
                    add_call_output(entries, callee->accesses.data[a].place,
                                    callee, CALL_OUTPUT_OPAQUE, owner);
            continue;
        }
        for (size_t o = 0; o < callee->outputs.len; o++)
            if (callee->outputs.data[o].kind == IR_SOUTPUT_PLACE)
                add_call_output(entries, callee->outputs.data[o].place,
                                callee, (int32_t)o, (uint32_t)entries->len);
    }
    core_list_free(targets.targets);
    return entries;
}

static uint32_t
call_output_part(deps_t *d, const ir_instr_t *call,
                 core_list_t(call_output_t) *entries, size_t e)
{
    call_output_t *owner = &entries->data[entries->data[e].owner];
    if (owner->part) return owner->part - 1;
    uint32_t  part   = new_part(d);
    recipe_t *recipe = d->parts.data[part];
    if (owner->output == CALL_OUTPUT_UNKNOWN) {
        argument_dependences(d, call, recipe);
        set_input(d, recipe, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    }
    else if (owner->output == CALL_OUTPUT_OPAQUE)
        opaque_dependences(d, call, owner->callee, recipe);
    else
        map_callee_output(d, call, owner->callee,
                          &owner->callee->outputs.data[owner->output],
                          recipe);
    owner = &entries->data[entries->data[e].owner];
    owner->part = part + 1;
    return part;
}

static void
call_place_dependences(deps_t *d, const ir_instr_t *call,
                       const ir_splace_t *target, const uint32_t *ids,
                       uint32_t n, recipe_t *into)
{
    core_list_t(call_output_t) *entries = call_output_entries(d, call);
    for (size_t e = 0; e < entries->len; e++)
        if (output_writes(d, call, &entries->data[e].place, target, ids, n))
            recipe_add(&into->parts, call_output_part(d, call, entries, e));
}

static uint32_t
call_place_part(deps_t *d, const ir_instr_t *call, uint32_t place)
{
    uint32_t index = instr_index(d, call);
    void    *key   = (void *)(uintptr_t)((uint64_t)(index + 1)
        * (ir_places_count(d->places) + 1) + place + 1);
    bool     found = false;
    uintptr_t known = (uintptr_t)core_dict_get(&d->call_parts, key, &found);
    if (found) return (uint32_t)known;
    uint32_t part = new_part(d);
    core_dict_put(&d->call_parts, key, (void *)(uintptr_t)part);
    call_place_dependences(d, call, nullptr, &place, 1, d->parts.data[part]);
    return part;
}

static uint64_t *
state_row(deps_t *d, uint32_t def, uint32_t slot)
{
    return d->state + ((size_t)def * d->slots + slot) * d->words;
}

static bool
store_touches(deps_t *d, const ir_access_t *store, uint32_t place)
{
    const uint32_t *ids = ir_places_access_places(d->places, store);
    for (uint32_t i = 0; i < store->count; i++)
        if (ir_places_may_alias(d->places, ids[i], place)) return true;
    return false;
}

static bool
store_covers(deps_t *d, const ir_access_t *store, uint32_t place)
{
    if (!store->whole || store->count != 1
        || ir_places_access_places(d->places, store)[0] != place)
        return false;
    ir_place_kind_t kind = ir_place_at(d->places, place)->kind;
    return kind != IR_PLACE_HEAP && kind != IR_PLACE_UNKNOWN;
}

static bool
private_slot(deps_t *d, uint32_t slot)
{
    const ir_place_t *place = ir_place_at(d->places, d->slot_places[slot]);
    return place->kind == IR_PLACE_LOCAL && !place->exposed;
}

static void
prepare_memory(deps_t *d, uint32_t accesses)
{
    uint32_t places = ir_places_count(d->places);
    d->slot_of     = core_alloc_array(uint32_t, places + 1);
    d->slot_places = core_alloc_array(uint32_t, places + 1);
    for (uint32_t p = 0; p < places; p++) d->slot_of[p] = UINT32_MAX;
    for (uint32_t a = 0; d->memssa && a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind != IR_ACCESS_LOAD
            || ir_places_access_private(d->places, access)
            || ir_memssa_load_def(d->memssa, a) == UINT32_MAX) continue;
        const uint32_t *ids = ir_places_access_places(d->places, access);
        for (uint32_t i = 0; i < access->count; i++)
            if (d->slot_of[ids[i]] == UINT32_MAX) {
                d->slot_places[d->slots] = ids[i];
                d->slot_of[ids[i]] = d->slots++;
            }
    }
    d->defs = d->slots ? ir_memssa_def_count(d->memssa) : 0;
    d->order = core_alloc_array(uint32_t, d->defs + 1);
    uint32_t  blocks = d->f->n_blocks;
    uint32_t *start  = core_alloc_array(uint32_t, blocks + 2);
    uint32_t *rank   = core_alloc_array(uint32_t, d->defs + 1);
    ir_doms_t *doms  = ir_func_doms(d->f);
    for (uint32_t def = 0; def < d->defs; def++) {
        const ir_memdef_t *md = ir_memssa_def(d->memssa, def);
        uint32_t position = md->block && doms
            ? ir_rpo_index(doms, md->block) : UINT32_MAX;
        rank[def] = position < blocks ? position : blocks;
        start[rank[def] + 1]++;
    }
    for (uint32_t b = 0; b < blocks + 1; b++) start[b + 1] += start[b];
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t def = 0; def < d->defs; def++) {
            bool phi = ir_memssa_def(d->memssa, def)->kind == IR_MEMDEF_PHI;
            if (phi == (pass == 0)) d->order[start[rank[def]]++] = def;
        }
    core_free(start);
    core_free(rank);
    d->call_slots = core_alloc_array(uint32_t,
                                     (size_t)d->defs * d->slots + 1);
    for (uint32_t def = 0; def < d->defs; def++) {
        const ir_memdef_t *md = ir_memssa_def(d->memssa, def);
        if (md->kind != IR_MEMDEF_CALL || !md->instr || !is_call(md->instr))
            continue;
        for (uint32_t slot = 0; slot < d->slots; slot++)
            if (!private_slot(d, slot))
                d->call_slots[(size_t)def * d->slots + slot] = 1
                    + call_place_part(d, md->instr, d->slot_places[slot]);
    }
}

static void
memory_states(deps_t *d, uint32_t accesses)
{
    for (uint32_t o = 0; o < d->defs; o++) {
        uint32_t           def  = d->order[o];
        const ir_memdef_t *md   = ir_memssa_def(d->memssa, def);
        uint32_t           prev = ir_memssa_def_prev(d->memssa, def);
        uint32_t           from = md->instr && md->kind == IR_MEMDEF_STORE
            ? instr_index(d, md->instr) : UINT32_MAX;
        for (uint32_t slot = 0; slot < d->slots; slot++) {
            uint64_t *out   = state_row(d, def, slot);
            uint32_t  place = d->slot_places[slot];
            switch (md->kind) {
            case IR_MEMDEF_ENTRY: {
                uint32_t bit = entry_input(d, place);
                if (bit != UINT32_MAX && bit < d->words * 64)
                    set_bit(d, out, bit);
                break;
            }
            case IR_MEMDEF_PHI:
                for (uint32_t i = 0; i < ir_memssa_phi_count(d->memssa, def);
                     i++) {
                    uint32_t in = ir_memssa_phi_incoming(d->memssa, def, i);
                    if (in != UINT32_MAX
                        && merge(d, out, state_row(d, in, slot)))
                        d->changed = true;
                }
                break;
            case IR_MEMDEF_STORE:
                if (!store_covers(d, md->access, place) && prev != UINT32_MAX
                    && merge(d, out, state_row(d, prev, slot)))
                    d->changed = true;
                if (from != UINT32_MAX && store_touches(d, md->access, place)
                    && merge(d, out, row(d, d->data, from)))
                    d->changed = true;
                break;
            case IR_MEMDEF_CALL: {
                if (prev != UINT32_MAX && merge(d, out, state_row(d, prev, slot)))
                    d->changed = true;
                uint32_t part = d->call_slots[(size_t)def * d->slots + slot];
                if (part) apply_recipe(d, d->parts.data[part - 1], out);
                break;
            }
            case IR_MEMDEF_ELEMENT_PHI:
                break;
            }
        }
    }
    for (uint32_t a = 0; a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind != IR_ACCESS_LOAD
            || ir_places_access_private(d->places, access)) continue;
        uint64_t *into = row(d, d->loads, a);
        uint32_t  def  = d->memssa ? ir_memssa_load_def(d->memssa, a)
                                   : UINT32_MAX;
        if (def == UINT32_MAX) {
            set_bit(d, into, deps_input(d,
                                        (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN}));
            continue;
        }
        const uint32_t *ids = ir_places_access_places(d->places, access);
        for (uint32_t i = 0; i < access->count; i++)
            if (merge(d, into, state_row(d, def, d->slot_of[ids[i]])))
                d->changed = true;
    }
}

typedef struct {
    uint32_t  generation;
    uint32_t *bits;
    uint32_t *rows;
    uint32_t *stores;
    uint32_t *loads;
    uint32_t *parts;
} stamps_t;

static void
flatten_list(const core_list_t(uint32_t) *from, core_list_t(uint32_t) *into,
             uint32_t *stamp, uint32_t generation)
{
    for (size_t i = 0; i < from->len; i++) {
        uint32_t value = from->data[i];
        if (stamp[value] == generation) continue;
        stamp[value] = generation;
        if (!into->data) *into = core_list_new(uint32_t);
        core_list_push(*into, value);
    }
}

static void
flatten_into(deps_t *d, const recipe_t *from, recipe_t *into, stamps_t *s)
{
    flatten_list(&from->bits, &into->bits, s->bits, s->generation);
    flatten_list(&from->rows, &into->rows, s->rows, s->generation);
    flatten_list(&from->stores, &into->stores, s->stores, s->generation);
    flatten_list(&from->loads, &into->loads, s->loads, s->generation);
    for (size_t i = 0; i < from->parts.len; i++) {
        uint32_t part = from->parts.data[i];
        if (s->parts[part] == s->generation) continue;
        s->parts[part] = s->generation;
        flatten_into(d, d->parts.data[part], into, s);
    }
}

static void
apply_unique(deps_t *d, const recipe_t *r, uint64_t *into, stamps_t *s)
{
    for (size_t i = 0; i < r->bits.len; i++) set_bit(d, into, r->bits.data[i]);
    for (size_t i = 0; i < r->rows.len; i++) {
        uint32_t index = r->rows.data[i];
        if (s->rows[index] == s->generation) continue;
        s->rows[index] = s->generation;
        merge(d, into, row(d, d->data, index));
    }
    for (size_t i = 0; i < r->stores.len; i++) {
        uint32_t place = r->stores.data[i];
        if (s->stores[place] == s->generation) continue;
        s->stores[place] = s->generation;
        merge(d, into, row(d, d->place_stores, place));
    }
    for (size_t i = 0; i < r->loads.len; i++) {
        uint32_t access = r->loads.data[i];
        if (s->loads[access] == s->generation) continue;
        s->loads[access] = s->generation;
        merge(d, into, row(d, d->loads, access));
    }
    for (size_t i = 0; i < r->parts.len; i++) {
        uint32_t part = r->parts.data[i];
        if (s->parts[part] == s->generation) continue;
        s->parts[part] = s->generation;
        apply_unique(d, d->parts.data[part], into, s);
    }
}

static stamps_t
new_stamps(deps_t *d)
{
    return (stamps_t){
        .bits   = core_alloc_array(uint32_t, d->summary->inputs.len + 1),
        .rows   = core_alloc_array(uint32_t, d->instrs + 1),
        .stores = core_alloc_array(uint32_t, ir_places_count(d->places) + 1),
        .loads  = core_alloc_array(uint32_t,
                                   ir_places_access_count(d->places) + 1),
        .parts  = core_alloc_array(uint32_t, d->parts.len + 1),
    };
}

static void
free_stamps(stamps_t *s)
{
    core_free(s->bits);
    core_free(s->rows);
    core_free(s->stores);
    core_free(s->loads);
    core_free(s->parts);
}

static void
flatten_recipe(deps_t *d, recipe_t *recipe, stamps_t *s)
{
    if (!recipe->parts.len) return;
    s->generation++;
    recipe_t flat = {.built = true};
    flatten_into(d, recipe, &flat, s);
    recipe_free(recipe);
    *recipe = flat;
}

static void
call_dependences(deps_t *d, ir_instr_t *call, recipe_t *into)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    if (targets.unknown) {
        argument_dependences(d, call, into);
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    }
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        if (opaque_summary(callee)) {
            opaque_dependences(d, call, callee, into);
            continue;
        }
        for (size_t o = 0; o < callee->outputs.len; o++) {
            const ir_soutput_t *output = &callee->outputs.data[o];
            if (output->kind == IR_SOUTPUT_RESULT
                || output->kind == IR_SOUTPUT_FAILURE)
                map_callee_output(d, call, callee, output, into);
        }
    }
    core_list_free(targets.targets);
}

static void
callee_inputs(deps_t *d, const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        for (size_t i = 0; i < callee->inputs.len; i++) {
            const ir_sinput_t *input = &callee->inputs.data[i];
            if (input->kind == IR_SINPUT_PLACE
                || input->kind == IR_SINPUT_CALL)
                deps_input(d, *input);
        }
        if (opaque_summary(callee) && !conservative_summary(callee)
            && callee->function)
            deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_CALL,
                .callee = callee_name(d, callee->function)});
    }
    core_list_free(targets.targets);
}

static void
instruction_dependences(deps_t *d, ir_instr_t *in, uint32_t index)
{
    uint64_t *into = row(d, d->data, index);
    if (is_call(in)) {
        recipe_t *recipe = &d->call_recipes[index];
        if (!recipe->built) {
            recipe->built = true;
            call_dependences(d, in, recipe);
        }
        apply_recipe(d, recipe, into);
        return;
    }
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
                set_bit(d, into, deps_input(d, (ir_sinput_t){
                    .kind = IR_SINPUT_VALUE, .param = i}));
    }
    uint32_t end = d->access_first[index] + d->access_count[index];
    for (uint32_t a = d->access_first[index]; a < end; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind == IR_ACCESS_LOAD
            && merge(d, into, row(d, d->loads, a)))
            d->changed = true;
    }
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
same_range(const ir_summary_t *s, uint32_t a, uint32_t b, uint32_t count)
{
    return !count || !memcmp(s->dependences.data + a, s->dependences.data + b,
                             count * sizeof(uint32_t));
}

static uint32_t
intern_range(ir_summary_t *s, uint32_t first, uint32_t count)
{
    for (size_t o = 0; o < s->outputs.len; o++) {
        const ir_soutput_t *have = &s->outputs.data[o];
        uint32_t found = have->data_count == count
                && same_range(s, have->data_first, first, count)
            ? have->data_first
            : have->control_count == count
                && same_range(s, have->control_first, first, count)
            ? have->control_first : UINT32_MAX;
        if (found != UINT32_MAX) {
            s->dependences.len = first;
            return found;
        }
    }
    return first;
}

static void
add_output(deps_t *d, ir_soutput_kind_t kind, ir_splace_t place,
           const uint64_t *data, const uint64_t *control)
{
    ir_summary_t *s = d->summary;
    ir_soutput_t  out = {.kind = kind, .place = place};
    uint32_t      bits = s->inputs.len < d->words * 64
        ? (uint32_t)s->inputs.len : d->words * 64;
    out.data_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < bits; bit++)
        if (data[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.data_count = (uint32_t)s->dependences.len - out.data_first;
    out.data_first = intern_range(s, out.data_first, out.data_count);
    out.control_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < bits; bit++)
        if (control[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.control_count = (uint32_t)s->dependences.len - out.control_first;
    out.control_first = intern_range(s, out.control_first, out.control_count);
    core_list_push(s->outputs, out);
}

static void
compact_inputs(ir_summary_t *summary)
{
    size_t    count = summary->inputs.len;
    uint32_t *remap = core_alloc_array(uint32_t, count + 1);
    for (size_t i = 0; i < count; i++) remap[i] = UINT32_MAX;
    for (size_t i = 0; i < summary->dependences.len; i++)
        remap[summary->dependences.data[i]] = 0;
    uint32_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (remap[i] != UINT32_MAX) {
            summary->inputs.data[kept] = summary->inputs.data[i];
            remap[i] = kept++;
        }
    summary->inputs.len = kept;
    for (size_t i = 0; i < summary->dependences.len; i++)
        summary->dependences.data[i] = remap[summary->dependences.data[i]];
    core_free(remap);
}

static void
collect_outputs(deps_t *d)
{
    uint64_t *data    = core_alloc_array(uint64_t, d->words);
    uint64_t *control = core_alloc_array(uint64_t, d->words);
    size_t     written  = d->summary->accesses.len;
    recipe_t  *gathered = core_alloc_array(recipe_t, written + 1);
    core_list_t(uint32_t) *writers =
        core_alloc_array(core_list_t(uint32_t), written + 1);
    for (uint32_t b = 0; b < d->f->n_blocks; b++)
        for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = d->f->blocks[b]->instrs[i];
            if (!is_call(in)) continue;
            core_list_t(call_output_t) *entries = call_output_entries(d, in);
            bool writes = false;
            for (size_t e = 0; e < entries->len; e++)
                if (entries->data[e].place.kind == IR_SPLACE_UNKNOWN) {
                    recipe_add(&gathered[written].parts,
                               call_output_part(d, in, entries, e));
                    writes = true;
                }
            if (writes) recipe_add(&writers[written], instr_index(d, in));
        }
    for (size_t a = 0; a < written; a++) {
        const ir_saccess_t *access = &d->summary->accesses.data[a];
        if (!(access->flags & IR_SACCESS_WRITE)) continue;
        for (uint32_t b = 0; b < d->f->n_blocks; b++)
            for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = d->f->blocks[b]->instrs[i];
                if (!is_call(in)) continue;
                core_list_t(call_output_t) *entries =
                    call_output_entries(d, in);
                bool writes = false;
                for (size_t e = 0; e < entries->len; e++)
                    if (entries->data[e].place.kind != IR_SPLACE_UNKNOWN
                        && output_writes(d, in, &entries->data[e].place,
                                         &access->place, nullptr, 0)) {
                        recipe_add(&gathered[a].parts,
                                   call_output_part(d, in, entries, e));
                        writes = true;
                    }
                if (writes) recipe_add(&writers[a], instr_index(d, in));
            }
    }
    stamps_t stamps = new_stamps(d);
    uint64_t *shared_data    = core_alloc_array(uint64_t, d->words);
    uint64_t *shared_control = core_alloc_array(uint64_t, d->words);
    uint64_t *call_data      = core_alloc_array(uint64_t,
                                                (written + 1) * d->words);
    bool     *computed       = core_alloc_array(bool, written + 1);
    stamps.generation++;
    apply_unique(d, &gathered[written], shared_data, &stamps);
    for (size_t c = 0; c < writers[written].len; c++)
        merge(d, shared_control, row(d, d->control, writers[written].data[c]));
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
        size_t same = a;
        for (size_t b = 0; same == a && b < a; b++)
            if (computed[b] && gathered[b].parts.len == gathered[a].parts.len
                && (!gathered[a].parts.len
                    || !memcmp(gathered[b].parts.data, gathered[a].parts.data,
                               gathered[a].parts.len * sizeof(uint32_t))))
                same = b;
        uint64_t *calls = call_data + (size_t)same * d->words;
        if (same == a) {
            stamps.generation++;
            apply_unique(d, &gathered[a], calls, &stamps);
            computed[a] = true;
        }
        merge(d, data, calls);
        merge(d, data, shared_data);
        merge(d, control, shared_control);
        for (size_t c = 0; c < writers[a].len; c++)
            merge(d, control, row(d, d->control, writers[a].data[c]));
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
    for (size_t a = 0; a <= written; a++) {
        recipe_free(&gathered[a]);
        core_list_free(writers[a]);
    }
    core_free(shared_data);
    core_free(shared_control);
    core_free(call_data);
    core_free(computed);
    core_free(gathered);
    core_free(writers);
    core_free(data);
    core_free(control);
    free_stamps(&stamps);
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
    core_dict_init(&d.inputs, nullptr, nullptr);
    core_dict_init(&d.input_parts, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            core_dict_put(&d.index, in, (void *)(uintptr_t)(++d.instrs));
            if (is_call(in)) callee_inputs(&d, in);
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
    uint32_t *place_bits = core_alloc_array(uint32_t,
                                            ir_places_count(d.places) + 1);
    for (uint32_t p = 0; p < ir_places_count(d.places); p++)
        place_bits[p] = entry_input(&d, p);
    d.place_bits = place_bits;
    input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    d.parts        = core_list_new(recipe_ptr_t);
    d.place_parts  = core_list_new(place_part_t);
    d.call_outputs = core_alloc_array(core_list_t(call_output_t), d.instrs + 1);
    core_dict_init(&d.call_parts, nullptr, nullptr);
    uint32_t accesses = ir_places_access_count(d.places);
    d.call_recipes = core_alloc_array(recipe_t, d.instrs + 1);
    d.words        = UINT32_MAX / 64;
    prepare_memory(&d, accesses);
    for (uint32_t b = 0, index = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++, index++)
            if (is_call(f->blocks[b]->instrs[i])) {
                d.call_recipes[index].built = true;
                call_dependences(&d, f->blocks[b]->instrs[i],
                                 &d.call_recipes[index]);
            }
    stamps_t stamps = new_stamps(&d);
    for (size_t i = 0; i < (size_t)d.defs * d.slots; i++)
        if (d.call_slots[i])
            flatten_recipe(&d, d.parts.data[d.call_slots[i] - 1], &stamps);
    for (uint32_t i = 0; i < d.instrs; i++)
        flatten_recipe(&d, &d.call_recipes[i], &stamps);
    free_stamps(&stamps);
    d.words   = (uint32_t)((summary->inputs.len + 63) / 64 + 1);
    d.data    = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.control = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.loads   = core_alloc_array(uint64_t, (size_t)(accesses + 1) * d.words);
    d.state   = core_alloc_array(uint64_t,
        ((size_t)d.defs * d.slots + 1) * d.words);
    d.place_stores = core_alloc_array(uint64_t,
        (size_t)(ir_places_count(d.places) + 1) * d.words);
    for (uint32_t round = 0; round < 256; round++) {
        d.changed = false;
        memory_states(&d, accesses);
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
    if (d.changed) {
        uint32_t unknown = input_of(summary,
                                    (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
        for (uint32_t index = 0; index < d.instrs; index++)
            set_bit(&d, row(&d, d.data, index), unknown);
    }
    collect_outputs(&d);
    compact_inputs(summary);
    for (uint32_t i = 0; i < d.instrs; i++) recipe_free(&d.call_recipes[i]);
    core_free(d.slot_of);
    core_free(d.slot_places);
    core_free(d.order);
    core_free(d.call_slots);
    core_free(d.state);
    for (size_t i = 0; i < d.parts.len; i++) {
        recipe_free(d.parts.data[i]);
        core_free(d.parts.data[i]);
    }
    core_list_free(d.parts);
    core_list_free(d.place_parts);
    for (uint32_t i = 0; i < d.instrs; i++) core_list_free(d.call_outputs[i]);
    core_free(d.call_outputs);
    core_dict_free(&d.call_parts);
    core_dict_free(&d.inputs);
    core_dict_foreach(&d.input_parts, item) {
        core_dict_free(item->value);
        core_free(item->value);
    }
    core_dict_free(&d.input_parts);
    core_free(place_bits);
    core_free(d.call_recipes);
    core_free(d.loads);
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

static bool
same_input(const ir_sinput_t *a, const ir_sinput_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case IR_SINPUT_VALUE:
    case IR_SINPUT_REFERENT:
        return a->param == b->param;
    case IR_SINPUT_CALL:
        return core_string_eq(a->callee, b->callee);
    case IR_SINPUT_PLACE:
        return ir_splace_eq(&a->place, &b->place);
    case IR_SINPUT_UNKNOWN:
        return true;
    }
    return false;
}

static int
compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return a < b ? -1 : a > b;
}

static bool
same_dependence_list(const ir_summary_t *a, const uint32_t *remap,
                     uint32_t a_first, uint32_t a_count,
                     const ir_summary_t *b, uint32_t b_first,
                     uint32_t b_count)
{
    if (a_count != b_count) return false;
    if (!a_count) return true;
    uint32_t *mapped = core_alloc_array(uint32_t, a_count);
    uint32_t *wanted = core_alloc_array(uint32_t, b_count);
    bool same = true;
    for (uint32_t i = 0; same && i < a_count; i++) {
        mapped[i] = remap[a->dependences.data[a_first + i]];
        same = mapped[i] != UINT32_MAX;
    }
    if (same) {
        memcpy(wanted, b->dependences.data + b_first,
               b_count * sizeof(uint32_t));
        qsort(mapped, a_count, sizeof(uint32_t), compare_u32);
        qsort(wanted, b_count, sizeof(uint32_t), compare_u32);
        same = !memcmp(mapped, wanted, a_count * sizeof(uint32_t));
    }
    core_free(mapped);
    core_free(wanted);
    return same;
}

static bool
same_dependences(const ir_summary_t *a, const ir_summary_t *b)
{
    if (a->outputs.len != b->outputs.len
        || a->inputs.len != b->inputs.len) return false;
    core_dict_t keys;
    core_dict_init(&keys, nullptr, nullptr);
    for (size_t j = 0; j < b->inputs.len; j++)
        core_dict_add(&keys, input_key(&b->inputs.data[j]),
                      (void *)(uintptr_t)(j + 1));
    uint32_t *remap = core_alloc_array(uint32_t, a->inputs.len + 1);
    bool same = true;
    for (size_t i = 0; same && i < a->inputs.len; i++) {
        uintptr_t j = (uintptr_t)core_dict_get(&keys,
            input_key(&a->inputs.data[i]), nullptr);
        if (!j || !same_input(&a->inputs.data[i], &b->inputs.data[j - 1])) {
            j = 0;
            for (size_t k = 0; !j && k < b->inputs.len; k++)
                if (same_input(&a->inputs.data[i], &b->inputs.data[k]))
                    j = k + 1;
        }
        remap[i] = j ? (uint32_t)(j - 1) : UINT32_MAX;
        same = j != 0;
    }
    for (size_t i = 0; same && i < a->outputs.len; i++) {
        const ir_soutput_t *left  = &a->outputs.data[i];
        bool                found = false;
        for (size_t j = 0; !found && j < b->outputs.len; j++) {
            const ir_soutput_t *right = &b->outputs.data[j];
            found = left->kind == right->kind
                && ir_splace_eq(&left->place, &right->place)
                && same_dependence_list(a, remap, left->data_first,
                                        left->data_count, b,
                                        right->data_first, right->data_count)
                && same_dependence_list(a, remap, left->control_first,
                                        left->control_count, b,
                                        right->control_first,
                                        right->control_count);
        }
        same = found;
    }
    core_free(remap);
    core_dict_free(&keys);
    return same;
}

static bool
same_escapes(const ir_summary_t *a, const ir_summary_t *b)
{
    if (a->escapes.len != b->escapes.len
        || a->returned.kind != b->returned.kind
        || a->returned.param != b->returned.param
        || a->returned.global != b->returned.global) return false;
    for (size_t i = 0; i < a->escapes.len; i++)
        if (a->escapes.data[i] != b->escapes.data[i]) return false;
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

static uint8_t
divergence_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    return is_call(in) && ir_summary_call_diverges(context, in)
        ? 0 : IR_PATH_UNPASSED;
}

typedef struct {
    ir_summaries_t   *summaries;
    ir_func_t *const *members;
    size_t            count;
} recursion_t;

static uint8_t
recursion_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    recursion_t *recursion = context;
    if (!is_call(in)) return IR_PATH_UNPASSED;
    call_targets_t targets;
    resolve_targets(recursion->summaries, in, &targets);
    bool member = false;
    for (size_t t = 0; t < targets.targets.len; t++)
        for (size_t m = 0; m < recursion->count; m++)
            member |= targets.targets.data[t] == recursion->members[m];
    core_list_free(targets.targets);
    return member || !ir_summary_call_diverges(recursion->summaries, in)
        ? IR_PATH_UNPASSED : 0;
}

static void
summarize_divergence(ir_summaries_t *summaries, ir_summary_t *summary,
                     ir_func_t *f)
{
    summary->flags &= ~(uint32_t)(IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN);
    size_t          scope = ir_analysis_scope_begin();
    ir_path_exits_t exits;
    if (ir_paths_flow(f, IR_PATH_UNPASSED, divergence_step, summaries,
                      &exits)
        && !exits.returned && !exits.failed)
        summary->flags |= IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN;
    ir_analysis_scope_end(scope);
}

uint32_t
ir_summary_field_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->fields.len : 0;
}

const ir_sfield_t *
ir_summary_field_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->fields.len
        ? &summary->fields.data[index] : nullptr;
}

static th_type *
record_key(const ir_func_t *f, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || th_kind_of(type) != TH_NAMED) return nullptr;
    th_type *definition = ir_type_definition(f->owner, type);
    definition = definition ? th_resolve(definition) : nullptr;
    return definition && th_kind_of(definition) == TH_RECORD ? type : nullptr;
}

static th_type *
record_or_referent(const ir_func_t *f, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    th_type *record = record_key(f, type);
    if (record || !type || th_kind_of(type) != TH_INDIRECTION) return record;
    return record_key(f, th_ind_referent(type));
}

static void
add_field(ir_summary_t *summary, th_type *record, core_string_t name,
          uint8_t flags)
{
    for (size_t i = 0; i < summary->fields.len; i++) {
        ir_sfield_t *field = &summary->fields.data[i];
        if (field->record == record && core_string_eq(field->name, name)) {
            field->flags |= flags;
            return;
        }
    }
    core_list_push(summary->fields,
                   ((ir_sfield_t){.record = record, .name = name,
                                  .flags = flags}));
}

static uint8_t
field_pointer_use(const ir_instr_t *use, uint32_t operand)
{
    switch (use->op) {
    case IR_OP_LOAD:
        return IR_SFIELD_READ;
    case IR_OP_STORE:
        return operand == 0 ? IR_SFIELD_WRITE
                            : IR_SFIELD_READ | IR_SFIELD_WRITE;
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
        return operand == 0 ? IR_SFIELD_WRITE : IR_SFIELD_READ;
    case IR_OP_MEMSET:
        return IR_SFIELD_WRITE;
    default:
        return IR_SFIELD_READ | IR_SFIELD_WRITE;
    }
}

static bool
opaque_use(ir_summaries_t *summaries, const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_BOX:
    case IR_OP_BOX_RAW:
    case IR_OP_BITCAST:
    case IR_OP_MEMCMP:
    case IR_OP_CALLBACK_CREATE:
        return true;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE: {
        call_targets_t targets;
        resolve_targets(summaries, in, &targets);
        bool opaque = targets.unknown;
        for (size_t t = 0; !opaque && t < targets.targets.len; t++)
            opaque = !targets.targets.data[t]->n_blocks;
        core_list_free(targets.targets);
        return opaque;
    }
    default:
        return false;
    }
}

static bool
generic_parameter(const ir_func_t *target, uint32_t index)
{
    th_type *type = index < target->n_params && target->params[index]->type
        ? th_resolve(target->params[index]->type) : nullptr;
    if (type && th_kind_of(type) == TH_INDIRECTION)
        type = th_resolve(th_ind_referent(type));
    return type && th_kind_of(type) == TH_VAR;
}

static bool
generic_argument(ir_summaries_t *summaries, const ir_instr_t *in,
                 uint32_t operand)
{
    if (in->op != IR_OP_CALL || operand == 0) return false;
    call_targets_t targets;
    resolve_targets(summaries, in, &targets);
    bool generic = false;
    for (size_t t = 0; !generic && t < targets.targets.len; t++)
        generic = generic_parameter(targets.targets.data[t], operand - 1);
    core_list_free(targets.targets);
    return generic;
}

static void
summarize_fields(ir_summaries_t *summaries, ir_summary_t *summary,
                 ir_func_t *f)
{
    core_dict_t pointers;
    core_dict_init(&pointers, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->n_operands < 2 || in->operands[1].kind != IR_OPD_NAME)
                continue;
            const ir_value_t *base = ir_operand_value(&in->operands[0]);
            if (in->op == IR_OP_FIELD && in->dest) {
                th_type *record = base && base->type
                    ? record_key(f, th_ind_referent(th_resolve(base->type)))
                    : nullptr;
                if (record) core_dict_put(&pointers, in->dest, in);
            }
            else if (in->op == IR_OP_EXTRACT || in->op == IR_OP_INSERT) {
                th_type *record = base ? record_key(f, base->type) : nullptr;
                if (record)
                    add_field(summary, record, in->operands[1].name,
                              in->op == IR_OP_EXTRACT ? IR_SFIELD_READ
                                                      : IR_SFIELD_WRITE);
            }
        }
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in     = f->blocks[b]->instrs[i];
            bool        opaque = opaque_use(summaries, in);
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_value(&in->operands[o]);
                if (!value) continue;
                bool        found = false;
                ir_instr_t *field = core_dict_get(&pointers, value, &found);
                if (found) {
                    const ir_value_t *base =
                        ir_operand_value(&field->operands[0]);
                    add_field(summary,
                              record_key(f, th_ind_referent(
                                  th_resolve(base->type))),
                              field->operands[1].name,
                              field_pointer_use(in, o));
                }
                th_type *record = opaque || generic_argument(summaries, in, o)
                    ? record_or_referent(f, value) : nullptr;
                if (record)
                    add_field(summary, record, (core_string_t){0},
                              IR_SFIELD_WHOLE);
            }
        }
    core_dict_free(&pointers);
}

static void
summarize_flow(ir_summaries_t *summaries, ir_summary_t *next,
               const ir_summary_t *previous, ir_func_t *f)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    ir_sflow_t flow = {0};
    if (!hooks || !hooks->flow
        || !hooks->flow(hooks->context, summaries, f, &previous->flow,
                        &flow)) {
        ir_sflow_release(&flow);
        flow = ir_sflow_opaque();
    }
    ir_sflow_release(&next->flow);
    next->flow = flow;
}

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
    bool changed = false;
    for (uint32_t round = 0; round < 64; round++) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *next    = ir_summary_new(nullptr, members[i]);
            if (summaries->escapes_only) {
                next->flags = summary->flags;
                for (size_t a = 0; a < summary->accesses.len; a++)
                    core_list_push(next->accesses, summary->accesses.data[a]);
                summarize_escapes(summaries, next, members[i]);
                summarize_divergence(summaries, next, members[i]);
            }
            else summarize_function(summaries, next, members[i]);
            summarize_flow(summaries, next, summary, members[i]);
            if (cyclic
                && (!same_summary(next, &summary->accesses, summary->flags)
                    || next->traps != summary->traps
                    || !same_escapes(next, summary)
                    || !same_dependences(next, summary)
                    || !ir_sflow_eq(&next->flow, &summary->flow)))
                changed = true;
            ir_summary_t previous = *summary;
            *summary = *next;
            *next    = previous;
            summary_free(next);
        }
        if (!cyclic || !changed) {
            changed = false;
            break;
        }
    }
    for (size_t i = 0; changed && i < count; i++)
        core_dict_put(&summaries->by_function, members[i],
                      ir_summary_conservative(summaries, members[i]));
    for (size_t i = 0; !summaries->escapes_only && i < count; i++)
        summarize_fields(summaries, ir_summary_of(summaries, members[i]),
                         members[i]);
    for (size_t i = 0; cyclic && !summaries->escapes_only && i < count;
         i++) {
        ir_summary_t *summary = ir_summary_of(summaries, members[i]);
        if (!(summary->flags & IR_SUMMARY_DIVERGES)) continue;
        recursion_t     recursion = {.summaries = summaries,
                                     .members = members, .count = count};
        size_t          scope = ir_analysis_scope_begin();
        ir_path_exits_t exits;
        if (ir_paths_flow(members[i], IR_PATH_UNPASSED, recursion_step,
                          &recursion, &exits)
            && (exits.returned || exits.failed))
            summary->flags |= IR_SUMMARY_UNBOUNDED_RECURSION;
        ir_analysis_scope_end(scope);
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

static void
collect_reinterpretations(ir_summaries_t *summaries,
                          ir_func_t *const *functions, size_t count)
{
    ir_ctx_t *ctx = count ? functions[0]->owner->ctx : nullptr;
    if (!ctx) return;
    ir_reinterpretations_open(ctx);
    core_list_t(summary_func_ptr_t) pending = core_list_new(summary_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) core_list_push(pending, functions[i]);
    while (pending.len) {
        ir_func_t *f = pending.data[--pending.len];
        if (!f || !core_dict_add(&seen, f, f)) continue;
        if (!f->n_blocks) {
            if (target_summary(summaries, f)->flags & IR_SUMMARY_REINTERPRETS)
                ir_reinterpretations_note_unseen(ctx);
            continue;
        }
        ir_reinterpretations_note_function(ctx, f);
        for (uint32_t b = 0; b < f->n_blocks; b++)
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *call = f->blocks[b]->instrs[i];
                if (call->op != IR_OP_CALL
                    && call->op != IR_OP_CALLBACK_INVOKE) continue;
                call_targets_t targets;
                resolve_targets(summaries, call, &targets);
                for (size_t t = 0; t < targets.targets.len; t++)
                    core_list_push(pending, targets.targets.data[t]);
                core_list_free(targets.targets);
            }
    }
    core_dict_free(&seen);
    core_list_free(pending);
    ir_reinterpretations_seal(ctx);
}

static ir_summaries_t *
compute_summaries(ir_func_t *const *functions, size_t count,
                  const ir_summary_hooks_t *hooks, bool escapes_only)
{
    ir_summaries_t *summaries = core_alloc(ir_summaries_t);
    summaries->hooks = hooks;
    summaries->escapes_only = escapes_only;
    summaries->owned = core_list_new(summary_ptr_t);
    core_dict_init(&summaries->by_function, nullptr, nullptr);
    core_dict_init(&summaries->names, nullptr, nullptr);
    collect_reinterpretations(summaries, functions, count);
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

ir_summaries_t *
ir_summaries_compute(ir_func_t *const *functions, size_t count,
                     const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, false);
}

ir_summaries_t *
ir_escapes_compute(ir_func_t *const *functions, size_t count,
                   const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, true);
}

void
ir_summaries_free(ir_summaries_t *summaries)
{
    if (!summaries) return;
    if (ir_analysis_summaries() == summaries) ir_analysis_bind_summaries(nullptr);
    for (size_t i = 0; i < summaries->owned.len; i++)
        summary_free(summaries->owned.data[i]);
    core_list_free(summaries->owned);
    core_dict_free(&summaries->by_function);
    core_dict_free(&summaries->names);
    core_free(summaries);
}

enum { SUMMARY_CODEC_VERSION = 1 };

static void
put_u8(core_buffer_t *out, uint8_t value)
{
    core_buffer_append(out, (const char *)&value, 1);
}

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static void
put_text(core_buffer_t *out, core_string_t text)
{
    put_u32(out, text.data ? (uint32_t)text.u8_bytes : 0);
    if (text.data && text.u8_bytes)
        core_buffer_append(out, text.data, text.u8_bytes);
}

static void
put_global(core_buffer_t *out, const ir_global_t *global)
{
    const ir_global_t *identity = global ? ir_global_identity(global)
                                         : nullptr;
    put_text(out, identity && identity->owner
        ? ir_module_name(identity->owner) : (core_string_t){0});
    put_text(out, identity ? identity->name : (core_string_t){0});
}

static void
put_u64(core_buffer_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)value);
    put_u32(out, (uint32_t)(value >> 32));
}

static void
put_flow(core_buffer_t *out, const ir_sflow_t *flow)
{
    for (int kind = 0; kind < IR_SFLOW_KINDS; kind++) {
        put_u64(out, flow->writes[kind]);
        put_u64(out, flow->escapes[kind]);
        put_u64(out, flow->leaks[kind]);
    }
    put_u64(out, flow->receives);
    put_u8(out, (uint8_t)(flow->receives_fresh
        | flow->receives_nonlocal << 1 | flow->writes_nonlocal << 2
        | flow->opaque << 3));
    put_u32(out, flow->n_carried);
    for (uint32_t s = 0; s < flow->n_carried; s++) {
        const ir_scarried_t *carried = &flow->carried[s];
        put_u64(out, carried->from[IR_SFLOW_SELF]);
        put_u64(out, carried->from[IR_SFLOW_CONTENTS]);
        put_u8(out, (uint8_t)(carried->nonlocal | carried->fresh << 1
                              | carried->shared << 2));
    }
}

static void
put_place_type(core_buffer_t *out, th_ctx *types, const ir_splace_t *place)
{
    bool named = place->kind == IR_SPLACE_GLOBAL
        || place->kind == IR_SPLACE_THREAD;
    put_text(out, !named && types && place->type
        ? th_print(types, place->type) : (core_string_t){0});
}

static void
put_splace(core_buffer_t *out, th_ctx *types, const ir_splace_t *place)
{
    put_u8(out, place->kind);
    put_u32(out, place->param);
    put_global(out, place->kind == IR_SPLACE_GLOBAL
            || place->kind == IR_SPLACE_THREAD ? place->global : nullptr);
    put_place_type(out, types, place);
}

bool
ir_summary_encode(const ir_summary_t *summary, core_buffer_t *out)
{
    if (!summary || !out) return false;
    th_ctx *types = summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr;
    core_buffer_append(out, "IRSM", 4);
    put_u8(out, SUMMARY_CODEC_VERSION);
    put_u32(out, summary->flags);
    put_u8(out, summary->all_escape);
    put_u8(out, summary->traps);
    put_u32(out, (uint32_t)summary->trap_sites.len);
    for (size_t i = 0; i < summary->trap_sites.len; i++) {
        const ir_strap_site_t *site = &summary->trap_sites.data[i];
        put_u8(out, site->kind);
        put_u32(out, site->line);
        put_u32(out, site->column);
    }
    put_u32(out, (uint32_t)summary->escapes.len);
    for (size_t i = 0; i < summary->escapes.len; i++)
        put_u8(out, summary->escapes.data[i]);
    put_u8(out, summary->returned.kind);
    put_u32(out, summary->returned.param);
    put_global(out, summary->returned.kind == IR_SRETURN_GLOBAL
        ? summary->returned.global : nullptr);
    put_u32(out, (uint32_t)summary->accesses.len);
    for (size_t i = 0; i < summary->accesses.len; i++) {
        const ir_saccess_t *access = &summary->accesses.data[i];
        put_u8(out, access->place.kind);
        put_u8(out, access->flags);
        put_u32(out, access->place.param);
        put_global(out, access->place.kind == IR_SPLACE_GLOBAL
                || access->place.kind == IR_SPLACE_THREAD
            ? access->place.global : nullptr);
        put_place_type(out, types, &access->place);
    }
    put_flow(out, &summary->flow);
    put_u32(out, (uint32_t)summary->inputs.len);
    for (size_t i = 0; i < summary->inputs.len; i++) {
        const ir_sinput_t *input = &summary->inputs.data[i];
        put_u8(out, input->kind);
        put_u32(out, input->param);
        put_splace(out, types, &input->place);
        put_text(out, input->kind == IR_SINPUT_CALL ? input->callee
                                                   : (core_string_t){0});
    }
    put_u32(out, (uint32_t)summary->outputs.len);
    for (size_t i = 0; i < summary->outputs.len; i++) {
        const ir_soutput_t *output = &summary->outputs.data[i];
        put_u8(out, output->kind);
        put_splace(out, types, &output->place);
        put_u32(out, output->data_first);
        put_u32(out, output->data_count);
        put_u32(out, output->control_first);
        put_u32(out, output->control_count);
    }
    put_u32(out, (uint32_t)summary->dependences.len);
    for (size_t i = 0; i < summary->dependences.len; i++)
        put_u32(out, summary->dependences.data[i]);
    return true;
}

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         at;
    bool           failed;
} summary_reader_t;

static uint8_t
get_u8(summary_reader_t *r)
{
    if (r->failed || r->at >= r->size) {
        r->failed = true;
        return 0;
    }
    return r->data[r->at++];
}

static uint32_t
get_u32(summary_reader_t *r)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= (uint32_t)get_u8(r) << (8 * i);
    return value;
}

static core_string_t
get_text(summary_reader_t *r)
{
    uint32_t length = get_u32(r);
    if (r->failed || length > r->size - r->at) {
        r->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = length
        ? core_string_borrow((const char *)r->data + r->at, length)
        : (core_string_t){0};
    r->at += length;
    return text;
}

static const ir_global_t *
get_global(summary_reader_t *r, ir_summary_global_fn resolve, void *context)
{
    core_string_t module = get_text(r);
    core_string_t name   = get_text(r);
    return !r->failed && module.data && name.data && resolve
        ? resolve(context, module, name) : nullptr;
}

static void
get_place_type(summary_reader_t *r, th_ctx *types, ir_splace_t *place)
{
    core_string_t text = get_text(r);
    if (!place->global && types && text.data && text.u8_bytes)
        place->type = ir_type_from_text(types, text);
}

static ir_splace_t
get_splace(summary_reader_t *r, th_ctx *types, ir_summary_global_fn resolve,
           void *context)
{
    ir_splace_t place = {.kind = get_u8(r)};
    place.param  = get_u32(r);
    place.global = get_global(r, resolve, context);
    if (place.kind > IR_SPLACE_UNKNOWN) r->failed = true;
    if (place.global) place.type = place.global->type;
    get_place_type(r, types, &place);
    if ((place.kind == IR_SPLACE_GLOBAL || place.kind == IR_SPLACE_THREAD)
        && !place.global)
        place = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
    return place;
}

static uint64_t
get_u64(summary_reader_t *r)
{
    uint64_t low = get_u32(r);
    return low | (uint64_t)get_u32(r) << 32;
}

static void
get_flow(summary_reader_t *r, ir_sflow_t *flow)
{
    for (int kind = 0; kind < IR_SFLOW_KINDS; kind++) {
        flow->writes[kind]  = get_u64(r);
        flow->escapes[kind] = get_u64(r);
        flow->leaks[kind]   = get_u64(r);
    }
    flow->receives = get_u64(r);
    uint8_t bits = get_u8(r);
    flow->receives_fresh    = bits & 1;
    flow->receives_nonlocal = bits >> 1 & 1;
    flow->writes_nonlocal   = bits >> 2 & 1;
    flow->opaque            = bits >> 3 & 1;
    uint32_t carried = get_u32(r);
    if (r->failed || carried > (r->size - r->at) / 17) {
        r->failed = true;
        return;
    }
    flow->n_carried = carried;
    flow->carried   = carried ? core_alloc_array(ir_scarried_t, carried)
                              : nullptr;
    for (uint32_t s = 0; s < carried; s++) {
        flow->carried[s].from[IR_SFLOW_SELF]     = get_u64(r);
        flow->carried[s].from[IR_SFLOW_CONTENTS] = get_u64(r);
        uint8_t flags = get_u8(r);
        flow->carried[s].nonlocal = flags & 1;
        flow->carried[s].fresh    = flags >> 1 & 1;
        flow->carried[s].shared   = flags >> 2 & 1;
    }
}

ir_summary_t *
ir_summary_decode(ir_summaries_t *summaries, const ir_func_t *function,
                  const uint8_t *data, size_t size,
                  ir_summary_global_fn resolve, void *context)
{
    summary_reader_t r = {.data = data, .size = size};
    if (!data || size < 5 || memcmp(data, "IRSM", 4)
        || data[4] != SUMMARY_CODEC_VERSION) return nullptr;
    r.at = 5;
    th_ctx *types = function && function->owner
        ? ir_module_types(function->owner) : nullptr;
    ir_summary_t *summary = ir_summary_new(nullptr, function);
    summary->flags      = get_u32(&r);
    summary->all_escape = get_u8(&r) != 0;
    summary->traps      = get_u8(&r) & IR_STRAP_ALL;
    uint32_t trap_sites = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < trap_sites; i++) {
        ir_strap_site_t site = {.kind = get_u8(&r) & IR_STRAP_ALL};
        site.line   = get_u32(&r);
        site.column = get_u32(&r);
        core_list_push(summary->trap_sites, site);
    }
    uint32_t escapes = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < escapes; i++)
        core_list_push(summary->escapes, get_u8(&r));
    ir_sreturn_t returned = {.kind = get_u8(&r)};
    returned.param = get_u32(&r);
    returned.global = get_global(&r, resolve, context);
    if (returned.kind > IR_SRETURN_UNKNOWN
        || (returned.kind == IR_SRETURN_GLOBAL && !returned.global))
        returned = (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    summary->returned = returned;
    uint32_t accesses = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < accesses; i++) {
        ir_splace_t place = {.kind = get_u8(&r)};
        uint8_t flags = get_u8(&r);
        place.param  = get_u32(&r);
        place.global = get_global(&r, resolve, context);
        if (place.kind > IR_SPLACE_UNKNOWN) r.failed = true;
        if (place.global) place.type = place.global->type;
        get_place_type(&r, types, &place);
        ir_summary_add_access(summary, place, flags);
    }
    get_flow(&r, &summary->flow);
    uint32_t inputs = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < inputs; i++) {
        ir_sinput_t input = {.kind = get_u8(&r)};
        input.param = get_u32(&r);
        input.place = get_splace(&r, types, resolve, context);
        core_string_t callee = get_text(&r);
        if (input.kind > IR_SINPUT_CALL) r.failed = true;
        if (input.kind == IR_SINPUT_CALL)
            input.callee = callee.data && function
                ? ir_strdup(function->owner->ctx, callee)
                : (core_string_t){0};
        if ((input.kind == IR_SINPUT_PLACE
             && input.place.kind == IR_SPLACE_UNKNOWN)
            || (input.kind == IR_SINPUT_CALL && !input.callee.data))
            input = (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN};
        core_list_push(summary->inputs, input);
    }
    uint32_t outputs = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < outputs; i++) {
        ir_soutput_t output = {.kind = get_u8(&r)};
        output.place         = get_splace(&r, types, resolve, context);
        output.data_first    = get_u32(&r);
        output.data_count    = get_u32(&r);
        output.control_first = get_u32(&r);
        output.control_count = get_u32(&r);
        if (output.kind > IR_SOUTPUT_PLACE) r.failed = true;
        core_list_push(summary->outputs, output);
    }
    uint32_t dependences = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < dependences; i++) {
        uint32_t input = get_u32(&r);
        if (input >= summary->inputs.len) r.failed = true;
        core_list_push(summary->dependences, input);
    }
    for (size_t i = 0; !r.failed && i < summary->outputs.len; i++) {
        const ir_soutput_t *output = &summary->outputs.data[i];
        if ((uint64_t)output->data_first + output->data_count > dependences
            || (uint64_t)output->control_first + output->control_count
                   > dependences)
            r.failed = true;
    }
    if (r.failed || r.at != r.size) {
        summary_free(summary);
        return nullptr;
    }
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}
