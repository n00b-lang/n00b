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
    return identity->mutability == IR_MUT_CONST
        || (identity->mutability != IR_MUT_MUTABLE
            && identity->has_static_initializer);
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

static bool
kills_cell(const flow_def_t *def, core_string_t cell)
{
    return def->kill == FLOW_KILL_SITE
        || (def->kill == FLOW_KILL_CELL && core_string_eq(def->cell, cell));
}

static void
flow_build(pt_t *pt)
{
    pt_flow_t       *flow = &pt->flow;
    const ir_func_t *f    = pt->f;
    flow->defs.len = 0;
    core_dict_free(&flow->at_instr);
    core_dict_init(&flow->at_instr, nullptr, nullptr);
    core_dict_free(&flow->covered);
    core_dict_init(&flow->covered, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
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
            if (flow->defs.len != first)
                core_dict_put(&flow->at_instr, (void *)in, (void *)(
                    (uintptr_t)first << 32 | (flow->defs.len - first)));
        }
    }
    flow->stale = false;
}

static void
flow_sources(pt_t *pt)
{
    pt_flow_t *flow = &pt->flow;
    if (flow->sourced) return;
    flow->sourced = true;
    ir_pdg_t *pdg = ir_func_pdg((ir_func_t *)pt->f);
    pt->pdg      = pdg;
    flow->memory = ir_pdg_memory(pdg);
    flow->rd     = ir_pdg_rd(pdg);
}

static bool
site_covered(pt_t *pt, uint32_t site)
{
    pt_flow_t *flow  = &pt->flow;
    void      *key   = (void *)(uintptr_t)(site + 1);
    bool       found = false;
    uintptr_t  known = (uintptr_t)core_dict_get(&flow->covered, key, &found);
    if (found) return known == 2;
    const ir_local_t *local   = pt->sites.data[site].local;
    bool              chained = local && local->address_taken;
    bool              covered = local
        && (chained ? flow->memory != nullptr : flow->rd != nullptr);
    for (size_t d = 0; covered && d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->site != site) continue;
        covered = chained
            ? ir_pdg_memory_defines(flow->memory, def->instr)
            : def->instr->dest_kind == IR_DEST_LOCAL
                && def->instr->dest_local == local;
    }
    core_dict_put(&flow->covered, key, (void *)(uintptr_t)(covered ? 2 : 1));
    return covered;
}

static void
reach_instr(pt_flow_t *flow, const ir_instr_t *in, uint32_t site,
            core_string_t cell, bool *stop)
{
    bool      found = false;
    uintptr_t range = (uintptr_t)core_dict_get(&flow->at_instr, in, &found);
    uint32_t  first = (uint32_t)(range >> 32);
    uint32_t  count = (uint32_t)range;
    *stop = core_dict_contains(&flow->pending, (void *)in);
    for (uint32_t d = first; found && d < first + count; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->site != site) continue;
        if (core_string_eq(def->cell, cell)) core_list_push(flow->reached, d);
        *stop |= kills_cell(def, cell);
    }
}

static void
chain_reach(pt_flow_t *flow, uint32_t site, core_string_t cell,
            uint32_t start)
{
    const ir_pdg_memory_t *m = flow->memory;
    uint64_t mark = ++flow->mark;
    flow->walk.len = 0;
    core_list_push(flow->walk, start);
    while (flow->walk.len) {
        uint32_t id = flow->walk.data[--flow->walk.len];
        if (id == UINT32_MAX) continue;
        void *key = (void *)(uintptr_t)(id + 1);
        if ((uintptr_t)core_dict_get(&flow->visited, key, nullptr) == mark)
            continue;
        core_dict_put(&flow->visited, key, (void *)(uintptr_t)mark);
        const ir_memdef_t *def = ir_pdg_memory_def(m, id);
        if (def->kind == IR_MEMDEF_ENTRY) continue;
        if (def->kind == IR_MEMDEF_PHI || def->kind == IR_MEMDEF_ELEMENT_PHI) {
            for (uint32_t p = 0; p < ir_pdg_memory_phi_count(m, id); p++)
                core_list_push(flow->walk, ir_pdg_memory_phi_incoming(m, id, p));
            continue;
        }
        bool      stop = false;
        uintptr_t seen = (uintptr_t)core_dict_get(&flow->visited, def->instr,
                                                  nullptr);
        if (seen >> 1 == mark) stop = seen & 1;
        else {
            reach_instr(flow, def->instr, site, cell, &stop);
            core_dict_put(&flow->visited, def->instr,
                          (void *)(uintptr_t)(mark << 1 | stop));
        }
        if (!stop) core_list_push(flow->walk, ir_pdg_memory_def_prev(m, id));
    }
}

static bool
reaching_defs(pt_t *pt, uint32_t site, const ir_instr_t *at)
{
    pt_flow_t *flow = &pt->flow;
    if (!core_dict_contains(&flow->positions, (void *)at)) return false;
    if (flow->stale) flow_build(pt);
    flow_sources(pt);
    if (!site_covered(pt, site)) return false;
    const ir_local_t *local = pt->sites.data[site].local;
    flow->reached.len = 0;
    if (!local->address_taken) {
        ir_instr_t *use = (ir_instr_t *)at;
        if (!ir_rd_is_use(flow->rd, use, (ir_local_t *)local)) return false;
        uint32_t count = ir_rd_count(flow->rd, use, (ir_local_t *)local);
        for (uint32_t r = 0; r < count; r++) {
            const ir_instr_t *def = ir_rd_at(flow->rd, use,
                                             (ir_local_t *)local, r);
            bool      found = false;
            uintptr_t range = (uintptr_t)core_dict_get(&flow->at_instr, def,
                                                       &found);
            uint32_t  first = (uint32_t)(range >> 32);
            for (uint32_t d = first; found && d < first + (uint32_t)range; d++)
                if (flow->defs.data[d].site == site)
                    core_list_push(flow->reached, d);
        }
        return true;
    }
    uint32_t start = ir_pdg_memory_def_before(flow->memory, at);
    if (start == UINT32_MAX) return false;
    for (size_t d = 0; d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        bool              first = def->site == site;
        for (size_t e = 0; first && e < d; e++)
            first = flow->defs.data[e].site != site
                || !core_string_eq(flow->defs.data[e].cell, def->cell);
        if (first) chain_reach(flow, site, def->cell, start);
    }
    return true;
}

static void
settle_writes(pt_t *pt, const ir_instr_t *in)
{
    pt_flow_t *flow = &pt->flow;
    if (!core_dict_contains(&flow->pending, (void *)in)) return;
    core_dict_remove(&flow->pending, in);
    uint32_t last = UINT32_MAX;
    for (size_t d = 0; d < flow->defs.len; d++) {
        uint32_t site = flow->defs.data[d].site;
        if (site == last) continue;
        last = site;
        pt_notify(pt, contents_key(site));
    }
}
