#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/doms.h"
#include "ir/walk.h"
#include "ir/pass.h"

typedef struct {
    ir_module_t             *m;
    ir_func_t               *f;
    ir_doms_t               *dom;
    const ir_promote_opts_t *opts;
    uint32_t                *counter;
    bool                     ok;
} P;

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

    core_buffer_t *b = core_buffer_empty();

    core_buffer_printf(b, "$%s is read on a path that does not assign it, so promote "
                  "supplied undef", l->name);

    char *msg = core_buffer_take(b);

    ir_diag_message(dg, p->opts->diags, msg);
    core_free(msg);
}

static const char *derived_name(P *p, const ir_local_t *l, uint32_t nth)
{
    if (p->opts && p->opts->rename) {
        return p->opts->rename(p->m->ctx, l->name, nth, p->opts->state);
    }

    size_t n    = strlen(l->name) + 24;
    char  *name = ir_alloc(p->m->ctx, n);

    snprintf(name, n, "%s_%u", l->name, nth);

    for (uint32_t bump = 1; ir_value_find(p->f, name); bump++) {
        snprintf(name, n, "%s_%u.%u", l->name, nth, bump);
    }

    return name;
}

static ir_value_t *fresh(P *p, ir_instr_t *in, const ir_local_t *l)
{
    const char *name = derived_name(p, l, p->counter[l->index]++);
    ir_value_t *value = ir_instr_set_dest(in, name, l->type)
        ? in->dest : nullptr;
    ir_annots_t *source = ir_value_annots(
        ir_local_value(p->f, (ir_local_t *)l));
    ir_annots_t *target = ir_value_annots(value);
    ir_ctx_t *ctx = p->m->ctx;
    for (uint32_t i = 0; value && source && target
         && i < ir_annot_count(source); i++) {
        if (!ir_annot_set(ctx, target, *ir_annot_at(source, i))) {
            p->ok = false;
            return nullptr;
        }
    }
    return value;
}

typedef struct {
    ir_instr_t *in;
    uint32_t    slot;
    uint32_t    tok;
    ir_local_t *l;
} useref;

#define NO_TOK UINT32_MAX

typedef struct {
    P        *p;
    ir_rd_t  *r;
    useref   *use;
    size_t    cap;
    uint32_t  n;
    bool     *wants;
    bool     *warn;
    bool      ok;
} gather_t;

static bool gather_operand(ir_visit_t *vi)
{
    gather_t   *s = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (!l || l->address_taken) {
        return true;
    }

    uint32_t tok = ir_rd_use_tok(s->r, vi->instr, l);

    s->use = ir_grow(s->p->m->ctx, s->use, &s->cap, s->n + 1u,
                     sizeof(useref));

    if (!s->use) {
        s->ok = false;
        ir_walk_stop(vi->walk);

        return false;
    }
    s->use[s->n++] = (useref){.in   = vi->instr, .slot = vi->index,
                              .tok  = tok,      .l    = l};

    if (tok == NO_TOK || !tok) {
        s->wants[l->index] = true;
        s->warn[l->index]  = s->warn[l->index] || tok != NO_TOK;
    }

    return true;
}

static bool gather_uses(P *p, ir_func_t *f, ir_rd_t *r, useref **out,
                        uint32_t *n_out, bool *wants, bool *warn)
{
    gather_t s = {.p = p, .r = r, .wants = wants, .warn = warn, .ok = true};

    ir_walk_t *w = ir_walk_new(&s);

    ir_walk_on_operand(w, gather_operand, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    *out   = s.use;
    *n_out = s.n;

    return s.ok;
}

typedef struct {
    ir_rd_t     *r;
    ir_instr_t **phi;
    bool         ok;
} mkphi_t;

static bool make_phis(ir_visit_t *vi)
{
    mkphi_t *s = vi->user;

    for (uint32_t k = 0; k < ir_rd_phis_in(s->r, vi->block); k++) {
        uint32_t q = ir_rd_phi_at(s->r, vi->block, k);

        s->phi[q] = ir_instr_generated(vi->block, IR_OP_PHI,
                                       ir_rd_phi_local(s->r, q)->span,
                                       "promote", k);

        if (!s->phi[q]) {
            s->ok = false;
            ir_walk_stop(vi->walk);

            return false;
        }
    }

    return false;
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
    for (uint32_t i = 0; i < f->n_locals; i++)
        f->locals[i]->address_taken = false;
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (instruction->op != IR_OP_ADDRESS_OF
                || !instruction->n_operands) continue;
            ir_local_t *local = ir_operand_local(&instruction->operands[0]);
            if (local && local->index < f->n_locals
                && f->locals[local->index] == local)
                local->address_taken = true;
        }
    }
    p->f       = f;
    p->dom     = ir_doms_of(f);
    p->counter = ir_alloc(p->m->ctx, f->n_locals * sizeof(uint32_t));

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

    bool *live_phi = ir_alloc(ctx, (np ? np : 1) * sizeof(bool));
    uint32_t n_defs = ir_rd_tok_count(r) - np - 1;
    for (uint32_t i = 0; i < n_use; i++)
        if (use[i].tok != NO_TOK && use[i].tok > n_defs)
            live_phi[use[i].tok - n_defs - 1] = true;
    bool changed;
    do {
        changed = false;
        for (uint32_t q = 0; q < np; q++) {
            if (!live_phi[q]) continue;
            for (uint32_t i = 0; i < ir_rd_arm_count(r, q); i++) {
                uint32_t tok = ir_rd_arm_tok(r, q, i);
                if (tok != NO_TOK && tok > n_defs
                    && !live_phi[tok - n_defs - 1]) {
                    live_phi[tok - n_defs - 1] = true;
                    changed = true;
                }
            }
        }
    } while (changed);

    for (uint32_t q = 0; q < np; q++) {
        for (uint32_t i = 0; i < ir_rd_arm_count(r, q); i++) {
            uint32_t l = ir_rd_phi_local(r, q)->index;

            if (!ir_rd_arm_tok(r, q, i)) {
                want[l] = true;
                warn[l] = warn[l] || live_phi[q];
            }
        }
        ir_block_t *join = ir_rd_phi_block(r, q);
        uint32_t local = ir_rd_phi_local(r, q)->index;
        for (uint32_t i = 0; i < ir_pred_count(p->dom, join); i++) {
            ir_block_t *pred = ir_pred_at(p->dom, join, i);
            if (!ir_block_reachable(p->dom, pred)) want[local] = true;
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

    mkphi_t mk = {.r = r, .phi = phi, .ok = true};

    ir_walk_t *pw = ir_walk_new(&mk);

    ir_walk_on_block(pw, make_phis, nullptr, nullptr);
    ir_walk_run_func(pw, f);
    ir_walk_free(pw);

    if (!mk.ok) {
        return false;
    }

    number_t nw = {.p = p, .r = r, .val = val, .phi = phi};

    ir_dom_walk(p->dom, ir_block_entry(f), number_block, nullptr, &nw);
    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *block = f->blocks[i];
        if (!ir_block_reachable(p->dom, block)) number_block(block, &nw);
    }

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
        ir_instr_t *instruction = phi[q];

        for (uint32_t i = 0; i < ir_rd_arm_count(r, q); i++) {
            uint32_t    tok = ir_rd_arm_tok(r, q, i);
            ir_value_t *v   = tok ? val[tok] : und[l->index];

            if (!v || !ir_instr_operand_phi_arm(instruction,
                                                ir_rd_arm_block(r, q, i),
                                             v)) {
                p->ok = false;
            }
        }
        ir_block_t *join = ir_rd_phi_block(r, q);
        for (uint32_t i = 0; i < ir_pred_count(p->dom, join); i++) {
            ir_block_t *pred = ir_pred_at(p->dom, join, i);
            if (ir_block_reachable(p->dom, pred)) continue;
            ir_value_t *value = und[l->index];
            if (!value || !ir_instr_operand_phi_arm(instruction, pred, value))
                p->ok = false;
        }
        if (instruction->n_operands != ir_pred_count(p->dom, join))
            p->ok = false;
    }
    uint32_t retained = 0;
    for (uint32_t i = 0; i < f->n_locals; i++) {
        ir_local_t *local = f->locals[i];
        if (!local->address_taken) continue;
        f->locals[retained] = local;
        local->index = retained;
        if (local->value) local->value->index = retained;
        retained++;
    }
    f->n_locals = retained;

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

static bool promote_run(ir_module_t *m, ir_diags_t *d, void *state)
{
    ir_promote_opts_t opts = {.diags = d};

    (void)state;

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

                if (!jump || !ir_instr_operand_block(jump, to)) {
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

static bool split_run(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)state;

    return ir_split_edges(m, d);
}

const ir_pass_t ir_pass_split_edges = {
    .name        = "split-edges",
    .requires    = IR_PROP_PARSED,
    .establishes = IR_PROP_NO_CRIT_EDGE,
    .invalidates = IR_PROP_STRUCTURED,
    .run         = split_run,
};
