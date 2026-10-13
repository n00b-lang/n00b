#pragma once

#include "compiler_tu.h"
#include "lib/link_unit.h"
#include "parse/commander.h"

bool n00bb_finalize_comptime_layout(n00bb_link_analysis_t *analysis);
bool n00bb_execute_comptime(core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            bool debug_enabled, bool native_cpu,
                            const char *compiler_path,
                            core_diagnostics_t *diagnostics);
bool n00bb_build_executable(core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            parse_cmdr_result_t *args,
                            bool debug_enabled, bool native_cpu,
                            const char *compiler_path,
                            core_diagnostics_t *diagnostics);
