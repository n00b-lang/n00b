#include "internal.h"

#define PT_ALL_EXTERNAL_KEY (UINTPTR_MAX << 2 | PT_EXTERNAL_WRITES_KEY)

static void pt_load(pt_t *pt, const pt_set_t *addresses, pt_set_t *into);
static void set_merge(const pt_t *pt, pt_set_t *into, const pt_set_t *from);

static bool pt_reaches(pt_t *pt, const ir_instr_t *from,
                       const ir_instr_t *to);

static bool alias_through(const pt_t *pt, const pt_item_t *alias,
                          const core_string_t *steps, uint32_t depth,
                          bool deep, pt_loc_t *out);
static void contents_into(pt_t *pt, const pt_loc_t *loc,
                          const pt_set_t *contents, pt_set_t *into);

static void pt_address(pt_t *pt, const ir_value_t *value, pt_set_t *into);

static bool
strong_writer(pt_t *pt, const ir_instr_t *in)
{
    bool released = false;
    core_dict_get(&pt->flow.released, in, &released);
    return !released;
}

static const pt_set_t *
site_contents(pt_t *pt, uint32_t site)
{
    pt_site_t *entry = &pt->sites.data[site];
    if (!entry->local || !pt->at) return &entry->contents;
    pt->positional = true;
    pt_flow_t *flow = &pt->flow;
    if (!flow_state_at(pt, pt->at)) return &entry->contents;
    flow->items.len = 0;
    for (size_t d = 0; d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->site != site
            || !(flow->state[d / 64] >> (d % 64) & 1)) continue;
        bool               present = false;
        const pt_writes_t *writes  = core_dict_get(&flow->writes, def->instr,
                                                   &present);
        const pt_write_t  *write   = &writes->data[def->record];
        for (size_t t = 0; t < write->items.len; t++) {
            const pt_item_t *item = &write->items.data[t];
            if ((def->member && item->flow == PT_SAME)
                || (def->pointers && item->flow != PT_SAME)) continue;
            if (def->member && !item->depth && !item->deep) {
                pt_item_t part = *item;
                loc_push(pt, &part.loc, def->cell);
                part.steps = steps_make(&def->cell, 1);
                part.depth = 1;
                set_add(pt, &flow->items, part);
            }
            else if (def->kill == FLOW_KILL_CELL || item->deep
                     || core_string_eq(item_cell(item), def->cell))
                set_add(pt, &flow->items, *item);
        }
    }
    return &flow->items;
}

static void
pt_read(pt_t *pt, const ir_value_t *value, pt_set_t *into)
{
    if (!value || !tracks(pt, value->type)) return;
    const pt_set_t *from = nullptr;
    switch (value->kind) {
    case IR_VAL_GLOBAL:
        if (!value->global) return;
        if (pointer_type(pt, value->type)) {
            pt_set_t address = (pt_set_t){0};
            set_add(pt, &address, item_at(global_loc(value->global)));
            pt_load(pt, &address, into);
            core_list_free(address);
        }
        else if (!constant_global(value->global)) {
            pt_item_t copy = contents_item(global_loc(value->global));
            copy.flow = PT_COPY;
            set_add(pt, into, copy);
        }
        return;
    case IR_VAL_PARAM:
    case IR_VAL_LOCAL: {
        if (value->kind == IR_VAL_LOCAL && !value->local) {
            unresolved(pt, into);
            return;
        }
        uint32_t site = variable_site(pt, value);
        pt_depend(pt, contents_key(site));
        from = site_contents(pt, site);
        break;
    }
    case IR_VAL_INSTR: {
        bool  found = false;
        void *index = core_dict_get(&pt->value_index, value, &found);
        pt_depend(pt, (uintptr_t)value);
        if (found) from = &pt->values.data[(uintptr_t)index];
        break;
    }
    default:
        return;
    }
    if (from) set_merge(pt, into, from);
}

static void
pt_address(pt_t *pt, const ir_value_t *value, pt_set_t *into)
{
    if (value && value->kind == IR_VAL_GLOBAL && value->global)
        set_add(pt, into, item_at(global_loc(value->global)));
    else
        pt_read(pt, value, into);
}

static pt_set_t *
value_set(pt_t *pt, const ir_value_t *value)
{
    bool  found = false;
    void *index = core_dict_get(&pt->value_index, value, &found);
    if (!found) {
        index = (void *)(uintptr_t)pt->values.len;
        core_list_push(pt->values, (pt_set_t){0});
        core_dict_put(&pt->value_index, value, index);
    }
    return &pt->values.data[(uintptr_t)index];
}

static void
define_items(pt_t *pt, const ir_instr_t *in, const pt_set_t *items)
{
    if (!items->len) return;
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        th_type *type = in->dest_local->type;
        if (!tracks(pt, type)) return;
        bool     data = !pointer_type(pt, type);
        uint32_t site = local_site(pt, in->dest_local);
        for (size_t i = 0; i < items->len; i++) {
            pt_item_t item = items->data[i];
            if (data && item.flow == PT_SAME) item.flow = PT_COPY;
            if (constant_item(&item)) continue;
            pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
                   item);
            note_write(pt, site, nullptr, false, item);
        }
        return;
    }
    pt_set_t *target = in->dest && tracks(pt, in->dest->type)
        ? value_set(pt, in->dest) : nullptr;
    bool data = target && !pointer_type(pt, in->dest->type);
    for (size_t i = 0; target && i < items->len; i++) {
        pt_item_t item = items->data[i];
        if (data && item.flow == PT_SAME) item.flow = PT_COPY;
        if (constant_item(&item)) continue;
        pt_add(pt, target, (uintptr_t)in->dest, item);
    }
}

static void
pt_define(pt_t *pt, const ir_instr_t *in, const pt_set_t *items)
{
    ir_pt_fresh_t fresh = pt->hooks && pt->hooks->fresh
        ? pt->hooks->fresh(pt->hooks->context, in) : IR_PT_FRESH_NONE;
    if (fresh == IR_PT_FRESH_NONE) {
        define_items(pt, in, items);
        return;
    }
    uint32_t site = allocation_site(pt, in);
    for (size_t i = 0; fresh == IR_PT_FRESH_HOLDING && i < items->len; i++)
        pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
               item_under(nullptr, 0, true, items->data[i]));
    pt_set_t object = (pt_set_t){0};
    set_add(pt, &object, item_at(site_loc(site)));
    define_items(pt, in, &object);
    core_list_free(object);
}

static void
project(const pt_t *pt, const pt_set_t *from, core_string_t step,
        pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (item.depth) {
            if (!core_string_eq(item.steps[0], step)) continue;
            item.steps++;
            item.depth--;
            set_add(pt, into, item);
            continue;
        }
        if (item.deep) set_add(pt, into, item);
        item.deep = false;
        loc_push(pt, &item.loc, step);
        set_add(pt, into, item);
    }
}

static void
variant_arm(const pt_t *pt, const pt_set_t *from, core_string_t arm,
            pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (item.depth) {
            if (!core_string_eq(item.steps[0], arm)) continue;
            item.steps++;
            item.depth--;
        }
        set_add(pt, into, item);
    }
}

static bool
is_address(const pt_item_t *item)
{
    return item->flow == PT_SAME && (!item->depth || item->deep);
}

static void
address_step(pt_t *pt, const pt_set_t *from, core_string_t step,
             pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (!is_address(&item)) continue;
        if (!slot_pointer(pt, &item)) {
            set_add(pt, into, deepened(&item));
            continue;
        }
        item.loc   = location_of(pt, &item);
        item.depth = 0;
        item.deep  = false;
        item.kind  = PT_ADDRESS;
        loc_push(pt, &item.loc, step);
        set_add(pt, into, item);
    }
}

static void
pointer_offset(pt_t *pt, const pt_set_t *from, pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (!is_address(&item)) continue;
        if (!slot_pointer(pt, &item)) {
            set_add(pt, into, deepened(&item));
            continue;
        }
        bool     contents = item.kind == PT_CONTENTS;
        uint32_t depth    = item.loc.path.depth;
        item.loc   = location_of(pt, &item);
        item.depth = 0;
        item.deep  = false;
        item.kind  = PT_ADDRESS;
        if (contents || !depth
            || !core_string_eq(item.loc.path.steps[depth - 1], element_step))
            loc_push(pt, &item.loc, element_step);
        set_add(pt, into, item);
    }
}

static bool
alias_visible(pt_t *pt, const pt_alias_t *alias)
{
    pt->positional = true;
    for (size_t o = 0; o < alias->origins.len; o++)
        if (pt_reaches(pt, alias->origins.data[o], pt->at)) return true;
    return false;
}

static bool
crosses(pt_t *pt, const pt_loc_t *loc, uint32_t from, uint32_t to)
{
    const flow_bits_t *mask = crossings(pt, loc);
    for (uint32_t i = from; i < to; i++)
        if (bit_has(mask, i)) return true;
    return false;
}

static void
set_merge(const pt_t *pt, pt_set_t *into, const pt_set_t *from)
{
    if (!from->len) return;
    if (!into->len) {
        for (size_t i = 0; i < from->len; i++)
            core_list_push(*into, from->data[i]);
        return;
    }
    if (from->len == 1) {
        set_add(pt, into, from->data[0]);
        return;
    }
    bool    *dropped = core_alloc_array(bool, into->len);
    pt_set_t added   = (pt_set_t){0};
    for (size_t i = 0; i < into->len; i++) dropped[i] = false;
    size_t same = 0;
    for (size_t f = 0; f < from->len; f++) {
        const pt_item_t *item = &from->data[f];
        while (same < into->len && set_order(&into->data[same], item) < 0)
            same++;
        if (same < into->len && !set_order(&into->data[same], item)
            && item_covers(&into->data[same], item)) continue;
        size_t low = 0, high = 0;
        if (set_lookup(into, item, &low, &high)) continue;
        for (size_t i = low; i < high; i++)
            if (!dropped[i] && item_covers(item, &into->data[i]))
                dropped[i] = true;
        core_list_push(added, *item);
    }
    if (added.len) {
        pt_set_t merged = (pt_set_t){0};
        size_t   i = 0, a = 0;
        while (i < into->len || a < added.len) {
            if (i < into->len && dropped[i]) {
                i++;
                continue;
            }
            bool take_added = a < added.len
                && (i == into->len
                    || set_order(&added.data[a], &into->data[i]) < 0);
            core_list_push(merged, take_added ? added.data[a++]
                                              : into->data[i++]);
        }
        core_list_free(*into);
        *into = merged;
    }
    core_list_free(added);
    core_free(dropped);
}

static void
cut_above(pt_t *pt, size_t frame)
{
    for (size_t f = frame + 1; f < pt->reading.len; f++) {
        stored_t *entry = pt->reading.data[f].entry;
        if (entry) entry->partial = true;
    }
}

static const pt_set_t *
stored_at(pt_t *pt, const pt_loc_t *loc)
{
    bool      found = false;
    stored_t *entry = core_dict_get(&pt->stored, (void *)loc, &found);
    if (!found) {
        entry  = core_alloc(stored_t);
        *entry = (stored_t){.loc = *loc};
        core_dict_put(&pt->stored, &entry->loc, entry);
    }
    if (entry->busy) {
        for (size_t f = pt->reading.len; f > 0; f--)
            if (pt->reading.data[f - 1].entry == entry) {
                cut_above(pt, f - 1);
                break;
            }
        return &entry->items;
    }
    bool      known   = false;
    uintptr_t changed = (uintptr_t)core_dict_get(
        &pt->changed, (void *)contents_key(loc->site), &known);
    if (!found || entry->partial || changed > entry->generation) {
        entry->items.len = 0;
        entry->partial   = false;
        entry->busy      = true;
        core_list_push(pt->reading, ((pt_frame_t){*loc, entry}));
        contents_into(pt, loc, &pt->sites.data[loc->site].contents,
                      &entry->items);
        pt->reading.len--;
        entry->busy       = false;
        entry->generation = pt->generation;
    }
    return &entry->items;
}

static void
content_at(pt_t *pt, const pt_loc_t *loc, pt_set_t *into)
{
    pt_depend(pt, contents_key(loc->site));
    pt_depend(pt, aliases_key(loc->site));
    if (pt->sites.data[loc->site].local)
        contents_into(pt, loc, site_contents(pt, loc->site), into);
    else set_merge(pt, into, stored_at(pt, loc));
    const core_string_t *at    = loc->path.steps;
    uint32_t             depth = loc->path.depth;
    const alias_list_t *aliases = &pt->sites.data[loc->site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        if (!alias_visible(pt, &aliases->data[a])) continue;
        pt_loc_t through;
        if (!alias_through(pt, &aliases->data[a].item, at, depth,
                           loc_deep(loc), &through))
            continue;
        set_add(pt, into, contents_item(through));
    }
}

static bool
begin_reading(pt_t *pt, const pt_loc_t *loc)
{
    for (size_t f = pt->reading.len; f > 0; f--)
        if (resolved_eq(&pt->reading.data[f - 1].loc, loc)) {
            cut_above(pt, f - 1);
            return false;
        }
    core_list_push(pt->reading, ((pt_frame_t){*loc, nullptr}));
    return true;
}

static void
contents_into(pt_t *pt, const pt_loc_t *loc, const pt_set_t *contents,
              pt_set_t *into)
{
    const core_string_t *at    = loc->path.steps;
    uint32_t             depth = loc->path.depth;
    bool                 deep  = loc_deep(loc);
    for (size_t i = 0; i < contents->len; i++) {
        const pt_item_t *held  = &contents->data[i];
        bool             under = steps_prefix(depth, at, held->depth,
                                              held->steps);
        bool             over  = steps_prefix(held->depth, held->steps,
                                              depth, at);
        if (!under && !over) continue;
        if (under) {
            pt_item_t item = *held;
            item.steps = held->steps + depth;
            item.depth = held->depth - depth;
            set_add(pt, into, deep ? deepened(&item) : item);
            continue;
        }
        if (held->deep) {
            set_add(pt, into, deepened(held));
            continue;
        }
        const core_string_t *rest  = at + held->depth;
        uint32_t             count = depth - held->depth;
        if (held->kind == PT_CONTENTS
            && (held->flow != PT_SAME || !slot_pointer(pt, held))) {
            pt_item_t item = *held;
            item.steps = nullptr;
            item.depth = 0;
            loc_extend(pt, &item.loc, rest, count, deep);
            set_add(pt, into, item);
            continue;
        }
        if (held->flow != PT_SAME || !step_eq(rest[0], value_step)) {
            set_add(pt, into, deepened(held));
            continue;
        }
        pt_loc_t target = location_of(pt, held);
        extend_through(pt, &target, rest, count, deep);
        if (target.site == PT_EXTERNAL) {
            set_add(pt, into, contents_item(target));
            continue;
        }
        if (!begin_reading(pt, &target)) continue;
        content_at(pt, &target, into);
        pt->reading.len--;
    }
}

static uintptr_t
external_key(const ir_spath_t *path)
{
    uintptr_t root = 0;
    if (path->root == IR_SPLACE_PARAM) root = (uintptr_t)path->param + 1;
    else if (named_root(path->root)) root = (uintptr_t)path->global;
    return root << 2 | PT_EXTERNAL_WRITES_KEY;
}

static pt_external_writes_t *
external_writes_of(pt_t *pt, uintptr_t key, bool create)
{
    bool      found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&pt->external_index,
                                               (void *)key, &found);
    if (found) return &pt->external_writes.data[index];
    if (!create) return nullptr;
    core_dict_put(&pt->external_index, (void *)key,
                  (void *)(uintptr_t)pt->external_writes.len);
    core_list_push(pt->external_writes, (pt_external_writes_t){0});
    return &pt->external_writes.data[pt->external_writes.len - 1];
}

static void
written_at(pt_t *pt, const pt_loc_t *loc, uintptr_t key, pt_set_t *into)
{
    pt_depend(pt, key);
    const pt_external_writes_t *writes = external_writes_of(pt, key, false);
    const ir_spath_t           *read   = &loc->path;
    bool                        any    = read->root == IR_SPLACE_UNKNOWN;
    for (size_t w = 0; writes && w < writes->len; w++) {
        const pt_external_write_t *write = &writes->data[w];
        const ir_spath_t          *at    = &write->at.path;
        if (!any && at->root != IR_SPLACE_UNKNOWN
            && !same_path_root(at, read)) continue;
        bool    unknown = any || at->root == IR_SPLACE_UNKNOWN;
        int32_t under   = path_within(read, at);
        bool    over    = path_within(at, read) >= 0;
        if (under < 0 && !over && !unknown) continue;
        if (under >= 0 && !over && !unknown && !(read->flags & IR_SPATH_DEEP)
            && crosses(pt, &write->at, (uint32_t)under, at->depth)) continue;
        pt_item_t node = contents_item(site_loc(write->site));
        node.flow = PT_DERIVED;
        set_add(pt, into, under >= 0 && !unknown
            ? item_under(at->steps + under, at->depth - (uint32_t)under,
                         at->flags & IR_SPATH_DEEP, node)
            : item_under(nullptr, 0, true, node));
    }
}

static void
external_written(pt_t *pt, const pt_loc_t *loc, pt_set_t *into)
{
    if (!pt->value_flow) return;
    const ir_spath_t unknown = {.root = IR_SPLACE_UNKNOWN};
    uintptr_t        anywhere = external_key(&unknown);
    if (loc->path.root == IR_SPLACE_UNKNOWN) {
        pt_depend(pt, PT_ALL_EXTERNAL_KEY);
        for (size_t r = 0; r < pt->external_writes.len; r++) {
            const pt_external_writes_t *writes = &pt->external_writes.data[r];
            if (writes->len)
                written_at(pt, loc, external_key(&writes->data[0].at.path),
                           into);
        }
        return;
    }
    written_at(pt, loc, external_key(&loc->path), into);
    written_at(pt, loc, anywhere, into);
}

static void
note_external_write(pt_t *pt, const pt_loc_t *at, const pt_set_t *values)
{
    if (!pt->value_flow) return;
    uintptr_t key  = external_key(&at->path);
    uint32_t  site = UINT32_MAX;
    for (size_t v = 0; v < values->len; v++) {
        if (values->data[v].flow == PT_SAME) continue;
        if (site == UINT32_MAX) {
            pt_external_writes_t *writes = external_writes_of(pt, key, true);
            for (size_t w = 0; site == UINT32_MAX && w < writes->len; w++)
                if (same_object(&writes->data[w].at, at)
                    && !compare_paths(&writes->data[w].at.path, &at->path))
                    site = writes->data[w].site;
            if (site == UINT32_MAX) {
                pt->nodes++;
                site = site_of(pt, (const void *)(uintptr_t)(2 * pt->nodes + 1),
                               nullptr, false, 0);
                pt->sites.data[site].node = true;
                core_list_push(*writes,
                               ((pt_external_write_t){.at = *at, .site = site}));
                pt_notify(pt, key);
                pt_notify(pt, PT_ALL_EXTERNAL_KEY);
            }
        }
        pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
               values->data[v]);
    }
}

static void
aliased_at(pt_t *pt, const uint32_t *sites, size_t count,
           const pt_loc_t *loc, pt_set_t *into)
{
    const ir_spath_t *read = &loc->path;
    for (size_t e = 0; e < count; e++) {
        uint32_t site = sites[e];
        for (size_t a = 0; a < pt->sites.data[site].aliases.len; a++) {
            const pt_alias_t *entry = &pt->sites.data[site].aliases.data[a];
            const pt_item_t   alias = entry->item;
            const ir_spath_t *at    = &alias.loc.path;
            if (!same_path_root(at, read) || !alias_visible(pt, entry))
                continue;
            int32_t under = path_within(read, at);
            int32_t over  = path_within(at, read);
            if (under < 0 && over < 0) continue;
            pt_item_t target = item_at(site_loc(site));
            loc_extend(pt, &target.loc, alias.steps, alias.depth, alias.deep);
            bool loose = loc_deep(&alias.loc);
            if (under >= 0) {
                if (!loc_deep(loc)
                    && crosses(pt, &alias.loc, (uint32_t)under, at->depth))
                    continue;
                set_add(pt, into, item_under(at->steps + under,
                                         at->depth - (uint32_t)under, loose,
                                         target));
                if (loc_deep(loc) && !(read->flags & SPATH_INLINE)) {
                    pt_set_t reached = (pt_set_t){0};
                    target.loc.path.flags |= IR_SPATH_DEEP;
                    content_at(pt, &target.loc, &reached);
                    merge_widened(pt, into, &reached, true);
                }
                continue;
            }
            const core_string_t *rest  = read->steps + over;
            uint32_t             remaining = read->depth - (uint32_t)over;
            if (!loose && remaining && !step_eq(rest[0], value_step)) {
                set_add(pt, into, item_under(nullptr, 0, true, target));
                continue;
            }
            if (!loose) extend_through(pt, &target.loc, rest, remaining, false);
            else {
                set_add(pt, into, item_under(nullptr, 0, true, target));
                target.loc.path.flags |= IR_SPATH_DEEP;
            }
            content_at(pt, &target.loc, into);
        }
    }
}

static void
external_aliased(pt_t *pt, const pt_loc_t *loc, pt_set_t *into)
{
    if (loc->path.root == IR_SPLACE_UNKNOWN) return;
    uintptr_t root = external_key(&loc->path);
    pt_depend(pt, alias_root_key(root));
    bool                   found = false;
    core_list_t(uint32_t) *sites = core_dict_get(&pt->alias_roots,
                                                 (void *)root, &found);
    if (sites) aliased_at(pt, sites->data, sites->len, loc, into);
}

static void
pt_load(pt_t *pt, const pt_set_t *addresses, pt_set_t *into)
{
    for (size_t i = 0; i < addresses->len; i++) {
        const pt_item_t *address = &addresses->data[i];
        if (!is_address(address)) continue;
        if (address->loc.site != PT_EXTERNAL) {
            content_at(pt, &address->loc, into);
            continue;
        }
        pt_loc_t at = location_of(pt, address);
        set_add(pt, into, contents_item(at));
        external_written(pt, &at, into);
        external_aliased(pt, &at, into);
    }
}

static bool
pt_reaches(pt_t *pt, const ir_instr_t *from, const ir_instr_t *to)
{
    return ir_instr_reaches(pt->reach, from, to);
}

static bool
alias_covers(const pt_item_t *a, const pt_item_t *b)
{
    return !object_order(&a->loc, &b->loc)
        && steps_cover(a->depth, a->deep, a->steps, b->depth, b->deep,
                       b->steps)
        && place_covers(&a->loc.path, &b->loc.path);
}

static bool
add_origin(core_list_t(summary_instr_ptr_t) *origins,
           const ir_instr_t *origin)
{
    for (size_t o = 0; o < origins->len; o++)
        if (origins->data[o] == origin) return false;
    core_list_push(*origins, origin);
    return true;
}

static void
pt_alias(pt_t *pt, const pt_item_t *target, pt_loc_t alias,
         const ir_instr_t *origin)
{
    uint32_t  site = target->loc.site;
    pt_item_t item = item_at(alias);
    if (target->kind == PT_CONTENTS) {
        item.loc.path.flags |= IR_SPATH_DEEP;
        item.deep            = true;
    }
    else {
        item.deep  = loc_deep(&target->loc);
        item.steps = steps_join(item.steps, item.depth,
                                target->loc.path.steps,
                                target->loc.path.depth);
        item.depth += target->loc.path.depth;
    }
    alias_list_t *aliases = &pt->sites.data[site].aliases;
    uintptr_t     root    = external_key(&alias.path);
    bool          known   = false;
    for (size_t a = 0; a < aliases->len; a++) {
        pt_alias_t *entry = &aliases->data[a];
        known = known || external_key(&entry->item.loc.path) == root;
        if (!alias_covers(&entry->item, &item)) continue;
        if (!add_origin(&entry->origins, origin)) return;
        pt_notify(pt, aliases_key(site));
        pt_notify(pt, alias_root_key(root));
        return;
    }
    if (!aliases->len) core_list_push(pt->aliased, site);
    pt_alias_t entry = {.item    = item,
                        .origins = core_list_new(summary_instr_ptr_t)};
    core_list_push(entry.origins, origin);
    size_t kept = 0;
    for (size_t a = 0; a < aliases->len; a++) {
        pt_alias_t *old = &aliases->data[a];
        if (!alias_covers(&item, &old->item)) {
            aliases->data[kept++] = *old;
            continue;
        }
        for (size_t o = 0; o < old->origins.len; o++)
            add_origin(&entry.origins, old->origins.data[o]);
        core_list_free(old->origins);
    }
    aliases->len = kept;
    core_list_push(*aliases, entry);
    if (!known) {
        bool                   found = false;
        core_list_t(uint32_t) *sites = core_dict_get(&pt->alias_roots,
                                                     (void *)root, &found);
        if (!sites) {
            sites  = core_alloc(core_list_t(uint32_t));
            *sites = core_list_new(uint32_t);
            core_dict_put(&pt->alias_roots, (void *)root, sites);
        }
        core_list_push(*sites, site);
    }
    pt_notify(pt, aliases_key(site));
    pt_notify(pt, alias_root_key(root));
}

static bool
alias_through(const pt_t *pt, const pt_item_t *alias,
              const core_string_t *steps, uint32_t depth, bool deep,
              pt_loc_t *out)
{
    *out = alias->loc;
    if (steps_prefix(alias->depth, alias->steps, depth, steps)) {
        if (alias->deep) {
            out->path.flags |= IR_SPATH_DEEP;
            return true;
        }
        loc_push(pt, out, value_step);
        loc_extend(pt, out, steps + alias->depth, depth - alias->depth, deep);
        return true;
    }
    if (!steps_prefix(depth, steps, alias->depth, alias->steps)) return false;
    out->path.flags |= IR_SPATH_DEEP;
    return true;
}

static void
pt_publish(pt_t *pt, const pt_set_t *values, const pt_loc_t *at)
{
    for (size_t i = 0; i < values->len; i++) {
        const pt_item_t *value = &values->data[i];
        if (value->flow != PT_SAME) continue;
        set_add(pt, &pt->published, *value);
        if (value->loc.site == PT_EXTERNAL) continue;
        pt_loc_t alias = *at;
        loc_extend(pt, &alias, value->steps, value->depth, value->deep);
        pt_alias(pt, value, alias, pt->at);
    }
}

static void
pt_store(pt_t *pt, const pt_set_t *addresses, const pt_set_t *values,
         bool must)
{
    bool weak = !must || addresses->len != 1;
    for (size_t i = 0; i < addresses->len; i++) {
        pt_loc_t at = location_of(pt, &addresses->data[i]);
        if (!is_address(&addresses->data[i])) continue;
        if (at.site == PT_EXTERNAL) {
            pt_publish(pt, values, &at);
            note_external_write(pt, &at, values);
            continue;
        }
        note_target(pt, pt->at, at.site, &at, weak);
        for (size_t v = 0; v < values->len; v++) {
            pt_item_t item = item_under(at.path.steps, at.path.depth,
                                        loc_deep(&at), values->data[v]);
            pt_add(pt, &pt->sites.data[at.site].contents,
                   contents_key(at.site), item);
            note_write(pt, at.site, &at, weak, item);
        }
    }
}

static bool
pt_elements(pt_t *pt, const ir_value_t *base, pt_set_t *into)
{
    pt_set_t items = (pt_set_t){0};
    pt_address(pt, base, &items);
    th_type *type   = base && base->type ? th_resolve(base->type) : nullptr;
    bool     memory = true;
    if (type && th_kind_of(type) == TH_INDIRECTION) {
        th_type *referent = defined_type(pt->f, th_ind_referent(type));
        uint64_t extent   = 0;
        if (referent && th_kind_of(referent) == TH_SEQUENCE
            && !th_seq_extent(referent, &extent)
            && !inline_field(pt->f, base, false)) {
            pt_set_t loaded = (pt_set_t){0};
            pt_load(pt, &items, &loaded);
            core_list_free(items);
            items = loaded;
        }
    }
    else
        memory = !inline_sequence(pt->f, type);
    if (memory) address_step(pt, &items, element_step, into);
    else project(pt, &items, element_step, into);
    core_list_free(items);
    return memory;
}
