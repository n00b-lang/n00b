#pragma once

/**
 * @file attest/n00b_attest_xdg.h
 * @brief Shared XDG path resolver for n00b-attest config-rooted files.
 *
 * Codifies D-052's canonical store-path pattern in a reusable helper.
 * Returns:
 *
 *   - `$XDG_CONFIG_HOME/n00b-attest/<suffix>` when
 *     `XDG_CONFIG_HOME` is set and non-empty.
 *   - `$HOME/.config/n00b-attest/<suffix>` when `XDG_CONFIG_HOME`
 *     is unset or empty.
 *
 * The `n00b-attest/` infix is hardcoded — this helper is
 * n00b-attest-specific and does not generalize to other libn00b
 * consumers. A separate libn00b-core lift would be a distinct
 * concern (DF, not surfaced).
 *
 * # Symbol prefix
 *
 * `n00b_attest_xdg_*` — n00b-attest util namespace, matching the
 * existing `n00b_attest_oci_*` / `n00b_attest_dsse_*` precedent.
 *
 * # Trailing slash policy
 *
 * Returned paths carry **no trailing slash**. The base path is
 * suffixed with `/n00b-attest/` (slash terminator after
 * `n00b-attest`) and the caller-supplied @p suffix is appended
 * verbatim. The suffix must NOT begin with a slash; callers that
 * want subdirectory composition pass the relative path directly
 * (e.g. `"signing-identities/myid.cert.pem"`).
 *
 * # `getenv` exception
 *
 * The implementation uses libc's `getenv("XDG_CONFIG_HOME")` /
 * `getenv("HOME")` under D-052's project-local libc exception
 * (read-side env-var config discovery, n00b-attest-only). A
 * future libn00b `n00b_getenv` lift (DF-010) will eliminate the
 * exception; the helper migrates atomically at that point.
 */

#include <n00b.h>

/**
 * @brief Resolve an n00b-attest XDG-rooted path for @p suffix.
 *
 * @param suffix  Path component appended after `n00b-attest/`. Must
 *                not begin with a leading slash; callers compose
 *                subdirectory paths (e.g.
 *                `"signing-identities/myid.cert.pem"`) directly.
 *                Must be non-null and non-empty; nullptr / empty
 *                returns nullptr.
 *
 * @kw allocator  Optional allocator (default: runtime). Owns the
 *                returned string's backing bytes.
 *
 * @return A new `n00b_string_t *` carrying the resolved path
 *         (`$XDG_CONFIG_HOME/n00b-attest/<suffix>` or
 *         `$HOME/.config/n00b-attest/<suffix>`). Returns nullptr
 *         if both `XDG_CONFIG_HOME` and `HOME` are unset/empty
 *         (the caller cannot resolve a config root at all).
 *
 * @pre @p suffix is non-null and not beginning with `/`.
 * @post Result, when non-null, carries no trailing slash.
 */
extern n00b_string_t *
n00b_attest_xdg_path(n00b_string_t *suffix) _kargs {
    n00b_allocator_t *allocator = nullptr;
};
