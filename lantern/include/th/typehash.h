#ifndef TYPEHASH_H
#define TYPEHASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TH_VERSION       0
#define TH_RESERVED      "__ty."
#define TH_TUPLE         "__ty.tuple"
#define TH_UNTYPED       "__ty.untyped"
#define TH_ERASED        "__ty.erased"
#define TH_BOTTOM        "__ty.bottom"

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
    TH_CONSUME,
} th_mode;

static inline bool th_mode_is_valid(th_mode mode)
{
    return mode <= TH_CONSUME;
}

static inline bool th_mode_accepts_input(th_mode mode)
{
    return mode == TH_IN || mode == TH_INOUT || mode == TH_CONSUME;
}

static inline bool th_mode_produces_output(th_mode mode)
{
    return mode == TH_OUT || mode == TH_INOUT;
}

static inline bool th_mode_is_input_only(th_mode mode)
{
    return mode == TH_IN || mode == TH_CONSUME;
}

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
typedef struct th_env  th_env;

typedef th_type *(*th_method_resolver_fn)(th_ctx *ctx, th_type *receiver,
                                          const char *name, void *user);

typedef struct {
    const char *file;
    uint32_t    start_line;
    uint32_t    start_col;
    uint32_t    end_line;
    uint32_t    end_col;
} th_span_t;

typedef enum : uint8_t {
    TH_ERR_UNIFY = 0,
    TH_ERR_CONSTRAINT,
    TH_ERR_OCCURS,
    TH_ERR_NO_SUCH_FIELD,
    TH_ERR_PARAM_MISMATCH,
    TH_ERR_ARITY_MISMATCH,
    TH_ERR_MISSING_KEYWORD,
    TH_ERR_UNKNOWN_KEYWORD,
    TH_ERR_NON_EXHAUSTIVE,
    TH_ERR_UNREACHABLE_PATTERN,
    TH_ERR_DUPLICATE_VARIANT,
    TH_ERR_NO_MATCHING_RULE,
    TH_ERR_BAD_NAME,
    TH_ERR_FREE_VAR,
    TH_ERR_KIND_COUNT,
} th_err_kind;

typedef struct {
    th_err_kind kind;
    const char *message;
    const char *constraint;
    th_type    *expected;
    th_type    *got;
    th_span_t   span;
    th_span_t   related;
    bool        has_related;
} th_error_t;

typedef enum : uint8_t {
    TH_CON_UNIFIES = 0,
    TH_CON_ONE_OF,
    TH_CON_IMPLEMENTS,
    TH_CON_HAS_FIELD,
    TH_CON_HAS_PARAM,
    TH_CON_PROMOTES,
    TH_CON_EXCLUDES,
    /* Resolve a named receiver method when this variable becomes concrete.
     * `call`, when present, is the positional/keyword call shape without the
     * receiver; its return type is `result`. */
    TH_CON_METHOD,
    /* Restrict a variable to a structural type family without fixing its
     * parameters, such as the width and signedness of a bit integer. */
    TH_CON_KIND,
} th_con_kind;

typedef struct {
    th_con_kind kind;
    /* Environment-backed constraints retain the visibility perspective in
     * which they were authored, even when resolution is deferred. */
    const char *scope_module;
    const char *scope_package;
    bool has_scope;
    union {
        th_kind type_kind;
        th_type    *target;
        const char *iface;
        struct {
            th_type **types;
            size_t    n;
        } one_of;
        struct {
            const char *name;
            th_type    *type;
        } field;
        struct {
            uint64_t index;
            th_type *type;
        } param;
        struct {
            const char *name;
            th_type *result;
            th_type *call;
        } method;
    };
} th_constraint;

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
/* Deep-copy a type graph into another type context, preserving graph sharing. */
th_type *th_type_clone(th_ctx *destination, const th_type *source);

typedef bool (*th_normalizer)(void *state, const char *in, size_t inlen,
                              const char **out, size_t *outlen);

void th_ctx_set_normalizer(th_ctx *ctx, th_normalizer fn, void *state);
void th_install_nfc(th_ctx *ctx);
/* Set the current module/package perspective used when new environment-backed
 * constraints are created. This may be called before an environment is
 * attached; existing constraints retain their captured scope. */
bool th_ctx_set_scope(th_ctx *ctx, const char *module, const char *package);

th_type *th_name(th_ctx *ctx, const char *name);
th_type *th_apply(th_ctx *ctx, const char *name, const th_arg *args, size_t n);
th_type *th_variant(th_ctx *ctx, const char *name, const th_arg *args,
                    size_t n);
th_type *th_bitint(th_ctx *ctx, uint64_t width, bool is_signed);
th_type *th_ind(th_ctx *ctx, uint8_t attrs, th_type *referent);
bool     th_indirection_attrs(const char *name, uint8_t *attrs);
th_type *th_array(th_ctx *ctx, th_type *elem);
th_type *th_array_n(th_ctx *ctx, th_type *elem, uint64_t extent);
th_type *th_record(th_ctx *ctx, const th_member *members, size_t n);
th_type *th_record_stub(th_ctx *ctx);
void     th_record_fill(th_ctx *ctx, th_type *stub, const th_member *members,
                        size_t n);
/* Associate a named type with its structural definition in this type context.
 * Definitions are semantic metadata and do not change the named type's identity. */
bool     th_type_define(th_ctx *ctx, th_type *type, th_type *definition);
th_type *th_type_definition(const th_ctx *ctx, const th_type *type);
th_type *th_record_lookup_in(const th_ctx *ctx, const th_type *type,
                             const char *name);
th_type *th_func(th_ctx *ctx, th_type *ret, const th_param *params, size_t np,
                 const th_param *rest, const th_kwparam *kw, size_t nkw);
/* The optional error type is a separate callable channel, not part of ret.
 * Pass nullptr when the function type has no failure channel. */
th_type *th_func_with_error(th_ctx *ctx, th_type *ret, th_type *error,
                            const th_param *params, size_t np,
                            const th_param *rest, const th_kwparam *kw,
                            size_t nkw);
th_type *th_binder(th_ctx *ctx, uint64_t arity, th_type *body);
th_type *th_bound(th_ctx *ctx, uint64_t distance, uint64_t position);
th_type *th_sum(th_ctx *ctx, th_comb comb, th_type *const *arms, size_t n);
th_type *th_var(th_ctx *ctx);
th_type *th_var_named(th_ctx *ctx, const char *name);
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

void      th_ctx_set_span(th_ctx *ctx, th_span_t span);
void      th_ctx_set_related(th_ctx *ctx, th_span_t span);
void      th_ctx_clear_related(th_ctx *ctx);
th_span_t th_ctx_span(const th_ctx *ctx);

size_t      th_error_count(const th_ctx *ctx);
bool        th_error_at(const th_ctx *ctx, size_t i, th_error_t *out);
void        th_errors_clear(th_ctx *ctx);
const char *th_err_kind_name(th_err_kind kind);

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
th_type *th_func_error(const th_type *type);
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
const char *th_var_name(const th_type *type);

bool   th_var_constrain(th_ctx *ctx, th_type *var, th_constraint c);
/* Attach a deferred method-call obligation to a concrete or unresolved
 * receiver. `call` is the positional/keyword argument shape excluding the
 * receiver; `result` is unified with the selected method's return type. */
bool   th_require_method(th_ctx *ctx, th_type *receiver, const char *name,
                         th_type *result, th_type *call);
/* Connect an existing deferred method-call obligation to its enclosing
 * failure channel without adding a second method constraint. */
bool   th_method_obligation_set_error(th_ctx *ctx, th_type *receiver,
                                      const char *name, th_type *result,
                                      th_type *error);
size_t th_var_constraint_count(const th_type *var);
bool   th_var_constraint_at(const th_type *var, size_t i, th_constraint *out);
/* Recheck method obligations deferred until an environment is attached.
 * th_ctx_set_environment() calls this automatically. */
bool   th_ctx_validate_constraints(th_ctx *ctx);

void th_register_promotion(th_ctx *ctx, const char *from, const char *to);
bool th_promotes_to(th_ctx *ctx, const char *from, const char *to);
bool th_type_promotes_to(th_ctx *ctx, const th_type *from,
                         const th_type *to);

typedef enum : uint8_t {
    TH_COERCE_NONE        = 0,
    TH_COERCE_PROMOTE     = 1 << 0,
    TH_COERCE_DEREF       = 1 << 1,
    TH_COERCE_OPTION_WRAP = 1 << 2,
    TH_COERCE_CUSTOM      = 1 << 3,
} th_coerce_flag;

typedef struct {
    uint8_t  allow;
    th_type *target;
} th_coerce_policy;

typedef struct {
    th_coerce_flag kind;
    th_type       *from;
    th_type       *to;
    th_span_t      span;
} th_coercion_t;

typedef bool (*th_coerce_fn)(th_ctx *ctx, th_type *from, th_type *to,
                             void *user);

th_type *th_coerce(th_ctx *ctx, th_type *from, th_coerce_policy policy);

void   th_register_coercion(th_ctx *ctx, th_coerce_fn fn, void *user);
size_t th_coercion_count(const th_ctx *ctx);
bool   th_coercion_at(const th_ctx *ctx, size_t i, th_coercion_t *out);
void   th_coercions_clear(th_ctx *ctx);

void   th_register_iface(th_ctx *ctx, const char *name, th_type *methods);
bool   th_register_method(th_ctx *ctx, th_type *receiver, const char *name,
                          th_type *signature);
bool   th_register_method_resolver(th_ctx *ctx,
                                   th_method_resolver_fn resolver,
                                   void *user);
th_type *th_method_lookup(th_ctx *ctx, th_type *receiver, const char *name);
/* For an unresolved constrained type variable, true means the trait is a
 * retained obligation, not proof of a concrete implementation or dispatch. */
bool   th_implements(th_ctx *ctx, th_type *type, const char *iface);
size_t th_infer_ifaces(th_ctx *ctx, th_type *type, const char **out,
                       size_t max);

th_type *th_resolve(th_type *type);

bool     th_is_erased(const th_type *type);
bool     th_is_bottom(const th_type *type);
size_t   th_free_vars(th_ctx *ctx, th_type *type, th_type ***out);
bool     th_is_closed(th_ctx *ctx, th_type *type);
bool     th_occurs(th_ctx *ctx, th_type *var, th_type *type);
th_type *th_apply_subst(th_ctx *ctx, th_type *type);
/* Unifies fresh copies of both inputs; neither input is constrained. */
th_type *th_unify(th_ctx *ctx, th_type *a, th_type *b);
/* Probes unification on fresh copies without retaining failure diagnostics. */
th_type *th_unify_probe(th_ctx *ctx, th_type *a, th_type *b);
/* Constrains the original monomorphic variables; failure may leave bindings. */
th_type *th_unify_in_place(th_ctx *ctx, th_type *a, th_type *b);
th_type *th_instantiate(th_ctx *ctx, th_type *type);
th_type *th_generalize(th_ctx *ctx, th_type *type);
th_type *th_generalize_excluding(th_ctx *ctx, th_type *type,
                                 th_type *const *environment,
                                 size_t environment_count);

#endif
