/**
 * @file print.h
 * @brief Print API: convert objects to strings and write to file descriptors.
 *
 * `n00b_print()` converts its argument to an `n00b_string_t` via the
 * vtable `N00B_BI_TO_STRING` slot, serializes it to UTF-8 bytes, and
 * writes them to a managed file descriptor using `n00b_write`.
 *
 * By default, print writes to fd 1 (stdout) using the runtime's
 * pre-managed fd owners.  Pass `.fd = 2` for stderr, or `.topic` to
 * redirect into a conduit pipeline.
 *
 * Usage:
 * @code
 *     n00b_print(my_string_ptr);
 *     n00b_print(my_int64_ptr, .fd = 2);
 *     n00b_print(my_obj, .topic = my_pipeline_topic);
 * @endcode
 */
#pragma once

#include "conduit/rw.h"
#include "core/option.h"
#include "core/type_info.h"
#include "core/string.h"
#include "strings/format.h"

n00b_option_decl(n00b_string_t);

/**
 * @brief Convert a managed object to its string representation.
 *
 * Looks up `N00B_BI_TO_STRING` in the object's vtable and calls it.
 * Returns a fallback `"<typename@0xADDR>"` if the type has no
 * `TO_STRING` slot, and `"(null)"` for nullptr.
 *
 * @param obj  Pointer to a managed allocation (or nullptr).
 * @return     String representation (by value).
 */
extern n00b_string_t n00b_to_string(void *obj);

/**
 * @brief Print an object to a file descriptor or conduit topic.
 *
 * Converts @p obj to a string via `n00b_to_string()`, appends the
 * @p end string, and writes the result.
 *
 * By default writes synchronously to stdout (fd 1) via the runtime's
 * managed fd owner and `n00b_write()`.  Pass `.fd = 2`
 * for stderr or any other managed fd.  Pass `.topic` to write to an
 * arbitrary conduit topic instead.
 *
 * @param obj  Object to print (looked up via vtable TO_STRING).
 *
 * @kw topic  Buffer topic to write to (nullptr = use fd path).
 * @kw end    String appended after the object (nullptr = "\\n").
 * @kw fd     File descriptor to write to (default: 1 = stdout).
 * @kw sync   If true (default), block until write completes.
 */
extern void
n00b_print(void *obj) _kargs
{
    n00b_conduit_topic_t(n00b_buffer_t *) *topic = nullptr;
    n00b_option_t(n00b_string_t)           end   = n00b_option_none(n00b_string_t);
    int                                    fd    = 1;
    bool                                   sync  = true;
};

/**
 * @brief Format a string and print it to a file descriptor.
 *
 * Combines `n00b_cformat()` and `n00b_print()` — formats @p fmt with
 * the variadic substitution arguments, appends @p end, and writes to
 * the file descriptor.
 *
 * @param fmt  NUL-terminated C format descriptor (rich markup).
 * @param +    Variadic substitution arguments.
 *
 * @kw topic Buffer topic to write to (nullptr = use fd path).
 * @kw fd    File descriptor (default: 1 = stdout).
 * @kw end   String appended after formatted output (default: "\\n").
 * @kw sync  If true (default), block until write completes.
 */
extern void
n00b_printf(const char *fmt, +) _kargs
{
    n00b_conduit_topic_t(n00b_buffer_t *) *topic = nullptr;
    int                                    fd    = 1;
    n00b_option_t(n00b_string_t)           end   = n00b_option_none(n00b_string_t);
    bool                                   sync  = true;
};

/**
 * @brief Format a string and print it to stderr.
 *
 * Macro that forwards to `n00b_printf` with `.fd = 2`.
 */
#define n00b_eprintf(fmt, ...) n00b_printf(fmt, ##__VA_ARGS__, .fd = 2)
