#ifndef TH_INTERNAL_H
#define TH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/object_archive.h"
#include "logic/datalog.h"
#include "th/environment.h"
#include "th/typehash.h"

typedef struct {
  uint8_t *p;
  size_t n;
} th_slice;

typedef struct {
  const char *from;
  const char *to;
} th_promo;

typedef struct {
  const char *name;
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
  const char *name;
  th_type *signature;
} th_method;

typedef struct {
  th_method_resolver_fn resolver;
  void *user;
} th_method_resolver;

typedef struct {
  const char *name;
  const char *module;
  const char *package;
  th_env_visibility_t visibility;
  th_type *scheme;
  th_type *errors;
  th_type *contracts;
  core_datalog_symbol_t logic_symbol;
} th_env_trait_t;

typedef struct {
  const char *qualified_name;
  const char *root_name;
  const char *module;
  const char *package;
  th_env_visibility_t visibility;
  size_t order;
  th_type *scheme;
  th_type *error_type;
  th_type *contract;
  core_datalog_symbol_t logic_symbol;
} th_env_function_t;

typedef struct {
  const char *qualified_name;
  const char *root_name;
  const char *module;
  const char *package;
  th_env_visibility_t visibility;
  th_type *type;
} th_env_value_t;

typedef struct {
  const char *trait_name;
  th_type *receiver_pattern;
  core_datalog_symbol_t logic_symbol;
} th_env_intrinsic_type_t;

typedef struct {
  size_t trait_index;
  const char *method_name;
  core_datalog_symbol_t logic_symbol;
} th_env_method_symbol_t;

typedef struct {
  const char *name;
  const char *module;
  const char *package;
  th_env_visibility_t visibility;
  th_env_type_shape_kind_t shape_kind;
  th_type *shape;
} th_env_type_origin_t;

typedef struct {
  th_env_metadata_owner_t owner_kind;
  const char *owner_name;
  const char *key;
  uint32_t version;
  const uint8_t *data;
  size_t size;
} th_env_metadata_record_t;

core_list_decl(th_constraint);
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
  uint64_t revision;
  union {
    struct {
      const char *name;
      size_t nlen;
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
      const char *name;
      size_t nlen;
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
      th_type *ref;
      const char *name;
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
  const char *name;
  const char *module;
  const char *package;
  uint64_t receiver_hash;
} th_active_constraint_t;
core_list_decl(th_active_constraint_t);

struct th_ctx {
  core_arena_t arena;
  core_buffer_t scratch;
  th_normalizer norm;
  void *norm_state;
  const char *err;
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
  void *custom_user;
  uint8_t method_argument_coercions;
  th_span_t span;
  th_span_t related;
  bool has_related;
  const th_env *environment;
  const char *module;
  const char *package;
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
  uint32_t archive_schema_version;
  bool owns_types;
  bool frozen;
};

void th_err(th_ctx *ctx, th_err_kind kind, const char *constraint,
            th_type *expected, th_type *got, const char *message);

void *th_alloc(th_ctx *ctx, size_t n);
char *th_strdup(th_ctx *ctx, const char *s, size_t n);
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
