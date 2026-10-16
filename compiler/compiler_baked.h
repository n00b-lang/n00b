#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler_link.h"

void n00bb_set_baked_capture(const uint8_t *data, size_t size);
bool n00bb_has_baked_capture(void);
bool n00bb_baked_graph_load(n00bb_link_analysis_t *analysis);
const char *n00bb_baked_root_identifier(const n00bb_link_analysis_t *analysis,
                                        core_string_t module,
                                        core_string_t name);
