/**
 * @file panic.h
 * @brief Always-on, fatal, formatted-message diagnostic.
 *
 * `n00b_panic(fmt, ...)` writes a `PANIC:`-prefixed message to stderr
 * via `n00b_eprintf` and calls `n00b_abort()`.  Use it for unrecoverable
 * conditions detected anywhere in the runtime (capacity exceeded,
 * invariant violated, "should never happen").
 *
 * Format syntax matches `n00b_eprintf` — `«#»` for positional
 * substitution; integer args cast to `int64_t`, string args as
 * `n00b_string_t *`.  See `text/strings/format.h` for full markup.
 */
#pragma once

#include "conduit/print.h"
#include "core/exit.h"

#define n00b_panic(fmt, ...)                                  \
    do {                                                      \
        n00b_eprintf("PANIC: " fmt __VA_OPT__(, ) __VA_ARGS__); \
        n00b_abort();                                         \
    } while (0)
