#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lib/option.h"
#include "lib/result.h"
#include "lib/string.h"
#include "lib/variant.h"

typedef int32_t core_csp_var_id_t;
core_option_decl(core_csp_var_id_t);

typedef struct {
  int64_t lo;
  int64_t hi;
} core_csp_dom_interval_t;

typedef struct {
  int64_t base;
  uint64_t bits;
} core_csp_dom_bitset_t;

typedef struct {
  int64_t *values;
  int32_t count;
  int32_t cap;
} core_csp_dom_sparse_t;

core_variant_decl(core_csp_domain_t, core_csp_dom_interval_t,
                  core_csp_dom_bitset_t, core_csp_dom_sparse_t);

#define CORE_CSP_DOMAIN_INTERVAL 1
#define CORE_CSP_DOMAIN_BITSET 2
#define CORE_CSP_DOMAIN_SPARSE 3

#define core_csp_domain_is_type(domain, Type)                                  \
  ((domain).tag == _Generic((Type){0},                                         \
       core_csp_dom_interval_t: CORE_CSP_DOMAIN_INTERVAL,                      \
       core_csp_dom_bitset_t: CORE_CSP_DOMAIN_BITSET,                          \
       core_csp_dom_sparse_t: CORE_CSP_DOMAIN_SPARSE))

#define core_csp_domain_is_set(domain) ((domain).tag != 0)

#define core_csp_domain_get(domain, Type)                                      \
  _Generic((Type){0},                                                          \
      core_csp_dom_interval_t: core_csp_domain_get_interval,                   \
      core_csp_dom_bitset_t: core_csp_domain_get_bitset,                       \
      core_csp_dom_sparse_t: core_csp_domain_get_sparse)(&(domain))

#define core_csp_domain_set(VariantType, Type, value)                          \
  _Generic((Type){0},                                                          \
      core_csp_dom_interval_t: core_csp_domain_make_interval,                  \
      core_csp_dom_bitset_t: core_csp_domain_make_bitset,                      \
      core_csp_dom_sparse_t: core_csp_domain_make_sparse)(value)

#define core_csp_domain_tag(Type)                                              \
  _Generic((Type){0},                                                          \
      core_csp_dom_interval_t: CORE_CSP_DOMAIN_INTERVAL,                       \
      core_csp_dom_bitset_t: CORE_CSP_DOMAIN_BITSET,                           \
      core_csp_dom_sparse_t: CORE_CSP_DOMAIN_SPARSE)

static inline core_csp_domain_t
core_csp_domain_make_interval(core_csp_dom_interval_t value) {
  return core_variant_make_1(core_csp_domain_t, value);
}

static inline core_csp_domain_t
core_csp_domain_make_bitset(core_csp_dom_bitset_t value) {
  return core_variant_make_2(core_csp_domain_t, value);
}

static inline core_csp_domain_t
core_csp_domain_make_sparse(core_csp_dom_sparse_t value) {
  return core_variant_make_3(core_csp_domain_t, value);
}

static inline core_csp_dom_interval_t
core_csp_domain_get_interval(const core_csp_domain_t *domain) {
  return core_variant_get_1(*domain);
}

static inline core_csp_dom_bitset_t
core_csp_domain_get_bitset(const core_csp_domain_t *domain) {
  return core_variant_get_2(*domain);
}

static inline core_csp_dom_sparse_t
core_csp_domain_get_sparse(const core_csp_domain_t *domain) {
  return core_variant_get_3(*domain);
}

typedef enum {
  CORE_CSP_CON_EQ,
  CORE_CSP_CON_EQ_CONST,
  CORE_CSP_CON_NE,
  CORE_CSP_CON_LT,
  CORE_CSP_CON_LE,
  CORE_CSP_CON_IN,
  CORE_CSP_CON_LINEAR,
  CORE_CSP_CON_ALLDIFF,
} core_csp_con_kind_t;

typedef struct {
  core_csp_con_kind_t kind;
  core_csp_var_id_t *vars;
  int32_t var_count;
  int64_t *coeffs;
  int64_t constant;
  core_csp_domain_t in_domain;
  void *provenance;
  bool entailed;
  bool failed;
} core_csp_constraint_t;

typedef struct {
  core_csp_var_id_t id;
  core_string_t name;
  core_csp_domain_t domain;
  bool ground;
  int64_t value;
} core_csp_var_t;

core_result_decl(core_csp_domain_result_t, const core_csp_domain_t *, int);
core_result_decl(core_csp_bool_result_t, bool, int);
core_result_decl(core_csp_int_result_t, int64_t, int);

typedef struct core_csp_store core_csp_store_t;
typedef bool (*core_csp_solution_cb)(core_csp_store_t *store, void *context);
