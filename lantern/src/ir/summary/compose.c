#include "internal.h"

typedef struct {
    uint8_t     *counts;
    ir_splace_t *places;
} path_places_t;



enum {
    UNPASSED_RETURN = 1u << 0,
    UNPASSED_FAIL   = 1u << 1,
};

static uint32_t
query_path_places(flow_query_t *q, const ir_summary_t *s, size_t index,
                  ir_splace_t places[2])
{
    if (!q->path_places)
        return ir_summary_path_places(s->function, &s->paths.data[index],
                                      places);
    bool           found  = false;
    path_places_t *cached = core_dict_get(q->path_places, (void *)s, &found);
    if (!found) {
        size_t count   = s->paths.len;
        cached         = core_alloc(path_places_t);
        cached->counts = core_alloc_array(uint8_t, count ? count : 1);
        cached->places = core_alloc_array(ir_splace_t, 2 * (count ? count
                                                                  : 1));
        for (size_t i = 0; i < count; i++)
            cached->counts[i] = (uint8_t)ir_summary_path_places(s->function,
                &s->paths.data[i], &cached->places[2 * i]);
        core_dict_put(q->path_places, (void *)s, cached);
    }
    places[0] = cached->places[2 * index];
    places[1] = cached->places[2 * index + 1];
    return cached->counts[index];
}

static void
path_places_free(core_dict_t *cache)
{
    core_dict_foreach(cache, item) {
        path_places_t *cached = item->value;
        core_free(cached->counts);
        core_free(cached->places);
        core_free(cached);
    }
    core_dict_free(cache);
}

static bool
named_place(const ir_splace_t *place)
{
    return place->kind != IR_SPLACE_HEAP && place->kind != IR_SPLACE_UNKNOWN;
}

static uint8_t
place_flags(const ir_spath_t *path)
{
    return path->depth ? path->flags & (uint8_t)~IR_SPATH_MUST : path->flags;
}

static uint8_t
target_access_flags(flow_query_t *q, const ir_instr_t *call,
                    const ir_summary_t *s)
{
    uint8_t flags = 0;
    for (size_t i = 0; q->target && i < s->paths.len; i++) {
        const ir_spath_t *path = &s->paths.data[i];
        ir_splace_t       places[2];
        uint32_t          count = query_path_places(q, s, i, places);
        for (uint32_t p = 0; p < count; p++) {
            const ir_splace_t *place = &places[p];
            bool matches = place->kind == IR_SPLACE_PARAM
                ? argument_is(q, call, path, q->target)
                : place->kind == IR_SPLACE_UNKNOWN
                ? unseen_reaches(q->summaries, q->target)
                : named_place(place) && ir_splace_eq(place, q->target);
            if (matches)
                flags |= named_place(place) ? place_flags(path)
                                            : path->flags & IR_SPATH_ACCESS;
        }
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
                flags |= IR_SPATH_ACCESS;
            continue;
        }
        for (size_t i = 0; i < entry->paths.len; i++) {
            const ir_spath_t *path = &entry->paths.data[i];
            ir_splace_t       places[2];
            uint32_t          count = path->root == IR_SPLACE_PARAM ? 0
                : query_path_places(q, entry, i, places);
            for (uint32_t p = 0; p < count; p++)
                if (access_touches(q->summaries, &places[p], q->target))
                    flags |= path->flags & IR_SPATH_ACCESS;
        }
    }
    return flags;
}

static uint8_t
written(uint8_t flags, uint8_t must)
{
    if (flags & must) return IR_PATH_PASSED;
    if (flags & IR_SPATH_WRITE) return IR_PATH_UNPASSED | IR_PATH_PASSED;
    return IR_PATH_UNPASSED;
}

static uint8_t
callee_effect(const ir_summary_t *s, uint8_t flags, ir_path_outcome_t outcome)
{
    bool diverges = (s->flags & IR_SUMMARY_DIVERGES) != 0;
    uint8_t success = diverges || (s->flags & IR_SUMMARY_NO_RETURN)
        ? 0 : written(flags, IR_SPATH_MUST_SUCCESS);
    uint8_t failure = !diverges && (s->flags & IR_SUMMARY_MAY_FAIL)
        ? written(flags, IR_SPATH_MUST_FAILURE) : 0;
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

static uint8_t
cached_call_effect(flow_query_t *q, const ir_instr_t *call,
                   ir_path_outcome_t outcome)
{
    if (!q->effects) return call_effect(q, call, outcome);
    if (!ir_splace_eq(&q->effects_place, q->target)) {
        core_dict_free(q->effects);
        core_dict_init(q->effects, nullptr, nullptr);
        q->effects_place = *q->target;
    }
    void     *key   = (void *)((uintptr_t)call | (uintptr_t)outcome);
    bool      found = false;
    uintptr_t cached = (uintptr_t)core_dict_get(q->effects, key, &found);
    if (found) return (uint8_t)cached;
    uint8_t effect = call_effect(q, call, outcome);
    core_dict_put(q->effects, key, (void *)(uintptr_t)effect);
    return effect;
}

static uint8_t
flow_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    flow_query_t *q = context;
    if (is_call(in)) return cached_call_effect(q, in, outcome);
    return writes_target(q, in) ? IR_PATH_PASSED : IR_PATH_UNPASSED;
}

static ir_spath_t
read_path(const ir_splace_t *place, const ir_instr_t *in)
{
    ir_spath_t path = {0};
    if ((in->op != IR_OP_LOAD && in->op != IR_OP_EXTRACT) || !in->n_operands)
        return path;
    if (in->op == IR_OP_EXTRACT) {
        if (in->n_operands != 2 || in->operands[1].kind != IR_OPD_NAME)
            return path;
        path_push(&path, in->operands[1].name);
    }
    const ir_value_t *address = field_chain(
        ir_operand_value(&in->operands[0]),
        in->op == IR_OP_LOAD ? IR_OP_FIELD : IR_OP_EXTRACT, &path);
    if (!path.depth || !address || address->kind != IR_VAL_GLOBAL
        || !address->global
        || ir_global_identity(address->global) != place->global)
        return (ir_spath_t){0};
    return path;
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
    for (size_t i = 0; i < summary->paths.len; i++) {
        ir_splace_t places[2];
        uint32_t    count = ir_summary_path_places(summary->function,
                                                   &summary->paths.data[i],
                                                   places);
        for (uint32_t p = 0; p < count; p++)
            if (named_place(&places[p]) && ir_splace_eq(&places[p], place))
                flags |= place_flags(&summary->paths.data[i]);
    }
    return callee_effect(summary, flags, outcome);
}

struct ir_summary_state_query_t {
    ir_func_t            *f;
    uint8_t               entry;
    ir_splace_t           place;
    core_dict_t           cache;
    flow_query_t          q;
    ir_paths_solution_t  *whole;
    bool                  split;
    path_list_t           leaves;
    flow_query_t         *leaf_queries;
    ir_paths_solution_t **leaf_solutions;
};

ir_summary_state_query_t *
ir_summary_state_query(ir_summaries_t *summaries, ir_func_t *f,
                       const ir_splace_t *place, uint8_t entry)
{
    pt_t *pt = function_engine(summaries, f);
    if (!pt) return nullptr;
    ir_summary_state_query_t *query = core_alloc(ir_summary_state_query_t);
    query->f     = f;
    query->entry = entry;
    query->place = *place;
    core_dict_init(&query->cache, nullptr, nullptr);
    query->q = (flow_query_t){.summaries = summaries, .f = f, .pt = pt,
                              .target = &query->place,
                              .path_places = &query->cache};
    query->whole = ir_paths_solve(f, entry, flow_step, &query->q);
    return query;
}

static void
split_leaves(ir_summary_state_query_t *query)
{
    query->split  = true;
    query->leaves = place_leaves(query->f, &query->place);
    query->leaf_queries = core_alloc_array(flow_query_t,
                                           query->leaves.len + 1);
    query->leaf_solutions = core_alloc_array(ir_paths_solution_t *,
                                             query->leaves.len + 1);
}

uint8_t
ir_summary_state_query_at(ir_summary_state_query_t *query,
                          const ir_instr_t *in)
{
    if (!query) return IR_PATH_UNPASSED;
    uint8_t state = ir_paths_solution_state(query->whole, in);
    if (!(state & IR_PATH_UNPASSED)) return state;
    if (!query->split) split_leaves(query);
    ir_spath_t read  = read_path(&query->place, in);
    bool       first = true;
    for (size_t l = 0; query->leaves.data[0].depth && l < query->leaves.len;
         l++) {
        if (!paths_overlap(&query->leaves.data[l], &read)) continue;
        if (!query->leaf_solutions[l]) {
            query->leaf_queries[l]      = query->q;
            query->leaf_queries[l].leaf = &query->leaves.data[l];
            query->leaf_solutions[l] = ir_paths_solve(query->f, query->entry,
                flow_step, &query->leaf_queries[l]);
        }
        state = combine_leaf(state,
            ir_paths_solution_state(query->leaf_solutions[l], in), first);
        first = false;
    }
    return state;
}

void
ir_summary_state_query_free(ir_summary_state_query_t *query)
{
    if (!query) return;
    ir_paths_solution_free(query->whole);
    for (size_t l = 0; query->split && l < query->leaves.len; l++)
        ir_paths_solution_free(query->leaf_solutions[l]);
    if (query->split) core_list_free(query->leaves);
    core_free(query->leaf_queries);
    core_free(query->leaf_solutions);
    path_places_free(&query->cache);
    core_free(query);
}

uint8_t
ir_summary_state_before(ir_summaries_t *summaries, ir_func_t *f,
                        const ir_splace_t *place, uint8_t entry,
                        const ir_instr_t *in)
{
    ir_summary_state_query_t *query = ir_summary_state_query(summaries, f,
                                                             place, entry);
    uint8_t state = ir_summary_state_query_at(query, in);
    ir_summary_state_query_free(query);
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

static bool
call_unobserved(const ir_instr_t *call)
{
    for (uint32_t a = 0; a < call->annots.n; a++)
        if (call->annots.a[a].kind == IR_ANNOT_BOOL && call->annots.a[a].b
            && core_string_eq(call->annots.a[a].key,
                              CORE_STRING_STATIC("ir.unobserved")))
            return true;
    return false;
}

enum {
    CALLBACK_PARAM_LIMIT = 32,
    CALLBACK_DEPTH_LIMIT = 4,
    INHERITED_FLAGS = IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
        | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
        | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
        | IR_SUMMARY_ALLOCATES_DIRECT | IR_SUMMARY_CALLS_FINALIZERS
        | IR_SUMMARY_STARTS_THREADS | IR_SUMMARY_REINTERPRETS
        | IR_SUMMARY_CALLS_CALLBACKS | IR_SUMMARY_MAY_DIVERGE
        | IR_SUMMARY_FINALIZES_PARAMS,
};

static const ir_value_t *
copied_value(const ir_value_t *value)
{
    for (int depth = 0; value && value->kind == IR_VAL_INSTR && value->def
         && depth < 8; depth++) {
        const ir_instr_t *def = value->def;
        if ((def->op != IR_OP_COPY && def->op != IR_OP_RETAIN_REF)
            || def->n_operands != 1 || def->operands[0].kind != IR_OPD_VALUE)
            break;
        value = def->operands[0].value;
    }
    return value;
}

static int32_t
callback_param(const ir_value_t *value)
{
    value = copied_value(value);
    return value && value->kind == IR_VAL_PARAM
        && value->index < CALLBACK_PARAM_LIMIT ? (int32_t)value->index : -1;
}

static ir_func_t *
created_handler(const ir_value_t *value)
{
    value = copied_value(value);
    const ir_instr_t *create = value && value->kind == IR_VAL_INSTR
        ? value->def : nullptr;
    if (!create || create->op != IR_OP_CALLBACK_CREATE || !create->n_operands
        || create->operands[0].kind != IR_OPD_VALUE) return nullptr;
    const ir_value_t *handler = copied_value(create->operands[0].value);
    return handler && handler->kind == IR_VAL_FUNC && handler->owner
        && handler->owner->n_blocks ? handler->owner : nullptr;
}

static void compose_target(ir_summaries_t *summaries, ir_summary_t *summary,
                           const ir_instr_t *call, ir_func_t *target,
                           uint32_t observes, int depth);
static bool panic_only(const ir_summary_t *summary);

static void
add_finalizer(void *sink, ir_func_t *finalizer)
{
    core_list_t(summary_func_ptr_t) *finalizers = sink;
    core_list_push(*finalizers, finalizer);
}

static bool
substitute_finalizers(ir_summaries_t *summaries, ir_summary_t *summary,
                      const ir_instr_t *call, const ir_summary_t *callee,
                      ir_func_t *target, uint32_t observes, int depth)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (!(callee->flags & IR_SUMMARY_FINALIZES_PARAMS) || !hooks
        || !hooks->call_finalizers || depth >= CALLBACK_DEPTH_LIMIT)
        return false;
    core_list_t(summary_func_ptr_t) finalizers =
        core_list_new(summary_func_ptr_t);
    bool bound = hooks->call_finalizers(hooks->context, call, target,
                                        add_finalizer, &finalizers);
    note_finalizers(summaries, call, target, &finalizers, bound);
    for (size_t i = 0; bound && i < finalizers.len; i++)
        compose_target(summaries, summary, call, finalizers.data[i],
                       observes, depth + 1);
    core_list_free(finalizers);
    return bound;
}

static bool
substitute_callbacks(ir_summaries_t *summaries, ir_summary_t *summary,
                     const ir_instr_t *call, const ir_summary_t *callee,
                     ir_func_t *target, uint32_t observes, int depth)
{
    if (!callee->callback_params || depth >= CALLBACK_DEPTH_LIMIT)
        return false;
    ir_func_t *handlers[CALLBACK_PARAM_LIMIT] = {0};
    uint32_t forwarded = 0;
    for (uint32_t p = 0; p < CALLBACK_PARAM_LIMIT; p++) {
        if (!(callee->callback_params >> p & 1)) continue;
        const ir_value_t *argument = p + 1 < call->n_operands
            && call->operands[p + 1].kind == IR_OPD_VALUE
            ? call->operands[p + 1].value : nullptr;
        handlers[p] = created_handler(argument);
        int32_t param = handlers[p] ? -1 : callback_param(argument);
        if (!handlers[p] && param < 0) return false;
        if (param >= 0) forwarded |= 1u << param;
    }
    if (forwarded) {
        bool deferring = summary->deferring;
        summary->deferring = true;
        summary->callback_params |= forwarded;
        uint32_t flags = callee->flags & ~callee->own_flags
            & (INHERITED_FLAGS | observes);
        if (flags)
            add_flags(summary, flags, call, target, (core_string_t){0});
        summary->traps |= callee->traps;
        summary->deferring = deferring;
    }
    for (uint32_t p = 0; p < CALLBACK_PARAM_LIMIT; p++)
        if (handlers[p])
            compose_target(summaries, summary, call, handlers[p], observes,
                           depth + 1);
    return true;
}

static void
compose_target(ir_summaries_t *summaries, ir_summary_t *summary,
               const ir_instr_t *call, ir_func_t *target, uint32_t observes,
               int depth)
{
    ir_summary_t *callee = target_summary(summaries, target);
    bool substituted = substitute_callbacks(summaries, summary, call, callee,
                                            target, observes, depth);
    uint32_t callee_flags = substituted ? callee->own_flags : callee->flags;
    uint8_t callee_traps = substituted ? callee->own_traps : callee->traps;
    summary->traps |= callee_traps;
    if (!summary->deferring) summary->own_traps |= callee_traps;
    uint32_t flags = callee_flags & (INHERITED_FLAGS | observes);
    if ((callee->flags & IR_SUMMARY_DIVERGES) && !panic_only(callee))
        flags |= IR_SUMMARY_MAY_DIVERGE;
    if (substitute_finalizers(summaries, summary, call, callee, target,
                              observes, depth))
        flags &= ~(uint32_t)IR_SUMMARY_FINALIZES_PARAMS;
    if (flags) add_flags(summary, flags, call, target, (core_string_t){0});
    const ir_summary_hooks_t *hooks = summaries->hooks;
    for (size_t e = 0; e < callee->effects.len; e++) {
        core_string_t effect = callee->effects.data[e];
        bool inherits = inherits_answer(summaries, summary->function, target,
                                        effect);
        note_inherits(summaries, summary->function, target, effect,
                      inherits);
        if (inherits) (void)ir_summary_add_effect(summary, effect);
    }
    note_effects(summaries, summary->function, target);
    if (hooks && hooks->callee_effects) {
        uint32_t before = summary->flags;
        hooks->callee_effects(hooks->context, target, summary);
        uint32_t added = summary->flags & ~before;
        if (added)
            add_flags(summary, added, call, target, (core_string_t){0});
    }
}

static void
compose_call(ir_summaries_t *summaries, ir_summary_t *summary,
             const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    uint32_t observes = call_unobserved(call) ? 0 : IR_SUMMARY_OBSERVES;
    int32_t param = call->op == IR_OP_CALLBACK_INVOKE && call->n_operands
        && call->operands[0].kind == IR_OPD_VALUE
        ? callback_param(call->operands[0].value) : -1;
    bool deferred = targets.unknown && param >= 0;
    bool deferring = summary->deferring;
    if (deferred) {
        summary->deferring = true;
        summary->callback_params |= 1u << param;
    }
    if (call->op == IR_OP_CALLBACK_INVOKE)
        add_flags(summary, IR_SUMMARY_CALLS_CALLBACKS, call, nullptr,
                  (core_string_t){0});
    const ir_summary_hooks_t *hooks = summaries->hooks;
    uint32_t extra = hooks && hooks->call_flags
        ? hooks->call_flags(hooks->context, call) : 0;
    if (extra)
        add_flags(summary, extra, call,
                  targets.targets.len ? targets.targets.data[0] : nullptr,
                  (core_string_t){0});
    if (extra & IR_SUMMARY_FINALIZES_PARAMS) {
        summary->deferring = deferring;
        core_list_free(targets.targets);
        return;
    }
    if (targets.unknown) {
        const ir_func_t *f = summary->function;
        core_string_t name = deferred && f
            && (uint32_t)param < f->n_params && f->params[param]
            ? f->params[param]->name : CORE_STRING_STATIC("?");
        add_flags(summary, IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
                      | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
                      | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
                      | IR_SUMMARY_MAY_DIVERGE | observes,
                  call, nullptr, name);
        summary->traps |= IR_STRAP_ALL;
        if (!summary->deferring) summary->own_traps |= IR_STRAP_ALL;
        summary->deferring = deferring;
        core_list_free(targets.targets);
        return;
    }
    for (size_t t = 0; t < targets.targets.len; t++)
        compose_target(summaries, summary, call, targets.targets.data[t],
                       observes, 0);
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
                | IR_SUMMARY_ALLOCATES_DIRECT
            : IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_ALLOC_USING:
    case IR_OP_ARRAY_NEW_USING:
    case IR_OP_BOX:
        return IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_BUFFER_SLICE_END:
        return IR_SUMMARY_MAY_COLLECT;
    default:
        return 0;
    }
}

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
    const ir_spath_t *within = q->leaf;
    path_list_t       leaves = place_leaves(f, q->target);
    bool              split  = false;
    uint8_t           parts  = 0;
    for (size_t l = 0; leaves.data[0].depth && l < leaves.len; l++) {
        const ir_spath_t *leaf = &leaves.data[l];
        if (within && (leaf->depth <= within->depth
                       || !steps_prefix(within->depth, within->steps,
                                        leaf->depth, leaf->steps)))
            continue;
        ir_path_exits_t exits;
        split   = true;
        q->leaf = leaf;
        parts  |= ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, q, &exits)
            ? exit_unpassed(&exits) : UNPASSED_RETURN | UNPASSED_FAIL;
    }
    q->leaf = within;
    core_list_free(leaves);
    return split ? parts : unpassed;
}

static const ir_instr_t *
exit_site(ir_summaries_t *summaries, const ir_func_t *f, uint32_t flag,
          ir_func_t **callee)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (flag == IR_SUMMARY_MAY_FAIL) {
                if (in->op == IR_OP_FAIL) return in;
                continue;
            }
            if (in->op == IR_OP_TRAP || in->op == IR_OP_UNREACHABLE)
                return in;
            if (!is_call(in)) continue;
            call_targets_t targets;
            resolve_targets(summaries, in, &targets);
            for (size_t t = 0; !*callee && t < targets.targets.len; t++)
                if (target_summary(summaries, targets.targets.data[t])->flags
                    & (IR_SUMMARY_NO_RETURN | IR_SUMMARY_DIVERGES))
                    *callee = targets.targets.data[t];
            core_list_free(targets.targets);
            if (*callee) return in;
        }
    return nullptr;
}

static bool
panic_only(const ir_summary_t *summary)
{
    return (summary->flags & IR_SUMMARY_DIVERGES)
        && (summary->flags & IR_SUMMARY_MAY_TRAP)
        && !(summary->flags & IR_SUMMARY_MAY_DIVERGE);
}

static bool
diverges_by_panic(ir_summaries_t *summaries, const ir_func_t *f)
{
    bool panics = false;
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->op == IR_OP_TRAP) {
                panics = true;
                continue;
            }
            if (in->op == IR_OP_UNREACHABLE) return false;
            if (!is_call(in)) continue;
            call_targets_t targets;
            resolve_targets(summaries, in, &targets);
            bool real = targets.unknown;
            for (size_t t = 0; !real && t < targets.targets.len; t++) {
                ir_summary_t *callee = target_summary(summaries,
                                                      targets.targets.data[t]);
                if (!(callee->flags & (IR_SUMMARY_DIVERGES
                                       | IR_SUMMARY_MAY_DIVERGE))) continue;
                if (panic_only(callee)) panics = true;
                else real = true;
            }
            core_list_free(targets.targets);
            if (real) return false;
        }
    return panics;
}

static bool
reaches_exit(ir_block_t *block, const bool *exits)
{
    ir_instr_t *term = ir_block_terminator(block);
    if (!term) return false;
    if (term->op == IR_OP_RETURN || term->op == IR_OP_FAIL
        || term->op == IR_OP_TRAP || term->op == IR_OP_UNREACHABLE)
        return true;
    for (uint32_t s = 0; s < ir_succ_count(term); s++) {
        ir_block_t *to = ir_succ_at(term, s);
        if (to && exits[to->index]) return true;
    }
    return false;
}

static const ir_instr_t *
endless_loop(ir_func_t *f)
{
    uint32_t count = f->n_blocks;
    bool *exits = core_alloc_array(bool, count ? count : 1);
    bool *seen = core_alloc_array(bool, count ? count : 1);
    uint32_t *stack = core_alloc_array(uint32_t, count ? count : 1);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = count; b-- > 0;)
            if (!exits[b] && reaches_exit(f->blocks[b], exits))
                changed = exits[b] = true;
    }
    const ir_instr_t *found = nullptr;
    ir_block_t *entry = ir_block_entry(f);
    uint32_t depth = 0;
    if (entry && entry->index < count) {
        seen[entry->index] = true;
        stack[depth++] = entry->index;
    }
    while (depth && !found) {
        ir_block_t *block = f->blocks[stack[--depth]];
        ir_instr_t *term = ir_block_terminator(block);
        for (uint32_t i = 0; !exits[block->index] && !found
             && i < block->n_instrs; i++)
            if (ir_span_known(block->instrs[i]->span))
                found = block->instrs[i];
        if (!exits[block->index] && !found) found = term;
        for (uint32_t s = 0; term && s < ir_succ_count(term); s++) {
            ir_block_t *to = ir_succ_at(term, s);
            if (!to || to->index >= count || seen[to->index]) continue;
            seen[to->index] = true;
            stack[depth++] = to->index;
        }
    }
    core_free(stack);
    core_free(seen);
    core_free(exits);
    return found;
}

static void
summarize_function(ir_summaries_t *summaries, ir_summary_t *summary,
                   ir_func_t *f, const uint32_t *settled)
{
    summary->flags = 0;
    summary->own_flags = 0;
    summary->own_traps = 0;
    summary->callback_params = 0;
    summary->deferring = false;
    for (uint32_t e = 0;; e++) {
        core_string_t effect = ir_func_effect_at(f, e);
        if (!effect.u8_bytes) break;
        (void)ir_summary_add_effect(summary, effect);
    }
    summary->witnesses.len = 0;
    const ir_instr_t *reinterpret = ir_reinterpretation_site(f);
    if (reinterpret)
        add_flags(summary, IR_SUMMARY_REINTERPRETS, reinterpret, nullptr,
                  (core_string_t){0});
    ir_sites_t  *sites  = ir_func_sites(f);
    ir_abcd_t *ranges = ir_module_props(f->owner) & IR_PROP_SSA
        ? ir_func_abcd(f) : nullptr;
    ir_abcd_t *prove_ranges = !summaries->hooks
            || !summaries->hooks->defer_trap_proofs
        ? ranges : nullptr;
    summary->proof_free = true;
    for (uint32_t s = 0; ranges && sites && s < ir_sites_count(sites); s++) {
        const ir_site_t *site = ir_site_at(sites, s);
        if (site->kind == IR_SITE_TRAP
            && ir_abcd_trap_proven(ranges, site->instr))
            summary->proof_free = false;
    }
    for (uint32_t s = 0; sites && s < ir_sites_count(sites); s++) {
        const ir_site_t *site = ir_site_at(sites, s);
        if (site->kind != IR_SITE_TRAP
            || (prove_ranges && ir_abcd_trap_proven(prove_ranges,
                                                     site->instr))) continue;
        uint8_t kind = trap_kind(site->instr);
        add_flags(summary, IR_SUMMARY_MAY_TRAP, site->instr, nullptr,
                  (core_string_t){0});
        summary->traps |= kind;
        summary->own_traps |= kind;
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
            if (is_call(in)) compose_call(summaries, summary, in);
            add_flags(summary, allocation_flags(f, in), in, nullptr,
                      (core_string_t){0});
        }
    }
    flow_query_t    q = {.summaries = summaries, .f = f};
    ir_path_exits_t exits;
    uint32_t        ends = IR_SUMMARY_MAY_FAIL;
    if (ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, &q, &exits))
        ends = (exits.returned ? 0 : IR_SUMMARY_NO_RETURN)
            | (exits.failed ? IR_SUMMARY_MAY_FAIL : 0)
            | (!exits.returned && !exits.failed ? IR_SUMMARY_DIVERGES : 0);
    if (settled)
        ends = (ends & ~(uint32_t)(IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN))
             | *settled;
    for (uint32_t b = 0; b < IR_SEFFECT_FLAG_BITS; b++) {
        uint32_t flag = 1u << b;
        if (!(ends & flag) || (summary->own_flags & flag)) continue;
        ir_func_t        *callee = nullptr;
        const ir_instr_t *site   = exit_site(summaries, f, flag, &callee);
        if (!site && flag == IR_SUMMARY_NO_RETURN)
            site = exit_site(summaries, f, IR_SUMMARY_MAY_FAIL, &callee);
        if (site) add_flags(summary, flag, site, callee, (core_string_t){0});
        else {
            summary->flags |= flag;
            summary->own_flags |= flag;
        }
    }
    if (!(summary->own_flags & IR_SUMMARY_MAY_DIVERGE)) {
        ir_func_t        *callee = nullptr;
        bool panics = (summary->own_flags & IR_SUMMARY_DIVERGES)
            && diverges_by_panic(summaries, f);
        const ir_instr_t *site   = summary->own_flags & IR_SUMMARY_DIVERGES
                && !panics
            ? exit_site(summaries, f, IR_SUMMARY_DIVERGES, &callee)
            : nullptr;
        if (!site) site = endless_loop(f);
        if (site)
            add_flags(summary, IR_SUMMARY_MAY_DIVERGE, site, callee,
                      (core_string_t){0});
        else if ((summary->own_flags & IR_SUMMARY_DIVERGES) && !panics) {
            summary->flags |= IR_SUMMARY_MAY_DIVERGE;
            summary->own_flags |= IR_SUMMARY_MAY_DIVERGE;
        }
        if (panics && !(summary->own_flags & IR_SUMMARY_MAY_TRAP)) {
            ir_func_t        *trapper = nullptr;
            const ir_instr_t *at = exit_site(summaries, f,
                                             IR_SUMMARY_DIVERGES, &trapper);
            add_flags(summary, IR_SUMMARY_MAY_TRAP, at, trapper,
                      (core_string_t){0});
        }
    }
    adjust_flags(summaries, summary);
}

static bool
path_touches(flow_query_t *q, const ir_summary_t *s, size_t index,
             const ir_splace_t *place)
{
    ir_splace_t places[2];
    uint32_t    count = query_path_places(q, s, index, places);
    for (uint32_t p = 0; p < count; p++)
        if (named_place(&places[p]) && ir_splace_eq(&places[p], place))
            return true;
    return false;
}

static uint8_t
must_write(flow_query_t *q, ir_func_t *f, const ir_splace_t *target,
           const ir_spath_t *leaf)
{
    ir_path_exits_t exits;
    q->target = target;
    q->leaf   = leaf;
    if (!ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, q, &exits)) return 0;
    uint8_t unpassed = unpassed_exits(q, f, &exits);
    return (unpassed & UNPASSED_RETURN ? 0 : IR_SPATH_MUST_SUCCESS)
        | (unpassed & UNPASSED_FAIL ? 0 : IR_SPATH_MUST_FAILURE);
}

static void
summarize_writes(ir_summaries_t *summaries, ir_func_t *f, pt_t *pt,
                 ir_summary_t *summary)
{
    core_dict_t  cache, stores, effects;
    core_dict_init(&cache, nullptr, nullptr);
    core_dict_init(&stores, nullptr, nullptr);
    core_dict_init(&effects, nullptr, nullptr);
    flow_query_t q = {.summaries = summaries, .f = f, .pt = pt,
                      .path_places = &cache, .stores = &stores,
                      .effects = &effects};
    for (size_t i = 0; i < summary->paths.len; i++) {
        ir_splace_t targets[2];
        uint32_t    count = summary->paths.data[i].flags & IR_SPATH_WRITE
            ? query_path_places(&q, summary, i, targets) : 0;
        for (uint32_t t = 0; t < count; t++) {
            bool seen = !named_place(&targets[t]);
            for (size_t j = 0; !seen && j < i; j++)
                seen = (summary->paths.data[j].flags & IR_SPATH_WRITE)
                    && path_touches(&q, summary, j, &targets[t]);
            uint8_t must = seen ? 0
                                : must_write(&q, f, &targets[t], nullptr);
            for (size_t j = i; must && j < summary->paths.len; j++)
                if ((summary->paths.data[j].flags & IR_SPATH_WRITE)
                    && path_touches(&q, summary, j, &targets[t]))
                    summary->paths.data[j].flags |= must;
        }
    }
    for (size_t i = 0; i < summary->paths.len; i++) {
        ir_spath_t *path = &summary->paths.data[i];
        ir_splace_t targets[2];
        uint32_t    count = (path->flags & IR_SPATH_WRITE) && path->depth
                && !(path->flags & IR_SPATH_DEEP)
                && (path->flags & IR_SPATH_MUST) != IR_SPATH_MUST
            ? query_path_places(&q, summary, i, targets) : 0;
        for (uint32_t t = 0; t < count; t++)
            if (named_place(&targets[t]))
                path->flags |= must_write(&q, f, &targets[t], path);
    }
    path_places_free(&cache);
    core_dict_foreach(&stores, entry)
        if (entry->value) core_free(entry->value);
    core_dict_free(&stores);
    core_dict_free(&effects);
}

static uint8_t
divergence_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    return is_call(in) && ir_summary_call_diverges(context, in)
        ? 0 : IR_PATH_UNPASSED;
}

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
    if ((int32_t)operand == ir_op_source_operand(use->op))
        return IR_SFIELD_READ;
    if ((int32_t)operand != ir_op_address_operand(use->op))
        return IR_SFIELD_READ | IR_SFIELD_WRITE;
    uint8_t access = ir_op_memory_access(use->op);
    return (access & IR_MEM_READ ? IR_SFIELD_READ : 0)
         | (access & IR_MEM_WRITE ? IR_SFIELD_WRITE : 0);
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
