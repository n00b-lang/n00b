#include "internal.h"

enum { SET_SCAN = 16 };

static int object_order(const pt_loc_t *a, const pt_loc_t *b);

static void trim_repeats(pt_item_t *item);

static void canonical_item(const pt_t *pt, pt_item_t *item);
static bool pointer_reach(const pt_t *pt, const pt_item_t *item);

static void loc_push(const pt_t *pt, pt_loc_t *loc, core_string_t step);

static uint64_t
unknown_position(const ir_instr_t *in)
{
    return in ? ((uint64_t)(in->block ? in->block->index : 0) << 32
                 | in->index) + 1
              : UINT64_MAX;
}

static void
note_unknown(pt_t *pt, core_string_t reason, ir_func_t *callee)
{
    uint64_t at = unknown_position(pt->at);
    if (pt->unknown_at && pt->unknown_at <= at) return;
    pt->unknown_at = at;
    pt->unknown    = (ir_switness_t){
        .flag   = IR_SWITNESS_UNKNOWN,
        .callee = callee,
        .module = callee ? ir_module_name(callee->owner) : (core_string_t){0},
        .name   = callee ? callee->name : reason,
        .line   = pt->at ? pt->at->span.start_line : 0,
        .column = pt->at ? pt->at->span.start_col : 0,
    };
}

static uintptr_t
alias_root_key(uintptr_t root)
{
    return root | (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
}

static uintptr_t
contents_key(uint32_t site)
{
    return (uintptr_t)site << 2 | 1;
}

static uintptr_t
aliases_key(uint32_t site)
{
    return (uintptr_t)site << 2 | 3;
}

static void
pt_depend(pt_t *pt, uintptr_t key)
{
    if (pt->resolving) core_list_push(*pt->resolving, (uint64_t)key);
    if (pt->reader == PT_NO_READER) return;
    bool                   found   = false;
    core_list_t(uint32_t) *readers = core_dict_get(&pt->readers,
                                                   (void *)key, &found);
    if (!readers) {
        readers  = core_alloc(core_list_t(uint32_t));
        *readers = core_list_new(uint32_t);
        core_dict_put(&pt->readers, (void *)key, readers);
    }
    for (size_t r = 0; r < readers->len; r++)
        if (readers->data[r] == pt->reader) return;
    core_list_push(*readers, pt->reader);
}

static void
pt_notify(pt_t *pt, uintptr_t key)
{
    pt->generation++;
    core_dict_put(&pt->changed, (void *)key, (void *)(uintptr_t)pt->generation);
    bool                   found   = false;
    core_list_t(uint32_t) *readers = core_dict_get(&pt->readers,
                                                   (void *)key, &found);
    for (size_t r = 0; readers && r < readers->len; r++)
        if (!pt->dirty[readers->data[r]]) {
            pt->dirty[readers->data[r]] = true;
            pt->pending                 = true;
        }
}

static bool
loc_deep(const pt_loc_t *loc)
{
    return loc->path.flags & IR_SPATH_DEEP;
}

static pt_loc_t
external_loc(ir_splace_kind_t root, uint32_t param, const ir_global_t *global)
{
    return (pt_loc_t){.site = PT_EXTERNAL, .path = {
        .root = root, .param = param,
        .global = global ? ir_global_identity((ir_global_t *)global)
                         : nullptr}};
}

static pt_loc_t
unknown_loc(void)
{
    pt_loc_t loc = external_loc(IR_SPLACE_UNKNOWN, 0, nullptr);
    loc.path.flags = IR_SPATH_DEEP;
    return loc;
}

static pt_loc_t
global_loc(const ir_global_t *global)
{
    bool thread = ir_global_storage_duration((ir_global_t *)global)
        == IR_STORAGE_DURATION_THREAD;
    return external_loc(thread ? IR_SPLACE_THREAD : IR_SPLACE_GLOBAL, 0,
                        global);
}

static pt_loc_t
path_loc(const ir_spath_t *path)
{
    if (path->root == IR_SPLACE_HEAP || path->root == IR_SPLACE_UNKNOWN)
        return unknown_loc();
    pt_loc_t loc = {.site = PT_EXTERNAL, .path = *path};
    loc.path.flags &= IR_SPATH_DEEP;
    return loc;
}

static pt_loc_t
site_loc(uint32_t site)
{
    return (pt_loc_t){.site = site};
}

static bool
same_object(const pt_loc_t *a, const pt_loc_t *b)
{
    return a->site == b->site
        && (a->site != PT_EXTERNAL || same_path_root(&a->path, &b->path));
}

static uint32_t
address_base(const pt_item_t *item)
{
    const ir_spath_t *path = &item->loc.path;
    uint32_t          base = item->loc.site == PT_EXTERNAL ? last_deref(path)
                                                           : UINT32_MAX;
    return base == UINT32_MAX ? path->depth : base;
}

static bool
item_covers(const pt_item_t *a, const pt_item_t *b)
{
    if (a->origin != b->origin || a->flow > b->flow
        || (a->flow == PT_SAME) != (b->flow == PT_SAME))
        return false;
    if (a->kind != b->kind
        && !(a->flow == PT_SAME && a->kind == PT_CONTENTS
             && b->kind == PT_ADDRESS
             && (a->loc.path.flags & IR_SPATH_DEEP)
             && !(a->loc.path.flags & SPATH_INLINE)
             && steps_prefix(a->loc.path.depth, a->loc.path.steps,
                             address_base(b), b->loc.path.steps)))
        return false;
    return !object_order(&a->loc, &b->loc)
        && steps_cover(a->depth, a->deep, a->steps, b->depth, b->deep,
                       b->steps)
        && place_covers(&a->loc.path, &b->loc.path);
}

static int
object_order(const pt_loc_t *a, const pt_loc_t *b)
{
    if (a->site != b->site) return a->site < b->site ? -1 : 1;
    if (a->site != PT_EXTERNAL) return 0;
    const ir_spath_t *x = &a->path, *y = &b->path;
    if (x->output != y->output) return x->output < y->output ? -1 : 1;
    if (x->root != y->root) return x->root < y->root ? -1 : 1;
    if ((x->root == IR_SPLACE_PARAM || x->root == IR_SPLACE_SOURCE
         || x->root == IR_SPLACE_HEAP) && x->param != y->param)
        return x->param < y->param ? -1 : 1;
    if (!named_root(x->root) || x->global == y->global) return 0;
    return (uintptr_t)x->global < (uintptr_t)y->global ? -1 : 1;
}

static size_t
object_bound(const pt_set_t *set, const pt_loc_t *loc, bool upper)
{
    size_t low = 0, high = set->len;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int    order  = object_order(&set->data[middle].loc, loc);
        if (order < 0 || (upper && !order)) low = middle + 1;
        else high = middle;
    }
    return low;
}

static int
step_order(core_string_t a, core_string_t b)
{
    if (a.data != b.data)
        return (uintptr_t)a.data < (uintptr_t)b.data ? -1 : 1;
    if (a.u8_bytes != b.u8_bytes) return a.u8_bytes < b.u8_bytes ? -1 : 1;
    return 0;
}

static int
steps_order(const core_string_t *a, uint32_t a_depth, const core_string_t *b,
            uint32_t b_depth)
{
    if (a == b) return a_depth == b_depth ? 0 : a_depth < b_depth ? -1 : 1;
    uint32_t common = a_depth < b_depth ? a_depth : b_depth;
    for (uint32_t s = 0; s < common; s++) {
        int order = step_order(a[s], b[s]);
        if (order) return order;
    }
    return a_depth == b_depth ? 0 : a_depth < b_depth ? -1 : 1;
}

static int
group_order(const pt_item_t *a, const pt_item_t *b)
{
    if (a->origin != b->origin)
        return (uintptr_t)a->origin < (uintptr_t)b->origin ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return 0;
}

static int
item_order(const pt_item_t *a, const pt_item_t *b)
{
    int order = group_order(a, b);
    if (order) return order;
    order = steps_order(a->loc.path.steps, a->loc.path.depth,
                        b->loc.path.steps, b->loc.path.depth);
    if (order) return order;
    uint8_t mask = IR_SPATH_DEEP | SPATH_INLINE;
    if ((a->loc.path.flags & mask) != (b->loc.path.flags & mask))
        return (a->loc.path.flags & mask) < (b->loc.path.flags & mask) ? -1
                                                                        : 1;
    order = steps_order(a->steps, a->depth, b->steps, b->depth);
    if (order) return order;
    if (a->deep != b->deep) return a->deep ? 1 : -1;
    if (a->flow != b->flow) return a->flow < b->flow ? -1 : 1;
    return 0;
}

static int
place_order(const pt_item_t *x, const pt_item_t *key, uint32_t depth,
            bool prefix)
{
    int order = group_order(x, key);
    if (order) return order;
    uint32_t own = x->loc.path.depth;
    if (prefix && own > depth) own = depth;
    return steps_order(x->loc.path.steps, own, key->loc.path.steps, depth);
}

static size_t
place_bound(const pt_set_t *set, size_t low, size_t high,
            const pt_item_t *key, uint32_t depth, bool prefix, bool upper)
{
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int    order  = place_order(&set->data[middle], key, depth, prefix);
        if (order < 0 || (upper && !order)) low = middle + 1;
        else high = middle;
    }
    return low;
}

static size_t
step_bound(const pt_set_t *set, size_t low, size_t high, uint32_t at,
           core_string_t step, bool upper)
{
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int    order  = step_order(set->data[middle].loc.path.steps[at], step);
        if (order < 0 || (upper && !order)) low = middle + 1;
        else high = middle;
    }
    return low;
}

static bool
set_lookup(const pt_set_t *set, const pt_item_t *item, size_t *low,
           size_t *high)
{
    size_t start = object_bound(set, &item->loc, false);
    size_t end   = object_bound(set, &item->loc, true);
    *low  = start;
    *high = end;
    if (end - start <= SET_SCAN) {
        for (size_t i = start; i < end; i++)
            if (item_covers(&set->data[i], item)) return true;
        return false;
    }
    const ir_spath_t *path = &item->loc.path;
    size_t from  = place_bound(set, start, end, item, 0, true, false);
    size_t to    = place_bound(set, from, end, item, 0, true, true);
    size_t   exact = from, below = from, outer = from;
    uint32_t k     = 0;
    for (;; k++) {
        for (exact = from;
             exact < to && set->data[exact].loc.path.depth == k; exact++)
            if (item_covers(&set->data[exact], item)) return true;
        if (k == path->depth || exact == to) break;
        below = exact;
        outer = to;
        if (!step_eq(path->steps[k], value_step)) {
            size_t value = step_bound(set, exact, to, k, value_step, false);
            for (size_t i = value;
                 i < to && set->data[i].loc.path.depth == k + 1
                 && step_eq(set->data[i].loc.path.steps[k], value_step); i++)
                if (item_covers(&set->data[i], item)) return true;
        }
        from = step_bound(set, exact, to, k, path->steps[k], false);
        to   = step_bound(set, from, to, k, path->steps[k], true);
    }
    bool wide = loc_deep(&item->loc) && !(path->flags & SPATH_INLINE);
    bool full = from < to && set->data[from].loc.path.depth >= path->depth;
    bool pointee = wide && k == path->depth && k
        && step_eq(path->steps[k - 1], value_step);
    *low  = pointee ? below : from;
    *high = pointee ? outer : !full ? from : wide ? to : exact;
    return false;
}

static int
set_order(const pt_item_t *a, const pt_item_t *b)
{
    int order = object_order(&a->loc, &b->loc);
    return order ? order : item_order(a, b);
}

static void
drop_covered(pt_set_t *set, const pt_item_t *item, size_t low, size_t high)
{
    size_t kept = low;
    for (size_t i = low; i < high; i++)
        if (!item_covers(item, &set->data[i]))
            set->data[kept++] = set->data[i];
    if (kept == high) return;
    memmove(&set->data[kept], &set->data[high],
            (set->len - high) * sizeof(pt_item_t));
    set->len -= high - kept;
}

static bool
insert_item(pt_set_t *set, const pt_item_t *item)
{
    size_t low = 0, high = 0;
    if (set_lookup(set, item, &low, &high)) return false;
    bool same = item->flow == PT_SAME;
    if (same && item->kind == PT_ADDRESS) {
        pt_item_t reach = *item;
        reach.kind = PT_CONTENTS;
        reach.loc.path.depth = address_base(item);
        reach.loc.path.flags |= IR_SPATH_DEEP;
        reach.loc.path.flags &= (uint8_t)~SPATH_INLINE;
        size_t from = 0, to = 0;
        if (set_lookup(set, &reach, &from, &to)) return false;
    }
    else if (same && (item->loc.path.flags & IR_SPATH_DEEP)
             && !(item->loc.path.flags & SPATH_INLINE)) {
        pt_item_t address = *item;
        address.kind = PT_ADDRESS;
        size_t from = 0, to = 0;
        set_lookup(set, &address, &from, &to);
        drop_covered(set, item, from, to);
        set_lookup(set, item, &low, &high);
    }
    drop_covered(set, item, low, high);
    size_t at = 0, top = set->len;
    while (at < top) {
        size_t middle = at + (top - at) / 2;
        if (set_order(&set->data[middle], item) < 0) at = middle + 1;
        else top = middle;
    }
    core_list_push(*set, *item);
    memmove(&set->data[at + 1], &set->data[at],
            (set->len - 1 - at) * sizeof(pt_item_t));
    set->data[at] = *item;
    return true;
}

static bool
set_add(const pt_t *pt, pt_set_t *set, pt_item_t item)
{
    bool added = false;
    for (;;) {
        trim_repeats(&item);
        canonical_item(pt, &item);
        bool     dependence = item.flow != PT_SAME
            && item.kind == PT_ADDRESS && item.loc.site == PT_EXTERNAL;
        uint32_t base = dependence ? last_deref(&item.loc.path) : UINT32_MAX;
        if (dependence && base == UINT32_MAX && loc_deep(&item.loc)) {
            item.kind = PT_CONTENTS;
            item.loc.path.flags &= (uint8_t)~SPATH_INLINE;
        }
        if (base != UINT32_MAX) {
            pt_item_t pointer = item;
            pointer.kind            = PT_CONTENTS;
            pointer.loc.path.depth  = base;
            pointer.loc.path.flags &= (uint8_t)~(IR_SPATH_DEEP
                                                 | SPATH_INLINE);
            added |= insert_item(set, &pointer);
            if (!loc_deep(&item.loc)) return added;
            item.kind = PT_CONTENTS;
            continue;
        }
        if (!pointer_reach(pt, &item)) return insert_item(set, &item) || added;
        pt_item_t pointer = item;
        pointer.loc.path.flags &= (uint8_t)~IR_SPATH_DEEP;
        pt_item_t pointee = pointer;
        loc_push(pt, &pointee.loc, value_step);
        if (loc_deep(&pointee.loc)
            || pointee.loc.path.depth != item.loc.path.depth + 1)
            return insert_item(set, &item) || added;
        canonical_item(pt, &pointer);
        added |= insert_item(set, &pointer);
        item = pointee;
        item.loc.path.flags |= IR_SPATH_DEEP;
    }
}

static void
trim_repeats(pt_item_t *item)
{
    for (uint32_t s = 1; s < item->depth; s++)
        for (uint32_t t = 0; t < s; t++)
            if (step_eq(item->steps[s], item->steps[t])) {
                item->depth = s;
                item->deep  = true;
                return;
            }
}

static void
pt_add(pt_t *pt, pt_set_t *set, uintptr_t key, pt_item_t item)
{
    if ((key & 3) == 1) typed_placement(pt, (uint32_t)(key >> 2), &item);
    if (set_add(pt, set, item)) pt_notify(pt, key);
}

static pt_item_t
item_at(pt_loc_t loc)
{
    return (pt_item_t){.kind = PT_ADDRESS, .loc = loc};
}

static pt_item_t
contents_item(pt_loc_t loc)
{
    return (pt_item_t){.kind = PT_CONTENTS, .loc = loc};
}

static pt_item_t
deep_item(pt_loc_t loc)
{
    loc.path.flags |= IR_SPATH_DEEP;
    return (pt_item_t){.deep = true, .loc = loc};
}

static pt_item_t
deepened(const pt_item_t *item)
{
    pt_item_t deep = deep_item(item->loc);
    deep.flow   = item->flow;
    deep.kind   = item->kind;
    deep.origin = item->origin;
    return deep;
}

static pt_item_t
anywhere_below(pt_item_t item)
{
    item.deep  = true;
    item.steps = nullptr;
    item.depth = 0;
    return item;
}

static void
merge_widened(const pt_t *pt, pt_set_t *into, pt_set_t *items, bool below)
{
    for (size_t i = 0; i < items->len; i++)
        set_add(pt, into, below ? anywhere_below(items->data[i])
                                : deepened(&items->data[i]));
    core_list_free(*items);
}

static void
unresolved(pt_t *pt, pt_set_t *into)
{
    note_unknown(pt, CORE_STRING_STATIC("an operand the analysis cannot "
                                        "resolve"), nullptr);
    set_add(pt, into, deep_item(unknown_loc()));
}

static void
loc_push(const pt_t *pt, pt_loc_t *loc, core_string_t step)
{
    if (loc_deep(loc)) return;
    th_type *type = loc_type(pt, loc, loc->path.depth);
    if (type && !step_fits(pt, type, step)) {
        loc->path.flags |= IR_SPATH_DEEP;
        return;
    }
    if (step_eq(step, value_step) && recursive_object(pt, loc, type)) return;
    bool repeated = false;
    for (uint32_t s = 0; !repeated && s < loc->path.depth; s++)
        repeated = step_eq(loc->path.steps[s], step)
            && recursive_step(pt, loc, s);
    if (repeated) loc->path.flags |= IR_SPATH_DEEP;
    else path_push(&loc->path, step);
}

static void
loc_extend(const pt_t *pt, pt_loc_t *loc, const core_string_t *steps,
           uint32_t depth, bool deep)
{
    for (uint32_t s = 0; s < depth; s++) loc_push(pt, loc, steps[s]);
    if (deep) loc->path.flags |= IR_SPATH_DEEP;
}

static pt_loc_t
location_of(const pt_t *pt, const pt_item_t *address)
{
    pt_loc_t loc = address->loc;
    if (address->kind != PT_CONTENTS || loc.site != PT_EXTERNAL) return loc;
    if (loc_deep(&loc)) {
        loc.path.flags &= (uint8_t)~SPATH_INLINE;
        return loc;
    }
    if (!slot_pointer(pt, address)) return loc;
    loc_push(pt, &loc, value_step);
    return loc;
}

static void
extend_through(const pt_t *pt, pt_loc_t *next, const core_string_t *steps,
               uint32_t depth, bool deep)
{
    if (depth && core_string_eq(steps[0], value_step)) {
        steps++;
        depth--;
    }
    loc_extend(pt, next, steps, depth, deep);
}

static void
extend_tail(const pt_t *pt, pt_loc_t *loc, const core_string_t *steps,
            uint32_t depth, bool deep, bool inline_tail)
{
    loc_extend(pt, loc, steps, depth, false);
    if (!deep) return;
    bool exact = !loc_deep(loc);
    loc->path.flags |= IR_SPATH_DEEP;
    if (inline_tail && exact) loc->path.flags |= SPATH_INLINE;
}

static pt_item_t
item_under(const core_string_t *steps, uint32_t depth, bool deep,
           pt_item_t item)
{
    uint32_t  tail   = deep ? 0 : item.depth;
    pt_item_t placed = {.loc = item.loc, .deep = deep || item.deep,
                        .kind = item.kind, .flow = item.flow,
                        .depth = depth + tail};
    placed.steps = depth ? steps_join(steps, depth, item.steps, tail)
                         : item.steps;
    trim_repeats(&placed);
    return placed;
}

static bool
inline_sequence(const ir_func_t *f, th_type *type)
{
    type = defined_type(f, type);
    uint64_t extent = 0;
    return type && th_kind_of(type) == TH_SEQUENCE
        && th_seq_extent(type, &extent);
}

static bool
pointer_reach(const pt_t *pt, const pt_item_t *item)
{
    const ir_spath_t *path = &item->loc.path;
    return item->kind == PT_CONTENTS
        && item->loc.site == PT_EXTERNAL && (path->flags & IR_SPATH_DEEP)
        && !(path->flags & SPATH_INLINE)
        && known_pointer(pt, &item->loc);
}

static void
canonical_item(const pt_t *pt, pt_item_t *item)
{
    ir_spath_t *path = &item->loc.path;
    if ((path->flags & SPATH_INLINE) && flat_slot(pt, &item->loc))
        path->flags &= (uint8_t)~(IR_SPATH_DEEP | SPATH_INLINE);
    if (item->loc.site != PT_EXTERNAL) return;
    if (item->flow != PT_SAME) return;
    if (item->kind != PT_CONTENTS || loc_deep(&item->loc)
        || !slot_pointer(pt, item))
        return;
    item->loc = location_of(pt, item);
    if (!loc_deep(&item->loc)) item->kind = PT_ADDRESS;
}
