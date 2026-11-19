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

static uint64_t
visited_hash(const pt_loc_t *loc)
{
    const ir_spath_t *path = &loc->path;
    uint64_t          hash = (uint64_t)loc->site * 0x9e3779b97f4a7c15ull
        ^ ((uint64_t)path->root << 40 | (uint64_t)path->output << 32
           | (uint64_t)path->flags << 24 | path->depth);
    hash = hash * 31 + path->param;
    hash = hash * 31 + (uint64_t)(uintptr_t)path->global;
    hash = hash * 31 + (uint64_t)(uintptr_t)path->type;
    for (uint32_t s = 0; s < path->depth; s++)
        hash = hash * 31 + (uint64_t)(uintptr_t)path->steps[s].data
            + path->steps[s].u8_bytes;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    return hash ^ hash >> 33;
}

static bool
same_visited(const pt_loc_t *a, const pt_loc_t *b)
{
    const ir_spath_t *x = &a->path, *y = &b->path;
    if (a->site != b->site || x->root != y->root || x->output != y->output
        || x->flags != y->flags || x->depth != y->depth
        || x->param != y->param || x->global != y->global
        || x->type != y->type)
        return false;
    if (x->steps == y->steps) return true;
    for (uint32_t s = 0; s < x->depth; s++)
        if ((x->steps[s].data != y->steps[s].data
             || x->steps[s].u8_bytes != y->steps[s].u8_bytes)
            && !core_string_eq(x->steps[s], y->steps[s]))
            return false;
    return true;
}

// Adds `loc` to `table`; false if it was there already.
static bool
loc_table_add(loc_table_t *table, const pt_loc_t *loc)
{
    if (2 * (table->len + 1) > table->cap) {
        loc_table_t old = *table;
        table->cap    = old.cap ? old.cap * 2 : 64;
        table->slots  = core_alloc_array(pt_loc_t, table->cap);
        table->hashes = core_alloc_array(uint64_t, table->cap);
        table->used   = core_alloc_array(bool, table->cap);
        memset(table->used, 0, table->cap * sizeof(bool));
        table->len = 0;
        for (size_t s = 0; s < old.cap; s++)
            if (old.used[s]) loc_table_add(table, &old.slots[s]);
        if (old.slots) core_free(old.slots);
        if (old.hashes) core_free(old.hashes);
        if (old.used) core_free(old.used);
    }
    uint64_t hash = visited_hash(loc);
    size_t   mask = table->cap - 1;
    size_t   at   = hash & mask;
    for (; table->used[at]; at = (at + 1) & mask)
        if (table->hashes[at] == hash
            && same_visited(&table->slots[at], loc))
            return false;
    table->slots[at]  = *loc;
    table->hashes[at] = hash;
    table->used[at]   = true;
    table->len++;
    return true;
}

static void
loc_table_clear(loc_table_t *table)
{
    if (table->len) memset(table->used, 0, table->cap * sizeof(bool));
    table->len = 0;
}

static void
loc_table_free(loc_table_t *table)
{
    if (table->slots) core_free(table->slots);
    if (table->hashes) core_free(table->hashes);
    if (table->used) core_free(table->used);
    *table = (loc_table_t){0};
}

// Whether the walk under way has met exactly `loc` before, noting it if
// not. A location met before needs nothing more: either it was resolved
// then, adding everything it reaches, or `seen` already covered it. The
// check stands in front of `seen`, whose covering lookups were most of the
// time a function with many mutually reachable sites spent solving: each
// site's contents name most of the other sites, so each site is met once
// per site that holds it.
static bool
visited_before(pt_t *pt, const pt_loc_t *loc)
{
    return !loc_table_add(&pt->visited, loc);
}

// Resolves what `held` points to, deeply. Where it points depends on its
// location and kind alone, and once the walk has gone through one such item
// it has met that place (see visited_before), so going through another
// item with the same location and kind finds nothing new. Many sites hold
// items naming the same places, and working out each one's place again (a
// step pushed and interned, slot types looked up) only to find it visited
// was most of what the walk did.
static void
resolve_through(pt_t *pt, const pt_item_t *held, pt_set_t *into,
                pt_set_t *seen)
{
    if (!loc_table_add(&pt->through[held->kind == PT_CONTENTS], &held->loc))
        return;
    pt_loc_t next = location_of(pt, held);
    next.path.flags |= IR_SPATH_DEEP;
    next.path.flags &= (uint8_t)~SPATH_INLINE;
    resolve_into(pt, next, into, seen);
}

static uint32_t
foreign_deref(const pt_t *pt, const pt_loc_t *loc)
{
    const ir_spath_t *path = &loc->path;
    uint32_t          slot = first_deref(path->steps, path->depth);
    while (slot < path->depth) {
        pt_loc_t owner    = *loc;
        owner.path.depth  = slot;
        owner.path.flags &= (uint8_t)~(IR_SPATH_DEEP | SPATH_INLINE);
        if (!owned_slot(pt, &owner)) break;
        slot += 1 + first_deref(path->steps + slot + 1,
                                path->depth - slot - 1);
    }
    return slot;
}

// Whether `held`, an item of `site`'s contents, is a pointer that lies in a
// string value there, or a value copied out of (or from inside) a string.
static bool
string_pointer(const pt_t *pt, uint32_t site, const pt_item_t *held)
{
    pt_loc_t at = {.site = site,
                   .path = {.steps = held->steps, .depth = held->depth}};
    if (within_string(pt, &at, held->depth)) return true;
    return held->kind == PT_CONTENTS
        && within_string(pt, &held->loc, held->loc.path.depth);
}

// A place to write that runs on below a string value stops at the string's
// own storage: what lies past its data pointer is never written.
static void
string_value_only(const pt_t *pt, pt_loc_t *loc)
{
    if (loc_deep(loc) && !(loc->path.flags & SPATH_INLINE)
        && within_string(pt, loc, loc->path.depth))
        loc->path.flags |= SPATH_INLINE;
}

static void
resolve_into(pt_t *pt, pt_loc_t loc, pt_set_t *into, pt_set_t *seen)
{
    if (pt->writing) string_value_only(pt, &loc);
    if (loc.site == PT_EXTERNAL) {
        // Met before in this walk, it was added then (see visited_before).
        if (visited_before(pt, &loc)) return;
        if (!pt->writing || !immutable_place(pt, &loc))
            set_add(pt, into, item_at(loc));
        return;
    }
    if (visited_before(pt, &loc) || !set_add(pt, seen, item_at(loc)))
        return;
    const ir_spath_t *path    = &loc.path;
    bool              deep    = loc_deep(&loc);
    bool              through = deep && !(path->flags & SPATH_INLINE);
    uint32_t          slot    = foreign_deref(pt, &loc);
    if (slot == path->depth) {
        set_add(pt, into, item_at(loc));
        if (!through) return;
    }
    // Strings are immutable: what a string's data points to (its bytes, its
    // rich body and everything in that) is written only while it is being
    // made, through the storage itself, never through a string. So a place
    // to write is never found by following a pointer that lies in a string
    // value, or one copied out of a string.
    if (pt->writing && slot < path->depth && within_string(pt, &loc, slot))
        return;
    uint32_t             count = slot < path->depth
        ? path->depth - slot - 1 : 0;
    // A path with no steps has none to point into (`steps` may be null).
    const core_string_t *tail  = count ? path->steps + slot + 1 : nullptr;
    uint8_t              flags = path->flags
        & (uint8_t)(IR_SPATH_DEEP | SPATH_INLINE);
    pt_depend(pt, contents_key(loc.site));
    const pt_set_t *contents = &pt->sites.data[loc.site].contents;
    for (size_t i = 0; i < contents->len; i++) {
        pt_item_t held = contents->data[i];
        if (held.flow != PT_SAME) continue;
        if (pt->writing && string_pointer(pt, loc.site, &held)) continue;
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
        uint32_t             within = slot - held.depth;
        const core_string_t *inner = within ? path->steps + held.depth
                                            : nullptr;
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
    return x->site == y->site && same_spath(&x->path, &y->path);
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

static core_hash_value_t
aliased_hash(void *key)
{
    const aliased_key_t *aliased = key;
    return resolved_hash((void *)&aliased->loc) * 31
        + (core_hash_value_t)aliased->signature;
}

static bool
aliased_eq(const void *a, const void *b)
{
    const aliased_key_t *x = a, *y = b;
    return x->signature == y->signature && resolved_eq(&x->loc, &y->loc);
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

// The places `loc` stands for; with `writes`, only those a write can reach
// (see resolve_into).
static void
pt_resolve_places(pt_t *pt, pt_loc_t loc, bool writes, pt_set_t *into)
{
    if (loc.site == PT_EXTERNAL) {
        if (!writes || !immutable_place(pt, &loc))
            set_add(pt, into, item_at(loc));
        return;
    }
    core_dict_t *memo  = writes ? &pt->resolved_writes : &pt->resolved;
    bool         found = false;
    resolved_t  *entry = core_dict_get(memo, &loc, &found);
    if (!found) {
        entry  = core_alloc(resolved_t);
        *entry = (resolved_t){.loc = loc};
        core_dict_put(memo, &entry->loc, entry);
    }
    if (memo_stale(pt, &entry->memo)) {
        pt_memo_frame_t frame;
        memo_begin(pt, &entry->memo, &frame);
        entry->items.len = 0;
        pt->seen.len     = 0;
        pt->writing      = writes;
        loc_table_clear(&pt->visited);
        loc_table_clear(&pt->through[0]);
        loc_table_clear(&pt->through[1]);
        resolve_into(pt, loc, &entry->items, &pt->seen);
        pt->writing = false;
        memo_end(pt, &entry->memo, &frame);
    }
    memo_use(pt, &entry->memo);
    set_merge(pt, into, &entry->items);
}

static void
pt_resolve(pt_t *pt, pt_loc_t loc, pt_set_t *into)
{
    pt_resolve_places(pt, loc, false, into);
}

static void
argument_items(pt_t *pt, const ir_instr_t *call, uint32_t param,
               pt_set_t *into)
{
    ir_call_arg_t argument = ir_call_argument(call, param);
    switch (argument.role) {
    case IR_ARG_ABSENT:
    case IR_ARG_MISSING:
        return;
    default:
        // A fresh operand, like any by-reference one, is the address of the
        // place the callee's parameter refers to.
        if (argument.value) pt_read(pt, argument.value, into);
        else unresolved(pt, into);
        return;
    }
}

// The items a call lets escape when its callee lets parameter `param`
// escape. An `out` or `modify` parameter refers to the caller's place;
// unless the callee lets that reference itself escape, only what the place
// holds escapes, not the place.
static void
escaping_items(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
               uint32_t param, uint8_t escape, pt_set_t *into)
{
    th_type *signature = callee->function && callee->function->type
        ? th_resolve(callee->function->type) : nullptr;
    bool by_reference = signature && th_kind_of(signature) == TH_FUNCTION
        && param < th_func_nparams(signature)
        && th_mode_by_reference(th_func_param(signature, param)->mode);
    if (!by_reference || (escape & IR_SESCAPE_ADDRESS)) {
        argument_items(pt, call, param, into);
        return;
    }
    pt_set_t address = (pt_set_t){0};
    argument_items(pt, call, param, &address);
    pt_load(pt, &address, into);
    core_list_free(address);
}

// The storage a constructor fills is uninitialized when the call starts: a
// callee path that reaches through a pointer it held then reaches nothing.
// (The callee's own stores into what it allocated are summarized at the
// allocations.)
static bool
fresh_entry_path(const ir_instr_t *call, const ir_spath_t *path)
{
    if (path->root != IR_SPLACE_PARAM) return false;
    ir_call_arg_t argument = ir_call_argument(call, path->param);
    if (argument.role != IR_ARG_FRESH) return false;
    uint32_t skip = path->depth && step_eq(path->steps[0], value_step) ? 1
                                                                       : 0;
    return first_deref(path->steps + skip, path->depth - skip)
        < path->depth - skip;
}

static void
map_param_place(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
                bool inline_tail, bool writes, pt_set_t *into)
{
    if (fresh_entry_path(call, path)) return;
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
        pt_resolve_places(pt, loc, writes, into);
    }
    core_list_free(items);
}

static void
map_param_path(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
               pt_set_t *into)
{
    map_param_place(pt, call, path, false, false, into);
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
        ir_summary_t *callee = call_summary(pt->summaries, pt->f,
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

static core_hash_value_t
places_hash(const pt_set_t *places, uint8_t mode)
{
    core_hash_value_t hash = places->len * 8 + mode;
    for (size_t p = 0; p < places->len; p++) {
        const pt_item_t *item = &places->data[p];
        hash = hash * 1000003
            ^ (spath_hash((void *)&item->loc.path) * 31 + item->loc.site);
        hash = hash * 31
            + ((core_hash_value_t)item->kind << 16
               | (core_hash_value_t)item->flow << 8 | item->deep)
            + item->depth;
        for (uint32_t s = 0; s < item->depth; s++)
            hash = hash * 31 + core_string_hash(item->steps[s]);
        hash = hash * 31 + (core_hash_value_t)(uintptr_t)item->origin;
    }
    return hash;
}

static bool
identical_place(const pt_item_t *a, const pt_item_t *b)
{
    const ir_spath_t *x = &a->loc.path, *y = &b->loc.path;
    if (a->kind != b->kind || a->flow != b->flow || a->deep != b->deep
        || a->depth != b->depth || a->origin != b->origin
        || a->loc.site != b->loc.site || x->flags != y->flags
        || x->type != y->type || compare_path_roots(x, y)
        || x->depth != y->depth)
        return false;
    for (uint32_t s = 0; s < a->depth; s++)
        if (!core_string_eq(a->steps[s], b->steps[s])) return false;
    for (uint32_t s = 0; s < x->depth; s++)
        if (!core_string_eq(x->steps[s], y->steps[s])) return false;
    return true;
}

static core_hash_value_t
places_memo_hash(void *key)
{
    return ((const places_memo_t *)key)->hash;
}

static bool
places_memo_eq(const void *a, const void *b)
{
    const places_memo_t *x = a, *y = b;
    if (x->hash != y->hash || x->mode != y->mode
        || x->places.len != y->places.len)
        return false;
    for (size_t p = 0; p < x->places.len; p++)
        if (!identical_place(&x->places.data[p], &y->places.data[p]))
            return false;
    return true;
}

// places_value, remembered by places and mode. Many of a callee's paths
// resolve to the same places (every path under a deeply aliased parameter
// reaches all of it), and each recomputed its value separately, merging
// every place's contents again; a function whose calls kept changing what
// those places hold spent nearly all its solve doing that. The entry is an
// ordinary memo, so it is recomputed whenever anything it read changes.
static void
shared_places_value(pt_t *pt, pt_set_t *places, uint8_t mode,
                    pt_set_t *into)
{
    if (pt->reading.len || pt->delta_since) {
        places_value(pt, places, mode, into);
        return;
    }
    places_memo_t  probe = {.places = *places, .mode = mode,
                            .hash   = places_hash(places, mode)};
    bool           found = false;
    places_memo_t *entry = core_dict_get(&pt->places_memo, &probe, &found);
    if (!found) {
        entry  = core_alloc(places_memo_t);
        *entry = (places_memo_t){.mode = mode, .hash = probe.hash};
        for (size_t p = 0; p < places->len; p++)
            core_list_push(entry->places, places->data[p]);
        core_dict_put(&pt->places_memo, entry, entry);
    }
    if (memo_stale(pt, &entry->memo)) {
        pt_memo_frame_t frame;
        uint64_t        cuts = pt->cuts;
        entry->items.len = 0;
        memo_begin(pt, &entry->memo, &frame);
        places_value(pt, places, mode, &entry->items);
        memo_end(pt, &entry->memo, &frame);
        if (pt->cuts != cuts) entry->memo.computed = false;
    }
    memo_use(pt, &entry->memo);
    set_merge(pt, into, &entry->items);
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
        shared_places_value(pt, &places, mode, &entry->items);
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
    if (fresh_entry_path(call, path)) return;
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

typedef struct {
    core_list_t(summary_type_ptr_t) vars;
    core_list_t(summary_type_ptr_t) values;
} type_binding_t;

// Matches `pattern` (a callee's signature) against `concrete` (the
// signature at a call to it), collecting what each of the pattern's
// unbound variables stands for. Binds nothing: false unless the two have
// the same shape throughout and every variable stands for one type.
static bool
bind_types(type_binding_t *b, th_type *pattern, th_type *concrete,
           uint32_t depth)
{
    pattern  = th_resolve(pattern);
    concrete = th_resolve(concrete);
    if (!pattern || !concrete) return pattern == concrete;
    if (depth > 64) return false;
    if (th_kind_of(pattern) == TH_VAR) {
        for (size_t v = 0; v < b->vars.len; v++)
            if (b->vars.data[v] == pattern)
                return th_same_type(b->values.data[v], concrete);
        core_list_push(b->vars, pattern);
        core_list_push(b->values, concrete);
        return true;
    }
    if (pattern == concrete) return true;
    if (!th_same_shape(pattern, concrete)) return false;
    size_t children = th_child_count(pattern);
    if (children != th_child_count(concrete)) return false;
    for (size_t c = 0; c < children; c++)
        if (!bind_types(b, th_child(pattern, c), th_child(concrete, c),
                        depth + 1))
            return false;
    return true;
}

// The type of storage a callee allocates as this call fixes it: the
// callee's (generic) heap type with its variables replaced by what they
// stand for here. Null when the callee's signature doesn't match the call
// exactly, or nothing in the type changes.
static th_type *
heap_instance(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
              th_type *type, th_ctx *types)
{
    if (!type || !types || !callee->function || !callee->function->type
        || th_is_closed(types, type))
        return nullptr;
    th_type *pattern = th_resolve(callee->function->type);
    if (pattern && th_kind_of(pattern) == TH_INDIRECTION)
        pattern = th_resolve(th_ind_referent(pattern));
    if (pattern && th_kind_of(pattern) == TH_BINDER)
        pattern = th_resolve(th_binder_body(pattern));
    th_type *concrete = ir_call_function_type(call);
    if (!pattern || !concrete || th_kind_of(pattern) != TH_FUNCTION)
        return nullptr;
    type_binding_t binding = {core_list_new(summary_type_ptr_t),
                              core_list_new(summary_type_ptr_t)};
    th_type *instance = nullptr;
    if (bind_types(&binding, pattern, concrete, 0) && binding.vars.len)
        instance = th_substitute(types, type, binding.vars.data,
                                 binding.values.data, binding.vars.len);
    core_list_free(binding.vars);
    core_list_free(binding.values);
    (void)pt;
    return instance;
}

// Storage a callee allocates, as a site of this call. Its type is the
// callee's heap type as this call instantiates it. When that type is
// closed, the site's origin (its name in this function's summary) is the
// callee's refined by the type: storage of two different types is never
// the same object, so callers keep, say, a List[string]'s storage apart
// from a List[Rule]'s that one generic allocation made. A type still open
// keeps the callee's origin, shared as before.
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
    th_ctx  *types = pt->f->owner ? ir_module_types(pt->f->owner) : nullptr;
    th_type *type  = heap ? heap->type : nullptr;
    th_type *found_type = heap
        ? heap_instance(pt, call, callee, type, types) : nullptr;
    uint32_t named = origin;
    if (found_type) {
        type = found_type;
        if (th_is_closed(types, type)) {
            uint64_t parts[2] = {origin, th_hash(types, type)};
            uint64_t hash     = core_xxh3_64bits_raw(parts, sizeof parts);
            named = (uint32_t)(hash ^ hash >> 32) | 1;
        }
    }
    *key = probe;
    site = pt->sites.len;
    core_list_push(pt->sites, ((pt_site_t){
        .key = key, .contents = (pt_set_t){0}, .aliases = (alias_list_t){0},
        .type = type, .elements = heap && heap->elements,
        .call = call, .origin = named, .base = origin}));
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
        // Nothing writes through a string (see resolve_into).
        if (!value && immutable_place(pt, &loc)) return;
        if (!value) string_value_only(pt, &loc);
        if (!value || root) set_add(pt, into, item_at(loc));
        else if (mode == MAP_DERIVED) set_add(pt, into, contents_item(loc));
        else content_at(pt, &loc, into);
    }
    else if (path->root == IR_SPLACE_PARAM && value)
        map_param_value(pt, call, path, mode == MAP_DERIVED, into);
    else if (path->root == IR_SPLACE_PARAM)
        map_param_place(pt, call, path, inline_tail, true, into);
    else {
        pt_loc_t loc = path_loc(path);
        if (inline_tail && loc_deep(&loc)
            && loc.path.root != IR_SPLACE_UNKNOWN)
            loc.path.flags |= SPATH_INLINE;
        if (!value && immutable_place(pt, &loc)) return;
        if (!value) string_value_only(pt, &loc);
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

static bool
same_dest_sources(const flow_dest_t *a, const flow_dest_t *b, bool values)
{
    return same_bits(&a->data, &b->data)
        && same_bits(&a->control, &b->control)
        && (!values || (same_bits(&a->alias, &b->alias)
                        && same_bits(&a->copy, &b->copy)));
}

static const uint32_t *
dest_groups(core_dict_t *cache, const ir_summary_t *callee,
            ir_sflow_table_t table, bool values)
{
    const void *key   = (const char *)callee + table;
    bool        found = false;
    uint32_t   *groups = core_dict_get(cache, (void *)key, &found);
    if (found) return groups;
    const flow_table_t *flows = &callee->flows[table];
    groups = core_alloc_array(uint32_t, flows->dests.len + 1);
    for (size_t d = 0; d < flows->dests.len; d++) {
        groups[d] = (uint32_t)d;
        for (size_t e = 0; e < d; e++)
            if (groups[e] == e
                && same_dest_sources(&flows->dests.data[d],
                                     &flows->dests.data[e], values)) {
                groups[d] = (uint32_t)e;
                break;
            }
    }
    core_dict_put(cache, (void *)key, groups);
    return groups;
}

static const uint32_t *
flow_groups_of(pt_t *pt, const ir_summary_t *callee, ir_sflow_table_t table)
{
    return dest_groups(&pt->flow_groups, callee, table, false);
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

// A write path's place, for finding the one a store destination names.
static core_hash_value_t
written_place_hash(void *key)
{
    const ir_spath_t *path = key;
    uint64_t head[4] = {
        (uint64_t)path->output << 8 | (uint64_t)path->root,
        path->root == IR_SPLACE_PARAM || path->root == IR_SPLACE_SOURCE
                || path->root == IR_SPLACE_HEAP ? path->param : 0,
        named_root(path->root) ? (uint64_t)(uintptr_t)path->global : 0,
        path->depth};
    core_hash_value_t hash = core_hash_raw(head, sizeof(head));
    for (uint32_t s = 0; s < path->depth; s++)
        hash = hash * 31 + core_string_hash(path->steps[s]);
    return hash;
}

static bool
written_place_eq(const void *a, const void *b)
{
    const ir_spath_t *x = a, *y = b;
    return same_path_root(x, y) && x->depth == y->depth
        && steps_prefix(x->depth, x->steps, y->depth, y->steps);
}

// Whether the callee surely writes each of its store destinations: the
// first non-deep write path to the destination's place says.
static const bool *
must_dests(pt_t *pt, const ir_summary_t *callee)
{
    const flow_table_t *flows = &callee->flows[IR_SFLOW_STORES];
    bool  found = false;
    bool *must  = core_dict_get(&pt->must_dests, (void *)callee, &found);
    if (found) return must;
    must = core_alloc_array(bool, flows->dests.len + 1);
    bool returns = !(callee->flags & (IR_SUMMARY_DIVERGES
                                      | IR_SUMMARY_NO_RETURN));
    bool fails   = callee->flags & IR_SUMMARY_MAY_FAIL;
    core_dict_t writes;
    core_dict_init(&writes, written_place_hash, written_place_eq);
    for (size_t p = 0; p < callee->paths.len; p++) {
        const ir_spath_t *path = &callee->paths.data[p];
        if (!(path->flags & IR_SPATH_WRITE) || (path->flags & IR_SPATH_DEEP))
            continue;
        bool seen = false;
        (void)core_dict_get(&writes, (void *)path, &seen);
        if (!seen) core_dict_put(&writes, (void *)path, (void *)path);
    }
    for (size_t d = 0; d < flows->dests.len; d++) {
        const ir_spath_t *to = flows->dests.data[d].to;
        if (to->flags & IR_SPATH_DEEP) continue;
        bool              seen = false;
        const ir_spath_t *path = core_dict_get(&writes, (void *)to, &seen);
        must[d] = seen && (!returns || (path->flags & IR_SPATH_MUST_SUCCESS))
            && (!fails || (path->flags & IR_SPATH_MUST_FAILURE));
    }
    core_dict_free(&writes);
    core_dict_put(&pt->must_dests, (void *)callee, must);
    return must;
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
    const uint32_t     *shared  = dest_groups(&pt->value_groups, callee,
                                              IR_SFLOW_STORES, true);
    size_t             *paired  = core_alloc_array(size_t,
                                                   flows->dests.len + 1);
    core_list_t(store_pair_t) pairs = core_list_new(store_pair_t);
    mapped_sources_t    mapped;
    mapped_sources_init(&mapped, call, callee, IR_SFLOW_STORES);
    for (size_t d = 0; d < flows->dests.len; d++) {
        store_pair_t pair = {.must = sole && must_dests(pt, callee)[d]};
        size_t source = shared[d] == d ? SIZE_MAX : paired[shared[d]];
        if (shared[d] == d) flowed_items(pt, &mapped, groups, d, &pair.from);
        for (size_t i = 0; source < pairs.len
                           && i < pairs.data[source].from.len; i++)
            core_list_push(pair.from, pairs.data[source].from.data[i]);
        paired[d] = pairs.len;
        if (!pair.from.len && !pair.must) {
            paired[d] = SIZE_MAX;
            core_list_free(pair.from);
            continue;
        }
        map_location(pt, call, callee, flows->dests.data[d].to, MAP_PLACE,
                     &pair.to);
        settled = settled && (!pair.must || pair.to.len);
        core_list_push(pairs, pair);
    }
    core_free(paired);
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
        uint8_t escape = ir_summary_param_escape(callee, param);
        if (!escape) continue;
        pt_set_t items = (pt_set_t){0};
        escaping_items(pt, call, callee, param, escape, &items);
        for (size_t w = 0; w < written.len; w++)
            pt_publish(pt, &items, &written.data[w].loc);
        core_list_free(items);
    }
    core_list_free(written);
    call_result(pt, call, callee, result);
    return true;
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
        // An `out` or `modify` argument is the caller's place, which the
        // call borrows: what the place holds escapes to the unknown callee,
        // and can be stored anywhere it writes, but not the place itself
        // (as escaping_items has it for a known callee that keeps no
        // reference to it). An unknown callee is reached through a function
        // reference, and no function that keeps a reference to a by-
        // reference parameter can be referred to (n00b's N00B-S0106).
        th_type *signature = ir_instr_call_signature(call);
        signature = signature ? th_resolve(signature) : nullptr;
        pt_set_t *arguments = core_alloc_array(pt_set_t, call->n_operands);
        pt_set_t written = (pt_set_t){0};
        for (uint32_t o = 1; arguments && o < call->n_operands; o++) {
            pt_read(pt, referenced(call, o), &arguments[o]);
            bool borrowed = call->op == IR_OP_CALL && signature
                && th_kind_of(signature) == TH_FUNCTION
                && o - 1 < th_func_nparams(signature)
                && th_mode_by_reference(th_func_param(signature, o - 1)->mode);
            pt_set_t held = (pt_set_t){0};
            if (borrowed) pt_load(pt, &arguments[o], &held);
            pt_set_t *escaping = borrowed ? &held : &arguments[o];
            pt_publish(pt, escaping, &unknown);
            merge_widened(pt, &written, escaping, false);
            core_list_free(held);
        }
        set_add(pt, &written, deep_item(unknown));
        // Known callees among the targets may write what they read too.
        if (callees->callees.len) merge_widened(pt, &written, &reach, false);
        for (uint32_t o = 1; arguments && o < call->n_operands; o++) {
            pt_set_t *items = &arguments[o];
            for (size_t i = 0; i < items->len; i++)
                if (items->data[i].loc.site != PT_EXTERNAL
                    && items->data[i].flow == PT_SAME) {
                    pt_loc_t whole = items->data[i].loc;
                    whole.path.flags |= IR_SPATH_DEEP;
                    write_reach(pt, &whole, &written);
                }
            core_list_free(*items);
        }
        core_free(arguments);
        core_list_free(written);
        set_add(pt, &result, deep_item(unknown));
    }
    bool settled = true;
    for (size_t t = 0; t < callees->callees.len; t++)
        settled = call_escapes(pt, call, callees->callees.data[t], &reach,
                               &result) && settled;
    if (settled) settle_writes(pt, call);
    pt_define(pt, call, &result);
    core_list_free(reach);
    core_list_free(result);
}
