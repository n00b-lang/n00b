#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/pdg.h"
#include "ir/doms.h"
#include "ir/walk.h"

#define NO_LOOP UINT32_MAX

typedef struct {
    uint32_t at;
    uint32_t on;
} ir_cdp_t;

struct ir_pdg_t {
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
    uint32_t      *loop_of;
    uint32_t      *loop_parent;
    ir_rd_t       *rd;
    uint32_t      *stamp;
    bool           exact;
};

const char *ir_edge_kind_name(ir_edge_kind_t k)
{
    switch (k) {
    case IR_EDGE_BRANCH_TRUE:
        return "branch-true";
    case IR_EDGE_BRANCH_FALSE:
        return "branch-false";
    case IR_EDGE_LOOP_BACK:
        return "loop-back";
    case IR_EDGE_LOOP_EXIT:
        return "loop-exit";
    case IR_EDGE_JUMP:
        return "jump";
    case IR_EDGE_CASE:
        return "case";
    default:
        return "fallthrough";
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
    ir_func_t   *f     = g->f;
    ir_ctx_t    *ctx   = f->owner->ctx;
    uint32_t     n     = f->n_blocks;
    uint64_t    *hdrs  = ir_alloc(ctx, n * sizeof(uint64_t));
    uint32_t    *stamp = ir_alloc(ctx, n * sizeof(uint32_t));
    ir_block_t **work  = ir_alloc(ctx, n * sizeof(ir_block_t *));
    uint32_t     n_h   = 0;

    g->loop_of     = ir_alloc(ctx, n * sizeof(uint32_t));
    g->loop_parent = ir_alloc(ctx, n * sizeof(uint32_t));

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

static void add_edge(ir_pdg_t *g, ir_instr_t *from, ir_instr_t *to,
                     ir_dep_kind_t kind, ir_dep_certainty_t cert,
                     ir_local_t *via)
{
    if (!from || !to || !from->block || !to->block) {
        return;
    }
    g->edges = ir_grow(g->f->owner->ctx, g->edges, &g->cap_edges,
                       g->n_edges + 1u, sizeof(ir_dep_edge_t));

    if (!g->edges) {
        return;
    }
    g->edges[g->n_edges++] = (ir_dep_edge_t){.from      = from,
                                             .to        = to,
                                             .kind      = kind,
                                             .certainty = cert,
                                             .via       = via};
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
    ir_ctx_t   *ctx = g->f->owner->ctx;
    ir_block_t *a = vi->block;
    ir_instr_t *t = ir_block_terminator(a);

    if (!t || !ir_block_reachable(g->dom, a)) {
        return false;
    }

    ir_block_t *stop = ir_ipdom_of(g->dom, a);

    for (uint32_t s = 0; s < t->n_operands; s++) {
        ir_block_t *at = ir_succ_of(&t->operands[s]);

        for (; at && at != stop; at = ir_ipdom_of(g->dom, at)) {
            if (g->stamp[at->index] == a->index) {
                break;
            }
            g->stamp[at->index] = a->index;

            g->cdp = ir_grow(ctx, g->cdp, &g->cap_cdp, g->n_cdp + 1u,
                             sizeof(ir_cdp_t));

            if (!g->cdp) {
                return false;
            }
            g->cdp[g->n_cdp++] = (ir_cdp_t){.at = at->index, .on = a->index};

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
    ir_func_t *f     = g->f;
    ir_ctx_t  *ctx   = f->owner->ctx;
    uint32_t  *stamp = ir_alloc(ctx, f->n_blocks * sizeof(uint32_t));

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        stamp[i] = UINT32_MAX;
    }

    g->stamp = stamp;

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

static void index_edges(ir_pdg_t *g)
{
    ir_ctx_t *ctx = g->f->owner->ctx;
    uint32_t  n   = g->n_nodes;
    uint32_t  nb  = g->f->n_blocks;

    g->out_off = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    g->in_off  = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_off[node_of(g, g->edges[i].from) + 1]++;
        g->in_off[node_of(g, g->edges[i].to) + 1]++;
    }

    for (uint32_t i = 0; i < n; i++) {
        g->out_off[i + 1] += g->out_off[i];
        g->in_off[i + 1] += g->in_off[i];
    }

    uint32_t *fo = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    uint32_t *fi = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    memcpy(fo, g->out_off, (n + 1) * sizeof(uint32_t));
    memcpy(fi, g->in_off, (n + 1) * sizeof(uint32_t));
    g->out_at = ir_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                  * sizeof(uint32_t));
    g->in_at  = ir_alloc(ctx, (g->n_edges ? g->n_edges : 1)
                                 * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_edges; i++) {
        g->out_at[fo[node_of(g, g->edges[i].from)]++] = i;
        g->in_at[fi[node_of(g, g->edges[i].to)]++]    = i;
    }

    g->cd_off = ir_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_cdp; i++) {
        g->cd_off[g->cdp[i].at + 1]++;
    }

    for (uint32_t i = 0; i < nb; i++) {
        g->cd_off[i + 1] += g->cd_off[i];
    }

    uint32_t *fc = ir_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    memcpy(fc, g->cd_off, (nb + 1) * sizeof(uint32_t));
    g->cd_at = ir_alloc(ctx, (g->n_cdp ? g->n_cdp : 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->n_cdp; i++) {
        g->cd_at[fc[g->cdp[i].at]++] = g->cdp[i].on;
    }
}

ir_pdg_t *ir_pdg_of(ir_func_t *f)
{
    if (!f || !f->n_blocks) {
        return nullptr;
    }

    ir_ctx_t *ctx = f->owner->ctx;
    ir_pdg_t *g   = ir_alloc(ctx, sizeof(ir_pdg_t));

    g->f     = f;
    g->dom   = ir_doms_of(f);
    g->base  = ir_alloc(ctx, (f->n_blocks + 1) * sizeof(uint32_t));
    g->exact = true;

    mark_loops(g);

    ir_walk_t *w = ir_walk_new(g);

    ir_walk_on_block(w, number_block, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    g->nodes = ir_alloc(ctx, (g->n_nodes ? g->n_nodes : 1)
                                 * sizeof(ir_instr_t *));
    g->n_nodes = 0;
    w          = ir_walk_new(g);

    ir_walk_on_instr(w, collect_node, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    control_edges(g);
    data_edges(g);
    index_edges(g);

    return g;
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

bool ir_control_dependent(const ir_pdg_t *g, ir_block_t *b, ir_block_t *on)
{
    for (uint32_t i = 0; i < ir_controllers_of(g, b); i++) {
        if (ir_controller_at(g, b, i) == on) {
            return true;
        }
    }

    return false;
}

bool ir_pdg_is_exact(const ir_pdg_t *g)
{
    return g && g->exact;
}
