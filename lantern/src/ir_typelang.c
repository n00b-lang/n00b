#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/typelang.h"

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/scanner.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include "lib/dict.h"
#include "lib/buffer.h"

#include "typelang_bnf.h"

static char *msg(const char *what)
{
    size_t n = strlen(what) + 12;
    char  *p = malloc(n);

    snprintf(p, n, "typelang: %s", what);

    return p;
}

static ncc_grammar_t *grammar;
static int64_t        tid_ident;
static int64_t        tid_int;
static int64_t        tid_str;

static int64_t lookup(ncc_grammar_t *g, const char *name)
{
    return ncc_lookup_terminal(g, ncc_string_from_cstr(name));
}

static ncc_grammar_t *load_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);
    ncc_register_terminal(g, ncc_string_from_cstr("INT_LIT"));
    ncc_register_terminal(g, ncc_string_from_cstr("STRING_LIT"));

    if (!ncc_bnf_load(ncc_string_from_cstr(TYPELANG_BNF),
                      ncc_string_from_cstr("union-type"), g)) {
        ncc_grammar_free(g);

        return nullptr;
    }

    tid_ident = NCC_TOK_IDENTIFIER;
    tid_int   = lookup(g, "INT_LIT");
    tid_str   = lookup(g, "STRING_LIT");

    grammar = g;

    return grammar;
}

void ir_typelang_shutdown(void)
{
    if (grammar) {
        ncc_grammar_free(grammar);
        grammar = nullptr;
    }
}

static bool after_tick(ncc_scanner_t *s)
{
    ncc_token_info_t *prev = ncc_scan_prev_token(s);

    return prev && prev->tid == '`';
}

static bool tl_scan(ncc_scanner_t *s)
{
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_emit(s, (int32_t)tid_str, ncc_scan_string_double(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        ncc_scan_emit(s, (int32_t)tid_int, ncc_scan_integer(s));

        return true;
    }

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80) {
        bool                       name = after_tick(s);
        ncc_option_t(ncc_string_t) id   = ncc_scan_identifier(s);

        if (ncc_option_is_set(id)) {
            int64_t kw = name
                           ? NCC_TOK_OTHER
                           : ncc_scan_terminal_id(
                               s, ncc_option_get(id).data);

            ncc_scan_emit(s, kw != NCC_TOK_OTHER ? (int32_t)kw
                                                 : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    ncc_codepoint_t next = ncc_scan_peek(s, 1);

    if (next > 0 && next < 128 && !isspace((int)next)) {
        char two[3] = {(char)c, (char)next, 0};

        int64_t op = ncc_scan_terminal_id(s, two);

        if (op != NCC_TOK_OTHER && op > 0xFF) {
            ncc_scan_advance_n(s, 2);
            ncc_scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);

    return true;
}

typedef enum {
    TL_TYPE,
    TL_NAME,
    TL_ARG,
    TL_MEMBER,
    TL_PARAM,
    TL_ATTR,
    TL_MODE,
    TL_INT,
} tl_kind;

typedef struct {
    tl_kind kind;
    union {
        th_type   *type;
        char      *name;
        th_arg     arg;
        th_member  member;
        uint8_t    attr;
        th_mode    mode;
        uint64_t   u;
        struct {
            th_mode     mode;
            const char *name;
            th_type    *type;
            bool        is_rest;
        } param;
    };
} tl_node;

typedef struct {
    char    *name;
    th_type *type;
} freev;

typedef struct {
    char **names;
    size_t n;
} binding;

typedef struct {
    th_ctx        *th;
    ncc_grammar_t *g;
    const char    *err;

    freev     *free_vars;
    size_t     nfree;
    size_t     cfree;
    ncc_dict_t fv_index;

    binding *binders;
    size_t   nb;
    size_t   cb;
} bctx;

static tl_node *node_new(bctx *c, tl_kind kind)
{
    (void)c;

    tl_node *n = calloc(1, sizeof(*n));

    n->kind = kind;

    return n;
}

static tl_node *node_type(bctx *c, th_type *t)
{
    tl_node *n = node_new(c, TL_TYPE);

    n->type = t;

    return n;
}

static th_type *as_type(void *v)
{
    tl_node *n = v;

    return n && n->kind == TL_TYPE ? n->type : nullptr;
}

static int32_t rule_of(ncc_parse_tree_t *n)
{
    return ncc_tree_is_leaf(n) ? -1 : ncc_tree_node_value(n).rule_index;
}

static tl_node *kid(ncc_reduce_t *r, size_t i)
{
    return i < r->n_kids ? r->kids[i] : nullptr;
}

static th_type *kid_type(ncc_reduce_t *r, size_t i)
{
    return as_type(kid(r, i));
}

static void *r_pass(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_paren_form(ncc_reduce_t *r)
{
    return r->n_kids == 3 ? kid(r, 1) : kid(r, 0);
}

static void *r_sum(ncc_reduce_t *r, th_comb comb)
{
    bctx    *c    = r->thunk;
    th_type *head = kid_type(r, 0);

    if (r->n_kids == 1 || !head) {
        return kid(r, 0);
    }

    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));

    if (!rest) {
        return kid(r, 0);
    }

    th_type *arms[64];
    size_t   n = 0;

    arms[n++] = head;

    for (size_t i = 0; i < rest->n && n < 64; i++) {
        th_type *t = as_type(rest->v[i]);

        if (t) {
            arms[n++] = t;
        }
    }

    return node_type(c, n == 1 ? arms[0] : th_sum(c->th, comb, arms, n));
}

static void *r_union(ncc_reduce_t *r)
{
    return r_sum(r, TH_UNION);
}

static void *r_isect(ncc_reduce_t *r)
{
    return r_sum(r, TH_INTERSECTION);
}

static void *r_qualified_name(ncc_reduce_t *r)
{
    bctx  *c = r->thunk;
    char   buf[512];
    size_t len = 0;

    ncc_string_t first = ncc_tree_leaf_text(ncc_tree_child(r->node, 0));

    if (first.data) {
        len = first.u8_bytes < sizeof(buf) ? first.u8_bytes : 0;
        memcpy(buf, first.data, len);
    }

    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));

    if (rest) {
        for (size_t i = 0; i < rest->n; i++) {
            tl_node *part = rest->v[i];

            if (!part || part->kind != TL_NAME) {
                continue;
            }

            size_t pl = strlen(part->name);

            if (len + pl + 1 >= sizeof(buf)) {
                break;
            }

            buf[len++] = '.';
            memcpy(buf + len, part->name, pl);
            len += pl;
        }
    }

    buf[len]     = 0;

    tl_node *n = node_new(c, TL_NAME);

    n->name = strdup(buf);

    return n;
}

static void *r_type_arg(ncc_reduce_t *r)
{
    bctx    *c    = r->thunk;
    tl_node *n    = node_new(c, TL_ARG);
    tl_node *only = kid(r, 0);

    if (only && only->kind == TL_INT) {
        n->arg.kind = TH_ARG_UINT;
        n->arg.u    = only->u;

        return n;
    }

    if (only && only->kind == TL_NAME) {
        n->arg.kind  = TH_ARG_STR;
        n->arg.str.p = only->name;
        n->arg.str.n = strlen(only->name);

        return n;
    }

    n->arg.kind = TH_ARG_TYPE;
    n->arg.type = as_type(only);

    return n;
}

static size_t collect_args(ncc_reduce_t *r, th_arg *out, size_t max)
{
    size_t   n     = 0;
    tl_node *first = kid(r, 0);

    if (first && first->kind == TL_ARG && n < max) {
        out[n++] = first->arg;
    }

    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));

    if (rest) {
        for (size_t i = 0; i < rest->n && n < max; i++) {
            tl_node *a = rest->v[i];

            if (a && a->kind == TL_ARG) {
                out[n++] = a->arg;
            }
        }
    }

    return n;
}

static void *r_type_app(ncc_reduce_t *r)
{
    return kid(r, 1);
}

static void *r_named_type(ncc_reduce_t *r)
{
    bctx    *c       = r->thunk;
    tl_node *nm      = nullptr;
    bool     variant = false;
    size_t   at      = 0;

    ncc_reduce_list_t *lead = ncc_reduce_as_list(kid(r, 0));

    if (lead) {
        variant = lead->n > 0;
        at      = 1;
    }

    nm = kid(r, at);

    if (!nm || nm->kind != TL_NAME) {
        c->err = "a named type needs a name";

        return nullptr;
    }

    th_arg args[64];
    size_t nargs = 0;

    ncc_reduce_list_t *app = ncc_reduce_as_list(kid(r, at + 1));

    for (size_t i = 0; app && i < app->n; i++) {
        ncc_reduce_list_t *list = ncc_reduce_as_list(app->v[i]);

        for (size_t j = 0; list && j < list->n && nargs < 64; j++) {
            tl_node *a = list->v[j];

            if (a && a->kind == TL_ARG) {
                args[nargs++] = a->arg;
            }
        }
    }

    if (variant) {
        return node_type(c, th_variant(c->th, nm->name, args, nargs));
    }

    return node_type(c, nargs ? th_apply(c->th, nm->name, args, nargs)
                              : th_name(c->th, nm->name));
}

static void *r_type_args(ncc_reduce_t *r)
{
    bctx    *c = r->thunk;
    th_arg   args[64];
    size_t   n = collect_args(r, args, 64);
    tl_node *out = node_new(c, TL_ARG);

    ncc_reduce_list_t *list = ncc_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        tl_node *a = node_new(c, TL_ARG);

        a->arg     = args[i];
        list->v[i] = a;
    }

    free(out);

    return list;
}

static void *r_tvar(ncc_reduce_t *r)
{
    bctx        *c    = r->thunk;
    ncc_string_t name = ncc_tree_leaf_text(ncc_tree_child(r->node, 1));

    if (!name.data) {
        c->err = "a type variable needs a name";

        return nullptr;
    }

    for (size_t d = c->nb; d > 0; d--) {
        for (size_t i = 0; i < c->binders[d - 1].n; i++) {
            if (!strcmp(c->binders[d - 1].names[i], name.data)) {
                return node_type(c, th_bound(c->th, c->nb - d + 1, i));
            }
        }
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)ncc_dict_get(&c->fv_index, name.data,
                                                   &found);

    if (found) {
        return node_type(c, c->free_vars[at].type);
    }

    if (c->nfree >= c->cfree) {
        c->cfree     = c->cfree ? c->cfree * 2 : 8;
        c->free_vars = realloc(c->free_vars, c->cfree * sizeof(freev));
    }

    th_type *t = th_var(c->th);

    c->free_vars[c->nfree].name = strdup(name.data);
    c->free_vars[c->nfree].type = t;
    ncc_dict_put(&c->fv_index, c->free_vars[c->nfree].name, c->nfree);
    c->nfree++;

    return node_type(c, t);
}

static void collect_tvar_names(ncc_grammar_t *g, ncc_parse_tree_t *n,
                               char **names, size_t *count, size_t max)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }

    if (ncc_tree_is_nt(g, n, "one-type")) {
        return;
    }

    if (ncc_tree_is_nt(g, n, "tvar")) {
        ncc_string_t s = ncc_tree_leaf_text(ncc_tree_child(n, 1));

        if (s.data && *count < max) {
            names[(*count)++] = strdup(s.data);
        }

        return;
    }

    size_t nc = ncc_tree_num_children(n);

    for (size_t i = 0; i < nc; i++) {
        collect_tvar_names(g, ncc_tree_child(n, i), names, count, max);
    }
}

static bool binder_open(ncc_visit_t *v)
{
    bctx  *c = v->user;
    char **names = calloc(64, sizeof(char *));
    size_t n     = 0;

    collect_tvar_names(v->grammar, v->node, names, &n, 64);

    if (c->nb >= c->cb) {
        c->cb      = c->cb ? c->cb * 2 : 4;
        c->binders = realloc(c->binders, c->cb * sizeof(binding));
    }

    c->binders[c->nb].names = names;
    c->binders[c->nb].n     = n;
    c->nb++;

    return true;
}

static void *r_binder(ncc_reduce_t *r)
{
    bctx    *c    = r->thunk;
    th_type *body = kid_type(r, r->n_kids - 1);
    size_t   n    = c->nb ? c->binders[c->nb - 1].n : 0;

    if (c->nb) {
        for (size_t i = 0; i < n; i++) {
            free(c->binders[c->nb - 1].names[i]);
        }

        free(c->binders[c->nb - 1].names);
        c->nb--;
    }

    if (!body) {
        c->err = "forall needs a body";

        return nullptr;
    }

    return node_type(c, th_binder(c->th, n, body));
}

static void *r_bitint(ncc_reduce_t *r)
{
    bctx    *c        = r->thunk;
    bool     is_signed = ncc_tree_leaf_is(ncc_tree_child(r->node, 0), "bitint");
    tl_node *width     = kid(r, 2);

    return node_type(c, th_bitint(c->th, width ? width->u : 0, is_signed));
}

static void *r_ind_attr(ncc_reduce_t *r)
{
    static const struct {
        const char *name;
        uint8_t     bit;
    } map[] = {
        {"nullable",   TH_NULLABLE  },
        {"reseatable", TH_RESEATABLE},
        {"indexable",  TH_INDEXABLE },
        {"owning",     TH_OWNING    },
        {"exclusive",  TH_EXCLUSIVE },
        {"temporary",  TH_TEMPORARY },
    };

    bctx        *c = r->thunk;
    ncc_string_t s = ncc_tree_leaf_text(ncc_tree_child(r->node, 0));
    tl_node     *n = node_new(c, TL_ATTR);

    for (size_t i = 0; s.data && i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(s.data, map[i].name)) {
            n->attr = map[i].bit;
            break;
        }
    }

    return n;
}

static void *r_ind_attrs(ncc_reduce_t *r)
{
    bctx    *c = r->thunk;
    tl_node *n = node_new(c, TL_ATTR);
    tl_node *first = kid(r, 0);

    n->attr = first && first->kind == TL_ATTR ? first->attr : 0;

    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));

    if (rest) {
        for (size_t i = 0; i < rest->n; i++) {
            tl_node *a = rest->v[i];

            if (a && a->kind == TL_ATTR) {
                n->attr |= a->attr;
            }
        }
    }

    return n;
}

static uint8_t sugar_attrs(const char *w)
{
    if (!strcmp(w, "ptr")) {
        return TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE;
    }
    if (!strcmp(w, "mutref")) {
        return TH_EXCLUSIVE;
    }
    if (!strcmp(w, "own")) {
        return TH_OWNING;
    }
    if (!strcmp(w, "gcref")) {
        return TH_NULLABLE | TH_RESEATABLE;
    }
    if (!strcmp(w, "tmpref")) {
        return TH_TEMPORARY;
    }

    return 0;
}

static void *r_ind_tail(ncc_reduce_t *r)
{
    bctx              *c    = r->thunk;
    ncc_reduce_list_t *opt  = ncc_reduce_as_list(kid(r, 1));
    tl_node           *n    = node_new(c, TL_ATTR);

    for (size_t i = 0; opt && i < opt->n; i++) {
        tl_node *a = opt->v[i];

        if (a && a->kind == TL_ATTR) {
            n->attr |= a->attr;
        }
    }

    return n;
}

static void *r_indirection(ncc_reduce_t *r)
{
    bctx        *c    = r->thunk;
    ncc_string_t name = ncc_tree_leaf_text(
        ncc_tree_child(ncc_tree_child(r->node, 0), 0));
    th_type *referent = kid_type(r, 2);

    if (!referent) {
        c->err = "an indirection needs a referent";

        return nullptr;
    }

    uint8_t            attrs = name.data ? sugar_attrs(name.data) : 0;
    ncc_reduce_list_t *tail  = ncc_reduce_as_list(kid(r, 3));

    for (size_t i = 0; tail && i < tail->n; i++) {
        tl_node *a = tail->v[i];

        if (a && a->kind == TL_ATTR) {
            attrs = a->attr;
        }
    }

    return node_type(c, th_ind(c->th, attrs, referent));
}

static void *r_sequence(ncc_reduce_t *r)
{
    bctx    *c    = r->thunk;
    th_type *elem = kid_type(r, 2);

    if (!elem) {
        return nullptr;
    }

    ncc_reduce_list_t *tail = ncc_reduce_as_list(kid(r, 3));

    if (tail) {
        for (size_t i = 0; i < tail->n; i++) {
            tl_node *e = tail->v[i];

            if (e && e->kind == TL_INT) {
                return node_type(c, th_array_n(c->th, elem, e->u));
            }
        }
    }

    return node_type(c, th_array(c->th, elem));
}

static void *r_anon_site(ncc_reduce_t *r)
{
    bctx        *c    = r->thunk;
    ncc_string_t path = ncc_tree_leaf_text(ncc_tree_child(r->node, 1));
    tl_node     *line = kid(r, 3);
    tl_node     *col  = kid(r, 5);

    if (!path.data || !line || !col) {
        c->err = "an anonymous site needs a path, a line and a column";

        return nullptr;
    }

    return node_type(c, th_name(c->th,
                                th_anon_name(c->th, path.data, line->u,
                                             col->u)));
}

static void *r_field(ncc_reduce_t *r)
{
    bctx        *c    = r->thunk;
    ncc_string_t name = ncc_tree_leaf_text(ncc_tree_child(r->node, 0));
    th_type     *type = kid_type(r, 2);

    if (!name.data || !type) {
        c->err = "a field needs a name and a type";

        return nullptr;
    }

    tl_node *n = node_new(c, TL_MEMBER);

    n->member.name = strdup(name.data);
    n->member.type = type;

    return n;
}

static size_t collect(ncc_reduce_t *r, tl_kind want, tl_node **out, size_t max)
{
    size_t   n     = 0;
    tl_node *first = kid(r, 0);

    if (first && first->kind == want && n < max) {
        out[n++] = first;
    }

    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));

    if (rest) {
        for (size_t i = 0; i < rest->n && n < max; i++) {
            tl_node *e = rest->v[i];

            if (e && e->kind == want) {
                out[n++] = e;
            }
        }
    }

    return n;
}

static void *r_field_list(ncc_reduce_t *r)
{
    tl_node           *got[64];
    size_t             n    = collect(r, TL_MEMBER, got, 64);
    ncc_reduce_list_t *list = ncc_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        list->v[i] = got[i];
    }

    return list;
}

static void *r_field_body(ncc_reduce_t *r)
{
    bctx      *c = r->thunk;
    th_member  members[64];
    size_t     nm = 0;

    ncc_reduce_list_t *fields = r->n_kids == 3
                                  ? ncc_reduce_as_list(kid(r, 1))
                                  : nullptr;

    if (fields) {
        for (size_t i = 0; i < fields->n && nm < 64; i++) {
            tl_node *f = fields->v[i];

            if (f && f->kind == TL_MEMBER) {
                members[nm++] = f->member;
            }
        }
    }

    return node_type(c, th_record(c->th, members, nm));
}

static void *r_tuple(ncc_reduce_t *r)
{
    bctx    *c = r->thunk;
    th_type *elems[64];
    size_t   ne = 0;

    ncc_reduce_list_t *inner = ncc_reduce_as_list(kid(r, 2));

    if (inner) {
        for (size_t i = 0; i < inner->n && ne < 64; i++) {
            th_type *t = as_type(inner->v[i]);

            if (t) {
                elems[ne++] = t;
            }

            ncc_reduce_list_t *more = ncc_reduce_as_list(inner->v[i]);

            for (size_t j = 0; more && j < more->n && ne < 64; j++) {
                th_type *u = as_type(more->v[j]);

                if (u) {
                    elems[ne++] = u;
                }
            }
        }
    }

    return node_type(c, th_tuple(c->th, elems, ne));
}

static void *r_mode(ncc_reduce_t *r)
{
    bctx        *c = r->thunk;
    ncc_string_t s = ncc_tree_leaf_text(ncc_tree_child(r->node, 0));
    tl_node     *n = node_new(c, TL_MODE);

    n->mode = s.data && !strcmp(s.data, "out")     ? TH_OUT
            : s.data && !strcmp(s.data, "inout")   ? TH_INOUT
                                                   : TH_IN;

    return n;
}

static void *r_rest_param(ncc_reduce_t *r)
{
    bctx    *c = r->thunk;
    tl_node *n = node_new(c, TL_PARAM);

    n->param.is_rest = true;
    n->param.type    = kid_type(r, 1);

    return n;
}

static void *r_param(ncc_reduce_t *r)
{
    bctx    *c    = r->thunk;
    tl_node *n    = node_new(c, TL_PARAM);
    th_mode  mode = TH_IN;

    for (size_t i = 0; i < r->n_kids; i++) {
        ncc_reduce_list_t *opt = ncc_reduce_as_list(kid(r, i));

        if (opt) {
            for (size_t j = 0; j < opt->n; j++) {
                tl_node *m = opt->v[j];

                if (m && m->kind == TL_MODE) {
                    mode = m->mode;
                }
            }

            continue;
        }

        tl_node *k = kid(r, i);

        if (!k) {
            continue;
        }

        if (k->kind == TL_MODE) {
            mode = k->mode;
        }
        else if (k->kind == TL_PARAM) {
            n->param = k->param;
        }
        else if (k->kind == TL_TYPE) {
            n->param.type = k->type;
        }
    }

    ncc_string_t named = ncc_tree_leaf_text(ncc_tree_child(r->node, 0));

    if (rule_of(r->node) == 1) {
        for (size_t i = 0; i < ncc_tree_num_children(r->node); i++) {
            ncc_string_t s = ncc_tree_leaf_text(ncc_tree_child(r->node, i));

            if (s.data && ncc_tree_leaf_tid(ncc_tree_child(r->node, i))
                              == tid_ident) {
                named = s;
                break;
            }
        }

        n->param.name = named.data ? strdup(named.data) : nullptr;
    }

    n->param.mode = mode;

    return n;
}

static void *r_param_list(ncc_reduce_t *r)
{
    tl_node           *got[64];
    size_t             n    = collect(r, TL_PARAM, got, 64);
    ncc_reduce_list_t *list = ncc_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        list->v[i] = got[i];
    }

    return list;
}

static void *r_function(ncc_reduce_t *r)
{
    bctx      *c = r->thunk;
    th_param   params[64];
    th_kwparam kws[64];
    th_param   rest      = {0};
    bool       have_rest = false;
    size_t     np        = 0;
    size_t     nkw       = 0;

    ncc_reduce_list_t *opt = ncc_reduce_as_list(kid(r, 1));

    if (opt) {
        for (size_t i = 0; i < opt->n; i++) {
            ncc_reduce_list_t *pl = ncc_reduce_as_list(opt->v[i]);

            for (size_t j = 0; pl && j < pl->n; j++) {
                tl_node *p = pl->v[j];

                if (!p || p->kind != TL_PARAM) {
                    continue;
                }

                if (p->param.is_rest) {
                    rest.mode = p->param.mode;
                    rest.type = p->param.type;
                    have_rest = true;
                }
                else if (p->param.name && nkw < 64) {
                    kws[nkw].name = p->param.name;
                    kws[nkw].mode = p->param.mode;
                    kws[nkw].type = p->param.type;
                    nkw++;
                }
                else if (np < 64) {
                    params[np].mode = p->param.mode;
                    params[np].type = p->param.type;
                    np++;
                }
            }
        }
    }

    th_type *ret = kid_type(r, r->n_kids - 1);

    if (!ret) {
        c->err = "a function needs a return type";

        return nullptr;
    }

    return node_type(c, th_func(c->th, ret, params, np,
                                have_rest ? &rest : nullptr, kws, nkw));
}

static void *r_int_leaf_or_str(ncc_reduce_t *r)
{
    bctx    *c   = r->thunk;
    int64_t  tid = ncc_tree_leaf_tid(r->node);

    if (tid == tid_int) {
        tl_node *n = node_new(c, TL_INT);

        n->u = strtoull(ncc_tree_leaf_text(r->node).data, nullptr, 10);

        return n;
    }

    if (tid == tid_str || tid == tid_ident) {
        tl_node *n = node_new(c, TL_NAME);

        n->name = strdup(ncc_tree_leaf_text(r->node).data);

        return n;
    }

    return nullptr;
}

static void reg(ncc_tree_walk_t *w, bctx *c, const char *nt, ncc_reduce_fn fn)
{
    if (!ncc_walk_reduce_nt(w, ncc_string_from_cstr(nt), fn, c)) {
        fprintf(stderr, "typelang: no production named <%s>\n", nt);
    }
}

static void install(ncc_tree_walk_t *w, bctx *c)
{
    reg(w, c, "type", r_pass);
    reg(w, c, "one-type", r_pass);
    reg(w, c, "base-type", r_pass);
    reg(w, c, "record", r_pass);
    reg(w, c, "paren-form", r_paren_form);
    reg(w, c, "union-type", r_union);
    reg(w, c, "isect-type", r_isect);
    reg(w, c, "named-type", r_named_type);
    reg(w, c, "qualified-name", r_qualified_name);
    reg(w, c, "type-args", r_type_args);
    reg(w, c, "type-arg", r_type_arg);
    reg(w, c, "tvar", r_tvar);
    reg(w, c, "binder", r_binder);
    reg(w, c, "bitint", r_bitint);
    reg(w, c, "indirection", r_indirection);
    reg(w, c, "ind-tail", r_ind_tail);
    reg(w, c, "type-app", r_type_app);
    reg(w, c, "ind-attr", r_ind_attr);
    reg(w, c, "ind-attrs", r_ind_attrs);
    reg(w, c, "sequence", r_sequence);
    reg(w, c, "anon-site", r_anon_site);
    reg(w, c, "field", r_field);
    reg(w, c, "field-list", r_field_list);
    reg(w, c, "field-body", r_field_body);
    reg(w, c, "tuple", r_tuple);
    reg(w, c, "mode", r_mode);
    reg(w, c, "rest-param", r_rest_param);
    reg(w, c, "param", r_param);
    reg(w, c, "param-list", r_param_list);
    reg(w, c, "function", r_function);

    ncc_walk_on_nt(w, ncc_string_from_cstr("binder"), binder_open, nullptr, c);
}

static th_type *lower(bctx *c, ncc_parse_tree_t *tree)
{
    ncc_tree_walk_t *w = ncc_tree_walk_new(c->g, c);

    ncc_walk_reduce_leaf(w, r_int_leaf_or_str, c);
    install(w, c);

    th_type *out = as_type(ncc_tree_walk_reduce(w, tree));

    ncc_tree_walk_free(w);

    return out;
}
#define TL_MAX_DEPTH 512

static bool too_deep(const char *src, size_t len)
{
    size_t depth = 0;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];

        if (c == '[' || c == '(' || c == '{') {
            if (++depth > TL_MAX_DEPTH) {
                return true;
            }
        }
        else if (c == ']' || c == ')' || c == '}') {
            depth -= depth != 0;
        }
    }

    return false;
}

static th_type *run(th_ctx *ctx, const char *src, size_t len, size_t *consumed,
                    char **err)
{
    if (err) {
        *err = nullptr;
    }

    ncc_grammar_t *g = load_grammar();

    if (!g || !src) {
        return nullptr;
    }

    if (too_deep(src, len)) {
        if (err) {
            *err = msg("this type nests deeper than the parser will descend");
        }

        return nullptr;
    }

    ncc_buffer_t  *buf = ncc_buffer_from_bytes(src, (int64_t)len);
    ncc_scanner_t *sc  = ncc_scanner_new(buf, tl_scan, g,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);
    ncc_token_stream_t *ts  = ncc_token_stream_new(sc);
    ncc_pwz_parser_t   *p   = ncc_pwz_new(g);
    th_type            *out = nullptr;
    int32_t             used = 0;
    bool                ok;

    if (consumed) {
        ok = ncc_pwz_parse_prefix(p, ts, &used);
    }
    else {
        ok = ncc_pwz_parse(p, ts);
    }

    if (ok) {
        bctx c = {.th = ctx, .g = g};

        ncc_dict_init(&c.fv_index, ncc_hash_cstring, ncc_dict_cstr_eq);

        out = lower(&c, ncc_pwz_get_tree(p));

        if (!out && err) {
            *err = msg(c.err ? c.err : "cannot build this type");
        }

        for (size_t i = 0; i < c.nfree; i++) {
            free(c.free_vars[i].name);
        }

        ncc_dict_free(&c.fv_index);
        free(c.free_vars);
        free(c.binders);
    }
    else if (err) {
        *err = msg("cannot parse a type here");
    }

    if (out && consumed) {
        ncc_token_info_t *last = used > 0 ? ncc_stream_get(ts, used - 1)
                                          : nullptr;

        *consumed = last ? last->end_byte : 0;
    }

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return out;
}

ir_typelang_lower_t *ir_typelang_lower_new(th_ctx *ctx)
{
    bctx *c = calloc(1, sizeof(bctx));

    if (!c) {
        return nullptr;
    }

    c->th = ctx;
    c->g  = load_grammar();

    ncc_dict_init(&c->fv_index, ncc_hash_cstring, ncc_dict_cstr_eq);

    return (ir_typelang_lower_t *)c;
}

void ir_typelang_install(ncc_tree_walk_t *w, ir_typelang_lower_t *l)
{
    bctx *c = (bctx *)l;

    ncc_walk_reduce_leaf(w, r_int_leaf_or_str, c);
    install(w, c);
}

th_type *ir_typelang_type_of(void *reduced)
{
    return as_type(reduced);
}

void ir_typelang_lower_free(ir_typelang_lower_t *l)
{
    bctx *c = (bctx *)l;

    if (!c) {
        return;
    }

    for (size_t i = 0; i < c->nfree; i++) {
        free(c->free_vars[i].name);
    }

    ncc_dict_free(&c->fv_index);
    free(c->free_vars);
    free(c->binders);
    free(c);
}

th_type *ir_typelang_parse(th_ctx *ctx, const char *src, char **err)
{
    return src ? run(ctx, src, strlen(src), nullptr, err) : nullptr;
}

th_type *ir_typelang_parse_prefix(th_ctx *ctx, const char *src, size_t len,
                                  size_t *consumed, char **err)
{
    return run(ctx, src, len, consumed, err);
}
