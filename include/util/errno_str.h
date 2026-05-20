#pragma once

/**
 * @file util/errno_str.h
 * @brief Allocator-free POSIX errno-to-string accessor.
 *
 * Houses @ref n00b_errno_str — a pure lookup that turns an
 * `errno`-shaped integer code into a short human-readable
 * `n00b_string_t *`. The accessor mirrors the project-internal
 * `n00b_attest_err_str` / `n00b_base64_err_str` shape (switch
 * over integer codes, return rich-string literals, fallback for
 * unknown) and is the §2.10-clean replacement for libc
 * `strerror()` / `strerror_r()` at libn00b call sites.
 *
 * # Why a dedicated helper
 *
 * libn00b APIs return `errno` values through `n00b_result_t(T)`'s
 * Err channel in dozens of places — every wrapper in
 * `include/adt/result.h` (`n00b_check_posix`, `n00b_check_mmap`,
 * `n00b_check_sysconf`), every conduit / fd / file primitive that
 * surfaces a `read(2)` / `write(2)` / `open(2)` failure, every
 * socket entry point. Without a project-local accessor, every
 * call site that wants to log the error has to reach for libc
 * `strerror()` — which violates the §2.10 "no libc I/O / no libc
 * string helpers" discipline.
 *
 * # Symbol prefix
 *
 * `n00b_errno_str` (top-level utility namespace, matching the
 * `n00b_base64_*` / `n00b_json_*` precedents). The accessor is
 * NOT scoped to any subsystem because errno is the cross-cutting
 * POSIX-domain code space; every subsystem that bottoms out in a
 * libc syscall surfaces the same code space.
 *
 * # Allocator discipline
 *
 * @ref n00b_errno_str is a pure lookup — no `_kargs`, no
 * allocator threading. The returned string is a rich-string
 * literal (`r"..."`) with process-lifetime storage; callers must
 * not free it.
 *
 * # Coverage
 *
 * Every POSIX.1-2008 portable errno value is covered. The exact
 * set is enumerated in the implementation and tested for non-empty
 * output in the regression test. Linux-only and BSD-only
 * extensions are NOT in the coverage table because they are not
 * portable; on platforms where they are defined they fall through
 * to the unknown-code fallback string. The fallback string is
 * documented and non-empty so callers that always interpolate the
 * result get a sensible default.
 *
 * # Relationship to libc `strerror`
 *
 * `strerror()` is banned under §2.10 (libc I/O / libc string
 * helpers). @ref n00b_errno_str is the project-local replacement.
 * The output text is not byte-identical to `strerror`'s — it is
 * short, lowercase, and intentionally terse for embedding inside
 * a larger n00b-style diagnostic format. Callers that need the
 * platform's exact `strerror()` text should NOT use this
 * primitive.
 */

#include <n00b.h>

/**
 * @brief Look up a short human-readable string for a POSIX errno
 *        integer.
 *
 * @param errno_val  An `errno`-shaped integer. Both positive
 *                   (libc-native) and negative (the libn00b
 *                   convention of `Err(-errno)` to avoid collision
 *                   with domain codes) values are supported — the
 *                   accessor folds the sign before lookup.
 *
 * @return A non-null `n00b_string_t *` containing a short
 *         description. The returned string is a rich-string
 *         literal with process-lifetime storage; the caller must
 *         NOT free it. Unknown codes (including platform-only
 *         extensions not in the portable POSIX.1-2008 set) return
 *         a documented fallback string of the form
 *         `r"unknown errno value"` (the integer value itself is
 *         not formatted into the message — call sites that need
 *         the integer have it already).
 *
 * @details This accessor is a pure lookup over a hard-coded
 * `switch` table. It allocates nothing and never fails. Repeated
 * calls with the same input return string pointers whose `data`
 * bytes are byte-identical (the rich-literal storage is
 * process-stable).
 *
 * The accessor accepts both signs because libn00b's
 * `n00b_check_posix` wrapper returns `Err(errno)` (positive)
 * directly, but several subsystems re-encode the POSIX code as
 * `Err(-errno)` to avoid collision with their negative
 * module-domain codes. Folding the sign at the lookup boundary
 * means a single accessor serves both conventions.
 *
 * @note The output text is intentionally short and lowercase for
 * embedding inside a larger n00b-style diagnostic format
 * (e.g., `n00b_eprintf(r"open(«#») failed: «#»", path,
 * n00b_errno_str(e))`). It is NOT byte-equivalent to libc
 * `strerror()` output and callers that need exact `strerror()`
 * text should look elsewhere (or, preferably, talk to the
 * orchestrator about lifting that into libn00b too).
 */
extern n00b_string_t *
n00b_errno_str(int errno_val);
