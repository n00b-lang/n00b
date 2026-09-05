#define XXH_INLINE_ALL
#include <xxhash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "typehash.h"
#include "th_internal.h"

void *th_alloc(th_ctx *ctx, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;

    if (!ctx->chunks || ctx->chunks->used + n > ctx->chunks->cap) {
        size_t cap = n > TH_CHUNK ? n : TH_CHUNK;
        th_chunk *c = calloc(1, sizeof(th_chunk) + cap);

        if (!c) {
            return nullptr;
        }
        c->cap      = cap;
        c->next     = ctx->chunks;
        ctx->chunks = c;
    }

    void *r = ctx->chunks->data + ctx->chunks->used;
    ctx->chunks->used += n;

    return r;
}

char *th_strdup(th_ctx *ctx, const char *s, size_t n)
{
    char *d = th_alloc(ctx, n + 1);

    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }

    return d;
}

th_ctx *th_ctx_new(void)
{
    return calloc(1, sizeof(th_ctx));
}

void th_ctx_free(th_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    th_chunk *c = ctx->chunks;

    while (c) {
        th_chunk *n = c->next;
        free(c);
        c = n;
    }

    free(ctx->scratch.p);
    free(ctx);
}

void th_ctx_set_normalizer(th_ctx *ctx, th_normalizer fn, void *state)
{
    ctx->norm       = fn;
    ctx->norm_state = state;
}

const char *th_last_error(const th_ctx *ctx)
{
    return ctx->err;
}

th_kind th_kind_of(const th_type *t)
{
    return t->kind;
}

bool th_is_scheme(const th_type *t)
{
    return t->kind == TH_BINDER;
}

static bool buf_need(th_buf *b, size_t n)
{
    if (b->len + n <= b->cap) {
        return true;
    }

    size_t cap = b->cap ? b->cap * 2 : 256;

    while (cap < b->len + n) {
        cap *= 2;
    }

    uint8_t *p = realloc(b->p, cap);

    if (!p) {
        return false;
    }
    b->p   = p;
    b->cap = cap;

    return true;
}

static bool put(th_buf *b, const void *src, size_t n)
{
    if (!buf_need(b, n)) {
        return false;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;

    return true;
}

static bool u8(th_buf *b, uint8_t v)
{
    return put(b, &v, 1);
}

static bool u32(th_buf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                    (uint8_t)v};

    return put(b, t, 4);
}

static bool u64(th_buf *b, uint64_t v)
{
    uint8_t t[8];

    for (int i = 0; i < 8; i++) {
        t[i] = (uint8_t)(v >> (56 - 8 * i));
    }

    return put(b, t, 8);
}

static bool str(th_buf *b, const char *s, size_t n)
{
    return u64(b, n) && put(b, s, n);
}

static bool is_start(unsigned char c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || c >= 0x80;
}

static bool is_cont(unsigned char c)
{
    return is_start(c) || (c >= '0' && c <= '9');
}

static bool valid_name(const char *s, size_t n)
{
    if (!n) {
        return false;
    }

    size_t seg = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '.') {
            if (!seg) {
                return false;
            }
            seg = 0;
            continue;
        }
        if (seg == 0 ? !is_start(c) : !is_cont(c)) {
            return false;
        }
        seg++;
    }

    return seg != 0;
}

static bool name(th_ctx *ctx, th_buf *b, const char *s, size_t n)
{
    if (!valid_name(s, n)) {
        ctx->err = "not a valid type name";

        return false;
    }

    if (ctx->norm) {
        const char *o;
        size_t      on;

        if (!ctx->norm(ctx->norm_state, s, n, &o, &on)) {
            ctx->err = "name normalization failed";

            return false;
        }

        return str(b, o, on);
    }

    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] >= 0x80) {
            ctx->err = "non-ASCII name requires an NFC normalizer; "
                       "call th_ctx_set_normalizer";

            return false;
        }
    }

    return str(b, s, n);
}

#define PM_DEAD ((const th_type *)(uintptr_t)1)

typedef struct {
    const th_type **key;
    size_t         *val;
    size_t          cap;
    size_t          used;
    const th_type **stack;
    size_t          scap;
    size_t          depth;
} th_path;

static size_t pm_slot(const th_type **key, size_t cap, const th_type *t)
{
    size_t h = (size_t)((uintptr_t)t >> 4) * 11400714819323198485u;
    size_t i = h & (cap - 1);

    while (key[i] && key[i] != t) {
        i = (i + 1) & (cap - 1);
    }

    return i;
}

static bool pm_room(th_path *p)
{
    if (p->cap && (p->used + 1) * 2 <= p->cap) {
        return true;
    }

    size_t          cap = p->cap ? p->cap * 2 : 64;
    const th_type **key = calloc(cap, sizeof(const th_type *));
    size_t         *val = calloc(cap, sizeof(size_t));

    if (!key || !val) {
        free(key);
        free(val);

        return false;
    }

    for (size_t i = 0; i < p->depth; i++) {
        size_t at = pm_slot(key, cap, p->stack[i]);

        key[at] = p->stack[i];
        val[at] = i;
    }
    free(p->key);
    free(p->val);
    p->key  = key;
    p->val  = val;
    p->cap  = cap;
    p->used = p->depth;

    return true;
}

static size_t pm_find(const th_path *p, const th_type *t)
{
    if (!p->cap) {
        return SIZE_MAX;
    }

    size_t at = pm_slot(p->key, p->cap, t);

    return p->key[at] == t ? p->val[at] : SIZE_MAX;
}

static bool pm_push(th_path *p, const th_type *t)
{
    if (!pm_room(p)) {
        return false;
    }

    if (p->depth + 1 > p->scap) {
        size_t          n = p->scap ? p->scap * 2 : 64;
        const th_type **q = realloc(p->stack, n * sizeof(const th_type *));

        if (!q) {
            return false;
        }
        p->stack = q;
        p->scap  = n;
    }

    size_t at = pm_slot(p->key, p->cap, t);

    p->key[at]          = t;
    p->val[at]          = p->depth;
    p->stack[p->depth++] = t;
    p->used++;

    return true;
}

static void pm_pop(th_path *p)
{
    const th_type *t  = p->stack[--p->depth];
    size_t         at = pm_slot(p->key, p->cap, t);

    if (p->key[at] == t) {
        p->key[at] = PM_DEAD;
    }
}

static void pm_free(th_path *p)
{
    free(p->key);
    free(p->val);
    free(p->stack);
}

typedef struct {
    th_path  path;
    bool     erase;
    th_ctx  *ctx;
    bool     ok;
} th_enc;

static void enc_node(th_enc *e, th_buf *b, const th_type *t);

void th_buf_free(th_buf *b)
{
    free(b->p);
    b->p   = nullptr;
    b->len = 0;
    b->cap = 0;
}

int th_cmp_slice(const void *a, const void *b)
{
    const th_slice *x = a, *y = b;
    size_t          m = x->n < y->n ? x->n : y->n;
    int             c = memcmp(x->p, y->p, m);

    if (c) {
        return c;
    }

    return x->n < y->n ? -1 : x->n > y->n ? 1 : 0;
}

static void enc_arg(th_enc *e, th_buf *b, const th_arg *a)
{
    switch (a->kind) {
    case TH_ARG_TYPE:
        e->ok &= u8(b, 0);
        enc_node(e, b, a->type);
        break;
    case TH_ARG_UINT:
        e->ok &= u8(b, 1) && u64(b, a->u);
        break;
    case TH_ARG_SINT:
        e->ok &= u8(b, 2) && u64(b, (uint64_t)a->s);
        break;
    case TH_ARG_STR:
        e->ok &= u8(b, 3) && str(b, a->str.p, a->str.n);
        break;
    }
}

static void enc_body(th_enc *e, th_buf *b, const th_type *t)
{
    e->ok &= u8(b, (uint8_t)(t->kind + 1));

    if (!e->ok) {
        return;
    }

    switch (t->kind) {
    case TH_NAMED:
        e->ok &= u8(b, t->named.is_variant ? 1 : 0)
              && name(e->ctx, b, t->named.name, t->named.nlen)
              && u64(b, t->named.nargs);

        for (size_t i = 0; e->ok && i < t->named.nargs; i++) {
            enc_arg(e, b, &t->named.args[i]);
        }
        break;

    case TH_BITINT:
        e->ok &= u64(b, t->bitint.width) && u8(b, t->bitint.is_signed ? 1 : 0);
        break;

    case TH_INDIRECTION:
        e->ok &= u8(b, t->ind.attrs);
        enc_node(e, b, t->ind.referent);
        break;

    case TH_SEQUENCE:
        if (t->seq.has_extent) {
            e->ok &= u8(b, 1) && u64(b, t->seq.extent);
        }
        else {
            e->ok &= u8(b, 0);
        }
        enc_node(e, b, t->seq.elem);
        break;

    case TH_RECORD: {
        size_t    n  = t->record.n;
        th_slice *sl = calloc(n ? n : 1, sizeof(th_slice));
        th_buf   *tb = calloc(n ? n : 1, sizeof(th_buf));

        for (size_t i = 0; i < n; i++) {
            e->ok &= name(e->ctx, &tb[i], t->record.members[i].name,
                          strlen(t->record.members[i].name));
            enc_node(e, &tb[i], t->record.members[i].type);
            sl[i].p = tb[i].p;
            sl[i].n = tb[i].len;
        }
        qsort(sl, n, sizeof(th_slice), th_cmp_slice);
        e->ok &= u64(b, n);

        for (size_t i = 0; e->ok && i < n; i++) {
            e->ok &= put(b, sl[i].p, sl[i].n);
        }

        for (size_t i = 0; i < n; i++) {
            free(tb[i].p);
        }
        free(tb);
        free(sl);
        break;
    }

    case TH_FUNCTION: {
        uint8_t flags = (uint8_t)((t->fn.rest ? 1 : 0) | (t->fn.nkw ? 2 : 0));

        e->ok &= u8(b, flags);
        enc_node(e, b, t->fn.ret);
        e->ok &= u64(b, t->fn.np);

        for (size_t i = 0; e->ok && i < t->fn.np; i++) {
            e->ok &= u8(b, t->fn.params[i].mode);
            enc_node(e, b, t->fn.params[i].type);
        }

        if (t->fn.rest) {
            e->ok &= u8(b, t->fn.rest->mode);
            enc_node(e, b, t->fn.rest->type);
        }

        if (t->fn.nkw) {
            size_t    n  = t->fn.nkw;
            th_slice *sl = calloc(n, sizeof(th_slice));
            th_buf   *tb = calloc(n, sizeof(th_buf));

            for (size_t i = 0; i < n; i++) {
                e->ok &= name(e->ctx, &tb[i], t->fn.kw[i].name,
                              strlen(t->fn.kw[i].name))
                      && u8(&tb[i], t->fn.kw[i].mode);
                enc_node(e, &tb[i], t->fn.kw[i].type);
                sl[i].p = tb[i].p;
                sl[i].n = tb[i].len;
            }
            qsort(sl, n, sizeof(th_slice), th_cmp_slice);
            e->ok &= u64(b, n);

            for (size_t i = 0; e->ok && i < n; i++) {
                e->ok &= put(b, sl[i].p, sl[i].n);
            }

            for (size_t i = 0; i < n; i++) {
                free(tb[i].p);
            }
            free(tb);
            free(sl);
        }
        break;
    }

    case TH_BINDER:
        e->ok &= u64(b, t->binder.arity);
        enc_node(e, b, t->binder.body);
        break;

    case TH_BOUND:
        e->ok &= u64(b, t->bound.distance) && u64(b, t->bound.position);
        break;

    case TH_VAR:
        if (e->erase) {
            e->ok &= u64(b, 0);
        }
        else if (!t->var.has_index) {
            e->ctx->err = "free variable has no index; normalize first";
            e->ok       = false;
        }
        else {
            e->ok &= u64(b, t->var.index);
        }
        break;

    case TH_SUM: {
        size_t    n  = t->sum.n;
        th_slice *sl = calloc(n ? n : 1, sizeof(th_slice));
        th_buf   *tb = calloc(n ? n : 1, sizeof(th_buf));

        for (size_t i = 0; i < n; i++) {
            enc_node(e, &tb[i], t->sum.arms[i]);
            sl[i].p = tb[i].p;
            sl[i].n = tb[i].len;
        }
        qsort(sl, n, sizeof(th_slice), th_cmp_slice);
        e->ok &= u8(b, t->sum.comb) && u64(b, n);

        for (size_t i = 0; e->ok && i < n; i++) {
            e->ok &= put(b, sl[i].p, sl[i].n);
        }

        for (size_t i = 0; i < n; i++) {
            free(tb[i].p);
        }
        free(tb);
        free(sl);
        break;
    }
    }
}

static void enc_node(th_enc *e, th_buf *b, const th_type *t)
{
    if (!e->ok) {
        return;
    }

    t = th_resolve_c(t);

    if (!t) {
        e->ok &= u8(b, 0xFF);

        return;
    }

    size_t back = pm_find(&e->path, t);

    if (back != SIZE_MAX) {
        e->ok &= u8(b, 0xFE) && u64(b, e->path.depth - back);

        return;
    }

    if (!pm_push(&e->path, t)) {
        e->ok = false;

        return;
    }
    enc_body(e, b, t);
    pm_pop(&e->path);
}

typedef struct {
    th_type **seen;
    size_t    n;
    size_t    cap;
    th_path   path;
    th_ctx   *ctx;
} th_norm;

static void norm_walk(th_norm *s, th_type *t);

void th_erase_encode(th_ctx *ctx, const th_type *t, th_buf *out)
{
    th_enc e = {.ctx = ctx, .erase = true, .ok = true};

    enc_node(&e, out, t);
    pm_free(&e.path);
}

void th_full_encode(th_ctx *ctx, const th_type *t, th_buf *out)
{
    th_enc e = {.ctx = ctx, .ok = true};

    enc_node(&e, out, t);
    pm_free(&e.path);
}

static void norm_children_sum(th_norm *s, th_type *t)
{
    size_t    n  = t->sum.n;
    th_slice *sl = calloc(n ? n : 1, sizeof(th_slice));
    th_buf   *tb = calloc(n ? n : 1, sizeof(th_buf));
    th_type **ar = calloc(n ? n : 1, sizeof(th_type *));

    for (size_t i = 0; i < n; i++) {
        th_erase_encode(s->ctx, t->sum.arms[i], &tb[i]);
        sl[i].p = tb[i].p;
        sl[i].n = tb[i].len;
        ar[i]   = t->sum.arms[i];
    }

    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (th_cmp_slice(&sl[j], &sl[i]) < 0) {
                th_slice ts = sl[i];
                th_type *tt = ar[i];
                sl[i]       = sl[j];
                sl[j]       = ts;
                ar[i]       = ar[j];
                ar[j]       = tt;
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        norm_walk(s, ar[i]);
    }

    for (size_t i = 0; i < n; i++) {
        free(tb[i].p);
    }
    free(tb);
    free(sl);
    free(ar);
}

int th_cmp_member(const void *a, const void *b)
{
    const th_member *x = a, *y = b;
    size_t           xn = strlen(x->name), yn = strlen(y->name);

    if (xn != yn) {
        return xn < yn ? -1 : 1;
    }

    return memcmp(x->name, y->name, xn);
}

int th_cmp_kw(const void *a, const void *b)
{
    const th_kwparam *x = a, *y = b;
    size_t            xn = strlen(x->name), yn = strlen(y->name);

    if (xn != yn) {
        return xn < yn ? -1 : 1;
    }

    return memcmp(x->name, y->name, xn);
}

static void norm_walk(th_norm *s, th_type *t)
{
    t = (th_type *)th_resolve_c(t);

    if (!t) {
        return;
    }

    if (pm_find(&s->path, t) != SIZE_MAX) {
        return;
    }

    if (!pm_push(&s->path, t)) {
        return;
    }

    switch (t->kind) {
    case TH_VAR:
        for (size_t i = 0; i < s->n; i++) {
            if (s->seen[i] == t) {
                goto done;
            }
        }

        if (s->n == s->cap) {
            s->cap  = s->cap ? s->cap * 2 : 16;
            s->seen = realloc(s->seen, s->cap * sizeof(th_type *));
        }
        t->var.index     = s->n;
        t->var.has_index = true;
        s->seen[s->n++]  = t;
        break;

    case TH_NAMED:
        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                norm_walk(s, t->named.args[i].type);
            }
        }
        break;

    case TH_INDIRECTION:
        norm_walk(s, t->ind.referent);
        break;

    case TH_SEQUENCE:
        norm_walk(s, t->seq.elem);
        break;

    case TH_RECORD: {
        th_member *m = calloc(t->record.n ? t->record.n : 1, sizeof(th_member));

        memcpy(m, t->record.members, t->record.n * sizeof(th_member));
        qsort(m, t->record.n, sizeof(th_member), th_cmp_member);

        for (size_t i = 0; i < t->record.n; i++) {
            norm_walk(s, m[i].type);
        }
        free(m);
        break;
    }

    case TH_FUNCTION: {
        norm_walk(s, t->fn.ret);

        for (size_t i = 0; i < t->fn.np; i++) {
            norm_walk(s, t->fn.params[i].type);
        }

        if (t->fn.rest) {
            norm_walk(s, t->fn.rest->type);
        }

        if (t->fn.nkw) {
            th_kwparam *k = calloc(t->fn.nkw, sizeof(th_kwparam));

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                norm_walk(s, k[i].type);
            }
            free(k);
        }
        break;
    }

    case TH_BINDER:
        norm_walk(s, t->binder.body);
        break;

    case TH_SUM:
        norm_children_sum(s, t);
        break;

    default:
        break;
    }

done:
    pm_pop(&s->path);
}

void th_normalize(th_type *t)
{
    th_ctx  tmp = {0};
    th_norm s   = {.ctx = &tmp};

    norm_walk(&s, t);
    free(s.seen);
    pm_free(&s.path);
}

bool th_encode(th_ctx *ctx, th_type *t, const uint8_t **out, size_t *len)
{
    th_normalize(t);
    ctx->scratch.len = 0;

    th_enc e = {.ctx = ctx, .ok = true};

    enc_node(&e, &ctx->scratch, t);
    pm_free(&e.path);

    if (!e.ok) {
        return false;
    }
    *out = ctx->scratch.p;
    *len = ctx->scratch.len;

    return true;
}

uint64_t th_hash(th_ctx *ctx, th_type *t)
{
    const uint8_t *p;
    size_t         n;

    if (!th_encode(ctx, t, &p, &n)) {
        return 0;
    }

    th_buf b = {0};

    u32(&b, TH_VERSION);
    put(&b, p, n);

    XXH128_hash_t h = XXH3_128bits(b.p, b.len);

    free(b.p);

    return h.low64;
}

uint64_t th_arm_tag(const char *n)
{
    uint64_t t = XXH3_64bits(n, strlen(n));

    return t ? t : 1;
}

const char *th_anon_name(th_ctx *ctx, const char *path, uint64_t line,
                         uint64_t col)
{
    th_buf b = {0};

    str(&b, path, strlen(path));
    u64(&b, line);
    u64(&b, col);

    XXH128_hash_t h = XXH3_128bits(b.p, b.len);
    uint64_t      d = h.low64 ? h.low64 : 1;

    free(b.p);

    char *s = th_alloc(ctx, 32);

    snprintf(s, 32, "_anon_%016llx", (unsigned long long)d);

    return s;
}

static th_type *node(th_ctx *ctx, th_kind k)
{
    th_type *t = th_alloc(ctx, sizeof(th_type));

    if (t) {
        t->kind = k;
    }

    return t;
}

th_type *th_name(th_ctx *ctx, const char *n)
{
    return th_apply(ctx, n, nullptr, 0);
}

static th_type *make_named(th_ctx *ctx, const char *n, const th_arg *a,
                           size_t cnt, bool variant)
{
    th_type *t = node(ctx, TH_NAMED);

    if (!t) {
        return nullptr;
    }

    size_t ln = strlen(n);

    t->named.name       = th_strdup(ctx, n, ln);
    t->named.nlen       = ln;
    t->named.is_variant = variant;
    t->named.nargs      = cnt;

    if (cnt) {
        t->named.args = th_alloc(ctx, cnt * sizeof(th_arg));
        memcpy(t->named.args, a, cnt * sizeof(th_arg));

        for (size_t i = 0; i < cnt; i++) {
            if (t->named.args[i].kind == TH_ARG_STR) {
                t->named.args[i].str.p = th_strdup(ctx, a[i].str.p,
                                                   a[i].str.n);
            }
        }
    }

    return t;
}

th_type *th_apply(th_ctx *ctx, const char *n, const th_arg *a, size_t cnt)
{
    return make_named(ctx, n, a, cnt, false);
}

th_type *th_variant(th_ctx *ctx, const char *n, const th_arg *a, size_t cnt)
{
    return make_named(ctx, n, a, cnt, true);
}

th_type *th_bitint(th_ctx *ctx, uint64_t w, bool sg)
{
    th_type *t = node(ctx, TH_BITINT);

    t->bitint.width     = w;
    t->bitint.is_signed = sg;

    return t;
}

th_type *th_ind(th_ctx *ctx, uint8_t attrs, th_type *r)
{
    th_type *t = node(ctx, TH_INDIRECTION);

    t->ind.attrs    = attrs;
    t->ind.referent = r;

    return t;
}

th_type *th_array(th_ctx *ctx, th_type *e)
{
    th_type *t = node(ctx, TH_SEQUENCE);

    t->seq.elem = e;

    return t;
}

th_type *th_array_n(th_ctx *ctx, th_type *e, uint64_t n)
{
    th_type *t = th_array(ctx, e);

    t->seq.has_extent = true;
    t->seq.extent     = n;

    return t;
}

th_type *th_record(th_ctx *ctx, const th_member *m, size_t n)
{
    th_type *t = node(ctx, TH_RECORD);

    t->record.n = n;

    if (n) {
        t->record.members = th_alloc(ctx, n * sizeof(th_member));

        for (size_t i = 0; i < n; i++) {
            t->record.members[i].name = th_strdup(ctx, m[i].name,
                                                  strlen(m[i].name));
            t->record.members[i].type = m[i].type;
        }
    }

    return t;
}

th_type *th_record_stub(th_ctx *ctx)
{
    return node(ctx, TH_RECORD);
}

void th_record_fill(th_ctx *ctx, th_type *stub, const th_member *m, size_t n)
{
    stub->record.n       = n;
    stub->record.members = nullptr;

    if (n) {
        stub->record.members = th_alloc(ctx, n * sizeof(th_member));

        for (size_t i = 0; i < n; i++) {
            stub->record.members[i].name = th_strdup(ctx, m[i].name,
                                                     strlen(m[i].name));
            stub->record.members[i].type = m[i].type;
        }
    }
}

th_type *th_func(th_ctx *ctx, th_type *ret, const th_param *p, size_t np,
                 const th_param *rest, const th_kwparam *kw, size_t nkw)
{
    th_type *t = node(ctx, TH_FUNCTION);

    t->fn.ret = ret;
    t->fn.np  = np;

    if (np) {
        t->fn.params = th_alloc(ctx, np * sizeof(th_param));
        memcpy(t->fn.params, p, np * sizeof(th_param));
    }

    if (rest) {
        t->fn.rest  = th_alloc(ctx, sizeof(th_param));
        *t->fn.rest = *rest;
    }

    t->fn.nkw = nkw;

    if (nkw) {
        t->fn.kw = th_alloc(ctx, nkw * sizeof(th_kwparam));

        for (size_t i = 0; i < nkw; i++) {
            t->fn.kw[i].name = th_strdup(ctx, kw[i].name, strlen(kw[i].name));
            t->fn.kw[i].mode = kw[i].mode;
            t->fn.kw[i].type = kw[i].type;
        }
    }

    return t;
}

th_type *th_binder(th_ctx *ctx, uint64_t arity, th_type *body)
{
    th_type *t = node(ctx, TH_BINDER);

    t->binder.arity = arity;
    t->binder.body  = body;

    return t;
}

th_type *th_bound(th_ctx *ctx, uint64_t d, uint64_t p)
{
    th_type *t = node(ctx, TH_BOUND);

    t->bound.distance = d;
    t->bound.position = p;

    return t;
}

th_type *th_var(th_ctx *ctx)
{
    th_type *t = node(ctx, TH_VAR);

    t->var.has_index = false;
    t->var.index     = ctx->next_var++;
    t->var.ref       = nullptr;

    return t;
}

th_type *th_tuple(th_ctx *ctx, th_type *const *e, size_t n)
{
    th_arg *a = calloc(n ? n : 1, sizeof(th_arg));

    for (size_t i = 0; i < n; i++) {
        a[i].kind = TH_ARG_TYPE;
        a[i].type = e[i];
    }

    th_type *t = th_apply(ctx, TH_TUPLE, a, n);

    free(a);

    return t;
}

th_type *th_sum(th_ctx *ctx, th_comb comb, th_type *const *arms, size_t n)
{
    th_type **flat = calloc(n ? n : 1, sizeof(th_type *));
    size_t    fn = 0, cap = n ? n : 1;

    for (size_t i = 0; i < n; i++) {
        th_type *a = arms[i];

        if (a->kind == TH_SUM && a->sum.comb == comb) {
            for (size_t j = 0; j < a->sum.n; j++) {
                if (fn == cap) {
                    cap *= 2;
                    flat = realloc(flat, cap * sizeof(th_type *));
                }
                flat[fn++] = a->sum.arms[j];
            }
        }
        else {
            if (fn == cap) {
                cap *= 2;
                flat = realloc(flat, cap * sizeof(th_type *));
            }
            flat[fn++] = a;
        }
    }

    th_type **uniq = calloc(fn ? fn : 1, sizeof(th_type *));
    th_buf   *enc  = calloc(fn ? fn : 1, sizeof(th_buf));
    size_t    un   = 0;

    for (size_t i = 0; i < fn; i++) {
        th_buf b = {0};

        th_erase_encode(ctx, flat[i], &b);

        bool dup = false;

        for (size_t j = 0; j < un; j++) {
            if (enc[j].len == b.len && !memcmp(enc[j].p, b.p, b.len)) {
                dup = true;
                break;
            }
        }

        if (dup) {
            free(b.p);
        }
        else {
            enc[un]    = b;
            uniq[un++] = flat[i];
        }
    }

    th_type *r;

    if (un == 1) {
        r = uniq[0];
    }
    else {
        r            = node(ctx, TH_SUM);
        r->sum.comb  = comb;
        r->sum.n     = un;
        r->sum.arms  = th_alloc(ctx, un * sizeof(th_type *));
        memcpy(r->sum.arms, uniq, un * sizeof(th_type *));
    }

    for (size_t i = 0; i < un; i++) {
        free(enc[i].p);
    }
    free(enc);
    free(uniq);
    free(flat);

    return r;
}
