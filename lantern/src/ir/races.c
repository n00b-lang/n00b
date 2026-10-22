#include "ir/races.h"

#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/places.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef ir_func_t *race_func_ptr_t;
core_list_decl(race_func_ptr_t);

typedef struct {
    core_list_t(race_func_ptr_t) functions;
    core_dict_t                  index;
} race_reach_t;

typedef struct {
    race_reach_t reach;
    core_dict_t  places;
} race_source_t;

typedef struct {
    core_dict_t *sharers;
    core_dict_t *own;
    bool         alone;
} race_others_t;

static void
add_function(race_reach_t *reach, ir_func_t *function)
{
    if (function && function->n_blocks
        && core_dict_add(&reach->index, function, reach->functions.len + 1))
        core_list_push(reach->functions, function);
}

static size_t
reach_slot(race_reach_t *reach, const ir_func_t *function)
{
    return function ? (uintptr_t)core_dict_get(&reach->index, function,
                                               nullptr)
                    : 0;
}

static void
reach_callee(void *context, const ir_func_t *target,
             const ir_summary_t *summary)
{
    (void)summary;
    add_function(context, (ir_func_t *)target);
}

static race_reach_t
reach_from(ir_summaries_t *summaries, ir_func_t *const *roots, size_t count)
{
    race_reach_t reach = {.functions = core_list_new(race_func_ptr_t)};
    core_dict_init(&reach.index, nullptr, nullptr);
    for (size_t r = 0; r < count; r++) add_function(&reach, roots[r]);
    for (size_t at = 0; at < reach.functions.len; at++) {
        ir_func_t *function = reach.functions.data[at];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                    ir_summary_call_callees(summaries, in, reach_callee,
                                            &reach);
            }
    }
    return reach;
}

static bool
shared_place(const ir_splace_t *place)
{
    return place->kind == IR_SPLACE_GLOBAL && place->global;
}

static void
add_place(core_dict_t *places, const ir_splace_t *place,
          const ir_race_hooks_t *hooks)
{
    if (!shared_place(place)
        || core_dict_contains(places, (void *)place->global))
        return;
    core_dict_put(places, place->global,
                  !(hooks->atomic
                    && hooks->atomic(hooks->context, nullptr, place)));
}

static race_source_t
source_of(ir_summaries_t *summaries, ir_func_t *const *roots, size_t count,
          const ir_race_entry_t *entry, const ir_race_hooks_t *hooks)
{
    race_source_t source = {.reach = reach_from(summaries, roots, count)};
    core_dict_init(&source.places, nullptr, nullptr);
    for (size_t f = 0; f < source.reach.functions.len; f++) {
        ir_func_t    *function = source.reach.functions.data[f];
        ir_summary_t *summary  = ir_summary_of(summaries, function);
        for (uint32_t a = 0; a < ir_summary_access_count(summary); a++) {
            const ir_saccess_t *access = ir_summary_access_at(summary, a);
            if (access->place.kind != IR_SPLACE_PARAM) {
                add_place(&source.places, &access->place, hooks);
                continue;
            }
            if (!entry || function != entry->entry) continue;
            for (uint32_t g = 0; g < entry->argument_count; g++)
                add_place(&source.places, &entry->arguments[g], hooks);
        }
    }
    return source;
}

static void
source_free(race_source_t *source)
{
    core_list_free(source->reach.functions);
    core_dict_free(&source->reach.index);
    core_dict_free(&source->places);
}

static void
share(core_dict_t *sharers, core_dict_t *places)
{
    core_dict_foreach(places, item) {
        if (!item->value) continue;
        uintptr_t count = (uintptr_t)core_dict_get(sharers, item->key, nullptr);
        core_dict_put(sharers, item->key, count + 1);
    }
}

static bool
conflicts(const race_others_t *others, const ir_splace_t *place)
{
    if (!shared_place(place)) return false;
    uintptr_t count = (uintptr_t)core_dict_get(others->sharers, place->global,
                                               nullptr);
    if (others->alone
        && core_dict_get(others->own, place->global, nullptr))
        count--;
    return count > 0;
}

static bool
held_on_entry(ir_func_t *function, ir_doms_t *doms, const bool *blocks,
              bool at_entry, ir_block_t *block)
{
    bool held = block == ir_block_entry(function) ? at_entry : true;
    for (uint32_t p = 0; doms && p < ir_pred_count(doms, block); p++)
        held = held && blocks[ir_pred_at(doms, block, p)->index];
    return held;
}

static ir_lock_role_t
lock_role(const ir_race_hooks_t *hooks, const ir_instr_t *in)
{
    return hooks->lock_role ? hooks->lock_role(hooks->context, in)
                            : IR_LOCK_NONE;
}

static bool
after_role(bool held, ir_lock_role_t role)
{
    return role == IR_LOCK_ACQUIRE || role == IR_LOCK_RELEASE
        ? role == IR_LOCK_ACQUIRE
        : held;
}

typedef void (*race_visit_fn)(void *context, ir_instr_t *in, bool held,
                              ir_lock_role_t role);

static void
walk_held(ir_func_t *function, bool at_entry, const ir_race_hooks_t *hooks,
          race_visit_fn visit, void *context)
{
    ir_doms_t *doms   = ir_func_doms(function);
    bool      *blocks = core_alloc_array(bool, function->n_blocks + 1);
    for (uint32_t b = 0; b < function->n_blocks; b++) blocks[b] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            bool held = held_on_entry(function, doms, blocks, at_entry, block);
            for (uint32_t i = 0; hooks->lock_role && i < block->n_instrs; i++)
                held = after_role(held, lock_role(hooks, block->instrs[i]));
            if (blocks[b] != held) {
                blocks[b] = held;
                changed   = true;
            }
        }
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        bool held = held_on_entry(function, doms, blocks, at_entry, block);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_lock_role_t role = lock_role(hooks, block->instrs[i]);
            visit(context, block->instrs[i], held, role);
            held = after_role(held, role);
        }
    }
    core_free(blocks);
}

typedef struct {
    race_reach_t          *reach;
    bool                  *held;
    const ir_race_hooks_t *hooks;
    bool                   changed;
} lock_walk_t;

static void
release_callee(void *context, ir_instr_t *in, bool held, ir_lock_role_t role)
{
    lock_walk_t *walk = context;
    if (held || role != IR_LOCK_NONE || !walk->hooks->direct_target) return;
    size_t slot = reach_slot(walk->reach,
        walk->hooks->direct_target(walk->hooks->context, in));
    if (!slot || !walk->held[slot - 1]) return;
    walk->held[slot - 1] = false;
    walk->changed        = true;
}

static bool *
solve_locks(race_reach_t *reach, const ir_func_t *entry,
            const ir_race_hooks_t *hooks)
{
    bool *held = core_alloc_array(bool, reach->functions.len + 1);
    for (size_t f = 0; f < reach->functions.len; f++)
        held[f] = reach->functions.data[f] != entry;
    lock_walk_t walk = {.reach = reach, .held = held, .hooks = hooks};
    for (walk.changed = true; walk.changed;) {
        walk.changed = false;
        for (size_t f = 0; f < reach->functions.len; f++) {
            size_t scope = ir_analysis_scope_begin();
            walk_held(reach->functions.data[f], held[f], hooks,
                      release_callee, &walk);
            ir_analysis_scope_end(scope);
        }
    }
    return held;
}

static void
note_unheld(void *context, ir_instr_t *in, bool held, ir_lock_role_t role)
{
    (void)role;
    if (!held) core_dict_put(context, in, 1);
}

static bool
written_place(ir_func_t *function, ir_places_t *places,
              const ir_access_t *access, const ir_race_entry_t *entry,
              const race_others_t *others, const ir_race_hooks_t *hooks,
              ir_splace_t *out)
{
    const uint32_t *ids = ir_places_access_places(places, access);
    for (uint32_t p = 0; p < access->count; p++) {
        ir_splace_t place;
        if (!ir_summary_place_of(function, ir_place_at(places, ids[p]),
                                 &place)) continue;
        if (place.kind == IR_SPLACE_PARAM) {
            for (uint32_t g = 0; function == entry->entry
                 && g < entry->argument_count; g++)
                if (conflicts(others, &entry->arguments[g])
                    && !(hooks->atomic && hooks->atomic(hooks->context,
                         access->instr, &entry->arguments[g]))) {
                    *out = entry->arguments[g];
                    return true;
                }
            continue;
        }
        if (conflicts(others, &place)
            && !(hooks->atomic
                 && hooks->atomic(hooks->context, access->instr, &place))) {
            *out = place;
            return true;
        }
    }
    return false;
}

typedef struct {
    ir_func_t             *function;
    const ir_instr_t      *call;
    ir_places_t           *places;
    const ir_race_entry_t *entry;
    const race_others_t   *others;
    const ir_race_hooks_t *hooks;
    bool                   found;
    ir_splace_t            place;
} call_write_t;

static bool
argument_conflicts(call_write_t *walk, const ir_value_t *argument)
{
    const ir_value_t *const *params = (const ir_value_t *const *)
        walk->function->params;
    for (uint32_t p = 0; walk->function == walk->entry->entry
         && p < walk->function->n_params; p++)
        if (params[p] == argument)
            for (uint32_t g = 0; g < walk->entry->argument_count; g++)
                if (conflicts(walk->others, &walk->entry->arguments[g])) {
                    walk->place = walk->entry->arguments[g];
                    return true;
                }
    const uint32_t *ids   = nullptr;
    uint32_t        count = 0;
    if (!ir_places_of(walk->places, argument, &ids, &count)) return false;
    for (uint32_t i = 0; i < count; i++) {
        ir_splace_t place;
        if (ir_summary_place_of(walk->function,
                                ir_place_at(walk->places, ids[i]), &place)
            && conflicts(walk->others, &place)) {
            walk->place = place;
            return true;
        }
    }
    return false;
}

static void
callee_writes(void *context, const ir_func_t *target,
              const ir_summary_t *summary)
{
    (void)target;
    call_write_t *walk = context;
    for (uint32_t a = 0; !walk->found && a < ir_summary_access_count(summary);
         a++) {
        const ir_saccess_t *access = ir_summary_access_at(summary, a);
        uint32_t operand = access->place.param + 1;
        if (!(access->flags & IR_SACCESS_WRITE)
            || access->place.kind != IR_SPLACE_PARAM
            || operand >= walk->call->n_operands) continue;
        walk->found = argument_conflicts(walk,
            ir_operand_value(&walk->call->operands[operand]));
    }
    if (walk->found && walk->hooks->atomic
        && walk->hooks->atomic(walk->hooks->context, walk->call,
                               &walk->place))
        walk->found = false;
}

static void
function_races(ir_summaries_t *summaries, ir_func_t *function, bool at_entry,
               const ir_race_entry_t *entry, const race_others_t *others,
               const ir_race_hooks_t *hooks, ir_race_report_fn report,
               void *context)
{
    ir_places_t *places = ir_func_places(function);
    if (!places) return;
    core_dict_t unheld;
    core_dict_init(&unheld, nullptr, nullptr);
    walk_held(function, at_entry, hooks, note_unheld, &unheld);
    for (uint32_t a = 0; a < ir_places_access_count(places); a++) {
        const ir_access_t *access = ir_places_access_at(places, a);
        ir_splace_t place;
        if (access->kind != IR_ACCESS_STORE
            || !core_dict_contains(&unheld, (void *)access->instr)
            || !written_place(function, places, access, entry, others, hooks,
                              &place)) continue;
        report(context, &(ir_race_t){
            .entry = entry->entry, .site = access->instr, .place = place,
            .self = entry->starts > 1,
        });
    }
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            if ((in->op != IR_OP_CALL && in->op != IR_OP_CALLBACK_INVOKE)
                || lock_role(hooks, in) != IR_LOCK_NONE
                || !core_dict_contains(&unheld, in)) continue;
            call_write_t walk = {
                .function = function, .call = in, .places = places,
                .entry = entry, .others = others, .hooks = hooks,
            };
            ir_summary_call_callees(summaries, in, callee_writes, &walk);
            if (walk.found)
                report(context, &(ir_race_t){
                    .entry = entry->entry, .site = in, .place = walk.place,
                    .self = entry->starts > 1,
                });
        }
    core_dict_free(&unheld);
}

void
ir_races_find(ir_summaries_t *summaries, ir_func_t *const *main_roots,
              size_t main_count, const ir_race_entry_t *entries,
              size_t entry_count, const ir_race_hooks_t *hooks,
              ir_race_report_fn report, void *context)
{
    race_source_t  main    = source_of(summaries, main_roots, main_count,
                                       nullptr, hooks);
    race_source_t *threads = core_alloc_array(race_source_t, entry_count + 1);
    core_dict_t    sharers;
    core_dict_init(&sharers, nullptr, nullptr);
    share(&sharers, &main.places);
    for (size_t e = 0; e < entry_count; e++) {
        threads[e] = source_of(summaries, &entries[e].entry, 1, &entries[e],
                               hooks);
        share(&sharers, &threads[e].places);
    }
    for (size_t e = 0; e < entry_count; e++) {
        race_reach_t *reach  = &threads[e].reach;
        race_others_t others = {
            .sharers = &sharers, .own = &threads[e].places,
            .alone = entries[e].starts < 2,
        };
        bool *held = solve_locks(reach, entries[e].entry, hooks);
        for (size_t f = 0; f < reach->functions.len; f++) {
            size_t scope = ir_analysis_scope_begin();
            function_races(summaries, reach->functions.data[f], held[f],
                           &entries[e], &others, hooks, report, context);
            ir_analysis_scope_end(scope);
        }
        core_free(held);
    }
    for (size_t e = 0; e < entry_count; e++) source_free(&threads[e]);
    core_free(threads);
    core_dict_free(&sharers);
    source_free(&main);
}
