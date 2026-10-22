#ifndef TYPEHASH_H
#define TYPEHASH_H

#include "lib/buffer.h"
#include "lib/string.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TH_VERSION       0
#define TH_RESERVED      "__ty."
#define TH_TUPLE         "__ty.tuple"
#define TH_OPTION        "Option"
#define TH_NIL           "nil"
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
    TH_TRAIT,
} th_kind;

typedef enum : uint8_t {
    TH_BITINT_UNSIGNED = 0,
    TH_BITINT_SIGNED = 1,
    TH_BITINT_BITS = 2,
} th_bitint_flavor;

typedef enum : uint8_t {
    TH_NULLABLE   = 0x01,
    TH_RESEATABLE = 0x02,
    TH_INDEXABLE  = 0x04,
    TH_OWNING     = 0x08,
    TH_EXCLUSIVE  = 0x10,
    TH_TEMPORARY  = 0x20,
    TH_FOREIGN    = 0x40,
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

typedef core_string_t (*th_print_name_fn)(core_string_t name, void *context);

typedef struct {
    th_print_name_fn named;
    void            *context;
    core_string_t    sequence;
    core_string_t    reference;
    core_string_t    option_suffix;
    bool             omit_constraints;
    bool             omit_nil_failure;
} th_print_options_t;

typedef th_type *(*th_method_resolver_fn)(th_ctx *ctx, th_type *receiver,
                                          core_string_t name, void *user);

typedef struct {
    core_string_t file;
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
    core_string_t message;
    core_string_t constraint;
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
    TH_CON_INTEGER,
} th_con_kind;

enum {
    TH_INTEGER_UNSIGNED = 1u << 0,
};

typedef enum : uint8_t {
    TH_INTEGER_NONE,
    TH_INTEGER_SIGNED,
    TH_INTEGER_UNSIGNED_TYPE,
} th_integer_class;

typedef struct {
    th_con_kind kind;
    /* Environment-backed constraints retain the visibility perspective in
     * which they were authored, even when resolution is deferred. */
    core_string_t scope_module;
    core_string_t scope_package;
    bool has_scope;
    /* The source span active when the constraint was authored, restored
     * while a deferred check reports against it. */
    th_span_t span;
    union {
        th_kind type_kind;
        struct {
            uint8_t  flags;
            uint32_t owner;
        } integer;
        th_type    *target;
        core_string_t iface;
        struct {
            th_type **types;
            size_t    n;
        } one_of;
        struct {
            core_string_t name;
            th_type    *type;
            bool        auto_deref;
        } field;
        struct {
            uint64_t index;
            th_type *type;
        } param;
        struct {
            core_string_t name;
            th_type *result;
            th_type *call;
            core_string_t in_place;
            const void *site;
        } method;
    };
} th_constraint;

/* Declared interface requirements, separate from inference obligations. */
typedef struct {
    th_type *subject;
    th_con_kind kind;
    th_type *target;
    core_string_t iface;
} th_trait_requirement;

typedef struct {
    th_argkind kind;
    union {
        th_type *type;
        uint64_t u;
        int64_t  s;
        core_string_t str;
    };
} th_arg;

typedef struct {
    core_string_t name;
    th_type    *type;
} th_member;

typedef struct {
    th_mode  mode;
    th_type *type;
} th_param;

typedef struct {
    core_string_t name;
    th_mode     mode;
    th_type    *type;
} th_kwparam;

th_ctx *th_ctx_new(void);
void    th_ctx_free(th_ctx *ctx);
uint64_t th_type_graph_revision(th_type *const *roots, size_t count);
/* Deep-copy a type graph into another type context, preserving graph sharing. */
th_type *th_type_clone(th_ctx *destination, const th_type *source);
th_type *th_type_clone_fresh(th_ctx *destination, const th_type *source);
typedef struct th_type_cloner th_type_cloner_t;
th_type_cloner_t *th_type_cloner_new(th_ctx *destination,
                                     const th_type *const *from,
                                     th_type *const *to, size_t count);
th_type *th_type_cloner_clone(th_type_cloner_t *cloner,
                              const th_type *source);
void th_type_cloner_free(th_type_cloner_t *cloner);
/* Clone related roots with one graph map, retaining shared variables and
 * constraint references between roots. */
bool th_type_clone_many(th_ctx *destination, const th_type *const *sources,
                        th_type **copies, size_t count);

typedef struct th_type_writer th_type_writer_t;
typedef struct th_type_reader th_type_reader_t;
th_type_writer_t *th_type_writer_new(void);
uint32_t th_type_writer_add(th_type_writer_t *writer, const th_type *type);
bool th_type_writer_finish(th_type_writer_t *writer, core_buffer_t *out);
void th_type_writer_free(th_type_writer_t *writer);
th_type_reader_t *th_type_reader_open(th_ctx *ctx, const uint8_t *data,
                                      size_t length, size_t *consumed);
th_type *th_type_reader_get(const th_type_reader_t *reader, uint32_t index);
void th_type_reader_free(th_type_reader_t *reader);
/* Clone a graph while replacing selected source nodes with existing nodes.
 * Replacements are borrowed and must outlive the result. */
th_type *th_type_clone_replacing(th_ctx *destination, const th_type *source,
                                 const th_type *const *from,
                                 th_type *const *to, size_t count);

typedef bool (*th_normalizer)(void *state, core_string_t in,
                              core_string_t *out);

void th_ctx_set_normalizer(th_ctx *ctx, th_normalizer fn, void *state);
void th_install_nfc(th_ctx *ctx);
/* Set the current module/package perspective used when new environment-backed
 * constraints are created. This may be called before an environment is
 * attached; existing constraints retain their captured scope. */
bool th_ctx_set_scope(th_ctx *ctx, core_string_t module,
                      core_string_t package);

th_type *th_name(th_ctx *ctx, core_string_t name);
bool th_name_syntax_valid(core_string_t name);
th_type *th_apply(th_ctx *ctx, core_string_t name, const th_arg *args,
                  size_t n);
th_type *th_variant(th_ctx *ctx, core_string_t name, const th_arg *args,
                    size_t n);
th_type *th_bitint(th_ctx *ctx, uint64_t width, bool is_signed);
th_type *th_bits(th_ctx *ctx, uint64_t width);
th_type *th_ind(th_ctx *ctx, uint8_t attrs, th_type *referent);
bool     th_indirection_attrs(core_string_t name, uint8_t *attrs);
th_type *th_array(th_ctx *ctx, th_type *elem);
th_type *th_array_n(th_ctx *ctx, th_type *elem, uint64_t extent);
th_type *th_record(th_ctx *ctx, const th_member *members, size_t n);
/* A named interface whose method order is part of its type identity. */
th_type *th_trait(th_ctx *ctx, core_string_t qualified_name,
                  const th_member *methods, size_t n,
                  const th_trait_requirement *requirements, size_t nr);
th_type *th_record_stub(th_ctx *ctx);
void     th_record_fill(th_ctx *ctx, th_type *stub, const th_member *members,
                        size_t n);
/* Associate a named type with its structural definition in this type context.
 * Definitions are semantic metadata and do not change the named type's identity. */
typedef enum : uint8_t {
    TH_BUILTIN_NONE,
    TH_BUILTIN_SCALAR,
    TH_BUILTIN_STRING,
    TH_BUILTIN_OPTION,
    TH_BUILTIN_ATOMIC,
    TH_BUILTIN_ARRAY,
    TH_BUILTIN_LIST,
    TH_BUILTIN_SET,
    TH_BUILTIN_DICT,
    TH_BUILTIN_CONTAINER_LIST,
    TH_BUILTIN_CONTAINER_SET,
    TH_BUILTIN_CONTAINER_DICT,
    TH_BUILTIN_VIEW,
    TH_BUILTIN_TUPLE,
    TH_BUILTIN_ANY,
    TH_BUILTIN_NIL,
    TH_BUILTIN_TYPESPEC,
    TH_BUILTIN_ALLOCATOR,
} th_builtin_kind_t;

typedef struct {
    core_string_t     name;
    th_builtin_kind_t kind;
    int8_t            arity;
    const void       *scalar;
} th_builtin_type_t;

#define TH_BUILTIN_VARIADIC (-1)

void th_ctx_set_builtin_types(th_ctx *ctx, const th_builtin_type_t *types,
                              size_t count);
bool th_ctx_has_builtin_types(const th_ctx *ctx);
void th_set_default_builtin_types(const th_builtin_type_t *types,
                                  size_t count);
void th_builtin_defaults_apply(th_ctx *ctx);
const th_builtin_type_t *th_builtin_type(th_ctx *ctx, const th_type *type);
const th_builtin_type_t *th_builtin_of(const th_type *type);
th_builtin_kind_t th_builtin_kind_of(const th_type *type);
th_type *th_builtin_applied(th_type *type, th_builtin_kind_t kind,
                            size_t count);
th_builtin_kind_t th_builtin_kind(th_ctx *ctx, const th_type *type);
th_type *th_builtin_item(th_ctx *ctx, const th_type *type,
                         th_builtin_kind_t kind, size_t index);

bool     th_type_define(th_ctx *ctx, th_type *type, th_type *definition);
bool     th_type_define_owned(th_ctx *ctx, th_type *type, th_type *definition,
                              void *owner);
th_type *th_type_definition(th_ctx *ctx, const th_type *type);
void    *th_type_definition_owner(th_ctx *ctx, const th_type *type);
th_type *th_auto_deref(th_ctx *ctx, th_type *type);
th_type *th_record_lookup_in(th_ctx *ctx, const th_type *type,
                             core_string_t name);
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
th_type *th_var_named(th_ctx *ctx, core_string_t name);
th_type *th_tuple(th_ctx *ctx, th_type *const *elems, size_t n);
th_type *th_option(th_ctx *ctx, th_type *item);
th_type *th_option_item(const th_type *type);
th_type *th_nil(th_ctx *ctx);
bool     th_is_nil(const th_type *type);

void     th_normalize(th_type *type);
bool     th_encode(th_ctx *ctx, th_type *type, const uint8_t **out,
                   size_t *len);
uint64_t th_hash(th_ctx *ctx, th_type *type);
core_string_t th_print(th_ctx *ctx, th_type *type);
core_string_t th_print_with_options(th_ctx *ctx, th_type *type,
                                    const th_print_options_t *options);
uint64_t th_arm_tag(core_string_t name);
core_string_t th_anon_name(th_ctx *ctx, core_string_t path, uint64_t line,
                           uint64_t col);

th_kind th_kind_of(const th_type *type);
bool    th_is_scheme(const th_type *type);
core_string_t th_last_error(const th_ctx *ctx);

void      th_ctx_set_span(th_ctx *ctx, th_span_t span);
void      th_ctx_set_related(th_ctx *ctx, th_span_t span);
void      th_ctx_clear_related(th_ctx *ctx);
th_span_t th_ctx_span(const th_ctx *ctx);

size_t      th_error_count(const th_ctx *ctx);
bool        th_error_at(const th_ctx *ctx, size_t i, th_error_t *out);
void        th_errors_clear(th_ctx *ctx);
void        th_ctx_set_probe_errors_observable(th_ctx *ctx, bool observable);
core_string_t th_err_kind_name(th_err_kind kind);

bool   th_named_info(const th_type *type, core_string_t *name,
                     bool *is_variant, size_t *nargs);
size_t th_named_nargs(const th_type *type);
const th_arg *th_named_arg(const th_type *type, size_t i);

bool th_bitint_info(const th_type *type, uint64_t *width,
                    th_bitint_flavor *flavor);

th_type *th_ind_referent(const th_type *type);
bool     th_ind_attrs(const th_type *type, uint8_t *attrs);

th_type *th_seq_elem(const th_type *type);
bool     th_seq_extent(const th_type *type, uint64_t *extent);

size_t th_record_len(const th_type *type);
const th_member *th_record_member(const th_type *type, size_t i);
th_type *th_record_lookup(const th_type *type, core_string_t name);

bool th_trait_name(const th_type *type, core_string_t *name);
size_t th_trait_len(const th_type *type);
const th_member *th_trait_method(const th_type *type, size_t i);
size_t th_trait_requirement_count(const th_type *type);
bool th_trait_requirement_at(const th_type *type, size_t i,
                             th_trait_requirement *out);

th_type *th_func_ret(const th_type *type);
th_type *th_func_error(const th_type *type);
size_t   th_func_nparams(const th_type *type);
const th_param *th_func_param(const th_type *type, size_t i);
const th_param *th_func_rest(const th_type *type);
size_t th_func_nkw(const th_type *type);
const th_kwparam *th_func_kw(const th_type *type, size_t i);
const th_kwparam *th_func_kw_lookup(const th_type *type, core_string_t name);

bool     th_binder_arity(const th_type *type, uint64_t *arity);
th_type *th_binder_body(const th_type *type);

bool th_bound_info(const th_type *type, uint64_t *distance,
                   uint64_t *position);

bool     th_sum_comb(const th_type *type, th_comb *comb);
size_t   th_sum_len(const th_type *type);
th_type *th_sum_arm(const th_type *type, size_t i);

bool th_var_index(const th_type *type, uint64_t *index);
core_string_t th_var_name(const th_type *type);

bool   th_var_constrain(th_ctx *ctx, th_type *var, th_constraint c);
/* Attach a deferred method-call obligation to a concrete or unresolved
 * receiver. `call` is the positional/keyword argument shape excluding the
 * receiver; `result` is unified with the selected method's return type. */
bool   th_require_method(th_ctx *ctx, th_type *receiver, core_string_t name,
                         th_type *result, th_type *call);
bool   th_require_method_in_place(th_ctx *ctx, th_type *receiver,
                                  core_string_t name, core_string_t in_place,
                                  th_type *result, th_type *call);
bool   th_require_method_at(th_ctx *ctx, th_type *receiver, core_string_t name,
                            core_string_t in_place, th_type *result,
                            th_type *call, const void *site);
bool   th_awaits_method_result(th_ctx *ctx, th_type *type);
/* Connect an existing deferred method-call obligation to its enclosing
 * failure channel without adding a second method constraint. */
bool   th_method_obligation_set_error(th_ctx *ctx, th_type *receiver,
                                      core_string_t name, th_type *result,
                                      th_type *error);
typedef struct {
    core_string_t name;
    bool        optional;
} th_formal_name;
th_type *th_call_shape_bind_keywords(th_ctx *ctx, th_type *call,
                                     const th_formal_name *formals,
                                     size_t formal_count);
bool   th_method_obligation_bind_keywords(th_ctx *ctx, th_type *receiver,
                                          core_string_t name,
                                          const th_formal_name *formals,
                                          size_t formal_count);
size_t th_var_constraint_count(const th_type *var);
bool   th_var_constraint_at(const th_type *var, size_t i, th_constraint *out);
/* Recheck method obligations deferred until an environment is attached.
 * th_ctx_set_environment() calls this automatically. */
bool   th_ctx_validate_constraints(th_ctx *ctx);

void th_register_promotion(th_ctx *ctx, core_string_t from, core_string_t to);
bool th_promotes_to(th_ctx *ctx, core_string_t from, core_string_t to);
bool th_type_promotes_to(th_ctx *ctx, const th_type *from,
                         const th_type *to);

typedef enum : uint8_t {
    TH_COERCE_NONE        = 0,
    TH_COERCE_PROMOTE     = 1 << 0,
    TH_COERCE_DEREF       = 1 << 1,
    TH_COERCE_OPTION_WRAP = 1 << 2,
    TH_COERCE_CUSTOM      = 1 << 3,
    TH_COERCE_NIL_OPTION  = 1 << 4,
    TH_COERCE_RECORD_CONSTRUCT = 1 << 5,
} th_coerce_flag;

void th_ctx_set_method_argument_coercions(th_ctx *ctx, uint8_t allowed);

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
typedef th_type *(*th_coerce_target_fn)(th_ctx *ctx, th_type *from,
                                        void *user);
typedef bool (*th_convert_fn)(th_ctx *ctx, th_type *from, th_type *to,
                              void *site, void *context, void *user);
typedef th_integer_class (*th_integer_class_fn)(th_ctx *ctx, th_type *type);
typedef bool (*th_integer_method_fn)(th_ctx *ctx, th_type *receiver,
                                     const th_constraint *method, void *user);

th_type *th_coerce(th_ctx *ctx, th_type *from, th_coerce_policy policy);

void   th_register_coercion(th_ctx *ctx, th_coerce_fn fn, void *user);
void   th_register_coercion_target(th_ctx *ctx, th_coerce_target_fn fn);
void   th_register_receiver_view(th_ctx *ctx, th_coerce_target_fn fn);
void   th_register_conversion(th_ctx *ctx, th_convert_fn fn, void *user);
void   th_register_integer_class(th_ctx *ctx, th_integer_class_fn fn);
void   th_register_integer_method(th_ctx *ctx, th_integer_method_fn fn,
                                  void *user);
bool   th_var_is_integer(const th_type *var, uint8_t *flags);
bool   th_var_owned_integer(const th_ctx *ctx, const th_type *var);
bool   th_ground_integer(th_ctx *ctx, th_type *var, th_type *type);
bool   th_converts(th_ctx *ctx, th_type *from, th_type *to, void *site,
                   void *context);
bool   th_custom_coerces(th_ctx *ctx, th_type *from, th_type *to);
size_t th_coercion_count(const th_ctx *ctx);
bool   th_coercion_at(const th_ctx *ctx, size_t i, th_coercion_t *out);
void   th_coercions_clear(th_ctx *ctx);

void   th_register_iface(th_ctx *ctx, core_string_t name, th_type *methods);
bool   th_register_method(th_ctx *ctx, th_type *receiver, core_string_t name,
                          th_type *signature);
bool   th_register_method_resolver(th_ctx *ctx,
                                   th_method_resolver_fn resolver,
                                   void *user);
th_type *th_method_lookup(th_ctx *ctx, th_type *receiver, core_string_t name);
/* For an unresolved constrained type variable, true means the trait is a
 * retained obligation, not proof of a concrete implementation or dispatch. */
bool   th_implements(th_ctx *ctx, th_type *type, core_string_t iface);
size_t th_infer_ifaces(th_ctx *ctx, th_type *type, core_string_t *out,
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
bool th_unify_probe(th_ctx *ctx, th_type *a, th_type *b);
bool th_unifies(th_ctx *ctx, th_type *a, th_type *b);
/* Constrains the original monomorphic variables; failure may leave bindings. */
th_type *th_unify_in_place(th_ctx *ctx, th_type *a, th_type *b);
th_type *th_instantiate(th_ctx *ctx, th_type *type);
th_type *th_generalize(th_ctx *ctx, th_type *type);
th_type *th_generalize_excluding(th_ctx *ctx, th_type *type,
                                 th_type *const *environment,
                                 size_t environment_count);

#endif
