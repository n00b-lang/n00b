#include "parse/tree_print.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
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
    D_COMMENT,
    D_GAP,
    D_SEQ,
    D_ITEMS,
    D_NEST,
    D_TRIVIA_NEST,
    D_ALIGN,
    D_GROUP,
} dkind_t;

typedef struct doc_t doc_t;

struct doc_t {
    dkind_t  kind;
    gap_t    gap;
    core_string_t text;
    doc_t  **kids;
    size_t   n;
    size_t   source_indent;
    bool     empty;
    bool     rigid;
    bool     standalone;
};

typedef doc_t *doc_ptr_t;
core_list_decl(doc_ptr_t);

typedef struct {
    doc_t  *doc;
    int32_t child;
    gap_t   gap;
} item_t;

core_list_decl(item_t);

typedef struct {
    doc_t *doc;
    size_t next;
} fitframe_t;

core_list_decl(fitframe_t);

typedef struct {
    core_arena_t    arena;
    parse_grammar_t *g;
    core_string_t   source;
    int32_t        width;
    int32_t        indent;
    core_list_t(fitframe_t) fit;
} pr_t;

typedef struct {
    gap_t gap;
    bool  set;
    bool  nest;
    bool  trivia_indent;
    bool  align;
    bool  group;
} slot_t;

typedef struct {
    slot_t *slot;
    size_t  n;
    gap_t   dflt;
    bool    nest_all;
    bool    trivia_indent_all;
    bool    align_all;
    bool    group_all;
} layout_t;

static doc_t *doc_new(pr_t *p, dkind_t kind)
{
    doc_t *d = core_arena_alloc(&p->arena, sizeof(doc_t));

    d->kind  = kind;
    d->empty = true;

    return d;
}

static doc_t *doc_seq(pr_t *p, dkind_t kind, doc_t **kids, size_t n)
{
    doc_t *d = doc_new(p, kind);

    if (n) {
        d->kids = core_arena_alloc(&p->arena, n * sizeof(doc_t *));
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

    d->kids    = core_arena_alloc(&p->arena, sizeof(doc_t *));
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

static gap_t gap_kind(parse_annot_kind_t k, bool *is_gap)
{
    *is_gap = true;

    switch (k) {
    case PARSE_ANNOT_NOSPACE:
    case PARSE_ANNOT_CONCAT:
        return GAP_NONE;
    case PARSE_ANNOT_SPACE:
        return GAP_SPACE;
    case PARSE_ANNOT_SOFTLINE:
        return GAP_SOFT;
    case PARSE_ANNOT_NEWLINE:
        return GAP_LINE;
    case PARSE_ANNOT_HARDLINE:
        return GAP_HARD;
    case PARSE_ANNOT_BLANKLINE:
        return GAP_BLANK;
    default:
        *is_gap = false;

        return GAP_NONE;
    }
}

static int32_t ref_index(pr_t *p, parse_tree_t *node, parse_child_ref_t ref)
{
    if (!parse_child_ref_is_set(ref)) {
        return -1;
    }

    if (ref.kind == PARSE_ROLE_BY_INDEX) {
        return ref.index;
    }

    parse_nonterm_t *want = parse_find_nonterm(p->g, ref.name);

    if (!want) {
        return -1;
    }

    size_t n = core_tree_num_children(node);

    for (size_t i = 0; i < n; i++) {
        parse_tree_t *child = core_tree_child(node, i);

        if (core_tree_is_leaf(child)) {
            continue;
        }

        parse_nt_node_t *cpn = &core_tree_node_value(child);

        if (!cpn->group_top && cpn->id == want->id) {
            return (int32_t)i;
        }
    }

    return -1;
}

static void layout_of(pr_t *p, parse_tree_t *node, parse_nonterm_t *nt,
                      parse_rule_t *rule, layout_t *out)
{
    memset(out, 0, sizeof(*out));

    out->n    = core_tree_num_children(node);
    out->slot = out->n ? core_arena_alloc(&p->arena, out->n * sizeof(slot_t))
                       : nullptr;
    out->dflt = GAP_SPACE;

    size_t n_nt   = parse_nt_annot_count(nt);
    size_t n_rule = parse_rule_annot_count(rule);

    for (size_t i = 0; i < n_nt + n_rule; i++) {
        parse_annotation_t *a = i < n_nt ? parse_nt_annot_at(nt, i)
                                       : parse_rule_annot_at(rule, i - n_nt);

        if (!a) {
            continue;
        }

        int32_t ix    = ref_index(p, node, a->name_ref);
        bool    named = ix >= 0 && (size_t)ix < out->n;
        bool    all   = ix < 0;
        bool    is_gap = false;
        gap_t   g      = gap_kind(a->kind, &is_gap);

        if (is_gap) {
            if (all) {
                out->dflt = g;
            }
            else if (named) {
                out->slot[ix].gap = g;
                out->slot[ix].set = true;
            }

            continue;
        }

        switch (a->kind) {
        case PARSE_ANNOT_INDENT:
            if (all) {
                out->nest_all = true;
            }
            else if (named) {
                out->slot[ix].nest = true;
            }
            break;
        case PARSE_ANNOT_TRIVIA_INDENT:
            if (all) {
                out->trivia_indent_all = true;
            }
            else if (named) {
                out->slot[ix].trivia_indent = true;
            }
            break;
        case PARSE_ANNOT_ALIGN:
            if (all) {
                out->align_all = true;
            }
            else if (named) {
                out->slot[ix].align = true;
            }
            break;
        case PARSE_ANNOT_GROUP:
            if (all) {
                out->group_all = true;
            }
            else if (named) {
                out->slot[ix].group = true;
            }
            break;
        default:
            break;
        }
    }
}

static size_t group_arity(parse_grammar_t *g, parse_rule_t *rule, size_t i)
{
    if (!rule || i >= core_list_len(rule->contents)) {
        return 1;
    }

    parse_match_t *m = &rule->contents.data[i];

    if (m->kind != PARSE_MATCH_GROUP || !m->group) {
        return 1;
    }

    parse_nonterm_t *body = parse_get_nonterm(
        g, ((parse_rule_group_t *)m->group)->contents_id);

    if (!body || core_list_len(body->rule_ids) != 1) {
        return 1;
    }

    parse_rule_t *br = parse_get_rule(g, body->rule_ids.data[0]);
    size_t            a  = br ? core_list_len(br->contents) : 0;

    return a ? a : 1;
}

static gap_t gap_max(gap_t a, gap_t b)
{
    return a > b ? a : b;
}

static void splice(core_list_t(item_t) *out, doc_t *d, int32_t child,
                   gap_t first, gap_t rest, size_t arity)
{
    if (d->kind != D_ITEMS || !d->n) {
        item_t it = {.doc = d, .child = child, .gap = first};

        core_list_push(*out, it);

        return;
    }

    for (size_t j = 0; j < d->n; j++) {
        item_t it = {
            .doc   = d->kids[j],
            .child = child,
            .gap   = j % arity == 0 ? first : rest,
        };

        core_list_push(*out, it);
    }
}

static size_t wrap_child(pr_t *p, core_list_t(item_t) *items, int32_t which,
                         dkind_t kind, size_t from)
{
    if (which < 0) {
        return from;
    }

    bool   all = which == INT32_MAX;
    size_t at  = items->len;
    size_t end = items->len;

    for (size_t i = all ? 0 : from; i < items->len; i++) {
        bool mine = all || items->data[i].child == which;

        if (mine && at == items->len) {
            at = i;
        }

        if (!mine && at != items->len) {
            end = i;
            break;
        }

        if (!all && at == items->len && items->data[i].child > which) {
            break;
        }
    }

    if (at >= items->len) {
        return from;
    }

    core_list_t(doc_ptr_t) body = core_list_new(doc_ptr_t);

    for (size_t i = at; i < end; i++) {
        if (i > at || kind != D_ALIGN) {
            core_list_push(body, doc_gap(p, items->data[i].gap));
        }

        core_list_push(body, items->data[i].doc);
    }

    item_t one = {
        .doc   = doc_wrap(p, kind, doc_seq(p, D_SEQ, body.data, body.len)),
        .child = items->data[at].child,
        .gap   = kind == D_ALIGN ? items->data[at].gap : GAP_NONE,
    };

    core_list_free(body);

    for (size_t i = end; i < items->len; i++) {
        items->data[at + 1 + (i - end)] = items->data[i];
    }

    items->data[at] = one;
    items->len      = at + 1 + (items->len - end);

    return at;
}

static doc_t *assemble(pr_t *p, core_list_t(item_t) *items)
{
    core_list_t(doc_ptr_t) parts = core_list_new(doc_ptr_t);

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
            core_list_push(parts, doc_gap(p, gap_max(pending, it->gap)));
        }

        core_list_push(parts, it->doc);

        any     = true;
        pending = GAP_NONE;
        held    = false;
    }

    if (any && held && pending >= GAP_SOFT) {
        core_list_push(parts, doc_gap(p, pending));
    }

    doc_t *d = doc_seq(p, D_SEQ, parts.data, parts.len);

    core_list_free(parts);

    return d;
}

static doc_t *doc_text(pr_t *p, core_string_t s)
{
    doc_t *d = doc_new(p, D_TEXT);

    if (!s.data || !s.u8_bytes) {
        return d;
    }

    d->text.data = core_arena_alloc(&p->arena, s.u8_bytes);
    memcpy(d->text.data, s.data, s.u8_bytes);
    d->text.u8_bytes = s.u8_bytes;
    d->empty         = false;

    return d;
}

static gap_t
trivia_gap(core_string_t text)
{
    size_t newlines = 0;

    for (size_t i = 0; text.data && i < text.u8_bytes; i++) {
        if (text.data[i] == '\n') {
            newlines++;
        }
    }

    return newlines > 1 ? GAP_BLANK : newlines ? GAP_HARD : GAP_SPACE;
}

static bool
has_comment(parse_trivia_t *trivia)
{
    for (parse_trivia_t *item = trivia; item; item = item->next) {
        if (item->kind == PARSE_TRIVIA_COMMENT && item->text.data
            && item->text.u8_bytes) {
            return true;
        }
    }

    return false;
}

static doc_t *
doc_comment(pr_t *p, parse_trivia_t *item)
{
    doc_t *d = doc_text(p, item->text);
    d->kind = D_COMMENT;
    if (!p->source.data || item->end_byte > p->source.u8_bytes
        || item->start_byte >= item->end_byte
        || item->text.u8_bytes > item->end_byte - item->start_byte)
        return d;

    size_t comment_start = item->start_byte;
    while (comment_start <= item->end_byte - item->text.u8_bytes
           && memcmp(p->source.data + comment_start, item->text.data,
                     item->text.u8_bytes)) comment_start++;
    if (comment_start > item->end_byte - item->text.u8_bytes) return d;
    size_t comment_end = comment_start + item->text.u8_bytes;
    size_t line_start = comment_start;
    while (line_start && p->source.data[line_start - 1] != '\n')
        line_start--;
    size_t line_end = comment_end;
    while (line_end < p->source.u8_bytes
           && p->source.data[line_end] != '\n') line_end++;
    bool standalone = true;
    for (size_t at = line_start; at < comment_start; at++)
        if (p->source.data[at] != ' ' && p->source.data[at] != '\t')
            standalone = false;
    for (size_t at = comment_end; at < line_end; at++)
        if (p->source.data[at] != ' ' && p->source.data[at] != '\t'
            && p->source.data[at] != '\r') standalone = false;
    d->standalone = standalone;
    d->source_indent = comment_start - line_start;
    d->rigid = standalone;
    return d;
}

static void
put_leading_trivia(pr_t *p, core_list_t(doc_ptr_t) *parts,
                   parse_trivia_t *trivia)
{
    gap_t before = GAP_NONE;

    for (parse_trivia_t *item = trivia; item; item = item->next) {
        if (item->kind == PARSE_TRIVIA_SPACE) {
            before = trivia_gap(item->text);
            continue;
        }

        if (item->kind != PARSE_TRIVIA_COMMENT || !item->text.data
            || !item->text.u8_bytes) {
            continue;
        }

        doc_t *comment = doc_comment(p, item);
        if (before != GAP_NONE || comment->standalone)
            core_list_push(*parts, doc_gap(p,
                comment->standalone ? gap_max(before, GAP_HARD) : before));
        core_list_push(*parts, comment);
        before = GAP_HARD;
    }

    if (before != GAP_NONE) {
        core_list_push(*parts, doc_gap(p, before));
    }
}

static void
put_trailing_trivia(pr_t *p, core_list_t(doc_ptr_t) *parts,
                    parse_trivia_t *trivia)
{
    gap_t before = GAP_SPACE;

    for (parse_trivia_t *item = trivia; item; item = item->next) {
        if (item->kind == PARSE_TRIVIA_SPACE) {
            before = trivia_gap(item->text);
            continue;
        }

        if (item->kind != PARSE_TRIVIA_COMMENT || !item->text.data
            || !item->text.u8_bytes) {
            continue;
        }

        doc_t *comment = doc_comment(p, item);
        core_list_push(*parts, doc_gap(p,
            comment->standalone ? gap_max(before, GAP_HARD) : before));
        core_list_push(*parts, comment);
        before = GAP_HARD;
    }

    core_list_push(*parts, doc_gap(p, GAP_HARD));
}

static void *reduce_leaf(parse_reduce_t *r)
{
    pr_t             *p = r->thunk;
    parse_token_info_t *t = parse_node_token(r->node);

    if (!t) {
        return doc_new(p, D_TEXT);
    }

    core_string_t s = {};
    char          one;

    core_option_t(core_string_t) value = parse_token_value(t);
    size_t start = parse_token_start_byte(t);
    size_t end   = parse_token_end_byte(t);
    int32_t tid  = parse_token_tid(t);

    if (core_option_is_set(value)) {
        s = core_option_get(value);

        if (p->source.data && end > start && end <= p->source.u8_bytes) {
            s = core_string_slice(p->source, start, end - start);
        }
    }

    if (!s.data && !core_option_is_set(value) && tid > 0 && tid < 128) {
        one = (char)tid;
        s   = core_string_borrow(&one, 1);
    }

    bool leading_comments  = has_comment(parse_token_leading_trivia(t));
    bool trailing_comments = has_comment(parse_token_trailing_trivia(t));

    if (!leading_comments && !trailing_comments) {
        return doc_text(p, s);
    }

    core_list_t(doc_ptr_t) parts = core_list_new(doc_ptr_t);

    if (leading_comments) {
        put_leading_trivia(p, &parts, parse_token_leading_trivia(t));
    }

    core_list_push(parts, doc_text(p, s));

    if (trailing_comments) {
        put_trailing_trivia(p, &parts, parse_token_trailing_trivia(t));
    }

    doc_t *d = doc_seq(p, D_SEQ, parts.data, parts.len);

    core_list_free(parts);

    return d;
}

static void *reduce_node(parse_reduce_t *r)
{
    pr_t          *p  = r->thunk;
    parse_nt_node_t *pn = &core_tree_node_value(r->node);

    if (pn->group_top) {
        core_list_t(doc_ptr_t) flat = core_list_new(doc_ptr_t);

        for (size_t i = 0; i < r->n_kids; i++) {
            doc_t *k = r->kids[i];

            if (!k) {
                continue;
            }

            if (k->kind == D_ITEMS) {
                for (size_t j = 0; j < k->n; j++) {
                    core_list_push(flat, k->kids[j]);
                }

                continue;
            }

            core_list_push(flat, k);
        }

        doc_t *d = doc_seq(p, D_ITEMS, flat.data, flat.len);

        core_list_free(flat);

        return d;
    }

    parse_nonterm_t    *nt   = parse_get_nonterm(p->g, pn->id);
    parse_rule_t *rule = parse_get_node_rule(p->g, pn);
    layout_t          lay;

    layout_of(p, r->node, nt, rule, &lay);

    core_list_t(item_t) items = core_list_new(item_t);

    for (size_t i = 0; i < r->n_kids; i++) {
        doc_t *k = r->kids[i];

        if (!k) {
            continue;
        }

        gap_t g = i < lay.n && lay.slot[i].set ? lay.slot[i].gap : lay.dflt;

        splice(&items, k, (int32_t)i, g, lay.dflt,
               group_arity(p->g, rule, i));
    }

    size_t from = 0;

    for (size_t i = 0; i < lay.n; i++) {
        if (lay.slot[i].nest) {
            from = wrap_child(p, &items, (int32_t)i, D_NEST, from);
        }

        if (lay.slot[i].trivia_indent) {
            from = wrap_child(p, &items, (int32_t)i, D_TRIVIA_NEST, from);
        }

        if (lay.slot[i].align) {
            from = wrap_child(p, &items, (int32_t)i, D_ALIGN, from);
        }

        if (lay.slot[i].group) {
            from = wrap_child(p, &items, (int32_t)i, D_GROUP, from);
        }
    }

    if (lay.nest_all) {
        wrap_child(p, &items, INT32_MAX, D_NEST, 0);
    }

    if (lay.trivia_indent_all) {
        wrap_child(p, &items, INT32_MAX, D_TRIVIA_NEST, 0);
    }

    if (lay.align_all) {
        wrap_child(p, &items, INT32_MAX, D_ALIGN, 0);
    }

    if (lay.group_all) {
        wrap_child(p, &items, INT32_MAX, D_GROUP, 0);
    }

    doc_t *d = assemble(p, &items);

    core_list_free(items);

    return d;
}

typedef struct {
    doc_t  *doc;
    int32_t ind;
    int32_t trivia_indent;
    bool    flat;
} rframe_t;

core_list_decl(rframe_t);

static bool fits(pr_t *p, doc_t *d, int32_t avail)
{
    if (avail < 0) {
        return false;
    }

    bool ok = true;

    p->fit.len = 0;

    core_list_push(p->fit, ((fitframe_t){.doc = d}));

    while (ok && p->fit.len) {
        fitframe_t *f = &p->fit.data[p->fit.len - 1];

        if (f->next == 0) {
            if (f->doc->kind == D_TEXT || f->doc->kind == D_COMMENT) {
                avail -= (int32_t)f->doc->text.u8_bytes;
                ok = avail >= 0;
                p->fit.len--;

                continue;
            }

            if (f->doc->kind == D_GAP) {
                if (f->doc->gap >= GAP_HARD) {
                    ok = false;
                }
                else if (f->doc->gap == GAP_SPACE
                         || f->doc->gap == GAP_LINE) {
                    avail -= 1;
                    ok = avail >= 0;
                }

                p->fit.len--;

                continue;
            }
        }

        if (f->next < f->doc->n) {
            doc_t *kid = f->doc->kids[f->next++];

            core_list_push(p->fit, ((fitframe_t){.doc = kid}));

            continue;
        }

        p->fit.len--;
    }

    return ok;
}

static core_string_t render(pr_t *p, doc_t *root)
{
    core_buffer_t *out = core_buffer_empty();

    core_list_t(rframe_t) stack = core_list_new(rframe_t);

    int32_t col     = 0;
    int32_t nl      = 0;
    int32_t nl_ind  = 0;
    int32_t spaces  = 0;
    bool    started = false;

    core_list_push(stack, ((rframe_t){.doc = root, .ind = 0, .flat = false}));

    while (stack.len) {
        rframe_t f = stack.data[--stack.len];

        switch (f.doc->kind) {
        case D_TEXT:
        case D_COMMENT:
            if (!f.doc->text.u8_bytes) {
                break;
            }

            if (nl) {
                for (int32_t i = 0; i < nl; i++) {
                    core_buffer_putc(out, '\n');
                }

                int32_t indent = f.doc->kind == D_COMMENT
                    && f.doc->standalone
                    ? f.ind + f.trivia_indent : nl_ind;
                for (int32_t i = 0; i < indent; i++) {
                    core_buffer_putc(out, ' ');
                }

                col = indent;
                nl  = 0;
            }
            else {
                for (int32_t i = 0; i < spaces; i++) {
                    core_buffer_putc(out, ' ');
                }

                col += spaces;
            }

            spaces = 0;

            if (f.doc->kind == D_COMMENT && f.doc->standalone) {
                int32_t       base = f.ind + f.trivia_indent;
                core_string_t text = f.doc->text;
                size_t        at   = 0;
                while (at < text.u8_bytes) {
                    size_t end = at;
                    while (end < text.u8_bytes && text.data[end] != '\n')
                        end++;
                    core_buffer_append(out, text.data + at, end - at);
                    col += (int32_t)(end - at);
                    if (end == text.u8_bytes) break;
                    core_buffer_putc(out, '\n');
                    col = 0;
                    at = end + 1;
                    size_t original = 0;
                    while (at < text.u8_bytes && text.data[at] == ' ') {
                        original++;
                        at++;
                    }
                    if (at < text.u8_bytes && text.data[at] != '\n') {
                        size_t relative = original > f.doc->source_indent
                            ? original - f.doc->source_indent : 0;
                        for (size_t i = 0; i < (size_t)base + relative; i++)
                            core_buffer_putc(out, ' ');
                        col = base + (int32_t)relative;
                    }
                }
            }
            else {
                core_buffer_append_string(out, f.doc->text);
                col += (int32_t)f.doc->text.u8_bytes;
            }
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
                core_list_push(stack, ((rframe_t){.doc  = f.doc->kids[i],
                                                 .ind  = f.ind,
                                                 .trivia_indent = f.trivia_indent,
                                                 .flat = f.flat}));
            }
            break;

        case D_NEST:
            core_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = f.ind + p->indent,
                                             .trivia_indent = f.trivia_indent,
                                             .flat = f.flat}));
            break;

        case D_TRIVIA_NEST:
            /* Keep delimiters in place while indenting their comments. */
            core_list_push(stack, ((rframe_t){
                .doc = f.doc->kids[0], .ind = f.ind,
                .trivia_indent = f.trivia_indent + p->indent,
                .flat = f.flat}));
            break;

        case D_ALIGN: {
            int32_t at = nl ? nl_ind : col + spaces;

            core_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = at,
                                             .trivia_indent = f.trivia_indent,
                                             .flat = f.flat}));
            break;
        }

        case D_GROUP: {
            int32_t at   = nl ? nl_ind : col + spaces;
            bool    flat = f.flat;

            if (!flat) {
                flat = !f.doc->rigid
                    && fits(p, f.doc->kids[0], p->width - at);
            }

            core_list_push(stack, ((rframe_t){.doc  = f.doc->kids[0],
                                             .ind  = f.ind,
                                             .trivia_indent = f.trivia_indent,
                                             .flat = flat}));
            break;
        }
        }
    }

    for (int32_t i = 0; i < nl; i++) {
        core_buffer_putc(out, '\n');
    }

    core_list_free(stack);

    return core_buffer_take_string(out);
}

core_string_t parse_tree_print(parse_grammar_t *g, parse_tree_t *tree,
                               const parse_print_opts_t *opts)
{
    if (!g || !tree) {
        return (core_string_t){};
    }

    pr_t p = {
        .g      = g,
        .source = opts ? opts->source : (core_string_t){0},
        .width  = opts && opts->width > 0 ? opts->width : 80,
        .indent = opts && opts->indent > 0 ? opts->indent : 2,
    };

    core_arena_init(&p.arena, 0);
    p.fit = core_list_new_cap(fitframe_t, 64);

    parse_tree_walk_t *w = parse_tree_walk_new(g, &p);

    parse_walk_reduce_default(w, reduce_node, &p);
    parse_walk_reduce_leaf(w, reduce_leaf, &p);

    doc_t *root = parse_tree_walk_reduce(w, tree);
    core_string_t text = root ? render(&p, root) : (core_string_t){};

    parse_tree_walk_free(w);
    core_list_free(p.fit);
    core_arena_free(&p.arena);

    return text;
}
