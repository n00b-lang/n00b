/**
 * @file preprocess.h
 * @brief C preprocessor pre-pass for naudit's parse pipeline.
 *
 * Real n00b C code uses preprocessor-expanded macros heavily
 * (`n00b_alloc(T)`, `typehash(T)`, `n00b_alloc_array_with_opts(T,
 * n, opts)`, etc.). Without preprocessor expansion, the grammar
 * sees raw type-name-as-function-argument constructs that aren't
 * valid C. naudit shells out to `cc -E` before tokenizing, for
 * languages whose `preprocess` flag is set in the language
 * registry (currently: C only).
 */
#pragma once

#include "n00b.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "core/string.h"

/**
 * @brief Run the C preprocessor over a source file.
 *
 * Spawns `ncc -E -x c <cpp_args...> <file_path>`, captures stdout
 * into a fresh buffer. ncc handles the C dialect / version flag
 * dance natively. We pass the file path directly (not stdin)
 * because piping stdin → child while draining child stdout
 * deadlocks on real-sized inputs (child fills the stdout pipe
 * buffer before consuming all stdin).
 *
 * @param file_path  Source file path (already canonical).
 * @param cpp_args   Space-separated additional args to pass to the
 *                   preprocessor invocation (e.g. `-I /path
 *                   -DFOO`). May be `nullptr` or empty.
 *
 * @return Ok(buffer) with preprocessed bytes; Err on subprocess
 *         spawn / wait failure or non-zero preprocessor exit.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_audit_preprocess_c(n00b_string_t *file_path, n00b_string_t *cpp_args);
