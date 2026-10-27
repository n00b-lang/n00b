#pragma once

#include "logic/clpfd_types.h"

core_csp_domain_t core_csp_dom_range(int64_t lo, int64_t hi);
core_csp_domain_t core_csp_dom_singleton(int64_t value);
core_csp_domain_t core_csp_dom_from_values(const int64_t *values,
                                           int32_t count);
core_csp_domain_t core_csp_dom_empty(void);
core_csp_domain_t core_csp_dom_clone(const core_csp_domain_t *domain);
void core_csp_dom_free(core_csp_domain_t *domain);

int64_t core_csp_dom_min(const core_csp_domain_t *domain);
int64_t core_csp_dom_max(const core_csp_domain_t *domain);
int64_t core_csp_dom_size(const core_csp_domain_t *domain);
bool core_csp_dom_is_singleton(const core_csp_domain_t *domain);
bool core_csp_dom_is_empty(const core_csp_domain_t *domain);
bool core_csp_dom_contains(const core_csp_domain_t *domain, int64_t value);
typedef bool (*core_csp_dom_visit_cb)(int64_t value, void *context);
bool core_csp_dom_visit(const core_csp_domain_t *domain,
                        core_csp_dom_visit_cb callback, void *context);

bool core_csp_dom_intersect(core_csp_domain_t *domain,
                            const core_csp_domain_t *other);
bool core_csp_dom_remove_value(core_csp_domain_t *domain, int64_t value);
bool core_csp_dom_restrict_min(core_csp_domain_t *domain, int64_t new_min);
bool core_csp_dom_restrict_max(core_csp_domain_t *domain, int64_t new_max);
