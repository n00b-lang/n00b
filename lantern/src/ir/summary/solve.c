#include "internal.h"

typedef struct {
    ir_summaries_t                *summaries;
    core_dict_t                    index;
    core_dict_t                    lowlink;
    core_dict_t                    on_stack;
    core_list_t(summary_func_ptr_t) stack;
    uint32_t                       counter;
} tarjan_t;

typedef core_list_t(uint32_t) member_callers_t;

typedef struct {
    ir_func_t                      *f;
    core_list_t(summary_func_ptr_t) callees;
    size_t                          next;
} frame_t;
core_list_decl(frame_t);

static void direct_callees(ir_summaries_t *summaries, ir_func_t *f,
                           core_list_t(summary_func_ptr_t) *out);

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

static member_callers_t *
member_callers(ir_summaries_t *summaries, ir_func_t *const *members,
               size_t count)
{
    member_callers_t *callers = core_alloc_array(member_callers_t, count);
    core_dict_t       index;
    core_dict_init(&index, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) {
        callers[i] = core_list_new(uint32_t);
        core_dict_put(&index, members[i], (void *)(uintptr_t)(i + 1));
    }
    for (size_t i = 0; i < count; i++) {
        core_list_t(summary_func_ptr_t) callees =
            core_list_new(summary_func_ptr_t);
        direct_callees(summaries, members[i], &callees);
        for (size_t c = 0; c < callees.len; c++) {
            bool      found = false;
            uintptr_t at    = (uintptr_t)core_dict_get(&index,
                                                       callees.data[c],
                                                       &found);
            member_callers_t *list = found ? &callers[at - 1] : nullptr;
            bool seen = !list;
            for (size_t k = 0; !seen && k < list->len; k++)
                seen = list->data[k] == i;
            if (!seen) core_list_push(*list, (uint32_t)i);
        }
        core_list_free(callees);
    }
    core_dict_free(&index);
    return callers;
}

static bool
witness_key(const ir_summary_t *summary, uint32_t key)
{
    return key == IR_SWITNESS_UNKNOWN ? exports_unknown(summary)
                                      : (summary->flags & key) != 0;
}

static void
set_witness(ir_summary_t *summary, const ir_switness_t *witness)
{
    for (size_t i = 0; i < summary->witnesses.len; i++)
        if (summary->witnesses.data[i].flag == witness->flag) {
            summary->witnesses.data[i] = *witness;
            return;
        }
    core_list_push(summary->witnesses, *witness);
}

static size_t
member_index(ir_func_t *const *members, size_t count, const ir_func_t *f)
{
    for (size_t i = 0; i < count; i++)
        if (members[i] == f) return i;
    return count;
}

static bool
witness_through(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count, const bool *done, uint32_t key, size_t at)
{
    ir_func_t    *f       = members[at];
    ir_summary_t *summary = ir_summary_of(summaries, f);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (!is_call(in)) continue;
            call_targets_t targets;
            resolve_targets(summaries, in, &targets);
            ir_func_t *found = nullptr;
            for (size_t t = 0; !found && t < targets.targets.len; t++) {
                ir_func_t *target = targets.targets.data[t];
                size_t     member = member_index(members, count, target);
                if (member < count && !done[member]) continue;
                if (witness_key(member < count
                                    ? ir_summary_of(summaries, target)
                                    : target_summary(summaries, target),
                                key))
                    found = target;
            }
            core_list_free(targets.targets);
            if (!found) continue;
            set_witness(summary, &(ir_switness_t){
                .flag   = key,
                .callee = found,
                .module = ir_module_name(found->owner),
                .name   = found->name,
                .line   = in->span.start_line,
                .column = in->span.start_col,
            });
            return true;
        }
    return false;
}

static void
ground_key(ir_summaries_t *summaries, ir_func_t *const *members,
           size_t count, uint32_t key, bool *done)
{
    bool pending = false;
    for (size_t i = 0; i < count; i++) {
        const ir_summary_t  *summary = ir_summary_of(summaries, members[i]);
        const ir_switness_t *witness = ir_summary_witness_for(summary, key);
        done[i] = !witness_key(summary, key)
            || (witness && (!witness->callee
                            || member_index(members, count, witness->callee)
                                   == count));
        pending |= !done[i];
    }
    for (bool changed = pending; changed;) {
        changed = false;
        for (size_t i = 0; i < count; i++)
            if (!done[i]
                && witness_through(summaries, members, count, done, key, i))
                changed = done[i] = true;
    }
}

static void
ground_witnesses(ir_summaries_t *summaries, ir_func_t *const *members,
                 size_t count)
{
    bool *done = core_alloc_array(bool, count);
    for (uint32_t b = 0; b <= IR_SEFFECT_FLAG_BITS; b++)
        ground_key(summaries, members, count,
                   b == IR_SEFFECT_FLAG_BITS ? IR_SWITNESS_UNKNOWN : 1u << b,
                   done);
    core_free(done);
}

static void
join_rounds(ir_summary_t *next, const ir_summary_t *previous)
{
    uint32_t shrinking = IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN;
    next->flags = ((next->flags | previous->flags) & ~shrinking)
                | (next->flags & previous->flags & shrinking);
    next->traps |= previous->traps;
    next->returns_external |= previous->returns_external;
    merge_effects(next, previous);
    join_flows(&next->flows[IR_SFLOW_RESULTS],
               &previous->flows[IR_SFLOW_RESULTS]);
    join_flows(&next->flows[IR_SFLOW_STORES],
               &previous->flows[IR_SFLOW_STORES]);
}


static void
solve_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *summary = ir_summary_new(summaries, members[i]);
        summary->flags = IR_SUMMARY_NO_RETURN | IR_SUMMARY_DIVERGES;
        summary->results_known = true;
        core_dict_put(&summaries->by_function, members[i], summary);
    }
    member_callers_t *callers = member_callers(summaries, members, count);
    bool cyclic = count > 1 || callers[0].len;
    uint32_t  shrinking = IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN;
    uint32_t *settled   = core_alloc_array(uint32_t, count);
    for (size_t i = 0; i < count; i++) settled[i] = shrinking;
    for (bool changed = cyclic && !summaries->escapes_only; changed;) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *probe   = ir_summary_new(nullptr, members[i]);
            uint32_t      before  = summary->flags & shrinking;
            summarize_divergence(summaries, probe, members[i]);
            uint32_t after = probe->flags & before;
            summary_free(probe);
            summary->flags = (summary->flags & ~shrinking) | after;
            settled[i]     = after;
            changed |= after != before;
        }
    }
    bool *dirty = core_alloc_array(bool, count);
    bool *next_dirty = core_alloc_array(bool, count);
    pt_t **engines = cyclic ? core_alloc_array(pt_t *, count) : nullptr;
    for (size_t i = 0; i < count; i++) dirty[i] = true;
    for (size_t i = 0; engines && i < count; i++) engines[i] = nullptr;
    size_t component_scope = engines ? ir_analysis_scope_begin() : 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            if (!dirty[i]) continue;
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *next    = ir_summary_new(nullptr, members[i]);
            size_t scope = engines ? 0 : ir_analysis_scope_begin();
            if (summaries->escapes_only)
                summarize_divergence(summaries, next, members[i]);
            else summarize_function(summaries, next, members[i],
                                    cyclic ? &settled[i] : nullptr);
            summarize_paths(summaries, next, members[i],
                            engines ? &engines[i] : nullptr);
            if (!engines) ir_analysis_scope_end(scope);
            if (cyclic) join_rounds(next, summary);
            prune_flows(&next->flows[IR_SFLOW_RESULTS]);
            prune_flows(&next->flows[IR_SFLOW_STORES]);
            bool differs = cyclic
                && (next->flags != summary->flags
                    || next->traps != summary->traps
                    || !same_effects(next, summary)
                    || !same_paths(next, summary));
            for (size_t c = 0; differs && c < callers[i].len; c++) {
                uint32_t caller = callers[i].data[c];
                if (caller <= i || !dirty[caller]) next_dirty[caller] = true;
            }
            changed |= differs;
            ir_summary_t previous = *summary;
            *summary = *next;
            *next    = previous;
            summary_free(next);
        }
        if (!cyclic) break;
        bool *swap = dirty;
        dirty      = next_dirty;
        next_dirty = swap;
        for (size_t i = 0; i < count; i++) next_dirty[i] = false;
    }
    for (size_t i = 0; i < count; i++) core_list_free(callers[i]);
    core_free(callers);
    for (size_t i = 0; engines && i < count; i++)
        if (engines[i]) {
            pt_free(engines[i]);
            core_free(engines[i]);
        }
    if (engines) ir_analysis_scope_end(component_scope);
    core_free(engines);
    core_free(dirty);
    core_free(next_dirty);
    core_free(settled);
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
    if (!summaries->escapes_only) ground_witnesses(summaries, members, count);
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
    core_dict_init(&summaries->type_memos, nullptr, nullptr);
    core_dict_init(&summaries->engines, nullptr, nullptr);
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
    type_memos_free(&summaries->type_memos);
    core_dict_foreach(&summaries->engines, entry) {
        engine_release(entry->value);
        core_free(entry->value);
    }
    core_dict_free(&summaries->engines);
    core_free(summaries);
}
