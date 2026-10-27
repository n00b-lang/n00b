#pragma once

#include "logic/clpfd_domain.h"

core_csp_store_t *core_csp_store_new(void);
void core_csp_store_free(core_csp_store_t *store);

core_csp_var_id_t core_csp_new_var(core_csp_store_t *store, core_string_t name,
                                   core_csp_domain_t domain);
core_option_t(core_csp_var_id_t)
    core_csp_find_var(core_csp_store_t *store, core_string_t name);
core_csp_domain_result_t core_csp_var_domain(core_csp_store_t *store,
                                             core_csp_var_id_t var);
core_csp_bool_result_t core_csp_var_is_ground(core_csp_store_t *store,
                                              core_csp_var_id_t var);
core_csp_int_result_t core_csp_var_value(core_csp_store_t *store,
                                         core_csp_var_id_t var);
int32_t core_csp_var_count(const core_csp_store_t *store);

typedef bool (*core_csp_dom_iter_cb)(int64_t value, void *context);
int32_t core_csp_dom_iterate(core_csp_store_t *store, core_csp_var_id_t var,
                             core_csp_dom_iter_cb callback, void *context);

bool core_csp_post_eq(core_csp_store_t *store, core_csp_var_id_t x,
                      core_csp_var_id_t y);
bool core_csp_post_eq_with_provenance(core_csp_store_t *store,
                                      core_csp_var_id_t x, core_csp_var_id_t y,
                                      void *provenance);
bool core_csp_post_eq_const(core_csp_store_t *store, core_csp_var_id_t x,
                            int64_t value);
bool core_csp_post_eq_const_with_provenance(core_csp_store_t *store,
                                            core_csp_var_id_t x, int64_t value,
                                            void *provenance);
bool core_csp_post_ne(core_csp_store_t *store, core_csp_var_id_t x,
                      core_csp_var_id_t y);
bool core_csp_post_ne_with_provenance(core_csp_store_t *store,
                                      core_csp_var_id_t x, core_csp_var_id_t y,
                                      void *provenance);
bool core_csp_post_lt(core_csp_store_t *store, core_csp_var_id_t x,
                      core_csp_var_id_t y);
bool core_csp_post_lt_with_provenance(core_csp_store_t *store,
                                      core_csp_var_id_t x, core_csp_var_id_t y,
                                      void *provenance);
bool core_csp_post_le(core_csp_store_t *store, core_csp_var_id_t x,
                      core_csp_var_id_t y);
bool core_csp_post_le_with_provenance(core_csp_store_t *store,
                                      core_csp_var_id_t x, core_csp_var_id_t y,
                                      void *provenance);
bool core_csp_post_in(core_csp_store_t *store, core_csp_var_id_t x,
                      core_csp_domain_t domain);
bool core_csp_post_in_with_provenance(core_csp_store_t *store,
                                      core_csp_var_id_t x,
                                      core_csp_domain_t domain,
                                      void *provenance);
bool core_csp_post_linear(core_csp_store_t *store,
                          const core_csp_var_id_t *vars, const int64_t *coeffs,
                          int32_t count, int64_t rhs);
bool core_csp_post_linear_with_provenance(core_csp_store_t *store,
                                          const core_csp_var_id_t *vars,
                                          const int64_t *coeffs, int32_t count,
                                          int64_t rhs, void *provenance);
bool core_csp_post_alldiff(core_csp_store_t *store,
                           const core_csp_var_id_t *vars, int32_t count);
bool core_csp_post_alldiff_with_provenance(core_csp_store_t *store,
                                           const core_csp_var_id_t *vars,
                                           int32_t count, void *provenance);

bool core_csp_propagate(core_csp_store_t *store);
int32_t core_csp_failed_constraint(const core_csp_store_t *store);
void *core_csp_failed_provenance(const core_csp_store_t *store);
void core_csp_push_state(core_csp_store_t *store);
void core_csp_pop_state(core_csp_store_t *store);
