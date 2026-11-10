#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/pdg_memory.h"
#include "ir/pdg.h"
#include "ir/places.h"
#include "ir/doms.h"
#include "ir/walk.h"
#include "lib/dict.h"

#define NO_LOOP UINT32_MAX

typedef struct {
    uint32_t at;
    uint32_t on;
    uint32_t slot;
} ir_cdp_t;

struct ir_pdg_t {
    core_arena_t   arena;
    ir_func_t     *f;
    ir_doms_t     *dom;
    ir_instr_t   **nodes;
    uint32_t       n_nodes;
    uint32_t      *base;
    ir_dep_edge_t *edges;
    uint32_t       n_edges;
    size_t         cap_edges;
    ir_cdp_t      *cdp;
    uint32_t       n_cdp;
    size_t         cap_cdp;
    uint32_t      *out_off;
    uint32_t      *out_at;
    uint32_t      *in_off;
    uint32_t      *in_at;
    uint32_t      *cd_off;
    uint32_t      *cd_at;
    uint32_t      *cd_slot;
    uint64_t      *slot_stamp;
    uint32_t      *loop_of;
    uint32_t      *loop_parent;
    ir_rd_t       *rd;
    ir_pdg_memory_t *memory;
    uint32_t      *stamp;
    bool           exact;
    bool           pending;
};

static bool arena_grow(core_arena_t *arena, void **data, size_t *cap,
                       size_t need, size_t elem)
{
    return need <= *cap
        || core_arena_try_reserve_raw(arena, data, cap, need < 8 ? 8 : need,
                                      elem);
}

static bool pdg_grow(ir_pdg_t *g, void **data, size_t *cap, size_t need,
                     size_t elem)
{
    return arena_grow(&g->arena, data, cap, need, elem);
}

void ir_pdg_free(ir_pdg_t *g)
{
    if (!g) {
        return;
    }
    ir_pdg_memory_free(g->memory);
    core_arena_free(&g->arena);
    core_free(g);
}

core_string_t ir_edge_kind_name(ir_edge_kind_t k)
{
    switch (k) {
    case IR_EDGE_BRANCH_TRUE:
        return CORE_STRING_STATIC("branch-true");
    case IR_EDGE_BRANCH_FALSE:
        return CORE_STRING_STATIC("branch-false");
    case IR_EDGE_LOOP_BACK:
        return CORE_STRING_STATIC("loop-back");
    case IR_EDGE_LOOP_EXIT:
        return CORE_STRING_STATIC("loop-exit");
    case IR_EDGE_JUMP:
        return CORE_STRING_STATIC("jump");
    case IR_EDGE_CASE:
        return CORE_STRING_STATIC("case");
    default:
        return CORE_STRING_STATIC("fallthrough");
    }
}

static int by_rpo_desc(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return x < y ? 1 : x > y ? -1 : 0;
}

static void claim(ir_pdg_t *g, uint32_t b, uint32_t h)
{
    if (g->loop_of[b] == NO_LOOP) {
        g->loop_of[b] = h;
        return;
    }

    uint32_t a = g->loop_of[b];

    while (g->loop_parent[a] != NO_LOOP) {
        a = g->loop_parent[a];
    }

    if (a != h) {
        g->loop_parent[a] = h;
    }
}

static void mark_loops(ir_pdg_t *g)
{
    ir_func_t    *f    = g->f;
    core_arena_t *ctx  = &g->arena;
    uint32_t      n    = f->n_blocks;
    uint64_t    *hdrs  = core_arena_alloc(ctx, n * sizeof(uint64_t));
    uint32_t    *stamp = core_arena_alloc(ctx, n * sizeof(uint32_t));
    ir_block_t **work  = core_arena_alloc(ctx, n * sizeof(ir_block_t *));
    uint32_t     n_h   = 0;

    g->loop_of     = core_arena_alloc(ctx, n * sizeof(uint32_t));
    g->loop_parent = core_arena_alloc(ctx, n * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        g->loop_of[i]     = NO_LOOP;
        g->loop_parent[i] = NO_LOOP;
        stamp[i]          = NO_LOOP;
    }

    for (uint32_t i = 0; i < n; i++) {
        ir_block_t *h = f->blocks[i];

        for (uint32_t k = 0; k < ir_pred_count(g->dom, h); k++) {
            if (ir_dominates(g->dom, h, ir_pred_at(g->dom, h, k))) {
                hdrs[n_h++] = ((uint64_t)ir_rpo_index(g->dom, h) << 32)
                            | h->index;
                break;
            }
        }
    }

    qsort(hdrs, n_h, sizeof(uint64_t), by_rpo_desc);

    for (uint32_t i = 0; i < n_h; i++) {
        uint32_t    hi = (uint32_t)(hdrs[i] & 0xffffffffu);
        ir_block_t *h  = f->blocks[hi];
        uint32_t    nw = 0;

        stamp[hi] = hi;
        claim(g, hi, hi);

        for (uint32_t k = 0; k < ir_pred_count(g->dom, h); k++) {
            ir_block_t *tail = ir_pred_at(g->dom, h, k);

            if (ir_dominates(g->dom, h, tail) && stamp[tail->index] != hi) {
                stamp[tail->index] = hi;
                work[nw++]         = tail;
            }
        }

        while (nw) {
            ir_block_t *b = work[--nw];

            claim(g, b->index, hi);

            for (uint32_t p = 0; p < ir_pred_count(g->dom, b); p++) {
                ir_block_t *pb = ir_pred_at(g->dom, b, p);

                if (stamp[pb->index] != hi) {
                    stamp[pb->index] = hi;
                    work[nw++]       = pb;
                }
            }
        }
    }
}

static bool in_loop(const ir_pdg_t *g, ir_block_t *b, uint32_t hdr)
{
    for (uint32_t c = g->loop_of[b->index]; c != NO_LOOP;
         c = g->loop_parent[c]) {
        if (c == hdr) {
            return true;
        }
    }

    return false;
}

ir_edge_kind_t ir_edge_kind(const ir_pdg_t *g, ir_block_t *from, uint32_t slot)
{
    ir_instr_t *t = from ? ir_block_terminator(from) : nullptr;

    if (!g || !t || slot >= t->n_operands) {
        return IR_EDGE_JUMP;
    }

    ir_operand_t *o  = &t->operands[slot];
    ir_block_t   *to = ir_succ_of(o);

    if (!to) {
        return IR_EDGE_JUMP;
    }

    if (ir_dominates(g->dom, to, from)) {
        return IR_EDGE_LOOP_BACK;
    }

    if (o->kind == IR_OPD_SWITCH_ARM || o->kind == IR_OPD_DEFAULT_BLOCK) {
        return IR_EDGE_CASE;
    }

    uint32_t mine = g->loop_of[from->index];

    if (mine != NO_LOOP && !in_loop(g, to, mine)) {
        return IR_EDGE_LOOP_EXIT;
    }

    if (t->op == IR_OP_BRANCH) {
        return slot == 1 ? IR_EDGE_BRANCH_TRUE : IR_EDGE_BRANCH_FALSE;
    }

    return from->index + 1 < g->f->n_blocks
                   && g->f->blocks[from->index + 1] == to
             ? IR_EDGE_FALLTHROUGH
             : IR_EDGE_JUMP;
}

static uint32_t node_of(const ir_pdg_t *g, const ir_instr_t *in)
{
    return in && in->block ? g->base[in->block->index] + in->index
                           : UINT32_MAX;
}

static uint32_t param_node(const ir_pdg_t *g, const ir_value_t *param)
{
    return param && param->kind == IR_VAL_PARAM && param->owner == g->f
                   && param->index < g->f->n_params
             ? g->n_nodes + param->index
             : UINT32_MAX;
}

static uint32_t from_node(const ir_pdg_t *g, const ir_dep_edge_t *e)
{
    return e->from ? node_of(g, e->from) : param_node(g, e->param);
}

static uint32_t to_node(const ir_pdg_t *g, const ir_dep_edge_t *e)
{
    return e->to ? node_of(g, e->to) : param_node(g, e->param);
}

static void push_edge(ir_pdg_t *g, ir_dep_edge_t edge)
{
    if (pdg_grow(g, (void **)&g->edges, &g->cap_edges, g->n_edges + 1u,
                 sizeof(ir_dep_edge_t))) {
        g->edges[g->n_edges++] = edge;
    }
}

static void add_edge(ir_pdg_t *g, ir_instr_t *from, ir_instr_t *to,
                     ir_dep_kind_t kind, ir_dep_certainty_t cert,
                     ir_local_t *via)
{
    if (!from || !to || !from->block || !to->block) {
        return;
    }
    push_edge(g, (ir_dep_edge_t){.from      = from,
                                 .to        = to,
                                 .kind      = kind,
                                 .certainty = cert,
                                 .via       = via});
}

static void add_param_edge(ir_pdg_t *g, ir_value_t *param, ir_instr_t *to)
{
    if (param_node(g, param) == UINT32_MAX || !to || !to->block) {
        return;
    }
    push_edge(g, (ir_dep_edge_t){.to        = to,
                                 .kind      = IR_DEP_DATA,
                                 .certainty = IR_DEP_EXACT,
                                 .param     = param});
}

static bool number_block(ir_visit_t *vi)
{
    ir_pdg_t *g = vi->user;

    g->base[vi->block->index] = g->n_nodes;
    g->n_nodes += vi->block->n_instrs;

    return false;
}

static bool collect_node(ir_visit_t *vi)
{
    ir_pdg_t *g = vi->user;

    g->nodes[g->n_nodes++] = vi->instr;

    return false;
}

static bool control_block(ir_visit_t *vi)
{
    ir_pdg_t   *g = vi->user;
    ir_block_t *a = vi->block;
    ir_instr_t *t = ir_block_terminator(a);

    if (!t || !ir_block_reachable(g->dom, a)) {
        return false;
    }

    ir_block_t *stop = ir_ipdom_of(g->dom, a);

    for (uint32_t s = 0; s < t->n_operands; s++) {
        ir_block_t *at = ir_succ_of(&t->operands[s]);

        uint64_t key = ((uint64_t)a->index << 32) | s;

        for (; at && at != stop; at = ir_ipdom_of(g->dom, at)) {
            if (g->slot_stamp[at->index] == key) {
                break;
            }
            g->slot_stamp[at->index] = key;

            if (!pdg_grow(g, (void **)&g->cdp, &g->cap_cdp, g->n_cdp + 1u,
                          sizeof(ir_cdp_t))) {
                return false;
            }
            g->cdp[g->n_cdp++] = (ir_cdp_t){.at = at->index, .on = a->index,
                                            .slot = s};

            if (g->stamp[at->index] == a->index) {
                continue;
            }
            g->stamp[at->index] = a->index;

            for (uint32_t k = 0; k < at->n_instrs; k++) {
                add_edge(g, t, at->instrs[k], IR_DEP_CONTROL, IR_DEP_EXACT,
                         nullptr);
            }
        }
    }

    return false;
}

static void control_edges(ir_pdg_t *g)
{
    ir_func_t    *f     = g->f;
    core_arena_t *ctx   = &g->arena;
    uint32_t     *stamp = core_arena_alloc(ctx, f->n_blocks * sizeof(uint32_t));

    uint64_t  *slot_stamp = core_arena_alloc(ctx, f->n_blocks * sizeof(uint64_t));

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        stamp[i]      = UINT32_MAX;
        slot_stamp[i] = UINT64_MAX;
    }

    g->stamp      = stamp;
    g->slot_stamp = slot_stamp;

    ir_walk_t *w = ir_walk_new(g);

    ir_walk_on_block(w, control_block, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);
}

static bool value_operand(ir_visit_t *vi)
{
    ir_pdg_t   *g   = vi->user;
    ir_value_t *val = ir_operand_value(vi->operand);

    if (val && val->kind == IR_VAL_INSTR && val->def) {
        add_edge(g, val->def, vi->instr, IR_DEP_DATA, IR_DEP_EXACT, nullptr);
    }
    else if (val && val->kind == IR_VAL_PARAM) {
        add_param_edge(g, val, vi->instr);
    }

    return true;
}

static bool local_operand(ir_visit_t *vi)
{
    ir_pdg_t   *g = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (!g->rd || !l || ir_local_read_earlier(vi->instr, vi->index, l)) {
        return true;
    }

    uint32_t n = ir_rd_count(g->rd, vi->instr, l);
    bool     x = n == 1 && !ir_rd_maybe_unset(g->rd, vi->instr, l);

    for (uint32_t d = 0; d < n; d++) {
        add_edge(g, ir_rd_at(g->rd, vi->instr, l, d), vi->instr, IR_DEP_DATA,
                 x ? IR_DEP_EXACT : IR_DEP_MAY, l);
    }
    g->exact = g->exact && x;

    return true;
}

static void data_edges(ir_pdg_t *g)
{
    if (g->f->n_locals) {
        g->rd = ir_rd_of(g->f, g->dom);
    }

    ir_walk_t *w = ir_walk_new(g);

    ir_walk_on_operand(w, value_operand, nullptr);
    ir_walk_on_operand(w, local_operand, nullptr);
    ir_walk_run_func(w, g->f);
    ir_walk_free(w);
}

static bool memory_call(const ir_instr_t *in)
{
    return in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE
        || in->op == IR_OP_BUFFER_AS_STRING
        || in->op == IR_OP_BUFFER_SLICE_END;
}

typedef struct {
    ir_pdg_t   *g;
    ir_instr_t *to;
    uint32_t    first;
} memory_sink_t;

static bool memory_source(void *context, ir_instr_t *from, bool exact)
{
    memory_sink_t *sink = context;

    for (uint32_t e = sink->first; e < sink->g->n_edges; e++) {
        if (sink->g->edges[e].from == from
            && sink->g->edges[e].kind == IR_DEP_MEMORY) {
            return true;
        }
    }
    add_edge(sink->g, from, sink->to, IR_DEP_MEMORY,
             exact ? IR_DEP_EXACT : IR_DEP_MAY, nullptr);

    return true;
}

static void memory_edges(ir_pdg_t *g)
{
    ir_places_t *places = ir_func_places(g->f);

    g->memory = places ? ir_pdg_memory_build(g->f, g->dom) : nullptr;
    if (!g->memory) {
        return;
    }
    for (uint32_t b = 0; b < g->f->n_blocks; b++) {
        ir_block_t *block = g->f->blocks[b];

        if (!ir_block_reachable(g->dom, block)) {
            continue;
        }
        for (uint32_t k = 0; k < block->n_instrs; k++) {
            ir_instr_t   *in   = block->instrs[k];
            memory_sink_t sink = {.g = g, .to = in, .first = g->n_edges};

            if (memory_call(in)) {
                ir_pdg_memory_call_sources(g->memory, in, memory_source,
                                           &sink);
                continue;
            }

            uint32_t first = 0;
            uint32_t n     = ir_places_accesses_of(places, in, &first);

            for (uint32_t a = 0; a < n; a++) {
                if (ir_places_access_at(places, first + a)->kind
                    == IR_ACCESS_LOAD) {
                    ir_pdg_memory_load_sources(g->memory, first + a,
                                               memory_source, &sink);
                }
            }
        }
    }
}

static void index_edges(ir_pdg_t *g)
{
    core_arena_t *ctx = &g->arena;
    uint32_t      n   = g->n_nodes + g->f->n_params;

    g->out_off = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    g->in_off  = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_off[from_node(g, &g->edges[i]) + 1]++;
        g->in_off[to_node(g, &g->edges[i]) + 1]++;
    }

    for (uint32_t i = 0; i < n; i++) {
        g->out_off[i + 1] += g->out_off[i];
        g->in_off[i + 1] += g->in_off[i];
    }

    uint32_t *fo = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    uint32_t *fi = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    memcpy(fo, g->out_off, (n + 1) * sizeof(uint32_t));
    memcpy(fi, g->in_off, (n + 1) * sizeof(uint32_t));
    g->out_at = core_arena_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                  * sizeof(uint32_t));
    g->in_at  = core_arena_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                 * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_at[fo[from_node(g, &g->edges[i])]++] = i;
        g->in_at[fi[to_node(g, &g->edges[i])]++]    = i;
    }
}

static void index_controllers(ir_pdg_t *g)
{
    core_arena_t *ctx = &g->arena;
    uint32_t      nb  = g->f->n_blocks;

    g->cd_off = core_arena_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_cdp; i++) {
        g->cd_off[g->cdp[i].at + 1]++;
    }

    for (uint32_t i = 0; i < nb; i++) {
        g->cd_off[i + 1] += g->cd_off[i];
    }

    uint32_t *fc = core_arena_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    memcpy(fc, g->cd_off, (nb + 1) * sizeof(uint32_t));
    g->cd_at = core_arena_alloc(ctx, (g->n_cdp ? g->n_cdp : 1) * sizeof(uint32_t));
    g->cd_slot = core_arena_alloc(ctx, (g->n_cdp ? g->n_cdp : 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_cdp; i++) {
        uint32_t at = fc[g->cdp[i].at]++;

        g->cd_at[at]   = g->cdp[i].on;
        g->cd_slot[at] = g->cdp[i].slot;
    }
}

ir_pdg_t *ir_pdg_with_doms(ir_func_t *f, ir_doms_t *dom)
{
    if (!f || !f->n_blocks || !dom) {
        return nullptr;
    }

    ir_pdg_t *g = core_alloc(ir_pdg_t);

    core_arena_init(&g->arena, 16 * 1024);

    core_arena_t *ctx = &g->arena;

    g->f     = f;
    g->dom   = dom;
    g->base  = core_arena_alloc(ctx, (f->n_blocks + 1) * sizeof(uint32_t));
    g->exact = true;

    mark_loops(g);

    ir_walk_t *w = ir_walk_new(g);

    ir_walk_on_block(w, number_block, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    g->nodes = core_arena_alloc(ctx, (g->n_nodes ? g->n_nodes : 1)
                                 * sizeof(ir_instr_t *));
    g->n_nodes = 0;
    w          = ir_walk_new(g);

    ir_walk_on_instr(w, collect_node, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    control_edges(g);
    index_controllers(g);
    data_edges(g);
    g->pending = true;

    return g;
}

void ir_pdg_complete(ir_pdg_t *g)
{
    if (g && g->pending) {
        g->pending = false;
        memory_edges(g);
        index_edges(g);
    }
}

uint32_t ir_pdg_node_count(const ir_pdg_t *g)
{
    return g ? g->n_nodes : 0;
}

ir_instr_t *ir_pdg_node_at(const ir_pdg_t *g, uint32_t i)
{
    return g && i < g->n_nodes ? g->nodes[i] : nullptr;
}

uint32_t ir_pdg_edge_count(const ir_pdg_t *g)
{
    return g ? g->n_edges : 0;
}

const ir_dep_edge_t *ir_pdg_edge_at(const ir_pdg_t *g, uint32_t i)
{
    return g && i < g->n_edges ? &g->edges[i] : nullptr;
}

uint32_t ir_pdg_out_count(const ir_pdg_t *g, ir_instr_t *in)
{
    uint32_t at = g ? node_of(g, in) : UINT32_MAX;

    return at < g->n_nodes ? g->out_off[at + 1] - g->out_off[at] : 0;
}

const ir_dep_edge_t *ir_pdg_out_at(const ir_pdg_t *g, ir_instr_t *in,
                                   uint32_t i)
{
    if (i >= ir_pdg_out_count(g, in)) {
        return nullptr;
    }

    return &g->edges[g->out_at[g->out_off[node_of(g, in)] + i]];
}

uint32_t ir_pdg_in_count(const ir_pdg_t *g, ir_instr_t *in)
{
    uint32_t at = g ? node_of(g, in) : UINT32_MAX;

    return at < g->n_nodes ? g->in_off[at + 1] - g->in_off[at] : 0;
}

const ir_dep_edge_t *ir_pdg_in_at(const ir_pdg_t *g, ir_instr_t *in,
                                  uint32_t i)
{
    if (i >= ir_pdg_in_count(g, in)) {
        return nullptr;
    }

    return &g->edges[g->in_at[g->in_off[node_of(g, in)] + i]];
}

uint32_t ir_pdg_param_out_count(const ir_pdg_t *g, const ir_value_t *param)
{
    uint32_t at = g ? param_node(g, param) : UINT32_MAX;

    return at != UINT32_MAX ? g->out_off[at + 1] - g->out_off[at] : 0;
}

const ir_dep_edge_t *ir_pdg_param_out_at(const ir_pdg_t *g,
                                         const ir_value_t *param, uint32_t i)
{
    if (i >= ir_pdg_param_out_count(g, param)) {
        return nullptr;
    }

    return &g->edges[g->out_at[g->out_off[param_node(g, param)] + i]];
}

uint32_t ir_controllers_of(const ir_pdg_t *g, ir_block_t *b)
{
    if (!g || !b || b->index >= g->f->n_blocks) {
        return 0;
    }

    return g->cd_off[b->index + 1] - g->cd_off[b->index];
}

ir_block_t *ir_controller_at(const ir_pdg_t *g, ir_block_t *b, uint32_t i)
{
    if (i >= ir_controllers_of(g, b)) {
        return nullptr;
    }

    return g->f->blocks[g->cd_at[g->cd_off[b->index] + i]];
}

uint32_t ir_controller_slot_at(const ir_pdg_t *g, ir_block_t *b, uint32_t i)
{
    if (i >= ir_controllers_of(g, b)) {
        return UINT32_MAX;
    }

    return g->cd_slot[g->cd_off[b->index] + i];
}

bool ir_pdg_is_exact(const ir_pdg_t *g)
{
    return g && g->exact;
}

ir_pdg_memory_t *ir_pdg_memory(const ir_pdg_t *g)
{
    return g ? g->memory : nullptr;
}

ir_rd_t *ir_pdg_rd(const ir_pdg_t *g)
{
    return g ? g->rd : nullptr;
}

uint32_t ir_pdg_local_version(const ir_pdg_t *g, const ir_instr_t *read,
                              const ir_local_t *local)
{
    if (!g || !g->rd || !local || local->address_taken) {
        return 0;
    }

    uint32_t version = ir_rd_use_tok(g->rd, (ir_instr_t *)read,
                                     (ir_local_t *)local);

    return version == UINT32_MAX ? 0 : version;
}

ir_doms_t *ir_pdg_doms(const ir_pdg_t *g)
{
    return g ? g->dom : nullptr;
}

struct ir_group_pdg_t {
    core_arena_t   arena;
    ir_func_t    **functions;
    ir_pdg_t     **graphs;
    uint32_t       n_functions;
    uint32_t      *offset;
    core_dict_t    index;
    ir_dep_edge_t *edges;
    uint32_t       n_edges;
    size_t         cap_edges;
    uint32_t      *out_off;
    uint32_t      *out_at;
    uint32_t      *in_off;
    uint32_t      *in_at;
    uint32_t      *exit_off;
    ir_instr_t   **exits;
    uint64_t      *calls;
    uint32_t       n_calls;
    size_t         cap_calls;
    uint32_t      *calls_off;
    uint32_t      *calls_at;
    uint32_t      *component;
    uint32_t      *members;
    uint32_t      *component_off;
    bool          *recursive;
    uint32_t       n_components;
};

typedef struct {
    ir_group_pdg_t *g;
    ir_func_t     **items;
    uint32_t        n;
    size_t          cap;
} target_sink_t;

static uint32_t group_index(const ir_group_pdg_t *g, const ir_func_t *f)
{
    bool  found = false;
    void *value = f ? core_dict_get((core_dict_t *)&g->index, f, &found)
                    : nullptr;

    return found ? (uint32_t)((uintptr_t)value - 1) : UINT32_MAX;
}

static uint32_t group_node(const ir_group_pdg_t *g, const ir_instr_t *in,
                           const ir_value_t *param)
{
    const ir_func_t *f = in ? (in->block ? in->block->owner : nullptr)
                            : param ? param->owner : nullptr;
    uint32_t         i = group_index(g, f);

    if (i == UINT32_MAX || !g->graphs[i]) {
        return UINT32_MAX;
    }

    uint32_t local = in ? node_of(g->graphs[i], in)
                        : param_node(g->graphs[i], param);

    return local == UINT32_MAX ? UINT32_MAX : g->offset[i] + local;
}

static uint32_t group_from(const ir_group_pdg_t *g, const ir_dep_edge_t *e)
{
    return e->from ? group_node(g, e->from, nullptr)
                   : group_node(g, nullptr, e->param);
}

static uint32_t group_to(const ir_group_pdg_t *g, const ir_dep_edge_t *e)
{
    return e->to ? group_node(g, e->to, nullptr)
                 : group_node(g, nullptr, e->param);
}

static void group_push(ir_group_pdg_t *g, ir_dep_edge_t edge)
{
    if (arena_grow(&g->arena, (void **)&g->edges, &g->cap_edges,
                   g->n_edges + 1u, sizeof(ir_dep_edge_t))) {
        g->edges[g->n_edges++] = edge;
    }
}

static void collect_target(void *sink, ir_func_t *target)
{
    target_sink_t *s = sink;

    if (!target) {
        return;
    }
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->items[i] == target) {
            return;
        }
    }
    if (arena_grow(&s->g->arena, (void **)&s->items, &s->cap, s->n + 1u,
                   sizeof(ir_func_t *))) {
        s->items[s->n++] = target;
    }
}

static bool direct_targets(const ir_instr_t *call, target_sink_t *sink)
{
    ir_value_t *callee = call->n_operands
                           ? ir_operand_value(&call->operands[0])
                           : nullptr;

    if (!callee || callee->kind != IR_VAL_FUNC || !callee->owner) {
        return false;
    }
    collect_target(sink, callee->owner);

    return true;
}

static void collect_exits(ir_group_pdg_t *g)
{
    core_arena_t *ctx   = &g->arena;
    uint32_t      total = 0;

    g->exit_off = core_arena_alloc(ctx, (g->n_functions + 1)
                                            * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_functions; i++) {
        ir_pdg_t *p = g->graphs[i];

        for (uint32_t k = 0; p && k < p->n_nodes; k++) {
            total += p->nodes[k]->op == IR_OP_RETURN
                  || p->nodes[k]->op == IR_OP_FAIL;
        }
    }

    g->exits = core_arena_alloc(ctx, (total ? total : 1)
                                         * sizeof(ir_instr_t *));
    total    = 0;

    for (uint32_t i = 0; i < g->n_functions; i++) {
        ir_pdg_t *p = g->graphs[i];

        g->exit_off[i] = total;
        for (uint32_t k = 0; p && k < p->n_nodes; k++) {
            ir_instr_t *in = p->nodes[k];

            if (in->op == IR_OP_RETURN || in->op == IR_OP_FAIL) {
                g->exits[total++] = in;
            }
        }
    }
    g->exit_off[g->n_functions] = total;
}

static ir_value_t *rest_value(const ir_func_t *f)
{
    for (uint32_t p = 0; p < f->n_params; p++) {
        if (f->params[p]->is_rest) {
            return f->params[p];
        }
    }

    return nullptr;
}

ir_value_t *ir_call_param(ir_func_t *target, const ir_operand_t *o,
                          uint32_t *position, uint32_t *type_index)
{
    *type_index = UINT32_MAX;
    if (o->kind == IR_OPD_NAMED_ARG) {
        for (uint32_t p = 0; p < target->n_params; p++) {
            if (target->params[p]->is_keyword
                && core_string_eq(target->params[p]->name, o->named.name)) {
                return o->named.value ? target->params[p] : nullptr;
            }
        }
        return nullptr;
    }
    if (o->kind != IR_OPD_VALUE && o->kind != IR_OPD_ARG
        && o->kind != IR_OPD_SPREAD && o->kind != IR_OPD_OUT
        && o->kind != IR_OPD_FRESH && o->kind != IR_OPD_MISSING) {
        return nullptr;
    }
    if (*position >= th_func_nparams(target->type)) {
        return o->kind == IR_OPD_MISSING ? nullptr : rest_value(target);
    }

    uint32_t index = (*position)++;

    *type_index = index;

    return o->kind == IR_OPD_MISSING ? nullptr
                                     : ir_func_param_value(target, index);
}

static void link_call(ir_group_pdg_t *g, uint32_t caller, ir_instr_t *call,
                      ir_func_t *target, ir_dep_certainty_t cert)
{
    uint32_t callee = group_index(g, target);

    if (callee == UINT32_MAX || !g->graphs[callee]) {
        return;
    }
    if (arena_grow(&g->arena, (void **)&g->calls, &g->cap_calls,
                   g->n_calls + 1u, sizeof(uint64_t))) {
        g->calls[g->n_calls++] = ((uint64_t)caller << 32) | callee;
    }

    uint32_t position = 0;

    for (uint32_t o = 1; o < call->n_operands; o++) {
        uint32_t    index = UINT32_MAX;
        ir_value_t *param = ir_call_param(target, &call->operands[o],
                                          &position, &index);

        if (param) {
            group_push(g, (ir_dep_edge_t){.from      = call,
                                          .kind      = IR_DEP_ARGUMENT,
                                          .certainty = cert,
                                          .param     = param});
        }
    }

    for (uint32_t k = g->exit_off[callee]; k < g->exit_off[callee + 1]; k++) {
        ir_instr_t *exit = g->exits[k];

        group_push(g, (ir_dep_edge_t){.from      = exit,
                                      .to        = call,
                                      .kind      = exit->op == IR_OP_RETURN
                                                     ? IR_DEP_RETURN
                                                     : IR_DEP_FAILURE,
                                      .certainty = cert});
    }
}

static void link_calls(ir_group_pdg_t *g, const ir_pdg_hooks_t *hooks)
{
    target_sink_t sink = {.g = g};

    for (uint32_t i = 0; i < g->n_functions; i++) {
        ir_pdg_t *p = g->graphs[i];

        for (uint32_t k = 0; p && k < p->n_nodes; k++) {
            ir_instr_t *call = p->nodes[k];

            if (call->op != IR_OP_CALL) {
                continue;
            }
            sink.n        = 0;
            bool complete = hooks && hooks->call_targets
                              ? hooks->call_targets(hooks->context, call,
                                                    collect_target, &sink)
                              : direct_targets(call, &sink);
            ir_dep_certainty_t cert = complete && sink.n == 1 ? IR_DEP_EXACT
                                                              : IR_DEP_MAY;

            for (uint32_t t = 0; t < sink.n; t++) {
                link_call(g, i, call, sink.items[t], cert);
            }
        }
    }
}

static void index_group_edges(ir_group_pdg_t *g)
{
    core_arena_t *ctx = &g->arena;
    uint32_t      n   = g->offset[g->n_functions];

    g->out_off = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    g->in_off  = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_off[group_from(g, &g->edges[i]) + 1]++;
        g->in_off[group_to(g, &g->edges[i]) + 1]++;
    }
    for (uint32_t i = 0; i < n; i++) {
        g->out_off[i + 1] += g->out_off[i];
        g->in_off[i + 1] += g->in_off[i];
    }

    uint32_t *fo = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    uint32_t *fi = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    memcpy(fo, g->out_off, (n + 1) * sizeof(uint32_t));
    memcpy(fi, g->in_off, (n + 1) * sizeof(uint32_t));
    g->out_at = core_arena_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                          * sizeof(uint32_t));
    g->in_at  = core_arena_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                          * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_at[fo[group_from(g, &g->edges[i])]++] = i;
        g->in_at[fi[group_to(g, &g->edges[i])]++]    = i;
    }
}

static void index_calls(ir_group_pdg_t *g)
{
    core_arena_t *ctx = &g->arena;
    uint32_t      n   = g->n_functions;

    g->calls_off = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    g->calls_at  = core_arena_alloc(ctx, (g->n_calls ? g->n_calls : 1)
                                             * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_calls; i++) {
        g->calls_off[(g->calls[i] >> 32) + 1]++;
    }
    for (uint32_t i = 0; i < n; i++) {
        g->calls_off[i + 1] += g->calls_off[i];
    }

    uint32_t *fill = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    memcpy(fill, g->calls_off, (n + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < g->n_calls; i++) {
        g->calls_at[fill[g->calls[i] >> 32]++] =
            (uint32_t)(g->calls[i] & 0xffffffffu);
    }
}

typedef struct {
    uint32_t node;
    uint32_t next;
} scc_frame_t;

static void close_component(ir_group_pdg_t *g, uint32_t root,
                            uint32_t *stack, uint32_t *depth, bool *on_stack)
{
    uint32_t c     = g->n_components++;
    uint32_t start = g->component_off[c];
    uint32_t count = start;
    uint32_t w;

    do {
        w = stack[--*depth];
        on_stack[w]           = false;
        g->component[w]       = c;
        g->members[count++]   = w;
    } while (w != root);

    g->component_off[c + 1] = count;
    g->recursive[c]         = count - start > 1;

    for (uint32_t k = g->calls_off[root]; k < g->calls_off[root + 1]; k++) {
        if (g->calls_at[k] == root) {
            g->recursive[c] = true;
        }
    }
}

static void find_components(ir_group_pdg_t *g)
{
    core_arena_t *ctx    = &g->arena;
    uint32_t      n      = g->n_functions;
    size_t        words  = (n ? n : 1) * sizeof(uint32_t);
    uint32_t     *order  = core_arena_alloc(ctx, words);
    uint32_t     *low    = core_arena_alloc(ctx, words);
    uint32_t     *stack  = core_arena_alloc(ctx, words);
    bool         *on_stk = core_arena_alloc(ctx, (n ? n : 1) * sizeof(bool));
    scc_frame_t  *frames = core_arena_alloc(ctx, (n ? n : 1)
                                                     * sizeof(scc_frame_t));
    uint32_t      seen   = 0;
    uint32_t      depth  = 0;

    g->component     = core_arena_alloc(ctx, words);
    g->members       = core_arena_alloc(ctx, words);
    g->component_off = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));
    g->recursive     = core_arena_alloc(ctx, (n ? n : 1) * sizeof(bool));

    for (uint32_t i = 0; i < n; i++) {
        order[i]        = UINT32_MAX;
        g->component[i] = UINT32_MAX;
    }

    for (uint32_t r = 0; r < n; r++) {
        if (!g->graphs[r] || order[r] != UINT32_MAX) {
            continue;
        }

        uint32_t top = 0;

        frames[top++] = (scc_frame_t){.node = r, .next = g->calls_off[r]};
        order[r] = low[r] = seen++;
        stack[depth++]    = r;
        on_stk[r]         = true;

        while (top) {
            scc_frame_t *f = &frames[top - 1];
            uint32_t     v = f->node;

            if (f->next < g->calls_off[v + 1]) {
                uint32_t w = g->calls_at[f->next++];

                if (order[w] == UINT32_MAX) {
                    frames[top++] = (scc_frame_t){.node = w,
                                                  .next = g->calls_off[w]};
                    order[w] = low[w] = seen++;
                    stack[depth++]    = w;
                    on_stk[w]         = true;
                }
                else if (on_stk[w] && order[w] < low[v]) {
                    low[v] = order[w];
                }
                continue;
            }

            if (low[v] == order[v]) {
                close_component(g, v, stack, &depth, on_stk);
            }
            top--;
            if (top && low[v] < low[frames[top - 1].node]) {
                low[frames[top - 1].node] = low[v];
            }
        }
    }
}

static ir_instr_t *entry_instr(const ir_group_pdg_t *g, ir_func_t *f)
{
    uint32_t    i     = group_index(g, f);
    ir_block_t *entry = i != UINT32_MAX && g->graphs[i] ? ir_block_entry(f)
                                                        : nullptr;

    return entry && entry->n_instrs ? entry->instrs[0] : nullptr;
}

static void link_comptime(ir_group_pdg_t *g, ir_func_t *root)
{
    uint32_t    i     = group_index(g, root);
    ir_instr_t *start = entry_instr(g, root);

    for (uint32_t k = start ? g->exit_off[i] : 0;
         start && k < g->exit_off[i + 1]; k++) {
        if (g->exits[k]->op == IR_OP_RETURN) {
            group_push(g, (ir_dep_edge_t){.from      = g->exits[k],
                                          .to        = start,
                                          .kind      = IR_DEP_CONTROL,
                                          .certainty = IR_DEP_EXACT});
        }
    }
}

ir_group_pdg_t *ir_group_pdg_of(ir_func_t *const *functions, size_t count,
                                const ir_pdg_hooks_t *hooks)
{
    if ((!functions && count) || count >= UINT32_MAX) {
        return nullptr;
    }

    ir_group_pdg_t *g = core_alloc(ir_group_pdg_t);

    core_arena_init(&g->arena, 16 * 1024);
    core_dict_init(&g->index, nullptr, nullptr);

    core_arena_t *ctx = &g->arena;
    uint32_t      n   = (uint32_t)count;

    g->n_functions = n;
    g->functions   = core_arena_alloc(ctx, (n ? n : 1) * sizeof(ir_func_t *));
    g->graphs      = core_arena_alloc(ctx, (n ? n : 1) * sizeof(ir_pdg_t *));
    g->offset      = core_arena_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        ir_func_t *f = functions[i];

        g->functions[i] = f;
        g->graphs[i]    = f && f->n_blocks ? ir_func_pdg(f) : nullptr;
        g->offset[i + 1] = g->offset[i]
                         + (g->graphs[i] ? g->graphs[i]->n_nodes
                                               + f->n_params
                                         : 0);
        if (f) {
            core_dict_add(&g->index, f, (uintptr_t)i + 1);
        }
    }

    collect_exits(g);
    link_calls(g, hooks);
    index_calls(g);
    if (hooks && hooks->comptime_root) {
        link_comptime(g, hooks->comptime_root);
    }
    index_group_edges(g);
    find_components(g);

    return g;
}

void ir_group_pdg_free(ir_group_pdg_t *g)
{
    if (!g) {
        return;
    }
    core_dict_free(&g->index);
    core_arena_free(&g->arena);
    core_free(g);
}

ir_pdg_t *ir_group_pdg_func(const ir_group_pdg_t *g, const ir_func_t *f)
{
    uint32_t i = g ? group_index(g, f) : UINT32_MAX;

    return i == UINT32_MAX ? nullptr : g->graphs[i];
}

uint32_t ir_group_pdg_edge_count(const ir_group_pdg_t *g)
{
    return g ? g->n_edges : 0;
}

const ir_dep_edge_t *ir_group_pdg_edge_at(const ir_group_pdg_t *g,
                                          uint32_t i)
{
    return g && i < g->n_edges ? &g->edges[i] : nullptr;
}

static uint32_t group_out_count(const ir_group_pdg_t *g, uint32_t at)
{
    return at != UINT32_MAX ? g->out_off[at + 1] - g->out_off[at] : 0;
}

static uint32_t group_in_count(const ir_group_pdg_t *g, uint32_t at)
{
    return at != UINT32_MAX ? g->in_off[at + 1] - g->in_off[at] : 0;
}

uint32_t ir_group_pdg_out_count(const ir_group_pdg_t *g, const ir_instr_t *in)
{
    return g ? group_out_count(g, group_node(g, in, nullptr)) : 0;
}

const ir_dep_edge_t *ir_group_pdg_out_at(const ir_group_pdg_t *g,
                                         const ir_instr_t *in, uint32_t i)
{
    if (i >= ir_group_pdg_out_count(g, in)) {
        return nullptr;
    }

    return &g->edges[g->out_at[g->out_off[group_node(g, in, nullptr)] + i]];
}

uint32_t ir_group_pdg_in_count(const ir_group_pdg_t *g, const ir_instr_t *in)
{
    return g ? group_in_count(g, group_node(g, in, nullptr)) : 0;
}

const ir_dep_edge_t *ir_group_pdg_in_at(const ir_group_pdg_t *g,
                                        const ir_instr_t *in, uint32_t i)
{
    if (i >= ir_group_pdg_in_count(g, in)) {
        return nullptr;
    }

    return &g->edges[g->in_at[g->in_off[group_node(g, in, nullptr)] + i]];
}

uint32_t ir_group_pdg_param_in_count(const ir_group_pdg_t *g,
                                     const ir_value_t *param)
{
    return g ? group_in_count(g, group_node(g, nullptr, param)) : 0;
}

const ir_dep_edge_t *ir_group_pdg_param_in_at(const ir_group_pdg_t *g,
                                              const ir_value_t *param,
                                              uint32_t i)
{
    if (i >= ir_group_pdg_param_in_count(g, param)) {
        return nullptr;
    }

    return &g->edges[g->in_at[g->in_off[group_node(g, nullptr, param)] + i]];
}

uint32_t ir_group_pdg_callee_count(const ir_group_pdg_t *g,
                                   const ir_func_t *f)
{
    uint32_t i = g ? group_index(g, f) : UINT32_MAX;

    return i == UINT32_MAX ? 0 : g->calls_off[i + 1] - g->calls_off[i];
}

ir_func_t *ir_group_pdg_callee_at(const ir_group_pdg_t *g,
                                  const ir_func_t *f, uint32_t i)
{
    if (i >= ir_group_pdg_callee_count(g, f)) {
        return nullptr;
    }

    return g->functions[g->calls_at[g->calls_off[group_index(g, f)] + i]];
}

uint32_t ir_group_pdg_component_count(const ir_group_pdg_t *g)
{
    return g ? g->n_components : 0;
}

uint32_t ir_group_pdg_component_size(const ir_group_pdg_t *g, uint32_t c)
{
    return g && c < g->n_components
             ? g->component_off[c + 1] - g->component_off[c]
             : 0;
}

ir_func_t *ir_group_pdg_component_at(const ir_group_pdg_t *g, uint32_t c,
                                     uint32_t i)
{
    if (i >= ir_group_pdg_component_size(g, c)) {
        return nullptr;
    }

    return g->functions[g->members[g->component_off[c] + i]];
}

bool ir_group_pdg_component_recursive(const ir_group_pdg_t *g, uint32_t c)
{
    return g && c < g->n_components && g->recursive[c];
}

uint32_t ir_group_pdg_component_of(const ir_group_pdg_t *g,
                                   const ir_func_t *f)
{
    uint32_t i = g ? group_index(g, f) : UINT32_MAX;

    return i == UINT32_MAX ? UINT32_MAX : g->component[i];
}

bool ir_pdg_reads_comptime(const ir_instr_t *in)
{
    const ir_value_t *v = in && in->op == IR_OP_LOAD && in->n_operands
                            ? ir_operand_value(&in->operands[0])
                            : nullptr;

    return v && v->kind == IR_VAL_GLOBAL && v->global
        && core_string_eq(v->global->name,
                          CORE_STRING_STATIC(IR_COMPTIME_GLOBAL));
}

bool ir_pdg_asks_frozen(const ir_instr_t *in)
{
    const ir_value_t *callee = in && in->op == IR_OP_CALL && in->n_operands
                                 ? ir_operand_value(&in->operands[0])
                                 : nullptr;

    return callee && callee->kind == IR_VAL_FUNC && callee->owner
        && core_string_eq(callee->owner->name,
                          CORE_STRING_STATIC(IR_COMPTIME_FROZEN));
}
