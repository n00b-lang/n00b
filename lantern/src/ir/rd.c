#include <string.h>

#include "private/ir/internal.h"
#include "ir/doms.h"
#include "ir/walk.h"

#define RD_NONE UINT32_MAX
#define TOK_UNSET 0u
#define TOK_DEF(i) (1u + (i))
#define TOK_PHI(r, i) (1u + (r)->n_defs + (i))

typedef struct {
    uint32_t block;
    uint32_t local;
} ir_vphi_t;

typedef struct {
    uint32_t phi;
    uint32_t tok;
    uint32_t from;
} ir_arm_t;

struct ir_rd_t {
    ir_func_t   *f;
    ir_doms_t   *dom;
    uint32_t    *base;
    uint32_t     n_nodes;

    ir_instr_t **def;
    uint32_t    *def_id;
    uint32_t    *def_local;
    uint32_t    *def_slot;
    bool        *def_used;
    uint32_t     n_defs;

    uint32_t    *ldef_off;
    uint32_t    *ldef_at;
    uint32_t    *lwords;
    uint32_t    *bdef_off;

    ir_vphi_t   *phi;
    uint32_t     n_phis;
    size_t       cap_phis;
    uint32_t    *phi_off;
    uint32_t    *phi_at;

    ir_arm_t    *arm;
    uint32_t     n_arms;
    size_t       cap_arms;
    uint32_t    *arm_off;
    uint32_t    *arm_at;
    uint32_t    *fan_off;
    uint32_t    *fan_at;

    uint64_t    *bits;
    size_t      *bit_off;

    uint32_t    *use_off;
    uint32_t    *use_local;
    uint32_t    *use_tok;
    uint32_t     n_uses;

    uint32_t    *rr_off;
    uint32_t    *rr_at;
    bool        *rr_unset;
};

static uint32_t use_slot(ir_rd_t *r, ir_instr_t *use, ir_local_t *l);

static uint32_t node_of(const ir_rd_t *r, const ir_instr_t *in)
{
    return in && in->block ? r->base[in->block->index] + in->index : RD_NONE;
}

static bool bit_get(const uint64_t *w, uint32_t i)
{
    return (w[i >> 6] >> (i & 63)) & 1u;
}

static bool bit_set(uint64_t *w, uint32_t i)
{
    if (bit_get(w, i)) {
        return false;
    }
    w[i >> 6] |= 1ull << (i & 63);

    return true;
}

static bool bit_or(uint64_t *dst, const uint64_t *src, uint32_t words)
{
    bool changed = false;

    for (uint32_t i = 0; i < words; i++) {
        uint64_t was = dst[i];

        dst[i] |= src[i];
        changed = changed || dst[i] != was;
    }

    return changed;
}

typedef struct {
    ir_rd_t  *r;
    uint32_t *fill;
    uint32_t  at_def;
} rd_fill_t;

static bool number_block(ir_visit_t *vi)
{
    ir_rd_t *r = vi->user;

    r->base[vi->block->index] = r->n_nodes;
    r->n_nodes += vi->block->n_instrs;

    return false;
}

static bool count_instr(ir_visit_t *vi)
{
    ir_rd_t *r = vi->user;

    if (vi->instr->dest_kind == IR_DEST_LOCAL && vi->instr->dest_local
        && !vi->instr->dest_local->address_taken) {
        r->n_defs++;
    }

    return true;
}

static bool count_operand(ir_visit_t *vi)
{
    ir_rd_t    *r = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (l && !l->address_taken
        && !ir_local_read_earlier(vi->instr, vi->index, l)) {
        r->use_off[node_of(r, vi->instr) + 1]++;
        r->n_uses++;
    }

    return true;
}

static bool fill_instr(ir_visit_t *vi)
{
    rd_fill_t  *s  = vi->user;
    ir_rd_t    *r  = s->r;
    ir_instr_t *in = vi->instr;

    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local
        && !in->dest_local->address_taken) {
        r->def[s->at_def]       = in;
        r->def_local[s->at_def] = in->dest_local->index;
        r->def_id[node_of(r, in)] = s->at_def;
        r->ldef_off[in->dest_local->index + 1]++;
        r->bdef_off[vi->block->index + 1]++;
        s->at_def++;
    }

    return true;
}

static bool fill_operand(ir_visit_t *vi)
{
    rd_fill_t  *s = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (l && !l->address_taken
        && !ir_local_read_earlier(vi->instr, vi->index, l)) {
        uint32_t u = s->fill[node_of(s->r, vi->instr)]++;

        s->r->use_local[u] = l->index;
        s->r->use_tok[u]   = RD_NONE;
    }

    return true;
}

static void collect(ir_rd_t *r)
{
    ir_func_t *f   = r->f;
    ir_ctx_t  *ctx = f->owner->ctx;

    r->base = ir_alloc(ctx, (f->n_blocks + 1) * sizeof(uint32_t));

    ir_walk_t *w = ir_walk_new(r);

    ir_walk_on_block(w, number_block, nullptr, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    r->use_off = ir_alloc(ctx, (r->n_nodes + 1) * sizeof(uint32_t));
    r->def_id  = ir_alloc(ctx, (r->n_nodes ? r->n_nodes : 1)
                                   * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_nodes; i++) {
        r->def_id[i] = RD_NONE;
    }

    w = ir_walk_new(r);

    ir_walk_on_instr(w, count_instr, nullptr, nullptr);
    ir_walk_on_operand(w, count_operand, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    for (uint32_t i = 0; i < r->n_nodes; i++) {
        r->use_off[i + 1] += r->use_off[i];
    }

    uint32_t nd = r->n_defs ? r->n_defs : 1;
    uint32_t nu = r->n_uses ? r->n_uses : 1;

    r->def       = ir_alloc(ctx, nd * sizeof(ir_instr_t *));
    r->def_local = ir_alloc(ctx, nd * sizeof(uint32_t));
    r->def_slot  = ir_alloc(ctx, nd * sizeof(uint32_t));
    r->def_used  = ir_alloc(ctx, nd * sizeof(bool));
    r->ldef_at   = ir_alloc(ctx, nd * sizeof(uint32_t));
    r->use_local = ir_alloc(ctx, nu * sizeof(uint32_t));
    r->use_tok   = ir_alloc(ctx, nu * sizeof(uint32_t));
    r->rr_unset  = ir_alloc(ctx, nu * sizeof(bool));
    r->ldef_off  = ir_alloc(ctx, (f->n_locals + 1) * sizeof(uint32_t));
    r->lwords    = ir_alloc(ctx, (f->n_locals + 1) * sizeof(uint32_t));
    r->bdef_off  = ir_alloc(ctx, (f->n_blocks + 1) * sizeof(uint32_t));

    uint32_t *fill = ir_alloc(ctx, (r->n_nodes + 1) * sizeof(uint32_t));

    memcpy(fill, r->use_off, (r->n_nodes + 1) * sizeof(uint32_t));

    rd_fill_t s = {.r = r, .fill = fill};

    w = ir_walk_new(&s);

    ir_walk_on_instr(w, fill_instr, nullptr, nullptr);
    ir_walk_on_operand(w, fill_operand, nullptr);
    ir_walk_run_func(w, f);
    ir_walk_free(w);

    for (uint32_t i = 0; i < f->n_locals; i++) {
        r->ldef_off[i + 1] += r->ldef_off[i];
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        r->bdef_off[i + 1] += r->bdef_off[i];
    }

    uint32_t *fd = ir_alloc(ctx, (f->n_locals + 1) * sizeof(uint32_t));

    memcpy(fd, r->ldef_off, (f->n_locals + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_defs; i++) {
        uint32_t l = r->def_local[i];

        r->def_slot[i]      = fd[l] - r->ldef_off[l];
        r->ldef_at[fd[l]++] = i;
    }

    for (uint32_t l = 0; l < f->n_locals; l++) {
        r->lwords[l] = (r->ldef_off[l + 1] - r->ldef_off[l] + 1 + 63) / 64;
    }
}

static void place_phis(ir_rd_t *r)
{
    ir_func_t    *f    = r->f;
    ir_ctx_t     *ctx  = f->owner->ctx;
    uint32_t      nb   = f->n_blocks;
    uint32_t     *seen = ir_alloc(ctx, nb * sizeof(uint32_t));
    uint32_t     *onq  = ir_alloc(ctx, nb * sizeof(uint32_t));
    ir_block_t  **work = ir_alloc(ctx, nb * sizeof(ir_block_t *));

    for (uint32_t i = 0; i < nb; i++) {
        seen[i] = RD_NONE;
        onq[i]  = RD_NONE;
    }

    for (uint32_t l = 0; l < f->n_locals; l++) {
        uint32_t n = 0;

        for (uint32_t i = r->ldef_off[l]; i < r->ldef_off[l + 1]; i++) {
            ir_block_t *b = r->def[r->ldef_at[i]]->block;

            if (onq[b->index] != l) {
                onq[b->index] = l;
                work[n++]     = b;
            }
        }

        while (n) {
            ir_block_t *b = work[--n];

            for (uint32_t e = 0; e < ir_df_count(r->dom, b); e++) {
                ir_block_t *y = ir_df_at(r->dom, b, e);

                if (seen[y->index] == l) {
                    continue;
                }
                seen[y->index] = l;
                r->phi         = ir_grow(ctx, r->phi, &r->cap_phis,
                                         r->n_phis + 1u, sizeof(ir_vphi_t));

                if (!r->phi) {
                    return;
                }
                r->phi[r->n_phis++] = (ir_vphi_t){.block = y->index,
                                                  .local = l};

                if (onq[y->index] != l) {
                    onq[y->index] = l;
                    work[n++]     = y;
                }
            }
        }
    }

    r->phi_off = ir_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_phis; i++) {
        r->phi_off[r->phi[i].block + 1]++;
    }

    for (uint32_t i = 0; i < nb; i++) {
        r->phi_off[i + 1] += r->phi_off[i];
    }
    r->phi_at = ir_alloc(ctx, (r->n_phis ? r->n_phis : 1) * sizeof(uint32_t));

    uint32_t *fp = ir_alloc(ctx, (nb + 1) * sizeof(uint32_t));

    memcpy(fp, r->phi_off, (nb + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_phis; i++) {
        r->phi_at[fp[r->phi[i].block]++] = i;
    }
}

static void add_arm(ir_rd_t *r, uint32_t phi, uint32_t tok, uint32_t from)
{
    r->arm = ir_grow(r->f->owner->ctx, r->arm, &r->cap_arms, r->n_arms + 1u,
                     sizeof(ir_arm_t));

    if (r->arm) {
        r->arm[r->n_arms++] = (ir_arm_t){.phi = phi, .tok = tok, .from = from};
    }
}

typedef struct {
    ir_rd_t  *r;
    uint32_t *st_off;
    uint32_t *st_top;
    uint32_t *st;
    uint32_t *trail;
    uint32_t *mark;
    uint32_t  tn;
} rename_t;

static bool rename_enter(ir_block_t *b, void *thunk)
{
    rename_t *w = thunk;
    ir_rd_t  *r = w->r;

    w->mark[b->index] = w->tn;

    for (uint32_t i = r->phi_off[b->index]; i < r->phi_off[b->index + 1];
         i++) {
        uint32_t p = r->phi_at[i];
        uint32_t l = r->phi[p].local;

        w->st[w->st_off[l] + w->st_top[l]++] = TOK_PHI(r, p);
        w->trail[w->tn++]                    = l;
    }

    for (uint32_t k = 0; k < b->n_instrs; k++) {
        ir_instr_t *in = b->instrs[k];
        uint32_t    at = node_of(r, in);

        for (uint32_t u = r->use_off[at]; u < r->use_off[at + 1]; u++) {
            uint32_t l = r->use_local[u];

            r->use_tok[u] = w->st_top[l]
                              ? w->st[w->st_off[l] + w->st_top[l] - 1]
                              : TOK_UNSET;
        }

        if (r->def_id[at] != RD_NONE) {
            uint32_t l = r->def_local[r->def_id[at]];

            w->st[w->st_off[l] + w->st_top[l]++] = TOK_DEF(r->def_id[at]);
            w->trail[w->tn++]                    = l;
        }
    }

    ir_instr_t *t = ir_block_terminator(b);

    for (uint32_t s = 0; t && s < t->n_operands; s++) {
        ir_block_t *y = ir_succ_of(&t->operands[s]);

        if (!y) {
            continue;
        }

        for (uint32_t i = r->phi_off[y->index]; i < r->phi_off[y->index + 1];
             i++) {
            uint32_t p = r->phi_at[i];
            uint32_t l = r->phi[p].local;

            add_arm(r, p,
                    w->st_top[l] ? w->st[w->st_off[l] + w->st_top[l] - 1]
                                 : TOK_UNSET,
                    b->index);
        }
    }

    return true;
}

static bool rename_leave(ir_block_t *b, void *thunk)
{
    rename_t *w = thunk;

    while (w->tn > w->mark[b->index]) {
        w->st_top[w->trail[--w->tn]]--;
    }

    return true;
}

static void rename_walk(ir_rd_t *r)
{
    ir_func_t *f    = r->f;
    ir_ctx_t  *ctx  = f->owner->ctx;
    uint32_t   room = r->n_defs + r->n_phis + 1;
    rename_t   w    = {.r = r};

    w.st_off = ir_alloc(ctx, (f->n_locals + 1) * sizeof(uint32_t));
    w.st_top = ir_alloc(ctx, f->n_locals * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_defs; i++) {
        w.st_off[r->def_local[i] + 1]++;
    }

    for (uint32_t i = 0; i < r->n_phis; i++) {
        w.st_off[r->phi[i].local + 1]++;
    }

    for (uint32_t i = 0; i < f->n_locals; i++) {
        w.st_off[i + 1] += w.st_off[i];
    }

    w.st    = ir_alloc(ctx, room * sizeof(uint32_t));
    w.trail = ir_alloc(ctx, room * sizeof(uint32_t));
    w.mark  = ir_alloc(ctx, f->n_blocks * sizeof(uint32_t));

    ir_dom_walk(r->dom, ir_block_entry(f), rename_enter, rename_leave, &w);
}

static uint64_t *phi_bits(ir_rd_t *r, uint32_t p)
{
    return r->bits + r->bit_off[p];
}

static void index_arms(ir_rd_t *r)
{
    ir_ctx_t *ctx = r->f->owner->ctx;

    r->arm_off = ir_alloc(ctx, (r->n_phis + 1) * sizeof(uint32_t));
    r->fan_off = ir_alloc(ctx, (r->n_phis + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_arms; i++) {
        r->arm_off[r->arm[i].phi + 1]++;

        if (r->arm[i].tok > r->n_defs) {
            r->fan_off[r->arm[i].tok - 1 - r->n_defs + 1]++;
        }
    }

    for (uint32_t i = 0; i < r->n_phis; i++) {
        r->arm_off[i + 1] += r->arm_off[i];
        r->fan_off[i + 1] += r->fan_off[i];
    }
    r->arm_at = ir_alloc(ctx, (r->n_arms ? r->n_arms : 1) * sizeof(uint32_t));
    r->fan_at = ir_alloc(ctx, (r->n_arms ? r->n_arms : 1) * sizeof(uint32_t));

    uint32_t *fa = ir_alloc(ctx, (r->n_phis + 1) * sizeof(uint32_t));
    uint32_t *ff = ir_alloc(ctx, (r->n_phis + 1) * sizeof(uint32_t));

    memcpy(fa, r->arm_off, (r->n_phis + 1) * sizeof(uint32_t));
    memcpy(ff, r->fan_off, (r->n_phis + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < r->n_arms; i++) {
        r->arm_at[fa[r->arm[i].phi]++] = i;

        if (r->arm[i].tok > r->n_defs) {
            r->fan_at[ff[r->arm[i].tok - 1 - r->n_defs]++] = r->arm[i].phi;
        }
    }
}

static void close_phis(ir_rd_t *r)
{
    ir_ctx_t *ctx   = r->f->owner->ctx;
    size_t    total = 0;

    r->bit_off = ir_alloc(ctx, (r->n_phis + 1) * sizeof(size_t));

    for (uint32_t p = 0; p < r->n_phis; p++) {
        r->bit_off[p] = total;
        total += r->lwords[r->phi[p].local];
    }
    r->bits = ir_alloc(ctx, (total ? total : 1) * sizeof(uint64_t));

    uint32_t *work = ir_alloc(ctx, (r->n_phis ? r->n_phis : 1)
                                       * sizeof(uint32_t));
    bool     *onq  = ir_alloc(ctx, (r->n_phis ? r->n_phis : 1) * sizeof(bool));
    uint32_t  n    = 0;

    for (uint32_t p = 0; p < r->n_phis; p++) {
        work[n++] = p;
        onq[p]    = true;
    }

    while (n) {
        uint32_t  p     = work[--n];
        uint64_t *dst   = phi_bits(r, p);
        uint32_t  l     = r->phi[p].local;
        uint32_t  undef = r->ldef_off[l + 1] - r->ldef_off[l];
        bool      moved = false;

        onq[p] = false;

        for (uint32_t i = r->arm_off[p]; i < r->arm_off[p + 1]; i++) {
            uint32_t tok = r->arm[r->arm_at[i]].tok;

            if (tok == TOK_UNSET) {
                moved = bit_set(dst, undef) || moved;
            }
            else if (tok <= r->n_defs) {
                moved = bit_set(dst, r->def_slot[tok - 1]) || moved;
            }
            else {
                moved = bit_or(dst, phi_bits(r, tok - 1 - r->n_defs),
                               r->lwords[l])
                     || moved;
            }
        }

        if (!moved) {
            continue;
        }

        for (uint32_t i = r->fan_off[p]; i < r->fan_off[p + 1]; i++) {
            uint32_t q = r->fan_at[i];

            if (!onq[q]) {
                onq[q]    = true;
                work[n++] = q;
            }
        }
    }
}

static void expand(ir_rd_t *r)
{
    ir_ctx_t *ctx = r->f->owner->ctx;

    r->rr_off = ir_alloc(ctx, (r->n_uses + 1) * sizeof(uint32_t));

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t u = 0; u < r->n_uses; u++) {
            uint32_t tok = r->use_tok[u];
            uint32_t l   = r->use_local[u];

            if (tok == RD_NONE) {
                continue;
            }

            if (tok == TOK_UNSET) {
                r->rr_unset[u] = true;

                continue;
            }

            if (tok <= r->n_defs) {
                if (pass) {
                    r->rr_at[r->rr_off[u]++] = tok - 1;
                }
                else {
                    r->rr_off[u + 1]++;
                }

                continue;
            }

            uint64_t *w  = phi_bits(r, tok - 1 - r->n_defs);
            uint32_t  nd = r->ldef_off[l + 1] - r->ldef_off[l];

            for (uint32_t i = 0; i < r->lwords[l]; i++) {
                for (uint64_t m = w[i]; m; m &= m - 1) {
                    uint32_t s = i * 64 + (uint32_t)__builtin_ctzll(m);

                    if (s >= nd) {
                        r->rr_unset[u] = true;

                        continue;
                    }

                    if (pass) {
                        r->rr_at[r->rr_off[u]++] = r->ldef_at[r->ldef_off[l]
                                                              + s];
                    }
                    else {
                        r->rr_off[u + 1]++;
                    }
                }
            }
        }

        if (pass) {
            break;
        }

        for (uint32_t u = 0; u < r->n_uses; u++) {
            r->rr_off[u + 1] += r->rr_off[u];
        }

        uint32_t n = r->rr_off[r->n_uses];

        r->rr_at = ir_alloc(ctx, (n ? n : 1) * sizeof(uint32_t));
    }

    for (uint32_t u = r->n_uses; u; u--) {
        r->rr_off[u] = r->rr_off[u - 1];
    }
    r->rr_off[0] = 0;

    for (uint32_t i = 0; i < r->rr_off[r->n_uses]; i++) {
        r->def_used[r->rr_at[i]] = true;
    }
}

static ir_rd_t *build(ir_func_t *f, ir_doms_t *d, bool sets)
{
    if (!f || !f->n_blocks || !d) {
        return nullptr;
    }

    ir_rd_t *r = ir_alloc(f->owner->ctx, sizeof(ir_rd_t));

    r->f   = f;
    r->dom = d;

    collect(r);

    if (f->n_locals) {
        place_phis(r);
        rename_walk(r);
        index_arms(r);
    }

    if (sets) {
        if (f->n_locals) {
            close_phis(r);
        }
        expand(r);
    }

    return r;
}

ir_rd_t *ir_rd_of(ir_func_t *f, ir_doms_t *d)
{
    return build(f, d, true);
}

ir_rd_t *ir_ssa_of(ir_func_t *f, ir_doms_t *d)
{
    return build(f, d, false);
}

uint32_t ir_rd_tok_count(const ir_rd_t *r)
{
    return r ? 1 + r->n_defs + r->n_phis : 0;
}

uint32_t ir_rd_phi_count(const ir_rd_t *r)
{
    return r ? r->n_phis : 0;
}

uint32_t ir_rd_phi_at(const ir_rd_t *r, ir_block_t *b, uint32_t i)
{
    return r->phi_at[r->phi_off[b->index] + i];
}

uint32_t ir_rd_phis_in(const ir_rd_t *r, ir_block_t *b)
{
    return r && r->phi_off ? r->phi_off[b->index + 1] - r->phi_off[b->index]
                           : 0;
}

ir_block_t *ir_rd_phi_block(const ir_rd_t *r, uint32_t p)
{
    return r->f->blocks[r->phi[p].block];
}

ir_local_t *ir_rd_phi_local(const ir_rd_t *r, uint32_t p)
{
    return r->f->locals[r->phi[p].local];
}

uint32_t ir_rd_phi_tok(const ir_rd_t *r, uint32_t p)
{
    return TOK_PHI(r, p);
}

uint32_t ir_rd_arm_count(const ir_rd_t *r, uint32_t p)
{
    return r->arm_off[p + 1] - r->arm_off[p];
}

uint32_t ir_rd_arm_tok(const ir_rd_t *r, uint32_t p, uint32_t i)
{
    return r->arm[r->arm_at[r->arm_off[p] + i]].tok;
}

ir_block_t *ir_rd_arm_block(const ir_rd_t *r, uint32_t p, uint32_t i)
{
    return r->f->blocks[r->arm[r->arm_at[r->arm_off[p] + i]].from];
}

uint32_t ir_rd_defs_in(const ir_rd_t *r, ir_block_t *b, uint32_t *first)
{
    *first = r->bdef_off[b->index];

    return r->bdef_off[b->index + 1] - r->bdef_off[b->index];
}

ir_instr_t *ir_rd_def_instr(const ir_rd_t *r, uint32_t d)
{
    return r->def[d];
}

ir_local_t *ir_rd_def_local(const ir_rd_t *r, uint32_t d)
{
    return r->f->locals[r->def_local[d]];
}

uint32_t ir_rd_def_tok(const ir_rd_t *r, uint32_t d)
{
    (void)r;

    return TOK_DEF(d);
}

uint32_t ir_rd_use_tok(ir_rd_t *r, ir_instr_t *use, ir_local_t *l)
{
    uint32_t u = use_slot(r, use, l);

    return u == RD_NONE ? RD_NONE : r->use_tok[u];
}

static uint32_t use_slot(ir_rd_t *r, ir_instr_t *use, ir_local_t *l)
{
    uint32_t at = r ? node_of(r, use) : RD_NONE;

    if (!l || at >= r->n_nodes) {
        return RD_NONE;
    }

    for (uint32_t u = r->use_off[at]; u < r->use_off[at + 1]; u++) {
        if (r->use_local[u] == l->index) {
            return u;
        }
    }

    return RD_NONE;
}

uint32_t ir_rd_count(ir_rd_t *r, ir_instr_t *use, ir_local_t *l)
{
    uint32_t u = use_slot(r, use, l);

    return u == RD_NONE ? 0 : r->rr_off[u + 1] - r->rr_off[u];
}

ir_instr_t *ir_rd_at(ir_rd_t *r, ir_instr_t *use, ir_local_t *l, uint32_t i)
{
    uint32_t u = use_slot(r, use, l);

    if (u == RD_NONE || i >= r->rr_off[u + 1] - r->rr_off[u]) {
        return nullptr;
    }

    return r->def[r->rr_at[r->rr_off[u] + i]];
}

bool ir_rd_maybe_unset(ir_rd_t *r, ir_instr_t *use, ir_local_t *l)
{
    uint32_t u = use_slot(r, use, l);

    return u != RD_NONE && r->rr_unset[u];
}

bool ir_rd_reaches_a_use(ir_rd_t *r, ir_instr_t *def)
{
    uint32_t at = r && def ? node_of(r, def) : RD_NONE;

    return at < r->n_nodes && r->def_id[at] != RD_NONE
        && r->def_used[r->def_id[at]];
}
