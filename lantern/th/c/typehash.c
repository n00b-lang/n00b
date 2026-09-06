#define XXH_INLINE_ALL
#include <xxhash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "typehash.h"
#include "th_internal.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"

void *th_alloc(th_ctx *ctx, size_t n)
{
    return ncc_arena_alloc(&ctx->arena, n);
}

char *th_strdup(th_ctx *ctx, const char *s, size_t n)
{
    char *d = th_alloc(ctx, n + 1);

    memcpy(d, s, n);
    d[n] = 0;

    return d;
}

th_ctx *th_ctx_new(void)
{
    th_ctx *ctx = ncc_alloc(th_ctx);

    ncc_arena_init(&ctx->arena, 0);

    return ctx;
}

void th_ctx_free(th_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    ncc_arena_free(&ctx->arena);
    ncc_buffer_release(&ctx->scratch);
    ncc_free(ctx);
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

static void u8(ncc_buffer_t *b, uint8_t v)
{
    ncc_buffer_append(b, (const char *)&v, 1);
}

static void u64(ncc_buffer_t *b, uint64_t v)
{
    char t[8];

    for (int i = 0; i < 8; i++) {
        t[i] = (char)(v >> (56 - 8 * i));
    }

    ncc_buffer_append(b, t, 8);
}

static void str(ncc_buffer_t *b, const char *s, size_t n)
{
    u64(b, n);
    ncc_buffer_append(b, s, n);
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

static bool name(th_ctx *ctx, ncc_buffer_t *b, const char *s, size_t n)
{
    if (!valid_name(s, n)) {
        th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
               "not a valid type name");

        return false;
    }

    if (ctx->norm) {
        const char *o;
        size_t      on;

        if (!ctx->norm(ctx->norm_state, s, n, &o, &on)) {
            th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
                   "name normalization failed");

            return false;
        }

        str(b, o, on);

        return true;
    }

    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] >= 0x80) {
            th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
                   "non-ASCII name requires an NFC normalizer; "
                   "call th_ctx_set_normalizer");

            return false;
        }
    }

    str(b, s, n);

    return true;
}

typedef th_type       *th_type_ptr_t;
typedef const th_type *th_type_cptr_t;

ncc_list_decl(th_type_ptr_t);

ncc_option_decl(th_type_cptr_t);
ncc_list_decl(th_type_cptr_t);

typedef struct {
    ncc_dict_t                 at;
    ncc_list_t(th_type_cptr_t) stack;
} th_path;

static void pm_init(th_path *p)
{
    ncc_dict_init(&p->at, nullptr, nullptr);
    p->stack = ncc_list_new(th_type_cptr_t);
}

static size_t pm_depth(const th_path *p)
{
    return p->stack.len;
}

static size_t pm_find(const th_path *p, const th_type *t)
{
    bool   found;
    size_t at = (size_t)(uintptr_t)ncc_dict_get((ncc_dict_t *)&p->at, t,
                                               &found);

    return found ? at - 1 : SIZE_MAX;
}

static void pm_push(th_path *p, const th_type *t)
{
    ncc_dict_put(&p->at, t, p->stack.len + 1);
    ncc_list_push(p->stack, t);
}

static void pm_pop(th_path *p)
{
    const th_type *t = ncc_option_get(ncc_list_pop(th_type_cptr_t, p->stack));

    ncc_dict_remove(&p->at, t);
}

static void pm_free(th_path *p)
{
    ncc_dict_free(&p->at);
    ncc_list_free(p->stack);
}

typedef struct {
    th_path  path;
    bool     erase;
    th_ctx  *ctx;
    bool     ok;
} th_enc;

static void enc_node(th_enc *e, ncc_buffer_t *b, const th_type *t);

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

typedef struct {
    ncc_buffer_t   pre;
    const th_type *child;
} estep;

ncc_option_decl(estep);
ncc_list_decl(estep);

typedef struct {
    const th_type    *t;
    ncc_buffer_t     *out;
    ncc_list_t(estep) steps;
    size_t            ninline;
    size_t            at;
    ncc_buffer_t     *side;
    ncc_buffer_t      post;
    bool              entered;
    bool              pushed;
} eframe;

ncc_option_decl(eframe);
ncc_list_decl(eframe);

static estep *step_add(ncc_list_t(estep) *v, const th_type *child)
{
    ncc_list_push(*v, ((estep){.child = child}));

    return &v->data[v->len - 1];
}

static bool enc_plan(th_enc *e, eframe *f)
{
    const th_type *t = f->t;
    estep         *s = nullptr;

    f->steps   = ncc_list_new(estep);
    f->ninline = 0;

    s = step_add(&f->steps, nullptr);

    u8(&s->pre, (uint8_t)(t->kind + 1));

    switch (t->kind) {
    case TH_NAMED:
        u8(&s->pre, t->named.is_variant ? 1 : 0);

        if (!name(e->ctx, &s->pre, t->named.name, t->named.nlen)) {
            return false;
        }

        u64(&s->pre, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            const th_arg *a  = &t->named.args[i];
            bool          ty = a->kind == TH_ARG_TYPE;

            s = step_add(&f->steps, ty ? a->type : nullptr);

            switch (a->kind) {
            case TH_ARG_TYPE:
                u8(&s->pre, 0);
                break;
            case TH_ARG_UINT:
                u8(&s->pre, 1);
                u64(&s->pre, a->u);
                break;
            case TH_ARG_SINT:
                u8(&s->pre, 2);
                u64(&s->pre, (uint64_t)a->s);
                break;
            case TH_ARG_STR:
                u8(&s->pre, 3);
                str(&s->pre, a->str.p, a->str.n);
                break;
            }
        }
        break;

    case TH_BITINT:
        u64(&s->pre, t->bitint.width);
        u8(&s->pre, t->bitint.is_signed ? 1 : 0);
        break;

    case TH_INDIRECTION:
        s->child = t->ind.referent;
        u8(&s->pre, t->ind.attrs);
        break;

    case TH_SEQUENCE:
        s->child = t->seq.elem;

        if (t->seq.has_extent) {
            u8(&s->pre, 1);
            u64(&s->pre, t->seq.extent);
        }
        else {
            u8(&s->pre, 0);
        }
        break;

    case TH_RECORD:
        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->record.n; i++) {
            const th_member *m = &t->record.members[i];

            s = step_add(&f->steps, m->type);

            if (!name(e->ctx, &s->pre, m->name, strlen(m->name))) {
                return false;
            }
        }

        u64(&f->post, t->record.n);
        break;

    case TH_FUNCTION: {
        uint8_t flags = (uint8_t)((t->fn.rest ? 1 : 0) | (t->fn.nkw ? 2 : 0));

        s->child = t->fn.ret;
        u8(&s->pre, flags);

        s = step_add(&f->steps, t->fn.np ? t->fn.params[0].type : nullptr);

        u64(&s->pre, t->fn.np);

        if (t->fn.np) {
            u8(&s->pre, t->fn.params[0].mode);
        }

        for (size_t i = 1; i < t->fn.np; i++) {
            s = step_add(&f->steps, t->fn.params[i].type);
            u8(&s->pre, t->fn.params[i].mode);
        }

        if (t->fn.rest) {
            s = step_add(&f->steps, t->fn.rest->type);
            u8(&s->pre, t->fn.rest->mode);
        }

        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->fn.nkw; i++) {
            const th_kwparam *k = &t->fn.kw[i];

            s = step_add(&f->steps, k->type);

            if (!name(e->ctx, &s->pre, k->name, strlen(k->name))) {
                return false;
            }

            u8(&s->pre, k->mode);
        }

        if (t->fn.nkw) {
            u64(&f->post, t->fn.nkw);
        }
        break;
    }

    case TH_BINDER:
        s->child = t->binder.body;
        u64(&s->pre, t->binder.arity);
        break;

    case TH_BOUND:
        u64(&s->pre, t->bound.distance);
        u64(&s->pre, t->bound.position);
        break;

    case TH_VAR:
        if (e->erase) {
            u64(&s->pre, 0);
        }
        else if (!t->var.has_index) {
            th_err(e->ctx, TH_ERR_FREE_VAR, nullptr, nullptr, (th_type *)t,
                   "free variable has no index; normalize first");

            return false;
        }
        else {
            u64(&s->pre, t->var.index);
        }
        break;

    case TH_SUM:
        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->sum.n; i++) {
            step_add(&f->steps, t->sum.arms[i]);
        }

        u8(&f->post, t->sum.comb);
        u64(&f->post, t->sum.n);
        break;

    default:
        break;
    }

    if (!f->ninline) {
        f->ninline = f->steps.len;
    }

    size_t nside = f->steps.len - f->ninline;

    if (nside) {
        f->side = ncc_alloc_array(ncc_buffer_t, nside);
    }

    return true;
}

static void emit(ncc_buffer_t *to, const ncc_buffer_t *from)
{
    ncc_buffer_append(to, from->data, from->byte_len);
}

static void enc_splice(eframe *f)
{
    size_t n = f->steps.len - f->ninline;

    emit(f->out, &f->post);

    if (!n) {
        return;
    }

    th_slice *sl = ncc_alloc_array(th_slice, n);

    for (size_t i = 0; i < n; i++) {
        sl[i].p = (uint8_t *)f->side[i].data;
        sl[i].n = f->side[i].byte_len;
    }

    qsort(sl, n, sizeof(th_slice), th_cmp_slice);

    for (size_t i = 0; i < n; i++) {
        ncc_buffer_append(f->out, (const char *)sl[i].p, sl[i].n);
    }

    ncc_free(sl);
}

static void enc_frame_free(eframe *f)
{
    size_t nsteps = f->steps.len;

    for (size_t i = 0; i < nsteps; i++) {
        ncc_buffer_release(&f->steps.data[i].pre);
    }
    ncc_list_free(f->steps);

    for (size_t i = f->ninline; i < nsteps; i++) {
        ncc_buffer_release(&f->side[i - f->ninline]);
    }
    ncc_free(f->side);
    ncc_buffer_release(&f->post);
}

static void enc_node(th_enc *e, ncc_buffer_t *b, const th_type *t)
{
    if (!e->ok) {
        return;
    }

    ncc_list_t(eframe) st = ncc_list_new_cap(eframe, 16);

    ncc_list_push(st, ((eframe){.t = t, .out = b}));

    while (st.len && e->ok) {
        eframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = th_resolve_c(f->t);

            if (!f->t) {
                u8(f->out, 0xFF);
                ncc_list_pop(eframe, st);

                continue;
            }

            size_t back = pm_find(&e->path, f->t);

            if (back != SIZE_MAX) {
                u8(f->out, 0xFE);
                u64(f->out, pm_depth(&e->path) - back);
                ncc_list_pop(eframe, st);

                continue;
            }

            pm_push(&e->path, f->t);
            f->pushed = true;

            if (!enc_plan(e, f)) {
                e->ok = false;

                break;
            }
            f = &st.data[st.len - 1];
        }

        if (f->at < f->steps.len) {
            size_t        i  = f->at++;
            ncc_buffer_t *to = i < f->ninline ? f->out
                                              : &f->side[i - f->ninline];

            emit(to, &f->steps.data[i].pre);

            const th_type *child = f->steps.data[i].child;

            if (!child) {
                continue;
            }

            ncc_list_push(st, ((eframe){.t = child, .out = to}));

            continue;
        }

        enc_splice(f);

        if (f->pushed) {
            pm_pop(&e->path);
        }
        enc_frame_free(f);
        ncc_list_pop(eframe, st);
    }

    while (st.len) {
        eframe done = ncc_option_get(ncc_list_pop(eframe, st));

        if (done.entered) {
            enc_frame_free(&done);
        }
    }
    ncc_list_free(st);
}

typedef struct {
    ncc_dict_t seen;
    th_path    path;
    th_ctx    *ctx;
} th_norm;

static void norm_walk(th_norm *s, th_type *t);

void th_erase_encode(th_ctx *ctx, const th_type *t, ncc_buffer_t *out)
{
    th_enc e = {.ctx = ctx, .erase = true, .ok = true};

    pm_init(&e.path);
    enc_node(&e, out, t);
    pm_free(&e.path);
}

void th_full_encode(th_ctx *ctx, const th_type *t, ncc_buffer_t *out)
{
    th_enc e = {.ctx = ctx, .ok = true};

    pm_init(&e.path);
    enc_node(&e, out, t);
    pm_free(&e.path);
}

typedef struct {
    th_slice key;
    th_type *arm;
} th_armkey;

static int cmp_armkey(const void *a, const void *b)
{
    const th_armkey *x = a, *y = b;

    return th_cmp_slice(&x->key, &y->key);
}

th_type **th_sort_arms(th_ctx *ctx, th_type *const *arms, size_t n)
{
    th_armkey    *ak = ncc_alloc_array(th_armkey, n);
    ncc_buffer_t *tb = ncc_alloc_array(ncc_buffer_t, n);
    th_type     **ar = ncc_alloc_array(th_type *, n);

    for (size_t i = 0; i < n; i++) {
        th_erase_encode(ctx, arms[i], &tb[i]);
        ak[i].key.p = (uint8_t *)tb[i].data;
        ak[i].key.n = tb[i].byte_len;
        ak[i].arm   = arms[i];
    }

    qsort(ak, n, sizeof(th_armkey), cmp_armkey);

    for (size_t i = 0; i < n; i++) {
        ar[i] = ak[i].arm;
        ncc_buffer_release(&tb[i]);
    }
    ncc_free(tb);
    ncc_free(ak);

    return ar;
}

static th_type **norm_children_sum(th_norm *s, th_type *t, size_t *out)
{
    *out = t->sum.n;

    return th_sort_arms(s->ctx, t->sum.arms, t->sum.n);
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

typedef struct {
    th_type  *t;
    th_type **kids;
    size_t    n;
    size_t    next;
    bool      entered;
} th_nframe;

ncc_option_decl(th_nframe);
ncc_list_decl(th_nframe);

static th_type **norm_children(th_norm *s, th_type *t, size_t *out)
{
    th_type **kids = nullptr;
    size_t    n    = 0;

    *out = 0;

    switch (t->kind) {
    case TH_VAR:
        if (ncc_dict_contains(&s->seen, t)) {
            return nullptr;
        }

        t->var.index     = ncc_dict_len(&s->seen);
        t->var.has_index = true;

        ncc_dict_put(&s->seen, t, t);

        return nullptr;

    case TH_NAMED:
        kids = ncc_alloc_array(th_type *, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                kids[n++] = t->named.args[i].type;
            }
        }
        break;

    case TH_INDIRECTION:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->ind.referent;
        break;

    case TH_SEQUENCE:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->seq.elem;
        break;

    case TH_RECORD: {
        th_member *m = ncc_alloc_array(th_member, t->record.n);

        memcpy(m, t->record.members, t->record.n * sizeof(th_member));
        qsort(m, t->record.n, sizeof(th_member), th_cmp_member);

        kids = ncc_alloc_array(th_type *, t->record.n);

        for (size_t i = 0; i < t->record.n; i++) {
            kids[n++] = m[i].type;
        }
        ncc_free(m);
        break;
    }

    case TH_FUNCTION: {
        size_t want = t->fn.np + t->fn.nkw + 2;

        kids      = ncc_alloc_array(th_type *, want);
        kids[n++] = t->fn.ret;

        for (size_t i = 0; i < t->fn.np; i++) {
            kids[n++] = t->fn.params[i].type;
        }

        if (t->fn.rest) {
            kids[n++] = t->fn.rest->type;
        }

        if (t->fn.nkw) {
            th_kwparam *k = ncc_alloc_array(th_kwparam, t->fn.nkw);

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                kids[n++] = k[i].type;
            }
            ncc_free(k);
        }
        break;
    }

    case TH_BINDER:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->binder.body;
        break;

    case TH_SUM:
        return norm_children_sum(s, t, out);

    default:
        return nullptr;
    }

    *out = n;

    return kids;
}

static void norm_walk(th_norm *s, th_type *root)
{
    ncc_list_t(th_nframe) st = ncc_list_new_cap(th_nframe, 16);

    ncc_list_push(st, ((th_nframe){.t = root}));

    while (st.len) {
        th_nframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = (th_type *)th_resolve_c(f->t);

            if (!f->t || pm_find(&s->path, f->t) != SIZE_MAX) {
                f->t = nullptr;
                ncc_list_pop(th_nframe, st);

                continue;
            }

            pm_push(&s->path, f->t);

            f->kids = norm_children(s, f->t, &f->n);
            f       = &st.data[st.len - 1];
        }

        if (f->next < f->n) {
            th_type *kid = f->kids[f->next++];

            ncc_list_push(st, ((th_nframe){.t = kid}));

            continue;
        }

        pm_pop(&s->path);
        ncc_free(f->kids);
        ncc_list_pop(th_nframe, st);
    }

    ncc_list_free(st);
}

void th_normalize(th_type *t)
{
    th_ctx  tmp = {0};
    th_norm s   = {.ctx = &tmp};

    ncc_dict_init(&s.seen, nullptr, nullptr);
    pm_init(&s.path);
    norm_walk(&s, t);
    ncc_dict_free(&s.seen);
    pm_free(&s.path);
}

bool th_encode(th_ctx *ctx, th_type *t, const uint8_t **out, size_t *len)
{
    th_normalize(t);
    ctx->scratch.byte_len = 0;

    th_enc e = {.ctx = ctx, .ok = true};

    pm_init(&e.path);
    enc_node(&e, &ctx->scratch, t);
    pm_free(&e.path);

    if (!e.ok) {
        return false;
    }
    *out = (const uint8_t *)ctx->scratch.data;
    *len = ctx->scratch.byte_len;

    return true;
}

uint64_t th_hash(th_ctx *ctx, th_type *t)
{
    const uint8_t *p;
    size_t         n;

    if (!th_encode(ctx, t, &p, &n)) {
        return 0;
    }

    uint8_t ver[4] = {(uint8_t)(TH_VERSION >> 24), (uint8_t)(TH_VERSION >> 16),
                      (uint8_t)(TH_VERSION >> 8), (uint8_t)TH_VERSION};
    XXH3_state_t st;

    XXH3_128bits_reset(&st);
    XXH3_128bits_update(&st, ver, sizeof ver);
    XXH3_128bits_update(&st, p, n);

    return XXH3_128bits_digest(&st).low64;
}

uint64_t th_arm_tag(const char *n)
{
    uint64_t t = XXH3_64bits(n, strlen(n));

    return t ? t : 1;
}

const char *th_anon_name(th_ctx *ctx, const char *path, uint64_t line,
                         uint64_t col)
{
    size_t       plen = strlen(path);
    uint8_t      head[8];
    XXH3_state_t st;

    for (int i = 0; i < 8; i++) {
        head[i] = (uint8_t)(plen >> (56 - 8 * i));
    }

    XXH3_128bits_reset(&st);
    XXH3_128bits_update(&st, head, sizeof head);
    XXH3_128bits_update(&st, path, plen);

    uint64_t at[2] = {line, col};

    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++) {
            head[i] = (uint8_t)(at[k] >> (56 - 8 * i));
        }
        XXH3_128bits_update(&st, head, sizeof head);
    }

    uint64_t d = XXH3_128bits_digest(&st).low64;

    d = d ? d : 1;

    char *s = th_alloc(ctx, 32);

    snprintf(s, 32, "_anon_%016llx", (unsigned long long)d);

    return s;
}

static th_type *node(th_ctx *ctx, th_kind k)
{
    th_type *t = th_alloc(ctx, sizeof(th_type));

    t->kind = k;

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
    t->binder.qvars = nullptr;

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
    t->var.name      = nullptr;

    return t;
}

th_type *th_var_named(th_ctx *ctx, const char *name)
{
    th_type *t = th_var(ctx);

    if (name) {
        t->var.name = th_strdup(ctx, name, strlen(name));
    }

    return t;
}

th_type *th_tuple(th_ctx *ctx, th_type *const *e, size_t n)
{
    th_arg *a = ncc_alloc_array(th_arg, n);

    for (size_t i = 0; i < n; i++) {
        a[i].kind = TH_ARG_TYPE;
        a[i].type = e[i];
    }

    th_type *t = th_apply(ctx, TH_TUPLE, a, n);

    ncc_free(a);

    return t;
}

static ncc_hash_value_t enc_hash(void *p)
{
    return ncc_buffer_hash((ncc_buffer_t *)p);
}

static bool enc_eq(const void *a, const void *b)
{
    const ncc_buffer_t *x = a, *y = b;

    return x->byte_len == y->byte_len
        && !memcmp(x->data, y->data, x->byte_len);
}

th_type *th_sum(th_ctx *ctx, th_comb comb, th_type *const *arms, size_t n)
{
    ncc_list_t(th_type_ptr_t) flat = ncc_list_new_cap(th_type_ptr_t, n);

    for (size_t i = 0; i < n; i++) {
        th_type *a = arms[i];

        if (a->kind == TH_SUM && a->sum.comb == comb) {
            for (size_t j = 0; j < a->sum.n; j++) {
                ncc_list_push(flat, a->sum.arms[j]);
            }
        }
        else {
            ncc_list_push(flat, a);
        }
    }

    size_t        fn   = flat.len;
    th_type     **uniq = ncc_alloc_array(th_type *, fn);
    ncc_buffer_t *enc  = ncc_alloc_array(ncc_buffer_t, fn);
    size_t        un   = 0;
    ncc_dict_t    seen;

    ncc_dict_init(&seen, enc_hash, enc_eq);

    for (size_t i = 0; i < fn; i++) {
        th_erase_encode(ctx, flat.data[i], &enc[un]);

        if (ncc_dict_contains(&seen, &enc[un])) {
            ncc_buffer_release(&enc[un]);

            continue;
        }
        ncc_dict_put(&seen, &enc[un], &enc[un]);
        uniq[un++] = flat.data[i];
    }
    ncc_dict_free(&seen);

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
        ncc_buffer_release(&enc[i]);
    }
    ncc_free(enc);
    ncc_free(uniq);
    ncc_list_free(flat);

    return r;
}
