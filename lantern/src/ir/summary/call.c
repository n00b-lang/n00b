#include "internal.h"


typedef enum : uint8_t {
    MAP_PLACE,
    MAP_VALUE,
    MAP_DERIVED,
} map_mode_t;

enum {
    VALUE_ROOT    = 1,
    VALUE_DEEP    = 2,
    VALUE_DERIVED = 4,
};

typedef struct {
    const ir_instr_t   *call;
    const ir_summary_t *callee;
    uintptr_t           table;
    uintptr_t           group;
} flow_node_key_t;

typedef struct {
    flow_node_key_t       key;
    uint32_t              site;
    core_list_t(uint32_t) filled;
} flow_node_t;

typedef struct {
    const ir_instr_t   *call;
    const ir_summary_t *callee;
    ir_sflow_table_t    table;
    const flow_table_t *flows;
} mapped_sources_t;

typedef struct {
    pt_set_t to;
    pt_set_t from;
    bool     must;
} store_pair_t;

core_list_decl(store_pair_t);

static core_hash_value_t resolved_hash(void *key);

static void resolve_into(pt_t *pt, pt_loc_t loc, pt_set_t *into,
                         pt_set_t *seen);

static void
resolve_through(pt_t *pt, const pt_item_t *held, pt_set_t *into,
                pt_set_t *seen)
{
    pt_loc_t next = location_of(pt, held);
    next.path.flags |= IR_SPATH_DEEP;
    next.path.flags &= (uint8_t)~SPATH_INLINE;
    resolve_into(pt, next, into, seen);
}

static void
resolve_into(pt_t *pt, pt_loc_t loc, pt_set_t *into, pt_set_t *seen)
{
    if (loc.site == PT_EXTERNAL) {
        set_add(pt, into, item_at(loc));
        return;
    }
    if (!set_add(pt, seen, item_at(loc))) return;
    const ir_spath_t *path    = &loc.path;
    bool              deep    = loc_deep(&loc);
    bool              through = deep && !(path->flags & SPATH_INLINE);
    uint32_t          slot    = first_deref(path->steps, path->depth);
    if (slot == path->depth) {
        set_add(pt, into, item_at(loc));
        if (!through) return;
    }
    const core_string_t *tail  = path->steps + slot + 1;
    uint32_t             count = slot < path->depth
        ? path->depth - slot - 1 : 0;
    uint8_t              flags = path->flags
        & (uint8_t)(IR_SPATH_DEEP | SPATH_INLINE);
    pt_depend(pt, contents_key(loc.site));
    const pt_set_t *contents = &pt->sites.data[loc.site].contents;
    for (size_t i = 0; i < contents->len; i++) {
        pt_item_t held = contents->data[i];
        if (held.flow != PT_SAME) continue;
        if (slot == path->depth) {
            if (steps_prefix(path->depth, path->steps, held.depth,
                             held.steps)
                || (held.deep && steps_prefix(held.depth, held.steps,
                                              path->depth, path->steps)))
                resolve_through(pt, &held, into, seen);
            continue;
        }
        if (!steps_prefix(held.depth, held.steps, slot, path->steps))
            continue;
        if (held.deep) {
            resolve_through(pt, &held, into, seen);
            continue;
        }
        const core_string_t *inner = path->steps + held.depth;
        uint32_t             within = slot - held.depth;
        pt_loc_t             next   = held.loc;
        if (held.kind == PT_CONTENTS) {
            loc_extend(pt, &next, inner, within, false);
            loc_push(pt, &next, value_step);
        }
        else if (within) {
            resolve_through(pt, &held, into, seen);
            continue;
        }
        loc_extend(pt, &next, tail, count, false);
        if (!loc_deep(&next)) next.path.flags |= flags;
        else next.path.flags &= (uint8_t)~SPATH_INLINE;
        resolve_into(pt, next, into, seen);
    }
}

static core_hash_value_t
resolved_hash(void *key)
{
    const pt_loc_t *loc = key;
    return spath_hash((void *)&loc->path) * 31 + loc->site;
}

static bool
resolved_eq(const void *a, const void *b)
{
    const pt_loc_t *x = a, *y = b;
    return x->site == y->site && !compare_paths(&x->path, &y->path);
}

static core_hash_value_t
content_hash(void *key)
{
    const content_key_t *content = key;
    return resolved_hash((void *)&content->loc) * 31
        + (core_hash_value_t)(uintptr_t)content->at;
}

static bool
content_eq(const void *a, const void *b)
{
    const content_key_t *x = a, *y = b;
    return x->at == y->at && resolved_eq(&x->loc, &y->loc);
}

static bool
depends_changed(pt_t *pt, const core_list_t(uint64_t) *depends,
                uint64_t generation)
{
    for (size_t d = 0; d < depends->len; d++) {
        bool      known   = false;
        uintptr_t changed = (uintptr_t)core_dict_get(
            &pt->changed, (void *)(uintptr_t)depends->data[d], &known);
        if (changed > generation) return true;
    }
    return false;
}

static bool
memo_stale(pt_t *pt, const pt_memo_t *memo)
{
    return !memo->computed || (memo->positional && memo->at != pt->at)
        || depends_changed(pt, &memo->depends, memo->generation);
}

static bool
growth_only_stale(pt_t *pt, const pt_memo_t *memo)
{
    if (!memo->computed || (memo->positional && memo->at != pt->at))
        return false;
    for (size_t d = 0; d < memo->depends.len; d++) {
        uintptr_t key     = (uintptr_t)memo->depends.data[d];
        uintptr_t changed = (uintptr_t)core_dict_get(
            &pt->changed, (void *)key, nullptr);
        if (changed > memo->generation
            && !(key >> (sizeof(uintptr_t) * 8 - 1)) && (key & 3) == 1
            && ((key >> 2) >= pt->sites.len
                || pt->sites.data[key >> 2].local))
            return false;
    }
    return true;
}

static void
memo_begin(pt_t *pt, pt_memo_t *memo, pt_memo_frame_t *frame)
{
    *frame = (pt_memo_frame_t){.resolving   = pt->resolving,
                               .positional  = pt->positional,
                               .delta_since = pt->delta_since};
    memo->depends.len = 0;
    pt->resolving     = &memo->depends;
    pt->positional    = false;
    pt->delta_since   = 0;
}

static int
compare_keys(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void
memo_end(pt_t *pt, pt_memo_t *memo, const pt_memo_frame_t *frame)
{
    if (memo->depends.len > 1) {
        qsort(memo->depends.data, memo->depends.len, sizeof(uint64_t),
              compare_keys);
        size_t kept = 1;
        for (size_t d = 1; d < memo->depends.len; d++)
            if (memo->depends.data[d] != memo->depends.data[kept - 1])
                memo->depends.data[kept++] = memo->depends.data[d];
        memo->depends.len = kept;
    }
    memo->computed   = true;
    memo->positional = pt->positional;
    memo->at         = pt->at;
    memo->generation = pt->generation;
    memo->reader     = pt->reader;
    pt->resolving    = frame->resolving;
    pt->positional   = frame->positional;
    pt->delta_since  = frame->delta_since;
}

static void
memo_use(pt_t *pt, pt_memo_t *memo)
{
    pt->positional = pt->positional || memo->positional;
    if (pt->resolving)
        for (size_t d = 0; d < memo->depends.len; d++)
            core_list_push(*pt->resolving, memo->depends.data[d]);
    if (pt->reader == PT_NO_READER || memo->reader == pt->reader) return;
    core_list_t(uint64_t) *resolving = pt->resolving;
    pt->resolving = nullptr;
    for (size_t d = 0; d < memo->depends.len; d++)
        pt_depend(pt, (uintptr_t)memo->depends.data[d]);
    pt->resolving = resolving;
    memo->reader  = pt->reader;
}

static void
memo_free(pt_memo_t *memo)
{
    core_list_free(memo->depends);
}

static void
pt_resolve(pt_t *pt, pt_loc_t loc, pt_set_t *into)
{
    if (loc.site == PT_EXTERNAL) {
        set_add(pt, into, item_at(loc));
        return;
    }
    bool        found = false;
    resolved_t *entry = core_dict_get(&pt->resolved, &loc, &found);
    if (!found) {
        entry  = core_alloc(resolved_t);
        *entry = (resolved_t){.loc = loc};
        core_dict_put(&pt->resolved, &entry->loc, entry);
    }
    if (memo_stale(pt, &entry->memo)) {
        pt_memo_frame_t frame;
        memo_begin(pt, &entry->memo, &frame);
        entry->items.len = 0;
        pt->seen.len     = 0;
        resolve_into(pt, loc, &entry->items, &pt->seen);
        memo_end(pt, &entry->memo, &frame);
    }
    memo_use(pt, &entry->memo);
    set_merge(pt, into, &entry->items);
}

static void
argument_items(pt_t *pt, const ir_instr_t *call, uint32_t param,
               pt_set_t *into)
{
    ir_call_arg_t argument = ir_call_argument(call, param);
    switch (argument.role) {
    case IR_ARG_ABSENT:
    case IR_ARG_MISSING:
    case IR_ARG_OUT:
        return;
    case IR_ARG_FRESH: {
        pt_set_t address = (pt_set_t){0};
        pt_read(pt, argument.value, &address);
        pt_load(pt, &address, into);
        core_list_free(address);
        return;
    }
    default:
        if (argument.value) pt_read(pt, argument.value, into);
        else unresolved(pt, into);
        return;
    }
}

static void
map_own_storage(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
                bool inline_tail, pt_set_t *into)
{
    ir_call_arg_t argument = ir_call_argument(call, path->param);
    if (!ir_call_argument_writes(&argument)) return;
    pt_set_t address = (pt_set_t){0};
    pt_address(pt, argument.value, &address);
    for (size_t i = 0; i < address.len; i++) {
        if (!is_address(&address.data[i])) continue;
        pt_loc_t loc = address.data[i].loc;
        extend_tail(pt, &loc, path->steps, path->depth,
                    path->flags & IR_SPATH_DEEP, inline_tail);
        set_add(pt, into, item_at(loc));
    }
    core_list_free(address);
}

static void
map_param_place(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
                bool inline_tail, pt_set_t *into)
{
    if (path->output) {
        map_own_storage(pt, call, path, inline_tail, into);
        return;
    }
    pt_set_t items = (pt_set_t){0};
    argument_items(pt, call, path->param, &items);
    bool deep = path->flags & IR_SPATH_DEEP;
    for (size_t i = 0; i < items.len; i++) {
        const pt_item_t *item = &items.data[i];
        if (item->flow != PT_SAME) continue;
        pt_loc_t loc = item->loc;
        if (steps_prefix(item->depth, item->steps, path->depth,
                         path->steps)) {
            const core_string_t *rest  = path->steps + item->depth;
            uint32_t             count = path->depth - item->depth;
            bool                 deref = item->kind == PT_CONTENTS
                ? first_deref(rest, count) < count
                : count && step_eq(rest[0], value_step);
            if (!deref && !deep && !item->deep) continue;
            if (!deref || item->deep) {
                loc.path.flags |= IR_SPATH_DEEP;
                if (inline_tail && !item->deep && deref)
                    loc.path.flags |= SPATH_INLINE;
            }
            else if (item->kind == PT_CONTENTS) {
                loc_extend(pt, &loc, rest, count, false);
                extend_tail(pt, &loc, nullptr, 0, deep, inline_tail);
            }
            else extend_tail(pt, &loc, rest + 1, count - 1, deep, inline_tail);
        }
        else if ((item->deep || deep)
                 && steps_prefix(path->depth, path->steps, item->depth,
                                 item->steps)) {
            loc.path.flags |= IR_SPATH_DEEP;
            if (inline_tail && !item->deep) loc.path.flags |= SPATH_INLINE;
        }
        else
            continue;
        pt_resolve(pt, loc, into);
    }
    core_list_free(items);
}

static void
map_param_path(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
               pt_set_t *into)
{
    map_param_place(pt, call, path, false, into);
}


static const pt_callees_t *
callees_of(pt_t *pt, const ir_instr_t *call)
{
    bool  found = false;
    void *index = core_dict_get(&pt->call_index, call, &found);
    return found ? &pt->calls.data[(uintptr_t)index] : nullptr;
}

static void
note_callees(pt_t *pt, const ir_instr_t *call)
{
    pt_callees_t entry = {.callees = core_list_new(summary_ptr_t)};
    resolve_targets(pt->summaries, call, &entry.targets);
    entry.known = !entry.targets.unknown;
    for (size_t t = 0; t < entry.targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(pt->summaries,
                                              entry.targets.targets.data[t]);
        entry.known = entry.known && callee->results_known;
        core_list_push(entry.callees, callee);
    }
    core_dict_put(&pt->call_index, call, (void *)(uintptr_t)pt->calls.len);
    core_list_push(pt->calls, entry);
}

static void
call_reach(pt_t *pt, const ir_instr_t *call, const pt_callees_t *callees,
           pt_set_t *into)
{
    pt_set_t items = (pt_set_t){0};
    for (uint32_t o = call->op == IR_OP_CALL ? 1 : 0; o < call->n_operands;
         o++)
        pt_read(pt, referenced(call, o), &items);
    merge_widened(pt, into, &items, false);
    if (callees->targets.unknown) set_add(pt, into, deep_item(unknown_loc()));
    for (size_t t = 0; t < callees->callees.len; t++) {
        const ir_summary_t *callee = callees->callees.data[t];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if ((path->flags & IR_SPATH_READ) && path->root != IR_SPLACE_PARAM) {
                pt_item_t read = deep_item(path_loc(path));
                if (read.loc.path.root != IR_SPLACE_UNKNOWN)
                    read.kind = PT_CONTENTS;
                set_add(pt, into, read);
            }
        }
    }
}

static void
write_reach(pt_t *pt, const pt_loc_t *at, const pt_set_t *reach)
{
    for (size_t r = 0; r < reach->len; r++) {
        pt_item_t item = item_under(at->path.steps, at->path.depth, true,
                                    reach->data[r]);
        pt_add(pt, &pt->sites.data[at->site].contents,
               contents_key(at->site), item);
        note_write(pt, at->site, at, true, item);
    }
}

static void
places_value(pt_t *pt, pt_set_t *places, uint8_t mode, pt_set_t *into)
{
    bool deep = mode & VALUE_DEEP;
    if (mode & VALUE_ROOT) {
        for (size_t p = 0; p < places->len; p++) {
            pt_item_t place = places->data[p];
            place.deep = deep;
            set_add(pt, into, place);
        }
        if (!deep) return;
    }
    if (mode & VALUE_DERIVED) {
        size_t kept = 0;
        for (size_t p = 0; p < places->len; p++) {
            pt_item_t place = places->data[p];
            if (place.loc.site == PT_EXTERNAL || !is_address(&place)) {
                places->data[kept++] = place;
                continue;
            }
            if (place.deep) place.loc.path.flags |= IR_SPATH_DEEP;
            place.loc.path.flags &= (uint8_t)~SPATH_INLINE;
            place.deep = false;
            place.kind = PT_CONTENTS;
            place.flow = PT_DERIVED;
            set_add(pt, into, place);
        }
        places->len = kept;
    }
    if (!deep) {
        pt_load(pt, places, into);
        return;
    }
    pt_set_t loaded = (pt_set_t){0};
    pt_load(pt, places, &loaded);
    merge_widened(pt, into, &loaded, false);
}

static core_hash_value_t
valued_hash(void *key)
{
    const valued_t *entry = key;
    return resolved_hash((void *)&entry->loc) * 7 + entry->mode;
}

static bool
valued_eq(const void *a, const void *b)
{
    const valued_t *x = a, *y = b;
    return x->mode == y->mode && resolved_eq(&x->loc, &y->loc);
}

static void
resolved_value(pt_t *pt, pt_loc_t loc, uint8_t mode, pt_set_t *into)
{
    valued_t  probe = {.loc = loc, .mode = mode};
    bool      found = false;
    valued_t *entry = core_dict_get(&pt->valued, &probe, &found);
    if (!found) {
        entry  = core_alloc(valued_t);
        *entry = (valued_t){.loc = loc, .mode = mode};
        core_dict_put(&pt->valued, entry, entry);
    }
    if (memo_stale(pt, &entry->memo)) {
        pt_memo_frame_t frame;
        pt_set_t        places = (pt_set_t){0};
        memo_begin(pt, &entry->memo, &frame);
        entry->items.len = 0;
        pt_resolve(pt, loc, &places);
        places_value(pt, &places, mode, &entry->items);
        core_list_free(places);
        memo_end(pt, &entry->memo, &frame);
    }
    memo_use(pt, &entry->memo);
    set_merge(pt, into, &entry->items);
}

static bool
param_place(const pt_t *pt, const pt_item_t *item, const core_string_t *rest,
            uint32_t count, pt_loc_t *loc)
{
    *loc = item->loc;
    if (item->deep) {
        loc->path.flags |= IR_SPATH_DEEP;
        return true;
    }
    if (item->kind == PT_CONTENTS) {
        loc_extend(pt, loc, rest, count, false);
        return true;
    }
    if (!count || !step_eq(rest[0], value_step)) return false;
    extend_through(pt, loc, rest, count, false);
    return true;
}

static void
map_param_value(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
                bool derived, pt_set_t *into)
{
    pt_set_t items = (pt_set_t){0};
    argument_items(pt, call, path->param, &items);
    bool    deep = path->flags & IR_SPATH_DEEP;
    uint8_t mode = (path->flags & IR_SPATH_ROOT ? VALUE_ROOT : 0)
        | (deep ? VALUE_DEEP : 0) | (derived ? VALUE_DERIVED : 0);
    for (size_t i = 0; i < items.len; i++) {
        pt_item_t item = items.data[i];
        if (steps_prefix(path->depth, path->steps, item.depth, item.steps)) {
            if (deep) item = deepened(&item);
            else {
                item.steps += path->depth;
                item.depth -= path->depth;
            }
            set_add(pt, into, item);
            continue;
        }
        if (!steps_prefix(item.depth, item.steps, path->depth, path->steps))
            continue;
        const core_string_t *rest  = path->steps + item.depth;
        uint32_t             count = path->depth - item.depth;
        pt_loc_t             loc;
        if (!param_place(pt, &item, rest, count, &loc)) {
            set_add(pt, into, deepened(&item));
            continue;
        }
        if (item.flow != PT_SAME) {
            pt_item_t part = item.kind == PT_CONTENTS || loc.site
                    == PT_EXTERNAL
                ? contents_item(loc) : item_at(loc);
            if (deep) part.loc.path.flags |= IR_SPATH_DEEP;
            part.flow = item.flow;
            set_add(pt, into, item.deep ? anywhere_below(part) : part);
            continue;
        }
        bool lost = item.deep || (loc_deep(&loc) && !deep);
        if (deep) loc.path.flags |= IR_SPATH_DEEP;
        pt_set_t value = (pt_set_t){0};
        resolved_value(pt, loc, mode, lost ? &value : into);
        merge_widened(pt, into, &value, true);
    }
    core_list_free(items);
}

static core_hash_value_t
heap_site_hash(void *key)
{
    return core_hash_raw(key, sizeof(heap_site_key_t));
}

static bool
heap_site_eq(const void *a, const void *b)
{
    const heap_site_key_t *x = a, *y = b;
    return x->call == y->call && x->origin == y->origin;
}

static uint32_t
heap_site(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
          uint32_t origin)
{
    if (!origin) origin = instr_origin(pt, call);
    heap_site_key_t probe = {call, origin};
    bool            found = false;
    uintptr_t       site  = (uintptr_t)core_dict_get(&pt->heap_sites, &probe,
                                                     &found);
    if (found) return (uint32_t)site;
    heap_site_key_t  *key  = core_alloc(heap_site_key_t);
    const ir_sheap_t *heap = callee ? ir_summary_heap_for(callee, origin)
                                    : nullptr;
    *key = probe;
    site = pt->sites.len;
    core_list_push(pt->sites, ((pt_site_t){
        .key = key, .contents = (pt_set_t){0}, .aliases = (alias_list_t){0},
        .type = heap ? heap->type : nullptr, .elements = heap && heap->elements,
        .call = call, .origin = origin}));
    core_dict_put(&pt->heap_sites, key, (void *)site);
    return (uint32_t)site;
}

static void
map_location(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
             const ir_spath_t *path, map_mode_t mode, pt_set_t *into)
{
    bool value       = mode != MAP_PLACE;
    bool inline_tail = !value && (path->flags & SPATH_INLINE);
    bool root        = path->flags & IR_SPATH_ROOT;
    if (path->root == IR_SPLACE_HEAP) {
        pt_loc_t loc = site_loc(heap_site(pt, call, callee, path->param));
        if (value)
            loc_extend(pt, &loc, path->steps, path->depth,
                       path->flags & IR_SPATH_DEEP);
        else extend_tail(pt, &loc, path->steps, path->depth,
                         path->flags & IR_SPATH_DEEP, inline_tail);
        if (!value || root) set_add(pt, into, item_at(loc));
        else if (mode == MAP_DERIVED) set_add(pt, into, contents_item(loc));
        else content_at(pt, &loc, into);
    }
    else if (path->root == IR_SPLACE_PARAM && value)
        map_param_value(pt, call, path, mode == MAP_DERIVED, into);
    else if (path->root == IR_SPLACE_PARAM)
        map_param_place(pt, call, path, inline_tail, into);
    else {
        pt_loc_t loc = path_loc(path);
        if (inline_tail && loc_deep(&loc)
            && loc.path.root != IR_SPLACE_UNKNOWN)
            loc.path.flags |= SPATH_INLINE;
        set_add(pt, into, value && !root && named_root(loc.path.root)
            ? contents_item(loc) : item_at(loc));
    }
}

static bool
same_bits(const flow_bits_t *a, const flow_bits_t *b)
{
    size_t words = a->len > b->len ? a->len : b->len;
    for (size_t w = 0; w < words; w++)
        if ((w < a->len ? a->data[w] : 0) != (w < b->len ? b->data[w] : 0))
            return false;
    return true;
}

static const uint32_t *
flow_groups_of(pt_t *pt, const ir_summary_t *callee, ir_sflow_table_t table)
{
    const void *key   = (const char *)callee + table;
    bool        found = false;
    uint32_t   *groups = core_dict_get(&pt->flow_groups, (void *)key, &found);
    if (found) return groups;
    const flow_table_t *flows = &callee->flows[table];
    groups = core_alloc_array(uint32_t, flows->dests.len + 1);
    for (size_t d = 0; d < flows->dests.len; d++) {
        groups[d] = (uint32_t)d;
        for (size_t e = 0; e < d; e++)
            if (groups[e] == e
                && same_bits(&flows->dests.data[d].data,
                             &flows->dests.data[e].data)
                && same_bits(&flows->dests.data[d].control,
                             &flows->dests.data[e].control)) {
                groups[d] = (uint32_t)e;
                break;
            }
    }
    core_dict_put(&pt->flow_groups, (void *)key, groups);
    return groups;
}

static core_hash_value_t
flow_node_hash(void *key)
{
    return core_hash_raw(key, sizeof(flow_node_key_t));
}

static bool
flow_node_eq(const void *a, const void *b)
{
    const flow_node_key_t *x = a, *y = b;
    return x->call == y->call && x->callee == y->callee
        && x->table == y->table && x->group == y->group;
}

static core_hash_value_t
mapped_hash(void *key)
{
    return core_hash_raw(key, sizeof(mapped_key_t));
}

static bool
mapped_eq(const void *a, const void *b)
{
    return !memcmp(a, b, sizeof(mapped_key_t));
}

static void
mapped_sources_init(mapped_sources_t *mapped, const ir_instr_t *call,
                    const ir_summary_t *callee, ir_sflow_table_t table)
{
    *mapped = (mapped_sources_t){.call = call, .callee = callee,
                                 .table = table,
                                 .flows = &callee->flows[table]};
}

static mapped_entry_t *
source_entry(pt_t *pt, mapped_sources_t *mapped, uint32_t source,
             map_mode_t mode)
{
    mapped_key_t    probe = {mapped->call, mapped->callee,
                             (uintptr_t)mapped->table << 8 | mode, source};
    bool            found = false;
    mapped_entry_t *entry = core_dict_get(&pt->mapped, &probe, &found);
    if (!found) {
        entry  = core_alloc(mapped_entry_t);
        *entry = (mapped_entry_t){.key = probe};
        core_dict_put(&pt->mapped, &entry->key, entry);
    }
    const ir_instr_t *at         = pt->at;
    bool              positional = pt->positional;
    pt->at = mapped->call;
    if (memo_stale(pt, &entry->memo)) {
        pt_memo_frame_t frame;
        uint64_t        since = growth_only_stale(pt, &entry->memo)
            ? entry->memo.generation : 0;
        memo_begin(pt, &entry->memo, &frame);
        entry->added.len = 0;
        if (since) {
            pt->delta_since = since;
            for (size_t i = 0; i < entry->items.len; i++)
                core_list_push(entry->added, entry->items.data[i]);
        }
        else entry->items.len = 0;
        map_location(pt, mapped->call, mapped->callee,
                     mapped->flows->sources.data[source], mode,
                     &entry->items);
        memo_end(pt, &entry->memo, &frame);
        if (since) set_difference(&entry->items, &entry->added);
        entry->grew = since != 0;
        entry->version++;
    }
    memo_use(pt, &entry->memo);
    pt->at         = at;
    pt->positional = positional;
    return entry;
}

static const pt_set_t *
source_items(pt_t *pt, mapped_sources_t *mapped, uint32_t source,
             map_mode_t mode)
{
    return &source_entry(pt, mapped, source, mode)->items;
}

static void
fill_flow_node(pt_t *pt, flow_node_t *node, mapped_sources_t *mapped,
               const flow_dest_t *dest)
{
    const flow_table_t *flows = mapped->flows;
    uint32_t            site  = node->site;
    while (node->filled.len < flows->sources.len)
        core_list_push(node->filled, 0);
    for (uint32_t s = 0; s < flows->sources.len; s++) {
        bool data    = bit_has(&dest->data, s);
        bool control = !data && bit_has(&dest->control, s);
        if (!data && !control) continue;
        mapped_entry_t *entry = source_entry(pt, mapped, s, MAP_DERIVED);
        if (node->filled.data[s] == entry->version) continue;
        const pt_set_t *items = entry->grew
                && node->filled.data[s] + 1 == entry->version
            ? &entry->added : &entry->items;
        node->filled.data[s] = entry->version;
        for (size_t i = 0; i < items->len; i++) {
            pt_item_t item = items->data[i];
            pt_flow_kind_t flow = data ? PT_DERIVED : PT_CONTROL;
            if (item.flow < flow) item.flow = flow;
            if (!constant_item(&item))
                pt_add(pt, &pt->sites.data[site].contents,
                       contents_key(site), item);
        }
    }
}

static pt_item_t
flow_node_item(pt_t *pt, mapped_sources_t *mapped, uint32_t group,
               size_t dest)
{
    const ir_instr_t   *call   = mapped->call;
    const ir_summary_t *callee = mapped->callee;
    ir_sflow_table_t    table  = mapped->table;
    flow_node_key_t probe = {call, callee, table, group};
    bool            found = false;
    flow_node_t    *node  = core_dict_get(&pt->flow_nodes, &probe, &found);
    if (!found) {
        node = core_alloc(flow_node_t);
        node->key = probe;
        pt->nodes++;
        node->site = (uint32_t)site_of(pt,
            (const void *)(uintptr_t)(2 * pt->nodes + 1), nullptr, false, 0);
        pt->sites.data[node->site].node = true;
        core_dict_put(&pt->flow_nodes, &node->key, node);
    }
    if (group == dest)
        fill_flow_node(pt, node, mapped,
                       &callee->flows[table].dests.data[dest]);
    pt_item_t item = contents_item(site_loc(node->site));
    item.flow = PT_DERIVED;
    return item;
}

static void
flowed_items(pt_t *pt, mapped_sources_t *mapped, const uint32_t *groups,
             size_t d, pt_set_t *into)
{
    const flow_dest_t *dest = &mapped->flows->dests.data[d];
    for (uint32_t s = 0; s < mapped->flows->sources.len; s++) {
        if (bit_has(&dest->alias, s))
            set_merge(pt, into, source_items(pt, mapped, s, MAP_VALUE));
        else if (groups && bit_has(&dest->copy, s)) {
            const pt_set_t *items = source_items(pt, mapped, s, MAP_VALUE);
            for (size_t i = 0; i < items->len; i++) {
                pt_item_t item = items->data[i];
                if (item.flow < PT_COPY) item.flow = PT_COPY;
                set_add(pt, into, item);
            }
        }
    }
    if (groups && (dest->data.len || dest->control.len))
        set_add(pt, into, flow_node_item(pt, mapped, groups[d], d));
}

static void
mapped_output(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
              uint8_t output, pt_set_t *into)
{
    const flow_table_t *flows  = &callee->flows[IR_SFLOW_RESULTS];
    const uint32_t     *groups = pt->value_flow
        ? flow_groups_of(pt, callee, IR_SFLOW_RESULTS) : nullptr;
    pt_set_t            values = (pt_set_t){0};
    mapped_sources_t    mapped;
    mapped_sources_init(&mapped, call, callee, IR_SFLOW_RESULTS);
    for (size_t d = 0; d < flows->dests.len; d++) {
        const ir_spath_t *to = flows->dests.data[d].to;
        if (to->output != output) continue;
        values.len = 0;
        flowed_items(pt, &mapped, groups, d, &values);
        for (size_t v = 0; v < values.len; v++)
            set_add(pt, into, item_under(to->steps, to->depth,
                                     to->flags & IR_SPATH_DEEP,
                                     values.data[v]));
    }
    core_list_free(values);
}

static bool
must_dest(const ir_summary_t *callee, const ir_spath_t *to)
{
    if (to->flags & IR_SPATH_DEEP) return false;
    bool returns = !(callee->flags & (IR_SUMMARY_DIVERGES
                                      | IR_SUMMARY_NO_RETURN));
    bool fails   = callee->flags & IR_SUMMARY_MAY_FAIL;
    for (size_t p = 0; p < callee->paths.len; p++) {
        const ir_spath_t *path = &callee->paths.data[p];
        if (!(path->flags & IR_SPATH_WRITE) || (path->flags & IR_SPATH_DEEP)
            || !same_path_root(path, to) || path->depth != to->depth
            || !steps_prefix(path->depth, path->steps, to->depth, to->steps))
            continue;
        return (!returns || (path->flags & IR_SPATH_MUST_SUCCESS))
            && (!fails || (path->flags & IR_SPATH_MUST_FAILURE));
    }
    return false;
}

static bool
sole_callee(pt_t *pt, const ir_instr_t *call)
{
    const pt_callees_t *callees = callees_of(pt, call);
    return callees && !callees->targets.unknown && callees->callees.len == 1
        && callees->callees.data[0]->results_known;
}

static bool
apply_stores(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
             ir_summary_t *summary)
{
    const flow_table_t *flows   = &callee->flows[IR_SFLOW_STORES];
    const uint32_t     *groups  = pt->value_flow
        ? flow_groups_of(pt, callee, IR_SFLOW_STORES) : nullptr;
    bool                sole    = !summary && sole_callee(pt, call)
        && strong_writer(pt, call);
    bool                settled = true;
    core_list_t(store_pair_t) pairs = core_list_new(store_pair_t);
    mapped_sources_t    mapped;
    mapped_sources_init(&mapped, call, callee, IR_SFLOW_STORES);
    for (size_t d = 0; d < flows->dests.len; d++) {
        store_pair_t pair = {.must = sole
                                 && must_dest(callee, flows->dests.data[d].to)};
        flowed_items(pt, &mapped, groups, d, &pair.from);
        if (!pair.from.len && !pair.must) {
            core_list_free(pair.from);
            continue;
        }
        map_location(pt, call, callee, flows->dests.data[d].to, MAP_PLACE,
                     &pair.to);
        settled = settled && (!pair.must || pair.to.len);
        core_list_push(pairs, pair);
    }
    for (size_t p = 0; p < pairs.len; p++) {
        store_pair_t *pair = &pairs.data[p];
        if (summary) record_stores(pt, summary, &pair->to, &pair->from);
        else pt_store(pt, &pair->to, &pair->from, pair->must);
        core_list_free(pair->to);
        core_list_free(pair->from);
    }
    core_list_free(pairs);
    return settled;
}

static const core_string_t outcome_arms[] = {
    CORE_STRING_STATIC("ok"),
    CORE_STRING_STATIC("error"),
    CORE_STRING_STATIC("tag"),
};

static bool
fallible_call(pt_t *pt, const ir_instr_t *call)
{
    th_type *signature = ir_call_function_type(call);
    signature = signature ? th_resolve(signature) : nullptr;
    th_type *error = signature && th_kind_of(signature) == TH_FUNCTION
        ? th_func_error(signature) : nullptr;
    return error && ir_machine_of(pt->f->owner, error).class != IR_CLASS_VOID;
}

static void
call_result(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
            pt_set_t *into)
{
    if (!call->dest || !tracks(pt, call->dest->type)) return;
    if (!callee->results_known) {
        note_unknown(pt, (core_string_t){0}, (ir_func_t *)callee->function);
        set_add(pt, into, deep_item(unknown_loc()));
        return;
    }
    if (!fallible_call(pt, call)) {
        mapped_output(pt, call, callee, 0, into);
        return;
    }
    static const uint8_t outputs[] = {0, IR_SOUTPUT_FAILURE,
                                      IR_SOUTPUT_STATUS};
    pt_set_t items = (pt_set_t){0};
    for (size_t a = 0; a < 3; a++) {
        items.len = 0;
        mapped_output(pt, call, callee, outputs[a], &items);
        for (size_t i = 0; i < items.len; i++)
            set_add(pt, into, item_under(&outcome_arms[a], 1, false,
                                     items.data[i]));
    }
    core_list_free(items);
}

static bool
call_escapes(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
             const pt_set_t *reach, pt_set_t *result)
{
    if (callee->results_known) {
        bool settled = apply_stores(pt, call, callee, nullptr);
        call_result(pt, call, callee, result);
        return settled;
    }
    pt_set_t written = (pt_set_t){0};
    for (size_t p = 0; p < callee->paths.len; p++) {
        const ir_spath_t *path = &callee->paths.data[p];
        if (!(path->flags & IR_SPATH_WRITE)) continue;
        pt_set_t locs = (pt_set_t){0};
        if (path->root == IR_SPLACE_PARAM) map_param_path(pt, call, path,
                                                          &locs);
        else set_add(pt, &locs, item_at(path_loc(path)));
        for (size_t l = 0; l < locs.len; l++) {
            if (locs.data[l].loc.site != PT_EXTERNAL) {
                write_reach(pt, &locs.data[l].loc, reach);
                continue;
            }
            pt_loc_t at = locs.data[l].loc;
            at.path.flags |= IR_SPATH_DEEP;
            set_add(pt, &written, item_at(at));
        }
        core_list_free(locs);
    }
    for (uint32_t param = 0;
         written.len && param < ir_call_argument_count(call); param++) {
        if (!ir_summary_param_escape(callee, param)) continue;
        pt_set_t items = (pt_set_t){0};
        argument_items(pt, call, param, &items);
        for (size_t w = 0; w < written.len; w++)
            pt_publish(pt, &items, &written.data[w].loc);
        core_list_free(items);
    }
    core_list_free(written);
    call_result(pt, call, callee, result);
    return true;
}

static uint8_t
output_ordinal(const ir_instr_t *call, uint32_t param)
{
    th_type *signature = ir_call_function_type(call);
    size_t   ordinal   = signature ? ir_call_param_output(signature, param)
                                   : SIZE_MAX;
    return ordinal + 1 < UINT8_MAX ? (uint8_t)(ordinal + 1) : 0;
}

static void
callee_outputs(pt_t *pt, const ir_instr_t *call,
               const pt_callees_t *callees, uint8_t output, pt_set_t *into)
{
    for (size_t t = 0; t < callees->callees.len; t++)
        mapped_output(pt, call, callees->callees.data[t], output, into);
}

static bool
written_outputs(pt_t *pt, const ir_instr_t *call,
                const pt_callees_t *callees, const pt_set_t *reach,
                ir_summary_t *summary)
{
    bool settled = true;
    for (uint32_t param = 0; param < ir_call_argument_count(call); param++) {
        ir_call_arg_t argument = ir_call_argument(call, param);
        if (!ir_call_argument_writes(&argument)) continue;
        uint8_t  output  = output_ordinal(call, param);
        pt_set_t address = (pt_set_t){0};
        pt_set_t values  = (pt_set_t){0};
        pt_address(pt, argument.value, &address);
        if (output && callees->known)
            callee_outputs(pt, call, callees, output, &values);
        else if (reach)
            for (size_t r = 0; r < reach->len; r++)
                set_add(pt, &values, reach->data[r]);
        else
            call_reach(pt, call, callees, &values);
        settled = settled && address.len;
        if (summary) {
            record_set(pt, summary, &address, IR_SPATH_WRITE, nullptr);
            record_stores(pt, summary, &address, &values);
        }
        else
            pt_store(pt, &address, &values, strong_writer(pt, call));
        core_list_free(address);
        core_list_free(values);
    }
    return settled;
}

static void
pt_call(pt_t *pt, const ir_instr_t *call)
{
    const pt_callees_t *callees = callees_of(pt, call);
    pt_set_t reach  = (pt_set_t){0};
    pt_set_t result = (pt_set_t){0};
    call_reach(pt, call, callees, &reach);
    if (callees->targets.unknown) {
        note_unknown(pt, CORE_STRING_STATIC("a call whose target is unknown"),
                     nullptr);
        pt_loc_t unknown = unknown_loc();
        for (uint32_t o = 1; o < call->n_operands; o++) {
            pt_set_t items = (pt_set_t){0};
            pt_read(pt, referenced(call, o), &items);
            pt_publish(pt, &items, &unknown);
            for (size_t i = 0; i < items.len; i++)
                if (items.data[i].loc.site != PT_EXTERNAL
                    && items.data[i].flow == PT_SAME) {
                    pt_loc_t whole = items.data[i].loc;
                    whole.path.flags |= IR_SPATH_DEEP;
                    write_reach(pt, &whole, &reach);
                }
            core_list_free(items);
        }
        set_add(pt, &result, deep_item(unknown));
    }
    bool settled = true;
    for (size_t t = 0; t < callees->callees.len; t++)
        settled = call_escapes(pt, call, callees->callees.data[t], &reach,
                               &result) && settled;
    settled = written_outputs(pt, call, callees, &reach, nullptr) && settled;
    if (settled) settle_writes(pt, call);
    pt_define(pt, call, &result);
    core_list_free(reach);
    core_list_free(result);
}

static void
call_output(pt_t *pt, const ir_instr_t *output, pt_set_t *into)
{
    size_t            ordinal   = 0;
    const ir_instr_t *call      = ir_call_output_source(output, &ordinal);
    th_type          *signature = call ? ir_call_function_type(call)
                                       : nullptr;
    size_t param = signature ? ir_call_output_param(signature, ordinal)
                             : SIZE_MAX;
    const pt_callees_t *callees = param == SIZE_MAX
        || ordinal + 1 >= IR_SOUTPUT_STATUS ? nullptr : callees_of(pt, call);
    if (!callees) {
        unresolved(pt, into);
        return;
    }
    if (callees->known)
        callee_outputs(pt, call, callees, (uint8_t)(ordinal + 1), into);
    else {
        argument_items(pt, call, (uint32_t)param, into);
        call_reach(pt, call, callees, into);
    }
}
