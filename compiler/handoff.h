#pragma once

#include "lib/string.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    N00BB_PHASE_ORDER_DEFAULT,
    N00BB_PHASE_ORDER_LITERAL_FIRST,
    N00BB_PHASE_ORDER_BUILD_TIME_FIRST,
} n00bb_phase_order_t;

typedef struct {
    int64_t             capture_fd;
    bool                capture_full;
    bool                cache_strict;
    core_string_t       cache_exclude;
    core_string_t       cache_summaries;
    n00bb_phase_order_t phase_order;
    bool                check_runtime_phase;
    core_string_t       check_diagnostics;
    core_string_t       toolchain_reason;
} n00bb_handoff_t;

n00bb_handoff_t *n00bb_handoff(void);
void n00bb_handoff_set(core_string_t *field, core_string_t value);
bool n00bb_handoff_pass(void);
