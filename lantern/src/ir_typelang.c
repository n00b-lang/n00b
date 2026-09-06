#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/typelang.h"

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"

#include "typelang_bnf.h"

static char *msg(const char *what)
{
    size_t n = strlen(what) + 12;
    char  *p = malloc(n);

    snprintf(p, n, "typelang: %s", what);

    return p;
}

static ncc_grammar_t *grammar;
static bool           char_terminals[128];
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

    memset(char_terminals, 0, sizeof(char_terminals));

    for (size_t i = 0; i < ncc_list_len(g->rules); i++) {
        ncc_parse_rule_t *r = &g->rules.data[i];

        for (size_t j = 0; j < ncc_list_len(r->contents); j++) {
            ncc_match_t *m = &r->contents.data[j];

            if (m->kind == NCC_MATCH_TERMINAL && m->terminal_id > 0
                && m->terminal_id < 128) {
                char_terminals[m->terminal_id] = true;
            }
        }
    }

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

typedef struct {
    ncc_token_info_t **v;
    size_t            *end;
    int32_t            n;
    int32_t            cap;
} toks;

static void emit(toks *t, int64_t tid, const char *at, size_t len, size_t end)
{
    if (t->n >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->v   = realloc(t->v, (size_t)t->cap * sizeof(*t->v));
        t->end = realloc(t->end, (size_t)t->cap * sizeof(*t->end));
    }

    ncc_token_info_t *tok = calloc(1, sizeof(*tok));

    tok->tid   = tid;
    tok->index = t->n;
    tok->line  = 1;
    tok->value = ncc_option_set(ncc_string_t,
                                ncc_string_from_raw(at, (int64_t)len));

    t->end[t->n] = end;
    t->v[t->n++] = tok;
}

static bool id_start(unsigned char c)
{
    return c == '_' || isalpha(c) || c >= 0x80;
}

static bool id_more(unsigned char c)
{
    return c == '_' || isalnum(c) || c >= 0x80;
}

static bool tokenize(ncc_grammar_t *g, const char *src, size_t len, toks *t)
{
    size_t i = 0;

    while (i < len && src[i]) {
        if (isspace((unsigned char)src[i])) {
            i++;
            continue;
        }

        if (src[i] == '"') {
            size_t start = i++;

            while (i < len && src[i] != '"') {
                i += src[i] == '\\' && i + 1 < len ? 2 : 1;
            }

            if (i >= len || src[i] != '"') {
                return false;
            }

            i++;
            emit(t, tid_str, src + start + 1, i - start - 2, i);
            continue;
        }

        if (isdigit((unsigned char)src[i])) {
            size_t start = i;

            while (i < len && isdigit((unsigned char)src[i])) {
                i++;
            }

            emit(t, tid_int, src + start, i - start, i);
            continue;
        }

        if (id_start((unsigned char)src[i])) {
            size_t start = i;

            while (i < len && id_more((unsigned char)src[i])) {
                i++;
            }

            size_t  n  = i - start;
            char    word[128];
            int64_t kw = 0;

            bool after_tick = t->n > 0 && t->v[t->n - 1]->tid == '`';

            if (after_tick) {
                kw = 0;
            }
            else if (n == 1) {
                unsigned char ch = (unsigned char)src[start];

                kw = ch < 128 && char_terminals[ch] ? ch : 0;
            }
            else if (n < sizeof(word)) {
                memcpy(word, src + start, n);
                word[n] = 0;
                kw      = lookup(g, word);
            }

            emit(t, kw ? kw : tid_ident, src + start, n, i);
            continue;
        }

        if (i + 1 < len) {
            char two[3] = {src[i], src[i + 1], 0};
            int64_t op  = lookup(g, two);

            if (op && op > 0xFF) {
                emit(t, op, src + i, 2, i + 2);
                i += 2;
                continue;
            }
        }

        emit(t, (unsigned char)src[i], src + i, 1, i + 1);
        i++;
    }

    return true;
}

static void free_toks(toks *t)
{
    free(t->v);
    free(t->end);
}

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

    freev  *free_vars;
    size_t  nfree;
    size_t  cfree;

    binding *binders;
    size_t   nb;
    size_t   cb;
} bctx;

static th_type *build(bctx *c, ncc_parse_tree_t *n);

static th_type *build_tvar(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    const char *name = nullptr;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_leaf_tid(k.v[i]) == tid_ident) {
            name = ncc_tree_leaf_text(k.v[i]).data;
        }
    }

    ncc_tree_kids_free(&k);

    if (!name) {
        c->err = "a type variable needs a name";

        return nullptr;
    }

    for (size_t d = c->nb; d > 0; d--) {
        for (size_t i = 0; i < c->binders[d - 1].n; i++) {
            if (!strcmp(c->binders[d - 1].names[i], name)) {
                return th_bound(c->th, c->nb - d + 1, i);
            }
        }
    }

    for (size_t i = 0; i < c->nfree; i++) {
        if (!strcmp(c->free_vars[i].name, name)) {
            return c->free_vars[i].type;
        }
    }

    if (c->nfree >= c->cfree) {
        c->cfree     = c->cfree ? c->cfree * 2 : 8;
        c->free_vars = realloc(c->free_vars, c->cfree * sizeof(freev));
    }

    th_type *t = th_var(c->th);

    c->free_vars[c->nfree].name = strdup(name);
    c->free_vars[c->nfree].type = t;
    c->nfree++;

    return t;
}

static th_type *build_binder(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    char  *names[64];
    size_t nn = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], "tvar")) {
            continue;
        }

        ncc_tree_kids_t v = {0};

        ncc_tree_kids(k.v[i], &v);

        for (size_t j = 0; j < v.n; j++) {
            if (ncc_tree_leaf_tid(v.v[j]) == tid_ident && nn < 64) {
                names[nn++] = strdup(ncc_tree_leaf_text(v.v[j]).data);
            }
        }

        ncc_tree_kids_free(&v);
    }

    if (!nn) {
        c->err = "forall needs at least one type variable";
        ncc_tree_kids_free(&k);

        return nullptr;
    }

    if (c->nb >= c->cb) {
        c->cb      = c->cb ? c->cb * 2 : 4;
        c->binders = realloc(c->binders, c->cb * sizeof(binding));
    }

    c->binders[c->nb].names = names;
    c->binders[c->nb].n     = nn;
    c->nb++;

    th_type *body = nullptr;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_is_nt(c->g, k.v[i], "one-type")) {
            body = build(c, k.v[i]);
        }
    }

    c->nb--;

    for (size_t i = 0; i < nn; i++) {
        free(names[i]);
    }

    ncc_tree_kids_free(&k);

    return body ? th_binder(c->th, nn, body) : nullptr;
}

static th_type *build_bitint(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    bool     is_signed = ncc_tree_leaf_is(k.v[0], "bitint");
    uint64_t width     = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_leaf_tid(k.v[i]) == tid_int) {
            width = strtoull(ncc_tree_leaf_text(k.v[i]).data, nullptr, 10);
        }
    }

    ncc_tree_kids_free(&k);

    return th_bitint(c->th, width, is_signed);
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

static bool attr_bit(const char *w, uint8_t *out)
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

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(w, map[i].name)) {
            *out = map[i].bit;

            return true;
        }
    }

    return false;
}

static th_type *build_indirection(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    const char *sugar    = nullptr;
    uint8_t     attrs    = 0;
    bool        explicit = false;
    th_type    *referent = nullptr;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_is_nt(c->g, k.v[i], "ind-name")) {
            ncc_tree_kids_t s = {0};

            ncc_tree_kids(k.v[i], &s);
            sugar = s.n ? ncc_tree_leaf_text(s.v[0]).data : nullptr;
            ncc_tree_kids_free(&s);
            continue;
        }

        if (ncc_tree_is_nt(c->g, k.v[i], "union-type")) {
            referent = build(c, k.v[i]);
            continue;
        }

        if (ncc_tree_is_nt(c->g, k.v[i], "ind-attrs")) {
            ncc_tree_kids_t a = {0};

            ncc_tree_kids(k.v[i], &a);
            explicit = true;

            for (size_t j = 0; j < a.n; j++) {
                ncc_tree_kids_t one = {0};

                ncc_tree_kids(a.v[j], &one);

                for (size_t m = 0; m < one.n; m++) {
                    const char *t = ncc_tree_leaf_text(one.v[m]).data;
                    uint8_t     b;

                    if (t && attr_bit(t, &b)) {
                        attrs |= b;
                    }
                }

                ncc_tree_kids_free(&one);
            }

            ncc_tree_kids_free(&a);
        }
    }

    ncc_tree_kids_free(&k);

    if (!referent) {
        c->err = c->err ? c->err : "an indirection needs a referent";

        return nullptr;
    }

    if (!explicit && sugar) {
        attrs = sugar_attrs(sugar);
    }

    return th_ind(c->th, attrs, referent);
}

static th_type *build_sequence(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_type *elem   = nullptr;
    bool     sized  = false;
    uint64_t extent = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_is_nt(c->g, k.v[i], "union-type")) {
            elem = build(c, k.v[i]);
        }
        else if (ncc_tree_leaf_tid(k.v[i]) == tid_int) {
            sized  = true;
            extent = strtoull(ncc_tree_leaf_text(k.v[i]).data, nullptr, 10);
        }
    }

    ncc_tree_kids_free(&k);

    if (!elem) {
        return nullptr;
    }

    return sized ? th_array_n(c->th, elem, extent) : th_array(c->th, elem);
}

static th_type *build_anon_site(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    const char *path = nullptr;
    uint64_t    nums[2] = {0, 0};
    size_t      nn      = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_leaf_tid(k.v[i]) == tid_str) {
            path = ncc_tree_leaf_text(k.v[i]).data;
        }
        else if (ncc_tree_leaf_tid(k.v[i]) == tid_int && nn < 2) {
            nums[nn++] = strtoull(ncc_tree_leaf_text(k.v[i]).data, nullptr, 10);
        }
    }

    ncc_tree_kids_free(&k);

    if (!path || nn != 2) {
        c->err = "an anonymous site needs a path, a line and a column";

        return nullptr;
    }

    return th_name(c->th, th_anon_name(c->th, path, nums[0], nums[1]));
}

static th_type *build_record(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_member members[64];
    size_t    nm = 0;

    for (size_t i = 0; i < k.n && nm < 64; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], "field-list")) {
            continue;
        }

        ncc_tree_kids_t fl = {0};

        ncc_tree_kids(k.v[i], &fl);

        for (size_t j = 0; j < fl.n && nm < 64; j++) {
            if (!ncc_tree_is_nt(c->g, fl.v[j], "field")) {
                continue;
            }

            ncc_tree_kids_t f = {0};

            ncc_tree_kids(fl.v[j], &f);

            const char *name = nullptr;
            th_type    *type = nullptr;

            for (size_t m = 0; m < f.n; m++) {
                if (ncc_tree_leaf_tid(f.v[m]) == tid_ident) {
                    name = ncc_tree_leaf_text(f.v[m]).data;
                }
                else if (ncc_tree_is_nt(c->g, f.v[m], "union-type")) {
                    type = build(c, f.v[m]);
                }
            }

            ncc_tree_kids_free(&f);

            if (!name || !type) {
                ncc_tree_kids_free(&fl);
                ncc_tree_kids_free(&k);

                return nullptr;
            }

            members[nm].name = name;
            members[nm].type = type;
            nm++;
        }

        ncc_tree_kids_free(&fl);
    }

    ncc_tree_kids_free(&k);

    return th_record(c->th, members, nm);
}

static th_type *build_tuple(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_type *elems[64];
    size_t   ne = 0;

    for (size_t i = 0; i < k.n && ne < 64; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], "union-type")) {
            continue;
        }

        th_type *t = build(c, k.v[i]);

        if (!t) {
            ncc_tree_kids_free(&k);

            return nullptr;
        }

        elems[ne++] = t;
    }

    ncc_tree_kids_free(&k);

    return th_tuple(c->th, elems, ne);
}

static bool read_mode(bctx *c, ncc_parse_tree_t *n, th_mode *m)
{
    if (!ncc_tree_is_nt(c->g, n, "mode")) {
        return false;
    }

    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    const char *t = k.n ? ncc_tree_leaf_text(k.v[0]).data : nullptr;

    ncc_tree_kids_free(&k);

    if (!t) {
        return false;
    }

    *m = !strcmp(t, "out") ? TH_OUT : !strcmp(t, "inout") ? TH_INOUT : TH_IN;

    return true;
}

static bool build_param(bctx *c, ncc_parse_tree_t *n, th_param *pos,
                        th_param *rest, th_kwparam *kw, int *which)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_mode     mode = TH_IN;
    const char *name = nullptr;
    th_type    *type = nullptr;
    bool        is_rest = false;

    for (size_t i = 0; i < k.n; i++) {
        th_mode m;

        if (read_mode(c, k.v[i], &m)) {
            mode = m;
            continue;
        }

        if (ncc_tree_is_nt(c->g, k.v[i], "rest-param")) {
            ncc_tree_kids_t r = {0};

            ncc_tree_kids(k.v[i], &r);
            is_rest = true;

            for (size_t j = 0; j < r.n; j++) {
                if (ncc_tree_is_nt(c->g, r.v[j], "union-type")) {
                    type = build(c, r.v[j]);
                }
            }

            ncc_tree_kids_free(&r);
            continue;
        }

        if (ncc_tree_leaf_tid(k.v[i]) == tid_ident) {
            name = ncc_tree_leaf_text(k.v[i]).data;
            continue;
        }

        if (ncc_tree_is_nt(c->g, k.v[i], "union-type")) {
            type = build(c, k.v[i]);
        }
    }

    ncc_tree_kids_free(&k);

    if (!type) {
        return false;
    }

    if (is_rest) {
        rest->mode = mode;
        rest->type = type;
        *which     = 1;
    }
    else if (name) {
        kw->name = name;
        kw->mode = mode;
        kw->type = type;
        *which   = 2;
    }
    else {
        pos->mode = mode;
        pos->type = type;
        *which    = 0;
    }

    return true;
}

static th_type *build_function(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_param   params[64];
    th_kwparam kws[64];
    th_param   rest      = {0};
    bool       have_rest = false;
    size_t     np        = 0;
    size_t     nkw       = 0;
    th_type   *ret       = nullptr;

    for (size_t i = 0; i < k.n; i++) {
        if (ncc_tree_is_nt(c->g, k.v[i], "param-list")) {
            ncc_tree_kids_t pl = {0};

            ncc_tree_kids(k.v[i], &pl);

            for (size_t j = 0; j < pl.n; j++) {
                if (!ncc_tree_is_nt(c->g, pl.v[j], "param")) {
                    continue;
                }

                th_param   p = {0};
                th_param   r = {0};
                th_kwparam w = {0};
                int        which;

                if (!build_param(c, pl.v[j], &p, &r, &w, &which)) {
                    ncc_tree_kids_free(&pl);
                    ncc_tree_kids_free(&k);

                    return nullptr;
                }

                if (which == 0 && np < 64) {
                    params[np++] = p;
                }
                else if (which == 1) {
                    rest      = r;
                    have_rest = true;
                }
                else if (which == 2 && nkw < 64) {
                    kws[nkw++] = w;
                }
            }

            ncc_tree_kids_free(&pl);
            continue;
        }

        if (ncc_tree_is_nt(c->g, k.v[i], "union-type")) {
            ret = build(c, k.v[i]);
        }
    }

    ncc_tree_kids_free(&k);

    if (!ret) {
        return nullptr;
    }

    return th_func(c->th, ret, params, np, have_rest ? &rest : nullptr, kws,
                   nkw);
}

static th_type *build_named(bctx *c, ncc_parse_tree_t *n)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    bool   variant = k.n && ncc_tree_leaf_is(k.v[0], "variant");
    char   name[512];
    size_t len = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], "qualified-name")) {
            continue;
        }

        ncc_tree_kids_t q = {0};

        ncc_tree_kids(k.v[i], &q);

        for (size_t j = 0; j < q.n; j++) {
            const char *part = ncc_tree_leaf_text(q.v[j]).data;
            size_t      pl   = part ? strlen(part) : 0;

            if (!pl || len + pl + 1 >= sizeof(name)) {
                continue;
            }

            memcpy(name + len, part, pl);
            len += pl;
        }

        ncc_tree_kids_free(&q);
        break;
    }

    name[len] = 0;

    th_arg args[64];
    size_t nargs = 0;

    for (size_t i = 0; i < k.n; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], "type-args")) {
            continue;
        }

        ncc_tree_kids_t a = {0};

        ncc_tree_kids(k.v[i], &a);

        for (size_t j = 0; j < a.n && nargs < 64; j++) {
            if (!ncc_tree_is_nt(c->g, a.v[j], "type-arg")) {
                continue;
            }

            ncc_tree_kids_t one = {0};

            ncc_tree_kids(a.v[j], &one);

            ncc_parse_tree_t *sole = one.n == 1 ? one.v[0] : nullptr;

            ncc_tree_kids_free(&one);

            if (!sole) {
                continue;
            }

            if (ncc_tree_leaf_tid(sole) == tid_int) {
                args[nargs].kind = TH_ARG_UINT;
                args[nargs].u    = strtoull(ncc_tree_leaf_text(sole).data, nullptr, 10);
                nargs++;
                continue;
            }

            if (ncc_tree_leaf_tid(sole) == tid_str) {
                const char *s = ncc_tree_leaf_text(sole).data;

                args[nargs].kind    = TH_ARG_STR;
                args[nargs].str.p   = s;
                args[nargs].str.n   = strlen(s);
                nargs++;
                continue;
            }

            th_type *t = build(c, sole);

            if (!t) {
                ncc_tree_kids_free(&a);
                ncc_tree_kids_free(&k);

                return nullptr;
            }

            args[nargs].kind = TH_ARG_TYPE;
            args[nargs].type = t;
            nargs++;
        }

        ncc_tree_kids_free(&a);
    }

    ncc_tree_kids_free(&k);

    if (variant) {
        return th_variant(c->th, name, args, nargs);
    }

    return nargs ? th_apply(c->th, name, args, nargs) : th_name(c->th, name);
}

static th_type *build_sum(bctx *c, ncc_parse_tree_t *n, th_comb comb,
                          const char *arm_nt)
{
    ncc_tree_kids_t k = {0};

    ncc_tree_kids(n, &k);

    th_type *arms[64];
    size_t   na = 0;

    for (size_t i = 0; i < k.n && na < 64; i++) {
        if (!ncc_tree_is_nt(c->g, k.v[i], arm_nt)) {
            continue;
        }

        th_type *t = build(c, k.v[i]);

        if (!t) {
            ncc_tree_kids_free(&k);

            return nullptr;
        }

        arms[na++] = t;
    }

    ncc_tree_kids_free(&k);

    if (na == 1) {
        return arms[0];
    }

    return th_sum(c->th, comb, arms, na);
}

static th_type *build(bctx *c, ncc_parse_tree_t *n)
{
    const char *name = ncc_tree_nt_name(c->g, n);

    if (!name) {
        return nullptr;
    }

    if (!strcmp(name, "union-type")) {
        return build_sum(c, n, TH_UNION, "isect-type");
    }

    if (!strcmp(name, "isect-type")) {
        return build_sum(c, n, TH_INTERSECTION, "one-type");
    }

    if (!strcmp(name, "type") || !strcmp(name, "one-type")
        || !strcmp(name, "base-type") || !strcmp(name, "paren-form")
        || !strcmp(name, "record") || !strcmp(name, "type-arg")) {
        size_t nc = ncc_tree_num_children(n);

        if (nc == 1) {
            return build(c, ncc_tree_child(n, 0));
        }

        if (nc == 3) {
            return build(c, ncc_tree_child(n, 1));
        }

        return nullptr;
    }

    if (!strcmp(name, "named-type")) {
        return build_named(c, n);
    }
    if (!strcmp(name, "tvar")) {
        return build_tvar(c, n);
    }
    if (!strcmp(name, "binder")) {
        return build_binder(c, n);
    }
    if (!strcmp(name, "bitint")) {
        return build_bitint(c, n);
    }
    if (!strcmp(name, "indirection")) {
        return build_indirection(c, n);
    }
    if (!strcmp(name, "sequence")) {
        return build_sequence(c, n);
    }
    if (!strcmp(name, "anon-site")) {
        return build_anon_site(c, n);
    }
    if (!strcmp(name, "tuple")) {
        return build_tuple(c, n);
    }
    if (!strcmp(name, "field-body")) {
        return build_record(c, n);
    }
    if (!strcmp(name, "function")) {
        return build_function(c, n);
    }

    c->err = "typelang: unhandled production";

    return nullptr;
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

    toks t = {0};

    if (!tokenize(g, src, len, &t) || !t.n) {
        free_toks(&t);

        if (err) {
            *err = msg("cannot read a type here");
        }

        return nullptr;
    }

    ncc_token_stream_t *ts = ncc_token_stream_from_array(t.v, t.n);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);
    th_type            *out = nullptr;
    int32_t             used = 0;
    bool                ok;

    if (consumed) {
        ok = ncc_pwz_parse_prefix(p, ts, &used);
    }
    else {
        ok   = ncc_pwz_parse(p, ts);
        used = t.n;
    }

    if (ok) {
        bctx c = {.th = ctx, .g = g};

        out = build(&c, ncc_pwz_get_tree(p));

        if (!out && err) {
            *err = msg(c.err ? c.err : "cannot build this type");
        }

        for (size_t i = 0; i < c.nfree; i++) {
            free(c.free_vars[i].name);
        }

        free(c.free_vars);
        free(c.binders);
    }
    else if (err) {
        *err = msg("cannot parse a type here");
    }

    if (out && consumed) {
        *consumed = used > 0 ? t.end[used - 1] : 0;
    }

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    free_toks(&t);

    return out;
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
