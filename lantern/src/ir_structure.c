#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/pass.h"
#include "ir/doms.h"
#include "ir/region_walk.h"

typedef struct {
    ir_region_fn pre;
    ir_region_fn post;
    void        *thunk;
} rslot_t;

typedef struct {
    rslot_t *v;
    size_t   n;
    size_t   cap;
} rchain_t;

struct ir_region_walk_t {
    void    *user;
    rchain_t any;
    rchain_t by_kind[IR_REGION_KIND_COUNT];
    bool     stopped;
};

static void radd(rchain_t *c, ir_region_fn pre, ir_region_fn post, void *thunk)
{
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->v   = realloc(c->v, c->cap * sizeof(*c->v));
    }

    c->v[c->n++] = (rslot_t){.pre = pre, .post = post, .thunk = thunk};
}

ir_region_walk_t *ir_region_walk_new(void *user)
{
    ir_region_walk_t *w = calloc(1, sizeof(*w));

    w->user = user;

    return w;
}

void ir_region_walk_free(ir_region_walk_t *w)
{
    if (!w) {
        return;
    }

    free(w->any.v);

    for (int i = 0; i < IR_REGION_KIND_COUNT; i++) {
        free(w->by_kind[i].v);
    }

    free(w);
}

void *ir_region_walk_user(ir_region_walk_t *w)
{
    return w ? w->user : nullptr;
}

void ir_region_on_node(ir_region_walk_t *w, ir_region_fn pre,
                       ir_region_fn post, void *thunk)
{
    radd(&w->any, pre, post, thunk);
}

void ir_region_on_kind(ir_region_walk_t *w, ir_region_kind_t kind,
                       ir_region_fn pre, ir_region_fn post, void *thunk)
{
    if (kind < IR_REGION_KIND_COUNT) {
        radd(&w->by_kind[kind], pre, post, thunk);
    }
}

void ir_region_walk_stop(ir_region_walk_t *w)
{
    if (w) {
        w->stopped = true;
    }
}

bool ir_region_walk_stopped(ir_region_walk_t *w)
{
    return w && w->stopped;
}

static void rrun(rchain_t *c, ir_region_visit_t *v, bool pre, bool reverse,
                 bool *asked, bool *wants)
{
    for (size_t k = 0; k < c->n; k++) {
        size_t       i  = reverse ? c->n - 1 - k : k;
        ir_region_fn fn = pre ? c->v[i].pre : c->v[i].post;

        if (!fn) {
            continue;
        }

        v->thunk = c->v[i].thunk;

        bool want = fn(v);

        if (pre) {
            *asked = true;
            *wants = *wants || want;
        }
    }
}

typedef struct {
    ir_region_t *r;
    ir_region_t *parent;
    uint32_t     index;
    uint32_t     next;
    bool         entered;
} rframe_t;

void ir_region_walk_run(ir_region_walk_t *w, ir_region_t *root)
{
    if (!w || !root) {
        return;
    }

    size_t    cap   = 16;
    rframe_t *stack = calloc(cap, sizeof(rframe_t));
    size_t    sn    = 0;

    w->stopped = false;
    stack[sn++] = (rframe_t){.r = root};

    while (sn && !w->stopped) {
        rframe_t         *f = &stack[sn - 1];
        ir_region_visit_t v = {.walk   = w,
                               .region = f->r,
                               .parent = f->parent,
                               .index  = f->index,
                               .depth  = (int32_t)sn - 1,
                               .user   = w->user};

        if (!f->entered) {
            bool asked = false;
            bool wants = false;

            f->entered = true;

            rrun(&w->any, &v, true, false, &asked, &wants);

            if (f->r->kind < IR_REGION_KIND_COUNT) {
                rrun(&w->by_kind[f->r->kind], &v, true, false, &asked, &wants);
            }

            if (asked && !wants) {
                f->next = f->r->n_kids;
            }

            if (w->stopped) {
                break;
            }
        }

        if (f->next < f->r->n_kids) {
            uint32_t     at  = f->next++;
            ir_region_t *kid = f->r->kids[at];

            if (sn == cap) {
                cap  *= 2;
                stack = realloc(stack, cap * sizeof(rframe_t));
                f     = &stack[sn - 1];
            }
            stack[sn++] = (rframe_t){.r = kid, .parent = f->r, .index = at};

            continue;
        }

        bool asked = false;
        bool wants = false;

        if (f->r->kind < IR_REGION_KIND_COUNT) {
            rrun(&w->by_kind[f->r->kind], &v, false, true, &asked, &wants);
        }

        rrun(&w->any, &v, false, true, &asked, &wants);
        sn--;
    }

    free(stack);
}

void ir_region_walk_run_func(ir_region_walk_t *w, ir_func_t *f)
{
    ir_region_walk_run(w, ir_region_root(f));
}

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

typedef struct {
    const bool *marked;
    uint32_t    n;
} cover_t;

static bool count_covered(ir_region_visit_t *v)
{
    cover_t *c = v->user;

    if (v->region->head && c->marked[v->region->head->index]) {
        c->n++;
    }

    return true;
}

static uint32_t covered_of(ir_region_t *r, const bool *marked)
{
    cover_t c = {.marked = marked};

    ir_region_walk_t *w = ir_region_walk_new(&c);

    ir_region_on_kind(w, IR_REGION_BLOCK, count_covered, nullptr, nullptr);
    ir_region_walk_run(w, r);
    ir_region_walk_free(w);

    return c.n;
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

typedef struct {
    ir_block_t  *want;
    ir_region_t *hit;
} seek_t;

static bool seek_block(ir_region_visit_t *v)
{
    seek_t *s = v->user;

    if (v->region->head == s->want) {
        s->hit = v->region;
        ir_region_walk_stop(v->walk);
    }

    return true;
}

static ir_region_t *find_block(ir_region_t *r, ir_block_t *b)
{
    seek_t s = {.want = b};

    ir_region_walk_t *w = ir_region_walk_new(&s);

    ir_region_on_kind(w, IR_REGION_BLOCK, seek_block, nullptr, nullptr);
    ir_region_walk_run(w, r);
    ir_region_walk_free(w);

    return s.hit;
}

ir_region_t *ir_region_of_block(ir_func_t *f, ir_block_t *b)
{
    return find_block(ir_region_root(f), b);
}

bool ir_region_needs_dispatch(const ir_region_t *r)
{
    return r && r->dispatch;
}

static bool seek_dispatch(ir_region_visit_t *v)
{
    seek_t *s = v->user;

    if (v->region->dispatch) {
        s->hit = v->region;
        ir_region_walk_stop(v->walk);
    }

    return true;
}

static ir_region_t *find_dispatch(ir_region_t *r)
{
    seek_t s = {0};

    ir_region_walk_t *w = ir_region_walk_new(&s);

    ir_region_on_node(w, seek_dispatch, nullptr, nullptr);
    ir_region_walk_run(w, r);
    ir_region_walk_free(w);

    return s.hit;
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

static bool draw_block(ir_region_visit_t *v)
{
    ir_sb *b = v->user;

    ir_sb_fmt(b, "%*sblock %s\n", v->depth * 2 + 4, "",
              v->region->head ? v->region->head->name : "?");

    return false;
}

static bool draw_open(ir_region_visit_t *v)
{
    ir_sb *b = v->user;

    if (v->region->kind == IR_REGION_BLOCK) {
        return true;
    }

    ir_sb_fmt(b, "%*s%s", v->depth * 2 + 4, "",
              ir_region_kind_name(v->region->kind));

    if (v->region->head) {
        ir_sb_fmt(b, " at %s", v->region->head->name);
    }

    if (v->region->dispatch) {
        ir_sb_str(b, "   # irreducible: a back end must emit a dispatch loop");
    }
    ir_sb_str(b, " {\n");

    return true;
}

static bool draw_close(ir_region_visit_t *v)
{
    if (v->region->kind != IR_REGION_BLOCK) {
        ir_sb_fmt((ir_sb *)v->user, "%*s}\n", v->depth * 2 + 4, "");
    }

    return true;
}

static void draw(ir_sb *b, ir_region_t *r)
{
    ir_region_walk_t *w = ir_region_walk_new(b);

    ir_region_on_node(w, draw_open, draw_close, nullptr);
    ir_region_on_kind(w, IR_REGION_BLOCK, draw_block, nullptr, nullptr);
    ir_region_walk_run(w, r);
    ir_region_walk_free(w);
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
        draw(&b, ir_region_root(f));
    }

    return ir_sb_take(&b);
}
