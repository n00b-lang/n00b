#include "ir/races.h"

#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/places.h"
#include "lib/alloc.h"
#include "lib/list.h"

typedef ir_func_t *race_func_ptr_t;
core_list_decl(race_func_ptr_t);

typedef struct {
    ir_splace_t place;
    bool        write;
} race_place_t;
core_list_decl(race_place_t);

typedef struct {
    ir_func_t *function;
    bool       held;
} race_lock_t;
core_list_decl(race_lock_t);

static bool
contains(const core_list_t(race_func_ptr_t) *list, const ir_func_t *function)
{
    for (size_t i = 0; i < list->len; i++)
        if (list->data[i] == function) return true;
    return false;
}

static void
add_function(core_list_t(race_func_ptr_t) *list, ir_func_t *function)
{
    if (function && function->n_blocks && !contains(list, function))
        core_list_push(*list, function);
}

static void
reach_callee(void *context, const ir_func_t *target,
             const ir_summary_t *summary)
{
    (void)summary;
    add_function(context, (ir_func_t *)target);
}

static core_list_t(race_func_ptr_t)
reach_from(ir_summaries_t *summaries, ir_func_t *const *roots, size_t count)
{
    core_list_t(race_func_ptr_t) reached = core_list_new(race_func_ptr_t);
    for (size_t r = 0; r < count; r++) add_function(&reached, roots[r]);
    for (size_t at = 0; at < reached.len; at++) {
        ir_func_t *function = reached.data[at];
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                    ir_summary_call_callees(summaries, in, reach_callee,
                                            &reached);
            }
    }
    return reached;
}

static bool
shared_place(const ir_splace_t *place)
{
    return place->kind == IR_SPLACE_GLOBAL && place->global;
}

static void
add_place(core_list_t(race_place_t) *places, const ir_splace_t *place,
          bool write, const ir_race_hooks_t *hooks)
{
    if (!shared_place(place)
        || (hooks->atomic && hooks->atomic(hooks->context, nullptr, place)))
        return;
    for (size_t i = 0; i < places->len; i++)
        if (ir_splace_eq(&places->data[i].place, place)) {
            places->data[i].write |= write;
            return;
        }
    core_list_push(*places, ((race_place_t){.place = *place, .write = write}));
}

static void
collect_places(ir_summaries_t *summaries,
               const core_list_t(race_func_ptr_t) *reach,
               const ir_race_entry_t *entry, const ir_race_hooks_t *hooks,
               core_list_t(race_place_t) *out)
{
    for (size_t f = 0; f < reach->len; f++) {
        ir_summary_t *summary = ir_summary_of(summaries, reach->data[f]);
        for (uint32_t a = 0; a < ir_summary_access_count(summary); a++) {
            const ir_saccess_t *access = ir_summary_access_at(summary, a);
            bool write = (access->flags & IR_SACCESS_WRITE) != 0;
            if (access->place.kind != IR_SPLACE_PARAM) {
                add_place(out, &access->place, write, hooks);
                continue;
            }
            if (!entry || reach->data[f] != entry->entry) continue;
            for (uint32_t g = 0; g < entry->argument_count; g++)
                add_place(out, &entry->arguments[g], write, hooks);
        }
    }
}

static bool
conflicts(const core_list_t(race_place_t) *others, const ir_splace_t *place)
{
    for (size_t i = 0; i < others->len; i++)
        if (ir_splace_eq(&others->data[i].place, place)) return true;
    return false;
}

static bool *
held_blocks(ir_func_t *function, bool at_entry, const ir_race_hooks_t *hooks)
{
    bool      *out  = core_alloc_array(bool, function->n_blocks + 1);
    ir_doms_t *doms = ir_func_doms(function);
    for (uint32_t b = 0; b < function->n_blocks; b++) out[b] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            bool held = block == ir_block_entry(function) ? at_entry : true;
            for (uint32_t p = 0; doms && p < ir_pred_count(doms, block); p++)
                held = held && out[ir_pred_at(doms, block, p)->index];
            for (uint32_t i = 0; hooks->lock_role && i < block->n_instrs;
                 i++) {
                ir_lock_role_t role = hooks->lock_role(hooks->context,
                                                       block->instrs[i]);
                if (role == IR_LOCK_ACQUIRE || role == IR_LOCK_RELEASE)
                    held = role == IR_LOCK_ACQUIRE;
            }
            if (out[b] != held) {
                out[b]  = held;
                changed = true;
            }
        }
    }
    return out;
}

static bool
held_before(ir_func_t *function, const bool *blocks, bool at_entry,
            const ir_instr_t *target, const ir_race_hooks_t *hooks)
{
    ir_doms_t  *doms  = ir_func_doms(function);
    ir_block_t *block = target->block;
    bool held = block == ir_block_entry(function) ? at_entry : true;
    for (uint32_t p = 0; doms && p < ir_pred_count(doms, block); p++)
        held = held && blocks[ir_pred_at(doms, block, p)->index];
    for (uint32_t i = 0; hooks->lock_role && i < block->n_instrs
         && block->instrs[i] != target; i++) {
        ir_lock_role_t role = hooks->lock_role(hooks->context,
                                               block->instrs[i]);
        if (role == IR_LOCK_ACQUIRE || role == IR_LOCK_RELEASE)
            held = role == IR_LOCK_ACQUIRE;
    }
    return held;
}

static bool *
entry_held(core_list_t(race_lock_t) *locks, const ir_func_t *function)
{
    for (size_t i = 0; function && i < locks->len; i++)
        if (locks->data[i].function == function) return &locks->data[i].held;
    return nullptr;
}

static void
solve_locks(core_list_t(race_lock_t) *locks,
            const core_list_t(race_func_ptr_t) *reach, const ir_func_t *entry,
            const ir_race_hooks_t *hooks)
{
    for (size_t f = 0; f < reach->len; f++)
        core_list_push(*locks, ((race_lock_t){reach->data[f],
                                               reach->data[f] != entry}));
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t f = 0; f < reach->len; f++) {
            ir_func_t *function = reach->data[f];
            size_t     scope    = ir_analysis_scope_begin();
            bool       at_entry = *entry_held(locks, function);
            bool      *blocks   = held_blocks(function, at_entry, hooks);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs;
                     i++) {
                    ir_instr_t *in     = function->blocks[b]->instrs[i];
                    bool       *callee = hooks->direct_target
                        ? entry_held(locks,
                                     hooks->direct_target(hooks->context, in))
                        : nullptr;
                    if (callee && *callee
                        && (!hooks->lock_role
                            || hooks->lock_role(hooks->context, in)
                                == IR_LOCK_NONE)
                        && !held_before(function, blocks, at_entry, in,
                                        hooks)) {
                        *callee = false;
                        changed = true;
                    }
                }
            core_free(blocks);
            ir_analysis_scope_end(scope);
        }
    }
}

static bool
written_place(ir_func_t *function, ir_places_t *places,
              const ir_access_t *access, const ir_race_entry_t *entry,
              const core_list_t(race_place_t) *others,
              const ir_race_hooks_t *hooks, ir_splace_t *out)
{
    const uint32_t *ids = ir_places_access_places(places, access);
    for (uint32_t p = 0; p < access->count; p++) {
        ir_splace_t place;
        if (!ir_summary_place_of(function, ir_place_at(places, ids[p]),
                                 &place)) continue;
        if (place.kind == IR_SPLACE_PARAM) {
            for (uint32_t g = 0; function == entry->entry
                 && g < entry->argument_count; g++)
                if (shared_place(&entry->arguments[g])
                    && conflicts(others, &entry->arguments[g])
                    && !(hooks->atomic && hooks->atomic(hooks->context,
                         access->instr, &entry->arguments[g]))) {
                    *out = entry->arguments[g];
                    return true;
                }
            continue;
        }
        if (shared_place(&place) && conflicts(others, &place)
            && !(hooks->atomic
                 && hooks->atomic(hooks->context, access->instr, &place))) {
            *out = place;
            return true;
        }
    }
    return false;
}

typedef struct {
    ir_func_t                       *function;
    const ir_instr_t                *call;
    ir_places_t                     *places;
    const ir_race_entry_t           *entry;
    const core_list_t(race_place_t) *others;
    const ir_race_hooks_t           *hooks;
    bool                             found;
    ir_splace_t                      place;
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
                if (shared_place(&walk->entry->arguments[g])
                    && conflicts(walk->others, &walk->entry->arguments[g])) {
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
            && shared_place(&place) && conflicts(walk->others, &place)) {
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

uint32_t
ir_races_find(ir_summaries_t *summaries, ir_func_t *const *main_roots,
              size_t main_count, const ir_race_entry_t *entries,
              size_t entry_count, const ir_race_hooks_t *hooks,
              ir_race_t *out, uint32_t capacity)
{
    uint32_t count = 0;
    core_list_t(race_func_ptr_t) main_reach = reach_from(summaries,
        main_roots, main_count);
    for (size_t e = 0; e < entry_count && count < capacity; e++) {
        const ir_race_entry_t *entry = &entries[e];
        core_list_t(race_func_ptr_t) threaded = reach_from(summaries,
            &entry->entry, 1);
        core_list_t(race_place_t) others = core_list_new(race_place_t);
        collect_places(summaries, &main_reach, nullptr, hooks, &others);
        for (size_t o = 0; o < entry_count; o++) {
            if (o == e && entries[o].starts < 2) continue;
            core_list_t(race_func_ptr_t) other = reach_from(summaries,
                &entries[o].entry, 1);
            collect_places(summaries, &other, &entries[o], hooks, &others);
            core_list_free(other);
        }
        core_list_t(race_lock_t) locks = core_list_new(race_lock_t);
        solve_locks(&locks, &threaded, entry->entry, hooks);
        for (size_t f = 0; f < threaded.len && count < capacity; f++) {
            ir_func_t   *function = threaded.data[f];
            size_t       scope    = ir_analysis_scope_begin();
            ir_places_t *places   = ir_func_places(function);
            bool         at_entry = *entry_held(&locks, function);
            bool        *blocks   = held_blocks(function, at_entry, hooks);
            for (uint32_t a = 0; places && count < capacity
                 && a < ir_places_access_count(places); a++) {
                const ir_access_t *access = ir_places_access_at(places, a);
                ir_splace_t place;
                if (access->kind != IR_ACCESS_STORE
                    || held_before(function, blocks, at_entry, access->instr,
                                   hooks)
                    || !written_place(function, places, access, entry,
                                      &others, hooks, &place)) continue;
                out[count++] = (ir_race_t){
                    .entry = entry->entry, .site = access->instr,
                    .place = place, .self = entry->starts > 1,
                };
            }
            for (uint32_t b = 0; places && count < capacity
                 && b < function->n_blocks; b++)
                for (uint32_t i = 0; count < capacity
                     && i < function->blocks[b]->n_instrs; i++) {
                    ir_instr_t *in = function->blocks[b]->instrs[i];
                    if ((in->op != IR_OP_CALL
                         && in->op != IR_OP_CALLBACK_INVOKE)
                        || (hooks->lock_role
                            && hooks->lock_role(hooks->context, in)
                                != IR_LOCK_NONE)
                        || held_before(function, blocks, at_entry, in,
                                       hooks)) continue;
                    call_write_t walk = {
                        .function = function, .call = in, .places = places,
                        .entry = entry, .others = &others, .hooks = hooks,
                    };
                    ir_summary_call_callees(summaries, in, callee_writes,
                                            &walk);
                    if (walk.found)
                        out[count++] = (ir_race_t){
                            .entry = entry->entry, .site = in,
                            .place = walk.place, .self = entry->starts > 1,
                        };
                }
            core_free(blocks);
            ir_analysis_scope_end(scope);
        }
        core_list_free(locks);
        core_list_free(others);
        core_list_free(threaded);
    }
    core_list_free(main_reach);
    return count;
}
