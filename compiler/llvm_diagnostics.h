#pragma once

#include "diagnostics.h"

void n00bb_backend_internal_error(core_diagnostics_t *diagnostics,
                                  core_diag_location_t at,
                                  core_string_t message);
