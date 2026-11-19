#ifndef TH_INTERNAL_H
#define TH_INTERNAL_H

#include <stdatomic.h>
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
  uint64_t hash;
  th_type *type;
  th_type *definition;
  void *owner;
} th_type_record_t;

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
  th_type *requirements;
  th_con_kind *requirement_kinds;
  core_string_t *requirement_traits;
  size_t requirement_count;
  core_datalog_symbol_t logic_symbol;
} th_env_function_t;

th_type *th_env_function_copy_source(const th_env_function_t *function);
bool th_env_copy_function(th_env *env, const th_env_function_t *function,
                          th_type *copied);

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
core_list_decl(th_type_record_t);
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
  // `hash` holds the hash of a type that is closed but not ground (it
  // reaches bound variables but no unbound one), valid while
  // th_shape_generation still equals closed_generation: see th_hash.
  bool closed_hashed;
  uint64_t hash;
  uint64_t closed_generation;
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
      bool open_slot;
      th_type *ref;
      core_string_t name;
    } var;
  };
};

core_list_decl(th_error_t);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

struct th_ctx {
  core_arena_t arena;
  core_buffer_t scratch;
  th_normalizer norm;
  void *norm_state;
  core_string_t err;
  uint64_t next_var;
  const th_builtin_type_t *builtin_types;
  size_t builtin_count;
  core_dict_t *builtin_heads;
  core_list_t(th_type_record_t) type_records;
  core_dict_t *closed_type_records;
  core_dict_t *open_type_records;
  core_dict_t *generic_type_heads;
  core_list_t(th_error_t) errors;
  th_span_t span;
  core_string_t source_file;
  th_span_t related;
  bool has_related;
  const th_env *environment;
  core_string_t module;
  core_string_t package;
  core_dict_t *implements_memo;
  core_dict_t *method_memo;
  // Closed types parsed from their text, by the text.
  core_dict_t *text_types;
  core_arena_t memo_arena;
};

typedef struct {
  core_dict_t heads;
  core_list_t(uint32_t) next;
  size_t indexed;
  bool ready;
} th_env_index_t;

void th_env_reset_indexes(th_env *env);

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
  th_env_index_t type_index;
  th_env_index_t value_index;
  th_env_index_t metadata_index;
  bool owns_types;
  bool frozen;
};


void th_err(th_ctx *ctx, th_err_kind kind, core_string_t constraint,
            th_type *expected, th_type *got, core_string_t message);

void *th_alloc(th_ctx *ctx, size_t n);
uint64_t th_var_fresh_origin(void);
core_string_t th_strdup(th_ctx *ctx, core_string_t s);
int th_cmp_slice(const void *a, const void *b);
int th_cmp_member(const void *a, const void *b);
int th_cmp_kw(const void *a, const void *b);
void th_erase_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);
th_type **th_sort_arms(th_ctx *ctx, th_type *const *arms, size_t n);
void th_full_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out);
void th_env_release_contents(th_env *env);

// Changes whenever a type graph changes in a way that can alter what an
// existing, variable-free resolution of it encodes to: a bound variable
// rebound, or a record stub filled in. Binding an unbound variable doesn't
// change it, since no closed type reaches an unbound variable; nor does a
// match undoing its trial bindings (see th_trial_bindings).
extern _Atomic uint64_t th_shape_generation;

static inline void th_note_shape_change(void) {
  atomic_fetch_add_explicit(&th_shape_generation, 1, memory_order_relaxed);
}

// How many matches are open with trial bindings they may undo. While any is,
// th_hash records no closed-type hashes, since a type it sees closed may be
// closed only by a binding about to be undone.
extern _Atomic uint64_t th_trial_bindings;

static inline void th_trial_bindings_open(void) {
  atomic_fetch_add_explicit(&th_trial_bindings, 1, memory_order_acq_rel);
}

static inline void th_trial_bindings_close(void) {
  atomic_fetch_sub_explicit(&th_trial_bindings, 1, memory_order_acq_rel);
}

static inline const th_type *th_resolve_c(const th_type *t) {
  while (t && t->kind == TH_VAR && t->var.ref) {
    t = t->var.ref;
  }

  return t;
}

#endif
