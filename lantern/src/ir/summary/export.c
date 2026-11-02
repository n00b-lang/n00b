#include "internal.h"

typedef core_list_t(uint32_t) site_list_t;

typedef struct {
    site_sources_t *entry;
    pt_set_t        next;
    size_t          at;
} source_frame_t;
core_list_decl(source_frame_t);

static void name_loc(pt_t *pt, const pt_loc_t *loc, path_list_t *into,
                     core_list_t(uint32_t) *pending);

static bool
constant_read(const ir_spath_t *path)
{
    const ir_global_t *global = path->global
        ? ir_global_identity((ir_global_t *)path->global) : nullptr;
    return global && named_root(path->root) && !path->depth
        && !(path->flags & (IR_SPATH_WRITE | IR_SPATH_DEEP))
        && global->mutability != IR_MUT_MUTABLE
        && global->has_static_initializer;
}

static void
note_context(pt_t *pt, const ir_instr_t *in)
{
    pt->context.len = 0;
    uint32_t first = 0;
    uint32_t count = pt->places
        ? ir_places_accesses_of(pt->places, in, &first) : 0;
    for (uint32_t a = first; a < first + count; a++) {
        const ir_access_t *access = ir_places_access_at(pt->places, a);
        const uint32_t    *ids    = ir_places_access_places(pt->places,
                                                            access);
        bool load = access->kind == IR_ACCESS_LOAD;
        for (uint32_t p = 0; p < access->count; p++) {
            ir_splace_t mapped;
            if (portable_place(pt->f, ir_place_at(pt->places, ids[p]),
                               &mapped))
                core_list_push(pt->context, ((pt_access_t){mapped, load}));
        }
    }
}

static bool
context_matches(const ir_splace_t *have, const ir_splace_t *want)
{
    return named_place(want) ? ir_splace_eq(have, want) : !named_place(have);
}

static void
apply_context(pt_t *pt, ir_spath_t *path)
{
    ir_splace_t places[2];
    ir_spath_t  probe = *path;
    probe.type   = nullptr;
    probe.flags |= IR_SPATH_ROOT;
    uint32_t count   = ir_summary_path_places(pt->f, &probe, places);
    bool     read    = path->flags & IR_SPATH_READ;
    bool     write   = path->flags & IR_SPATH_WRITE;
    bool     heap    = false;
    bool     root    = !pt->context.len;
    bool     unknown = !pt->context.len;
    th_type *type    = nullptr;
    for (size_t c = 0; c < pt->context.len; c++) {
        const pt_access_t *access = &pt->context.data[c];
        if (!(access->load ? read : write)) continue;
        for (uint32_t p = 0; p < count; p++) {
            if (!context_matches(&access->place, &places[p])) continue;
            if (named_place(&places[p])) {
                root = true;
                continue;
            }
            if (access->place.kind != IR_SPLACE_HEAP || !access->place.type)
                root = unknown = true;
            else if (!heap) type = access->place.type;
            else if (!same_type(type, access->place.type)) unknown = true;
            heap = true;
        }
    }
    if (root || !heap) path->flags |= IR_SPATH_ROOT;
    if (unknown) return;
    path->type   = heap ? type : nullptr;
    path->flags |= IR_SPATH_TYPED;
}

static void
record_loc(pt_t *pt, ir_summary_t *summary, const pt_loc_t *loc,
           uint8_t flags, th_type *carried)
{
    if (pt->summaries->escapes_only && !(flags & IR_SPATH_WRITE)) return;
    path_list_t names = core_list_new(ir_spath_t);
    pt_loc_t    at    = {.site = PT_EXTERNAL};
    bool        own   = copy_in_param(pt, loc->site, &at.path);
    if (own) {
        loc_extend(pt, &at, loc->path.steps, loc->path.depth, loc_deep(loc));
        add_path_to(&names, &at.path);
    }
    else name_loc(pt, loc, &names, nullptr);
    for (size_t n = 0; n < names.len; n++) {
        ir_spath_t path = names.data[n];
        path.flags &= (uint8_t)~SPATH_INLINE;
        path.flags |= flags;
        path.type   = carried;
        if (pt->contextual) apply_context(pt, &path);
        if (path.flags & IR_SPATH_READ) {
            bool entry = (pt->contextual
                          || (flags & IR_SPATH_READS_ENTRY))
                && !written_before(pt, pt->at, &path);
            path.flags = (uint8_t)((path.flags & ~IR_SPATH_READS_ENTRY)
                                   | (entry ? IR_SPATH_READS_ENTRY : 0));
        }
        if (own) path.flags |= IR_SPATH_ROOT;
        else if (loc->site != PT_EXTERNAL)
            path.flags &= (uint8_t)~IR_SPATH_ROOT;
        if ((path.flags & IR_SPATH_WRITE) && string_bytes_path(pt, &path)) {
            path.flags &= (uint8_t)~IR_SPATH_WRITE;
            if (!(path.flags & IR_SPATH_READ)) continue;
        }
        if (!constant_read(&path)) add_function_path(summary, pt->f, &path);
    }
    core_list_free(names);
}

static void
record_set(pt_t *pt, ir_summary_t *summary, const pt_set_t *addresses,
           uint8_t flags, th_type *carried)
{
    for (size_t i = 0; i < addresses->len; i++) {
        const pt_item_t *address = &addresses->data[i];
        if (!is_address(address)) continue;
        pt_loc_t at = location_of(pt, address);
        record_loc(pt, summary, &at, flags, carried);
    }
}

static void
record_global_values(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in)
{
    if (in->op == IR_OP_ADDRESS_OF) return;
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = referenced(in, o);
        if (!value || value->kind != IR_VAL_GLOBAL || !value->global
            || ir_op_is_address_operand(in->op, o)) continue;
        pt_loc_t at = global_loc(value->global);
        record_loc(pt, summary, &at, IR_SPATH_READ, nullptr);
    }
}

static void
record_operand(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in,
               uint32_t operand, uint8_t flags)
{
    if (operand >= in->n_operands) return;
    pt_set_t addresses = (pt_set_t){0};
    pt_address(pt, referenced(in, operand), &addresses);
    record_set(pt, summary, &addresses, flags, nullptr);
    core_list_free(addresses);
}

static void
record_callee_stores(pt_t *pt, ir_summary_t *summary, const ir_instr_t *call,
                     const ir_summary_t *callee)
{
    pt_set_t to   = (pt_set_t){0};
    pt_set_t from = (pt_set_t){0};
    if (callee->results_known) apply_stores(pt, call, callee, summary);
    else {
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & IR_SPATH_WRITE)) continue;
            if (path->root == IR_SPLACE_PARAM)
                map_param_path(pt, call, path, &to);
            else
                set_add(pt, &to, item_at(path_loc(path)));
        }
        for (uint32_t param = 0; param < ir_call_argument_count(call);
             param++)
            if (ir_summary_param_escape(callee, param)) {
                pt_set_t items = (pt_set_t){0};
                argument_items(pt, call, param, &items);
                for (size_t i = 0; i < items.len; i++)
                    if (items.data[i].flow == PT_SAME)
                        set_add(pt, &from, deepened(&items.data[i]));
                core_list_free(items);
            }
        for (size_t i = 0; i < to.len; i++)
            to.data[i].loc.path.flags |= IR_SPATH_DEEP;
        record_stores(pt, summary, &to, &from);
    }
    core_list_free(to);
    core_list_free(from);
}

static void
rebase_access(pt_t *pt, const ir_instr_t *call, uint32_t param,
              ir_spath_t *carried)
{
    ir_call_arg_t   argument = ir_call_argument(call, param);
    const uint32_t *ids      = nullptr;
    uint32_t        count    = 0;
    bool known = argument.value && pt->places
        && ir_places_of(pt->places, argument.value, &ids, &count);
    bool typed = known && (carried->flags & IR_SPATH_TYPED);
    bool owned = !known;
    for (uint32_t i = 0; i < count; i++) {
        ir_splace_t mapped;
        if (!portable_place(pt->f, ir_place_at(pt->places, ids[i]), &mapped))
            continue;
        if (named_place(&mapped)) {
            owned = true;
            continue;
        }
        if (mapped.kind != IR_SPLACE_HEAP) owned = true;
        ir_spath_t part = {.flags = IR_SPATH_TYPED, .type = mapped.type};
        typed = typed && mapped.kind == IR_SPLACE_HEAP && mapped.type
            && merge_type(carried, &part);
    }
    if (!owned) carried->flags &= (uint8_t)~IR_SPATH_ROOT;
    if (!typed) {
        carried->flags &= (uint8_t)~IR_SPATH_TYPED;
        carried->type   = nullptr;
    }
}

static void
record_call(pt_t *pt, ir_summary_t *summary, const ir_instr_t *call)
{
    const pt_callees_t *callees = callees_of(pt, call);
    for (size_t t = 0; t < callees->callees.len; t++)
        if (ir_summary_witness_for(callees->callees.data[t],
                                   IR_SWITNESS_UNKNOWN))
            note_unknown(pt, (core_string_t){0},
                         (ir_func_t *)callees->callees.data[t]->function);
    if (callees->targets.unknown) {
        note_unknown(pt, CORE_STRING_STATIC("a call whose target is unknown"),
                     nullptr);
        add_unknown_path(summary, IR_SPATH_ACCESS);
        pt_set_t to   = (pt_set_t){0};
        pt_set_t from = (pt_set_t){0};
        set_add(pt, &to, item_at(unknown_loc()));
        call_reach(pt, call, callees, &from);
        record_stores(pt, summary, &to, &from);
        core_list_free(to);
        core_list_free(from);
    }
    pt->contextual = false;
    for (size_t t = 0; t < callees->callees.len; t++) {
        const ir_summary_t *callee = callees->callees.data[t];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & (IR_SPATH_READ | IR_SPATH_WRITE))) continue;
            ir_spath_t carried = {
                .flags = path->flags
                    & (IR_SPATH_ACCESS | IR_SPATH_TYPED | IR_SPATH_ROOT),
                .type  = path->type};
            if (path->root != IR_SPLACE_PARAM) {
                pt_loc_t loc = path_loc(path);
                record_loc(pt, summary, &loc, carried.flags, carried.type);
                continue;
            }
            rebase_access(pt, call, path->param, &carried);
            pt_set_t locs = (pt_set_t){0};
            map_param_path(pt, call, path, &locs);
            record_set(pt, summary, &locs, carried.flags, carried.type);
            core_list_free(locs);
        }
        record_callee_stores(pt, summary, call, callee);
    }
    pt->contextual = true;
    written_outputs(pt, call, callees, nullptr, summary);
}

static void
record_instr(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in)
{
    if (pt->summaries->escapes_only && !is_call(in)
        && !(ir_op_memory_access(in->op) & IR_MEM_WRITE) && !launders(pt, in))
        return;
    pt_set_t first  = (pt_set_t){0};
    pt_set_t second = (pt_set_t){0};
    const ir_value_t *base = referenced(in, 0);
    pt->contextual = true;
    note_context(pt, in);
    record_global_values(pt, summary, in);
    switch (in->op) {
    case IR_OP_LOAD:
        if (whole_load(pt, in))
            record_operand(pt, summary, in, 0, IR_SPATH_READ);
        break;
    case IR_OP_EXTRACT:
        if (base && base->kind == IR_VAL_INSTR && base->def
            && base->def->op == IR_OP_LOAD && base->def->n_operands
            && !whole_load(pt, base->def) && in->n_operands > 1
            && in->operands[1].kind == IR_OPD_NAME) {
            note_context(pt, base->def);
            pt_address(pt, referenced(base->def, 0), &first);
            address_step(pt, &first, in->operands[1].name, &second);
            record_set(pt, summary, &second, IR_SPATH_READ, nullptr);
        }
        break;
    case IR_OP_SEQUENCE_GET:
        if (pt_elements(pt, base, &first))
            record_set(pt, summary, &first, IR_SPATH_READ, nullptr);
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        record_call(pt, summary, in);
        break;
    case IR_OP_CAST:
    case IR_OP_BITCAST:
        if (launders(pt, in)) {
            set_add(pt, &first, item_at(unknown_loc()));
            pt_read(pt, base, &second);
            record_stores(pt, summary, &first, &second);
        }
        break;
    default: {
        uint8_t access = ir_op_memory_access(in->op);
        uint8_t flags  = (access & IR_MEM_READ ? IR_SPATH_READ : 0)
                      | (access & IR_MEM_WRITE ? IR_SPATH_WRITE : 0);
        bool transfers = ir_op_transfers(in->op);
        if (!access || (transfers && !stored_sets(pt, in, &first, &second)))
            break;
        if (transfers) record_set(pt, summary, &first, flags, nullptr);
        else record_operand(pt, summary, in,
                            (uint32_t)ir_op_address_operand(in->op), flags);
        int32_t source = ir_op_source_operand(in->op);
        if (source >= 0)
            record_operand(pt, summary, in, (uint32_t)source, IR_SPATH_READ);
        if (transfers) record_stores(pt, summary, &first, &second);
        break;
    }
    }
    core_list_free(first);
    core_list_free(second);
}

static void
name_loc(pt_t *pt, const pt_loc_t *loc, path_list_t *into,
         site_list_t *pending)
{
    if (loc->site == PT_EXTERNAL) {
        ir_spath_t path = loc->path;
        path.flags &= IR_SPATH_DEEP | SPATH_INLINE;
        add_path_to(into, &path);
        return;
    }
    if (pending) {
        ir_spath_t heap = {.root  = IR_SPLACE_HEAP,
                           .param = pt->sites.data[loc->site].origin,
                           .flags = loc->path.flags
                               & (IR_SPATH_DEEP | SPATH_INLINE)};
        for (uint32_t s = 0; s < loc->path.depth; s++)
            path_push(&heap, loc->path.steps[s]);
        add_path_to(into, &heap);
        core_list_push(*pending, loc->site);
    }
    const alias_list_t *aliases = &pt->sites.data[loc->site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        if (!alias_visible(pt, &aliases->data[a])) continue;
        pt_loc_t alias;
        const pt_item_t *through = &aliases->data[a].item;
        if (!alias_through(pt, through, loc->path.steps, loc->path.depth,
                           loc_deep(loc), &alias)) continue;
        if ((loc->path.flags & SPATH_INLINE) && !through->deep
            && !loc_deep(&through->loc) && loc_deep(&alias))
            alias.path.flags |= SPATH_INLINE;
        name_loc(pt, &alias, into, nullptr);
    }
}

static void
summary_sources(pt_t *pt, flow_table_t *flows, uint32_t name,
                ir_sflow_kind_t kind, core_list_t(uint64_t) *parts)
{
    bool          found = false;
    source_map_t *map   = core_dict_get(&pt->source_maps, flows, &found);
    if (!map) {
        map  = core_alloc(source_map_t);
        *map = (source_map_t){.spans = core_list_new(uint64_t),
                              .pool  = core_list_new(uint32_t)};
        core_dict_put(&pt->source_maps, flows, map);
    }
    while (map->spans.len <= name) core_list_push(map->spans, UINT64_MAX);
    if (map->spans.data[name] == UINT64_MAX) {
        size_t first = parts->len;
        canonical_sources(flows, pt->source_names.sources.data[name], kind,
                          parts);
        map->spans.data[name] = (uint64_t)map->pool.len << 32
                              | (parts->len - first);
        for (size_t p = first; p < parts->len; p++)
            core_list_push(map->pool, (uint32_t)(parts->data[p] >> 8));
        return;
    }
    uint64_t span  = map->spans.data[name];
    uint32_t start = (uint32_t)(span >> 32);
    for (uint32_t p = 0; p < (uint32_t)span; p++)
        core_list_push(*parts, (uint64_t)map->pool.data[start + p] << 8 | kind);
}

static void
bits_or(flow_bits_t *into, const flow_bits_t *from)
{
    while (into->len < from->len) core_list_push(*into, UINT64_C(0));
    for (size_t w = 0; w < from->len; w++) into->data[w] |= from->data[w];
}

static void
source_named(pt_t *pt, site_sources_t *entry, const ir_spath_t *from,
             bool control)
{
    ir_spath_t key  = canonical_path(from);
    uint32_t   name = flow_source(&pt->source_names, &key);
    bit_set(control ? &entry->control : &entry->data, name);
}

static bool
aggregate_slot(pt_t *pt, const ir_spath_t *path)
{
    ir_module_t *module = pt->f->owner;
    th_type     *slot   = end_type(module, root_object_type(pt->f, path, pt),
                                   path);
    return slot && by_value(module, slot)
        && ir_machine_of(module, slot).class != IR_CLASS_PTR;
}

static void
address_name(ir_spath_t *name, bool shorthand)
{
    if (shorthand && name->depth && !(name->flags & IR_SPATH_DEEP)
        && step_eq(name->steps[name->depth - 1], value_step))
        name->depth--;
    else name->flags |= IR_SPATH_ROOT;
}

static void
external_name(pt_t *pt, const pt_item_t *item, ir_spath_t *name)
{
    if (item->kind == PT_ADDRESS) {
        address_name(name, item->flow == PT_SAME);
        return;
    }
    if (!item->loc.path.depth && item->flow > PT_COPY
        && aggregate_slot(pt, name))
        name->flags |= IR_SPATH_DEEP;
}

static void
sources_at(pt_t *pt, pt_loc_t at, site_sources_t *entry, pt_set_t *next)
{
    bool            node     = pt->sites.data[at.site].node;
    const pt_set_t *contents = &pt->sites.data[at.site].contents;
    for (size_t c = 0; c < contents->len; c++) {
        const pt_item_t *held  = &contents->data[c];
        bool             under = steps_prefix(at.path.depth, at.path.steps,
                                              held->depth, held->steps);
        bool             over  = steps_prefix(held->depth, held->steps,
                                              at.path.depth, at.path.steps);
        if (!under && !over) continue;
        bool                 narrow = !under && !held->deep
            && held->flow <= PT_COPY && held->kind == PT_CONTENTS
            && !slot_pointer(pt, held);
        const core_string_t *rest   = at.path.steps + held->depth;
        uint32_t             count  = narrow
            ? at.path.depth - held->depth : 0;
        if (held->loc.site != PT_EXTERNAL) {
            pt_loc_t reached = held->loc;
            if (held->flow == PT_SAME && !narrow) {
                reached.path.depth  = 0;
                reached.path.flags |= IR_SPATH_DEEP;
                reached.path.flags &= (uint8_t)~SPATH_INLINE;
            }
            else loc_extend(pt, &reached, rest, count, false);
            core_list_push(*next, item_at(reached));
            continue;
        }
        if (constant_source(held)) continue;
        pt_loc_t extended = held->loc;
        loc_extend(pt, &extended, rest, count, false);
        ir_spath_t from = extended.path;
        from.flags &= IR_SPATH_DEEP;
        if (!node && !loc_deep(&extended) && held->kind == PT_CONTENTS
            && known_pointer(pt, &extended)) {
            ir_spath_t pointer = from;
            external_name(pt, held, &pointer);
            source_named(pt, entry, &pointer, held->flow == PT_CONTROL);
            loc_push(pt, &extended, value_step);
            from = extended.path;
            from.flags &= IR_SPATH_DEEP;
        }
        if (!node) from.flags |= IR_SPATH_DEEP;
        external_name(pt, held, &from);
        source_named(pt, entry, &from, held->flow == PT_CONTROL);
    }
    const alias_list_t *aliases = &pt->sites.data[at.site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        pt_loc_t through;
        if (!alias_through(pt, &aliases->data[a].item, at.path.steps,
                           at.path.depth, loc_deep(&at), &through))
            continue;
        ir_spath_t from = through.path;
        from.flags &= IR_SPATH_DEEP;
        if (!node || !from.depth) from.flags |= IR_SPATH_DEEP;
        source_named(pt, entry, &from, false);
    }
}

static site_sources_t *
sources_entry(pt_t *pt, const pt_loc_t *loc)
{
    bool            found = false;
    site_sources_t *entry = core_dict_get(&pt->site_sources, (void *)loc,
                                          &found);
    if (found) return entry;
    entry      = core_alloc(site_sources_t);
    entry->loc = *loc;
    core_dict_put(&pt->site_sources, &entry->loc, entry);
    return entry;
}

static void
close_sources(pt_t *pt, site_sources_t *root)
{
    size_t first = pt->source_stack.len;
    while ((site_sources_t *)(uintptr_t)pt->source_stack.data[first - 1]
           != root)
        first--;
    first--;
    for (size_t m = first + 1; m < pt->source_stack.len; m++) {
        site_sources_t *member =
            (site_sources_t *)(uintptr_t)pt->source_stack.data[m];
        bits_or(&root->data, &member->data);
        bits_or(&root->control, &member->control);
    }
    for (size_t m = first; m < pt->source_stack.len; m++) {
        site_sources_t *member =
            (site_sources_t *)(uintptr_t)pt->source_stack.data[m];
        if (member != root) {
            member->data.len    = 0;
            member->control.len = 0;
            bits_or(&member->data, &root->data);
            bits_or(&member->control, &root->control);
        }
        member->open = false;
        member->done = true;
    }
    pt->source_stack.len = first;
}

static void
open_sources(pt_t *pt, core_list_t(source_frame_t) *frames,
             site_sources_t *entry)
{
    entry->order = entry->low = ++pt->source_order;
    entry->open  = true;
    core_list_push(pt->source_stack, (uint64_t)(uintptr_t)entry);
    source_frame_t frame = {.entry = entry, .next = (pt_set_t){0}};
    sources_at(pt, entry->loc, entry, &frame.next);
    core_list_push(*frames, frame);
}

static void
reached_sources(site_sources_t *entry, const site_sources_t *child,
                bool returned)
{
    if (returned) {
        if (child->low < entry->low) entry->low = child->low;
    }
    else if (child->open && child->order < entry->low)
        entry->low = child->order;
    if (!child->done) return;
    bits_or(&entry->data, &child->data);
    bits_or(&entry->control, &child->control);
}

static void
compute_sources(pt_t *pt, site_sources_t *root)
{
    core_list_t(source_frame_t) frames = core_list_new(source_frame_t);
    open_sources(pt, &frames, root);
    while (frames.len) {
        source_frame_t *frame = &frames.data[frames.len - 1];
        site_sources_t *entry = frame->entry;
        if (frame->at < frame->next.len) {
            site_sources_t *child =
                sources_entry(pt, &frame->next.data[frame->at].loc);
            if (child != entry && !child->order) {
                open_sources(pt, &frames, child);
                continue;
            }
            if (child != entry) reached_sources(entry, child, false);
            frame->at++;
            continue;
        }
        core_list_free(frame->next);
        frames.len--;
        if (entry->low == entry->order) close_sources(pt, entry);
        if (!frames.len) break;
        source_frame_t *parent = &frames.data[frames.len - 1];
        reached_sources(parent->entry, entry, true);
        parent->at++;
    }
    core_list_free(frames);
}

static const site_sources_t *
site_sources(pt_t *pt, const pt_loc_t *loc)
{
    site_sources_t *entry = sources_entry(pt, loc);
    if (!entry->done) compute_sources(pt, entry);
    return entry;
}

static ir_spath_t
value_dest(const ir_spath_t *base, const pt_item_t *item)
{
    ir_spath_t to = *base;
    if (to.flags & IR_SPATH_DEEP) return to;
    for (uint32_t s = 0; s < item->depth; s++) path_push(&to, item->steps[s]);
    if (item->deep || (to.flags & IR_SPATH_DEEP))
        to.flags |= IR_SPATH_DEEP | SPATH_INLINE;
    return to;
}

static void
describe_names(pt_t *pt, const pt_item_t *item, flow_table_t *flows,
               site_list_t *pending, core_list_t(uint64_t) *parts)
{
    path_list_t from    = core_list_new(ir_spath_t);
    site_list_t ignored = core_list_new(uint32_t);
    name_loc(pt, &item->loc, &from,
             item->flow == PT_SAME || !pending ? pending : &ignored);
    core_list_free(ignored);
    ir_sflow_kind_t kind = item->flow == PT_SAME ? IR_SFLOW_ALIAS
                                                 : flow_kind(item->flow);
    for (size_t f = 0; f < from.len; f++) {
        ir_spath_t *name = &from.data[f];
        name->flags &= (uint8_t)~SPATH_INLINE;
        if (item->loc.site == PT_EXTERNAL) external_name(pt, item, name);
        else if (name->root == IR_SPLACE_HEAP)
            name->flags |= item->kind == PT_ADDRESS ? IR_SPATH_ROOT : 0;
        else if (!(name->flags & IR_SPATH_DEEP)) address_name(name, true);
        canonical_sources(flows, name, kind, parts);
    }
    core_list_free(from);
}

static void
describe_values(pt_t *pt, const pt_set_t *values, const ir_spath_t *bases,
                size_t count, flow_table_t *flows, site_list_t *pending)
{
    if (!count) return;
    core_list_t(uint64_t) parts = core_list_new(uint64_t);
    for (size_t i = 0; i < values->len; i++) {
        const pt_item_t *item = &values->data[i];
        if (constant_item(item)) continue;
        parts.len = 0;
        if (item->flow != PT_SAME && item->loc.site != PT_EXTERNAL) {
            const site_sources_t *sources = site_sources(pt, &item->loc);
            const flow_bits_t    *data    = &sources->data;
            const flow_bits_t    *control = &sources->control;
            size_t words = data->len > control->len ? data->len : control->len;
            for (size_t w = 0; w < words; w++) {
                uint64_t set = (w < data->len ? data->data[w] : 0)
                    | (w < control->len ? control->data[w] : 0);
                for (; set; set &= set - 1) {
                    uint32_t s = (uint32_t)(w * 64 + __builtin_ctzll(set));
                    ir_sflow_kind_t kind = bit_has(data, s)
                            && item->flow != PT_CONTROL
                        ? IR_SFLOW_DATA : IR_SFLOW_CONTROL;
                    summary_sources(pt, flows, s, kind, &parts);
                }
            }
        }
        else describe_names(pt, item, flows, pending, &parts);
        for (size_t b = 0; parts.len && b < count; b++) {
            ir_spath_t to = value_dest(&bases[b], item);
            to = canonical_path(&to);
            flow_dest_t *dest = flow_dest(flows, &to);
            for (size_t p = 0; p < parts.len; p++)
                bit_set(kind_bits(dest, (ir_sflow_kind_t)(parts.data[p]
                                                          & 0xff)),
                        (uint32_t)(parts.data[p] >> 8));
        }
    }
    core_list_free(parts);
}

static void
describe_heap(pt_t *pt, ir_summary_t *summary, site_list_t *pending)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    while (pending->len) {
        uint32_t site = pending->data[--pending->len];
        void    *key  = (void *)(uintptr_t)(site + 1);
        if (core_dict_contains(&seen, key)) continue;
        core_dict_put(&seen, key, key);
        ir_spath_t heap = {.root  = IR_SPLACE_HEAP,
                           .param = pt->sites.data[site].origin};
        describe_values(pt, &pt->sites.data[site].contents, &heap, 1,
                        &summary->flows[IR_SFLOW_STORES], pending);
    }
    core_dict_free(&seen);
}

static void
summarize_output(pt_t *pt, ir_summary_t *summary, const pt_set_t *returned,
                 uint8_t output)
{
    ir_spath_t base = {.output = output};
    describe_values(pt, returned, &base, 1,
                    &summary->flows[IR_SFLOW_RESULTS], &pt->described);
}

static void
record_stores(pt_t *pt, ir_summary_t *summary, const pt_set_t *addresses,
              const pt_set_t *values)
{
    if (!values->len) return;
    path_list_t to = core_list_new(ir_spath_t);
    for (size_t i = 0; i < addresses->len; i++)
        if (is_address(&addresses->data[i])) {
            pt_loc_t at = location_of(pt, &addresses->data[i]);
            name_loc(pt, &at, &to, nullptr);
        }
    describe_values(pt, values, to.data, to.len,
                    &summary->flows[IR_SFLOW_STORES], &pt->described);
    core_list_free(to);
}

static void
summarize_results(pt_t *pt, ir_summary_t *summary)
{
    const ir_func_t *f = pt->f;
    summary->results_known = true;
    size_t   count   = ir_call_output_count(f->type);
    uint32_t outputs = count < IR_SOUTPUT_STATUS ? (uint32_t)count + 1
                                                 : IR_SOUTPUT_STATUS;
    for (uint32_t output = returns_value(f) ? 0 : 1; output < outputs;
         output++) {
        size_t operand = output
            ? ir_return_output_operand(f->owner, f->type, output - 1) : 0;
        pt_set_t returned = (pt_set_t){0};
        for (uint32_t b = 0; b < f->n_blocks; b++)
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = f->blocks[b]->instrs[i];
                if (in->op != IR_OP_RETURN || operand >= in->n_operands)
                    continue;
                pt_read(pt, referenced(in, operand), &returned);
                reach_control(pt, f->blocks[b], &returned);
            }
        summarize_output(pt, summary, &returned, (uint8_t)output);
        core_list_free(returned);
    }
    pt_set_t failed = (pt_set_t){0};
    pt_set_t status = (pt_set_t){0};
    bool     fails  = false;
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        if (!block->n_instrs) continue;
        const ir_instr_t *exit = block->instrs[block->n_instrs - 1];
        if (exit->op != IR_OP_FAIL) continue;
        fails = true;
        pt_read(pt, referenced(exit, 0), &failed);
        reach_control(pt, block, &failed);
    }
    for (uint32_t b = 0; fails && b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        const ir_instr_t *exit = block->n_instrs
            ? block->instrs[block->n_instrs - 1] : nullptr;
        if (exit && (exit->op == IR_OP_FAIL || exit->op == IR_OP_RETURN))
            reach_control(pt, block, &status);
    }
    if (fails) {
        summarize_output(pt, summary, &failed, IR_SOUTPUT_FAILURE);
        summarize_output(pt, summary, &status, IR_SOUTPUT_STATUS);
    }
    core_list_free(failed);
    core_list_free(status);
}

static void
note_copy_ins(pt_t *pt)
{
    const ir_func_t *f       = pt->f;
    size_t           outputs = ir_call_output_count(f->type);
    for (uint32_t b = 0; outputs && b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->op != IR_OP_RETURN) continue;
            for (size_t o = 0; o < outputs; o++) {
                size_t operand = ir_return_output_operand(f->owner, f->type,
                                                          o);
                size_t param   = ir_call_output_param(f->type, o);
                const ir_value_t *value = operand < in->n_operands
                    ? referenced(in, (uint32_t)operand) : nullptr;
                const ir_instr_t *copy  = value
                    && value->kind == IR_VAL_INSTR ? value->def : nullptr;
                if (!copy || copy->op != IR_OP_COPY || !copy->n_operands
                    || param >= f->n_params) continue;
                const ir_local_t *local = ir_operand_local(&copy->operands[0]);
                if (local)
                    core_dict_put(&pt->copy_ins, (void *)local,
                                  (void *)((uintptr_t)(o + 1) << 32
                                           | (uintptr_t)param));
            }
        }
}

static bool
writes_definitely(pt_t *pt, const ir_instr_t *call)
{
    for (uint32_t param = 0; param < ir_call_argument_count(call); param++) {
        ir_call_arg_t argument = ir_call_argument(call, param);
        if (ir_call_argument_writes(&argument)) return true;
    }
    if (!sole_callee(pt, call)) return false;
    const ir_summary_t *callee = callees_of(pt, call)->callees.data[0];
    const flow_table_t *flows  = &callee->flows[IR_SFLOW_STORES];
    for (size_t d = 0; d < flows->dests.len; d++)
        if (must_dest(callee, flows->dests.data[d].to)) return true;
    return false;
}

static void
note_writers(pt_t *pt)
{
    for (size_t o = 0; o < pt->order.len; o++) {
        const ir_instr_t *in = pt->order.data[o];
        if (in->dest_kind == IR_DEST_LOCAL && in->dest_local
            && tracks(pt, in->dest_local->type))
            note_target(pt, in, local_site(pt, in->dest_local), nullptr,
                        false);
        if (must_store(in)
            || ((in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                && writes_definitely(pt, in)))
            core_dict_put(&pt->flow.pending, (void *)in, (void *)1);
    }
}

static bool
copy_in_param(const pt_t *pt, uint32_t site, ir_spath_t *own)
{
    if (site == PT_EXTERNAL || !pt->sites.data[site].local) return false;
    bool      found = false;
    uintptr_t entry = (uintptr_t)core_dict_get(
        (core_dict_t *)&pt->copy_ins, (void *)pt->sites.data[site].local,
        &found);
    if (!found || (entry >> 32) >= UINT8_MAX) return false;
    *own = (ir_spath_t){.root   = IR_SPLACE_PARAM,
                        .param  = (uint32_t)entry,
                        .output = (uint8_t)(entry >> 32)};
    return true;
}
