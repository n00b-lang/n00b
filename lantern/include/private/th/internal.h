#ifndef TH_INTERNAL_H
#define TH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/object_archive.h"
#include "lib/string.h"
#include "logic/datalog.h"
#include "th/environment.h"
#include "th/typehash.h"

typedef struct {
  uint8_t *p;
  size_t n;
} th_slice;

typedef struct {
  core_string_t from;
  core_string_t to;
} th_promo;

typedef struct {
  core_string_t name;
  th_type *methods;
} th_iface;

typedef struct {
  uint64_t hash;
  th_type *type;
  th_type *definition;
  void *owner;
} th_type_record_t;

typedef struct {
  th_type *receiver;
  core_string_t name;
  th_type *signature;
} th_method;

typedef struct {
  th_method_resolver_fn resolver;
  void *user;
} th_method_resolver;

typedef struct {
  core_string_t name;
  core_string_t module;
  core_string_t package;
  th_env_visibility_t visibility;
  th_type *scheme;
  th_type *errors;
  th_type *contracts;
  core_datalog_symbol_t logic_symbol;
} th_env_trait_t;

typedef struct {
  core_string_t qualified_name;
  core_string_t root_name;
  core_string_t module;
  core_string_t package;
  th_env_visibility_t visibility;
  size_t order;
  th_type *scheme;
  th_type *error_type;
  th_type *contract;
  size_t required_params;
  core_datalog_symbol_t logic_symbol;
} th_env_function_t;

typedef struct {
  core_string_t qualified_name;
  core_string_t root_name;
  core_string_t module;
  core_string_t package;
  th_env_visibility_t visibility;
  th_type *type;
} th_env_value_t;

typedef struct {
  core_string_t trait_name;
  th_type *receiver_pattern;
  core_datalog_symbol_t logic_symbol;
} th_env_intrinsic_type_t;

typedef struct {
  size_t trait_index;
  core_string_t method_name;
  core_datalog_symbol_t logic_symbol;
} th_env_method_symbol_t;

typedef struct {
  core_string_t name;
  core_string_t module;
  core_string_t package;
  th_env_visibility_t visibility;
  th_env_type_shape_kind_t shape_kind;
  th_type *shape;
} th_env_type_origin_t;

typedef struct {
  th_env_metadata_owner_t owner_kind;
  core_string_t owner_name;
  core_string_t key;
  uint32_t version;
  const uint8_t *data;
  size_t size;
} th_env_metadata_record_t;

core_list_decl(th_constraint);

typedef struct {
  th_type *var;
  th_constraint method;
} th_pending_method_t;
core_list_decl(th_pending_method_t);
core_list_decl(th_promo);
core_list_decl(th_iface);
core_list_decl(th_type_record_t);
core_list_decl(th_method);
core_list_decl(th_method_resolver);
core_list_decl(th_env_trait_t);
core_list_decl(th_env_function_t);
core_list_decl(th_env_value_t);
core_list_decl(th_env_intrinsic_type_t);
core_list_decl(th_env_method_symbol_t);
core_list_decl(th_env_type_origin_t);
core_list_decl(th_env_metadata_record_t);

struct th_type {
  th_kind kind;
  uint8_t ground;
  bool hashed;
  uint64_t revision;
  uint64_t hash;
  th_normalizer hash_norm;
  union {
    struct {
      core_string_t name;
      bool is_variant;
      bool builtin_known;
      th_arg *args;
      size_t nargs;
      const th_builtin_type_t *builtin;
    } named;
    struct {
      uint64_t width;
      th_bitint_flavor flavor;
    } bitint;
    struct {
      uint8_t attrs;
      th_type *referent;
    } ind;
    struct {
      bool has_extent;
      uint64_t extent;
      th_type *elem;
    } seq;
    struct {
      th_member *members;
      size_t n;
    } record;
    struct {
      core_string_t name;
      th_member *methods;
      size_t n;
      th_trait_requirement *requirements;
      size_t nr;
    } trait;
    struct {
      th_type *ret;
      th_type *error;
      th_param *params;
      size_t np;
      th_param *rest;
      th_kwparam *kw;
      size_t nkw;
    } fn;
    struct {
      uint64_t arity;
      th_type *body;
      th_type **qvars;
    } binder;
    struct {
      uint64_t distance;
      uint64_t position;
    } bound;
    struct {
      th_comb comb;
      th_type **arms;
      size_t n;
    } sum;
    struct {
      uint64_t index;
      uint64_t origin;
      bool has_index;
      bool cons_borrowed;
      th_type *ref;
      core_string_t name;
      core_list_t(th_constraint) cons;
    } var;
  };
};

core_list_decl(th_error_t);
core_list_decl(th_coercion_t);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

typedef struct {
  th_con_kind kind;
  core_string_t name;
  core_string_t module;
  core_string_t package;
  uint64_t receiver_hash;
} th_active_constraint_t;
core_list_decl(th_active_constraint_t);

struct th_ctx {
  core_arena_t arena;
  core_buffer_t scratch;
  th_normalizer norm;
  void *norm_state;
  core_string_t err;
  uint64_t next_var;
  core_list_t(th_promo) promos;
  core_list_t(th_iface) ifaces;
  const th_builtin_type_t *builtin_types;
  size_t builtin_count;
  core_dict_t *builtin_heads;
  core_list_t(th_type_record_t) type_records;
  core_dict_t *closed_type_records;
  core_dict_t *open_type_records;
  core_dict_t *generic_type_heads;
  core_list_t(th_method) methods;
  core_list_t(th_method_resolver) method_resolvers;
  core_dict_t *promo_index;
  core_dict_t *iface_index;
  core_list_t(th_error_t) errors;
  core_list_t(th_coercion_t) coercions;
  core_list_t(th_type_ptr_t) pending_environment_constraints;
  core_list_t(th_active_constraint_t) active_environment_constraints;
  th_coerce_fn custom;
  th_coerce_target_fn custom_target;
  th_coerce_target_fn receiver_view;
  void *custom_user;
  th_convert_fn conversion;
  void *conversion_user;
  th_integer_class_fn integer_class;
  th_integer_method_fn integer_method;
  void *integer_method_user;
  core_list_t(th_pending_method_t) integer_methods_pending;
  uint32_t integer_method_depth;
  uint32_t integer_owner;
  size_t probe_depth;
  bool grounding_integers;
  uint8_t method_argument_coercions;
  th_span_t span;
  core_string_t source_file;
  th_span_t related;
  bool has_related;
  bool probe_errors_discarded;
  const th_env *environment;
  core_string_t module;
  core_string_t package;
  core_dict_t *implements_memo;
  core_dict_t *method_memo;
  core_arena_t memo_arena;
};

struct th_env {
  th_ctx *types;
  core_list_t(th_env_trait_t) traits;
  core_list_t(th_env_function_t) functions;
  core_list_t(th_env_value_t) values;
  core_list_t(th_env_intrinsic_type_t) intrinsic_types;
  core_list_t(th_env_type_origin_t) type_origins;
  core_list_t(th_env_method_symbol_t) method_symbols;
  core_list_t(th_env_metadata_record_t) metadata;
  core_datalog_engine_t *logic;
  core_datalog_relation_id_t candidate_relation;
  core_datalog_relation_id_t intrinsic_type_relation;
  core_object_archive_t *persisted_archive;
  bool owns_types;
  bool frozen;
};

typedef struct {
  core_arena_mark_t arena;
  core_string_t err;
  size_t errors;
  size_t coercions;
  size_t pending_environment_constraints;
  size_t active_environment_constraints;
  size_t type_records;
  size_t methods;
  size_t method_resolvers;
  size_t promos;
  size_t ifaces;
  size_t depth;
} th_probe_t;

void th_var_cons_push(th_type *var, th_constraint constraint);
th_probe_t th_probe_begin(th_ctx *ctx);
bool th_probe_end(th_ctx *ctx, const th_probe_t *probe);

void th_err(th_ctx *ctx, th_err_kind kind, core_string_t constraint,
            th_type *expected, th_type *got, core_string_t message);

void *th_alloc(th_ctx *ctx, size_t n);
uint64_t th_var_fresh_origin(void);
core_string_t th_strdup(th_ctx *ctx, core_string_t s);
size_t th_child_count(const th_type *t);
th_type *th_child(const th_type *t, size_t i);
int th_cmp_slice(const void *a, const void *b);
int th_cmp_member(const void *a, const void *b);
int th_cmp_kw(const void *a, const void *b);
void th_erase_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);
th_type **th_sort_arms(th_ctx *ctx, th_type *const *arms, size_t n);
void th_full_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);
void th_env_release_contents(th_env *env);

static inline const th_type *th_resolve_c(const th_type *t) {
  while (t && t->kind == TH_VAR && t->var.ref) {
    t = t->var.ref;
  }

  return t;
}

#endif
