#pragma once

#include "compiler_tu.h"
#include "lib/link_unit.h"
#include "parse/commander.h"

#ifdef __APPLE__
#define N00BB_NATIVE_LINKER "/usr/bin/clang"
#else
#define N00BB_NATIVE_LINKER "clang"
#endif

typedef struct {
    const core_buffer_t *bitcode;
    size_t bitcode_count;
    const core_string_t *archives;
    size_t archive_count;
    const core_string_t *libraries;
    size_t library_count;
    const core_string_t *frameworks;
    size_t framework_count;
    const core_string_t *search_paths;
    size_t search_path_count;
} n00bb_native_inputs_t;

bool n00bb_build_time_run_needed(const n00bb_link_analysis_t *analysis);
bool n00bb_build_executable(core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            parse_cmdr_result_t *args,
                            bool debug_enabled, bool native_cpu,
                            const char *compiler_path,
                            const n00bb_native_inputs_t *native,
                            core_diagnostics_t *diagnostics);
