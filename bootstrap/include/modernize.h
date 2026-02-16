#pragma once

#include <stdbool.h>

#include "argv_parse.h"

/**
 * @brief Flags controlling which modernization transforms to skip.
 *
 * Set by parsing the NCC_MODERNIZE_SKIP environment variable
 * (comma-separated list of group names). All fields default to false
 * (meaning the transform is enabled).
 */
typedef struct {
    bool skip_keywords;   // _Bool->bool, _Static_assert->static_assert, etc.
    bool skip_includes;   // Remove stdbool.h, stdalign.h, stdnoreturn.h
    bool skip_elifdef;    // #elif defined(X) -> #elifdef X
    bool skip_attributes; // __attribute__((unused)) -> [[maybe_unused]], etc.
    bool skip_builtins;   // __builtin_unreachable -> unreachable(), etc.
    bool skip_empty_init; // = {0} -> = {}
    bool skip_va_paste;   // ##__VA_ARGS__ -> __VA_OPT__(,) __VA_ARGS__
    bool skip_va_start;   // va_start(ap, last) -> va_start(ap)
    bool skip_overflow;   // Manual overflow check -> ckd_* rewrite
    bool skip_nullptr;      // NULL -> nullptr, (void*)0 -> nullptr
    bool skip_pragma_once;  // #ifndef/#define/#endif guard -> #pragma once
} modernize_skip_t;

/**
 * @brief Parse NCC_MODERNIZE_SKIP environment variable into skip flags.
 *
 * Recognizes these group names (comma-separated):
 *   keywords, includes, elifdef, attributes, builtins,
 *   empty-init, va-paste, va-start, overflow, nullptr
 *
 * Unknown names are reported to stderr but do not abort.
 */
extern void modernize_parse_skip(modernize_skip_t *skip);

/**
 * @brief Modernize a C source file to C23.
 *
 * Reads the source file, applies C23 modernization transforms
 * (keyword updates, attribute rewrites, NULL->nullptr, etc.),
 * runs clang-format on the result, and writes to stdout or -o file.
 *
 * @param ctx Parsed command-line arguments (must have exactly one source)
 */
extern void modernize_file(ncc_argv_t *ctx);
