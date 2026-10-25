#pragma once

#include "compiler_tu.h"
#include "abi_emit.h"
#include "lib/link_unit.h"
#include "parse/commander.h"

bool n00bb_emit_abi_artifacts(core_link_unit_t *link,
                              n00bb_link_analysis_t *analysis,
                              bool debug_enabled);
bool n00bb_register_library_abi(core_link_unit_t *link,
                                n00bb_link_analysis_t *analysis);
// Writes each module's ABI object as DIRECTORY/MODULE.n00bm, for tests of
// the ABI linker (`library --dev-module-abi`).
bool n00bb_write_module_abi_artifacts(core_link_unit_t *link,
                                      core_string_t directory,
                                      core_diagnostics_t *diagnostics);
bool n00bb_write_library_abi_artifact(core_link_unit_t *link,
                                      n00bb_link_analysis_t *analysis,
                                      const core_target_desc_t *target,
                                      parse_cmdr_result_t *args,
                                      core_diagnostics_t *diagnostics);
