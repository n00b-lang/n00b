#include "internal.h"

struct ir_points_to_t {
    pt_t pt;
};

bool
ir_pto_eq(const ir_pto_t *a, const ir_pto_t *b)
{
    return a->kind == b->kind && a->param == b->param && a->instr == b->instr
        && a->value == b->value && a->local == b->local
        && a->global == b->global && a->origin == b->origin;
}

bool
ir_pto_has(const ir_pto_list_t *objects, const ir_pto_t *object)
{
    for (size_t i = 0; i < objects->len; i++)
        if (ir_pto_eq(&objects->data[i], object)) return true;
    return false;
}

bool
ir_pto_add(ir_pto_list_t *objects, ir_pto_t object)
{
    if (ir_pto_has(objects, &object)) return false;
    core_list_push(*objects, object);
    return true;
}

static ir_pto_t
site_object(const pt_t *pt, uint32_t site)
{
    const pt_site_t *entry = &pt->sites.data[site];
    return (ir_pto_t){
        .kind  = IR_PTO_STORAGE,
        .local = entry->local,
        .value = entry->local ? nullptr : entry->value,
        .instr = entry->local || entry->value || entry->node ? nullptr
               : entry->call ? entry->call : entry->key,
        .origin = entry->call ? entry->origin : 0,
    };
}

static ir_pto_t
external_object(const ir_spath_t *path, bool contents)
{
    switch (path->root) {
    case IR_SPLACE_PARAM:
        return (ir_pto_t){.kind = contents ? IR_PTO_CONTENTS : IR_PTO_PARAM,
                          .param = path->param};
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return (ir_pto_t){.kind = IR_PTO_GLOBAL, .global = path->global};
    default:
        return (ir_pto_t){.kind = IR_PTO_UNKNOWN};
    }
}

static void
item_objects(const pt_t *pt, const pt_item_t *item, ir_pto_list_t *into)
{
    const pt_loc_t *loc = &item->loc;
    if (item->flow != PT_SAME) return;
    if (loc->site != PT_EXTERNAL) {
        ir_pto_add(into, site_object(pt, loc->site));
        return;
    }
    bool beyond = loc->path.depth || item->kind == PT_CONTENTS;
    bool self   = !loc->path.depth && item->kind != PT_CONTENTS;
    if (self) ir_pto_add(into, external_object(&loc->path, false));
    if (beyond || (self && loc_deep(loc)))
        ir_pto_add(into, external_object(&loc->path, true));
}

static void
items_objects(const pt_t *pt, const pt_set_t *items, ir_pto_list_t *into)
{
    for (size_t i = 0; i < items->len; i++)
        item_objects(pt, &items->data[i], into);
}

static bool
object_site(const pt_t *pt, const ir_pto_t *object, uint32_t *site)
{
    const void *key = object->local ? (const void *)object->local
        : object->value ? (const void *)object->value
                        : (const void *)object->instr;
    bool  found = false;
    void *index = nullptr;
    if (object->kind == IR_PTO_STORAGE && object->origin) {
        heap_site_key_t probe = {object->instr, object->origin};
        index = core_dict_get((core_dict_t *)&pt->heap_sites, &probe, &found);
    }
    else if (object->kind == IR_PTO_STORAGE)
        index = core_dict_get((core_dict_t *)&pt->site_index, key, &found);
    *site = (uint32_t)(uintptr_t)index;
    return found;
}

ir_points_to_t *
ir_points_to_new(ir_summaries_t *summaries, const ir_func_t *function,
                 const ir_points_to_hooks_t *hooks)
{
    ir_points_to_t *points_to = core_alloc(ir_points_to_t);
    pt_init(&points_to->pt, summaries, function, false);
    points_to->pt.hooks = hooks;
    pt_solve(&points_to->pt);
    return points_to;
}

void
ir_points_to_free(ir_points_to_t *points_to)
{
    if (!points_to) return;
    pt_free(&points_to->pt);
    core_free(points_to);
}

void
ir_points_to_value(ir_points_to_t *points_to, const ir_instr_t *at,
                   const ir_value_t *value, ir_pto_list_t *into)
{
    pt_t    *pt    = &points_to->pt;
    pt_set_t items = (pt_set_t){0};
    pt->at = at;
    pt_read(pt, value, &items);
    pt->at = nullptr;
    items_objects(pt, &items, into);
    core_list_free(items);
}

void
ir_points_to_loaded(ir_points_to_t *points_to, const ir_instr_t *at,
                    const ir_value_t *value, ir_pto_list_t *into)
{
    pt_t    *pt        = &points_to->pt;
    pt_set_t addresses = (pt_set_t){0};
    pt_set_t loaded    = (pt_set_t){0};
    pt->at = at;
    pt_read(pt, value, &addresses);
    pt_load(pt, &addresses, &loaded);
    pt->at = nullptr;
    items_objects(pt, &loaded, into);
    core_list_free(addresses);
    core_list_free(loaded);
}

static bool
alias_under(const pt_item_t *alias, const ir_pto_t *object)
{
    const ir_spath_t *path = &alias->loc.path;
    switch (object->kind) {
    case IR_PTO_PARAM:
    case IR_PTO_CONTENTS:
        return path->root == IR_SPLACE_PARAM && path->param == object->param;
    case IR_PTO_GLOBAL:
        return (path->root == IR_SPLACE_GLOBAL
                || path->root == IR_SPLACE_THREAD)
            && path->global == object->global;
    case IR_PTO_UNKNOWN:
        return path->root == IR_SPLACE_UNKNOWN;
    default:
        return false;
    }
}

void
ir_points_to_reach(ir_points_to_t *points_to, ir_pto_list_t *objects,
                   bool contents)
{
    const pt_t *pt = &points_to->pt;
    for (size_t i = 0; i < objects->len; i++) {
        ir_pto_t object = objects->data[i];
        uint32_t site   = 0;
        if (object_site(pt, &object, &site)) {
            items_objects(pt, &pt->sites.data[site].contents, objects);
            continue;
        }
        if (contents && object.kind == IR_PTO_PARAM)
            ir_pto_add(objects, (ir_pto_t){.kind  = IR_PTO_CONTENTS,
                                           .param = object.param});
        for (size_t a = 0; a < pt->aliased.len; a++) {
            const pt_site_t *entry = &pt->sites.data[pt->aliased.data[a]];
            for (size_t k = 0; k < entry->aliases.len; k++)
                if (alias_under(&entry->aliases.data[k].item, &object)) {
                    ir_pto_add(objects,
                               site_object(pt, pt->aliased.data[a]));
                    break;
                }
        }
    }
}

static void
argument_reach(ir_points_to_t *points_to, const ir_instr_t *call,
               uint64_t trusted, ir_pto_list_t *into)
{
    pt_t    *pt    = &points_to->pt;
    pt_set_t items = (pt_set_t){0};
    for (uint32_t p = 0; p < ir_call_argument_count(call); p++)
        if (p >= 64 || !(trusted >> p & 1)) argument_items(pt, call, p,
                                                           &items);
    ir_pto_list_t reached = core_list_new(ir_pto_t);
    items_objects(pt, &items, &reached);
    ir_points_to_reach(points_to, &reached, true);
    for (size_t i = 0; i < reached.len; i++)
        ir_pto_add(into, reached.data[i]);
    core_list_free(reached);
    core_list_free(items);
}

bool
ir_points_to_call_writes(ir_points_to_t *points_to, const ir_instr_t *call,
                         uint64_t trusted, ir_pto_list_t *into)
{
    pt_t               *pt      = &points_to->pt;
    const pt_callees_t *callees = callees_of(pt, call);
    if (!callees || callees->targets.unknown) return false;
    pt->at = call;
    pt_set_t written = (pt_set_t){0};
    for (size_t c = 0; c < callees->callees.len; c++) {
        const ir_summary_t *callee = callees->callees.data[c];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & IR_SPATH_WRITE)) continue;
            if (path->root == IR_SPLACE_PARAM) {
                if (path->param >= 64 || !(trusted >> path->param & 1))
                    map_param_path(pt, call, path, &written);
            }
            else if (path->root == IR_SPLACE_UNKNOWN)
                argument_reach(points_to, call, trusted, into);
            else set_add(pt, &written, item_at(path_loc(path)));
        }
    }
    pt->at = nullptr;
    items_objects(pt, &written, into);
    core_list_free(written);
    return true;
}

static uint64_t
instr_position(const pt_t *pt, const ir_instr_t *in)
{
    bool      found = false;
    uintptr_t at    = (uintptr_t)core_dict_get(
        (core_dict_t *)&pt->flow.positions, in, &found);
    return found ? at : UINT64_MAX;
}

bool
ir_points_to_escapes(ir_points_to_t *points_to, const ir_pto_t *object,
                     const ir_instr_t **where)
{
    const pt_t *pt   = &points_to->pt;
    uint32_t    site = 0;
    *where = nullptr;
    if (!object_site(pt, object, &site)
        || !pt->sites.data[site].aliases.len)
        return false;
    uint64_t first = UINT64_MAX;
    const alias_list_t *aliases = &pt->sites.data[site].aliases;
    for (size_t a = 0; a < aliases->len; a++)
        for (size_t o = 0; o < aliases->data[a].origins.len; o++) {
            const ir_instr_t *origin = aliases->data[a].origins.data[o];
            uint64_t at = origin ? instr_position(pt, origin) : UINT64_MAX;
            if (origin && (!*where || at < first)) {
                *where = origin;
                first  = at;
            }
        }
    return true;
}

static bool
holds_object(const pt_t *pt, const pt_set_t *items, const ir_pto_t *object,
             ir_pto_list_t *scratch, ir_points_to_t *points_to)
{
    scratch->len = 0;
    items_objects(pt, items, scratch);
    ir_points_to_reach(points_to, scratch, false);
    return ir_pto_has(scratch, object);
}

void
ir_points_to_holders(ir_points_to_t *points_to, const ir_pto_t *object,
                     ir_points_to_holder_fn holder, void *context)
{
    const pt_t   *pt      = &points_to->pt;
    ir_pto_list_t scratch = core_list_new(ir_pto_t);
    core_dict_foreach((core_dict_t *)&pt->value_index, entry) {
        const pt_set_t *items =
            &pt->values.data[(uintptr_t)entry->value];
        if (holds_object(pt, items, object, &scratch, points_to))
            holder(context, entry->key, nullptr);
    }
    for (size_t s = 0; s < pt->sites.len; s++) {
        const pt_site_t *entry = &pt->sites.data[s];
        if (entry->local
            && holds_object(pt, &entry->contents, object, &scratch,
                            points_to))
            holder(context, nullptr, entry->local);
    }
    core_list_free(scratch);
}
