#ifndef TH_INTERNAL_H
#define TH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "typehash.h"

#define TH_CHUNK 8192

typedef struct th_chunk {
    struct th_chunk *next;
    size_t           used;
    size_t           cap;
    unsigned char    data[];
} th_chunk;

typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} th_buf;

typedef struct {
    uint8_t *p;
    size_t   n;
} th_slice;

struct th_type {
    th_kind kind;
    union {
        struct {
            const char *name;
            size_t      nlen;
            bool        is_variant;
            th_arg     *args;
            size_t      nargs;
        } named;
        struct {
            uint64_t width;
            bool     is_signed;
        } bitint;
        struct {
            uint8_t  attrs;
            th_type *referent;
        } ind;
        struct {
            bool     has_extent;
            uint64_t extent;
            th_type *elem;
        } seq;
        struct {
            th_member *members;
            size_t     n;
        } record;
        struct {
            th_type    *ret;
            th_param   *params;
            size_t      np;
            th_param   *rest;
            th_kwparam *kw;
            size_t      nkw;
        } fn;
        struct {
            uint64_t arity;
            th_type *body;
        } binder;
        struct {
            uint64_t distance;
            uint64_t position;
        } bound;
        struct {
            th_comb   comb;
            th_type **arms;
            size_t    n;
        } sum;
        struct {
            uint64_t index;
            bool     has_index;
            th_type *ref;
        } var;
    };
};

struct th_ctx {
    th_chunk     *chunks;
    th_buf        scratch;
    th_normalizer norm;
    void         *norm_state;
    const char   *err;
    uint64_t      next_var;
};

void *th_alloc(th_ctx *ctx, size_t n);
char *th_strdup(th_ctx *ctx, const char *s, size_t n);
int   th_cmp_slice(const void *a, const void *b);
int   th_cmp_member(const void *a, const void *b);
int   th_cmp_kw(const void *a, const void *b);
void  th_erase_encode(th_ctx *ctx, const th_type *t, th_buf *out);
void  th_full_encode(th_ctx *ctx, const th_type *t, th_buf *out);
void  th_buf_free(th_buf *b);

static inline const th_type *th_resolve_c(const th_type *t)
{
    while (t && t->kind == TH_VAR && t->var.ref) {
        t = t->var.ref;
    }

    return t;
}

#endif
