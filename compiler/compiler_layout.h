#pragma once

#include "compiler_tu.h"

bool publish_known_scalar_layouts(n00bb_link_analysis_t *analysis);
bool publish_requested_c_record_layouts(n00bb_link_analysis_t *analysis,
                                        bool require_complete);
bool publish_declared_aggregate_layouts(n00bb_link_analysis_t *analysis);
bool publish_interned_variant_layouts(n00bb_link_analysis_t *analysis);
bool select_storage_placement(n00bb_link_analysis_t *analysis);
bool n00bb_has_deferred_shape_layout(n00bb_link_analysis_t *analysis,
                                      bool report_unresolved);
