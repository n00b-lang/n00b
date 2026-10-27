#pragma once

#include "compiler_cli.h"

#include "lib/string.h"

#include <stdint.h>
#include <stdio.h>

void n00bb_print_toolchain_marker(FILE *out);
int n00bb_toolchain_select(const n00bb_invocation_t *invocation,
                           bool *handled);
core_string_t n00bb_toolchain_selection(void);
core_string_t n00bb_toolchain_selection_problem(void);
int n00bb_unknown_action(core_string_t command, core_string_t action,
                         const core_string_t *choices, size_t count);
int n00bb_toolchain_command(core_string_t action, core_string_t spec,
                            int64_t keep);
int n00bb_cache_command(core_string_t action, bool all);
