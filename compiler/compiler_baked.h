#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler_link.h"

void n00bb_set_baked_capture(const uint8_t *data, size_t size, bool full);
bool n00bb_has_baked_capture(void);
bool n00bb_full_build_time_stage_ran(void);
bool n00bb_literal_values_captured(void);
void n00bb_set_baked_values_opaque(bool opaque);
bool n00bb_baked_values_opaque(void);
bool n00bb_baked_graph_load(n00bb_link_analysis_t *analysis);
core_string_t n00bb_baked_root_identifier(const n00bb_link_analysis_t *analysis,
                                          core_string_t module,
                                          core_string_t name);
