#include "parse/tree_visit.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <string.h>

struct ncc_reduce_state_t;
static void reduce_release(ncc_tree_walk_t *w);

typedef struct {
    ncc_visit_fn pre;
    ncc_visit_fn post;
    void        *thunk;
    int32_t      phase;
} visitor_t;

ncc_list_decl(visitor_t);

typedef ncc_list_t(int32_t) chain_t;

typedef struct {
    ncc_parse_tree_t *node;
    ncc_parse_tree_t *parent;
    int32_t           depth;
    int32_t           child_index;
    int32_t           next_child;
    bool              entered;
    bool              descend;
} frame_t;

ncc_list_decl(frame_t);

struct ncc_tree_walk_t {
    ncc_grammar_t          *g;
    void                   *user;
    ncc_list_t(visitor_t)   visitors;
    chain_t                 any;
    chain_t                 leaf;
    chain_t                 by_annot[NCC_ANNOT_KIND_COUNT];
    ncc_dict_t             *by_nt;
    ncc_list_t(frame_t)     stack;
    struct ncc_reduce_state_t *reduce;
    int32_t                 max_phase;
    bool                    stopped;
};

static int32_t
add_visitor(ncc_tree_walk_t *w, ncc_visit_fn pre, ncc_visit_fn post,
            void *thunk, int32_t phase)
{
    visitor_t v = {.pre = pre, .post = post, .thunk = thunk, .phase = phase};

    ncc_list_push(w->visitors, v);

    if (phase > w->max_phase) {
        w->max_phase = phase;
    }

    return (int32_t)(w->visitors.len - 1);
}

ncc_tree_walk_t *
ncc_tree_walk_new(ncc_grammar_t *g, void *user)
{
    ncc_tree_walk_t *w = ncc_alloc(ncc_tree_walk_t);

    memset(w, 0, sizeof(*w));

    w->g     = g;
    w->user  = user;
    w->by_nt = ncc_alloc(ncc_dict_t);

    ncc_dict_init(w->by_nt, ncc_hash_word, nullptr);

    return w;
}

void
ncc_tree_walk_free(ncc_tree_walk_t *w)
{
    if (!w) {
        return;
    }

    for (size_t b = 0; b < w->by_nt->capacity; b++) {
        if (w->by_nt->buckets[b].state == _NCC_BUCKET_OCCUPIED) {
            chain_t *c = (chain_t *)w->by_nt->buckets[b].value;

            ncc_free(c->data);
            ncc_free(c);
        }
    }

    ncc_dict_free(w->by_nt);
    ncc_free(w->by_nt);

    for (int i = 0; i < NCC_ANNOT_KIND_COUNT; i++) {
        ncc_free(w->by_annot[i].data);
    }

    ncc_free(w->any.data);
    ncc_free(w->leaf.data);
    ncc_free(w->visitors.data);
    ncc_free(w->stack.data);
    reduce_release(w);
    ncc_free(w);
}

void *
ncc_tree_walk_user(ncc_tree_walk_t *w)
{
    return w ? w->user : nullptr;
}

void
ncc_walk_on_node(ncc_tree_walk_t *w, ncc_visit_fn pre, ncc_visit_fn post,
                 void *thunk)
{
    ncc_list_push(w->any, add_visitor(w, pre, post, thunk, 0));
}

void
ncc_walk_on_leaf(ncc_tree_walk_t *w, ncc_visit_fn fn, void *thunk)
{
    ncc_list_push(w->leaf, add_visitor(w, fn, nullptr, thunk, 0));
}

void
ncc_walk_on_annot_phase(ncc_tree_walk_t *w, ncc_annot_kind_t kind,
                        int32_t phase, ncc_visit_fn pre, ncc_visit_fn post,
                        void *thunk)
{
    if (kind < 0 || kind >= NCC_ANNOT_KIND_COUNT || phase < 0) {
        return;
    }

    ncc_list_push(w->by_annot[kind], add_visitor(w, pre, post, thunk, phase));
}

void
ncc_walk_on_annot(ncc_tree_walk_t *w, ncc_annot_kind_t kind, ncc_visit_fn pre,
                  ncc_visit_fn post, void *thunk)
{
    ncc_walk_on_annot_phase(w, kind, 0, pre, post, thunk);
}

bool
ncc_walk_on_nt(ncc_tree_walk_t *w, ncc_string_t nt_name, ncc_visit_fn pre,
               ncc_visit_fn post, void *thunk)
{
    if (!w->g) {
        return false;
    }

    ncc_nonterm_t *nt   = nullptr;
    size_t         n    = ncc_list_len(w->g->nt_list);

    for (size_t i = 0; i < n; i++) {
        if (ncc_string_eq(w->g->nt_list.data[i].name, nt_name)) {
            nt = &w->g->nt_list.data[i];
            break;
        }
    }

    if (!nt) {
        return false;
    }

    bool     found = false;
    chain_t *c     = (chain_t *)ncc_dict_get(w->by_nt, (uintptr_t)nt->id,
                                             &found);

    if (!found) {
        c = ncc_alloc(chain_t);
        memset(c, 0, sizeof(*c));
        ncc_dict_put(w->by_nt, (uintptr_t)nt->id, c);
    }

    ncc_list_push(*c, add_visitor(w, pre, post, thunk, 0));

    return true;
}

static chain_t *
nt_chain(ncc_tree_walk_t *w, int64_t nt_id)
{
    bool  found = false;
    void *c     = ncc_dict_get(w->by_nt, (uintptr_t)nt_id, &found);

    return found ? (chain_t *)c : nullptr;
}

static bool
run_chain(ncc_tree_walk_t *w, chain_t *c, ncc_visit_t *v, bool pre,
          bool reverse, int32_t phase)
{
    bool asked = false;
    bool wants = false;

    if (!c) {
        return true;
    }

    for (size_t k = 0; k < c->len; k++) {
        size_t       i  = reverse ? c->len - 1 - k : k;
        visitor_t   *vi = &w->visitors.data[c->data[i]];
        ncc_visit_fn fn = pre ? vi->pre : vi->post;

        if (!fn || (phase >= 0 && vi->phase != phase)) {
            continue;
        }

        v->thunk = vi->thunk;

        bool want = fn(v);

        if (pre) {
            asked = true;
            wants = wants || want;
        }
    }

    return !asked || wants;
}

static bool
run_annots(ncc_tree_walk_t *w, ncc_visit_t *v, bool pre, bool reverse)
{
    bool   descend = true;
    size_t n_nt    = ncc_nt_annot_count(v->nt);
    size_t n_rule  = ncc_rule_annot_count(v->rule);
    size_t total   = n_nt + n_rule;

    for (int32_t p = 0; p <= w->max_phase; p++) {
        int32_t phase = reverse ? w->max_phase - p : p;

        for (size_t k = 0; k < total; k++) {
            size_t            i = reverse ? total - 1 - k : k;
            ncc_annotation_t *a
                = i < n_nt ? ncc_nt_annot_at(v->nt, i)
                           : ncc_rule_annot_at(v->rule, i - n_nt);

            if (!a) {
                continue;
            }

            v->annot = a;

            if (!run_chain(w, &w->by_annot[a->kind], v, pre, reverse, phase)) {
                descend = false;
            }
        }
    }

    v->annot = nullptr;

    return descend;
}

static void
fill(ncc_tree_walk_t *w, ncc_visit_t *v, frame_t *f)
{
    memset(v, 0, sizeof(*v));

    v->walk        = w;
    v->grammar     = w->g;
    v->node        = f->node;
    v->parent      = f->parent;
    v->depth       = f->depth;
    v->child_index = f->child_index;
    v->user        = w->user;

    if (ncc_tree_is_leaf(f->node)) {
        return;
    }

    v->pn = &ncc_tree_node_value(f->node);

    if (v->pn->id < 0 || v->pn->id == NCC_EMPTY_STRING || v->pn->group_top) {
        return;
    }

    v->nt   = ncc_get_nonterm(w->g, v->pn->id);
    v->rule = ncc_get_node_rule(w->g, v->pn);
}

static void
push_frame(ncc_tree_walk_t *w, ncc_parse_tree_t *node, ncc_parse_tree_t *parent,
           int32_t depth, int32_t child_index)
{
    frame_t f = {
        .node        = node,
        .parent      = parent,
        .depth       = depth,
        .child_index = child_index,
    };

    ncc_list_push(w->stack, f);
}

void
ncc_tree_walk_run(ncc_tree_walk_t *w, ncc_parse_tree_t *root)
{
    if (!w || !root) {
        return;
    }

    w->stopped   = false;
    w->stack.len = 0;

    push_frame(w, root, nullptr, 0, -1);

    while (w->stack.len > 0) {
        size_t      top = w->stack.len - 1;
        frame_t    *f   = &w->stack.data[top];
        ncc_visit_t v;

        if (!f->entered) {
            f->entered = true;
            f->descend = !w->stopped;

            fill(w, &v, f);

            if (ncc_tree_is_leaf(f->node)) {
                if (!w->stopped) {
                    run_chain(w, &w->leaf, &v, true, false, -1);
                }

                w->stack.len--;
                continue;
            }

            if (!w->stopped) {
                if (!run_chain(w, &w->any, &v, true, false, -1)) {
                    f->descend = false;
                }

                if (v.nt && !run_chain(w, nt_chain(w, v.pn->id), &v, true,
                                       false, -1)) {
                    f->descend = false;
                }

                if (v.nt && !run_annots(w, &v, true, false)) {
                    f->descend = false;
                }
            }

            f = &w->stack.data[top];
        }

        size_t nc = f->descend && !w->stopped ? ncc_tree_num_children(f->node)
                                              : 0;

        if ((size_t)f->next_child < nc) {
            int32_t           ix    = f->next_child++;
            ncc_parse_tree_t *child = ncc_tree_child(f->node, ix);
            ncc_parse_tree_t *node  = f->node;
            int32_t           depth = f->depth;

            push_frame(w, child, node, depth + 1, ix);
            continue;
        }

        fill(w, &v, &w->stack.data[top]);

        if (v.nt) {
            run_annots(w, &v, false, true);
            run_chain(w, nt_chain(w, v.pn->id), &v, false, true, -1);
        }

        run_chain(w, &w->any, &v, false, true, -1);

        w->stack.len = top;
    }
}

void
ncc_tree_walk_stop(ncc_tree_walk_t *w)
{
    if (w) {
        w->stopped = true;
    }
}

bool
ncc_tree_walk_stopped(ncc_tree_walk_t *w)
{
    return w && w->stopped;
}

typedef struct {
    ncc_reduce_fn fn;
    void         *thunk;
} reducer_t;

ncc_list_decl(reducer_t);

struct ncc_reduce_state_t {
    ncc_list_t(reducer_t) by_nt;
    reducer_t             any;
    reducer_t             leaf;
    bool                  have_any;
    bool                  have_leaf;
};

static struct ncc_reduce_state_t *reduce_state(ncc_tree_walk_t *w)
{
    if (!w->reduce) {
        w->reduce = ncc_alloc(struct ncc_reduce_state_t);
        memset(w->reduce, 0, sizeof(*w->reduce));
    }

    return w->reduce;
}

bool ncc_walk_reduce_nt(ncc_tree_walk_t *w, ncc_string_t nt_name,
                        ncc_reduce_fn fn, void *thunk)
{
    if (!w || !w->g) {
        return false;
    }

    size_t n  = ncc_list_len(w->g->nt_list);
    int64_t id = -1;

    for (size_t i = 0; i < n; i++) {
        if (ncc_string_eq(w->g->nt_list.data[i].name, nt_name)) {
            id = w->g->nt_list.data[i].id;
            break;
        }
    }

    if (id < 0) {
        return false;
    }

    struct ncc_reduce_state_t *st = reduce_state(w);

    while ((int64_t)ncc_list_len(st->by_nt) <= id) {
        reducer_t none = {0};

        ncc_list_push(st->by_nt, none);
    }

    st->by_nt.data[id] = (reducer_t){.fn = fn, .thunk = thunk};

    return true;
}

void ncc_walk_reduce_default(ncc_tree_walk_t *w, ncc_reduce_fn fn, void *thunk)
{
    struct ncc_reduce_state_t *st = reduce_state(w);

    st->any      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_any = true;
}

void ncc_walk_reduce_leaf(ncc_tree_walk_t *w, ncc_reduce_fn fn, void *thunk)
{
    struct ncc_reduce_state_t *st = reduce_state(w);

    st->leaf      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_leaf = true;
}

typedef struct {
    ncc_parse_tree_t *node;
    size_t            next;
    size_t            mark;
    bool              entered;
} rframe_t;

ncc_list_decl(rframe_t);

typedef void *vptr_t;
ncc_list_decl(vptr_t);

#define REDUCE_LIST_MAGIC 0x52644c73u

void *ncc_tree_walk_reduce(ncc_tree_walk_t *w, ncc_parse_tree_t *root)
{
    if (!w || !root || !w->reduce) {
        return nullptr;
    }

    struct ncc_reduce_state_t *st = w->reduce;

    ncc_list_t(rframe_t) frames = ncc_list_new(rframe_t);
    ncc_list_t(vptr_t)   done   = ncc_list_new(vptr_t);

    ncc_list_push(frames, ((rframe_t){.node = root}));

    while (frames.len > 0) {
        rframe_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            ncc_visit_t v;
            frame_t     vf = {.node = f->node};

            f->entered = true;
            f->mark    = done.len;

            fill(w, &v, &vf);
            run_chain(w, &w->any, &v, true, false, -1);

            if (v.nt) {
                run_chain(w, nt_chain(w, v.pn->id), &v, true, false, -1);
                run_annots(w, &v, true, false);
            }
        }

        if (!ncc_tree_is_leaf(f->node)
            && f->next < ncc_tree_num_children(f->node)) {
            ncc_parse_tree_t *ch = ncc_tree_child(f->node, f->next++);

            ncc_list_push(frames, ((rframe_t){.node = ch}));
            continue;
        }

        f = &frames.data[frames.len - 1];

        reducer_t *use = nullptr;

        if (ncc_tree_is_leaf(f->node)) {
            use = st->have_leaf ? &st->leaf : nullptr;
        }
        else {
            ncc_nt_node_t *pn = &ncc_tree_node_value(f->node);

            if (pn->id >= 0 && !pn->group_top
                && (size_t)pn->id < ncc_list_len(st->by_nt)
                && st->by_nt.data[pn->id].fn) {
                use = &st->by_nt.data[pn->id];
            }
            else if (st->have_any) {
                use = &st->any;
            }
        }

        void  *result = nullptr;
        size_t mark   = f->mark;

        if (use) {
            ncc_reduce_t r = {
                .walk    = w,
                .grammar = w->g,
                .node    = f->node,
                .kids    = done.data + mark,
                .n_kids  = done.len - mark,
                .thunk   = use->thunk,
                .user    = w->user,
            };

            result = use->fn(&r);
        }
        else if (done.len - mark == 1) {
            result = done.data[mark];
        }

        {
            ncc_visit_t v;
            frame_t     vf = {.node = f->node};

            fill(w, &v, &vf);

            if (v.nt) {
                run_annots(w, &v, false, true);
                run_chain(w, nt_chain(w, v.pn->id), &v, false, true, -1);
            }

            run_chain(w, &w->any, &v, false, true, -1);
        }

        if (!use && !ncc_tree_is_leaf(f->node)
            && ncc_tree_node_value(f->node).group_top) {
            size_t             n    = done.len - mark;
            ncc_reduce_list_t *list = ncc_alloc(ncc_reduce_list_t);

            list->n = n;
            list->v = n ? ncc_alloc_array(vptr_t, n) : nullptr;

            for (size_t i = 0; i < n; i++) {
                list->v[i] = done.data[mark + i];
            }

            list->magic = REDUCE_LIST_MAGIC;
            result      = list;
        }

        done.len = mark;
        ncc_list_push(done, result);
        frames.len--;
    }

    void *out = done.len ? done.data[done.len - 1] : nullptr;

    ncc_list_free(frames);
    ncc_list_free(done);

    return out;
}

static void reduce_release(ncc_tree_walk_t *w)
{
    if (!w || !w->reduce) {
        return;
    }

    ncc_free(w->reduce->by_nt.data);
    ncc_free(w->reduce);
    w->reduce = nullptr;
}

ncc_reduce_list_t *ncc_reduce_as_list(void *result)
{
    ncc_reduce_list_t *l = result;

    return l && l->magic == REDUCE_LIST_MAGIC ? l : nullptr;
}

ncc_reduce_list_t *ncc_reduce_list_new(size_t n)
{
    ncc_reduce_list_t *l = ncc_alloc(ncc_reduce_list_t);

    l->magic = REDUCE_LIST_MAGIC;
    l->n     = n;
    l->v     = n ? ncc_alloc_array(vptr_t, n) : nullptr;

    return l;
}
