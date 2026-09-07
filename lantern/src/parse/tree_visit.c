#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <string.h>

struct parse_reduce_state_t;
static void reduce_release(parse_tree_walk_t *w);

typedef struct {
    parse_visit_fn pre;
    parse_visit_fn post;
    void        *thunk;
    int32_t      phase;
} visitor_t;

core_list_decl(visitor_t);

typedef core_list_t(int32_t) chain_t;

typedef struct {
    parse_tree_t *node;
    parse_tree_t *parent;
    int32_t           depth;
    int32_t           child_index;
    int32_t           next_child;
    bool              entered;
    bool              descend;
} frame_t;

core_list_decl(frame_t);

struct parse_tree_walk_t {
    parse_grammar_t          *g;
    void                   *user;
    core_list_t(visitor_t)   visitors;
    chain_t                 any;
    chain_t                 leaf;
    chain_t                 by_annot[PARSE_ANNOT_KIND_COUNT];
    core_dict_t             *by_nt;
    core_list_t(frame_t)     stack;
    struct parse_reduce_state_t *reduce;
    int32_t                 max_phase;
    bool                    stopped;
    uint32_t                replaced;
    parse_tree_t       *new_root;
};

static int32_t
add_visitor(parse_tree_walk_t *w, parse_visit_fn pre, parse_visit_fn post,
            void *thunk, int32_t phase)
{
    visitor_t v = {.pre = pre, .post = post, .thunk = thunk, .phase = phase};

    core_list_push(w->visitors, v);

    if (phase > w->max_phase) {
        w->max_phase = phase;
    }

    return (int32_t)(w->visitors.len - 1);
}

parse_tree_walk_t *
parse_tree_walk_new(parse_grammar_t *g, void *user)
{
    parse_tree_walk_t *w = core_alloc(parse_tree_walk_t);

    memset(w, 0, sizeof(*w));

    w->g     = g;
    w->user  = user;
    w->by_nt = core_alloc(core_dict_t);

    core_dict_init(w->by_nt, core_hash_word, nullptr);

    return w;
}

void
parse_tree_walk_free(parse_tree_walk_t *w)
{
    if (!w) {
        return;
    }

    core_dict_foreach (w->by_nt, e) {
        chain_t *c = (chain_t *)e->value;

        core_free(c->data);
        core_free(c);
    }

    core_dict_free(w->by_nt);
    core_free(w->by_nt);

    for (int i = 0; i < PARSE_ANNOT_KIND_COUNT; i++) {
        core_free(w->by_annot[i].data);
    }

    core_free(w->any.data);
    core_free(w->leaf.data);
    core_free(w->visitors.data);
    core_free(w->stack.data);
    reduce_release(w);
    core_free(w);
}

void *
parse_tree_walk_user(parse_tree_walk_t *w)
{
    return w ? w->user : nullptr;
}

void
parse_walk_on_node(parse_tree_walk_t *w, parse_visit_fn pre, parse_visit_fn post,
                 void *thunk)
{
    core_list_push(w->any, add_visitor(w, pre, post, thunk, 0));
}

void
parse_walk_on_leaf(parse_tree_walk_t *w, parse_visit_fn fn, void *thunk)
{
    core_list_push(w->leaf, add_visitor(w, fn, nullptr, thunk, 0));
}

void
parse_walk_on_annot_phase(parse_tree_walk_t *w, parse_annot_kind_t kind,
                        int32_t phase, parse_visit_fn pre, parse_visit_fn post,
                        void *thunk)
{
    if (kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT || phase < 0) {
        return;
    }

    core_list_push(w->by_annot[kind], add_visitor(w, pre, post, thunk, phase));
}

void
parse_walk_on_annot(parse_tree_walk_t *w, parse_annot_kind_t kind, parse_visit_fn pre,
                  parse_visit_fn post, void *thunk)
{
    parse_walk_on_annot_phase(w, kind, 0, pre, post, thunk);
}

bool
parse_walk_on_nt(parse_tree_walk_t *w, core_string_t nt_name, parse_visit_fn pre,
               parse_visit_fn post, void *thunk)
{
    if (!w->g) {
        return false;
    }

    parse_nonterm_t *nt = parse_find_nonterm(w->g, nt_name);

    if (!nt) {
        return false;
    }

    bool     found = false;
    chain_t *c     = (chain_t *)core_dict_get(w->by_nt, (uintptr_t)nt->id,
                                             &found);

    if (!found) {
        c = core_alloc(chain_t);
        memset(c, 0, sizeof(*c));
        core_dict_put(w->by_nt, (uintptr_t)nt->id, c);
    }

    core_list_push(*c, add_visitor(w, pre, post, thunk, 0));

    return true;
}

static chain_t *
nt_chain(parse_tree_walk_t *w, int64_t nt_id)
{
    bool  found = false;
    void *c     = core_dict_get(w->by_nt, (uintptr_t)nt_id, &found);

    return found ? (chain_t *)c : nullptr;
}

static bool
run_chain(parse_tree_walk_t *w, chain_t *c, parse_visit_t *v, bool pre,
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
        parse_visit_fn fn = pre ? vi->pre : vi->post;

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
run_annots(parse_tree_walk_t *w, parse_visit_t *v, bool pre, bool reverse)
{
    bool   descend = true;
    size_t n_nt    = parse_nt_annot_count(v->nt);
    size_t n_rule  = parse_rule_annot_count(v->rule);
    size_t total   = n_nt + n_rule;

    for (int32_t p = 0; p <= w->max_phase; p++) {
        int32_t phase = reverse ? w->max_phase - p : p;

        for (size_t k = 0; k < total; k++) {
            size_t            i = reverse ? total - 1 - k : k;
            parse_annotation_t *a
                = i < n_nt ? parse_nt_annot_at(v->nt, i)
                           : parse_rule_annot_at(v->rule, i - n_nt);

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
fill(parse_tree_walk_t *w, parse_visit_t *v, frame_t *f)
{
    memset(v, 0, sizeof(*v));

    v->walk        = w;
    v->grammar     = w->g;
    v->node        = f->node;
    v->parent      = f->parent;
    v->depth       = f->depth;
    v->child_index = f->child_index;
    v->user        = w->user;

    if (core_tree_is_leaf(f->node)) {
        return;
    }

    v->pn = &core_tree_node_value(f->node);

    if (v->pn->id < 0 || v->pn->id == PARSE_EMPTY_STRING || v->pn->group_top) {
        return;
    }

    v->nt   = parse_get_nonterm(w->g, v->pn->id);
    v->rule = parse_get_node_rule(w->g, v->pn);
}

static void
push_frame(parse_tree_walk_t *w, parse_tree_t *node, parse_tree_t *parent,
           int32_t depth, int32_t child_index)
{
    frame_t f = {
        .node        = node,
        .parent      = parent,
        .depth       = depth,
        .child_index = child_index,
    };

    core_list_push(w->stack, f);
}

void
parse_tree_walk_run(parse_tree_walk_t *w, parse_tree_t *root)
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
        parse_visit_t v;

        if (!f->entered) {
            f->entered = true;
            f->descend = !w->stopped;

            fill(w, &v, f);

            if (core_tree_is_leaf(f->node)) {
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

        size_t nc = f->descend && !w->stopped ? core_tree_num_children(f->node)
                                              : 0;

        if ((size_t)f->next_child < nc) {
            int32_t           ix    = f->next_child++;
            parse_tree_t *child = core_tree_child(f->node, ix);
            parse_tree_t *node  = f->node;
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

        if (v.replace && v.replace != v.node) {
            if (v.parent && v.child_index >= 0) {
                core_tree_set_child(v.parent, (size_t)v.child_index,
                                   v.replace);
            }
            else {
                w->new_root = v.replace;
            }

            w->replaced++;
        }

        w->stack.len = top;
    }
}

parse_tree_t *
parse_tree_walk_rewrite(parse_tree_walk_t *w, parse_tree_t *root)
{
    if (!w || !root) {
        return root;
    }

    w->replaced = 0;
    w->new_root = nullptr;

    parse_tree_walk_run(w, root);

    return w->new_root ? w->new_root : root;
}

uint32_t
parse_tree_walk_replaced(const parse_tree_walk_t *w)
{
    return w ? w->replaced : 0;
}

void
parse_tree_walk_stop(parse_tree_walk_t *w)
{
    if (w) {
        w->stopped = true;
    }
}

bool
parse_tree_walk_stopped(parse_tree_walk_t *w)
{
    return w && w->stopped;
}

typedef struct {
    parse_reduce_fn fn;
    void         *thunk;
} reducer_t;

core_list_decl(reducer_t);

struct parse_reduce_state_t {
    core_list_t(reducer_t) by_nt;
    reducer_t             any;
    reducer_t             leaf;
    core_list_t(reducer_t) unit_leaf;
    bool                  have_any;
    bool                  have_leaf;
};

static struct parse_reduce_state_t *reduce_state(parse_tree_walk_t *w)
{
    if (!w->reduce) {
        w->reduce = core_alloc(struct parse_reduce_state_t);
        memset(w->reduce, 0, sizeof(*w->reduce));
    }

    return w->reduce;
}

bool parse_walk_reduce_nt(parse_tree_walk_t *w, core_string_t nt_name,
                        parse_reduce_fn fn, void *thunk)
{
    if (!w || !w->g) {
        return false;
    }

    parse_nonterm_t *nt = parse_find_nonterm(w->g, nt_name);

    if (!nt) {
        return false;
    }

    int64_t id = nt->id;

    struct parse_reduce_state_t *st = reduce_state(w);

    while ((int64_t)core_list_len(st->by_nt) <= id) {
        reducer_t none = {0};

        core_list_push(st->by_nt, none);
    }

    st->by_nt.data[id] = (reducer_t){.fn = fn, .thunk = thunk};

    return true;
}

void parse_walk_reduce_default(parse_tree_walk_t *w, parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    st->any      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_any = true;
}

void parse_walk_reduce_leaf(parse_tree_walk_t *w, parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    st->leaf      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_leaf = true;
}

void parse_walk_reduce_leaf_in(parse_tree_walk_t *w, int32_t unit,
                             parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    while (core_list_len(st->unit_leaf) <= (size_t)unit) {
        core_list_push(st->unit_leaf, ((reducer_t){0}));
    }
    st->unit_leaf.data[unit] = (reducer_t){.fn = fn, .thunk = thunk};
}

static reducer_t *leaf_in_unit(struct parse_reduce_state_t *st, int32_t unit)
{
    if (unit > 0 && (size_t)unit < core_list_len(st->unit_leaf)
        && st->unit_leaf.data[unit].fn) {
        return &st->unit_leaf.data[unit];
    }

    return nullptr;
}

static reducer_t *leaf_for(struct parse_reduce_state_t *st, parse_grammar_t *g,
                           parse_tree_t **up, size_t n)
{
    for (size_t i = n; i-- > 0;) {
        parse_tree_t *p = up[i];

        if (!p || core_tree_is_leaf(p)) {
            continue;
        }

        parse_nonterm_t *nt = parse_get_nonterm(g, core_tree_node_value(p).id);
        reducer_t     *hit = nt ? leaf_in_unit(st, nt->unit) : nullptr;

        if (hit) {
            return hit;
        }
    }

    return st->have_leaf ? &st->leaf : nullptr;
}

typedef struct {
    parse_tree_t *node;
    size_t            next;
    size_t            mark;
    bool              entered;
} rframe_t;

core_list_decl(rframe_t);

typedef void *vptr_t;
core_list_decl(vptr_t);

#define REDUCE_LIST_MAGIC 0x52644c73u

void *parse_tree_walk_reduce(parse_tree_walk_t *w, parse_tree_t *root)
{
    if (!w || !root || !w->reduce) {
        return nullptr;
    }

    struct parse_reduce_state_t *st = w->reduce;

    core_list_t(rframe_t) frames = core_list_new(rframe_t);
    core_list_t(vptr_t)   done   = core_list_new(vptr_t);

    w->stopped = false;
    core_list_push(frames, ((rframe_t){.node = root}));

    while (frames.len > 0 && !w->stopped) {
        rframe_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            parse_visit_t v;
            frame_t     vf = {.node = f->node};

            f->entered = true;
            f->mark    = done.len;

            fill(w, &v, &vf);
            run_chain(w, &w->any, &v, true, false, -1);

            if (v.nt && !w->stopped) {
                run_chain(w, nt_chain(w, v.pn->id), &v, true, false, -1);
                run_annots(w, &v, true, false);
            }

            if (w->stopped) {
                break;
            }
        }

        if (!core_tree_is_leaf(f->node)
            && f->next < core_tree_num_children(f->node)) {
            parse_tree_t *ch = core_tree_child(f->node, f->next++);

            core_list_push(frames, ((rframe_t){.node = ch}));
            continue;
        }

        f = &frames.data[frames.len - 1];

        reducer_t *use = nullptr;

        if (core_tree_is_leaf(f->node)) {
            parse_tree_t *up[8];
            size_t            nup = 0;

            for (size_t k = frames.len - 1; k-- > 0 && nup < 8;) {
                up[nup++] = frames.data[k].node;
            }

            for (size_t a = 0, z = nup; a + 1 < z; a++, z--) {
                parse_tree_t *t = up[a];

                up[a]     = up[z - 1];
                up[z - 1] = t;
            }
            use = leaf_for(st, w->g, up, nup);
        }
        else {
            parse_nt_node_t *pn = &core_tree_node_value(f->node);

            if (pn->id >= 0 && !pn->group_top
                && (size_t)pn->id < core_list_len(st->by_nt)
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
            parse_reduce_t r = {
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
            parse_visit_t v;
            frame_t     vf = {.node = f->node};

            fill(w, &v, &vf);

            if (v.nt) {
                run_annots(w, &v, false, true);
                run_chain(w, nt_chain(w, v.pn->id), &v, false, true, -1);
            }

            run_chain(w, &w->any, &v, false, true, -1);
        }

        if (!use && !core_tree_is_leaf(f->node)
            && core_tree_node_value(f->node).group_top) {
            size_t             n    = done.len - mark;
            parse_reduce_list_t *list = core_alloc(parse_reduce_list_t);

            list->n = n;
            list->v = n ? core_alloc_array(vptr_t, n) : nullptr;

            for (size_t i = 0; i < n; i++) {
                list->v[i] = done.data[mark + i];
            }

            list->magic = REDUCE_LIST_MAGIC;
            result      = list;
        }

        done.len = mark;
        core_list_push(done, result);
        frames.len--;
    }

    void *out = !w->stopped && done.len ? done.data[done.len - 1] : nullptr;

    core_list_free(frames);
    core_list_free(done);

    return out;
}

static void reduce_release(parse_tree_walk_t *w)
{
    if (!w || !w->reduce) {
        return;
    }

    core_free(w->reduce->by_nt.data);
    core_free(w->reduce);
    w->reduce = nullptr;
}

parse_reduce_list_t *parse_reduce_as_list(void *result)
{
    parse_reduce_list_t *l = result;

    return l && l->magic == REDUCE_LIST_MAGIC ? l : nullptr;
}

parse_reduce_list_t *parse_reduce_list_new(size_t n)
{
    parse_reduce_list_t *l = core_alloc(parse_reduce_list_t);

    l->magic = REDUCE_LIST_MAGIC;
    l->n     = n;
    l->v     = n ? core_alloc_array(vptr_t, n) : nullptr;

    return l;
}
