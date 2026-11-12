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
static uint64_t points_to_content(const ir_summary_t *summary);
static bool witness_from_calls(ir_summaries_t *summaries,
                               ir_func_t *const *members, size_t count,
                               const bool *done, uint32_t key, ir_func_t *f,
                               ir_summary_t *summary);

static void
summarize_fields(ir_summaries_t *summaries, ir_summary_t *summary,
                 ir_func_t *f)
{
    core_dict_t pointers;
    core_dict_init(&pointers, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->op == IR_OP_COPY && in->dest && in->n_operands == 1) {
                bool        found = false;
                ir_instr_t *field = core_dict_get(&pointers,
                    ir_operand_value(&in->operands[0]), &found);
                if (found) core_dict_put(&pointers, in->dest, field);
                continue;
            }
            if (in->n_operands < 2 || in->operands[1].kind != IR_OPD_NAME)
                continue;
            const ir_value_t *base = ir_operand_value(&in->operands[0]);
            if (in->op == IR_OP_FIELD && in->dest) {
                if (record_or_referent(f, base))
                    core_dict_put(&pointers, in->dest, in);
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
                if (found && !(in->op == IR_OP_COPY && in->dest
                               && core_dict_contains(&pointers, in->dest)))
                    add_field(summary, record_or_referent(f,
                                  ir_operand_value(&field->operands[0])),
                              field->operands[1].name,
                              field_pointer_use(in, o));
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
    summaries->recording = summary;
    bool found_witness = witness_from_calls(summaries, members, count, done,
                                            key, f, summary);
    summaries->recording = nullptr;
    return found_witness;
}

static bool
witness_from_calls(ir_summaries_t *summaries, ir_func_t *const *members,
                   size_t count, const bool *done, uint32_t key,
                   ir_func_t *f, ir_summary_t *summary)
{
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
    next->own_flags = ((next->own_flags | previous->own_flags) & ~shrinking)
                    | (next->own_flags & previous->own_flags & shrinking);
    next->traps |= previous->traps;
    next->own_traps |= previous->own_traps;
    next->callback_params |= previous->callback_params;
    next->returns_external |= previous->returns_external;
    merge_effects(next, previous);
    join_flows(&next->flows[IR_SFLOW_RESULTS],
               &previous->flows[IR_SFLOW_RESULTS]);
    join_flows(&next->flows[IR_SFLOW_STORES],
               &previous->flows[IR_SFLOW_STORES]);
}



static uint64_t
summary_content(ir_summary_t *summary)
{
    if (summary->hashed) return summary->content;
    core_buffer_t out = {0};
    (void)ir_summary_encode(summary, &out);
    uint64_t scalars[] = {
        summary->own_flags, summary->own_traps, summary->callback_params,
        summary->pointer_result, summary->deferring,
    };
    core_buffer_append(&out, (const char *)scalars, sizeof(scalars));
    for (size_t i = 0; i < summary->fields.len; i++) {
        const ir_sfield_t *field = &summary->fields.data[i];
        uint64_t record = (uint64_t)(uintptr_t)field->record;
        core_buffer_append(&out, (const char *)&record, sizeof(record));
        core_buffer_append_string(&out, field->name);
        core_buffer_append(&out, (const char *)&field->flags,
                           sizeof(field->flags));
    }
    summary->content = core_xxh3_64bits_raw(out.data ? out.data : "",
                                            out.byte_len) | 1;
    summary->hashed  = true;
    core_buffer_release(&out);
    return summary->content;
}

static void
stamp_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *summary = ir_summary_of(summaries, members[i]);
        summary->identity = ir_func_identity(members[i]);
        for (size_t d = 0; d < summary->deps.len; d++) {
            summary_dep_t *dep = &summary->deps.data[d];
            if (member_index(members, count, dep->function) < count) {
                dep->content = dep->points_to = 0;
                continue;
            }
            ir_summary_t *callee = target_summary(summaries,
                                                  (ir_func_t *)dep->function);
            dep->content   = summary_content(callee);
            dep->points_to = points_to_content(callee);
        }
    }
}

static bool
depends_on(const ir_summary_t *summary, const ir_func_t *function)
{
    for (size_t d = 0; d < summary->deps.len; d++)
        if (summary->deps.data[d].function == function) return true;
    return false;
}

static ir_summary_t *
previous_summary(ir_summaries_t *summaries, ir_func_t *function)
{
    if (!summaries->reusable || !summaries->previous) return nullptr;
    ir_summary_t *old = ir_summary_of(summaries->previous, function);
    if (!old || old->pinned || !old->identity
        || (summaries->registry_changed && old->reads_registry)
        || core_dict_contains(&summaries->previous->adopted, old)
        || old->identity != ir_func_identity(function))
        return nullptr;
    return old;
}

static bool
covers_callees(ir_summaries_t *summaries, ir_func_t *const *members,
               size_t count, size_t at, const ir_summary_t *old)
{
    core_list_t(summary_func_ptr_t) callees =
        core_list_new(summary_func_ptr_t);
    direct_callees(summaries, members[at], &callees);
    bool covered = true;
    for (size_t c = 0; covered && c < callees.len; c++)
        covered = member_index(members, count, callees.data[c]) < count
            || depends_on(old, callees.data[c]);
    core_list_free(callees);
    return covered;
}

static bool
reusable_member(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count, size_t at)
{
    ir_summary_t *old = previous_summary(summaries, members[at]);
    if (!old || (summaries->proofs_changed && !old->proof_free)) return false;
    for (size_t d = 0; d < old->deps.len; d++) {
        const summary_dep_t *dep = &old->deps.data[d];
        if (member_index(members, count, dep->function) < count) continue;
        if (summary_content(target_summary(summaries,
                (ir_func_t *)dep->function)) != dep->content)
            return false;
    }
    return covers_callees(summaries, members, count, at, old);
}

static const ir_summary_t *
reusable_paths(ir_summaries_t *summaries, ir_func_t *const *members,
               size_t count, bool cyclic)
{
    if (cyclic || summaries->escapes_only) return nullptr;
    ir_summary_t *old = previous_summary(summaries, members[0]);
    if (!old || old->bookkept) return nullptr;
    for (size_t d = 0; d < old->deps.len; d++) {
        const summary_dep_t *dep = &old->deps.data[d];
        if (points_to_content(target_summary(summaries,
                (ir_func_t *)dep->function)) != dep->points_to)
            return nullptr;
    }
    return covers_callees(summaries, members, count, 0, old) ? old : nullptr;
}

static void
copy_paths(ir_summary_t *next, const ir_summary_t *old)
{
    for (size_t i = 0; i < old->paths.len; i++)
        core_list_push(next->paths, old->paths.data[i]);
    for (size_t i = 0; i < old->heaps.len; i++)
        core_list_push(next->heaps, old->heaps.data[i]);
    copy_flows(&next->flows[IR_SFLOW_RESULTS], &old->flows[IR_SFLOW_RESULTS]);
    copy_flows(&next->flows[IR_SFLOW_STORES], &old->flows[IR_SFLOW_STORES]);
    next->results_known    = old->results_known;
    next->returns_external = old->returns_external;
    const ir_switness_t *unknown = old->explained
        ? ir_summary_witness_for(old, IR_SWITNESS_UNKNOWN) : nullptr;
    if (unknown && !ir_summary_witness_for(next, IR_SWITNESS_UNKNOWN)) {
        core_list_push(next->witnesses, *unknown);
        next->explained = true;
    }
    for (size_t d = 0; d < old->deps.len; d++)
        if (!depends_on(next, old->deps.data[d].function))
            core_list_push(next->deps, ((summary_dep_t){
                .function = old->deps.data[d].function}));
}

static bool
adopt_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (!reusable_member(summaries, members, count, i)) return false;
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *old = ir_summary_of(summaries->previous, members[i]);
        core_dict_put(&summaries->previous->adopted, old, old);
        core_list_push(summaries->owned, old);
        core_dict_put(&summaries->by_function, members[i], old);
    }
    return true;
}

static void
solve_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    if (adopt_component(summaries, members, count)) return;
    uint64_t queries = ir_reinterpretation_queries();
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *summary = ir_summary_new(summaries, members[i]);
        summary->flags = IR_SUMMARY_NO_RETURN | IR_SUMMARY_DIVERGES;
        summary->results_known = true;
        core_dict_put(&summaries->by_function, members[i], summary);
    }
    member_callers_t *callers = member_callers(summaries, members, count);
    bool cyclic = count > 1 || callers[0].len;
    const ir_summary_t *reused = reusable_paths(summaries, members, count,
                                                cyclic);
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
    for (size_t i = 0; i < count; i++) dirty[i] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            if (!dirty[i]) continue;
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *next    = ir_summary_new(nullptr, members[i]);
            size_t scope = ir_analysis_scope_begin();
            summaries->recording = next;
            if (summaries->escapes_only)
                summarize_divergence(summaries, next, members[i]);
            else summarize_function(summaries, next, members[i],
                                    cyclic ? &settled[i] : nullptr);
            if (reused) copy_paths(next, reused);
            else summarize_paths(summaries, next, members[i]);
            const ir_summary_hooks_t *hooks = summaries->hooks;
            uint64_t bookkept = hooks && hooks->bookkeeping
                ? hooks->bookkeeping(hooks->context, members[i], next) : 0;
            ir_summary_paths_as_reads(next, bookkept);
            next->bookkept = bookkept != 0;
            summaries->recording = nullptr;
            ir_analysis_scope_end(scope);
            if (cyclic) join_rounds(next, summary);
            prune_flows(&next->flows[IR_SFLOW_RESULTS]);
            prune_flows(&next->flows[IR_SFLOW_STORES]);
            bool differs = cyclic
                && (next->flags != summary->flags
                    || next->own_flags != summary->own_flags
                    || next->callback_params != summary->callback_params
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
    bool reads_registry = ir_reinterpretation_queries() != queries
        || (reused && reused->reads_registry);
    for (size_t i = 0; reads_registry && i < count; i++)
        ir_summary_of(summaries, members[i])->reads_registry = true;
    stamp_component(summaries, members, count);
}

static void
direct_callees(ir_summaries_t *summaries, ir_func_t *f,
               core_list_t(summary_func_ptr_t) *out)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            ir_func_t *handler = in->op == IR_OP_CALLBACK_CREATE && in->dest
                ? created_handler(in->dest) : nullptr;
            if (handler) core_list_push(*out, handler);
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
        ir_func_t *f = ir_func_definition(pending.data[--pending.len]);
        if (!f || !core_dict_add(&seen, f, f)) continue;
        if (!f->n_blocks) {
            if (target_summary(summaries, f)->flags & IR_SUMMARY_REINTERPRETS)
                ir_reinterpretations_note_unseen(ctx, f->owner);
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

static bool
same_hooks(const ir_summary_hooks_t *a, const ir_summary_hooks_t *b)
{
    return a->context == b->context && a->call_targets == b->call_targets
        && a->axiom == b->axiom && a->adjust == b->adjust
        && a->entries == b->entries
        && a->global_reachable == b->global_reachable
        && a->bookkeeping == b->bookkeeping
        && a->inherits_effect == b->inherits_effect
        && a->callee_effects == b->callee_effects
        && a->call_flags == b->call_flags
        && a->call_finalizers == b->call_finalizers
        && a->owned_field == b->owned_field;
}

static ir_summaries_t *
compute_summaries(ir_func_t *const *functions, size_t count,
                  const ir_summary_hooks_t *hooks, bool escapes_only,
                  ir_summaries_t *previous)
{
    ir_summaries_t *summaries = core_alloc(ir_summaries_t);
    summaries->hooks = hooks;
    if (hooks) summaries->hook_values = *hooks;
    summaries->escapes_only = escapes_only;
    summaries->owned = core_list_new(summary_ptr_t);
    core_dict_init(&summaries->by_function, nullptr, nullptr);
    core_dict_init(&summaries->type_memos, nullptr, nullptr);
    core_dict_init(&summaries->engines, nullptr, nullptr);
    core_dict_init(&summaries->adopted, nullptr, nullptr);
    collect_reinterpretations(summaries, functions, count);
    summaries->previous = previous;
    summaries->reusable = previous && hooks && count && !escapes_only
        && !previous->escapes_only
        && same_hooks(&previous->hook_values, hooks);
    summaries->registry_changed = !count
        || ir_reinterpretations_changed(functions[0]->owner->ctx);
    summaries->proofs_changed = summaries->reusable
        && previous->hook_values.defer_trap_proofs
               != hooks->defer_trap_proofs;
    tarjan_t t = {.summaries = summaries,
                  .stack = core_list_new(summary_func_ptr_t)};
    core_dict_init(&t.index, nullptr, nullptr);
    core_dict_init(&t.lowlink, nullptr, nullptr);
    core_dict_init(&t.on_stack, nullptr, nullptr);
    ir_analysis_read_only_begin();
    for (size_t i = 0; i < count; i++) {
        bool found = false;
        dict_u32(&t.index, functions[i], &found);
        if (!found && functions[i]->n_blocks) strongconnect(&t, functions[i]);
    }
    ir_analysis_read_only_end();
    core_dict_free(&t.index);
    core_dict_free(&t.lowlink);
    core_dict_free(&t.on_stack);
    core_list_free(t.stack);
    summaries->previous = nullptr;
    return summaries;
}

ir_summaries_t *
ir_summaries_compute(ir_func_t *const *functions, size_t count,
                     const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, false, nullptr);
}

ir_summaries_t *
ir_summaries_recompute(ir_func_t *const *functions, size_t count,
                       const ir_summary_hooks_t *hooks,
                       ir_summaries_t *previous)
{
    return compute_summaries(functions, count, hooks, false, previous);
}

ir_summaries_t *
ir_escapes_compute(ir_func_t *const *functions, size_t count,
                   const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, true, nullptr);
}

void
ir_summaries_free(ir_summaries_t *summaries)
{
    if (!summaries) return;
    if (ir_analysis_summaries() == summaries) ir_analysis_bind_summaries(nullptr);
    for (size_t i = 0; i < summaries->owned.len; i++)
        if (!core_dict_contains(&summaries->adopted, summaries->owned.data[i]))
            summary_free(summaries->owned.data[i]);
    core_list_free(summaries->owned);
    core_dict_free(&summaries->adopted);
    core_dict_free(&summaries->by_function);
    type_memos_free(&summaries->type_memos);
    core_dict_foreach(&summaries->engines, entry) {
        engine_release(entry->value);
        core_free(entry->value);
    }
    core_dict_free(&summaries->engines);
    core_free(summaries);
}
