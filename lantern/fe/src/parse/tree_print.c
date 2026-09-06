#include "parse/tree_print.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "internal/parse/grammar_internal.h"
#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/list.h"

#include <string.h>

typedef enum {
    GAP_NONE = 0,
    GAP_SPACE,
    GAP_SOFT,
    GAP_LINE,
    GAP_HARD,
    GAP_BLANK,
} gap_t;

typedef enum {
    D_TEXT,
    D_GAP,
    D_SEQ,
    D_ITEMS,
    D_NEST,
    D_ALIGN,
    D_GROUP,
} dkind_t;

typedef struct doc_t doc_t;

struct doc_t {
    dkind_t  kind;
    gap_t    gap;
    char    *text;
    size_t   len;
    doc_t  **kids;
    size_t   n;
    bool     empty;
    bool     rigid;
};

typedef doc_t *doc_ptr_t;
ncc_list_decl(doc_ptr_t);

typedef struct {
    doc_t  *doc;
    int32_t child;
    gap_t   gap;
} item_t;

ncc_list_decl(item_t);

typedef struct {
    ncc_arena_t    arena;
    ncc_grammar_t *g;
    ncc_string_t   source;
    int32_t        width;
    int32_t        indent;
} pr_t;

#define SLOTS 64

typedef struct {
    gap_t slot[SLOTS];
    bool  set[SLOTS];
    bool  nest[SLOTS];
    bool  align[SLOTS];
    bool  group[SLOTS];
    gap_t dflt;
    bool  nest_all;
    bool  align_all;
    bool  group_all;
} layout_t;

static doc_t *doc_new(pr_t *p, dkind_t kind)
{
    doc_t *d = ncc_arena_alloc(&p->arena, sizeof(doc_t));

    d->kind  = kind;
    d->empty = true;

    return d;
}

static doc_t *doc_seq(pr_t *p, dkind_t kind, doc_t **kids, size_t n)
{
    doc_t *d = doc_new(p, kind);

    if (n) {
        d->kids = ncc_arena_alloc(&p->arena, n * sizeof(doc_t *));
        memcpy(d->kids, kids, n * sizeof(doc_t *));
    }
    d->n = n;

    for (size_t i = 0; i < n; i++) {
        if (!kids[i]->empty) {
            d->empty = false;
        }

        if (kids[i]->rigid) {
            d->rigid = true;
        }
    }

    return d;
}

static doc_t *doc_wrap(pr_t *p, dkind_t kind, doc_t *body)
{
    doc_t *d = doc_new(p, kind);

    d->kids    = ncc_arena_alloc(&p->arena, sizeof(doc_t *));
    d->kids[0] = body;
    d->n       = 1;
    d->empty   = body->empty;
    d->rigid   = body->rigid;

    return d;
}

static doc_t *doc_gap(pr_t *p, gap_t g)
{
    doc_t *d = doc_new(p, D_GAP);

    d->gap   = g;
    d->rigid = g >= GAP_HARD;

    return d;
}

static gap_t gap_kind(ncc_annot_kind_t k, bool *is_gap)
{
    *is_gap = true;

    switch (k) {
    case NCC_ANNOT_NOSPACE:
    case NCC_ANNOT_CONCAT:
        return GAP_NONE;
    case NCC_ANNOT_SPACE:
        return GAP_SPACE;
    case NCC_ANNOT_SOFTLINE:
        return GAP_SOFT;
    case NCC_ANNOT_NEWLINE:
        return GAP_LINE;
    case NCC_ANNOT_HARDLINE:
        return GAP_HARD;
    case NCC_ANNOT_BLANKLINE:
        return GAP_BLANK;
    default:
        *is_gap = false;

        return GAP_NONE;
    }
}

static int32_t ref_index(pr_t *p, ncc_parse_tree_t *node, ncc_child_ref_t ref)
{
    if (!ncc_child_ref_is_set(ref)) {
        return -1;
    }

    if (ref.kind == NCC_ROLE_BY_INDEX) {
        return ref.index;
    }

    size_t n = ncc_tree_num_children(node);

    for (size_t i = 0; i < n; i++) {
        ncc_string_t have = ncc_tree_nt_name(p->g, ncc_tree_child(node, i));

        if (ncc_string_eq(have, ref.name)) {
            return (int32_t)i;
        }
    }

    return -1;
}

static void layout_of(pr_t *p, ncc_parse_tree_t *node, ncc_nonterm_t *nt,
                      ncc_parse_rule_t *rule, layout_t *out)
{
    memset(out, 0, sizeof(*out));

    out->dflt = GAP_SPACE;

    size_t n_nt   = ncc_nt_annot_count(nt);
    size_t n_rule = ncc_rule_annot_count(rule);

    for (size_t i = 0; i < n_nt + n_rule; i++) {
        ncc_annotation_t *a = i < n_nt ? ncc_nt_annot_at(nt, i)
                                       : ncc_rule_annot_at(rule, i - n_nt);

        if (!a) {
            continue;
        }

        int32_t ix     = ref_index(p, node, a->name_ref);
        bool    is_gap = false;
        gap_t   g      = gap_kind(a->kind, &is_gap);

        if (is_gap) {
            if (ix < 0) {
                out->dflt = g;
            }
            else if (ix < SLOTS) {
                out->slot[ix] = g;
                out->set[ix]  = true;
            }

            continue;
        }

        bool *all  = nullptr;
        bool *some = nullptr;

        switch (a->kind) {
        case NCC_ANNOT_INDENT:
            all  = &out->nest_all;
            some = out->nest;
            break;
        case NCC_ANNOT_ALIGN:
            all  = &out->align_all;
            some = out->align;
            break;
        case NCC_ANNOT_GROUP:
            all  = &out->group_all;
            some = out->group;
            break;
        default:
            continue;
        }

        if (ix < 0) {
            *all = true;
        }
        else if (ix < SLOTS) {
            some[ix] = true;
        }
    }
}

static size_t group_arity(ncc_grammar_t *g, ncc_parse_rule_t *rule, size_t i)
{
    if (!rule || i >= ncc_list_len(rule->contents)) {
        return 1;
    }

    ncc_match_t *m = &rule->contents.data[i];

    if (m->kind != NCC_MATCH_GROUP || !m->group) {
        return 1;
    }

    ncc_nonterm_t *body = ncc_get_nonterm(
        g, ((ncc_rule_group_t *)m->group)->contents_id);

    if (!body || ncc_list_len(body->rule_ids) != 1) {
        return 1;
    }

    ncc_parse_rule_t *br = ncc_get_rule(g, body->rule_ids.data[0]);
    size_t            a  = br ? ncc_list_len(br->contents) : 0;

    return a ? a : 1;
}

static gap_t gap_max(gap_t a, gap_t b)
{
    return a > b ? a : b;
}

static void splice(ncc_list_t(item_t) *out, doc_t *d, int32_t child,
                   gap_t first, gap_t rest, size_t arity)
{
    if (d->kind != D_ITEMS || !d->n) {
        item_t it = {.doc = d, .child = child, .gap = first};

        ncc_list_push(*out, it);

        return;
    }

    for (size_t j = 0; j < d->n; j++) {
        item_t it = {
            .doc   = d->kids[j],
            .child = child,
            .gap   = j % arity == 0 ? first : rest,
        };

        ncc_list_push(*out, it);
    }
}

static void wrap_child(pr_t *p, ncc_list_t(item_t) *items, int32_t which,
                       dkind_t kind)
{
    if (which < 0) {
        return;
    }

    size_t at  = items->len;
    size_t end = items->len;

    for (size_t i = 0; i < items->len; i++) {
        bool mine = which == INT32_MAX || items->data[i].child == which;

        if (mine && at == items->len) {
            at = i;
        }

        if (!mine && at != items->len) {
            end = i;
            break;
        }
    }

    if (at >= items->len) {
        return;
    }

    ncc_list_t(doc_ptr_t) body = ncc_list_new(doc_ptr_t);

    for (size_t i = at; i < end; i++) {
        if (i > at || kind != D_ALIGN) {
            ncc_list_push(body, doc_gap(p, items->data[i].gap));
        }

        ncc_list_push(body, items->data[i].doc);
    }

    item_t one = {
        .doc   = doc_wrap(p, kind, doc_seq(p, D_SEQ, body.data, body.len)),
        .child = items->data[at].child,
        .gap   = kind == D_ALIGN ? items->data[at].gap : GAP_NONE,
    };

    ncc_list_free(body);

    for (size_t i = end; i < items->len; i++) {
        items->data[at + 1 + (i - end)] = items->data[i];
    }

    items->data[at] = one;
    items->len      = at + 1 + (items->len - end);
}

static doc_t *assemble(pr_t *p, ncc_list_t(item_t) *items)
{
    ncc_list_t(doc_ptr_t) parts = ncc_list_new(doc_ptr_t);

    bool  any     = false;
    gap_t pending = GAP_NONE;
    bool  held    = false;

    for (size_t i = 0; i < items->len; i++) {
        item_t *it = &items->data[i];

        if (it->doc->empty) {
            if (any && it->gap >= GAP_SOFT) {
                pending = gap_max(pending, it->gap);
                held    = true;
            }

            continue;
        }

        if (any) {
            ncc_list_push(parts, doc_gap(p, gap_max(pending, it->gap)));
        }

        ncc_list_push(parts, it->doc);

        any     = true;
        pending = GAP_NONE;
        held    = false;
    }

    if (any && held && pending >= GAP_SOFT) {
        ncc_list_push(parts, doc_gap(p, pending));
    }

    doc_t *d = doc_seq(p, D_SEQ, parts.data, parts.len);

    ncc_list_free(parts);

    return d;
}

static doc_t *doc_text(pr_t *p, const char *s, size_t len)
{
    doc_t *d = doc_new(p, D_TEXT);

    if (!s || !len) {
        return d;
    }

    d->text = ncc_arena_alloc(&p->arena, len + 1);
    memcpy(d->text, s, len);
    d->len   = len;
    d->empty = false;

    return d;
}

static void *reduce_leaf(ncc_reduce_t *r)
{
    pr_t             *p = r->thunk;
    ncc_token_info_t *t = ncc_parse_node_token(r->node);

    if (!t) {
        return doc_new(p, D_TEXT);
    }

    const char *s   = nullptr;
    size_t      len = 0;
    char        one[2];

    if (ncc_option_is_set(t->value)) {
        ncc_string_t v = ncc_option_get(t->value);

        s   = v.data;
        len = v.u8_bytes;

        if (p->source.data && t->end_byte > t->start_byte
            && t->end_byte <= p->source.u8_bytes) {
            s   = p->source.data + t->start_byte;
            len = t->end_byte - t->start_byte;
        }
    }

    if (!s && t->tid > 0 && t->tid < 128) {
        one[0] = (char)t->tid;
        one[1] = '\0';
        s      = one;
        len    = 1;
    }

    bool any_comment = false;

    for (ncc_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
        if (tv->kind == NCC_TRIVIA_COMMENT && tv->text.data
            && tv->text.u8_bytes) {
            any_comment = true;
            break;
        }
    }

    if (!any_comment) {
        return doc_text(p, s, len);
    }

    ncc_list_t(doc_ptr_t) parts = ncc_list_new(doc_ptr_t);

    for (ncc_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
        if (tv->kind != NCC_TRIVIA_COMMENT || !tv->text.data
            || !tv->text.u8_bytes) {
            continue;
        }

        ncc_list_push(parts, doc_text(p, tv->text.data, tv->text.u8_bytes));
        ncc_list_push(parts, doc_gap(p, GAP_HARD));
    }

    ncc_list_push(parts, doc_text(p, s, len));

    doc_t *d = doc_seq(p, D_SEQ, parts.data, parts.len);

    ncc_list_free(parts);

    return d;
}

static void *reduce_node(ncc_reduce_t *r)
{
    pr_t          *p  = r->thunk;
    ncc_nt_node_t *pn = &ncc_tree_node_value(r->node);

    if (pn->group_top) {
        ncc_list_t(doc_ptr_t) flat = ncc_list_new(doc_ptr_t);

        for (size_t i = 0; i < r->n_kids; i++) {
            doc_t *k = r->kids[i];

            if (!k) {
                continue;
            }

            if (k->kind == D_ITEMS) {
                for (size_t j = 0; j < k->n; j++) {
                    ncc_list_push(flat, k->kids[j]);
                }

                continue;
            }

            ncc_list_push(flat, k);
        }

        doc_t *d = doc_seq(p, D_ITEMS, flat.data, flat.len);

        ncc_list_free(flat);

        return d;
    }

    ncc_nonterm_t    *nt   = ncc_get_nonterm(p->g, pn->id);
    ncc_parse_rule_t *rule = ncc_get_node_rule(p->g, pn);
    layout_t          lay;

    layout_of(p, r->node, nt, rule, &lay);

    ncc_list_t(item_t) items = ncc_list_new(item_t);

    for (size_t i = 0; i < r->n_kids; i++) {
        doc_t *k = r->kids[i];

        if (!k) {
            continue;
        }

        gap_t g = i < 64 && lay.set[i] ? lay.slot[i] : lay.dflt;

        splice(&items, k, (int32_t)i, g, lay.dflt,
               group_arity(p->g, rule, i));
    }

    for (int32_t i = 0; i < SLOTS; i++) {
        if (lay.nest[i]) {
            wrap_child(p, &items, i, D_NEST);
        }

        if (lay.align[i]) {
            wrap_child(p, &items, i, D_ALIGN);
        }

        if (lay.group[i]) {
            wrap_child(p, &items, i, D_GROUP);
        }
    }

    if (lay.nest_all) {
        wrap_child(p, &items, INT32_MAX, D_NEST);
    }

    if (lay.align_all) {
        wrap_child(p, &items, INT32_MAX, D_ALIGN);
    }

    if (lay.group_all) {
        wrap_child(p, &items, INT32_MAX, D_GROUP);
    }

    doc_t *d = assemble(p, &items);

    ncc_list_free(items);

    return d;
}

typedef struct {
    doc_t  *doc;
    int32_t ind;
    bool    flat;
} rframe_t;

ncc_list_decl(rframe_t);

static bool fits(doc_t *d, int32_t avail)
{
    if (avail < 0) {
        return false;
    }

    ncc_list_t(doc_ptr_t) stack = ncc_list_new(doc_ptr_t);
    bool ok = true;

    ncc_list_push(stack, d);

    while (stack.len) {
        doc_t *cur = stack.data[--stack.len];

        switch (cur->kind) {
        case D_TEXT:
            avail -= (int32_t)cur->len;

            if (avail < 0) {
                ok = false;
            }
            break;

        case D_GAP:
            if (cur->gap >= GAP_HARD) {
                ok = false;
                break;
            }

            if (cur->gap == GAP_SPACE || cur->gap == GAP_LINE) {
                avail -= 1;

                if (avail < 0) {
                    ok = false;
                }
            }
            break;

        default:
            for (size_t i = cur->n; i-- > 0;) {
                ncc_list_push(stack, cur->kids[i]);
            }
            break;
        }

        if (!ok) {
            break;
        }
    }

    ncc_list_free(stack);

    return ok;
}

static char *render(pr_t *p, doc_t *root)
{
    ncc_buffer_t *out = ncc_buffer_empty();

    ncc_list_t(rframe_t) stack = ncc_list_new(rframe_t);

    int32_t col     = 0;
    int32_t nl      = 0;
    int32_t nl_ind  = 0;
    int32_t spaces  = 0;
    bool    started = false;

    ncc_list_push(stack, ((rframe_t){.doc = root, .ind = 0, .flat = false}));

    while (stack.len) {
        rframe_t f = stack.data[--stack.len];

        switch (f.doc->kind) {
        case D_TEXT:
            if (!f.doc->len) {
                break;
            }

            if (nl) {
                for (int32_t i = 0; i < nl; i++) {
                    ncc_buffer_putc(out, '\n');
                }

                for (int32_t i = 0; i < nl_ind; i++) {
                    ncc_buffer_putc(out, ' ');
                }

                col = nl_ind;
                nl  = 0;
            }
            else {
                for (int32_t i = 0; i < spaces; i++) {
                    ncc_buffer_putc(out, ' ');
                }

                col += spaces;
            }

            spaces = 0;

            ncc_buffer_append(out, f.doc->text, f.doc->len);

            col += (int32_t)f.doc->len;
            started = true;
            break;

        case D_GAP:
            if (!started) {
                break;
            }

            if (f.flat && f.doc->gap < GAP_HARD) {
                if (f.doc->gap == GAP_SPACE || f.doc->gap == GAP_LINE) {
                    spaces = 1;
                }

                break;
            }

            switch (f.doc->gap) {
            case GAP_NONE:
                break;
            case GAP_SPACE:
                spaces = 1;
                break;
            case GAP_SOFT:
            case GAP_LINE:
            case GAP_HARD:
                nl     = nl > 1 ? nl : 1;
                nl_ind = f.ind;
                spaces = 0;
                break;
            case GAP_BLANK:
                nl     = 2;
                nl_ind = f.ind;
                spaces = 0;
                break;
            }
            break;

        case D_SEQ:
        case D_ITEMS:
            for (size_t i = f.doc->n; i-- > 0;) {
                ncc_list_push(stack, ((rframe_t){.doc  = f.doc->kids[i],
                                                 .ind  = f.ind,
                                                 .flat = f.flat}));
            }
            break;

        case D_NEST:
            ncc_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = f.ind + p->indent,
                                             .flat = f.flat}));
            break;

        case D_ALIGN: {
            int32_t at = nl ? nl_ind : col + spaces;

            ncc_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = at,
                                             .flat = f.flat}));
            break;
        }

        case D_GROUP: {
            int32_t at   = nl ? nl_ind : col + spaces;
            bool    flat = f.flat;

            if (!flat) {
                flat = !f.doc->rigid && fits(f.doc->kids[0], p->width - at);
            }

            ncc_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = f.ind,
                                             .flat = flat}));
            break;
        }
        }
    }

    for (int32_t i = 0; i < nl; i++) {
        ncc_buffer_putc(out, '\n');
    }

    ncc_list_free(stack);

    return ncc_buffer_take(out);
}

char *ncc_tree_print(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                     const ncc_print_opts_t *opts)
{
    if (!g || !tree) {
        return nullptr;
    }

    pr_t p = {
        .g      = g,
        .source = opts ? opts->source : (ncc_string_t){0},
        .width  = opts && opts->width > 0 ? opts->width : 80,
        .indent = opts && opts->indent > 0 ? opts->indent : 2,
    };

    ncc_arena_init(&p.arena, 0);

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, &p);

    ncc_walk_reduce_default(w, reduce_node, &p);
    ncc_walk_reduce_leaf(w, reduce_leaf, &p);

    doc_t *root = ncc_tree_walk_reduce(w, tree);
    char  *text = root ? render(&p, root) : nullptr;

    ncc_tree_walk_free(w);
    ncc_arena_free(&p.arena);

    return text;
}
