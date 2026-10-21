#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/memssa.h"
#include "ir/summary.h"
#include "ir/vn.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    ir_memdef_t def;
    uint32_t    prev;
    uint32_t    first_incoming;
    uint32_t    incoming_count;
} memdef_node_t;

core_list_decl(memdef_node_t);
core_list_decl(uint32_t);

struct ir_memssa_t {
    ir_func_t                 *f;
    ir_doms_t                 *doms;
    ir_places_t               *places;
    ir_value_numbers_t        *numbers;
    core_list_t(memdef_node_t) defs;
    core_list_t(uint32_t)      incoming;
    uint32_t                  *use_def;
    uint32_t                  *phi_of;
    uint32_t                  *block_in;
    core_dict_t                last_def;
    core_list_t(uint32_t)      current;
    core_dict_t                element_phis;
    core_list_t(uint32_t)      pending_phis;
    core_list_t(uint32_t)      links;
    uint32_t                  *link_first;
    uint32_t                  *link_count;
    core_dict_t                call_writes;
};

static uint32_t
add_def(ir_memssa_t *m, ir_memdef_t def, uint32_t prev)
{
    core_list_push(m->defs, ((memdef_node_t){.def = def, .prev = prev}));
    return (uint32_t)(m->defs.len - 1);
}

static bool
call_like(const ir_instr_t *in)
{
    return in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE
        || in->op == IR_OP_BUFFER_AS_STRING
        || in->op == IR_OP_BUFFER_BORROW_END;
}

static const ir_value_t *
uncopied_value(const ir_value_t *value)
{
    for (uint32_t i = 0; value && value->def && i < 64; i++) {
        const ir_instr_t *def = value->def;
        if (def->op != IR_OP_COPY || def->n_operands != 1
            || def->operands[0].kind != IR_OPD_VALUE
            || def->operands[0].value == value) break;
        value = def->operands[0].value;
    }
    return value;
}

static const ir_value_t *
operand_value(const ir_instr_t *in, uint32_t index)
{
    return index < in->n_operands && in->operands[index].kind == IR_OPD_VALUE
        ? in->operands[index].value : nullptr;
}

bool
ir_memssa_element_of(const ir_instr_t *in, const ir_value_t **base,
                     const ir_value_t **index)
{
    if (!in) return false;
    if (in->op == IR_OP_SEQUENCE_GET || in->op == IR_OP_SEQUENCE_SET) {
        *base = operand_value(in, 0);
        *index = operand_value(in, 1);
        return *base && *index;
    }
    if (in->op != IR_OP_LOAD && in->op != IR_OP_STORE) return false;
    const ir_value_t *address = uncopied_value(operand_value(in, 0));
    const ir_instr_t *element = address ? address->def : nullptr;
    if (!element || element->op != IR_OP_INDEX) return false;
    *base = operand_value(element, 0);
    *index = operand_value(element, 1);
    return *base && *index;
}

static void
block_accesses(ir_memssa_t *m, ir_block_t *block, uint32_t *cursor,
               uint32_t *def)
{
    for (uint32_t i = 0; i < block->n_instrs; i++) {
        ir_instr_t *in     = block->instrs[i];
        uint32_t    before = *def;
        uint32_t    first  = *cursor;
        uint32_t    end   = first;
        uint32_t    total = ir_places_access_count(m->places);
        while (end < total && ir_places_access_at(m->places, end)->instr == in)
            end++;
        for (uint32_t a = first; a < end; a++) {
            const ir_access_t *access = ir_places_access_at(m->places, a);
            if (access->kind == IR_ACCESS_LOAD
                && !ir_places_access_private(m->places, access))
                m->use_def[a] = *def;
        }
        if (call_like(in))
            *def = add_def(m, (ir_memdef_t){.kind = IR_MEMDEF_CALL,
                                            .instr = in, .block = block},
                           *def);
        const ir_value_t *base = nullptr, *index = nullptr;
        bool element = ir_memssa_element_of(in, &base, &index);
        for (uint32_t a = first; a < end; a++) {
            const ir_access_t *access = ir_places_access_at(m->places, a);
            if (access->kind == IR_ACCESS_STORE
                && !ir_places_access_private(m->places, access))
                *def = add_def(m, (ir_memdef_t){
                    .kind = IR_MEMDEF_STORE, .instr = in, .access = access,
                    .block = block,
                    .element_base = element
                        ? ir_value_number(m->numbers, base) : 0,
                    .element_index = element ? index : nullptr,
                }, *def);
        }
        if (*def != before)
            core_dict_put(&m->last_def, in, (void *)(uintptr_t)(*def + 1));
        *cursor = end;
    }
}

typedef struct {
    ir_memssa_t *m;
    uint32_t    *block_cursor;
    uint32_t    *block_out;
} rename_t;

static bool
rename_pre(ir_block_t *block, void *thunk)
{
    rename_t    *r = thunk;
    ir_memssa_t *m = r->m;
    uint32_t     def = m->current.data[m->current.len - 1];
    if (m->phi_of[block->index] != UINT32_MAX) def = m->phi_of[block->index];
    m->block_in[block->index] = def;
    uint32_t cursor = r->block_cursor[block->index];
    block_accesses(m, block, &cursor, &def);
    r->block_out[block->index] = def;
    core_list_push(m->current, def);
    return true;
}

static bool
rename_post(ir_block_t *block, void *thunk)
{
    (void)block;
    rename_t *r = thunk;
    r->m->current.len--;
    return true;
}

static bool
places_overlap(const ir_places_t *places, const ir_access_t *a,
               const ir_access_t *b)
{
    const uint32_t *x = ir_places_access_places(places, a);
    const uint32_t *y = ir_places_access_places(places, b);
    for (uint32_t i = 0; i < a->count; i++)
        for (uint32_t j = 0; j < b->count; j++)
            if (ir_places_may_alias(places, x[i], y[j])) return true;
    return false;
}

static bool
all_private(const ir_places_t *places, const ir_access_t *access)
{
    const uint32_t *ids = ir_places_access_places(places, access);
    for (uint32_t i = 0; i < access->count; i++) {
        const ir_place_t *place = ir_place_at(places, ids[i]);
        if (place->kind != IR_PLACE_LOCAL || place->exposed) return false;
    }
    return true;
}

static bool
call_writes_place(ir_memssa_t *m, const ir_instr_t *call, uint32_t place)
{
    uintptr_t key = ((uintptr_t)call->block->index << 40)
        | ((uintptr_t)call->index << 20) | (uintptr_t)(place + 1);
    bool found = false;
    uintptr_t known = (uintptr_t)core_dict_get(&m->call_writes,
                                               (void *)key, &found);
    if (found) return known == 2;
    bool writes = ir_summary_call_writes(ir_analysis_summaries(), m->f,
                                         m->places, call, &place, 1);
    core_dict_put(&m->call_writes, (void *)key,
                  (void *)(uintptr_t)(writes ? 2 : 1));
    return writes;
}

static bool
call_clobbers(ir_memssa_t *m, const ir_instr_t *call, const ir_access_t *load)
{
    if (all_private(m->places, load)) return false;
    const uint32_t *ids = ir_places_access_places(m->places, load);
    for (uint32_t i = 0; i < load->count; i++)
        if (call_writes_place(m, call, ids[i])) return true;
    return false;
}

static bool
covers(const ir_places_t *places, const ir_access_t *store,
       const ir_access_t *load)
{
    if (!store->whole || store->count != 1 || load->count != 1) return false;
    uint32_t a = ir_places_access_places(places, store)[0];
    uint32_t b = ir_places_access_places(places, load)[0];
    ir_place_kind_t kind = ir_place_at(places, a)->kind;
    return a == b && kind != IR_PLACE_HEAP && kind != IR_PLACE_UNKNOWN;
}

static uint32_t element_reaching(ir_memssa_t *m, uint32_t def, uint32_t base,
                                 const ir_access_t *load);

static uint32_t
element_phi(ir_memssa_t *m, uint32_t memory_phi, uint32_t base)
{
    uint64_t key = ((uint64_t)memory_phi << 32) | base;
    bool found = false;
    uintptr_t known = (uintptr_t)core_dict_get(&m->element_phis,
                                               (void *)(uintptr_t)key, &found);
    if (found) return (uint32_t)known;
    uint32_t count = m->defs.data[memory_phi].incoming_count;
    uint32_t id = add_def(m, (ir_memdef_t){
        .kind = IR_MEMDEF_ELEMENT_PHI,
        .block = m->defs.data[memory_phi].def.block,
        .element_base = base,
        .memory_phi = memory_phi,
    }, UINT32_MAX);
    m->defs.data[id].first_incoming = (uint32_t)m->incoming.len;
    m->defs.data[id].incoming_count = count;
    for (uint32_t i = 0; i < count; i++)
        core_list_push(m->incoming, UINT32_MAX);
    core_dict_put(&m->element_phis, (void *)(uintptr_t)key,
                  (void *)(uintptr_t)id);
    core_list_push(m->pending_phis, id);
    return id;
}

static uint32_t
element_reaching(ir_memssa_t *m, uint32_t def, uint32_t base,
                 const ir_access_t *load)
{
    while (def != UINT32_MAX) {
        const memdef_node_t *node = &m->defs.data[def];
        switch (node->def.kind) {
        case IR_MEMDEF_ENTRY:
        case IR_MEMDEF_ELEMENT_PHI:
            return def;
        case IR_MEMDEF_PHI:
            return element_phi(m, def, base);
        case IR_MEMDEF_CALL:
            if (call_clobbers(m, node->def.instr, load)) return def;
            def = node->prev;
            break;
        case IR_MEMDEF_STORE:
            if (places_overlap(m->places, node->def.access, load)) return def;
            def = node->prev;
            break;
        }
    }
    return def;
}

static void
fill_element_phis(ir_memssa_t *m, const ir_access_t *load)
{
    while (m->pending_phis.len) {
        uint32_t id = m->pending_phis.data[--m->pending_phis.len];
        uint32_t phi = m->defs.data[id].def.memory_phi;
        uint32_t base = m->defs.data[id].def.element_base;
        for (uint32_t i = 0; i < m->defs.data[id].incoming_count; i++) {
            uint32_t from = m->incoming.data[
                m->defs.data[phi].first_incoming + i];
            uint32_t reaching = element_reaching(m, from, base, load);
            m->incoming.data[m->defs.data[id].first_incoming + i] = reaching;
        }
    }
}

static uint32_t
load_base(const ir_memssa_t *m, const ir_access_t *load,
          const ir_value_t **index)
{
    const ir_value_t *base = nullptr;
    if (!ir_memssa_element_of(load->instr, &base, index)) return 0;
    return ir_value_number(m->numbers, base);
}

ir_element_relation_t
ir_memssa_element_relation(const ir_memssa_t *m, uint32_t def,
                           uint32_t load_access)
{
    const ir_access_t *load = m && def < m->defs.len
        ? ir_places_access_at(m->places, load_access) : nullptr;
    const ir_value_t *index = nullptr;
    uint32_t base = load ? load_base(m, load, &index) : 0;
    const ir_memdef_t *store = load ? &m->defs.data[def].def : nullptr;
    if (!store || store->kind != IR_MEMDEF_STORE || !base
        || store->element_base != base || !store->element_index)
        return IR_ELEMENT_MAYBE;
    uint32_t written = ir_value_number(m->numbers, store->element_index);
    uint32_t read = ir_value_number(m->numbers, index);
    if (written && written == read) return IR_ELEMENT_SAME;
    ir_int_t a = 0, b = 0;
    if (ir_value_number_constant(m->numbers, written, &a)
        && ir_value_number_constant(m->numbers, read, &b) && a != b)
        return IR_ELEMENT_DISJOINT;
    return IR_ELEMENT_MAYBE;
}

static void
link_element_load(ir_memssa_t *m, uint32_t load_access)
{
    const ir_access_t *load = ir_places_access_at(m->places, load_access);
    const ir_value_t *index = nullptr;
    uint32_t base = load_base(m, load, &index);
    m->link_first[load_access] = (uint32_t)m->links.len;
    if (!base || m->use_def[load_access] == UINT32_MAX) return;
    core_list_t(uint32_t) pending = core_list_new(uint32_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    uint32_t start = element_reaching(m, m->use_def[load_access], base, load);
    fill_element_phis(m, load);
    core_list_push(pending, start);
    while (pending.len) {
        uint32_t def = pending.data[--pending.len];
        if (def == UINT32_MAX
            || !core_dict_add(&seen, (void *)(uintptr_t)(def + 1),
                              (void *)(uintptr_t)(def + 1)))
            continue;
        const memdef_node_t node = m->defs.data[def];
        uint32_t next = UINT32_MAX;
        switch (node.def.kind) {
        case IR_MEMDEF_ENTRY:
            core_list_push(m->links, def);
            break;
        case IR_MEMDEF_ELEMENT_PHI:
            for (uint32_t i = 0; i < node.incoming_count; i++)
                core_list_push(pending,
                               m->incoming.data[node.first_incoming + i]);
            break;
        case IR_MEMDEF_PHI:
            core_list_push(pending, element_phi(m, def, base));
            fill_element_phis(m, load);
            break;
        case IR_MEMDEF_CALL:
            if (call_clobbers(m, node.def.instr, load))
                core_list_push(m->links, def);
            next = node.prev;
            break;
        case IR_MEMDEF_STORE: {
            ir_element_relation_t relation =
                ir_memssa_element_relation(m, def, load_access);
            if (relation != IR_ELEMENT_DISJOINT)
                core_list_push(m->links, def);
            if (relation != IR_ELEMENT_SAME
                && !covers(m->places, node.def.access, load))
                next = node.prev;
            break;
        }
        }
        if (next == UINT32_MAX) continue;
        core_list_push(pending, element_reaching(m, next, base, load));
        fill_element_phis(m, load);
    }
    m->link_count[load_access] = (uint32_t)m->links.len
        - m->link_first[load_access];
    core_dict_free(&seen);
    core_list_free(pending);
}

ir_memssa_t *
ir_memssa_build(ir_func_t *f)
{
    ir_doms_t   *doms   = ir_func_doms(f);
    ir_places_t *places = ir_func_places(f);
    ir_block_t  *entry  = f ? ir_block_entry(f) : nullptr;
    if (!doms || !places || !entry) return nullptr;
    ir_memssa_t *m = core_alloc(ir_memssa_t);
    m->f        = f;
    m->doms     = doms;
    m->places   = places;
    m->numbers  = ir_value_numbers_build(f, nullptr);
    m->defs     = core_list_new(memdef_node_t);
    m->incoming = core_list_new(uint32_t);
    m->current  = core_list_new(uint32_t);
    m->pending_phis = core_list_new(uint32_t);
    m->links    = core_list_new(uint32_t);
    uint32_t accesses = ir_places_access_count(places);
    m->use_def = core_alloc_array(uint32_t, accesses ? accesses : 1);
    m->link_first = core_alloc_array(uint32_t, accesses ? accesses : 1);
    m->link_count = core_alloc_array(uint32_t, accesses ? accesses : 1);
    m->phi_of   = core_alloc_array(uint32_t, f->n_blocks);
    m->block_in = core_alloc_array(uint32_t, f->n_blocks);
    core_dict_init(&m->last_def, nullptr, nullptr);
    core_dict_init(&m->element_phis, nullptr, nullptr);
    core_dict_init(&m->call_writes, nullptr, nullptr);
    uint32_t *cursor = core_alloc_array(uint32_t, f->n_blocks);
    uint32_t *out    = core_alloc_array(uint32_t, f->n_blocks);
    for (uint32_t a = 0; a < accesses; a++) m->use_def[a] = UINT32_MAX;

    uint32_t position = 0;
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        cursor[block->index] = position;
        for (uint32_t i = 0; i < block->n_instrs; i++)
            while (position < accesses
                   && ir_places_access_at(places, position)->instr
                          == block->instrs[i])
                position++;
    }

    uint32_t start = add_def(m, (ir_memdef_t){.kind = IR_MEMDEF_ENTRY,
                                              .block = entry},
                             UINT32_MAX);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        m->phi_of[block->index]   = UINT32_MAX;
        m->block_in[block->index] = UINT32_MAX;
        out[block->index]         = UINT32_MAX;
        if (block != entry && ir_block_reachable(doms, block)
            && ir_pred_count(doms, block) >= 2)
            m->phi_of[block->index] = add_def(m, (ir_memdef_t){
                .kind = IR_MEMDEF_PHI, .block = block}, UINT32_MAX);
    }

    core_list_push(m->current, start);
    rename_t r = {.m = m, .block_cursor = cursor, .block_out = out};
    ir_dom_walk(doms, entry, rename_pre, rename_post, &r);

    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        uint32_t    phi   = m->phi_of[block->index];
        if (phi == UINT32_MAX) continue;
        uint32_t count = ir_pred_count(doms, block);
        m->defs.data[phi].first_incoming = (uint32_t)m->incoming.len;
        m->defs.data[phi].incoming_count = count;
        for (uint32_t p = 0; p < count; p++) {
            ir_block_t *pred = ir_pred_at(doms, block, p);
            uint32_t    def  = pred && ir_block_reachable(doms, pred)
                ? out[pred->index] : UINT32_MAX;
            core_list_push(m->incoming, def == UINT32_MAX ? start : def);
        }
    }
    core_free(cursor);
    core_free(out);
    for (uint32_t a = 0; a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(places, a);
        if (access->kind == IR_ACCESS_LOAD) link_element_load(m, a);
    }
    return m;
}

void
ir_memssa_free(ir_memssa_t *m)
{
    if (!m) return;
    ir_value_numbers_free(m->numbers);
    core_list_free(m->defs);
    core_list_free(m->incoming);
    core_list_free(m->current);
    core_list_free(m->pending_phis);
    core_list_free(m->links);
    core_free(m->use_def);
    core_free(m->link_first);
    core_free(m->link_count);
    core_free(m->phi_of);
    core_free(m->block_in);
    core_dict_free(&m->last_def);
    core_dict_free(&m->element_phis);
    core_dict_free(&m->call_writes);
    core_free(m);
}

uint32_t
ir_memssa_def_before(const ir_memssa_t *m, const ir_instr_t *in)
{
    ir_block_t *block = m && in ? in->block : nullptr;
    if (!block || block->index >= m->f->n_blocks) return UINT32_MAX;
    uint32_t def = m->block_in[block->index];
    for (uint32_t i = 0; def != UINT32_MAX && i < block->n_instrs; i++) {
        if (block->instrs[i] == in) return def;
        uintptr_t last = (uintptr_t)core_dict_get(
            (core_dict_t *)&m->last_def, block->instrs[i], nullptr);
        if (last) def = (uint32_t)(last - 1);
    }
    return UINT32_MAX;
}

uint32_t
ir_memssa_def_count(const ir_memssa_t *m)
{
    return m ? (uint32_t)m->defs.len : 0;
}

uint32_t
ir_memssa_load_def(const ir_memssa_t *m, uint32_t load_access)
{
    return m && load_access < ir_places_access_count(m->places)
        ? m->use_def[load_access] : UINT32_MAX;
}

const ir_memdef_t *
ir_memssa_def(const ir_memssa_t *m, uint32_t id)
{
    return m && id < m->defs.len ? &m->defs.data[id].def : nullptr;
}

uint32_t
ir_memssa_def_prev(const ir_memssa_t *m, uint32_t id)
{
    return m && id < m->defs.len ? m->defs.data[id].prev : UINT32_MAX;
}

uint32_t
ir_memssa_phi_count(const ir_memssa_t *m, uint32_t id)
{
    return m && id < m->defs.len ? m->defs.data[id].incoming_count : 0;
}

uint32_t
ir_memssa_phi_incoming(const ir_memssa_t *m, uint32_t id, uint32_t index)
{
    return index < ir_memssa_phi_count(m, id)
        ? m->incoming.data[m->defs.data[id].first_incoming + index]
        : UINT32_MAX;
}

uint32_t
ir_memssa_access_of(const ir_memssa_t *m, const ir_instr_t *in,
                    ir_access_kind_t kind)
{
    uint32_t first = 0;
    uint32_t count = m && in ? ir_places_accesses_of(m->places, in, &first)
                             : 0;
    for (uint32_t a = first; a < first + count; a++)
        if (ir_places_access_at(m->places, a)->kind == kind) return a;
    return UINT32_MAX;
}

uint32_t
ir_memssa_element_link_count(const ir_memssa_t *m, uint32_t load_access)
{
    return m && load_access < ir_places_access_count(m->places)
        ? m->link_count[load_access] : 0;
}

uint32_t
ir_memssa_element_link(const ir_memssa_t *m, uint32_t load_access,
                       uint32_t index)
{
    return index < ir_memssa_element_link_count(m, load_access)
        ? m->links.data[m->link_first[load_access] + index] : UINT32_MAX;
}

bool
ir_memssa_visit_clobbers(ir_memssa_t *m, uint32_t load_access,
                         ir_memdef_visit_fn visit, void *context)
{
    const ir_access_t *load = m ? ir_places_access_at(m->places, load_access)
                                : nullptr;
    if (!load || load->kind != IR_ACCESS_LOAD
        || m->use_def[load_access] == UINT32_MAX) return false;
    core_list_t(uint32_t) pending = core_list_new(uint32_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(pending, m->use_def[load_access]);
    bool ok = true;
    while (ok && pending.len) {
        uint32_t index = pending.data[--pending.len];
        if (!core_dict_add(&seen, (void *)(uintptr_t)(index + 1),
                           (void *)(uintptr_t)(index + 1)))
            continue;
        const memdef_node_t *node = &m->defs.data[index];
        switch (node->def.kind) {
        case IR_MEMDEF_ENTRY:
            ok = visit(context, &node->def);
            break;
        case IR_MEMDEF_PHI:
            for (uint32_t i = 0; i < node->incoming_count; i++)
                core_list_push(pending,
                               m->incoming.data[node->first_incoming + i]);
            break;
        case IR_MEMDEF_CALL:
            if (call_clobbers(m, node->def.instr, load))
                ok = visit(context, &node->def);
            core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_STORE:
            if (places_overlap(m->places, node->def.access, load)) {
                ok = visit(context, &node->def);
                if (covers(m->places, node->def.access, load)) break;
            }
            core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_ELEMENT_PHI:
            break;
        }
    }
    core_dict_free(&seen);
    core_list_free(pending);
    return ok;
}

bool
ir_memssa_visit_element_defs(ir_memssa_t *m, uint32_t load_access,
                             ir_element_relation_fn relate,
                             ir_memdef_visit_fn visit, void *context)
{
    const ir_access_t *load = m ? ir_places_access_at(m->places, load_access)
                                : nullptr;
    if (!load || load->kind != IR_ACCESS_LOAD || !relate
        || m->use_def[load_access] == UINT32_MAX) return false;
    core_list_t(uint32_t) pending = core_list_new(uint32_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(pending, m->use_def[load_access]);
    bool ok = true;
    while (ok && pending.len) {
        uint32_t index = pending.data[--pending.len];
        if (!core_dict_add(&seen, (void *)(uintptr_t)(index + 1),
                           (void *)(uintptr_t)(index + 1)))
            continue;
        const memdef_node_t *node = &m->defs.data[index];
        switch (node->def.kind) {
        case IR_MEMDEF_ENTRY:
            ok = visit(context, &node->def);
            break;
        case IR_MEMDEF_PHI:
            for (uint32_t i = 0; i < node->incoming_count; i++)
                core_list_push(pending,
                               m->incoming.data[node->first_incoming + i]);
            break;
        case IR_MEMDEF_CALL:
            if (call_clobbers(m, node->def.instr, load))
                ok = visit(context, &node->def);
            core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_STORE: {
            if (!places_overlap(m->places, node->def.access, load)) {
                core_list_push(pending, node->prev);
                break;
            }
            ir_element_relation_t relation = relate(context, node->def.instr,
                                                    load->instr);
            if (relation == IR_ELEMENT_DISJOINT) {
                core_list_push(pending, node->prev);
                break;
            }
            ok = visit(context, &node->def);
            if (relation != IR_ELEMENT_SAME
                && !covers(m->places, node->def.access, load))
                core_list_push(pending, node->prev);
            break;
        }
        case IR_MEMDEF_ELEMENT_PHI:
            break;
        }
    }
    core_dict_free(&seen);
    core_list_free(pending);
    return ok;
}

bool
ir_memssa_reads_entry(ir_memssa_t *m, uint32_t load_access)
{
    const ir_access_t *load = m ? ir_places_access_at(m->places, load_access)
                                : nullptr;
    if (!load || load->kind != IR_ACCESS_LOAD
        || m->use_def[load_access] == UINT32_MAX) return false;
    bool *seen = core_alloc_array(bool, m->defs.len);
    core_list_t(uint32_t) pending = core_list_new(uint32_t);
    core_list_push(pending, m->use_def[load_access]);
    bool entry = false;
    while (!entry && pending.len) {
        uint32_t index = pending.data[--pending.len];
        if (seen[index]) continue;
        seen[index] = true;
        const memdef_node_t *node = &m->defs.data[index];
        switch (node->def.kind) {
        case IR_MEMDEF_ENTRY:
            entry = true;
            break;
        case IR_MEMDEF_PHI:
            for (uint32_t i = 0; i < node->incoming_count; i++)
                core_list_push(pending,
                               m->incoming.data[node->first_incoming + i]);
            break;
        case IR_MEMDEF_CALL:
            core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_STORE:
            if (!covers(m->places, node->def.access, load))
                core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_ELEMENT_PHI:
            break;
        }
    }
    core_list_free(pending);
    core_free(seen);
    return entry;
}

static bool
def_dominates(ir_memssa_t *m, uint32_t def, ir_block_t *block)
{
    const ir_memdef_t *d = &m->defs.data[def].def;
    return d->kind == IR_MEMDEF_ENTRY
        || (d->block && ir_dominates(m->doms, d->block, block));
}

static bool
verify_phi(ir_memssa_t *m, uint32_t id, uint32_t expected)
{
    const memdef_node_t *node = &m->defs.data[id];
    if (node->incoming_count != expected) return false;
    for (uint32_t i = 0; i < node->incoming_count; i++) {
        uint32_t from = m->incoming.data[node->first_incoming + i];
        ir_block_t *pred = ir_pred_at(m->doms, node->def.block, i);
        if (from >= m->defs.len
            || (pred && ir_block_reachable(m->doms, pred)
                && !def_dominates(m, from, pred)))
            return false;
    }
    return true;
}

bool
ir_memssa_verify(ir_memssa_t *m)
{
    if (!m) return false;
    for (uint32_t id = 0; id < m->defs.len; id++) {
        const memdef_node_t *node = &m->defs.data[id];
        switch (node->def.kind) {
        case IR_MEMDEF_ENTRY:
            if (id != 0) return false;
            break;
        case IR_MEMDEF_STORE:
        case IR_MEMDEF_CALL:
            if (node->prev >= m->defs.len
                || !def_dominates(m, node->prev, node->def.block))
                return false;
            if (node->def.kind == IR_MEMDEF_STORE && node->def.element_index) {
                const ir_value_t *base = nullptr, *index = nullptr;
                if (!ir_memssa_element_of(node->def.instr, &base, &index)
                    || index != node->def.element_index
                    || ir_value_number(m->numbers, base)
                        != node->def.element_base)
                    return false;
            }
            break;
        case IR_MEMDEF_PHI:
            if (!verify_phi(m, id, ir_pred_count(m->doms, node->def.block)))
                return false;
            break;
        case IR_MEMDEF_ELEMENT_PHI: {
            uint32_t phi = node->def.memory_phi;
            if (phi >= m->defs.len
                || m->defs.data[phi].def.kind != IR_MEMDEF_PHI
                || m->defs.data[phi].def.block != node->def.block
                || !verify_phi(m, id, m->defs.data[phi].incoming_count))
                return false;
            break;
        }
        }
    }
    uint32_t accesses = ir_places_access_count(m->places);
    for (uint32_t a = 0; a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(m->places, a);
        if (access->kind != IR_ACCESS_LOAD || m->use_def[a] == UINT32_MAX)
            continue;
        if (m->use_def[a] != ir_memssa_def_before(m, access->instr))
            return false;
        for (uint32_t l = 0; l < m->link_count[a]; l++) {
            uint32_t def = m->links.data[m->link_first[a] + l];
            if (def >= m->defs.len) return false;
            const memdef_node_t *node = &m->defs.data[def];
            if (node->def.kind == IR_MEMDEF_PHI
                || node->def.kind == IR_MEMDEF_ELEMENT_PHI
                || (node->def.kind == IR_MEMDEF_STORE
                    && (!places_overlap(m->places, node->def.access, access)
                        || ir_memssa_element_relation(m, def, a)
                               == IR_ELEMENT_DISJOINT)))
                return false;
        }
    }
    return true;
}
