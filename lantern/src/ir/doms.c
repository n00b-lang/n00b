#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/doms.h"

struct ir_doms_t {
    ir_func_t   *f;
    uint32_t     n;
    ir_block_t **rpo;
    uint32_t     n_rpo;
    uint32_t    *order;
    int32_t     *idom;
    uint32_t    *pred_off;
    uint32_t    *pred_at;
    uint32_t    *succ_off;
    uint32_t    *succ_at;
    uint32_t    *tin;
    uint32_t    *tout;
    int32_t     *ipdom;
    uint32_t    *df_off;
    uint32_t    *df_at;
    uint32_t    *kid_off;
    uint32_t    *kid_at;
};

ir_block_t *ir_succ_of(const ir_operand_t *o)
{
    if (!o) {
        return nullptr;
    }

    switch (o->kind) {
    case IR_OPD_BLOCK:
    case IR_OPD_DEFAULT_BLOCK:
        return o->block;
    case IR_OPD_SWITCH_ARM:
        return o->arm.block;
    default:
        return nullptr;
    }
}

uint32_t ir_succ_count(ir_instr_t *t)
{
    uint32_t n = 0;

    for (uint32_t i = 0; t && i < t->n_operands; i++) {
        ir_opdkind_t k = t->operands[i].kind;

        if (k == IR_OPD_BLOCK || k == IR_OPD_DEFAULT_BLOCK
            || k == IR_OPD_SWITCH_ARM) {
            n++;
        }
    }

    return n;
}

ir_block_t *ir_succ_at(ir_instr_t *t, uint32_t want)
{
    uint32_t n = 0;

    for (uint32_t i = 0; t && i < t->n_operands; i++) {
        ir_operand_t *o = &t->operands[i];
        ir_block_t   *b = o->kind == IR_OPD_BLOCK         ? o->block
                        : o->kind == IR_OPD_DEFAULT_BLOCK ? o->block
                        : o->kind == IR_OPD_SWITCH_ARM    ? o->arm.block
                                                          : nullptr;

        if (b && n++ == want) {
            return b;
        }
    }

    return nullptr;
}

static void build_preds(ir_doms_t *d, ir_func_t *f)
{
    ir_ctx_t *ctx   = f->owner->ctx;
    uint32_t  edges = 0;

    d->pred_off = ir_alloc(ctx, (d->n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_instr_t *t = ir_block_terminator(f->blocks[i]);

        for (uint32_t k = 0; t && k < t->n_operands; k++) {
            ir_block_t *to = ir_succ_of(&t->operands[k]);

            if (to) {
                edges++;
                d->pred_off[to->index + 1]++;
            }
        }
    }

    for (uint32_t i = 0; i < d->n; i++) {
        d->pred_off[i + 1] += d->pred_off[i];
    }

    uint32_t *fill = ir_alloc(ctx, (d->n + 1) * sizeof(uint32_t));

    memcpy(fill, d->pred_off, (d->n + 1) * sizeof(uint32_t));
    d->pred_at = ir_alloc(ctx, (edges ? edges : 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_instr_t *t = ir_block_terminator(f->blocks[i]);

        for (uint32_t k = 0; t && k < t->n_operands; k++) {
            ir_block_t *to = ir_succ_of(&t->operands[k]);

            if (to) {
                d->pred_at[fill[to->index]++] = f->blocks[i]->index;
            }
        }
    }
}

static void build_rpo(ir_doms_t *d, ir_func_t *f)
{
    ir_ctx_t    *ctx   = f->owner->ctx;
    ir_block_t **stack = ir_alloc(ctx, d->n * sizeof(ir_block_t *));
    uint32_t    *next  = ir_alloc(ctx, d->n * sizeof(uint32_t));
    bool        *seen  = ir_alloc(ctx, d->n * sizeof(bool));
    uint32_t     top   = 0;
    uint32_t     post  = 0;
    ir_block_t  *entry = ir_block_entry(f);

    if (!entry) {
        return;
    }
    stack[top]        = entry;
    next[top]         = 0;
    seen[entry->index] = true;

    while (true) {
        ir_block_t *b = stack[top];
        ir_instr_t *t = ir_block_terminator(b);

        while (next[top] < (t ? t->n_operands : 0)
               && !ir_succ_of(&t->operands[next[top]])) {
            next[top]++;
        }

        if (t && next[top] < t->n_operands) {
            ir_block_t *to = ir_succ_of(&t->operands[next[top]++]);

            if (!seen[to->index]) {
                seen[to->index] = true;
                stack[++top]    = to;
                next[top]       = 0;
            }
            continue;
        }
        d->rpo[post++] = b;

        if (!top) {
            break;
        }
        top--;
    }

    for (uint32_t i = 0; i < post / 2; i++) {
        ir_block_t *t        = d->rpo[i];
        d->rpo[i]            = d->rpo[post - 1 - i];
        d->rpo[post - 1 - i] = t;
    }
    d->n_rpo = post;

    for (uint32_t i = 0; i < post; i++) {
        d->order[d->rpo[i]->index] = i;
    }
}

typedef struct {
    uint32_t        n;
    uint32_t        root;
    const uint32_t *succ_off;
    const uint32_t *succ_at;
    const uint32_t *pred_off;
    const uint32_t *pred_at;
} lt_graph;

typedef struct {
    uint32_t *dfn;
    uint32_t *vertex;
    uint32_t *parent;
    uint32_t *semi;
    uint32_t *ancestor;
    uint32_t *label;
    uint32_t *idom;
    uint32_t *bucket;
    uint32_t *next;
    uint32_t *path;
    uint32_t  n;
} lt_t;

static uint32_t lt_eval(lt_t *lt, uint32_t v)
{
    if (!lt->ancestor[v]) {
        return lt->label[v];
    }

    uint32_t n = 0;
    uint32_t x = v;

    while (lt->ancestor[lt->ancestor[x]]) {
        lt->path[n++] = x;
        x             = lt->ancestor[x];
    }

    for (uint32_t i = n; i-- > 0;) {
        uint32_t y = lt->path[i];
        uint32_t a = lt->ancestor[y];

        if (lt->semi[lt->label[a]] < lt->semi[lt->label[y]]) {
            lt->label[y] = lt->label[a];
        }
        lt->ancestor[y] = lt->ancestor[a];
    }

    return lt->label[v];
}

static uint32_t lt_dfs(const lt_graph *g, lt_t *lt, uint32_t *stack,
                       uint32_t *next)
{
    uint32_t top = 0;
    uint32_t at  = 0;

    lt->dfn[g->root] = ++at;
    lt->vertex[at]   = g->root;
    lt->parent[at]   = 0;
    stack[0]         = g->root;
    next[0]          = g->succ_off[g->root];

    while (true) {
        uint32_t b    = stack[top];
        bool     down = false;

        while (next[top] < g->succ_off[b + 1]) {
            uint32_t to = g->succ_at[next[top]++];

            if (lt->dfn[to]) {
                continue;
            }

            lt->dfn[to]  = ++at;
            lt->vertex[at] = to;
            lt->parent[at] = lt->dfn[b];
            stack[++top]   = to;
            next[top]      = g->succ_off[to];
            down           = true;
            break;
        }

        if (down) {
            continue;
        }

        if (!top) {
            break;
        }
        top--;
    }

    return at;
}

static void lt_run(ir_ctx_t *ctx, const lt_graph *g, int32_t *out)
{
    uint32_t n  = g->n;
    lt_t     lt = {0};

    for (uint32_t i = 0; i < n; i++) {
        out[i] = -1;
    }

    lt.dfn      = ir_alloc(ctx, n * sizeof(uint32_t));
    lt.vertex   = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.parent   = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.semi     = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.ancestor = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.label    = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.idom     = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.bucket   = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.next     = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    lt.path     = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    uint32_t *stack = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    uint32_t *step  = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));
    uint32_t  last  = lt_dfs(g, &lt, stack, step);

    lt.n = last;

    for (uint32_t i = 1; i <= last; i++) {
        lt.semi[i]  = i;
        lt.label[i] = i;
    }

    for (uint32_t i = last; i >= 2; i--) {
        uint32_t w = lt.vertex[i];

        for (uint32_t k = g->pred_off[w]; k < g->pred_off[w + 1]; k++) {
            uint32_t p = lt.dfn[g->pred_at[k]];

            if (!p) {
                continue;
            }

            uint32_t u = lt_eval(&lt, p);

            if (lt.semi[u] < lt.semi[i]) {
                lt.semi[i] = lt.semi[u];
            }
        }

        lt.next[i]            = lt.bucket[lt.semi[i]];
        lt.bucket[lt.semi[i]] = i;
        lt.ancestor[i]        = lt.parent[i];

        for (uint32_t v = lt.bucket[lt.parent[i]]; v; v = lt.next[v]) {
            uint32_t u = lt_eval(&lt, v);

            lt.idom[v] = lt.semi[u] < lt.semi[v] ? u : lt.parent[i];
        }
        lt.bucket[lt.parent[i]] = 0;
    }

    for (uint32_t i = 2; i <= last; i++) {
        if (lt.idom[i] != lt.semi[i]) {
            lt.idom[i] = lt.idom[lt.idom[i]];
        }
    }

    if (last) {
        lt.idom[1] = 1;
    }

    for (uint32_t i = 1; i <= last; i++) {
        out[lt.vertex[i]] = (int32_t)lt.vertex[lt.idom[i]];
    }
}

static void build_succs(ir_doms_t *d, ir_func_t *f)
{
    ir_ctx_t *ctx = f->owner->ctx;
    uint32_t  n   = d->n;

    d->succ_off = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        d->succ_off[f->blocks[i]->index + 1]
            = ir_succ_count(ir_block_terminator(f->blocks[i]));
    }

    for (uint32_t i = 0; i < n; i++) {
        d->succ_off[i + 1] += d->succ_off[i];
    }

    d->succ_at = ir_alloc(ctx, (d->succ_off[n] ? d->succ_off[n] : 1)
                                   * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        ir_block_t *b  = f->blocks[i];
        ir_instr_t *t  = ir_block_terminator(b);
        uint32_t    at = d->succ_off[b->index];

        for (uint32_t k = 0; t && k < t->n_operands; k++) {
            ir_block_t *to = ir_succ_of(&t->operands[k]);

            if (to) {
                d->succ_at[at++] = to->index;
            }
        }
    }
}

static void build_idoms(ir_doms_t *d, ir_func_t *f)
{
    lt_graph g = {
        .n        = d->n,
        .root     = ir_block_entry(f)->index,
        .succ_off = d->succ_off,
        .succ_at  = d->succ_at,
        .pred_off = d->pred_off,
        .pred_at  = d->pred_at,
    };

    lt_run(f->owner->ctx, &g, d->idom);
}

static void build_tour(ir_doms_t *d, ir_func_t *f)
{
    ir_ctx_t *ctx      = f->owner->ctx;
    uint32_t *kid_off  = ir_alloc(ctx, (d->n + 2) * sizeof(uint32_t));
    uint32_t *kid_at   = ir_alloc(ctx, (d->n ? d->n : 1) * sizeof(uint32_t));
    uint32_t  root     = d->n_rpo ? d->rpo[0]->index : 0;

    for (uint32_t i = 0; i < d->n_rpo; i++) {
        uint32_t b = d->rpo[i]->index;

        if (b != root && d->idom[b] >= 0) {
            kid_off[(uint32_t)d->idom[b] + 1]++;
        }
    }

    for (uint32_t i = 0; i < d->n; i++) {
        kid_off[i + 1] += kid_off[i];
    }

    uint32_t *fill = ir_alloc(ctx, (d->n + 1) * sizeof(uint32_t));

    memcpy(fill, kid_off, (d->n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < d->n_rpo; i++) {
        uint32_t b = d->rpo[i]->index;

        if (b != root && d->idom[b] >= 0) {
            kid_at[fill[(uint32_t)d->idom[b]]++] = b;
        }
    }

    uint32_t *stack = ir_alloc(ctx, (d->n + 1) * sizeof(uint32_t));
    uint32_t *at    = ir_alloc(ctx, (d->n + 1) * sizeof(uint32_t));
    uint32_t  top   = 0;
    uint32_t  clock = 0;

    if (!d->n_rpo) {
        return;
    }
    stack[0]   = root;
    at[0]      = kid_off[root];
    d->tin[root] = clock++;

    while (true) {
        uint32_t b = stack[top];

        if (at[top] < kid_off[b + 1]) {
            uint32_t kid = kid_at[at[top]++];

            d->tin[kid] = clock++;
            stack[++top] = kid;
            at[top]      = kid_off[kid];
            continue;
        }
        d->tout[b] = clock++;

        if (!top) {
            break;
        }
        top--;
    }
}

static void build_postdoms(ir_doms_t *d, ir_func_t *f)
{
    ir_ctx_t *ctx  = f->owner->ctx;
    uint32_t  n    = d->n;
    uint32_t  exit = n;
    uint32_t  m    = n + 1;
    uint32_t *soff = ir_alloc(ctx, (m + 1) * sizeof(uint32_t));
    uint32_t *poff = ir_alloc(ctx, (m + 1) * sizeof(uint32_t));

    for (uint32_t v = 0; v < n; v++) {
        soff[v + 1] = d->pred_off[v + 1] - d->pred_off[v];
        poff[v + 1] = d->succ_off[v + 1] - d->succ_off[v];

        if (soff[v + 1] == 0 && v == exit) {
            continue;
        }

        if (d->succ_off[v + 1] == d->succ_off[v]) {
            soff[exit + 1]++;
            poff[v + 1]++;
        }
    }

    for (uint32_t v = 0; v < m; v++) {
        soff[v + 1] += soff[v];
        poff[v + 1] += poff[v];
    }

    uint32_t *sat = ir_alloc(ctx, (soff[m] ? soff[m] : 1) * sizeof(uint32_t));
    uint32_t *pat = ir_alloc(ctx, (poff[m] ? poff[m] : 1) * sizeof(uint32_t));
    uint32_t *fill = ir_alloc(ctx, (m + 1) * sizeof(uint32_t));

    for (uint32_t v = 0; v <= m; v++) {
        fill[v] = soff[v];
    }

    for (uint32_t v = 0; v < n; v++) {
        for (uint32_t k = d->pred_off[v]; k < d->pred_off[v + 1]; k++) {
            sat[fill[v]++] = d->pred_at[k];
        }

        if (d->succ_off[v + 1] == d->succ_off[v]) {
            sat[fill[exit]++] = v;
        }
    }

    for (uint32_t v = 0; v <= m; v++) {
        fill[v] = poff[v];
    }

    for (uint32_t v = 0; v < n; v++) {
        for (uint32_t k = d->succ_off[v]; k < d->succ_off[v + 1]; k++) {
            pat[fill[v]++] = d->succ_at[k];
        }

        if (d->succ_off[v + 1] == d->succ_off[v]) {
            pat[fill[v]++] = exit;
        }
    }

    lt_graph g = {
        .n        = m,
        .root     = exit,
        .succ_off = soff,
        .succ_at  = sat,
        .pred_off = poff,
        .pred_at  = pat,
    };

    lt_run(ctx, &g, d->ipdom);
}

ir_doms_t *ir_doms_of(ir_func_t *f)
{
    if (!f || !f->n_blocks) {
        return nullptr;
    }

    ir_ctx_t  *ctx = f->owner->ctx;
    ir_doms_t *d   = ir_alloc(ctx, sizeof(ir_doms_t));

    d->f     = f;
    d->n     = f->n_blocks;
    d->rpo   = ir_alloc(ctx, d->n * sizeof(ir_block_t *));
    d->order = ir_alloc(ctx, d->n * sizeof(uint32_t));
    d->idom  = ir_alloc(ctx, d->n * sizeof(int32_t));
    d->tin   = ir_alloc(ctx, d->n * sizeof(uint32_t));
    d->tout  = ir_alloc(ctx, d->n * sizeof(uint32_t));
    d->ipdom = ir_alloc(ctx, (d->n + 1) * sizeof(int32_t));

    for (uint32_t i = 0; i < d->n; i++) {
        d->order[i] = UINT32_MAX;
        d->idom[i]  = -1;
    }

    build_preds(d, f);
    build_succs(d, f);
    build_rpo(d, f);

    if (!d->n_rpo) {
        return d;
    }
    build_idoms(d, f);
    build_tour(d, f);
    build_postdoms(d, f);

    return d;
}

ir_block_t *ir_idom_of(ir_doms_t *d, ir_block_t *b)
{
    if (!d || !b || d->idom[b->index] < 0
        || (uint32_t)d->idom[b->index] == b->index) {
        return nullptr;
    }

    return d->f->blocks[d->idom[b->index]];
}

uint32_t ir_rpo_index(ir_doms_t *d, ir_block_t *b)
{
    return d && b ? d->order[b->index] : UINT32_MAX;
}

bool ir_block_reachable(ir_doms_t *d, ir_block_t *b)
{
    return ir_rpo_index(d, b) != UINT32_MAX;
}

bool ir_dominates(ir_doms_t *d, ir_block_t *a, ir_block_t *b)
{
    if (!d || !a || !b || !ir_block_reachable(d, a)
        || !ir_block_reachable(d, b)) {
        return false;
    }

    return d->tin[a->index] <= d->tin[b->index]
        && d->tout[b->index] <= d->tout[a->index];
}

uint32_t ir_pred_total(ir_doms_t *d)
{
    return d ? d->pred_off[d->n] : 0;
}

uint32_t ir_pred_count(ir_doms_t *d, ir_block_t *b)
{
    return d && b ? d->pred_off[b->index + 1] - d->pred_off[b->index] : 0;
}

ir_block_t *ir_pred_at(ir_doms_t *d, ir_block_t *b, uint32_t i)
{
    if (!d || !b || i >= ir_pred_count(d, b)) {
        return nullptr;
    }

    return d->f->blocks[d->pred_at[d->pred_off[b->index] + i]];
}

ir_block_t *ir_ipdom_of(ir_doms_t *d, ir_block_t *b)
{
    if (!d || !b || d->ipdom[b->index] < 0
        || (uint32_t)d->ipdom[b->index] >= d->n) {
        return nullptr;
    }

    return d->f->blocks[d->ipdom[b->index]];
}

static void build_frontiers(ir_doms_t *d)
{
    ir_ctx_t *ctx  = d->f->owner->ctx;
    uint32_t  n    = d->n;
    uint32_t *mark = ir_alloc(ctx, n * sizeof(uint32_t));

    d->df_off = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            mark[i] = UINT32_MAX;
        }

        for (uint32_t i = 0; i < n; i++) {
            ir_block_t *b = d->f->blocks[i];

            if (ir_pred_count(d, b) < 2) {
                continue;
            }

            ir_block_t *stop = ir_idom_of(d, b);

            for (uint32_t k = 0; k < ir_pred_count(d, b); k++) {
                for (ir_block_t *at = ir_pred_at(d, b, k); at && at != stop;
                     at = ir_idom_of(d, at)) {
                    if (mark[at->index] == b->index) {
                        break;
                    }
                    mark[at->index] = b->index;

                    if (pass) {
                        d->df_at[d->df_off[at->index]++] = b->index;
                    }
                    else {
                        d->df_off[at->index + 1]++;
                    }
                }
            }
        }

        if (pass) {
            break;
        }

        for (uint32_t i = 0; i < n; i++) {
            d->df_off[i + 1] += d->df_off[i];
        }
        d->df_at = ir_alloc(ctx, (d->df_off[n] ? d->df_off[n] : 1)
                                     * sizeof(uint32_t));
    }

    for (uint32_t i = n; i; i--) {
        d->df_off[i] = d->df_off[i - 1];
    }
    d->df_off[0] = 0;
}

uint32_t ir_df_count(ir_doms_t *d, ir_block_t *b)
{
    if (!d || !b) {
        return 0;
    }

    if (!d->df_off) {
        build_frontiers(d);
    }

    return d->df_off[b->index + 1] - d->df_off[b->index];
}

ir_block_t *ir_df_at(ir_doms_t *d, ir_block_t *b, uint32_t i)
{
    if (i >= ir_df_count(d, b)) {
        return nullptr;
    }

    return d->f->blocks[d->df_at[d->df_off[b->index] + i]];
}

static void build_kids(ir_doms_t *d)
{
    ir_ctx_t *ctx = d->f->owner->ctx;
    uint32_t  n   = d->n;

    d->kid_off = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        ir_block_t *p = ir_idom_of(d, d->f->blocks[i]);

        if (p) {
            d->kid_off[p->index + 1]++;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        d->kid_off[i + 1] += d->kid_off[i];
    }
    d->kid_at = ir_alloc(ctx, (d->kid_off[n] ? d->kid_off[n] : 1)
                                  * sizeof(uint32_t));

    uint32_t *fill = ir_alloc(ctx, (n + 1) * sizeof(uint32_t));

    memcpy(fill, d->kid_off, (n + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        ir_block_t *p = ir_idom_of(d, d->f->blocks[i]);

        if (p) {
            d->kid_at[fill[p->index]++] = i;
        }
    }
}

uint32_t ir_dom_kid_count(ir_doms_t *d, ir_block_t *b)
{
    if (!d || !b) {
        return 0;
    }

    if (!d->kid_off) {
        build_kids(d);
    }

    return d->kid_off[b->index + 1] - d->kid_off[b->index];
}

ir_block_t *ir_dom_kid_at(ir_doms_t *d, ir_block_t *b, uint32_t i)
{
    if (i >= ir_dom_kid_count(d, b)) {
        return nullptr;
    }

    return d->f->blocks[d->kid_at[d->kid_off[b->index] + i]];
}

typedef struct {
    ir_block_t *b;
    uint32_t    next;
    bool        entered;
} dom_frame_t;

void ir_dom_walk(ir_doms_t *d, ir_block_t *from, ir_dom_fn pre,
                 ir_dom_fn post, void *thunk)
{
    if (!d || !from || !ir_block_reachable(d, from)) {
        return;
    }

    uint32_t     cap    = d->n + 1;
    dom_frame_t *stack  = ir_alloc(d->f->owner->ctx,
                                   cap * sizeof(dom_frame_t));
    uint32_t     sn     = 0;

    stack[sn++] = (dom_frame_t){.b = from};

    while (sn) {
        dom_frame_t *f = &stack[sn - 1];

        if (!f->entered) {
            f->entered = true;

            if (pre && !pre(f->b, thunk)) {
                f->next = ir_dom_kid_count(d, f->b);
            }
        }

        if (f->next < ir_dom_kid_count(d, f->b)) {
            ir_block_t *kid = ir_dom_kid_at(d, f->b, f->next++);

            if (sn < cap) {
                stack[sn++] = (dom_frame_t){.b = kid};
            }

            continue;
        }

        if (post) {
            post(f->b, thunk);
        }

        sn--;
    }
}
