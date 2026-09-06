#ifndef TYPEHASH_H
#define TYPEHASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TH_VERSION       0
#define TH_RESERVED      "__ty."
#define TH_TUPLE         "__ty.tuple"
#define TH_UNTYPED       "__ty.untyped"

typedef enum : uint8_t {
    TH_NAMED = 0,
    TH_BITINT,
    TH_INDIRECTION,
    TH_SEQUENCE,
    TH_RECORD,
    TH_FUNCTION,
    TH_BINDER,
    TH_BOUND,
    TH_SUM,
    TH_VAR,
} th_kind;

typedef enum : uint8_t {
    TH_NULLABLE   = 0x01,
    TH_RESEATABLE = 0x02,
    TH_INDEXABLE  = 0x04,
    TH_OWNING     = 0x08,
    TH_EXCLUSIVE  = 0x10,
    TH_TEMPORARY  = 0x20,
} th_attr;

typedef enum : uint8_t {
    TH_IN = 0,
    TH_OUT,
    TH_INOUT,
} th_mode;

typedef enum : uint8_t {
    TH_UNION = 0,
    TH_INTERSECTION,
} th_comb;

typedef enum : uint8_t {
    TH_ARG_TYPE = 0,
    TH_ARG_UINT,
    TH_ARG_SINT,
    TH_ARG_STR,
} th_argkind;

typedef struct th_ctx  th_ctx;
typedef struct th_type th_type;

typedef struct {
    th_argkind kind;
    union {
        th_type *type;
        uint64_t u;
        int64_t  s;
        struct {
            const char *p;
            size_t      n;
        } str;
    };
} th_arg;

typedef struct {
    const char *name;
    th_type    *type;
} th_member;

typedef struct {
    th_mode  mode;
    th_type *type;
} th_param;

typedef struct {
    const char *name;
    th_mode     mode;
    th_type    *type;
} th_kwparam;

th_ctx *th_ctx_new(void);
void    th_ctx_free(th_ctx *ctx);

typedef bool (*th_normalizer)(void *state, const char *in, size_t inlen,
                              const char **out, size_t *outlen);

void th_ctx_set_normalizer(th_ctx *ctx, th_normalizer fn, void *state);
void th_install_nfc(th_ctx *ctx);

th_type *th_name(th_ctx *ctx, const char *name);
th_type *th_apply(th_ctx *ctx, const char *name, const th_arg *args, size_t n);
th_type *th_variant(th_ctx *ctx, const char *name, const th_arg *args,
                    size_t n);
th_type *th_bitint(th_ctx *ctx, uint64_t width, bool is_signed);
th_type *th_ind(th_ctx *ctx, uint8_t attrs, th_type *referent);
th_type *th_array(th_ctx *ctx, th_type *elem);
th_type *th_array_n(th_ctx *ctx, th_type *elem, uint64_t extent);
th_type *th_record(th_ctx *ctx, const th_member *members, size_t n);
th_type *th_record_stub(th_ctx *ctx);
void     th_record_fill(th_ctx *ctx, th_type *stub, const th_member *members,
                        size_t n);
th_type *th_func(th_ctx *ctx, th_type *ret, const th_param *params, size_t np,
                 const th_param *rest, const th_kwparam *kw, size_t nkw);
th_type *th_binder(th_ctx *ctx, uint64_t arity, th_type *body);
th_type *th_bound(th_ctx *ctx, uint64_t distance, uint64_t position);
th_type *th_sum(th_ctx *ctx, th_comb comb, th_type *const *arms, size_t n);
th_type *th_var(th_ctx *ctx);
th_type *th_tuple(th_ctx *ctx, th_type *const *elems, size_t n);

void     th_normalize(th_type *type);
bool     th_encode(th_ctx *ctx, th_type *type, const uint8_t **out,
                   size_t *len);
uint64_t th_hash(th_ctx *ctx, th_type *type);
const char *th_print(th_ctx *ctx, th_type *type);
uint64_t th_arm_tag(const char *name);
const char *th_anon_name(th_ctx *ctx, const char *path, uint64_t line,
                         uint64_t col);

th_kind th_kind_of(const th_type *type);
bool    th_is_scheme(const th_type *type);
const char *th_last_error(const th_ctx *ctx);

bool   th_named_info(const th_type *type, const char **name, size_t *nlen,
                     bool *is_variant, size_t *nargs);
size_t th_named_nargs(const th_type *type);
const th_arg *th_named_arg(const th_type *type, size_t i);

bool th_bitint_info(const th_type *type, uint64_t *width, bool *is_signed);

th_type *th_ind_referent(const th_type *type);
bool     th_ind_attrs(const th_type *type, uint8_t *attrs);

th_type *th_seq_elem(const th_type *type);
bool     th_seq_extent(const th_type *type, uint64_t *extent);

size_t th_record_len(const th_type *type);
const th_member *th_record_member(const th_type *type, size_t i);
th_type *th_record_lookup(const th_type *type, const char *name);

th_type *th_func_ret(const th_type *type);
size_t   th_func_nparams(const th_type *type);
const th_param *th_func_param(const th_type *type, size_t i);
const th_param *th_func_rest(const th_type *type);
size_t th_func_nkw(const th_type *type);
const th_kwparam *th_func_kw(const th_type *type, size_t i);
const th_kwparam *th_func_kw_lookup(const th_type *type, const char *name);

bool     th_binder_arity(const th_type *type, uint64_t *arity);
th_type *th_binder_body(const th_type *type);

bool th_bound_info(const th_type *type, uint64_t *distance,
                   uint64_t *position);

bool     th_sum_comb(const th_type *type, th_comb *comb);
size_t   th_sum_len(const th_type *type);
th_type *th_sum_arm(const th_type *type, size_t i);

bool th_var_index(const th_type *type, uint64_t *index);

th_type *th_resolve(th_type *type);
th_type *th_clone(th_ctx *ctx, th_type *type);
size_t   th_free_vars(th_ctx *ctx, th_type *type, th_type ***out);
bool     th_is_closed(th_ctx *ctx, th_type *type);
bool     th_occurs(th_ctx *ctx, th_type *var, th_type *type);
th_type *th_apply_subst(th_ctx *ctx, th_type *type);
th_type *th_unify(th_ctx *ctx, th_type *a, th_type *b);
th_type *th_instantiate(th_ctx *ctx, th_type *type);
th_type *th_generalize(th_ctx *ctx, th_type *type);

#endif
