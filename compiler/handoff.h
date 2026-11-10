#pragma once

#include "lib/string.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool          cache_strict;
    core_string_t cache_exclude;
    core_string_t cache_summaries;
    core_string_t toolchain_reason;
} n00bb_handoff_t;

n00bb_handoff_t *n00bb_handoff(void);
void n00bb_handoff_set(core_string_t *field, core_string_t value);
bool n00bb_handoff_pass(void);
