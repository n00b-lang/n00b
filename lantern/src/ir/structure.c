#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "lib/option.h"
#include "ir/pass.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/region_walk.h"

#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    ir_region_fn pre;
    ir_region_fn post;
} rslot_t;

core_list_decl(rslot_t);

typedef core_list_t(rslot_t) rchain_t;

struct ir_region_walk_t {
    void    *user;
    rchain_t any;
    rchain_t by_kind[IR_REGION_KIND_COUNT];
};

static void radd(rchain_t *c, ir_region_fn pre, ir_region_fn post)
{
    core_list_push(*c, ((rslot_t){.pre = pre, .post = post}));
}

ir_region_walk_t *ir_region_walk_new(void *user)
{
    ir_region_walk_t *w = core_alloc(typeof(*w));

    w->user = user;

    return w;
}

void ir_region_walk_free(ir_region_walk_t *w)
{
    if (!w) {
        return;
    }

    core_list_free(w->any);

    for (int i = 0; i < IR_REGION_KIND_COUNT; i++) {
        core_list_free(w->by_kind[i]);
    }

    core_free(w);
}

void ir_region_on_node(ir_region_walk_t *w, ir_region_fn pre,
                       ir_region_fn post)
{
    radd(&w->any, pre, post);
}

void ir_region_on_kind(ir_region_walk_t *w, ir_region_kind_t kind,
                       ir_region_fn pre, ir_region_fn post)
{
    if (kind < IR_REGION_KIND_COUNT) {
        radd(&w->by_kind[kind], pre, post);
    }
}

static void rrun(rchain_t *c, ir_region_visit_t *v, bool pre, bool reverse,
                 bool *asked, bool *wants)
{
    for (size_t k = 0; k < c->len; k++) {
        size_t       i  = reverse ? c->len - 1 - k : k;
        ir_region_fn fn = pre ? c->data[i].pre : c->data[i].post;

        if (!fn) {
            continue;
        }

        bool want = fn(v);

        if (pre) {
            *asked = true;
            *wants = *wants || want;
        }
    }
}

typedef struct {
    ir_region_t *r;
    uint32_t     next;
    bool         entered;
} rframe_t;

core_option_decl(rframe_t);
core_list_decl(rframe_t);

void ir_region_walk_run(ir_region_walk_t *w, ir_region_t *root)
{
    if (!w || !root) {
        return;
    }

    core_list_t(rframe_t) stack = core_list_new_cap(rframe_t, 16);

    core_list_push(stack, ((rframe_t){.r = root}));

    while (stack.len) {
        rframe_t         *f = &stack.data[stack.len - 1];
        ir_region_visit_t v = {.region = f->r, .user = w->user};

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
        }

        if (f->next < f->r->n_kids) {
            ir_region_t *kid = f->r->kids[f->next++];

            core_list_push(stack, ((rframe_t){.r = kid}));

            continue;
        }

        bool asked = false;
        bool wants = false;

        if (f->r->kind < IR_REGION_KIND_COUNT) {
            rrun(&w->by_kind[f->r->kind], &v, false, true, &asked, &wants);
        }

        rrun(&w->any, &v, false, true, &asked, &wants);
        core_list_pop(rframe_t, stack);
    }

    core_list_free(stack);
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

typedef enum : uint8_t {
    P_STEP,
    P_LOOP,
    P_ARMS,
} sphase;

typedef struct {
    ir_region_t  *seq;
    ir_block_t   *cur;
    ir_block_t   *stop;
    size_t        parent;
    ir_region_t  *result;

    sphase        ph;
    ir_region_t  *sub;
    ir_instr_t   *term;
    ir_block_t   *join;
    ir_block_t   *after;
    ir_block_t  **body;
    bool         *in;
    uint32_t      nbody;
    uint32_t      ns;
    uint32_t      i;
} sframe;

core_list_decl(sframe);

static ir_region_t *seq_done(ir_region_t *seq)
{
    if (!seq->n_kids) {
        return nullptr;
    }

    return seq->n_kids == 1 ? seq->kids[0] : seq;
}

static void start_loop(S *s, sframe *f)
{
    ir_ctx_t   *ctx  = s->m->ctx;
    ir_block_t *tail = back_edge_source(s, f->cur);
    uint32_t    n    = natural_loop(s, f->cur, tail);
    uint32_t    id   = s->mark[f->cur->index];

    f->body  = ir_alloc(ctx, n * sizeof(ir_block_t *));
    f->nbody = n;
    memcpy(f->body, s->work, n * sizeof(ir_block_t *));

    f->after = loop_exit(s, f->body, n, id);
    f->sub   = region(s, IR_REGION_LOOP, f->cur);
    f->term  = ir_block_terminator(f->cur);
    f->ns    = f->term ? f->term->n_operands : 0;
    f->in    = ir_alloc(ctx, (f->ns ? f->ns : 1) * sizeof(bool));

    for (uint32_t i = 0; i < f->ns; i++) {
        ir_block_t *to = ir_succ_of(&f->term->operands[i]);

        f->in[i] = to && s->mark[to->index] == id;
    }
    s->placed[f->cur->index] = true;
    adopt(s, f->sub, region(s, IR_REGION_BLOCK, f->cur));

    f->join = f->cur;
    f->i    = 0;
    f->ph   = P_LOOP;
}

static void finish_loop(S *s, sframe *f)
{
    for (uint32_t i = 0; i < f->nbody; i++) {
        if (!s->placed[f->body[i]->index]) {
            s->placed[f->body[i]->index] = true;
            adopt(s, f->sub, region(s, IR_REGION_BLOCK, f->body[i]));
        }
    }
    adopt(s, f->seq, f->sub);

    f->cur = f->after;
    f->ph  = P_STEP;
}

static void start_arms(S *s, sframe *f, uint32_t n)
{
    f->sub = region(s, n == 2 ? IR_REGION_IF : IR_REGION_SWITCH, f->cur);

    adopt(s, f->sub, region(s, IR_REGION_BLOCK, f->cur));

    f->term = ir_block_terminator(f->cur);
    f->join = ir_ipdom_of(s->dom, f->cur);
    f->ns   = f->term ? f->term->n_operands : 0;
    f->i    = 0;
    f->ph   = P_ARMS;
}

static ir_block_t *next_child(S *s, sframe *f)
{
    while (f->i < f->ns) {
        ir_block_t *to = ir_succ_of(&f->term->operands[f->i++]);

        if (f->ph == P_LOOP) {
            if (f->in[f->i - 1] && to && !s->placed[to->index]) {
                return to;
            }

            continue;
        }

        if (to && to != f->join && !s->placed[to->index]
            && ir_dominates(s->dom, f->cur, to)) {
            return to;
        }
    }

    return nullptr;
}

static ir_region_t *build_seq(S *s, ir_block_t *from, ir_block_t *stop)
{
    core_list_t(sframe) st   = core_list_new(sframe);
    ir_region_t       *root = nullptr;

    core_list_push(st, ((sframe){.seq    = region(s, IR_REGION_SEQ, from),
                                .cur    = from,
                                .stop   = stop,
                                .parent = SIZE_MAX}));

    while (st.len) {
        size_t  n = st.len;
        sframe *f = &st.data[n - 1];

        if (f->ph != P_STEP) {
            adopt(s, f->sub, f->result);
            f->result = nullptr;

            ir_block_t *kid = next_child(s, f);

            if (kid) {
                ir_block_t *kstop = f->join;

                core_list_push(st,
                              ((sframe){.seq = region(s, IR_REGION_SEQ, kid),
                                        .cur    = kid,
                                        .stop   = kstop,
                                        .parent = n - 1}));

                continue;
            }

            if (f->ph == P_LOOP) {
                finish_loop(s, f);
            }
            else {
                adopt(s, f->seq, f->sub);
                f->cur = f->join;
                f->ph  = P_STEP;
            }
        }

        bool yielded = false;

        while (f->cur && f->cur != f->stop && !s->placed[f->cur->index]) {
            if (is_loop_header_at(s, f->cur)) {
                start_loop(s, f);
                yielded = true;
                break;
            }

            ir_instr_t *t  = ir_block_terminator(f->cur);
            uint32_t    ns = ir_succ_count(t);

            s->placed[f->cur->index] = true;

            if (ns < 2) {
                adopt(s, f->seq, region(s, IR_REGION_BLOCK, f->cur));
                f->cur = ns ? ir_succ_at(t, 0) : nullptr;

                continue;
            }

            start_arms(s, f, ns);
            yielded = true;
            break;
        }

        if (yielded) {
            continue;
        }

        ir_region_t *done = seq_done(f->seq);
        size_t       up   = f->parent;

        st.len--;

        if (up == SIZE_MAX) {
            root = done;
        }
        else {
            st.data[up].result = done;
        }
    }

    core_list_free(st);

    return root;
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
    core_dict_t  count;
} cover_t;

static uintptr_t covered(cover_t *c, ir_region_t *r)
{
    bool found = false;

    return (uintptr_t)core_dict_get(&c->count, r, &found);
}

static bool count_covered(ir_region_visit_t *v)
{
    cover_t  *c = v->user;
    uintptr_t n = v->region->kind == IR_REGION_BLOCK && v->region->head
                       && c->marked[v->region->head->index];

    for (uint32_t i = 0; i < v->region->n_kids; i++) {
        n += covered(c, v->region->kids[i]);
    }

    core_dict_put(&c->count, v->region, n);

    return true;
}

static void mark_dispatch(ir_region_t *root, const bool *marked, uint32_t all)
{
    cover_t c = {.marked = marked};

    core_dict_init(&c.count, nullptr, nullptr);

    ir_region_walk_t *w = ir_region_walk_new(&c);

    ir_region_on_node(w, nullptr, count_covered);
    ir_region_walk_run(w, root);
    ir_region_walk_free(w);

    ir_region_t *at = root;

    for (bool moved = true; moved;) {
        moved = false;

        for (uint32_t i = 0; i < at->n_kids; i++) {
            if (covered(&c, at->kids[i]) == all) {
                at    = at->kids[i];
                moved = true;
                break;
            }
        }
    }
    at->dispatch = true;
    core_dict_free(&c.count);
}

static bool structure_func(S *s, ir_func_t *f)
{
    if (!f->n_blocks) {
        return true;
    }
    size_t scope = ir_analysis_scope_begin();
    s->f      = f;
    s->dom    = ir_func_doms(f);
    s->placed = core_alloc_array(bool, f->n_blocks);
    s->mark   = core_alloc_array(uint32_t, f->n_blocks);
    s->work   = core_alloc_array(ir_block_t *, f->n_blocks);
    s->loops  = 0;

    ir_region_t *root = region(s, IR_REGION_SEQ, ir_block_entry(f));

    bool    *marked = core_alloc_array(bool, f->n_blocks);
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

    core_free(marked);
    core_free(s->placed);
    core_free(s->mark);
    core_free(s->work);
    s->placed = nullptr;
    s->mark   = nullptr;
    s->work   = nullptr;
    s->dom    = nullptr;
    ir_analysis_scope_end(scope);

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

static bool structure_run(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)state;

    return ir_structure(m, d);
}

const ir_pass_t ir_pass_structure = {
    .name        = CORE_STRING_STATIC("structure"),
    .requires    = IR_PROP_PARSED,
    .establishes = IR_PROP_STRUCTURED,
    .run         = structure_run,
};
