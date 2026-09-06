#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/doms.h"
#include "ir/pass.h"

typedef struct {
    ir_module_t             *m;
    ir_func_t               *f;
    ir_doms_t               *dom;
    const ir_promote_opts_t *opts;
    uint32_t                *counter;
    const char             **taken;
    size_t                   taken_cap;
    bool                     ok;
} P;

static size_t name_slot(P *p, const char *k)
{
    size_t h = 1469598103934665603u;

    for (const unsigned char *c = (const unsigned char *)k; *c; c++) {
        h = (h ^ *c) * 1099511628211u;
    }

    size_t i = h & (p->taken_cap - 1);

    while (p->taken[i] && strcmp(p->taken[i], k)) {
        i = (i + 1) & (p->taken_cap - 1);
    }

    return i;
}

static bool name_free(P *p, const char *k)
{
    return !p->taken[name_slot(p, k)];
}

static void name_take(P *p, const char *k)
{
    p->taken[name_slot(p, k)] = k;
}

static void gather_names(P *p, ir_func_t *f)
{
    size_t want = f->n_params + 1;

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        want += f->blocks[i]->n_instrs;
    }
    p->taken_cap = 16;

    while (p->taken_cap < want * 4) {
        p->taken_cap *= 2;
    }
    p->taken = ir_alloc(p->m->ctx, p->taken_cap * sizeof(char *));

    for (uint32_t i = 0; i < f->n_params; i++) {
        name_take(p, f->params[i]->name);
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        for (uint32_t k = 0; k < f->blocks[i]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[i]->instrs[k];

            if (in->dest_kind == IR_DEST_VALUE && in->dest) {
                name_take(p, in->dest->name);
            }
        }
    }
}

static void warn_unset(P *p, const ir_local_t *l)
{
    if (!p->opts || !p->opts->diags) {
        return;
    }

    ir_diag_t *dg = ir_diag_begin_at(p->opts->diags, IR_DIAG_UNSET_LOCAL,
                                     nullptr, ir_block_entry(p->f), p->f,
                                     "this local");

    if (!dg) {
        return;
    }

    ir_sb b = {0};

    ir_sb_fmt(&b, "$%s is read on a path that does not assign it, so promote "
                  "supplied undef", l->name);

    char *msg = ir_sb_take(&b);

    ir_diag_message(dg, p->opts->diags, msg);
    free(msg);
}

static const char *derived_name(P *p, const ir_local_t *l, uint32_t nth)
{
    if (p->opts && p->opts->rename) {
        return p->opts->rename(p->m->ctx, l->name, nth, p->opts->state);
    }

    size_t n    = strlen(l->name) + 24;
    char  *name = ir_alloc(p->m->ctx, n);

    snprintf(name, n, "%s_%u", l->name, nth);

    for (uint32_t bump = 1; !name_free(p, name); bump++) {
        snprintf(name, n, "%s_%u.%u", l->name, nth, bump);
    }
    name_take(p, name);

    return name;
}

static ir_value_t *fresh(P *p, ir_instr_t *in, const ir_local_t *l)
{
    const char *name = derived_name(p, l, p->counter[l->index]++);

    return ir_instr_set_dest(in, name, l->type) ? in->dest : nullptr;
}

typedef struct {
    ir_instr_t *in;
    uint32_t    slot;
    uint32_t    tok;
    ir_local_t *l;
} useref;

#define NO_TOK UINT32_MAX

static bool gather_uses(P *p, ir_func_t *f, ir_rd_t *r, useref **out,
                        uint32_t *n_out, bool *wants, bool *warn)
{
    ir_ctx_t *ctx = p->m->ctx;
    size_t    cap = 0;
    useref   *use = nullptr;
    uint32_t  n   = 0;

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        for (uint32_t k = 0; k < f->blocks[i]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[i]->instrs[k];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_local_t *l = ir_operand_local(&in->operands[o]);

                if (!l) {
                    continue;
                }

                uint32_t tok = ir_rd_use_tok(r, in, l);

                use = ir_grow(ctx, use, &cap, n + 1u, sizeof(useref));

                if (!use) {
                    return false;
                }
                use[n++] = (useref){.in = in, .slot = o, .tok = tok, .l = l};

                if (tok == NO_TOK || !tok) {
                    wants[l->index] = true;
                    warn[l->index]  = warn[l->index] || tok != NO_TOK;
                }
            }
        }
    }
    *out   = use;
    *n_out = n;

    return true;
}

typedef struct {
    P           *p;
    ir_rd_t     *r;
    ir_value_t **val;
    ir_instr_t **phi;
} number_t;

static bool number_block(ir_block_t *b, void *thunk)
{
    number_t *w = thunk;
    uint32_t  first;
    uint32_t  n = ir_rd_defs_in(w->r, b, &first);

    for (uint32_t k = 0; k < ir_rd_phis_in(w->r, b); k++) {
        uint32_t q = ir_rd_phi_at(w->r, b, k);

        w->val[ir_rd_phi_tok(w->r, q)] = fresh(w->p, w->phi[q],
                                               ir_rd_phi_local(w->r, q));
    }

    for (uint32_t d = first; d < first + n; d++) {
        w->val[ir_rd_def_tok(w->r, d)] = fresh(w->p, ir_rd_def_instr(w->r, d),
                                               ir_rd_def_local(w->r, d));
    }

    return true;
}

static bool promote_func(P *p, ir_func_t *f)
{
    if (!f->n_locals || !f->n_blocks) {
        return true;
    }
    p->f       = f;
    p->dom     = ir_doms_of(f);
    p->counter = ir_alloc(p->m->ctx, f->n_locals * sizeof(uint32_t));
    gather_names(p, f);

    ir_ctx_t *ctx = p->m->ctx;
    ir_rd_t  *r   = ir_ssa_of(f, p->dom);
    uint32_t  np  = ir_rd_phi_count(r);

    ir_value_t **val = ir_alloc(ctx, ir_rd_tok_count(r) * sizeof(ir_value_t *));
    ir_value_t **und = ir_alloc(ctx, f->n_locals * sizeof(ir_value_t *));
    ir_instr_t **phi = ir_alloc(ctx, (np ? np : 1) * sizeof(ir_instr_t *));
    bool        *want = ir_alloc(ctx, f->n_locals * sizeof(bool));
    bool        *warn = ir_alloc(ctx, f->n_locals * sizeof(bool));
    useref      *use  = nullptr;
    uint32_t     n_use = 0;

    if (!gather_uses(p, f, r, &use, &n_use, want, warn)) {
        return false;
    }

    for (uint32_t q = 0; q < np; q++) {
        for (uint32_t i = 0; i < ir_rd_arm_count(r, q); i++) {
            uint32_t l = ir_rd_phi_local(r, q)->index;

            if (!ir_rd_arm_tok(r, q, i)) {
                want[l] = true;
                warn[l] = true;
            }
        }
    }

    uint32_t at = 0;

    for (uint32_t l = 0; l < f->n_locals; l++) {
        if (!want[l]) {
            continue;
        }

        ir_instr_t *u = ir_instr_generated(ir_block_entry(f), IR_OP_UNDEF,
                                           f->locals[l]->span, "promote",
                                           at++);

        if (!u) {
            return false;
        }
        und[l] = fresh(p, u, f->locals[l]);

        if (warn[l]) {
            warn_unset(p, f->locals[l]);
        }
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < ir_rd_phis_in(r, b); k++) {
            uint32_t q = ir_rd_phi_at(r, b, k);

            phi[q] = ir_instr_generated(b, IR_OP_PHI,
                                        ir_rd_phi_local(r, q)->span,
                                        "promote", k);

            if (!phi[q]) {
                return false;
            }
        }
    }

    number_t nw = {.p = p, .r = r, .val = val, .phi = phi};

    ir_dom_walk(p->dom, ir_block_entry(f), number_block, nullptr, &nw);

    for (uint32_t i = 0; i < n_use; i++) {
        ir_value_t *v = use[i].tok == NO_TOK || !use[i].tok
                          ? und[use[i].l->index]
                          : val[use[i].tok];

        if (!v) {
            p->ok = false;

            continue;
        }
        use[i].in->operands[use[i].slot].value = v;
    }

    for (uint32_t q = 0; q < np; q++) {
        ir_local_t *l = ir_rd_phi_local(r, q);

        if (ir_rd_arm_count(r, q)
            != ir_pred_count(p->dom, ir_rd_phi_block(r, q))) {
            p->ok = false;
        }

        for (uint32_t i = 0; i < ir_rd_arm_count(r, q); i++) {
            uint32_t    tok = ir_rd_arm_tok(r, q, i);
            ir_value_t *v   = tok ? val[tok] : und[l->index];

            if (!v || !ir_instr_push_phi_arm(phi[q], ir_rd_arm_block(r, q, i),
                                             v)) {
                p->ok = false;
            }
        }
    }
    f->n_locals = 0;

    return p->ok;
}

bool ir_promote(ir_module_t *m, const ir_promote_opts_t *opts)
{
    if (!m) {
        return false;
    }

    P p = {.m = m, .opts = opts, .ok = true};

    for (size_t i = 0; i < ir_func_count(m); i++) {
        if (!promote_func(&p, ir_func_at(m, i))) {
            return false;
        }
    }

    if (p.ok) {
        ir_module_establish(m, IR_PROP_SSA);
    }

    return p.ok;
}

static bool promote_run(ir_module_t *m, void *state)
{
    ir_promote_opts_t opts = {.diags = (ir_diags_t *)state};

    return ir_promote(m, &opts);
}

const ir_pass_t ir_pass_promote = {
    .name        = "promote",
    .requires    = IR_PROP_PARSED | IR_PROP_TYPED,
    .establishes = IR_PROP_SSA,
    .run         = promote_run,
};

static void retarget(ir_instr_t *t, ir_block_t *from, ir_block_t *to)
{
    for (uint32_t i = 0; i < t->n_operands; i++) {
        ir_operand_t *o = &t->operands[i];

        if ((o->kind == IR_OPD_BLOCK || o->kind == IR_OPD_DEFAULT_BLOCK)
            && o->block == from) {
            o->block = to;

            return;
        }

        if (o->kind == IR_OPD_SWITCH_ARM && o->arm.block == from) {
            o->arm.block = to;

            return;
        }
    }
}

bool ir_split_edges(ir_module_t *m, ir_diags_t *d)
{
    (void)d;

    if (!m) {
        return false;
    }

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f     = ir_func_at(m, i);
        uint32_t   walls = f->n_blocks;
        ir_doms_t *dom   = ir_doms_of(f);

        uint32_t     edges = ir_pred_total(dom);
        ir_block_t **mids  = ir_alloc(m->ctx, (edges ? edges : 1)
                                                  * sizeof(ir_block_t *));
        uint32_t    *first = ir_alloc(m->ctx, walls * sizeof(uint32_t));
        uint32_t    *count = ir_alloc(m->ctx, walls * sizeof(uint32_t));
        uint32_t    *used  = ir_alloc(m->ctx, walls * sizeof(uint32_t));
        uint32_t    *hit   = ir_alloc(m->ctx, walls * sizeof(uint32_t));

        for (uint32_t k = 0; k < walls; k++) {
            ir_block_t *to = f->blocks[k];
            uint32_t    nh = 0;
            uint32_t    nm = 0;

            if (ir_pred_count(dom, to) < 2) {
                continue;
            }

            for (uint32_t p = 0; p < ir_pred_count(dom, to); p++) {
                ir_block_t *b = ir_pred_at(dom, to, p);
                ir_instr_t *t = ir_block_terminator(b);

                if (ir_succ_count(t) < 2) {
                    continue;
                }

                size_t n    = strlen(b->name) + strlen(to->name) + 20;
                char  *name = ir_alloc(m->ctx, n);

                snprintf(name, n, "%s.to.%s", b->name, to->name);

                for (uint32_t bump = 1; ir_block_find(f, name); bump++) {
                    snprintf(name, n, "%s.to.%s.%u", b->name, to->name, bump);
                }

                ir_block_t *cut = ir_block_add(f, name);

                if (!cut) {
                    return false;
                }
                cut->span = ir_span_known(b->span) ? b->span : to->span;

                ir_instr_t *jump = ir_instr_generated(cut, IR_OP_JUMP,
                                                      cut->span, "split-edges",
                                                      0);

                if (!jump || !ir_instr_push_block(jump, to)) {
                    return false;
                }
                retarget(t, to, cut);

                if (!count[b->index]) {
                    first[b->index] = nm;
                    hit[nh++]       = b->index;
                }
                count[b->index]++;
                mids[nm++] = cut;
            }

            for (uint32_t a = 0; nh && a < to->n_instrs; a++) {
                ir_instr_t *phi = to->instrs[a];

                if (phi->op != IR_OP_PHI) {
                    break;
                }

                for (uint32_t x = 0; x < nh; x++) {
                    used[hit[x]] = 0;
                }

                for (uint32_t o = 0; o < phi->n_operands; o++) {
                    ir_block_t *arm = phi->operands[o].kind == IR_OPD_PHI_ARM
                                        ? phi->operands[o].phi.block
                                        : nullptr;

                    if (!arm || arm->index >= walls || !count[arm->index]) {
                        continue;
                    }

                    uint32_t u = used[arm->index]++;

                    if (u < count[arm->index]) {
                        phi->operands[o].phi.block = mids[first[arm->index]
                                                          + u];
                    }
                }
            }

            for (uint32_t x = 0; x < nh; x++) {
                count[hit[x]] = 0;
            }
        }
    }
    ir_module_invalidate(m, ir_pass_split_edges.invalidates);
    ir_module_establish(m, IR_PROP_NO_CRIT_EDGE);

    return true;
}

static bool split_run(ir_module_t *m, void *state)
{
    return ir_split_edges(m, (ir_diags_t *)state);
}

const ir_pass_t ir_pass_split_edges = {
    .name        = "split-edges",
    .requires    = IR_PROP_PARSED,
    .establishes = IR_PROP_NO_CRIT_EDGE,
    .invalidates = IR_PROP_STRUCTURED,
    .run         = split_run,
};
