#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/typelang.h"

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
#include "scanner/scanner.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/buffer.h"

#include "typelang_bnf.h"

static char *msg(const char *what)
{
    size_t n = strlen(what) + 12;
    char  *p = core_alloc_size(1, n);

    snprintf(p, n, "typelang: %s", what);

    return p;
}

static parse_grammar_t *grammar;
static int64_t        tid_ident;
static int64_t        tid_int;
static int64_t        tid_str;

static int64_t lookup(parse_grammar_t *g, core_string_t name)
{
    return parse_lookup_terminal(g, name);
}

static parse_grammar_t *load_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_register_terminal(g, CORE_STRING_STATIC("INT_LIT"));
    parse_register_terminal(g, CORE_STRING_STATIC("STRING_LIT"));

    if (!parse_bnf_load(CORE_STRING_STATIC(TYPELANG_BNF),
                      CORE_STRING_STATIC("union-type"), g)) {
        parse_grammar_free(g);

        return nullptr;
    }

    tid_ident = PARSE_TOK_IDENTIFIER;
    tid_int   = lookup(g, CORE_STRING_STATIC("INT_LIT"));
    tid_str   = lookup(g, CORE_STRING_STATIC("STRING_LIT"));

    grammar = g;

    return grammar;
}

void ir_typelang_shutdown(void)
{
    if (grammar) {
        parse_grammar_free(grammar);
        grammar = nullptr;
    }
}

static bool after_tick(parse_scanner_t *s)
{
    parse_token_info_t *prev = scan_prev_token(s);

    return prev && prev->tid == '`';
}

static bool tl_scan(parse_scanner_t *s)
{
    scan_skip_whitespace(s);

    if (scan_at_eof(s)) {
        return false;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c == '"') {
        scan_emit(s, (int32_t)tid_str, scan_string_double(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        scan_emit(s, (int32_t)tid_int, scan_integer(s));

        return true;
    }

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80) {
        bool                       name = after_tick(s);
        core_option_t(core_string_t) id   = scan_identifier(s);

        if (core_option_is_set(id)) {
            int64_t kw = name ? PARSE_TOK_OTHER
                              : scan_terminal_id(s,
                                                     core_option_get(id));

            scan_emit(s, kw != PARSE_TOK_OTHER ? (int32_t)kw
                                                 : PARSE_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    core_codepoint_t next = scan_peek(s, 1);

    if (next > 0 && next < 128 && !isspace((int)next)) {
        char two[3] = {(char)c, (char)next, 0};

        int64_t op = scan_terminal_id(s, core_string_borrow(two, 2));

        if (op != PARSE_TOK_OTHER && op > 0xFF) {
            scan_advance_n(s, 2);
            scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);

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

#define TL_NODE_MAGIC 0x546c4e64u

typedef struct {
    uint32_t magic;
    tl_kind  kind;
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

core_list_decl(th_arg);
core_list_decl(th_member);
core_list_decl(th_param);
core_list_decl(th_kwparam);
core_list_decl(freev);

typedef struct {
    char **names;
    size_t n;
} binding;

core_list_decl(binding);

typedef struct {
    th_ctx        *th;
    parse_grammar_t *g;
    const char    *err;

    core_list_t(freev) free_vars;
    core_dict_t        fv_index;

    core_list_t(binding) binders;
} bctx;

static tl_node *node_new(bctx *c, tl_kind kind)
{
    (void)c;

    tl_node *n = core_alloc(typeof(*n));

    n->magic = TL_NODE_MAGIC;
    n->kind  = kind;

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

    if (!n || parse_reduce_as_list(v) || n->magic != TL_NODE_MAGIC) {
        return nullptr;
    }

    return n->kind == TL_TYPE ? n->type : nullptr;
}

static int32_t rule_of(parse_tree_t *n)
{
    return core_tree_is_leaf(n) ? -1 : core_tree_node_value(n).rule_index;
}

static tl_node *kid(parse_reduce_t *r, size_t i)
{
    return i < r->n_kids ? r->kids[i] : nullptr;
}

static th_type *kid_type(parse_reduce_t *r, size_t i)
{
    return as_type(kid(r, i));
}

static void *r_pass(parse_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_paren_form(parse_reduce_t *r)
{
    return r->n_kids == 3 ? kid(r, 1) : kid(r, 0);
}

static void *r_sum(parse_reduce_t *r, th_comb comb)
{
    bctx    *c    = r->thunk;
    th_type *head = kid_type(r, 0);

    if (r->n_kids == 1 || !head) {
        return kid(r, 0);
    }

    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));

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

static void *r_union(parse_reduce_t *r)
{
    return r_sum(r, TH_UNION);
}

static void *r_isect(parse_reduce_t *r)
{
    return r_sum(r, TH_INTERSECTION);
}

static void *r_qualified_name(parse_reduce_t *r)
{
    bctx  *c = r->thunk;
    char   buf[512];
    size_t len = 0;

    core_string_t first = parse_tree_leaf_text(core_tree_child(r->node, 0));

    if (first.data) {
        len = first.u8_bytes < sizeof(buf) ? first.u8_bytes : 0;
        memcpy(buf, first.data, len);
    }

    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));

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

    n->name = core_strdup(buf);

    return n;
}

static void *r_type_arg(parse_reduce_t *r)
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

static void collect_args(parse_reduce_t *r, core_list_t(th_arg) * out)
{
    tl_node *first = kid(r, 0);

    if (first && first->kind == TL_ARG) {
        core_list_push(*out, first->arg);
    }

    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));

    if (rest) {
        for (size_t i = 0; i < rest->n; i++) {
            tl_node *a = rest->v[i];

            if (a && a->kind == TL_ARG) {
                core_list_push(*out, a->arg);
            }
        }
    }
}

static void *r_type_app(parse_reduce_t *r)
{
    return kid(r, 1);
}

static void *r_named_type(parse_reduce_t *r)
{
    bctx    *c       = r->thunk;
    tl_node *nm      = nullptr;
    bool     variant = false;
    size_t   at      = 0;

    parse_reduce_list_t *lead = parse_reduce_as_list(kid(r, 0));

    if (lead) {
        variant = lead->n > 0;
        at      = 1;
    }

    nm = kid(r, at);

    if (!nm || nm->kind != TL_NAME) {
        c->err = "a named type needs a name";

        return nullptr;
    }

    core_list_t(th_arg) args = core_list_new(th_arg);

    parse_reduce_list_t *app = parse_reduce_as_list(kid(r, at + 1));

    for (size_t i = 0; app && i < app->n; i++) {
        parse_reduce_list_t *list = parse_reduce_as_list(app->v[i]);

        for (size_t j = 0; list && j < list->n; j++) {
            tl_node *a = list->v[j];

            if (a && a->kind == TL_ARG) {
                core_list_push(args, a->arg);
            }
        }
    }

    th_type *made;

    if (variant) {
        made = th_variant(c->th, nm->name, args.data, args.len);
    }
    else {
        made = args.len ? th_apply(c->th, nm->name, args.data, args.len)
                        : th_name(c->th, nm->name);
    }

    core_list_free(args);

    return node_type(c, made);
}

static void *r_type_args(parse_reduce_t *r)
{
    bctx *c = r->thunk;

    core_list_t(th_arg) args = core_list_new(th_arg);

    collect_args(r, &args);

    tl_node *out = node_new(c, TL_ARG);

    parse_reduce_list_t *list = parse_reduce_list_new(args.len);

    for (size_t i = 0; i < args.len; i++) {
        tl_node *a = node_new(c, TL_ARG);

        a->arg     = args.data[i];
        list->v[i] = a;
    }

    core_list_free(args);

    core_free(out);

    return list;
}

static void *r_tvar(parse_reduce_t *r)
{
    bctx        *c    = r->thunk;
    core_string_t name = parse_tree_leaf_text(core_tree_child(r->node, 1));

    if (!name.data) {
        c->err = "a type variable needs a name";

        return nullptr;
    }

    for (size_t d = c->binders.len; d > 0; d--) {
        for (size_t i = 0; i < c->binders.data[d - 1].n; i++) {
            if (!strcmp(c->binders.data[d - 1].names[i], name.data)) {
                return node_type(c,
                                 th_bound(c->th, c->binders.len - d + 1, i));
            }
        }
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)core_dict_get(&c->fv_index, name.data,
                                                   &found);

    if (found) {
        return node_type(c, c->free_vars.data[at].type);
    }

    th_type *t = th_var(c->th);

    core_list_push(c->free_vars,
                  ((freev){.name = core_strdup(name.data), .type = t}));
    core_dict_put(&c->fv_index, c->free_vars.data[c->free_vars.len - 1].name,
                 c->free_vars.len - 1);

    return node_type(c, t);
}

static void collect_tvar_names(parse_grammar_t *g, parse_tree_t *n,
                               char **names, size_t *count, size_t max)
{
    if (!n || core_tree_is_leaf(n)) {
        return;
    }

    if (parse_tree_is_nt(g, n, CORE_STRING_STATIC("one-type"))) {
        return;
    }

    if (parse_tree_is_nt(g, n, CORE_STRING_STATIC("tvar"))) {
        core_string_t s = parse_tree_leaf_text(core_tree_child(n, 1));

        if (s.data && *count < max) {
            names[(*count)++] = core_strdup(s.data);
        }

        return;
    }

    size_t nc = core_tree_num_children(n);

    for (size_t i = 0; i < nc; i++) {
        collect_tvar_names(g, core_tree_child(n, i), names, count, max);
    }
}

static bool binder_open(parse_visit_t *v)
{
    bctx  *c = v->thunk;
    char **names = core_alloc_array(char *, 64);
    size_t n     = 0;

    collect_tvar_names(v->grammar, v->node, names, &n, 64);

    core_list_push(c->binders, ((binding){.names = names, .n = n}));

    return true;
}

static void *r_binder(parse_reduce_t *r)
{
    bctx    *c    = r->thunk;
    th_type *body = kid_type(r, r->n_kids - 1);
    size_t   at   = c->binders.len;
    size_t   n    = at ? c->binders.data[at - 1].n : 0;

    if (at) {
        for (size_t i = 0; i < n; i++) {
            core_free(c->binders.data[at - 1].names[i]);
        }

        core_free(c->binders.data[at - 1].names);
        c->binders.len--;
    }

    if (!body) {
        c->err = "forall needs a body";

        return nullptr;
    }

    return node_type(c, th_binder(c->th, n, body));
}

static void *r_bitint(parse_reduce_t *r)
{
    bctx    *c        = r->thunk;
    bool     is_signed = parse_tree_leaf_is(core_tree_child(r->node, 0),
                                          CORE_STRING_STATIC("bitint"));
    tl_node *width     = kid(r, 2);

    return node_type(c, th_bitint(c->th, width ? width->u : 0, is_signed));
}

static void *r_ind_attr(parse_reduce_t *r)
{
    static const struct {
        core_string_t name;
        uint8_t      bit;
    } map[] = {
        {CORE_STRING_STATIC("nullable"),   TH_NULLABLE  },
        {CORE_STRING_STATIC("reseatable"), TH_RESEATABLE},
        {CORE_STRING_STATIC("indexable"),  TH_INDEXABLE },
        {CORE_STRING_STATIC("owning"),     TH_OWNING    },
        {CORE_STRING_STATIC("exclusive"),  TH_EXCLUSIVE },
        {CORE_STRING_STATIC("temporary"),  TH_TEMPORARY },
    };

    bctx    *c   = r->thunk;
    tl_node *n   = node_new(c, TL_ATTR);
    int32_t  alt = rule_of(r->node);

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("ind-attr"),
                               alt, map[i].name)) {
            n->attr = map[i].bit;
            break;
        }
    }

    return n;
}

static void *r_ind_attrs(parse_reduce_t *r)
{
    bctx    *c = r->thunk;
    tl_node *n = node_new(c, TL_ATTR);
    tl_node *first = kid(r, 0);

    n->attr = first && first->kind == TL_ATTR ? first->attr : 0;

    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));

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

static uint8_t sugar_attrs(parse_grammar_t *g, int32_t alt)
{
    static const struct {
        core_string_t name;
        uint8_t      attrs;
    } map[] = {
        {CORE_STRING_STATIC("ptr"),    TH_NULLABLE | TH_RESEATABLE
                                          | TH_INDEXABLE       },
        {CORE_STRING_STATIC("mutref"), TH_EXCLUSIVE             },
        {CORE_STRING_STATIC("own"),    TH_OWNING                },
        {CORE_STRING_STATIC("gcref"),  TH_NULLABLE | TH_RESEATABLE},
        {CORE_STRING_STATIC("tmpref"), TH_TEMPORARY             },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (parse_alt_leads_with(g, CORE_STRING_STATIC("ind-name"), alt,
                               map[i].name)) {
            return map[i].attrs;
        }
    }

    return 0;
}

static void *r_ind_tail(parse_reduce_t *r)
{
    bctx              *c    = r->thunk;
    parse_reduce_list_t *opt  = parse_reduce_as_list(kid(r, 1));
    tl_node           *n    = node_new(c, TL_ATTR);

    for (size_t i = 0; opt && i < opt->n; i++) {
        tl_node *a = opt->v[i];

        if (a && a->kind == TL_ATTR) {
            n->attr |= a->attr;
        }
    }

    return n;
}

static void *r_indirection(parse_reduce_t *r)
{
    bctx    *c        = r->thunk;
    th_type *referent = kid_type(r, 2);

    if (!referent) {
        c->err = "an indirection needs a referent";

        return nullptr;
    }

    uint8_t attrs = sugar_attrs(r->grammar,
                                rule_of(core_tree_child(r->node, 0)));
    parse_reduce_list_t *tail = parse_reduce_as_list(kid(r, 3));

    for (size_t i = 0; tail && i < tail->n; i++) {
        tl_node *a = tail->v[i];

        if (a && a->kind == TL_ATTR) {
            attrs = a->attr;
        }
    }

    return node_type(c, th_ind(c->th, attrs, referent));
}

static void *r_sequence(parse_reduce_t *r)
{
    bctx    *c    = r->thunk;
    th_type *elem = kid_type(r, 2);

    if (!elem) {
        return nullptr;
    }

    parse_reduce_list_t *tail = parse_reduce_as_list(kid(r, 3));

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

static void *r_anon_site(parse_reduce_t *r)
{
    bctx        *c    = r->thunk;
    core_string_t path = parse_tree_leaf_text(core_tree_child(r->node, 1));
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

static void *r_field(parse_reduce_t *r)
{
    bctx        *c    = r->thunk;
    core_string_t name = parse_tree_leaf_text(core_tree_child(r->node, 0));
    th_type     *type = kid_type(r, 2);

    if (!name.data || !type) {
        c->err = "a field needs a name and a type";

        return nullptr;
    }

    tl_node *n = node_new(c, TL_MEMBER);

    n->member.name = core_strdup(name.data);
    n->member.type = type;

    return n;
}

static size_t collect(parse_reduce_t *r, tl_kind want, tl_node **out, size_t max)
{
    size_t   n     = 0;
    tl_node *first = kid(r, 0);

    if (first && first->kind == want && n < max) {
        out[n++] = first;
    }

    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));

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

static void *r_field_list(parse_reduce_t *r)
{
    tl_node           *got[64];
    size_t             n    = collect(r, TL_MEMBER, got, 64);
    parse_reduce_list_t *list = parse_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        list->v[i] = got[i];
    }

    return list;
}

static void *r_field_body(parse_reduce_t *r)
{
    bctx      *c = r->thunk;
    core_list_t(th_member) members = core_list_new(th_member);

    parse_reduce_list_t *fields = r->n_kids == 3
                                  ? parse_reduce_as_list(kid(r, 1))
                                  : nullptr;

    if (fields) {
        for (size_t i = 0; i < fields->n; i++) {
            tl_node *f = fields->v[i];

            if (f && f->kind == TL_MEMBER) {
                core_list_push(members, f->member);
            }
        }
    }

    th_type *rec = th_record(c->th, members.data, members.len);

    core_list_free(members);

    return node_type(c, rec);
}

static void *r_tuple_args(parse_reduce_t *r)
{
    tl_node           *got[64];
    size_t             n    = collect(r, TL_TYPE, got, 64);
    parse_reduce_list_t *list = parse_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        list->v[i] = got[i];
    }

    return list;
}

static void *r_tuple(parse_reduce_t *r)
{
    bctx    *c = r->thunk;
    th_type *elems[64];
    size_t   ne = 0;

    parse_reduce_list_t *outer = parse_reduce_as_list(kid(r, 2));
    parse_reduce_list_t *args  = outer && outer->n
                                 ? parse_reduce_as_list(outer->v[0])
                                 : nullptr;

    for (size_t i = 0; args && i < args->n && ne < 64; i++) {
        th_type *t = as_type(args->v[i]);

        if (t) {
            elems[ne++] = t;
        }
    }

    return node_type(c, th_tuple(c->th, elems, ne));
}

static void *r_mode(parse_reduce_t *r)
{
    bctx    *c   = r->thunk;
    tl_node *n   = node_new(c, TL_MODE);
    int32_t  alt = rule_of(r->node);

    n->mode = parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("mode"), alt,
                                 CORE_STRING_STATIC("out"))
                ? TH_OUT
            : parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("mode"), alt,
                                 CORE_STRING_STATIC("inout"))
                ? TH_INOUT
                                                                  : TH_IN;

    return n;
}

static void *r_rest_param(parse_reduce_t *r)
{
    bctx    *c = r->thunk;
    tl_node *n = node_new(c, TL_PARAM);

    n->param.is_rest = true;
    n->param.type    = kid_type(r, 1);

    return n;
}

static void *r_param(parse_reduce_t *r)
{
    bctx    *c    = r->thunk;
    tl_node *n    = node_new(c, TL_PARAM);
    th_mode  mode = TH_IN;

    for (size_t i = 0; i < r->n_kids; i++) {
        parse_reduce_list_t *opt = parse_reduce_as_list(kid(r, i));

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

    core_string_t named = parse_tree_leaf_text(core_tree_child(r->node, 0));

    if (rule_of(r->node) == 1) {
        for (size_t i = 0; i < core_tree_num_children(r->node); i++) {
            core_string_t s = parse_tree_leaf_text(core_tree_child(r->node, i));

            if (s.data && parse_tree_leaf_tid(core_tree_child(r->node, i))
                              == tid_ident) {
                named = s;
                break;
            }
        }

        n->param.name = core_strdup(named.data);
    }

    n->param.mode = mode;

    return n;
}

static void *r_param_list(parse_reduce_t *r)
{
    tl_node           *got[64];
    size_t             n    = collect(r, TL_PARAM, got, 64);
    parse_reduce_list_t *list = parse_reduce_list_new(n);

    for (size_t i = 0; i < n; i++) {
        list->v[i] = got[i];
    }

    return list;
}

static void *r_function(parse_reduce_t *r)
{
    bctx      *c = r->thunk;
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) kws  = core_list_new(th_kwparam);
    th_param   rest      = {0};
    bool       have_rest = false;

    parse_reduce_list_t *opt = parse_reduce_as_list(kid(r, 1));

    if (opt) {
        for (size_t i = 0; i < opt->n; i++) {
            parse_reduce_list_t *pl = parse_reduce_as_list(opt->v[i]);

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
                else if (p->param.name) {
                    core_list_push(kws,
                                   ((th_kwparam){
                                       .name = p->param.name,
                                       .mode = p->param.mode,
                                       .type = p->param.type,
                                   }));
                }
                else {
                    core_list_push(params,
                                   ((th_param){
                                       .mode = p->param.mode,
                                       .type = p->param.type,
                                   }));
                }
            }
        }
    }

    th_type *ret = kid_type(r, r->n_kids - 1);

    if (!ret) {
        c->err = "a function needs a return type";

        core_list_free(params);
        core_list_free(kws);

        return nullptr;
    }

    th_type *fn = th_func(c->th, ret, params.data, params.len,
                          have_rest ? &rest : nullptr, kws.data, kws.len);

    core_list_free(params);
    core_list_free(kws);

    return node_type(c, fn);
}

static void *r_int_leaf_or_str(parse_reduce_t *r)
{
    bctx    *c   = r->thunk;
    int64_t  tid = parse_tree_leaf_tid(r->node);

    core_string_t text = parse_tree_leaf_text(r->node);

    if (!text.data) {
        return nullptr;
    }

    if (tid == tid_int) {
        tl_node *n = node_new(c, TL_INT);

        n->u = strtoull(text.data, nullptr, 10);

        return n;
    }

    if (tid == tid_str || tid == tid_ident) {
        tl_node *n = node_new(c, TL_NAME);

        n->name = core_strdup(text.data);

        return n;
    }

    return nullptr;
}

static void reg(parse_tree_walk_t *w, bctx *c, core_string_t nt,
                parse_reduce_fn fn)
{
    if (!parse_walk_reduce_nt(w, nt, fn, c)) {
        fprintf(stderr, "typelang: no production named <%s>\n", nt.data);
    }
}

static void install(parse_tree_walk_t *w, bctx *c)
{
    reg(w, c, CORE_STRING_STATIC("type"), r_pass);
    reg(w, c, CORE_STRING_STATIC("one-type"), r_pass);
    reg(w, c, CORE_STRING_STATIC("base-type"), r_pass);
    reg(w, c, CORE_STRING_STATIC("record"), r_pass);
    reg(w, c, CORE_STRING_STATIC("paren-form"), r_paren_form);
    reg(w, c, CORE_STRING_STATIC("union-type"), r_union);
    reg(w, c, CORE_STRING_STATIC("isect-type"), r_isect);
    reg(w, c, CORE_STRING_STATIC("named-type"), r_named_type);
    reg(w, c, CORE_STRING_STATIC("qualified-name"), r_qualified_name);
    reg(w, c, CORE_STRING_STATIC("type-args"), r_type_args);
    reg(w, c, CORE_STRING_STATIC("type-arg"), r_type_arg);
    reg(w, c, CORE_STRING_STATIC("tvar"), r_tvar);
    reg(w, c, CORE_STRING_STATIC("binder"), r_binder);
    reg(w, c, CORE_STRING_STATIC("bitint"), r_bitint);
    reg(w, c, CORE_STRING_STATIC("indirection"), r_indirection);
    reg(w, c, CORE_STRING_STATIC("ind-tail"), r_ind_tail);
    reg(w, c, CORE_STRING_STATIC("type-app"), r_type_app);
    reg(w, c, CORE_STRING_STATIC("ind-attr"), r_ind_attr);
    reg(w, c, CORE_STRING_STATIC("ind-attrs"), r_ind_attrs);
    reg(w, c, CORE_STRING_STATIC("sequence"), r_sequence);
    reg(w, c, CORE_STRING_STATIC("anon-site"), r_anon_site);
    reg(w, c, CORE_STRING_STATIC("field"), r_field);
    reg(w, c, CORE_STRING_STATIC("field-list"), r_field_list);
    reg(w, c, CORE_STRING_STATIC("field-body"), r_field_body);
    reg(w, c, CORE_STRING_STATIC("tuple-args"), r_tuple_args);
    reg(w, c, CORE_STRING_STATIC("tuple"), r_tuple);
    reg(w, c, CORE_STRING_STATIC("mode"), r_mode);
    reg(w, c, CORE_STRING_STATIC("rest-param"), r_rest_param);
    reg(w, c, CORE_STRING_STATIC("param"), r_param);
    reg(w, c, CORE_STRING_STATIC("param-list"), r_param_list);
    reg(w, c, CORE_STRING_STATIC("function"), r_function);

    parse_walk_on_nt(w, CORE_STRING_STATIC("binder"), binder_open, nullptr, c);
}

static th_type *lower(bctx *c, parse_tree_t *tree)
{
    parse_tree_walk_t *w = parse_tree_walk_new(c->g, c);

    ir_typelang_install(w, c->g, (ir_typelang_lower_t *)c);

    th_type *out = as_type(parse_tree_walk_reduce(w, tree));

    parse_tree_walk_free(w);

    return out;
}
static th_type *run(th_ctx *ctx, const char *src, size_t len, size_t *consumed,
                    char **err)
{
    if (err) {
        *err = nullptr;
    }

    parse_grammar_t *g = load_grammar();

    if (!g || !src) {
        return nullptr;
    }

    core_buffer_t  *buf = core_buffer_from_bytes(src, (int64_t)len);
    parse_scanner_t *sc  = scan_scanner_new(buf, tl_scan, g,
                                         core_option_none(core_string_t),
                                         nullptr, nullptr);
    parse_token_stream_t *ts  = scan_token_stream_new(sc);
    parse_pwz_parser_t   *p   = parse_pwz_new(g);
    th_type            *out = nullptr;
    int32_t             used = 0;
    bool                ok;

    if (consumed) {
        ok = parse_pwz_parse_prefix(p, ts, &used);
    }
    else {
        ok = parse_pwz_parse(p, ts);
    }

    if (ok) {
        bctx c = {.th = ctx, .g = g};

        core_dict_init(&c.fv_index, core_hash_cstring, core_dict_cstr_eq);

        out = lower(&c, parse_pwz_get_tree(p));

        if (!out && err) {
            *err = msg(c.err ? c.err : "cannot build this type");
        }

        for (size_t i = 0; i < c.free_vars.len; i++) {
            core_free(c.free_vars.data[i].name);
        }

        core_dict_free(&c.fv_index);
        core_list_free(c.free_vars);
        core_list_free(c.binders);
    }
    else if (err) {
        *err = msg("cannot parse a type here");
    }

    if (out && consumed) {
        parse_token_info_t *last = used > 0 ? scan_stream_get(ts, used - 1)
                                          : nullptr;

        *consumed = last ? last->end_byte : 0;
    }

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return out;
}

ir_typelang_lower_t *ir_typelang_lower_new(th_ctx *ctx)
{
    bctx *c = core_alloc(bctx);

    c->th = ctx;
    c->g  = load_grammar();

    core_dict_init(&c->fv_index, core_hash_cstring, core_dict_cstr_eq);

    return (ir_typelang_lower_t *)c;
}

void ir_typelang_install(parse_tree_walk_t *w, parse_grammar_t *g,
                         ir_typelang_lower_t *l)
{
    bctx *c = (bctx *)l;

    parse_walk_reduce_leaf_in(w, parse_nt_unit(g, CORE_STRING_STATIC("union-type")), r_int_leaf_or_str,
                            c);
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

    for (size_t i = 0; i < c->free_vars.len; i++) {
        core_free(c->free_vars.data[i].name);
    }

    core_dict_free(&c->fv_index);
    core_list_free(c->free_vars);
    core_list_free(c->binders);
    core_free(c);
}

th_type *ir_typelang_parse(th_ctx *ctx, const char *src, char **err)
{
    return src ? run(ctx, src, strlen(src), nullptr, err) : nullptr;
}

th_type *ir_type_from_text(th_ctx *types, core_string_t spec)
{
    if (!types || !spec.data) {
        return nullptr;
    }

    char    *err = nullptr;
    th_type *t   = run(types, spec.data, spec.u8_bytes, nullptr, &err);

    core_free(err);

    return t;
}

th_type *ir_type_from_node(th_ctx           *types,
                           parse_grammar_t    *g,
                           parse_tree_t *node)
{
    if (!types || !g || !node) {
        return nullptr;
    }

    ir_typelang_lower_t *l = ir_typelang_lower_new(types);
    parse_tree_walk_t     *w = parse_tree_walk_new(g, nullptr);

    ir_typelang_install(w, g, l);

    th_type *t = ir_typelang_type_of(parse_tree_walk_reduce(w, node));

    parse_tree_walk_free(w);
    ir_typelang_lower_free(l);

    return t;
}


th_type *ir_typelang_parse_prefix(th_ctx *ctx, const char *src, size_t len,
                                  size_t *consumed, char **err)
{
    return run(ctx, src, len, consumed, err);
}
