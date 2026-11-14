#pragma once

#include "compiler_tu.h"
#include "lib/link_unit.h"
#include "parse/commander.h"

typedef struct n00bb_native_inputs n00bb_native_inputs_t;

struct n00bb_native_inputs {
    const core_buffer_t *bitcode;
    size_t bitcode_count;
    bool exports;
    const core_string_t *archives;
    size_t archive_count;
    const core_string_t *libraries;
    size_t library_count;
    const core_string_t *frameworks;
    size_t framework_count;
    const core_string_t *search_paths;
    size_t search_path_count;
    // Chooses again for the runtime program, from the C symbols its runtime
    // code calls: the compile-time program may call more.
    bool (*select_runtime)(void *context, const core_string_t *symbols,
                           size_t count, n00bb_native_inputs_t *out);
    void *select_context;
};

bool n00bb_build_executable(core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            parse_cmdr_result_t *args,
                            bool debug_enabled, bool native_cpu,
                            const n00bb_native_inputs_t *native,
                            core_diagnostics_t *diagnostics);
