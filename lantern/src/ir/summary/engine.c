#include "internal.h"

typedef struct {
    const core_string_t *tokens;
    uint32_t             count;
    bool                 deep;
} pt_region_t;
core_list_decl(pt_region_t);
typedef core_list_t(pt_region_t) region_list_t;

typedef struct {
    pt_t     *pt;
    uint64_t  fingerprint;
} engine_t;

static void escape_item(pt_t *pt, const pt_item_t *item,
                        pt_set_t *work);

static void
pt_init(pt_t *pt, ir_summaries_t *summaries, const ir_func_t *f,
        bool value_flow)
{
    *pt = (pt_t){
        .summaries = summaries, .f = f, .value_flow = value_flow,
        .values    = core_list_new(pt_set_t),
        .sites     = core_list_new(pt_site_t),
    };
    core_dict_init(&pt->value_index, nullptr, nullptr);
    core_dict_init(&pt->site_index, nullptr, nullptr);
    pt->types = type_memo(summaries, f->owner);
    core_dict_init(&pt->load_uses, nullptr, nullptr);
    core_dict_init(&pt->external_index, nullptr, nullptr);
    core_dict_init(&pt->alias_roots, nullptr, nullptr);
    core_dict_init(&pt->changed, nullptr, nullptr);
    core_dict_init(&pt->mapped, mapped_hash, mapped_eq);
    core_dict_init(&pt->stored, resolved_hash, resolved_eq);
    core_dict_init(&pt->contents_memo, content_hash, content_eq);
    core_dict_init(&pt->valued, valued_hash, valued_eq);
    core_dict_init(&pt->site_sources, resolved_hash, resolved_eq);
    flow_table_init(&pt->source_names, nullptr);
    core_dict_init(&pt->source_maps, nullptr, nullptr);
    core_dict_init(&pt->flow_nodes, flow_node_hash, flow_node_eq);
    core_dict_init(&pt->heap_sites, heap_site_hash, heap_site_eq);
    core_dict_init(&pt->copy_ins, nullptr, nullptr);
    core_dict_init(&pt->escaped, nullptr, nullptr);
    core_dict_init(&pt->resolved, resolved_hash, resolved_eq);
    core_dict_init(&pt->flow_groups, nullptr, nullptr);
    core_dict_init(&pt->call_index, nullptr, nullptr);
    pt->reach     = ir_reach_new(f);
    pt->aliased   = core_list_new(uint32_t);
    pt->described = core_list_new(uint32_t);
    pt->seen      = (pt_set_t){0};
    for (size_t s = 0; s < 3; s++) pt->scratch[s] = (pt_set_t){0};
    pt->calls     = core_list_new(pt_callees_t);
    pt->context   = core_list_new(pt_access_t);
    pt->flow      = (pt_flow_t){
        .defs  = core_list_new(flow_def_t),
        .steps = core_list_new(flow_step_t),
        .items = (pt_set_t){0},
    };
    core_dict_init(&pt->flow.writes, nullptr, nullptr);
    core_dict_init(&pt->flow.positions, nullptr, nullptr);
    core_dict_init(&pt->flow.pending, nullptr, nullptr);
    core_dict_init(&pt->flow.released, nullptr, nullptr);
    pt->order  = core_list_new(summary_instr_ptr_t);
    pt->reader = PT_NO_READER;
    core_dict_init(&pt->readers, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (is_call(in)) note_callees(pt, in);
            if (step_matters(pt, in)) core_list_push(pt->order, in);
            core_dict_put(&pt->flow.positions, in,
                          (void *)((uintptr_t)b << 32 | i));
        }
    pt->dirty = core_alloc_array(bool, pt->order.len ? pt->order.len : 1);
    note_load_uses(pt);
    note_copy_ins(pt);
    note_writers(pt);
    flow_build(pt);
}

static void
flow_free(pt_flow_t *flow)
{
    core_dict_foreach(&flow->writes, item) {
        pt_writes_t *writes = item->value;
        for (size_t w = 0; w < writes->len; w++)
            core_list_free(writes->data[w].items);
        core_list_free(*writes);
        core_free(writes);
    }
    core_dict_free(&flow->writes);
    core_dict_free(&flow->positions);
    core_dict_free(&flow->pending);
    core_dict_free(&flow->released);
    core_list_free(flow->defs);
    core_list_free(flow->steps);
    core_list_free(flow->items);
    core_free(flow->block_steps);
    core_free(flow->kills);
    core_free(flow->block_in);
    core_free(flow->state);
}

static void
pt_free(pt_t *pt)
{
    for (size_t i = 0; i < pt->values.len; i++)
        core_list_free(pt->values.data[i]);
    for (size_t i = 0; i < pt->sites.len; i++) {
        core_list_free(pt->sites.data[i].contents);
        for (size_t a = 0; a < pt->sites.data[i].aliases.len; a++)
            core_list_free(pt->sites.data[i].aliases.data[a].origins);
        core_list_free(pt->sites.data[i].aliases);
    }
    core_list_free(pt->values);
    core_list_free(pt->sites);
    core_dict_free(&pt->value_index);
    core_dict_free(&pt->site_index);
    core_dict_free(&pt->load_uses);
    ir_reach_free(pt->reach);
    for (size_t i = 0; i < pt->calls.len; i++) {
        core_list_free(pt->calls.data[i].targets.targets);
        core_list_free(pt->calls.data[i].callees);
    }
    core_list_free(pt->calls);
    core_list_free(pt->context);
    core_dict_free(&pt->call_index);
    core_list_free(pt->aliased);
    core_dict_foreach(&pt->alias_roots, entry) {
        core_list_t(uint32_t) *sites = entry->value;
        core_list_free(*sites);
        core_free(sites);
    }
    core_dict_free(&pt->alias_roots);
    core_dict_free(&pt->changed);
    core_dict_foreach(&pt->mapped, entry) {
        mapped_entry_t *mapped = entry->value;
        core_list_free(mapped->items);
        memo_free(&mapped->memo);
        core_free(mapped);
    }
    core_dict_free(&pt->mapped);
    core_dict_foreach(&pt->stored, entry) {
        stored_t *stored = entry->value;
        core_list_free(stored->items);
        core_free(stored);
    }
    core_dict_free(&pt->stored);
    core_dict_foreach(&pt->valued, entry) {
        valued_t *valued = entry->value;
        core_list_free(valued->items);
        memo_free(&valued->memo);
        core_free(valued);
    }
    core_dict_free(&pt->valued);
    for (size_t r = 0; r < pt->external_writes.len; r++)
        core_list_free(pt->external_writes.data[r]);
    core_list_free(pt->external_writes);
    core_dict_free(&pt->external_index);
    core_dict_foreach(&pt->site_sources, entry) {
        site_sources_t *sources = entry->value;
        core_list_free(sources->data);
        core_list_free(sources->control);
        core_free(sources);
    }
    core_dict_free(&pt->site_sources);
    flow_table_free(&pt->source_names);
    core_dict_foreach(&pt->source_maps, entry) {
        source_map_t *map = entry->value;
        core_list_free(map->spans);
        core_list_free(map->pool);
        core_free(map);
    }
    core_dict_free(&pt->source_maps);
    core_list_free(pt->source_stack);
    core_dict_foreach(&pt->flow_nodes, entry) core_free(entry->key);
    core_dict_free(&pt->flow_nodes);
    core_dict_foreach(&pt->heap_sites, entry) core_free(entry->key);
    core_dict_free(&pt->heap_sites);
    core_dict_free(&pt->copy_ins);
    core_dict_foreach(&pt->escaped, entry) {
        region_list_t *regions = entry->value;
        core_list_free(*regions);
        core_free(regions);
    }
    core_dict_free(&pt->escaped);
    core_list_free(pt->published);
    if (pt->entry.built) {
        core_list_free(pt->entry.cells);
        core_dict_free(&pt->entry.gens);
        core_free(pt->entry.in);
        core_free(pt->entry.state);
    }
    core_dict_foreach(&pt->resolved, entry) {
        resolved_t *resolved = entry->value;
        core_list_free(resolved->items);
        memo_free(&resolved->memo);
        core_free(resolved);
    }
    core_dict_free(&pt->resolved);
    core_dict_foreach(&pt->contents_memo, entry) {
        content_memo_t *content = entry->value;
        core_list_free(content->items);
        core_list_free(content->widened);
        core_list_free(content->log);
        memo_free(&content->memo);
        core_free(content);
    }
    core_dict_free(&pt->contents_memo);
    core_dict_foreach(&pt->flow_groups, entry) core_free(entry->value);
    core_dict_free(&pt->flow_groups);
    core_list_free(pt->described);
    core_list_free(pt->seen);
    core_list_free(pt->reading);
    core_list_free(pt->crossed);
    for (size_t s = 0; s < 3; s++) core_list_free(pt->scratch[s]);
    flow_free(&pt->flow);
    core_list_free(pt->order);
    core_dict_foreach(&pt->readers, item) {
        core_list_t(uint32_t) *readers = item->value;
        core_list_free(*readers);
        core_free(readers);
    }
    core_dict_free(&pt->readers);
    core_free(pt->dirty);
}

static bool
release_writers(pt_t *pt)
{
    pt_flow_t *flow = &pt->flow;
    if (!core_dict_len(&flow->pending)) return false;
    for (size_t o = 0; o < pt->order.len; o++) {
        const ir_instr_t *in      = pt->order.data[o];
        bool              pending = false;
        core_dict_get(&flow->pending, in, &pending);
        if (!pending) continue;
        core_dict_put(&flow->released, (void *)in, (void *)1);
        settle_writes(pt, in);
    }
    return true;
}

static void
pt_solve(pt_t *pt)
{
    size_t count = pt->order.len;
    for (size_t i = 0; i < count; i++) {
        const ir_instr_t *in = pt->order.data[i];
        pt->dirty[i] = true;
        if (in->dest_kind == IR_DEST_LOCAL && in->dest_local
            && tracks(pt, in->dest_local->type))
            note_target(pt, in, local_site(pt, in->dest_local), nullptr,
                        false);
    }
    pt->pending = count != 0;
    for (;;) {
        while (pt->pending) {
            pt->pending = false;
            for (size_t i = 0; i < count; i++) {
                if (!pt->dirty[i]) continue;
                pt->dirty[i] = false;
                pt->reader   = (uint32_t)i;
                pt->at       = pt->order.data[i];
                pt_step(pt, pt->at);
            }
        }
        pt->reader = PT_NO_READER;
        pt->at     = nullptr;
        if (release_writers(pt)) continue;
        spread_escapes(pt);
        if (!pt->pending) break;
    }
}

static void
engine_release(engine_t *engine)
{
    if (!engine->pt) return;
    pt_free(engine->pt);
    core_free(engine->pt);
    engine->pt = nullptr;
}

static pt_t *
function_engine(ir_summaries_t *summaries, ir_func_t *f)
{
    bool      found  = false;
    uint64_t  now    = ir_func_shape(f);
    engine_t *engine = core_dict_get(&summaries->engines, f, &found);
    if (found && engine->fingerprint == now) return engine->pt;
    if (!found) {
        engine = core_alloc(engine_t);
        core_dict_put(&summaries->engines, f, engine);
    }
    engine_release(engine);
    engine->fingerprint = now;
    engine->pt          = core_alloc(pt_t);
    pt_init(engine->pt, summaries, f, false);
    pt_solve(engine->pt);
    return engine->pt;
}

static const core_string_t cross_mark    = CORE_STRING_STATIC("*");
static const core_string_t unknown_mark  = CORE_STRING_STATIC("?");
static const core_string_t unknown_cross = CORE_STRING_STATIC("?*");

static const flow_bits_t *
crossings(pt_t *pt, const pt_loc_t *loc)
{
    const ir_spath_t *path     = &loc->path;
    th_type          *type     = nullptr;
    bool              elements = false;
    if (loc->site == PT_EXTERNAL)
        type = root_object_type(pt->f, path, pt);
    else {
        type     = pt->sites.data[loc->site].type;
        elements = pt->sites.data[loc->site].elements;
    }
    flow_bits_t *mask  = &pt->crossed;
    uint32_t     start = 0;
    mask->len          = 0;
    pt->crossed_known  = 0;
    if (elements && path->depth) {
        th_type *sequence = defined_type(pt->f, type);
        if (!core_string_eq(path->steps[0], element_step) || !sequence
            || th_kind_of(sequence) != TH_SEQUENCE) return mask;
        type  = th_seq_elem(sequence);
        start = 1;
    }
    bool     out_of_line = false;
    uint32_t i           = start;
    for (; type && i < path->depth; i++) {
        core_string_t step    = path->steps[i];
        th_type      *named   = type;
        th_type      *current = defined_type(pt->f, type);
        pt->crossed_known     = i;
        if (!current) return mask;
        pt->crossed_known = i + 1;
        if (core_string_eq(step, value_step)) {
            bit_set(mask, i);
            type        = pointee_type(pt->f->owner, current);
            out_of_line = false;
            if (!type) pt->crossed_known = path->depth;
            continue;
        }
        switch (th_kind_of(current)) {
        case TH_SEQUENCE:
            if (!core_string_eq(step, element_step)) return mask;
            if (out_of_line) bit_set(mask, i);
            type        = th_seq_elem(current);
            out_of_line = false;
            break;
        case TH_RECORD: {
            bool known = false;
            type = field_type(pt, current, step);
            bool inside = type
                && member_inline(pt->f, pt, named, current, step, &known);
            out_of_line = type && known && !inside;
            break;
        }
        default:
            return mask;
        }
    }
    pt->crossed_known = i < path->depth ? i : path->depth;
    return mask;
}

static const core_string_t *
region_tokens(pt_t *pt, const pt_item_t *item, uint32_t *count)
{
    const pt_loc_t    *loc   = &item->loc;
    const flow_bits_t *mask  = crossings(pt, loc);
    uint32_t           known = pt->crossed_known;
    core_list_t(core_string_t) tokens = core_list_new(core_string_t);
    for (uint32_t i = 0; i < known; i++) {
        if (bit_has(mask, i)) core_list_push(tokens, cross_mark);
        if (!core_string_eq(loc->path.steps[i], value_step))
            core_list_push(tokens, loc->path.steps[i]);
    }
    bool held = item->kind == PT_CONTENTS && loc->site == PT_EXTERNAL;
    if (known < loc->path.depth)
        core_list_push(tokens, held ? unknown_cross : unknown_mark);
    else if (held) core_list_push(tokens, cross_mark);
    *count = (uint32_t)tokens.len;
    const core_string_t *run = steps_make(tokens.data, *count);
    core_list_free(tokens);
    return run;
}

static void *
region_key(const pt_loc_t *loc)
{
    return (void *)(loc->site == PT_EXTERNAL ? external_key(&loc->path)
                                             : (uintptr_t)loc->site << 2 | 1);
}

static bool
has_mark(const core_string_t *tokens, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++)
        if (core_string_eq(tokens[i], cross_mark)
            || core_string_eq(tokens[i], unknown_cross)) return true;
    return false;
}

static bool
unknown_token(core_string_t token)
{
    return core_string_eq(token, unknown_mark)
        || core_string_eq(token, unknown_cross);
}

static bool
regions_overlap(const pt_region_t *escaped, const core_string_t *tokens,
                uint32_t count, bool deep)
{
    uint32_t common = escaped->count < count ? escaped->count : count;
    for (uint32_t i = 0; i < common; i++) {
        if (unknown_token(escaped->tokens[i]) || unknown_token(tokens[i]))
            return true;
        if (!core_string_eq(escaped->tokens[i], tokens[i])) return false;
    }
    if (count > escaped->count)
        return escaped->deep || !has_mark(tokens, common, count);
    if (escaped->count > count)
        return deep || !has_mark(escaped->tokens, common, escaped->count);
    return true;
}

static region_list_t *
regions_of(pt_t *pt, const pt_loc_t *loc, bool create)
{
    bool           found   = false;
    region_list_t *regions = core_dict_get(&pt->escaped, region_key(loc),
                                           &found);
    if (found || !create) return found ? regions : nullptr;
    regions  = core_alloc(region_list_t);
    *regions = core_list_new(pt_region_t);
    core_dict_put(&pt->escaped, region_key(loc), regions);
    return regions;
}

static bool
add_region(pt_t *pt, const pt_loc_t *loc, const core_string_t *tokens,
           uint32_t count, bool deep)
{
    region_list_t *regions = regions_of(pt, loc, true);
    for (size_t r = 0; r < regions->len; r++) {
        const pt_region_t *have   = &regions->data[r];
        bool               prefix = have->count <= count;
        bool               within = !have->deep;
        for (uint32_t i = 0; prefix && i < have->count; i++)
            prefix = core_string_eq(have->tokens[i], tokens[i]);
        for (uint32_t i = have->count; prefix && within && i < count; i++)
            within = !core_string_eq(tokens[i], cross_mark)
                  && !unknown_token(tokens[i]);
        if (prefix && (!deep || have->deep) && (have->deep || within))
            return false;
    }
    core_list_push(*regions, ((pt_region_t){.tokens = tokens, .count = count,
                                            .deep = deep}));
    return true;
}

static void
escape_contents(pt_t *pt, uint32_t site, const core_string_t *tokens,
                uint32_t inside, pt_set_t *work)
{
    const pt_set_t *contents = &pt->sites.data[site].contents;
    for (size_t i = 0; i < contents->len; i++) {
        pt_item_t held = contents->data[i];
        if (held.flow != PT_SAME) continue;
        uint32_t common = held.depth < inside ? held.depth : inside;
        bool     match  = true;
        for (uint32_t s = 0; match && s < common; s++)
            match = core_string_eq(held.steps[s], tokens[s]);
        if (!match) continue;
        held.depth = 0;
        held.deep  = true;
        held.loc.path.flags |= IR_SPATH_DEEP;
        set_add(pt, work, held);
    }
}

static void
escape_item(pt_t *pt, const pt_item_t *item, pt_set_t *work)
{
    if (item->flow != PT_SAME) return;
    uint32_t             count  = 0;
    const core_string_t *tokens = region_tokens(pt, item, &count);
    bool                 deep   = item->deep || loc_deep(&item->loc);
    if (item->loc.site == PT_EXTERNAL) {
        add_region(pt, &item->loc, tokens, count, deep);
        return;
    }
    uint32_t inside = 0;
    while (inside < count && !core_string_eq(tokens[inside], cross_mark)
           && !unknown_token(tokens[inside]))
        inside++;
    if (add_region(pt, &item->loc, tokens, count, deep))
        escape_contents(pt, item->loc.site, tokens, inside, work);
}

static void
escaped_build(pt_t *pt)
{
    if (pt->escaped_built) return;
    pt->escaped_built = true;
    const ir_func_t *f     = pt->f;
    pt_set_t         work  = (pt_set_t){0};
    for (size_t p = 0; p < pt->published.len; p++)
        set_add(pt, &work, pt->published.data[p]);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->op != IR_OP_RETURN && in->op != IR_OP_FAIL) continue;
            for (uint32_t o = 0; o < in->n_operands; o++)
                pt_read(pt, referenced(in, o), &work);
        }
    while (work.len) {
        pt_item_t item = work.data[--work.len];
        escape_item(pt, &item, &work);
    }
    core_list_free(work);
}

static bool
item_escapes(pt_t *pt, const pt_item_t *item)
{
    if (item->flow != PT_SAME) return false;
    if (item->loc.site == PT_EXTERNAL
        && item->loc.path.root == IR_SPLACE_UNKNOWN) return true;
    const region_list_t *regions = regions_of(pt, &item->loc, false);
    if (!regions) return false;
    uint32_t             count  = 0;
    const core_string_t *tokens = region_tokens(pt, item, &count);
    bool                 deep   = item->deep || loc_deep(&item->loc);
    for (size_t r = 0; r < regions->len; r++)
        if (regions_overlap(&regions->data[r], tokens, count, deep))
            return true;
    return false;
}

static bool
engine_escapes(ir_summaries_t *summaries, const ir_func_t *f,
               const ir_value_t *address)
{
    pt_t *pt = function_engine(summaries, (ir_func_t *)f);
    if (!pt) return true;
    escaped_build(pt);
    pt_set_t items   = (pt_set_t){0};
    bool     escapes = false;
    pt_address(pt, address, &items);
    for (size_t i = 0; !escapes && i < items.len; i++)
        escapes = item_escapes(pt, &items.data[i]);
    core_list_free(items);
    return escapes;
}

static void
pt_record(pt_t *pt, ir_summary_t *summary)
{
    const ir_func_t *f = pt->f;
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            pt->at = f->blocks[b]->instrs[i];
            record_instr(pt, summary, pt->at);
        }
    pt->at = nullptr;
}

static void
note_heap(ir_summary_t *summary, const ir_spath_t *path)
{
    if (path->root != IR_SPLACE_HEAP || !path->param
        || ir_summary_heap_for(summary, path->param)) return;
    core_list_push(summary->heaps, ((ir_sheap_t){.origin = path->param}));
}

static void
note_flow_heaps(ir_summary_t *summary, const flow_table_t *flows)
{
    for (size_t i = 0; i < flows->sources.len; i++)
        note_heap(summary, flows->sources.data[i]);
    for (size_t i = 0; i < flows->dests.len; i++)
        note_heap(summary, flows->dests.data[i].to);
}

static bool
heap_from_callees(const pt_t *pt, ir_sheap_t *heap)
{
    for (size_t c = 0; c < pt->calls.len; c++) {
        const pt_callees_t *callees = &pt->calls.data[c];
        for (size_t t = 0; t < callees->callees.len; t++) {
            const ir_sheap_t *named = ir_summary_heap_for(
                callees->callees.data[t], heap->origin);
            if (!named || !named->name.u8_bytes) continue;
            *heap = *named;
            return true;
        }
    }
    return false;
}

static void
name_heaps(pt_t *pt, ir_summary_t *summary)
{
    summary->heaps.len = 0;
    for (size_t i = 0; i < summary->paths.len; i++)
        note_heap(summary, &summary->paths.data[i]);
    note_flow_heaps(summary, &summary->flows[IR_SFLOW_RESULTS]);
    note_flow_heaps(summary, &summary->flows[IR_SFLOW_STORES]);
    size_t unnamed = 0;
    for (size_t h = 0; h < summary->heaps.len; h++)
        unnamed += !heap_from_callees(pt, &summary->heaps.data[h]);
    const ir_func_t *f      = pt->f;
    core_string_t    module = f->owner ? ir_module_name(f->owner)
                                       : (core_string_t){0};
    for (size_t s = 0; unnamed && s < pt->sites.len; s++) {
        const pt_site_t *site = &pt->sites.data[s];
        if (!site->local && !site->value) continue;
        ir_sheap_t *heap = (ir_sheap_t *)ir_summary_heap_for(summary,
                                                            site->origin);
        if (!heap || heap->name.u8_bytes) continue;
        ir_span_t span = site->local ? site->local->span : f->span;
        *heap = (ir_sheap_t){
            .origin = heap->origin, .module = module, .name = f->name,
            .line = span.start_line, .column = span.start_col,
        };
        unnamed--;
    }
    for (uint32_t b = 0; unnamed && b < f->n_blocks; b++)
        for (uint32_t i = 0; unnamed && i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in     = f->blocks[b]->instrs[i];
            ir_sheap_t       *heap   = (ir_sheap_t *)ir_summary_heap_for(
                summary, instr_origin(pt, in));
            if (!heap || heap->name.u8_bytes) continue;
            *heap = (ir_sheap_t){
                .origin = heap->origin, .module = module, .name = f->name,
                .line = in->span.start_line, .column = in->span.start_col,
            };
            unnamed--;
        }
    for (size_t s = 0; s < pt->sites.len; s++) {
        const pt_site_t *site = &pt->sites.data[s];
        ir_sheap_t      *heap = (ir_sheap_t *)ir_summary_heap_for(summary,
                                                                 site->origin);
        if (!heap || heap->type || !site->type) continue;
        heap->type     = site->type;
        heap->elements = site->elements;
    }
}

static bool
unknown_root(const ir_spath_t *path)
{
    return path->root == IR_SPLACE_UNKNOWN;
}

static bool
flows_unknown(const flow_table_t *flows)
{
    for (size_t i = 0; i < flows->sources.len; i++)
        if (unknown_root(flows->sources.data[i])) return true;
    for (size_t i = 0; i < flows->dests.len; i++)
        if (unknown_root(flows->dests.data[i].to)) return true;
    return false;
}

static bool
exports_unknown(const ir_summary_t *summary)
{
    for (size_t i = 0; i < summary->paths.len; i++)
        if (unknown_root(&summary->paths.data[i])) return true;
    return flows_unknown(&summary->flows[IR_SFLOW_RESULTS])
        || flows_unknown(&summary->flows[IR_SFLOW_STORES]);
}

static void
explain_unknown(ir_summary_t *summary, const ir_switness_t *witness)
{
    if (!exports_unknown(summary)
        || ir_summary_witness_for(summary, IR_SWITNESS_UNKNOWN)) return;
    core_list_push(summary->witnesses, *witness);
}

static void
summarize_paths(ir_summaries_t *summaries, ir_summary_t *summary,
                ir_func_t *f)
{
    pt_t pt;
    pt_init(&pt, summaries, f, !summaries->escapes_only);
    pt_solve(&pt);
    pt.places = ir_func_places(f);
    pt_record(&pt, summary);
    summarize_results(&pt, summary);
    describe_heap(&pt, summary, &pt.described);
    name_heaps(&pt, summary);
    if (pt.unknown_at) explain_unknown(summary, &pt.unknown);
    if (!summaries->escapes_only)
        summarize_writes(summaries, f, &pt, summary);
    pt_free(&pt);
}
