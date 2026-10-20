#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/memssa.h"
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
    core_list_t(memdef_node_t) defs;
    core_list_t(uint32_t)      incoming;
    uint32_t                  *use_def;
    uint32_t                  *phi_of;
    uint32_t                  *block_in;
    core_dict_t                last_def;
    core_list_t(uint32_t)      current;
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
        for (uint32_t a = first; a < end; a++)
            if (ir_places_access_at(m->places, a)->kind == IR_ACCESS_LOAD)
                m->use_def[a] = *def;
        if (call_like(in))
            *def = add_def(m, (ir_memdef_t){.kind = IR_MEMDEF_CALL,
                                            .instr = in, .block = block},
                           *def);
        for (uint32_t a = first; a < end; a++) {
            const ir_access_t *access = ir_places_access_at(m->places, a);
            if (access->kind == IR_ACCESS_STORE)
                *def = add_def(m, (ir_memdef_t){.kind = IR_MEMDEF_STORE,
                                                .instr = in, .access = access,
                                                .block = block},
                               *def);
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
    m->defs     = core_list_new(memdef_node_t);
    m->incoming = core_list_new(uint32_t);
    m->current  = core_list_new(uint32_t);
    uint32_t accesses = ir_places_access_count(places);
    m->use_def = core_alloc_array(uint32_t, accesses ? accesses : 1);
    m->phi_of   = core_alloc_array(uint32_t, f->n_blocks);
    m->block_in = core_alloc_array(uint32_t, f->n_blocks);
    core_dict_init(&m->last_def, nullptr, nullptr);
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
    return m;
}

void
ir_memssa_free(ir_memssa_t *m)
{
    if (!m) return;
    core_list_free(m->defs);
    core_list_free(m->incoming);
    core_list_free(m->current);
    core_free(m->use_def);
    core_free(m->phi_of);
    core_free(m->block_in);
    core_dict_free(&m->last_def);
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
    for (uint32_t i = 0; i < access->count; i++)
        if (ir_place_at(places, ids[i])->kind != IR_PLACE_LOCAL) return false;
    return true;
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

bool
ir_memssa_visit_clobbers(ir_memssa_t *m, uint32_t load_access,
                         ir_memdef_visit_fn visit, void *context)
{
    const ir_access_t *load = m ? ir_places_access_at(m->places, load_access)
                                : nullptr;
    if (!load || load->kind != IR_ACCESS_LOAD
        || m->use_def[load_access] == UINT32_MAX) return false;
    bool local = all_private(m->places, load);
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
            if (!local) ok = visit(context, &node->def);
            core_list_push(pending, node->prev);
            break;
        case IR_MEMDEF_STORE:
            if (places_overlap(m->places, node->def.access, load)) {
                ok = visit(context, &node->def);
                if (covers(m->places, node->def.access, load)) break;
            }
            core_list_push(pending, node->prev);
            break;
        }
    }
    core_dict_free(&seen);
    core_list_free(pending);
    return ok;
}
