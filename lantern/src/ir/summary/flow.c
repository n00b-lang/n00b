#include "internal.h"

typedef core_list_t(uint32_t) index_list_t;

typedef struct {
    const ir_spath_t *path;
    uint32_t          index;
} indexed_path_t;

static void flow_table_init(flow_table_t *table, const ir_func_t *function);
static void flow_table_free(flow_table_t *table);

static void
flow_table_init(flow_table_t *table, const ir_func_t *function)
{
    table->sources  = core_list_new(spath_ptr_t);
    table->dests    = core_list_new(flow_dest_t);
    table->function = function;
    core_dict_init(&table->source_index, spath_hash, spath_eq);
    core_dict_init(&table->dest_index, spath_hash, spath_eq);
}

static void
flow_table_free(flow_table_t *table)
{
    for (size_t s = 0; s < table->sources.len; s++)
        core_free(table->sources.data[s]);
    for (size_t d = 0; d < table->dests.len; d++) {
        core_free(table->dests.data[d].to);
        core_list_free(table->dests.data[d].data);
        core_list_free(table->dests.data[d].control);
        core_list_free(table->dests.data[d].alias);
        core_list_free(table->dests.data[d].copy);
    }
    core_list_free(table->sources);
    core_list_free(table->dests);
    core_dict_free(&table->source_index);
    core_dict_free(&table->dest_index);
}

static ir_spath_t *
owned_path(const ir_spath_t *path)
{
    ir_spath_t *owned = core_alloc(ir_spath_t);
    *owned = *path;
    owned->steps = steps_make(owned->steps, owned->depth);
    return owned;
}

static uint32_t
flow_source(flow_table_t *table, const ir_spath_t *path)
{
    bool      found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&table->source_index,
                                               (void *)path, &found);
    if (found) return (uint32_t)index;
    ir_spath_t *owned = owned_path(path);
    index = table->sources.len;
    core_list_push(table->sources, owned);
    core_dict_put(&table->source_index, owned, (void *)index);
    return (uint32_t)index;
}

static flow_dest_t *
flow_dest(flow_table_t *table, const ir_spath_t *to)
{
    bool      found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&table->dest_index,
                                               (void *)to, &found);
    if (found) return &table->dests.data[index];
    ir_spath_t *owned = owned_path(to);
    index = table->dests.len;
    core_list_push(table->dests, ((flow_dest_t){
        .to = owned, .data = core_list_new(uint64_t),
        .control = core_list_new(uint64_t),
        .alias = core_list_new(uint64_t), .copy = core_list_new(uint64_t)}));
    core_dict_put(&table->dest_index, owned, (void *)index);
    return &table->dests.data[index];
}

static bool
bit_set(flow_bits_t *bits, uint32_t bit)
{
    while (bits->len <= bit / 64) core_list_push(*bits, UINT64_C(0));
    uint64_t mask = UINT64_C(1) << (bit % 64);
    if (bits->data[bit / 64] & mask) return false;
    bits->data[bit / 64] |= mask;
    return true;
}

static bool
bit_has(const flow_bits_t *bits, uint32_t bit)
{
    return bit / 64 < bits->len
        && (bits->data[bit / 64] >> (bit % 64) & 1);
}

static uint32_t
bit_count(const flow_bits_t *bits)
{
    uint32_t count = 0;
    for (size_t w = 0; w < bits->len; w++)
        count += (uint32_t)__builtin_popcountll(bits->data[w]);
    return count;
}

static flow_bits_t *
kind_bits(flow_dest_t *dest, ir_sflow_kind_t kind)
{
    return kind == IR_SFLOW_DATA ? &dest->data
         : kind == IR_SFLOW_CONTROL ? &dest->control
         : kind == IR_SFLOW_ALIAS ? &dest->alias : &dest->copy;
}

static uint32_t
dest_count(const flow_dest_t *dest)
{
    return bit_count(&dest->data) + bit_count(&dest->control)
        + bit_count(&dest->alias) + bit_count(&dest->copy);
}

static th_type *
pointer_slot(const flow_table_t *table, const ir_spath_t *path)
{
    const ir_func_t *f = table->function;
    if (!f || !(path->flags & IR_SPATH_DEEP) || (path->flags & IR_SPATH_ROOT)
        || path->root == IR_SPLACE_UNKNOWN || path->root == IR_SPLACE_HEAP
        || path->root == IR_SPLACE_SOURCE)
        return nullptr;
    th_type *slot = ir_summary_path_type(f, path, path->depth);
    return slot && pointer_class(f->owner, slot) ? slot : nullptr;
}

static th_type *
pointee_slot(const flow_table_t *table, th_type *slot)
{
    ir_module_t *module = table->function->owner;
    th_type     *next   = pointee_type(module, slot);
    return next && next != slot && pointer_class(module, next)
            && !ir_type_eq(module, next, slot)
        ? next : nullptr;
}

static void
emit_source(flow_table_t *table, const ir_spath_t *path, ir_sflow_kind_t kind,
            core_list_t(uint64_t) *parts)
{
    core_list_push(*parts, (uint64_t)flow_source(table, path) << 8 | kind);
}

static void
canonical_sources(flow_table_t *table, const ir_spath_t *from,
                  ir_sflow_kind_t kind, core_list_t(uint64_t) *parts)
{
    ir_spath_t key  = canonical_path(from);
    uint32_t   base = kind != IR_SFLOW_ALIAS && (from->flags & IR_SPATH_ROOT)
        ? last_deref(from) : UINT32_MAX;
    if (base != UINT32_MAX) {
        ir_spath_t pointer = key;
        pointer.depth  = base;
        pointer.flags &= (uint8_t)~(IR_SPATH_ROOT | IR_SPATH_DEEP);
        emit_source(table, &pointer, kind, parts);
        if (!(from->flags & IR_SPATH_DEEP)) return;
        key.flags &= (uint8_t)~IR_SPATH_ROOT;
    }
    else if (kind != IR_SFLOW_ALIAS && (from->flags & IR_SPATH_ROOT)
             && (from->flags & IR_SPATH_DEEP))
        key.flags &= (uint8_t)~IR_SPATH_ROOT;
    th_type *slot = pointer_slot(table, &key);
    while (slot) {
        ir_spath_t pointer = key;
        pointer.flags &= (uint8_t)~IR_SPATH_DEEP;
        emit_source(table, &pointer, kind, parts);
        key.steps = steps_append(key.steps, key.depth, value_step);
        key.depth++;
        slot = pointee_slot(table, slot);
    }
    emit_source(table, &key, kind, parts);
}

static void
flow_put(flow_table_t *table, const ir_spath_t *to, const ir_spath_t *from,
         ir_sflow_kind_t kind)
{
    ir_spath_t   key_to = canonical_path(to);
    uint32_t     source = flow_source(table, from);
    flow_dest_t *dest   = flow_dest(table, &key_to);
    bit_set(kind_bits(dest, kind), source);
}

static void
flow_add(flow_table_t *table, const ir_spath_t *to, const ir_spath_t *from,
         ir_sflow_kind_t kind)
{
    core_list_t(uint64_t) parts = core_list_new(uint64_t);
    canonical_sources(table, from, kind, &parts);
    ir_spath_t   key_to = canonical_path(to);
    flow_dest_t *dest   = flow_dest(table, &key_to);
    for (size_t p = 0; p < parts.len; p++)
        bit_set(kind_bits(dest, kind), (uint32_t)(parts.data[p] >> 8));
    core_list_free(parts);
}

static bool
same_flow_bits(const flow_table_t *a, const flow_bits_t *have,
               const flow_table_t *b, const flow_bits_t *want)
{
    if (bit_count(have) != bit_count(want)) return false;
    for (uint32_t s = 0; s < a->sources.len; s++) {
        if (!bit_has(have, s)) continue;
        bool      found = false;
        uintptr_t other = (uintptr_t)core_dict_get(
            (core_dict_t *)&b->source_index, a->sources.data[s], &found);
        if (!found || !bit_has(want, (uint32_t)other)) return false;
    }
    return true;
}

static void
join_flows(flow_table_t *into, const flow_table_t *from)
{
    for (size_t d = 0; d < from->dests.len; d++) {
        flow_dest_t *dest = &from->dests.data[d];
        for (uint32_t s = 0; s < from->sources.len; s++)
            for (int kind = IR_SFLOW_DATA; kind <= IR_SFLOW_COPY; kind++)
                if (bit_has(kind_bits(dest, kind), s))
                    flow_put(into, dest->to, from->sources.data[s], kind);
    }
}

static bool
place_covers(const ir_spath_t *a, const ir_spath_t *b)
{
    if (!(a->flags & SPATH_INLINE))
        return steps_cover(a->depth, a->flags & IR_SPATH_DEEP, a->steps,
                           b->depth, b->flags & IR_SPATH_DEEP, b->steps);
    return steps_prefix(a->depth, a->steps, b->depth, b->steps)
        && first_deref(b->steps + a->depth, b->depth - a->depth)
               == b->depth - a->depth
        && (!(b->flags & IR_SPATH_DEEP) || (b->flags & SPATH_INLINE));
}

static bool
dest_covers(const ir_spath_t *a, const ir_spath_t *b)
{
    return same_path_root(a, b) && place_covers(a, b);
}

static bool
covers_path(const ir_spath_t *outer, uint32_t outer_index,
            const ir_spath_t *inner, uint32_t inner_index, bool dests)
{
    bool (*covers)(const ir_spath_t *, const ir_spath_t *) =
        dests ? dest_covers : spath_covers;
    return outer_index != inner_index && (outer->flags & IR_SPATH_DEEP)
        && covers(outer, inner)
        && (!covers(inner, outer) || outer_index < inner_index);
}

static index_list_t *
covering_paths(spath_ptr_t *paths, uint32_t count, bool dests)
{
    index_list_t *covers = core_alloc_array(index_list_t, count + 1);
    for (uint32_t i = 0; i < count; i++) {
        covers[i] = core_list_new(uint32_t);
        core_list_push(covers[i], i);
    }
    for (uint32_t o = 0; o < count; o++) {
        if (!(paths[o]->flags & IR_SPATH_DEEP)) continue;
        for (uint32_t i = 0; i < count; i++)
            if (covers_path(paths[o], o, paths[i], i, dests))
                core_list_push(covers[i], o);
    }
    return covers;
}

static void
free_covers(index_list_t *covers, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) core_list_free(covers[i]);
    core_free(covers);
}

static bool
flow_covered(flow_table_t *table, const index_list_t *dests,
             const index_list_t *sources, uint32_t d, uint32_t s,
             ir_sflow_kind_t kind)
{
    for (size_t e = 0; e < dests[d].len; e++) {
        flow_dest_t *dest = &table->dests.data[dests[d].data[e]];
        for (size_t t = 0; t < sources[s].len; t++) {
            uint32_t source = sources[s].data[t];
            bool     self   = dests[d].data[e] == d && source == s;
            if (kind == IR_SFLOW_ALIAS || kind == IR_SFLOW_COPY) {
                if (!self && (bit_has(&dest->alias, source)
                              || bit_has(kind_bits(dest, kind), source)))
                    return true;
                continue;
            }
            if (!self && bit_has(&dest->copy, source)) return true;
            if (bit_has(&dest->data, source)
                && !(self && kind == IR_SFLOW_DATA)) return true;
            if (kind == IR_SFLOW_CONTROL && !self
                && bit_has(&dest->control, source))
                return true;
        }
    }
    return false;
}

static int
compare_indexed_paths(const void *a, const void *b)
{
    return compare_paths(((const indexed_path_t *)a)->path,
                         ((const indexed_path_t *)b)->path);
}

static uint32_t *
sorted_paths(spath_ptr_t *paths, uint32_t count)
{
    indexed_path_t *entries = core_alloc_array(indexed_path_t, count + 1);
    for (uint32_t i = 0; i < count; i++)
        entries[i] = (indexed_path_t){paths[i], i};
    qsort(entries, count, sizeof(indexed_path_t), compare_indexed_paths);
    uint32_t *order = core_alloc_array(uint32_t, count + 1);
    for (uint32_t i = 0; i < count; i++) order[i] = entries[i].index;
    core_free(entries);
    return order;
}

static bool
pointee_reach_of(const ir_spath_t *reach, const ir_spath_t *pointer)
{
    if (!(reach->flags & IR_SPATH_DEEP) || !same_path_root(reach, pointer))
        return false;
    if (reach->flags & IR_SPATH_ROOT) {
        if (pointer->flags & (IR_SPATH_ROOT | IR_SPATH_DEEP)) return false;
        if (steps_prefix(reach->depth, reach->steps, pointer->depth,
                         pointer->steps))
            return true;
        return reach->depth == pointer->depth + 1
            && steps_prefix(pointer->depth, pointer->steps, reach->depth,
                            reach->steps)
            && step_eq(reach->steps[pointer->depth], value_step);
    }
    uint32_t base = pointer->depth;
    if (pointer->flags & IR_SPATH_ROOT) {
        uint32_t deref = last_deref(pointer);
        if (deref != UINT32_MAX) base = deref;
    }
    else if (pointer->flags & IR_SPATH_DEEP) return false;
    return steps_prefix(reach->depth, reach->steps, base, pointer->steps);
}

static bool
pointer_covered(flow_table_t *table, const index_list_t *dests, uint32_t d,
                uint32_t s, ir_sflow_kind_t kind)
{
    if (kind != IR_SFLOW_ALIAS) return false;
    const ir_spath_t *pointer = table->sources.data[s];
    for (size_t e = 0; e < dests[d].len; e++) {
        flow_dest_t *dest = &table->dests.data[dests[d].data[e]];
        for (uint32_t t = 0; t < table->sources.len; t++)
            if (bit_has(&dest->alias, t)
                && pointee_reach_of(table->sources.data[t], pointer))
                return true;
    }
    return false;
}

static void
prune_flows(flow_table_t *table)
{
    uint32_t      dest_count   = (uint32_t)table->dests.len;
    uint32_t      source_count = (uint32_t)table->sources.len;
    spath_ptr_t  *to = core_alloc_array(spath_ptr_t, dest_count + 1);
    for (uint32_t d = 0; d < dest_count; d++) to[d] = table->dests.data[d].to;
    index_list_t *dests   = covering_paths(to, dest_count, true);
    index_list_t *sources = covering_paths(table->sources.data, source_count,
                                           false);
    flow_dest_t  *kept    = core_alloc_array(flow_dest_t, dest_count + 1);
    bool         *used    = core_alloc_array(bool, source_count + 1);
    for (uint32_t d = 0; d < dest_count; d++) {
        flow_dest_t *dest = &table->dests.data[d];
        kept[d] = (flow_dest_t){.data    = core_list_new(uint64_t),
                                .control = core_list_new(uint64_t),
                                .alias   = core_list_new(uint64_t),
                                .copy    = core_list_new(uint64_t)};
        for (uint32_t s = 0; s < source_count; s++) {
            bool copy = bit_has(&dest->copy, s);
            bool data = copy || bit_has(&dest->data, s);
            for (int kind = IR_SFLOW_DATA; kind <= IR_SFLOW_COPY; kind++) {
                if (!bit_has(kind_bits(dest, kind), s)
                    || (kind == IR_SFLOW_CONTROL && data)
                    || (kind == IR_SFLOW_DATA && copy)
                    || flow_covered(table, dests, sources, d, s, kind)
                    || pointer_covered(table, dests, d, s, kind))
                    continue;
                bit_set(kind_bits(&kept[d], kind), s);
                used[s] = true;
            }
        }
    }
    uint32_t     *dest_order   = sorted_paths(to, dest_count);
    uint32_t     *source_order = sorted_paths(table->sources.data,
                                              source_count);
    uint32_t     *renumber     = core_alloc_array(uint32_t, source_count + 1);
    flow_table_t  pruned;
    flow_table_init(&pruned, table->function);
    for (uint32_t i = 0; i < source_count; i++)
        if (used[source_order[i]])
            renumber[source_order[i]] =
                flow_source(&pruned, table->sources.data[source_order[i]]);
    for (uint32_t i = 0; i < dest_count; i++) {
        uint32_t     d    = dest_order[i];
        flow_dest_t *dest = nullptr;
        for (uint32_t s = 0; s < source_count; s++)
            for (int kind = IR_SFLOW_DATA; kind <= IR_SFLOW_COPY; kind++) {
                if (!bit_has(kind_bits(&kept[d], kind), s)) continue;
                if (!dest) dest = flow_dest(&pruned, to[d]);
                bit_set(kind_bits(dest, kind), renumber[s]);
            }
    }
    for (uint32_t d = 0; d < dest_count; d++) {
        core_list_free(kept[d].data);
        core_list_free(kept[d].control);
        core_list_free(kept[d].alias);
        core_list_free(kept[d].copy);
    }
    core_free(kept);
    core_free(used);
    core_free(renumber);
    core_free(dest_order);
    core_free(source_order);
    free_covers(dests, dest_count);
    free_covers(sources, source_count);
    core_free(to);
    flow_table_free(table);
    *table = pruned;
}

static uint32_t
live_dests(const flow_table_t *table)
{
    uint32_t live = 0;
    for (size_t d = 0; d < table->dests.len; d++)
        live += dest_count(&table->dests.data[d]) != 0;
    return live;
}

static bool
same_flows(const flow_table_t *a, const flow_table_t *b)
{
    if (live_dests(a) != live_dests(b)) return false;
    for (size_t d = 0; d < a->dests.len; d++) {
        const flow_dest_t *have = &a->dests.data[d];
        if (!dest_count(have)) continue;
        bool      found = false;
        uintptr_t other = (uintptr_t)core_dict_get(
            (core_dict_t *)&b->dest_index, have->to, &found);
        if (!found) return false;
        const flow_dest_t *want = &b->dests.data[other];
        if (!same_flow_bits(a, &have->data, b, &want->data)
            || !same_flow_bits(a, &have->control, b, &want->control)
            || !same_flow_bits(a, &have->alias, b, &want->alias)
            || !same_flow_bits(a, &have->copy, b, &want->copy))
            return false;
    }
    return true;
}

uint32_t
ir_summary_flow_count(const ir_summary_t *summary, ir_sflow_table_t table)
{
    return summary ? (uint32_t)summary->flows[table].dests.len : 0;
}

const ir_spath_t *
ir_summary_flow_to(const ir_summary_t *summary, ir_sflow_table_t table,
                   uint32_t flow)
{
    return summary && flow < summary->flows[table].dests.len
        ? summary->flows[table].dests.data[flow].to : nullptr;
}

uint32_t
ir_summary_flow_source_count(const ir_summary_t *summary,
                             ir_sflow_table_t table)
{
    return summary ? (uint32_t)summary->flows[table].sources.len : 0;
}

const ir_spath_t *
ir_summary_flow_source(const ir_summary_t *summary, ir_sflow_table_t table,
                       uint32_t source)
{
    return summary && source < summary->flows[table].sources.len
        ? summary->flows[table].sources.data[source] : nullptr;
}

bool
ir_summary_flow_has(const ir_summary_t *summary, ir_sflow_table_t table,
                    uint32_t flow, uint32_t source, ir_sflow_kind_t kind)
{
    if (!summary || flow >= summary->flows[table].dests.len) return false;
    return bit_has(kind_bits((flow_dest_t *)&summary->flows[table].dests
                                 .data[flow], kind), source);
}

void
ir_summary_add_flow(ir_summary_t *summary, ir_sflow_table_t table,
                    const ir_spath_t *to, const ir_spath_t *from,
                    ir_sflow_kind_t kind)
{
    flow_add(&summary->flows[table], to, from, kind);
    summary->results_known = true;
}
