#include "internal.h"

enum { POINTER_RESULT_UNKNOWN, POINTER_RESULT_NO, POINTER_RESULT_YES };

typedef struct {
    const flow_table_t *table;
    size_t              dest;
    uint32_t            source;
} alias_cursor_t;

typedef struct {
    ir_summaries_t   *summaries;
    const ir_func_t  *f;
    ir_places_t      *places;
    const ir_instr_t *call;
    const uint32_t   *ids;
    uint32_t          count;
} write_query_t;

static bool holds_pointers(const ir_func_t *f, th_type *type, uint32_t depth);
static bool returns_value(const ir_func_t *f);

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

bool
ir_splace_eq(const ir_splace_t *a, const ir_splace_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case IR_SPLACE_PARAM:
    case IR_SPLACE_SOURCE:
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
    summary->fields      = core_list_new(ir_sfield_t);
    summary->trap_sites  = core_list_new(ir_strap_site_t);
    summary->witnesses   = core_list_new(ir_switness_t);
    summary->heaps       = core_list_new(ir_sheap_t);
    summary->effects     = core_list_new(core_string_t);
    summary->paths       = core_list_new(ir_spath_t);
    flow_table_init(&summary->flows[IR_SFLOW_RESULTS], function);
    flow_table_init(&summary->flows[IR_SFLOW_STORES], function);
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

static void
summary_free(ir_summary_t *summary)
{
    for (size_t i = 0; i < summary->effects.len; i++)
        core_free(summary->effects.data[i].data);
    core_list_free(summary->effects);
    core_list_free(summary->paths);
    flow_table_free(&summary->flows[IR_SFLOW_RESULTS]);
    flow_table_free(&summary->flows[IR_SFLOW_STORES]);
    core_list_free(summary->witnesses);
    core_list_free(summary->heaps);
    core_list_free(summary->trap_sites);
    core_list_free(summary->fields);
    core_free(summary);
}

void
ir_summary_set_flags(ir_summary_t *summary, uint32_t flags)
{
    summary->flags |= flags;
}

bool
ir_summary_add_effect(ir_summary_t *summary, core_string_t name)
{
    if (!summary || !name.u8_bytes) return false;
    size_t at = 0;
    while (at < summary->effects.len) {
        int order = core_string_cmp(summary->effects.data[at], name);
        if (order == 0) return true;
        if (order > 0) break;
        at++;
    }
    core_string_t copy = core_string_dup(name);
    if (!copy.data) return false;
    core_list_insert(summary->effects, at, copy);
    return true;
}

uint32_t
ir_summary_effect_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->effects.len : 0;
}

core_string_t
ir_summary_effect_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->effects.len
        ? summary->effects.data[index] : (core_string_t){0};
}

static const core_string_t flag_names[] = {
#define IR_EFFECT_FLAG(id, name, bit) [bit] = CORE_STRING_STATIC(#name),
#include "ir/summary_effects.def"
};

static const core_string_t trap_names[] = {
#define IR_EFFECT_TRAP(id, name, bit) [bit] = CORE_STRING_STATIC(#name),
#include "ir/summary_effects.def"
};

static const struct {
    core_string_t name;
    uint64_t      bits;
} effect_names[] = {
#define IR_EFFECT_FLAG(id, name, bit) \
    {CORE_STRING_STATIC(#name), UINT64_C(1) << bit},
#define IR_EFFECT_TRAP(id, name, bit) \
    {CORE_STRING_STATIC("traps_" #name), \
     UINT64_C(1) << (IR_SEFFECT_TRAPS_SHIFT + bit)},
#define IR_EFFECT_NONLOCAL(id, name, bit) \
    {CORE_STRING_STATIC(#name), UINT64_C(1) << bit},
#include "ir/summary_effects.def"
};

core_string_t
ir_summary_flag_name(uint32_t bit)
{
    return bit < sizeof(flag_names) / sizeof(*flag_names)
        ? flag_names[bit] : (core_string_t){0};
}

core_string_t
ir_summary_trap_name(uint32_t bit)
{
    return bit < sizeof(trap_names) / sizeof(*trap_names)
        ? trap_names[bit] : (core_string_t){0};
}

bool
ir_summary_effect_lookup(core_string_t name, uint64_t *bits)
{
    for (size_t i = 0; i < sizeof(effect_names) / sizeof(*effect_names); i++)
        if (core_string_eq(effect_names[i].name, name)) {
            *bits = effect_names[i].bits;
            return true;
        }
    return false;
}

bool
ir_summary_has_effect(const ir_summary_t *summary, core_string_t name)
{
    for (size_t i = 0; summary && i < summary->effects.len; i++)
        if (core_string_eq(summary->effects.data[i], name)) return true;
    return false;
}

static bool
same_effects(const ir_summary_t *a, const ir_summary_t *b)
{
    if (a->effects.len != b->effects.len) return false;
    for (size_t i = 0; i < a->effects.len; i++)
        if (!core_string_eq(a->effects.data[i], b->effects.data[i]))
            return false;
    return true;
}

static bool
merge_effects(ir_summary_t *into, const ir_summary_t *from)
{
    for (size_t i = 0; i < from->effects.len; i++)
        if (!ir_summary_add_effect(into, from->effects.data[i])) return false;
    return true;
}


static const uint32_t worst_case_flags = IR_SUMMARY_MAY_FAIL
    | IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_BLOCK
    | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT | IR_SUMMARY_MAY_DIVERGE
    | IR_SUMMARY_OBSERVES;

ir_summary_t *
ir_summary_conservative(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_new(summaries, function);
    summary->flags = worst_case_flags | IR_SUMMARY_CALLS_UNKNOWN;
    add_path(summary, &(ir_spath_t){
        .root  = IR_SPLACE_UNKNOWN,
        .flags = IR_SPATH_ACCESS | IR_SPATH_DEEP});
    summary->traps = IR_STRAP_ALL;
    return summary;
}

ir_summary_t *
ir_summary_bounded(ir_summaries_t *summaries, const ir_func_t *function,
                   uint64_t forbidden)
{
    ir_summary_t *summary = ir_summary_new(summaries, function);
    summary->flags = (worst_case_flags | IR_SUMMARY_STARTS_THREADS)
        & ~(uint32_t)forbidden;
    if (!(summary->flags & IR_SUMMARY_OBSERVES))
        summary->flags &= ~(uint32_t)IR_SUMMARY_MAY_IO;
    uint8_t globals = (forbidden & IR_SEFFECT_READS_NONLOCAL
                       ? 0 : IR_SPATH_READ | IR_SPATH_READS_ENTRY)
        | (forbidden & IR_SEFFECT_WRITES_NONLOCAL ? 0 : IR_SPATH_WRITE);
    if (globals)
        add_path(summary, &(ir_spath_t){
            .root = IR_SPLACE_UNKNOWN, .flags = globals | IR_SPATH_DEEP});
    for (uint32_t p = 0; p < function->n_params; p++) {
        ir_spath_t path = ir_spath_of_place(&(ir_splace_t){
            .kind = IR_SPLACE_PARAM, .param = p}, IR_SPATH_ACCESS);
        add_path(summary, &path);
    }
    if (summary->flags & IR_SUMMARY_MAY_TRAP) summary->traps = IR_STRAP_ALL;
    return summary;
}

static th_type *
summary_callable(const ir_summary_t *summary)
{
    th_type *callable = summary->function
        ? th_resolve(summary->function->type) : nullptr;
    return callable && th_kind_of(callable) == TH_FUNCTION ? callable
                                                           : nullptr;
}

static bool
returns_pointers(const ir_summary_t *summary)
{
    if (summary->pointer_result != POINTER_RESULT_UNKNOWN)
        return summary->pointer_result == POINTER_RESULT_YES;
    th_type *callable = summary_callable(summary);
    bool     pointers = !callable
        || holds_pointers(summary->function, th_func_ret(callable), 0);
    ((ir_summary_t *)summary)->pointer_result = pointers ? POINTER_RESULT_YES
                                                         : POINTER_RESULT_NO;
    return pointers;
}

static uint8_t
escape_from(const ir_spath_t *from, uint32_t param, bool address)
{
    if (from->root != IR_SPLACE_PARAM || from->param != param) return 0;
    bool deep = from->flags & IR_SPATH_DEEP;
    if (first_deref(from->steps, from->depth) < from->depth)
        return IR_SESCAPE_REFERENT;
    uint8_t escape = deep ? IR_SESCAPE_REFERENT : 0;
    if (address && !(from->flags & IR_SPATH_ROOT))
        escape |= IR_SESCAPE_ADDRESS;
    return escape;
}

static alias_cursor_t
aliases_of(const ir_summary_t *summary, ir_sflow_table_t table)
{
    return (alias_cursor_t){.table = &summary->flows[table]};
}

static bool
next_alias(alias_cursor_t *c, const ir_spath_t **to, const ir_spath_t **from)
{
    for (; c->dest < c->table->dests.len; c->dest++, c->source = 0) {
        const flow_dest_t *dest = &c->table->dests.data[c->dest];
        for (; c->source < c->table->sources.len; c->source++)
            if (bit_has(&dest->alias, c->source)) {
                *to   = dest->to;
                *from = c->table->sources.data[c->source++];
                return true;
            }
    }
    return false;
}

uint8_t
ir_summary_param_escape(const ir_summary_t *summary, uint32_t param)
{
    if (!summary || !summary->results_known)
        return IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT;
    uint8_t           escape = 0;
    const ir_spath_t *to, *from;
    alias_cursor_t    stores = aliases_of(summary, IR_SFLOW_STORES);
    while (next_alias(&stores, &to, &from))
        escape |= escape_from(from, param, true);
    th_type *callable = summary->function ? summary->function->type
                                          : nullptr;
    alias_cursor_t results = aliases_of(summary, IR_SFLOW_RESULTS);
    while (next_alias(&results, &to, &from)) {
        bool direct = !to->depth && !(to->flags & IR_SPATH_DEEP);
        bool own    = to->output
            ? direct && callable
                && ir_call_output_param(callable, to->output - 1u) == param
            : returns_pointers(summary);
        escape |= escape_from(from, param, !own);
    }
    const ir_func_t *function = summary->function;
    th_type         *type     = function && param < function->n_params
            && function->params[param]
        ? function->params[param]->type : nullptr;
    if ((escape & IR_SESCAPE_ADDRESS) && type && !generic_type(type)
        && !pointer_class(function->owner, type))
        escape = (uint8_t)((escape & ~IR_SESCAPE_ADDRESS)
                           | IR_SESCAPE_REFERENT);
    return escape;
}

static ir_sreturn_t
result_source(const ir_summary_t *summary, const ir_spath_t *from)
{
    bool own = !from->depth && (from->flags & IR_SPATH_ROOT)
        && !(from->flags & IR_SPATH_DEEP);
    switch (from->root) {
    case IR_SPLACE_PARAM:
        return own ? (ir_sreturn_t){.kind = IR_SRETURN_PARAM,
                                    .param = from->param}
                   : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return own && from->global
            ? (ir_sreturn_t){.kind = IR_SRETURN_GLOBAL,
                             .global = from->global}
            : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    case IR_SPLACE_HEAP:
        if (!(from->flags & IR_SPATH_ROOT)) break;
        return (ir_sreturn_t){.kind = summary->returns_external
                                  ? IR_SRETURN_FRESH_EXTERNAL
                                  : IR_SRETURN_FRESH};
    case IR_SPLACE_UNKNOWN:
    case IR_SPLACE_SOURCE:
        break;
    }
    return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
}

ir_sreturn_t
ir_summary_return(const ir_summary_t *summary)
{
    if (!summary || !summary->results_known)
        return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    ir_sreturn_t returned = {.kind = IR_SRETURN_NONE};
    if (!returns_pointers(summary)) return returned;
    const ir_spath_t *to, *from;
    alias_cursor_t    results = aliases_of(summary, IR_SFLOW_RESULTS);
    while (next_alias(&results, &to, &from)) {
        if (to->output || to->depth) continue;
        ir_sreturn_t source = result_source(summary, from);
        bool same = source.kind == returned.kind
            && source.param == returned.param
            && source.global == returned.global;
        returned = returned.kind == IR_SRETURN_NONE || same
            ? source : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    }
    return returned;
}

bool
ir_summary_returns_fresh(const ir_summary_t *summary)
{
    if (!summary || !summary->results_known) return false;
    bool              fresh = false;
    const ir_spath_t *to, *from;
    alias_cursor_t    results = aliases_of(summary, IR_SFLOW_RESULTS);
    while (next_alias(&results, &to, &from)) {
        if (to->output) continue;
        if (from->root != IR_SPLACE_HEAP || !(from->flags & IR_SPATH_ROOT))
            return false;
        fresh = true;
    }
    alias_cursor_t stores = aliases_of(summary, IR_SFLOW_STORES);
    while (fresh && next_alias(&stores, &to, &from))
        if (to->root == IR_SPLACE_HEAP && from->root != IR_SPLACE_HEAP)
            return false;
    return fresh;
}

const ir_func_t *ir_summary_function(const ir_summary_t *s) { return s->function; }
bool ir_summary_results_known(const ir_summary_t *s)
{
    return s && s->results_known;
}
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
const ir_switness_t *ir_summary_witness_for(const ir_summary_t *s,
                                            uint32_t flag)
{
    for (size_t i = 0; s && i < s->witnesses.len; i++)
        if (s->witnesses.data[i].flag == flag) return &s->witnesses.data[i];
    return nullptr;
}
const ir_sheap_t *ir_summary_heap_for(const ir_summary_t *s, uint32_t origin)
{
    for (size_t i = 0; s && i < s->heaps.len; i++)
        if (s->heaps.data[i].origin == origin) return &s->heaps.data[i];
    return nullptr;
}

void
ir_switness_callee_text(const ir_switness_t *witness, core_buffer_t *out)
{
    if (witness->module.u8_bytes) {
        core_buffer_append(out, witness->module.data, witness->module.u8_bytes);
        core_buffer_append(out, ".", 1);
    }
    int64_t overload = core_string_find_byte(witness->name, '#');
    core_buffer_append(out, witness->name.data,
                       overload >= 0 ? (size_t)overload
                                     : witness->name.u8_bytes);
}

static int
compare_targets(const void *left, const void *right)
{
    const ir_func_t *a = *(const ir_func_t *const *)left;
    const ir_func_t *b = *(const ir_func_t *const *)right;
    int order = core_string_cmp(ir_module_name(a->owner),
                                ir_module_name(b->owner));
    return order ? order : core_string_cmp(a->name, b->name);
}

static uint8_t
witness_tier(const ir_switness_t *witness, const ir_func_t *self)
{
    if (self && witness->callee == self) return 2;
    return !witness->callee
        && core_string_eq(witness->name, CORE_STRING_STATIC("?"));
}

static bool
better_witness(const ir_switness_t *witness, const ir_switness_t *candidate,
               const ir_func_t *self)
{
    uint8_t tier = witness_tier(witness, self);
    uint8_t candidate_tier = witness_tier(candidate, self);
    if (tier != candidate_tier) return candidate_tier < tier;
    if (!candidate->callee || !witness->callee) return false;
    if (candidate->line != witness->line)
        return candidate->line < witness->line;
    if (candidate->column != witness->column)
        return candidate->column < witness->column;
    return compare_targets(&candidate->callee, &witness->callee) < 0;
}

static void
add_flags(ir_summary_t *summary, uint32_t flags, const ir_instr_t *in,
          ir_func_t *callee, core_string_t name)
{
    summary->flags |= flags;
    for (uint32_t b = 0; flags && b < IR_SEFFECT_FLAG_BITS; b++) {
        uint32_t flag = 1u << b;
        if (!(flags & flag)) continue;
        ir_switness_t candidate = {
            .flag   = flag,
            .callee = callee,
            .module = callee ? ir_module_name(callee->owner)
                             : (core_string_t){0},
            .name   = callee ? callee->name : name,
            .line   = in ? in->span.start_line : 0,
            .column = in ? in->span.start_col : 0,
        };
        ir_switness_t *witness = nullptr;
        for (size_t i = 0; !witness && i < summary->witnesses.len; i++)
            if (summary->witnesses.data[i].flag == flag)
                witness = &summary->witnesses.data[i];
        if (!witness) core_list_push(summary->witnesses, candidate);
        else if (better_witness(witness, &candidate, summary->function))
            *witness = candidate;
    }
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
    if (!hooks || !hooks->adjust) return;
    uint32_t before = summary->flags;
    summary->flags  = hooks->adjust(hooks->context, summary->function,
                                    summary->flags);
    const ir_func_t *f     = summary->function;
    uint32_t         added = summary->flags & ~before;
    for (uint32_t b = 0; f && added && b < IR_SEFFECT_FLAG_BITS; b++)
        if (added & 1u << b && !ir_summary_witness_for(summary, 1u << b))
            core_list_push(summary->witnesses, ((ir_switness_t){
                .flag   = 1u << b,
                .name   = CORE_STRING_STATIC("declared by the compiler"),
                .line   = f->span.start_line,
                .column = f->span.start_col,
            }));
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

static const ir_value_t *
referenced(const ir_instr_t *in, uint32_t operand)
{
    return operand < in->n_operands
        ? ir_operand_referenced_value(&in->operands[operand]) : nullptr;
}

static bool
holds_pointers(const ir_func_t *f, th_type *type, uint32_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 8) return true;
    switch (ir_machine_of(f->owner, type).class) {
    case IR_CLASS_SINT:
    case IR_CLASS_UINT:
    case IR_CLASS_FLOAT:
    case IR_CLASS_BOOL:
    case IR_CLASS_VOID:
        return false;
    default:
        break;
    }
    uint64_t extent = 0;
    switch (th_kind_of(type)) {
    case TH_BITINT:
        return false;
    case TH_SEQUENCE:
        return !th_seq_extent(type, &extent)
            || holds_pointers(f, th_seq_elem(type), depth + 1);
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || holds_pointers(f, member->type, depth + 1))
                return true;
        }
        return false;
    case TH_SUM:
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (holds_pointers(f, th_sum_arm(type, i), depth + 1))
                return true;
        return false;
    case TH_NAMED: {
        th_type *definition = ir_type_definition(f->owner, type);
        return !definition || definition == type
            || holds_pointers(f, definition, depth + 1);
    }
    default:
        return true;
    }
}

static bool
inline_member(const ir_func_t *f, th_type *record, core_string_t name,
              bool without_layout)
{
    bool known  = false;
    bool inside = ir_layout_member_inline(f->owner, record, name, &known);
    return known ? inside : without_layout;
}

static bool
inline_field(const ir_func_t *f, const ir_value_t *base, bool without_layout)
{
    const ir_instr_t *field = base && base->kind == IR_VAL_INSTR
        ? base->def : nullptr;
    if (!field || field->op != IR_OP_FIELD || field->n_operands != 2
        || field->operands[0].kind != IR_OPD_VALUE
        || field->operands[1].kind != IR_OPD_NAME
        || !field->operands[0].value) return false;
    return inline_member(f, th_ind_referent(field->operands[0].value->type),
                         field->operands[1].name, without_layout);
}

static void
add_unknown_path(ir_summary_t *summary, uint8_t flags)
{
    ir_spath_t path = {.root = IR_SPLACE_UNKNOWN,
                       .flags = flags | IR_SPATH_DEEP};
    add_path(summary, &path);
}

static void
add_function_path(ir_summary_t *summary, const ir_func_t *f,
                  ir_spath_t *path)
{
    th_type *type = path->root == IR_SPLACE_PARAM && path->param < f->n_params
            && f->params[path->param]->type
        ? th_resolve(f->params[path->param]->type) : nullptr;
    th_kind kind = type ? th_kind_of(type) : TH_NAMED;
    if (kind == TH_VAR || kind == TH_BOUND || kind == TH_BINDER
        || kind == TH_TRAIT) {
        path->depth  = 0;
        path->flags |= IR_SPATH_DEEP;
    }
    add_path(summary, path);
}

uint64_t
ir_summary_effect_bits(const ir_summary_t *summary)
{
    uint64_t bits = summary->flags & IR_SEFFECT_FLAG_MASK;
    bits |= (uint64_t)summary->traps << IR_SEFFECT_TRAPS_SHIFT;
    for (size_t i = 0; i < summary->paths.len; i++) {
        const ir_spath_t *path = &summary->paths.data[i];
        bool read  = path->flags & IR_SPATH_READ;
        bool write = path->flags & IR_SPATH_WRITE;
        if (path->root == IR_SPLACE_HEAP) continue;
        if (path->root != IR_SPLACE_PARAM) {
            if (read) bits |= IR_SEFFECT_READS_NONLOCAL;
            if (write) bits |= IR_SEFFECT_WRITES_NONLOCAL;
            continue;
        }
        uint32_t param = path->param < 16 ? path->param : 15;
        if (read) bits |= UINT64_C(1) << (IR_SEFFECT_READS_PARAM_SHIFT + param);
        if (write)
            bits |= UINT64_C(1) << (IR_SEFFECT_WRITES_PARAM_SHIFT + param);
    }
    return bits;
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
        | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS
        | IR_SUMMARY_OBSERVES;
    bool *pure = context;
    *pure = *pure && !(summary->flags & effects);
    for (size_t i = 0; *pure && i < summary->paths.len; i++)
        *pure = summary->paths.data[i].root == IR_SPLACE_HEAP
            || !(summary->paths.data[i].flags & IR_SPATH_WRITE);
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
    for (size_t i = 0; i < summary->paths.len; i++) {
        const ir_spath_t *path = &summary->paths.data[i];
        ir_splace_t       places[2];
        uint32_t          count = path->flags & IR_SPATH_WRITE
            ? ir_summary_path_places(summary->function, path, places) : 0;
        for (uint32_t p = 0; p < count; p++)
            if (access_writes(q, &places[p], follow)) return true;
    }
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
ir_summary_call_adds_effects(ir_summaries_t *summaries, const ir_func_t *caller,
                             const ir_func_t *callee)
{
    const ir_summary_hooks_t *hooks = summaries ? summaries->hooks : nullptr;
    if (!hooks || !hooks->callee_effects || !caller || !callee) return false;
    ir_summary_t *probe = ir_summary_new(nullptr, caller);
    hooks->callee_effects(hooks->context, callee, probe);
    bool adds = probe->flags || probe->effects.len;
    summary_free(probe);
    return adds;
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
returns_value(const ir_func_t *f)
{
    return ir_callable_returns_value(f->owner, f->type);
}

bool
ir_address_escapes(ir_summaries_t *summaries, const ir_func_t *function,
                   const ir_value_t *address)
{
    return engine_escapes(summaries, function, address);
}

void
ir_summary_set_param_escape(ir_summary_t *summary, uint32_t param,
                            uint8_t flags)
{
    const ir_spath_t anywhere = {.root  = IR_SPLACE_UNKNOWN,
                                 .flags = IR_SPATH_DEEP};
    if (flags & IR_SESCAPE_ADDRESS)
        flow_add(&summary->flows[IR_SFLOW_STORES], &anywhere,
                 &(ir_spath_t){.root = IR_SPLACE_PARAM, .param = param},
                 IR_SFLOW_ALIAS);
    if (flags & IR_SESCAPE_REFERENT)
        flow_add(&summary->flows[IR_SFLOW_STORES], &anywhere,
                 &(ir_spath_t){.root = IR_SPLACE_PARAM, .param = param,
                               .steps = &value_step, .depth = 1,
                               .flags = IR_SPATH_DEEP},
                 IR_SFLOW_ALIAS);
    summary->results_known = true;
}

void
ir_summary_set_return(ir_summary_t *summary, ir_sreturn_t returned)
{
    ir_spath_t from = {.root = IR_SPLACE_UNKNOWN, .flags = IR_SPATH_DEEP};
    switch (returned.kind) {
    case IR_SRETURN_NONE:
        summary->results_known = true;
        return;
    case IR_SRETURN_PARAM:
        from = (ir_spath_t){.root = IR_SPLACE_PARAM, .param = returned.param,
                            .flags = IR_SPATH_ROOT};
        break;
    case IR_SRETURN_GLOBAL:
        from = ir_spath_of_place(&(ir_splace_t){
            .kind = IR_SPLACE_GLOBAL, .global = returned.global}, 0);
        break;
    case IR_SRETURN_FRESH_EXTERNAL:
        summary->returns_external = true;
        from = (ir_spath_t){.root = IR_SPLACE_HEAP, .flags = IR_SPATH_ROOT};
        break;
    case IR_SRETURN_FRESH:
        from = (ir_spath_t){.root = IR_SPLACE_HEAP, .flags = IR_SPATH_ROOT};
        break;
    case IR_SRETURN_UNKNOWN:
        break;
    }
    flow_add(&summary->flows[IR_SFLOW_RESULTS], &(ir_spath_t){0}, &from,
             IR_SFLOW_ALIAS);
    summary->results_known = true;
}
