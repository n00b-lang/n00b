#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/pass.h"
#include "ir/verify.h"

typedef struct {
    ir_module_t  *m;
    ir_func_t    *f;
    ir_doms_t    *dom;
    bool         *placed;
    uint32_t     *mark;
    ir_block_t  **work;
    uint32_t      loops;
    bool          irreducible;
} S;

static int by_index(const void *a, const void *b)
{
    uint32_t x = (*(ir_block_t *const *)a)->index;
    uint32_t y = (*(ir_block_t *const *)b)->index;

    return x < y ? -1 : x > y ? 1 : 0;
}

const char *ir_region_kind_name(ir_region_kind_t k)
{
    switch (k) {
    case IR_REGION_IF:
        return "if";
    case IR_REGION_LOOP:
        return "loop";
    case IR_REGION_SWITCH:
        return "switch";
    case IR_REGION_BLOCK:
        return "block";
    default:
        return "seq";
    }
}

static ir_region_t *region(S *s, ir_region_kind_t kind, ir_block_t *head)
{
    ir_region_t *r = ir_alloc(s->m->ctx, sizeof(ir_region_t));

    r->kind = kind;
    r->head = head;
    r->span = head ? head->span : IR_SPAN_UNKNOWN;

    return r;
}

static void adopt(S *s, ir_region_t *parent, ir_region_t *kid)
{
    if (!kid) {
        return;
    }

    size_t cap = parent->cap_kids;

    parent->kids = ir_grow(s->m->ctx, parent->kids, &cap, parent->n_kids + 1u,
                           sizeof(ir_region_t *));
    parent->cap_kids               = (uint32_t)cap;
    parent->kids[parent->n_kids++] = kid;
    kid->parent                    = parent;
    parent->span                   = ir_span_cover(parent->span, kid->span);
}

static bool is_loop_header_at(S *s, ir_block_t *b)
{
    for (uint32_t i = 0; i < ir_pred_count(s->dom, b); i++) {
        if (ir_dominates(s->dom, b, ir_pred_at(s->dom, b, i))) {
            return true;
        }
    }

    return false;
}

static ir_block_t *back_edge_source(S *s, ir_block_t *header)
{
    for (uint32_t i = 0; i < ir_pred_count(s->dom, header); i++) {
        ir_block_t *p = ir_pred_at(s->dom, header, i);

        if (ir_dominates(s->dom, header, p)) {
            return p;
        }
    }

    return nullptr;
}

static uint32_t natural_loop(S *s, ir_block_t *header, ir_block_t *tail)
{
    uint32_t id   = ++s->loops;
    uint32_t n    = 0;
    uint32_t head = 0;

    s->mark[header->index] = id;
    s->work[n++]           = header;

    if (tail != header) {
        s->mark[tail->index] = id;
        s->work[n++]         = tail;
    }

    while (head < n) {
        ir_block_t *b = s->work[head++];

        if (b == header) {
            continue;
        }

        for (uint32_t i = 0; i < ir_pred_count(s->dom, b); i++) {
            ir_block_t *p = ir_pred_at(s->dom, b, i);

            if (s->mark[p->index] != id) {
                s->mark[p->index] = id;
                s->work[n++]      = p;
            }
        }
    }
    qsort(s->work, n, sizeof(ir_block_t *), by_index);

    return n;
}

static ir_block_t *loop_exit(S *s, ir_block_t **body, uint32_t n, uint32_t id)
{
    ir_block_t *best = nullptr;

    for (uint32_t i = 0; i < n; i++) {
        ir_instr_t *t = ir_block_terminator(body[i]);

        for (uint32_t k = 0; t && k < t->n_operands; k++) {
            ir_block_t *to = ir_succ_of(&t->operands[k]);

            if (!to || s->mark[to->index] == id) {
                continue;
            }

            if (!best || ir_rpo_index(s->dom, to) < ir_rpo_index(s->dom, best)) {
                best = to;
            }
        }
    }

    return best;
}

static ir_region_t *build_seq(S *s, ir_block_t *from, ir_block_t *stop);

static ir_region_t *build_loop(S *s, ir_block_t *header, ir_block_t **after)
{
    ir_ctx_t     *ctx  = s->m->ctx;
    ir_block_t   *tail = back_edge_source(s, header);
    uint32_t      n    = natural_loop(s, header, tail);
    uint32_t      id   = s->mark[header->index];
    ir_block_t  **body = ir_alloc(ctx, n * sizeof(ir_block_t *));

    memcpy(body, s->work, n * sizeof(ir_block_t *));

    ir_block_t  *out = loop_exit(s, body, n, id);
    ir_region_t *r   = region(s, IR_REGION_LOOP, header);
    ir_instr_t  *t   = ir_block_terminator(header);
    uint32_t     ns  = t ? t->n_operands : 0;
    bool        *in  = ir_alloc(ctx, (ns ? ns : 1) * sizeof(bool));

    for (uint32_t i = 0; i < ns; i++) {
        ir_block_t *to = ir_succ_of(&t->operands[i]);

        in[i] = to && s->mark[to->index] == id;
    }
    s->placed[header->index] = true;
    adopt(s, r, region(s, IR_REGION_BLOCK, header));

    for (uint32_t i = 0; i < ns; i++) {
        ir_block_t *to = ir_succ_of(&t->operands[i]);

        if (in[i] && !s->placed[to->index]) {
            adopt(s, r, build_seq(s, to, header));
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        if (!s->placed[body[i]->index]) {
            s->placed[body[i]->index] = true;
            adopt(s, r, region(s, IR_REGION_BLOCK, body[i]));
        }
    }
    *after = out;

    return r;
}

static ir_region_t *build_seq(S *s, ir_block_t *from, ir_block_t *stop)
{
    ir_region_t *seq = region(s, IR_REGION_SEQ, from);
    ir_block_t  *cur = from;

    while (cur && cur != stop && !s->placed[cur->index]) {
        if (is_loop_header_at(s, cur)) {
            ir_block_t *after = nullptr;

            adopt(s, seq, build_loop(s, cur, &after));
            cur = after;
            continue;
        }

        ir_instr_t *t = ir_block_terminator(cur);
        uint32_t    n = ir_succ_count(t);

        s->placed[cur->index] = true;

        if (n < 2) {
            adopt(s, seq, region(s, IR_REGION_BLOCK, cur));
            cur = n ? ir_succ_at(t, 0) : nullptr;
            continue;
        }

        ir_region_t *r = region(s, n == 2 ? IR_REGION_IF : IR_REGION_SWITCH,
                                cur);

        adopt(s, r, region(s, IR_REGION_BLOCK, cur));

        ir_block_t *join = ir_ipdom_of(s->dom, cur);

        for (uint32_t i = 0; i < t->n_operands; i++) {
            ir_block_t *arm = ir_succ_of(&t->operands[i]);

            if (arm && arm != join && !s->placed[arm->index]
                && ir_dominates(s->dom, cur, arm)) {
                adopt(s, r, build_seq(s, arm, join));
            }
        }
        adopt(s, seq, r);
        cur = join;
    }

    if (!seq->n_kids) {
        return nullptr;
    }

    return seq->n_kids == 1 ? seq->kids[0] : seq;
}

static uint32_t irreducible(S *s, bool *marked)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < s->f->n_blocks; i++) {
        ir_block_t *b = s->f->blocks[i];
        ir_instr_t *t = ir_block_terminator(b);

        if (!ir_block_reachable(s->dom, b)) {
            continue;
        }

        for (uint32_t k = 0; t && k < t->n_operands; k++) {
            ir_block_t *to = ir_succ_of(&t->operands[k]);

            if (to && ir_rpo_index(s->dom, to) <= ir_rpo_index(s->dom, b)
                && !ir_dominates(s->dom, to, b)) {
                n += !marked[b->index];
                n += !marked[to->index];
                marked[b->index]  = true;
                marked[to->index] = true;
            }
        }
    }

    return n;
}

static uint32_t covered_of(const ir_region_t *r, const bool *marked)
{
    uint32_t n = 0;

    if (!r) {
        return 0;
    }

    if (r->kind == IR_REGION_BLOCK && r->head && marked[r->head->index]) {
        n++;
    }

    for (uint32_t i = 0; i < r->n_kids; i++) {
        n += covered_of(r->kids[i], marked);
    }

    return n;
}

static void mark_dispatch(ir_region_t *root, const bool *marked, uint32_t all)
{
    ir_region_t *at = root;

    for (bool moved = true; moved;) {
        moved = false;

        for (uint32_t i = 0; i < at->n_kids; i++) {
            if (covered_of(at->kids[i], marked) == all) {
                at    = at->kids[i];
                moved = true;
                break;
            }
        }
    }
    at->dispatch = true;
}

static bool structure_func(S *s, ir_func_t *f)
{
    if (!f->n_blocks) {
        return true;
    }
    s->f      = f;
    s->dom    = ir_doms_of(f);
    s->placed = ir_alloc(s->m->ctx, f->n_blocks * sizeof(bool));
    s->mark   = ir_alloc(s->m->ctx, f->n_blocks * sizeof(uint32_t));
    s->work   = ir_alloc(s->m->ctx, f->n_blocks * sizeof(ir_block_t *));
    s->loops  = 0;

    ir_region_t *root = region(s, IR_REGION_SEQ, ir_block_entry(f));

    bool    *marked = ir_alloc(s->m->ctx, f->n_blocks * sizeof(bool));
    uint32_t n_irr  = irreducible(s, marked);

    ir_region_t *body = build_seq(s, ir_block_entry(f), nullptr);

    if (body && body->kind == IR_REGION_SEQ) {
        for (uint32_t i = 0; i < body->n_kids; i++) {
            adopt(s, root, body->kids[i]);
        }
    }
    else {
        adopt(s, root, body);
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        if (!s->placed[f->blocks[i]->index]) {
            s->placed[f->blocks[i]->index] = true;
            adopt(s, root, region(s, IR_REGION_BLOCK, f->blocks[i]));
        }
    }
    if (n_irr) {
        mark_dispatch(root, marked, n_irr);
    }
    f->regions = root;

    return true;
}

bool ir_structure(ir_module_t *m, ir_diags_t *d)
{
    (void)d;

    if (!m) {
        return false;
    }

    S s = {.m = m};

    for (size_t i = 0; i < ir_func_count(m); i++) {
        if (!structure_func(&s, ir_func_at(m, i))) {
            return false;
        }
    }
    ir_module_establish(m, IR_PROP_STRUCTURED);

    return true;
}

ir_region_t *ir_region_root(ir_func_t *f)
{
    return f ? f->regions : nullptr;
}

static ir_region_t *find_block(ir_region_t *r, ir_block_t *b)
{
    if (!r) {
        return nullptr;
    }

    if (r->kind == IR_REGION_BLOCK && r->head == b) {
        return r;
    }

    for (uint32_t i = 0; i < r->n_kids; i++) {
        ir_region_t *hit = find_block(r->kids[i], b);

        if (hit) {
            return hit;
        }
    }

    return nullptr;
}

ir_region_t *ir_region_of_block(ir_func_t *f, ir_block_t *b)
{
    return find_block(ir_region_root(f), b);
}

bool ir_region_needs_dispatch(const ir_region_t *r)
{
    return r && r->dispatch;
}

static ir_region_t *find_dispatch(ir_region_t *r)
{
    if (!r) {
        return nullptr;
    }

    if (r->dispatch) {
        return r;
    }

    for (uint32_t i = 0; i < r->n_kids; i++) {
        ir_region_t *hit = find_dispatch(r->kids[i]);

        if (hit) {
            return hit;
        }
    }

    return nullptr;
}

ir_region_t *ir_region_dispatch_site(ir_func_t *f)
{
    return find_dispatch(ir_region_root(f));
}

static bool structure_run(ir_module_t *m, void *state)
{
    return ir_structure(m, (ir_diags_t *)state);
}

const ir_pass_t ir_pass_structure = {
    .name        = "structure",
    .requires    = IR_PROP_PARSED,
    .establishes = IR_PROP_STRUCTURED,
    .run         = structure_run,
};

static void draw(ir_sb *b, ir_module_t *m, const ir_region_t *r, int depth)
{
    if (!r) {
        return;
    }

    if (r->kind == IR_REGION_BLOCK) {
        ir_sb_fmt(b, "%*sblock %s\n", depth * 2 + 4, "",
                  r->head ? r->head->name : "?");

        return;
    }

    ir_sb_fmt(b, "%*s%s", depth * 2 + 4, "", ir_region_kind_name(r->kind));

    if (r->head) {
        ir_sb_fmt(b, " at %s", r->head->name);
    }

    if (r->dispatch) {
        ir_sb_str(b, "   # irreducible: a back end must emit a dispatch loop");
    }
    ir_sb_str(b, " {\n");

    for (uint32_t i = 0; i < r->n_kids; i++) {
        draw(b, m, r->kids[i], depth + 1);
    }
    ir_sb_fmt(b, "%*s}\n", depth * 2 + 4, "");
}

char *ir_module_render_nested(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    ir_sb b = {0};

    ir_sb_fmt(&b, "# a rendering of module %s, not its format\n",
              ir_module_name(m));

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f = ir_func_at(m, i);

        ir_sb_fmt(&b, "\n  func @%s\n", f->name);

        if (!ir_region_root(f)) {
            ir_sb_str(&b, "    # no overlay; run structure first\n");
            continue;
        }
        draw(&b, m, ir_region_root(f), 0);
    }

    return ir_sb_take(&b);
}
