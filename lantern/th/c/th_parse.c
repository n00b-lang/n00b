#include <stdlib.h>
#include <string.h>

#include "typehash.h"

typedef struct {
    char    *name;
    th_type *type;
} th_freev;

typedef struct {
    char **names;
    size_t n;
    size_t cap;
} th_binding;

#define TH_MAX_PARSE_DEPTH 512

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    th_ctx     *ctx;
    th_error   *err;
    bool        failed;
    size_t      depth;

    th_freev   *freev;
    size_t      nfree;
    size_t      cfree;

    th_binding *binders;
    size_t      nb;
    size_t      cb;
} P;

static bool reserve(void *slot, size_t *cap, size_t need, size_t elem)
{
    void **p = slot;

    if (need <= *cap) {
        return true;
    }

    size_t n = *cap ? *cap * 2 : 8;

    while (n < need) {
        n *= 2;
    }

    void *q = realloc(*p, n * elem);

    if (!q) {
        return false;
    }
    memset((char *)q + *cap * elem, 0, (n - *cap) * elem);
    *p   = q;
    *cap = n;

    return true;
}

static th_type *p_union(P *p);
static th_type *p_one(P *p);

static void fail(P *p, const char *msg)
{
    if (!p->failed) {
        p->failed = true;

        if (p->err) {
            p->err->msg    = msg;
            p->err->offset = p->pos;
        }
    }
}

static void skip_ws(P *p)
{
    while (p->pos < p->len
           && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t'
               || p->src[p->pos] == '\n' || p->src[p->pos] == '\r')) {
        p->pos++;
    }
}

static bool id_start(unsigned char c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || c >= 0x80;
}

static bool id_cont(unsigned char c)
{
    return id_start(c) || (c >= '0' && c <= '9');
}

static bool at_ch(P *p, char c)
{
    skip_ws(p);

    return p->pos < p->len && p->src[p->pos] == c;
}

static bool eat_ch(P *p, char c)
{
    if (at_ch(p, c)) {
        p->pos++;

        return true;
    }

    return false;
}

static bool eat_arrow(P *p)
{
    skip_ws(p);

    if (p->pos + 1 < p->len && p->src[p->pos] == '-'
        && p->src[p->pos + 1] == '>') {
        p->pos += 2;

        return true;
    }

    return false;
}

static bool peek_arrow(P *p)
{
    size_t save = p->pos;
    bool   r    = eat_arrow(p);

    p->pos = save;

    return r;
}

static size_t scan_id(P *p, size_t at, size_t *len)
{
    size_t i = at;

    if (i >= p->len || !id_start((unsigned char)p->src[i])) {
        *len = 0;

        return at;
    }
    i++;

    while (i < p->len && id_cont((unsigned char)p->src[i])) {
        i++;
    }
    *len = i - at;

    return at;
}

static bool at_id(P *p)
{
    skip_ws(p);

    size_t n;

    scan_id(p, p->pos, &n);

    return n > 0;
}

static char *take_id(P *p)
{
    skip_ws(p);

    size_t n;
    size_t at = scan_id(p, p->pos, &n);

    if (!n) {
        fail(p, "expected an identifier");

        return nullptr;
    }
    p->pos = at + n;

    char *s = malloc(n + 1);

    memcpy(s, p->src + at, n);
    s[n] = 0;

    return s;
}

static bool peek_word(P *p, const char *w)
{
    skip_ws(p);

    size_t n;
    size_t at = scan_id(p, p->pos, &n);

    return n && n == strlen(w) && !memcmp(p->src + at, w, n);
}

static bool eat_word(P *p, const char *w)
{
    if (peek_word(p, w)) {
        size_t n;

        scan_id(p, p->pos, &n);
        p->pos += n;

        return true;
    }

    return false;
}

static uint64_t take_int(P *p)
{
    skip_ws(p);

    if (p->pos >= p->len || p->src[p->pos] < '0' || p->src[p->pos] > '9') {
        fail(p, "expected an integer");

        return 0;
    }

    uint64_t v = 0;

    while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
        v = v * 10 + (uint64_t)(p->src[p->pos++] - '0');
    }

    return v;
}

static char *take_str(P *p)
{
    skip_ws(p);

    if (!eat_ch(p, '"')) {
        fail(p, "expected a string");

        return nullptr;
    }

    size_t start = p->pos;

    while (p->pos < p->len && p->src[p->pos] != '"') {
        p->pos++;
    }

    size_t n = p->pos - start;
    char  *s = malloc(n + 1);

    memcpy(s, p->src + start, n);
    s[n] = 0;

    if (!eat_ch(p, '"')) {
        fail(p, "unterminated string");
    }

    return s;
}

static const char *const RESERVED[] = {
    "const",     "volatile",   "in",        "out",       "inout",
    "nullable",  "reseatable", "indexable", "owning",    "exclusive",
    "temporary", "ptr",        "ref",       "mutref",    "own",
    "gcref",     "tmpref",     "forall",    "bitint",    "ubitint",
    "array",     "variant",    nullptr,
};

static bool word_is(const char *w, size_t n, const char *lit)
{
    return strlen(lit) == n && !memcmp(w, lit, n);
}

static bool is_reserved_n(const char *w, size_t n)
{
    for (size_t i = 0; RESERVED[i]; i++) {
        if (word_is(w, n, RESERVED[i])) {
            return true;
        }
    }

    return false;
}

static uint8_t sugar_attrs_n(const char *w, size_t n, bool *found)
{
    *found = true;

    if (word_is(w, n, "ptr")) {
        return TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE;
    }
    if (word_is(w, n, "ref")) {
        return 0;
    }
    if (word_is(w, n, "mutref")) {
        return TH_EXCLUSIVE;
    }
    if (word_is(w, n, "own")) {
        return TH_OWNING;
    }
    if (word_is(w, n, "gcref")) {
        return TH_NULLABLE | TH_RESEATABLE;
    }
    if (word_is(w, n, "tmpref")) {
        return TH_TEMPORARY;
    }
    *found = false;

    return 0;
}

static bool attr_bit(const char *w, uint8_t *out)
{
    if (!strcmp(w, "nullable")) {
        *out = TH_NULLABLE;
    }
    else if (!strcmp(w, "reseatable")) {
        *out = TH_RESEATABLE;
    }
    else if (!strcmp(w, "indexable")) {
        *out = TH_INDEXABLE;
    }
    else if (!strcmp(w, "owning")) {
        *out = TH_OWNING;
    }
    else if (!strcmp(w, "exclusive")) {
        *out = TH_EXCLUSIVE;
    }
    else if (!strcmp(w, "temporary")) {
        *out = TH_TEMPORARY;
    }
    else {
        return false;
    }

    return true;
}

static th_type *p_tvar(P *p)
{
    eat_ch(p, '`');

    char *n = take_id(p);

    if (!n) {
        return nullptr;
    }

    for (size_t d = p->nb; d > 0; d--) {
        for (size_t i = 0; i < p->binders[d - 1].n; i++) {
            if (!strcmp(p->binders[d - 1].names[i], n)) {
                free(n);

                return th_bound(p->ctx, p->nb - d + 1, i);
            }
        }
    }

    for (size_t i = 0; i < p->nfree; i++) {
        if (!strcmp(p->freev[i].name, n)) {
            free(n);

            return p->freev[i].type;
        }
    }

    if (!reserve(&p->freev, &p->cfree, p->nfree + 1, sizeof(th_freev))) {
        fail(p, "out of memory");
        free(n);

        return nullptr;
    }

    th_type *t = th_var(p->ctx);

    p->freev[p->nfree].name = n;
    p->freev[p->nfree].type = t;
    p->nfree++;

    return t;
}

static th_type *p_binder(P *p)
{
    if (!eat_ch(p, '[')) {
        fail(p, "expected [ after forall");

        return nullptr;
    }

    if (!reserve(&p->binders, &p->cb, p->nb + 1, sizeof(th_binding))) {
        fail(p, "out of memory");

        return nullptr;
    }

    size_t slot        = p->nb++;
    p->binders[slot].n = 0;

    do {
        if (!eat_ch(p, '`')) {
            fail(p, "expected a type variable");

            return nullptr;
        }

        char *n = take_id(p);

        if (!n) {
            return nullptr;
        }

        th_binding *b = &p->binders[slot];

        if (!reserve(&b->names, &b->cap, b->n + 1, sizeof(char *))) {
            fail(p, "out of memory");
            free(n);

            return nullptr;
        }
        b->names[b->n++] = n;
    } while (eat_ch(p, ','));

    if (!eat_ch(p, ']')) {
        fail(p, "expected ]");

        return nullptr;
    }

    th_type *body  = p_one(p);
    uint64_t arity = p->binders[slot].n;

    for (size_t i = 0; i < p->binders[slot].n; i++) {
        free(p->binders[slot].names[i]);
    }
    p->binders[slot].n = 0;
    p->nb--;

    if (!body) {
        return nullptr;
    }

    return th_binder(p->ctx, arity, body);
}

static th_type *p_indirection(P *p, uint8_t sugar)
{
    if (!eat_ch(p, '[')) {
        fail(p, "expected [");

        return nullptr;
    }

    th_type *ref   = p_union(p);
    uint8_t  attrs = sugar;

    if (eat_ch(p, ';')) {
        attrs = 0;

        while (at_id(p)) {
            char   *w = take_id(p);
            uint8_t bit;

            if (!w || !attr_bit(w, &bit)) {
                fail(p, "unknown indirection attribute");
                free(w);

                return nullptr;
            }
            attrs |= bit;
            free(w);

            if (!eat_ch(p, ',')) {
                break;
            }
        }
    }

    if (!eat_ch(p, ']')) {
        fail(p, "expected ]");

        return nullptr;
    }

    return th_ind(p->ctx, attrs, ref);
}

static th_type *p_sequence(P *p)
{
    if (!eat_ch(p, '[')) {
        fail(p, "expected [");

        return nullptr;
    }

    th_type *e = p_union(p);
    th_type *r;

    if (eat_ch(p, ';')) {
        r = th_array_n(p->ctx, e, take_int(p));
    }
    else {
        r = th_array(p->ctx, e);
    }

    if (!eat_ch(p, ']')) {
        fail(p, "expected ]");

        return nullptr;
    }

    return r;
}

static char *p_qualified(P *p)
{
    char *first = take_id(p);

    if (!first) {
        return nullptr;
    }

    size_t cap = strlen(first) + 1;
    char  *acc = malloc(cap);

    memcpy(acc, first, cap);
    free(first);

    for (;;) {
        size_t save = p->pos;

        if (!eat_ch(p, '.')) {
            break;
        }

        if (!at_id(p)) {
            p->pos = save;
            break;
        }

        char *next = take_id(p);
        size_t need = strlen(acc) + 1 + strlen(next) + 1;

        acc = realloc(acc, need);
        strcat(acc, ".");
        strcat(acc, next);
        free(next);
    }

    return acc;
}

static th_type *p_named(P *p, bool variant)
{
    char *n = p_qualified(p);

    if (!n) {
        return nullptr;
    }

    th_arg  *args = nullptr;
    size_t   na   = 0;
    size_t   ca   = 0;
    th_type *r;

    if (eat_ch(p, '[')) {
        do {
            skip_ws(p);

            if (!reserve(&args, &ca, na + 1, sizeof(th_arg))) {
                fail(p, "out of memory");
                free(args);
                free(n);

                return nullptr;
            }

            if (p->pos < p->len && p->src[p->pos] >= '0'
                && p->src[p->pos] <= '9') {
                args[na].kind = TH_ARG_UINT;
                args[na].u    = take_int(p);
            }
            else if (at_ch(p, '"')) {
                char *s = take_str(p);

                args[na].kind  = TH_ARG_STR;
                args[na].str.p = s;
                args[na].str.n = s ? strlen(s) : 0;
            }
            else {
                args[na].kind = TH_ARG_TYPE;
                args[na].type = p_union(p);
            }
            na++;
        } while (eat_ch(p, ','));

        if (!eat_ch(p, ']')) {
            fail(p, "expected ]");
            free(args);
            free(n);

            return nullptr;
        }
    }

    r = variant ? th_variant(p->ctx, n, args, na)
                : th_apply(p->ctx, n, args, na);

    for (size_t i = 0; i < na; i++) {
        if (args[i].kind == TH_ARG_STR) {
            free((void *)args[i].str.p);
        }
    }
    free(args);
    free(n);

    return r;
}

static th_type *p_tuple(P *p)
{
    eat_word(p, "t");

    if (!eat_ch(p, '(')) {
        fail(p, "expected (");

        return nullptr;
    }

    th_type **elems = nullptr;
    size_t    n     = 0;
    size_t    cap   = 0;

    if (!at_ch(p, ')')) {
        do {
            if (!reserve(&elems, &cap, n + 1, sizeof(th_type *))) {
                fail(p, "out of memory");
                free(elems);

                return nullptr;
            }
            elems[n++] = p_union(p);
        } while (eat_ch(p, ','));
    }

    if (!eat_ch(p, ')')) {
        fail(p, "expected )");
        free(elems);

        return nullptr;
    }

    th_type *t = th_tuple(p->ctx, elems, n);

    free(elems);

    return t;
}

static bool looks_like_fields(P *p)
{
    size_t save = p->pos;

    skip_ws(p);

    if (!eat_ch(p, '(')) {
        p->pos = save;

        return false;
    }
    skip_ws(p);

    size_t n;
    size_t at = scan_id(p, p->pos, &n);
    bool   r  = false;

    if (n) {
        size_t q = at + n;

        while (q < p->len && (p->src[q] == ' ' || p->src[q] == '\t')) {
            q++;
        }
        r = q < p->len && p->src[q] == ':';
    }
    p->pos = save;

    return r;
}

static bool paren_is_function(P *p)
{
    size_t save  = p->pos;
    int    depth = 0;

    skip_ws(p);

    while (p->pos < p->len) {
        char c = p->src[p->pos];

        if (c == '(') {
            depth++;
        }
        else if (c == ')') {
            depth--;

            if (!depth) {
                p->pos++;
                bool r = peek_arrow(p);
                p->pos = save;

                return r;
            }
        }
        p->pos++;
    }
    p->pos = save;

    return false;
}

static size_t p_fields(P *p, th_member **out, size_t *cap)
{
    size_t n = 0;

    if (!eat_ch(p, '(')) {
        fail(p, "expected (");

        return 0;
    }

    if (!at_ch(p, ')')) {
        do {
            if (!reserve(out, cap, n + 1, sizeof(th_member))) {
                fail(p, "out of memory");

                return n;
            }

            th_member *slot = *out;
            char      *nm   = take_id(p);

            if (!nm || !eat_ch(p, ':')) {
                fail(p, "expected name: type");
                free(nm);

                return n;
            }
            slot[n].name = nm;
            slot[n].type = p_union(p);
            n++;
        } while (eat_ch(p, ','));
    }

    if (!eat_ch(p, ')')) {
        fail(p, "expected )");
    }

    return n;
}

static bool p_mode(P *p, th_mode *m)
{
    if (eat_word(p, "inout")) {
        *m = TH_INOUT;

        return true;
    }
    if (eat_word(p, "out")) {
        *m = TH_OUT;

        return true;
    }
    if (eat_word(p, "in")) {
        *m = TH_IN;

        return true;
    }

    return false;
}

static th_type *p_function(P *p)
{
    eat_ch(p, '(');

    th_param   *params = nullptr;
    th_kwparam *kw     = nullptr;
    th_param    rest;
    size_t      np = 0, nkw = 0, cp = 0, ck = 0;
    bool        has_rest = false;

    if (!at_ch(p, ')')) {
        do {
            th_mode mode = TH_IN;

            p_mode(p, &mode);

            if (eat_ch(p, '*')) {
                if (has_rest) {
                    fail(p, "more than one rest parameter");
                    free(params);
                    free(kw);

                    return nullptr;
                }
                has_rest  = true;
                rest.mode = mode;
                rest.type = p_union(p);
            }
            else {
                size_t save = p->pos;
                bool   iskw = false;

                if (at_id(p)) {
                    size_t n;
                    size_t at = scan_id(p, p->pos, &n);
                    size_t q  = at + n;

                    while (q < p->len
                           && (p->src[q] == ' ' || p->src[q] == '\t')) {
                        q++;
                    }
                    iskw = n && q < p->len && p->src[q] == ':';
                }

                if (iskw) {
                    char *nm = take_id(p);

                    eat_ch(p, ':');
                    p_mode(p, &mode);

                    if (!reserve(&kw, &ck, nkw + 1, sizeof(th_kwparam))) {
                        fail(p, "out of memory");
                        free(nm);
                        free(params);
                        free(kw);

                        return nullptr;
                    }
                    kw[nkw].name = nm;
                    kw[nkw].mode = mode;
                    kw[nkw].type = p_union(p);
                    nkw++;
                }
                else {
                    p->pos = save;

                    if (!reserve(&params, &cp, np + 1, sizeof(th_param))) {
                        fail(p, "out of memory");
                        free(params);
                        free(kw);

                        return nullptr;
                    }
                    params[np].mode = mode;
                    params[np].type = p_union(p);
                    np++;
                }
            }
        } while (eat_ch(p, ','));
    }

    if (!eat_ch(p, ')') || !eat_arrow(p)) {
        fail(p, "expected ) ->");
        free(params);
        free(kw);

        return nullptr;
    }

    th_type *ret = p_union(p);
    th_type *r   = th_func(p->ctx, ret, params, np, has_rest ? &rest : nullptr,
                           kw, nkw);

    for (size_t i = 0; i < nkw; i++) {
        free((void *)kw[i].name);
    }
    free(params);
    free(kw);

    return r;
}

static th_type *p_paren(P *p)
{
    if (paren_is_function(p)) {
        return p_function(p);
    }

    if (looks_like_fields(p)) {
        th_member *m   = nullptr;
        size_t     cap = 0;
        size_t     n   = p_fields(p, &m, &cap);
        th_type   *r   = th_record(p->ctx, m, n);

        for (size_t i = 0; i < n; i++) {
            free((void *)m[i].name);
        }
        free(m);

        return r;
    }

    eat_ch(p, '(');

    th_type *inner = p_union(p);

    if (at_ch(p, ',')) {
        fail(p, "parentheses group a single type; a tuple is t(a, b)");

        return nullptr;
    }

    if (!eat_ch(p, ')')) {
        fail(p, "expected )");

        return nullptr;
    }

    return inner;
}

static th_type *p_anon_site(P *p)
{
    eat_ch(p, '@');

    char *path = take_str(p);

    if (!path || !eat_ch(p, ':')) {
        fail(p, "expected @\"path\":line:col");
        free(path);

        return nullptr;
    }

    uint64_t line = take_int(p);

    if (!eat_ch(p, ':')) {
        fail(p, "expected :col");
        free(path);

        return nullptr;
    }

    uint64_t    col = take_int(p);
    const char *nm  = th_anon_name(p->ctx, path, line, col);

    free(path);

    return th_name(p->ctx, nm);
}

static th_type *p_base(P *p)
{
    skip_ws(p);

    if (p->pos >= p->len) {
        fail(p, "unexpected end of input");

        return nullptr;
    }

    char c = p->src[p->pos];

    if (c == '`') {
        return p_tvar(p);
    }
    if (c == '@') {
        return p_anon_site(p);
    }
    if (c == '(') {
        return p_paren(p);
    }

    if (peek_word(p, "const") || peek_word(p, "volatile")) {
        fail(p, "not part of a type: mutability is a parameter mode or a "
                "property of storage, and atomic is a constructor, atomic[t]");

        return nullptr;
    }

    if (eat_word(p, "forall")) {
        return p_binder(p);
    }

    if (peek_word(p, "bitint") || peek_word(p, "ubitint")) {
        bool sg = peek_word(p, "bitint");

        eat_word(p, sg ? "bitint" : "ubitint");

        if (!eat_ch(p, '[')) {
            fail(p, "expected [");

            return nullptr;
        }

        uint64_t w = take_int(p);

        if (!eat_ch(p, ']')) {
            fail(p, "expected ]");

            return nullptr;
        }

        return th_bitint(p->ctx, w, sg);
    }

    if (at_id(p)) {
        size_t n;
        size_t at = scan_id(p, p->pos, &n);

        {
            bool    found;
            uint8_t sugar = sugar_attrs_n(p->src + at, n, &found);

            if (found) {
                size_t q = at + n;

                while (q < p->len && (p->src[q] == ' ' || p->src[q] == '\t')) {
                    q++;
                }

                if (q < p->len && p->src[q] == '[') {
                    p->pos = at + n;

                    return p_indirection(p, sugar);
                }
            }

            if (word_is(p->src + at, n, "array")) {
                size_t q = at + n;

                while (q < p->len && (p->src[q] == ' ' || p->src[q] == '\t')) {
                    q++;
                }

                if (q < p->len && p->src[q] == '[') {
                    p->pos = at + n;

                    return p_sequence(p);
                }
            }

            if (word_is(p->src + at, n, "t")) {
                size_t q = at + n;

                while (q < p->len && (p->src[q] == ' ' || p->src[q] == '\t')) {
                    q++;
                }

                if (q < p->len && p->src[q] == '(') {
                    return p_tuple(p);
                }
            }

            if (word_is(p->src + at, n, "variant")) {
                p->pos = at + n;

                return p_named(p, true);
            }

            if (is_reserved_n(p->src + at, n)) {
                fail(p, "reserved word is not a type name");

                return nullptr;
            }
        }

        return p_named(p, false);
    }

    fail(p, "cannot start a type");

    return nullptr;
}

static th_type *p_one(P *p)
{
    if (p->depth >= TH_MAX_PARSE_DEPTH) {
        fail(p, "type nests deeper than the parser will descend");

        return nullptr;
    }
    p->depth++;

    th_type *t = p_base(p);

    p->depth--;

    return t;
}

static th_type *p_isect(P *p)
{
    th_type *first = p_one(p);

    if (p->failed || !at_ch(p, '&')) {
        return first;
    }

    th_type **arms = nullptr;
    size_t    n    = 0;
    size_t    cap  = 0;

    if (!reserve(&arms, &cap, 1, sizeof(th_type *))) {
        fail(p, "out of memory");

        return nullptr;
    }
    arms[n++] = first;

    while (eat_ch(p, '&')) {
        if (!reserve(&arms, &cap, n + 1, sizeof(th_type *))) {
            fail(p, "out of memory");
            free(arms);

            return nullptr;
        }
        arms[n++] = p_one(p);
    }

    th_type *r = th_sum(p->ctx, TH_INTERSECTION, arms, n);

    free(arms);

    return r;
}

static th_type *p_union(P *p)
{
    th_type *first = p_isect(p);

    if (p->failed || !at_ch(p, '|')) {
        return first;
    }

    th_type **arms = nullptr;
    size_t    n    = 0;
    size_t    cap  = 0;

    if (!reserve(&arms, &cap, 1, sizeof(th_type *))) {
        fail(p, "out of memory");

        return nullptr;
    }
    arms[n++] = first;

    while (eat_ch(p, '|')) {
        if (!reserve(&arms, &cap, n + 1, sizeof(th_type *))) {
            fail(p, "out of memory");
            free(arms);

            return nullptr;
        }
        arms[n++] = p_isect(p);
    }

    th_type *r = th_sum(p->ctx, TH_UNION, arms, n);

    free(arms);

    return r;
}

static th_type *parse_common(th_ctx *ctx, const char *src, size_t len,
                             th_error *err, size_t *consumed)
{
    P p = {.src = src, .len = len, .ctx = ctx, .err = err};

    if (err) {
        err->msg    = nullptr;
        err->offset = 0;
    }

    th_type *t = p_union(&p);

    skip_ws(&p);

    if (!p.failed && !consumed && p.pos != p.len) {
        fail(&p, "trailing input");
    }

    for (size_t i = 0; i < p.nfree; i++) {
        free(p.freev[i].name);
    }
    free(p.freev);

    for (size_t i = 0; i < p.cb; i++) {
        for (size_t k = 0; k < p.binders[i].n; k++) {
            free(p.binders[i].names[k]);
        }
        free(p.binders[i].names);
    }
    free(p.binders);

    if (p.failed) {
        return nullptr;
    }

    if (consumed) {
        *consumed = p.pos;
    }

    return t;
}

th_type *th_parse(th_ctx *ctx, const char *src, th_error *err)
{
    return parse_common(ctx, src, strlen(src), err, nullptr);
}

/* The caller usually already knows how much input there is. Measuring it here
   instead makes parsing one type out of a long buffer cost the length of the
   whole buffer, and a caller in a loop pays that per type. */
th_type *th_parse_prefix_n(th_ctx *ctx, const char *src, size_t len,
                           size_t *consumed, th_error *err)
{
    size_t   n = 0;
    th_type *t = parse_common(ctx, src, len, err, &n);

    if (consumed) {
        *consumed = n;
    }

    return t;
}

th_type *th_parse_prefix(th_ctx *ctx, const char *src, size_t *consumed,
                         th_error *err)
{
    return th_parse_prefix_n(ctx, src, strlen(src), consumed, err);
}
