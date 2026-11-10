#pragma once

#include "compiler_tu.h"

bool publish_declared_aggregate_layouts(n00bb_link_analysis_t *analysis);
bool publish_interned_variant_layouts(n00bb_link_analysis_t *analysis);
bool select_storage_placement(n00bb_link_analysis_t *analysis);
