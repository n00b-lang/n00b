#pragma once

#include "logic/clpfd_store.h"

bool core_csp_label(core_csp_store_t *store);
int64_t core_csp_label_all(core_csp_store_t *store,
                           core_csp_solution_cb callback, void *context);
