#pragma once

#include "compiler_tu.h"
#include "abi_emit.h"
#include "lib/link_unit.h"
#include "parse/commander.h"

bool n00bb_link_abi_artifact(core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             const core_target_desc_t *target,
                             bool final_image, uint8_t **out_object,
                             size_t *out_object_bytes,
                             core_diagnostics_t *diagnostics);
bool n00bb_emit_abi_artifacts(core_link_unit_t *link,
                              n00bb_link_analysis_t *analysis,
                              bool debug_enabled,
                              bool root_only);
bool n00bb_write_compile_abi_artifact(core_link_unit_t *link,
                                      parse_cmdr_result_t *args,
                                      core_diagnostics_t *diagnostics);
bool n00bb_write_library_abi_artifact(core_link_unit_t *link,
                                      n00bb_link_analysis_t *analysis,
                                      const core_target_desc_t *target,
                                      parse_cmdr_result_t *args,
                                      core_diagnostics_t *diagnostics);
