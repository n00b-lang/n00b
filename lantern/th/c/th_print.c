#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "th_internal.h"

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} pb;

static void pb_put(pb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;

        while (cap < b->len + n + 1) {
            cap *= 2;
        }

        char *q = realloc(b->p, cap);

        if (!q) {
            return;
        }
        b->p   = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void pb_str(pb *b, const char *s)
{
    pb_put(b, s, strlen(s));
}

static void pb_fmt(pb *b, const char *fmt, ...)
{
    va_list ap;
    va_list copy;

    va_start(ap, fmt);
    va_copy(copy, ap);

    int n = vsnprintf(nullptr, 0, fmt, ap);

    va_end(ap);

    if (n < 0) {
        va_end(copy);

        return;
    }

    if (b->len + (size_t)n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;

        while (cap < b->len + (size_t)n + 1) {
            cap *= 2;
        }

        char *q = realloc(b->p, cap);

        if (!q) {
            va_end(copy);

            return;
        }
        b->p   = q;
        b->cap = cap;
    }
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, copy);
    va_end(copy);
    b->len += (size_t)n;
}

typedef struct {
    th_ctx *ctx;
    size_t  depth;
} pctx;

static void varname(pb *b, size_t depth, uint64_t pos)
{
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
    uint64_t          n       = depth * 8 + pos;

    pb_fmt(b, "%c", alpha[n % 26]);

    if (n >= 26) {
        pb_fmt(b, "%" PRIu64, n / 26);
    }
}

static void emit(pctx *c, pb *b, const th_type *t);

static const struct {
    const char *name;
    uint8_t     attrs;
} sugar[] = {
    {"ref", 0x00},
    {"ptr", TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE},
    {"mutref", TH_EXCLUSIVE},
    {"own", TH_OWNING},
    {"gcref", TH_NULLABLE | TH_RESEATABLE},
    {"tmpref", TH_TEMPORARY},
};

static const struct {
    const char *name;
    uint8_t     bit;
} attr_names[] = {
    {"nullable", TH_NULLABLE},   {"reseatable", TH_RESEATABLE},
    {"indexable", TH_INDEXABLE}, {"owning", TH_OWNING},
    {"exclusive", TH_EXCLUSIVE}, {"temporary", TH_TEMPORARY},
};

static const char *mode_name(th_mode m)
{
    return m == TH_OUT ? "out " : m == TH_INOUT ? "inout " : "";
}

typedef struct {
    uint8_t *key;
    size_t   keylen;
    size_t   index;
} ordered;

static int cmp_ordered(const void *a, const void *b)
{
    const ordered *x = a, *y = b;
    th_slice       sx = {.p = x->key, .n = x->keylen};
    th_slice       sy = {.p = y->key, .n = y->keylen};

    return th_cmp_slice(&sx, &sy);
}

static void emit_arg(pctx *c, pb *b, const th_arg *a)
{
    switch (a->kind) {
    case TH_ARG_TYPE:
        emit(c, b, a->type);
        break;
    case TH_ARG_UINT:
        pb_fmt(b, "%" PRIu64, a->u);
        break;
    case TH_ARG_SINT:
        pb_fmt(b, "%" PRId64, a->s);
        break;
    case TH_ARG_STR:
        pb_str(b, "\"");
        pb_put(b, a->str.p, a->str.n);
        pb_str(b, "\"");
        break;
    }
}

static void emit_record(pctx *c, pb *b, const th_type *t)
{
    size_t   n = t->record.n;
    ordered *o = calloc(n ? n : 1, sizeof(ordered));

    for (size_t i = 0; i < n; i++) {
        const char *nm  = t->record.members[i].name;
        size_t      len = strlen(nm);
        uint8_t     hdr[8];
        pb          key = {0};
        th_buf      enc = {0};

        for (int k = 0; k < 8; k++) {
            hdr[k] = (uint8_t)(len >> (56 - 8 * k));
        }
        pb_put(&key, (const char *)hdr, 8);
        pb_put(&key, nm, len);

        th_full_encode(c->ctx, t->record.members[i].type, &enc);
        pb_put(&key, (const char *)enc.p, enc.len);
        th_buf_free(&enc);

        o[i].key    = (uint8_t *)key.p;
        o[i].keylen = key.len;
        o[i].index  = i;
    }
    qsort(o, n, sizeof(ordered), cmp_ordered);

    pb_str(b, "(");

    for (size_t i = 0; i < n; i++) {
        const th_member *m = &t->record.members[o[i].index];

        pb_fmt(b, "%s%s: ", i ? ", " : "", m->name);
        emit(c, b, m->type);
    }
    pb_str(b, ")");

    for (size_t i = 0; i < n; i++) {
        free(o[i].key);
    }
    free(o);
}

static void emit_function(pctx *c, pb *b, const th_type *t)
{
    pb_str(b, "(");

    bool first = true;

    for (size_t i = 0; i < t->fn.np; i++) {
        pb_str(b, first ? "" : ", ");
        first = false;
        pb_str(b, mode_name(t->fn.params[i].mode));
        emit(c, b, t->fn.params[i].type);
    }

    if (t->fn.rest) {
        pb_str(b, first ? "" : ", ");
        first = false;
        pb_str(b, mode_name(t->fn.rest->mode));
        pb_str(b, "*");
        emit(c, b, t->fn.rest->type);
    }

    if (t->fn.nkw) {
        size_t      n = t->fn.nkw;
        th_kwparam *k = calloc(n, sizeof(th_kwparam));

        memcpy(k, t->fn.kw, n * sizeof(th_kwparam));
        qsort(k, n, sizeof(th_kwparam), th_cmp_kw);

        for (size_t i = 0; i < n; i++) {
            pb_str(b, first ? "" : ", ");
            first = false;
            pb_fmt(b, "%s: %s", k[i].name, mode_name(k[i].mode));
            emit(c, b, k[i].type);
        }
        free(k);
    }
    pb_str(b, ") -> ");
    emit(c, b, t->fn.ret);
}

static void emit_sum(pctx *c, pb *b, const th_type *t)
{
    size_t   n = t->sum.n;
    ordered *o = calloc(n ? n : 1, sizeof(ordered));

    for (size_t i = 0; i < n; i++) {
        th_buf enc = {0};

        th_full_encode(c->ctx, t->sum.arms[i], &enc);
        o[i].key    = enc.p;
        o[i].keylen = enc.len;
        o[i].index  = i;
    }
    qsort(o, n, sizeof(ordered), cmp_ordered);

    const char *op = t->sum.comb == TH_UNION ? " | " : " & ";

    pb_str(b, "(");

    for (size_t i = 0; i < n; i++) {
        pb_str(b, i ? op : "");
        emit(c, b, t->sum.arms[o[i].index]);
    }
    pb_str(b, ")");

    for (size_t i = 0; i < n; i++) {
        free(o[i].key);
    }
    free(o);
}

static void emit(pctx *c, pb *b, const th_type *t)
{
    t = th_resolve_c(t);

    if (!t) {
        pb_str(b, "?");

        return;
    }

    switch (t->kind) {
    case TH_NAMED: {
        bool all_types = true;

        for (size_t i = 0; i < t->named.nargs; i++) {
            all_types &= t->named.args[i].kind == TH_ARG_TYPE;
        }

        if (!t->named.is_variant && all_types
            && t->named.nlen == strlen(TH_TUPLE)
            && !memcmp(t->named.name, TH_TUPLE, t->named.nlen)) {
            pb_str(b, "t(");

            for (size_t i = 0; i < t->named.nargs; i++) {
                pb_str(b, i ? ", " : "");
                emit(c, b, t->named.args[i].type);
            }
            pb_str(b, ")");

            return;
        }

        if (t->named.is_variant) {
            pb_str(b, "variant ");
        }
        pb_put(b, t->named.name, t->named.nlen);

        if (t->named.nargs) {
            pb_str(b, "[");

            for (size_t i = 0; i < t->named.nargs; i++) {
                pb_str(b, i ? ", " : "");
                emit_arg(c, b, &t->named.args[i]);
            }
            pb_str(b, "]");
        }

        return;
    }

    case TH_BITINT:
        pb_fmt(b, "%s[%" PRIu64 "]",
               t->bitint.is_signed ? "bitint" : "ubitint", t->bitint.width);

        return;

    case TH_INDIRECTION: {
        for (size_t i = 0; i < sizeof(sugar) / sizeof(sugar[0]); i++) {
            if (sugar[i].attrs == t->ind.attrs) {
                pb_fmt(b, "%s[", sugar[i].name);
                emit(c, b, t->ind.referent);
                pb_str(b, "]");

                return;
            }
        }
        pb_str(b, "ref[");
        emit(c, b, t->ind.referent);
        pb_str(b, "; ");

        bool first = true;

        for (size_t i = 0; i < sizeof(attr_names) / sizeof(attr_names[0]);
             i++) {
            if (t->ind.attrs & attr_names[i].bit) {
                pb_fmt(b, "%s%s", first ? "" : ", ", attr_names[i].name);
                first = false;
            }
        }
        pb_str(b, "]");

        return;
    }

    case TH_SEQUENCE:
        pb_str(b, "array[");
        emit(c, b, t->seq.elem);

        if (t->seq.has_extent) {
            pb_fmt(b, "; %" PRIu64 "]", t->seq.extent);
        }
        else {
            pb_str(b, "]");
        }

        return;

    case TH_RECORD:
        emit_record(c, b, t);

        return;

    case TH_FUNCTION:
        emit_function(c, b, t);

        return;

    case TH_BINDER: {
        pb_str(b, "forall[");

        for (uint64_t i = 0; i < t->binder.arity; i++) {
            pb_str(b, i ? ", `" : "`");
            varname(b, c->depth, i);
        }
        pb_str(b, "] ");
        c->depth++;
        emit(c, b, t->binder.body);
        c->depth--;

        return;
    }

    case TH_BOUND: {
        pb_str(b, "`");

        if (t->bound.distance == 0 || t->bound.distance > c->depth) {
            pb_fmt(b, "?%" PRIu64, t->bound.position);

            return;
        }
        varname(b, c->depth - t->bound.distance, t->bound.position);

        return;
    }

    case TH_VAR:
        pb_str(b, "`");

        if (!t->var.has_index) {
            pb_str(b, "?");

            return;
        }
        varname(b, 0, t->var.index);

        return;

    case TH_SUM:
        emit_sum(c, b, t);

        return;

    default:
        pb_str(b, "?");
    }
}

const char *th_print(th_ctx *ctx, th_type *type)
{
    if (!ctx || !type) {
        return nullptr;
    }
    th_normalize(type);

    pctx  c = {.ctx = ctx};
    pb    b = {0};

    emit(&c, &b, type);

    if (!b.p) {
        return th_strdup(ctx, "", 0);
    }

    char *out = th_strdup(ctx, b.p, b.len);

    free(b.p);

    return out;
}
