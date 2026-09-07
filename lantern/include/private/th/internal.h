#ifndef TH_INTERNAL_H
#define TH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "th/typehash.h"
#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/dict.h"

typedef struct {
    uint8_t *p;
    size_t   n;
} th_slice;

typedef struct {
    const char *from;
    const char *to;
} th_promo;

typedef struct {
    const char *name;
    th_type    *methods;
} th_iface;

core_list_decl(th_constraint);
core_list_decl(th_promo);
core_list_decl(th_iface);

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
            uint64_t   arity;
            th_type   *body;
            th_type  **qvars;
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
            uint64_t                  index;
            bool                      has_index;
            th_type                  *ref;
            const char               *name;
            core_list_t(th_constraint) cons;
        } var;
    };
};

core_list_decl(th_error_t);
core_list_decl(th_coercion_t);

struct th_ctx {
    core_arena_t            arena;
    core_buffer_t           scratch;
    th_normalizer          norm;
    void                  *norm_state;
    const char            *err;
    uint64_t               next_var;
    core_list_t(th_promo)   promos;
    core_list_t(th_iface)   ifaces;
    core_dict_t            *promo_index;
    core_dict_t            *iface_index;
    core_list_t(th_error_t) errors;
    core_list_t(th_coercion_t) coercions;
    th_coerce_fn           custom;
    void                  *custom_user;
    th_span_t              span;
    th_span_t              related;
    bool                   has_related;
};

void th_err(th_ctx *ctx, th_err_kind kind, const char *constraint,
            th_type *expected, th_type *got, const char *message);

void *th_alloc(th_ctx *ctx, size_t n);
char *th_strdup(th_ctx *ctx, const char *s, size_t n);
int   th_cmp_slice(const void *a, const void *b);
int   th_cmp_member(const void *a, const void *b);
int   th_cmp_kw(const void *a, const void *b);
void  th_erase_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);
th_type **th_sort_arms(th_ctx *ctx, th_type *const *arms, size_t n);
void  th_full_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);

static inline const th_type *th_resolve_c(const th_type *t)
{
    while (t && t->kind == TH_VAR && t->var.ref) {
        t = t->var.ref;
    }

    return t;
}

#endif
