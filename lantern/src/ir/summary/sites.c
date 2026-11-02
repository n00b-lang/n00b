#include "internal.h"

static bool
pointer_type(pt_t *pt, th_type *type)
{
    if (!type) return true;
    bool  found  = false;
    void *cached = core_dict_get(&pt->types->pointer_types, type, &found);
    if (found) return cached != nullptr;
    bool pointers = holds_pointers(pt->f, type, 0);
    core_dict_put(&pt->types->pointer_types, type, pointers ? type : nullptr);
    return pointers;
}

static bool
tracks(pt_t *pt, th_type *type)
{
    return pt->value_flow || pointer_type(pt, type);
}

static bool
constant_global(const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    return identity->mutability != IR_MUT_MUTABLE
        && identity->has_static_initializer;
}

static ir_sflow_kind_t
flow_kind(pt_flow_kind_t flow)
{
    return flow == PT_CONTROL ? IR_SFLOW_CONTROL
         : flow == PT_COPY ? IR_SFLOW_COPY : IR_SFLOW_DATA;
}


static bool
constant_source(const pt_item_t *item)
{
    return item->loc.site == PT_EXTERNAL && named_root(item->loc.path.root)
        && item->loc.path.global && constant_global(item->loc.path.global);
}

static bool
constant_item(const pt_item_t *item)
{
    return item->flow != PT_SAME && constant_source(item);
}

static void
derive_items(pt_set_t *items, pt_flow_kind_t flow)
{
    for (size_t i = 0; i < items->len; i++)
        if (items->data[i].flow < flow) items->data[i].flow = flow;
}

static size_t
put_decimal(char *out, uint32_t value)
{
    char   digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (size_t i = 0; i < count; i++) out[i] = digits[count - 1 - i];
    return count;
}

static size_t
put_bytes(char *out, core_string_t text)
{
    if (text.u8_bytes) memcpy(out, text.data, text.u8_bytes);
    return text.u8_bytes;
}

static uint32_t
origin_of(const ir_func_t *f, core_string_t name, uint32_t block,
          uint32_t index)
{
    core_string_t module = f->owner ? ir_module_name(f->owner)
                                    : (core_string_t){0};
    size_t need = module.u8_bytes + f->name.u8_bytes + name.u8_bytes + 24;
    char   local[256];
    char  *key = need <= sizeof local ? local : core_alloc_array(char, need);
    size_t at  = put_bytes(key, module);
    key[at++]  = 0;
    at        += put_bytes(key + at, f->name);
    key[at++]  = 0;
    at        += put_bytes(key + at, name);
    key[at++]  = ':';
    at        += put_decimal(key + at, block);
    key[at++]  = ':';
    at        += put_decimal(key + at, index);
    uint64_t hash = core_xxh3_64bits_raw(key, at);
    if (key != local) core_free(key);
    return (uint32_t)(hash ^ hash >> 32) | 1;
}

static uint32_t
instr_origin(const pt_t *pt, const ir_instr_t *in)
{
    return origin_of(pt->f, (core_string_t){0},
                     in->block ? in->block->index : 0, in->index);
}

static uint32_t
site_of(pt_t *pt, const void *key, th_type *type, bool elements,
        uint32_t origin)
{
    bool  found = false;
    void *index = core_dict_get(&pt->site_index, key, &found);
    if (found) return (uint32_t)(uintptr_t)index;
    uint32_t site = (uint32_t)pt->sites.len;
    core_list_push(pt->sites, ((pt_site_t){
        .key = key, .type = type, .elements = elements,
        .contents = (pt_set_t){0},
        .aliases  = (alias_list_t){0},
        .origin   = origin}));
    core_dict_put(&pt->site_index, key, (void *)(uintptr_t)site);
    return site;
}

static bool
known_site(pt_t *pt, const void *key, uint32_t *site)
{
    bool  found = false;
    void *index = core_dict_get(&pt->site_index, key, &found);
    *site       = (uint32_t)(uintptr_t)index;
    return found;
}

static uint32_t
local_site(pt_t *pt, const ir_local_t *local)
{
    uint32_t site = 0;
    if (known_site(pt, local, &site)) return site;
    site = site_of(pt, local, local->type, false,
                   origin_of(pt->f, local->name, UINT32_MAX, 0));
    pt->sites.data[site].local = local;
    return site;
}

static bool
by_value(ir_module_t *module, th_type *type)
{
    if (th_builtin_kind(ir_ctx_types(module->ctx), type) == TH_BUILTIN_STRING)
        return true;
    th_type *shape  = definition_in(module, type);
    uint64_t extent = 0;
    if (!shape) return false;
    switch (th_kind_of(shape)) {
    case TH_RECORD:
    case TH_SUM:
        return true;
    case TH_SEQUENCE:
        return th_seq_extent(shape, &extent);
    default:
        return false;
    }
}

static uint32_t
variable_site(pt_t *pt, const ir_value_t *value)
{
    if (value->kind == IR_VAL_LOCAL) return local_site(pt, value->local);
    uint32_t site = 0;
    if (known_site(pt, value, &site)) return site;
    site = site_of(pt, value, value->type, false,
                   origin_of(pt->f, (core_string_t){0}, UINT32_MAX - 1,
                             value->index));
    pt->sites.data[site].value = value;
    if (!tracks(pt, value->type)) return site;
    uint32_t param   = value->index;
    bool     own     = param < pt->f->n_params && pt->f->params[param] == value;
    bool     pointer = pointer_type(pt, value->type);
    bool     whole   = value->type && by_value(pt->f->owner, value->type);
    if (!own)
        note_unknown(pt, CORE_STRING_STATIC("an operand the analysis cannot "
                                            "resolve"), nullptr);
    pt_loc_t  loc  = own ? external_loc(IR_SPLACE_PARAM, param, nullptr)
                         : unknown_loc();
    pt_item_t item = own || !pointer || whole ? contents_item(loc)
                                              : item_at(loc);
    if (!pointer) item.flow = PT_COPY;
    set_add(pt, &pt->sites.data[site].contents, item);
    return site;
}

static uint32_t
allocation_site(pt_t *pt, const ir_instr_t *in)
{
    th_type *type  = in->dest ? th_resolve(in->dest->type) : nullptr;
    th_type *shape = type ? defined_type(pt->f, type) : nullptr;
    if (shape && th_kind_of(shape) == TH_INDIRECTION)
        return site_of(pt, in, th_ind_referent(shape), false,
                       instr_origin(pt, in));
    bool elements = shape && th_kind_of(shape) == TH_SEQUENCE;
    return site_of(pt, in, elements ? type : nullptr, elements,
                   instr_origin(pt, in));
}

static core_string_t
item_cell(const pt_item_t *item)
{
    return item->depth ? item->steps[0] : (core_string_t){0};
}

static bool
write_has_cell(const pt_write_t *write, const pt_item_t *item)
{
    core_string_t cell = item_cell(item);
    for (size_t i = 0; i < write->items.len; i++) {
        const pt_item_t *held = &write->items.data[i];
        if ((held->flow == PT_SAME) == (item->flow == PT_SAME)
            && core_string_eq(item_cell(held), cell))
            return true;
    }
    return false;
}

static pt_write_t *
note_target(pt_t *pt, const ir_instr_t *in, uint32_t site, const pt_loc_t *at,
            bool weak)
{
    if (!in || !pt->sites.data[site].local) return nullptr;
    pt_flow_t   *flow   = &pt->flow;
    bool         found  = false;
    pt_writes_t *writes = core_dict_get(&flow->writes, in, &found);
    if (!writes) {
        writes  = core_alloc(pt_writes_t);
        *writes = core_list_new(pt_write_t);
        core_dict_put(&flow->writes, in, writes);
    }
    pt_write_t *write = nullptr;
    for (size_t w = 0; !write && w < writes->len; w++)
        if (writes->data[w].site == site) write = &writes->data[w];
    ir_spath_t path = at ? at->path : (ir_spath_t){0};
    weak |= at && loc_deep(at);
    if (!write) {
        core_list_push(*writes, ((pt_write_t){
            .site = site, .path = path, .weak = weak,
            .items = (pt_set_t){0}}));
        flow->stale = true;
        return &writes->data[writes->len - 1];
    }
    if (!write->weak
        && (weak || write->path.depth != path.depth
            || !steps_prefix(path.depth, path.steps, write->path.depth,
                             write->path.steps))) {
        write->weak = true;
        flow->stale = true;
        pt_notify(pt, contents_key(site));
    }
    return write;
}

static void
note_write(pt_t *pt, uint32_t site, const pt_loc_t *at, bool weak,
           pt_item_t item)
{
    pt_write_t *write = note_target(pt, pt->at, site, at, weak);
    if (!write) return;
    if (!write_has_cell(write, &item)) pt->flow.stale = true;
    if (set_add(pt, &write->items, item)) pt_notify(pt, contents_key(site));
}

static flow_kill_t
write_kill(const pt_write_t *write)
{
    if (write->weak) return FLOW_KILL_NONE;
    if (!write->path.depth) return FLOW_KILL_SITE;
    return write->path.depth == 1
            && !core_string_eq(write->path.steps[0], element_step)
        ? FLOW_KILL_CELL : FLOW_KILL_NONE;
}

static void
flow_apply(const pt_flow_t *flow, const flow_step_t *step, uint64_t *state)
{
    if (step->block) memset(state, 0, flow->words * sizeof(uint64_t));
    for (uint32_t d = step->first; d < step->first + step->count; d++) {
        const uint64_t *kill = &flow->kills[(size_t)d * flow->words];
        for (uint32_t w = 0; w < flow->words; w++) state[w] &= ~kill[w];
    }
    for (uint32_t d = step->first; d < step->first + step->count; d++)
        state[d / 64] |= UINT64_C(1) << (d % 64);
}

static void
flow_kills(pt_flow_t *flow)
{
    core_free(flow->kills);
    flow->kills = core_alloc_array(uint64_t,
                                   flow->defs.len * flow->words + 1);
    for (size_t d = 0; d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->kill == FLOW_KILL_NONE) continue;
        uint64_t *kill = &flow->kills[d * flow->words];
        for (size_t e = 0; e < flow->defs.len; e++)
            if (flow->defs.data[e].site == def->site
                && (def->kill == FLOW_KILL_SITE
                    || core_string_eq(flow->defs.data[e].cell, def->cell)))
                kill[e / 64] |= UINT64_C(1) << (e % 64);
    }
}

static void
flow_build(pt_t *pt)
{
    pt_flow_t       *flow = &pt->flow;
    const ir_func_t *f    = pt->f;
    flow->defs.len  = 0;
    flow->steps.len = 0;
    core_free(flow->block_steps);
    flow->block_steps = core_alloc_array(uint32_t, f->n_blocks + 1);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        flow->block_steps[b] = (uint32_t)flow->steps.len;
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in     = f->blocks[b]->instrs[i];
            bool              found  = false;
            pt_writes_t      *writes = core_dict_get(&flow->writes, in,
                                                     &found);
            uint32_t first = (uint32_t)flow->defs.len;
            for (size_t r = 0; writes && r < writes->len; r++) {
                const pt_write_t *write = &writes->data[r];
                flow_kill_t       kill  = write_kill(write);
                size_t            start = flow->defs.len;
                th_type *record = kill == FLOW_KILL_SITE
                    ? record_of(f, pt->sites.data[write->site].type)
                    : nullptr;
                for (size_t m = 0; record && m < th_record_len(record); m++)
                    core_list_push(flow->defs, ((flow_def_t){
                        .instr = in, .record = (uint32_t)r,
                        .site = write->site,
                        .cell = th_record_member(record, m)->name,
                        .kill = kill, .member = true}));
                for (size_t t = 0; t < write->items.len; t++) {
                    if (record && write->items.data[t].flow != PT_SAME)
                        continue;
                    core_string_t cell = item_cell(&write->items.data[t]);
                    bool          seen = false;
                    for (size_t d = start; !seen && d < flow->defs.len; d++)
                        seen = !flow->defs.data[d].member
                            && core_string_eq(flow->defs.data[d].cell, cell);
                    if (!seen)
                        core_list_push(flow->defs, ((flow_def_t){
                            .instr = in, .record = (uint32_t)r,
                            .site = write->site,
                            .cell = kill == FLOW_KILL_CELL
                                ? write->path.steps[0] : cell,
                            .kill = kill, .pointers = record != nullptr}));
                }
                if (kill != FLOW_KILL_NONE && flow->defs.len == start)
                    core_list_push(flow->defs, ((flow_def_t){
                        .instr = in, .record = (uint32_t)r,
                        .site = write->site,
                        .cell = kill == FLOW_KILL_CELL
                            ? write->path.steps[0] : (core_string_t){0},
                        .kill = kill}));
            }
            bool block = false;
            core_dict_get(&flow->pending, in, &block);
            if (flow->defs.len != first || block)
                core_list_push(flow->steps, ((flow_step_t){
                    .index = i, .first = first,
                    .count = (uint32_t)flow->defs.len - first,
                    .block = block}));
        }
    }
    flow->block_steps[f->n_blocks] = (uint32_t)flow->steps.len;
    flow->words = (uint32_t)((flow->defs.len + 63) / 64) + 1;
    flow_kills(flow);
    core_free(flow->block_in);
    core_free(flow->state);
    flow->block_in = core_alloc_array(uint64_t,
                                      (size_t)flow->words * f->n_blocks + 1);
    flow->state    = core_alloc_array(uint64_t, flow->words);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            memcpy(flow->state, &flow->block_in[(size_t)b * flow->words],
                   flow->words * sizeof(uint64_t));
            for (uint32_t s = flow->block_steps[b];
                 s < flow->block_steps[b + 1]; s++)
                flow_apply(flow, &flow->steps.data[s], flow->state);
            ir_instr_t *last = block->n_instrs
                ? block->instrs[block->n_instrs - 1] : nullptr;
            for (uint32_t s = 0; last && s < ir_succ_count(last); s++) {
                ir_block_t *next = ir_succ_at(last, s);
                uint64_t   *in   = next
                    ? &flow->block_in[(size_t)next->index * flow->words]
                    : nullptr;
                for (uint32_t w = 0; in && w < flow->words; w++) {
                    uint64_t merged = in[w] | flow->state[w];
                    changed |= merged != in[w];
                    in[w] = merged;
                }
            }
        }
    }
    flow->stale = false;
}

static bool
flow_state_at(pt_t *pt, const ir_instr_t *in)
{
    pt_flow_t *flow  = &pt->flow;
    bool       found = false;
    uintptr_t  at    = (uintptr_t)core_dict_get(&flow->positions, in, &found);
    if (!found) return false;
    if (flow->stale) flow_build(pt);
    uint32_t block = (uint32_t)(at >> 32), index = (uint32_t)at;
    memcpy(flow->state, &flow->block_in[(size_t)block * flow->words],
           flow->words * sizeof(uint64_t));
    for (uint32_t s = flow->block_steps[block];
         s < flow->block_steps[block + 1]
         && flow->steps.data[s].index < index; s++)
        flow_apply(flow, &flow->steps.data[s], flow->state);
    return true;
}

static void
settle_writes(pt_t *pt, const ir_instr_t *in)
{
    pt_flow_t *flow    = &pt->flow;
    bool       pending = false;
    core_dict_get(&flow->pending, in, &pending);
    if (!pending) return;
    if (flow_state_at(pt, in))
        for (size_t d = 0; d < flow->defs.len; d++)
            if (flow->state[d / 64] >> (d % 64) & 1)
                pt_notify(pt, contents_key(flow->defs.data[d].site));
    core_dict_remove(&flow->pending, in);
    flow->stale = true;
}
