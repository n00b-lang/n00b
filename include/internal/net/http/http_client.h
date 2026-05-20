/**
 * @file include/internal/net/http/http_client.h
 * @brief Internal-only HTTP client helpers exposed for unit testing.
 *
 * The public surface lives in `include/net/http/http_client.h`; this
 * sibling internal header surfaces matcher primitives that the public
 * dispatcher uses internally but that unit tests need to exercise
 * directly (the matchers are small, side-effect-free, and exercising
 * them at the dispatcher level would require either a live network
 * fixture or a heavyweight redirect mock).
 *
 * Operators should not include this header; the symbols here are not
 * part of the project's API stability promise.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "adt/list.h"

/**
 * @brief Test if @p host matches any entry in @p allowlist, with
 *        wildcard support.
 *
 * @param host       Host string from the parsed URL's authority
 *                   component.  Assumed already lowercased per
 *                   RFC 3986 § 3.2.2 (the URL parser does this).
 * @param allowlist  Pointer to a `n00b_list_t(n00b_string_t *)`
 *                   lvalue.  Each entry is either an exact host
 *                   (no `*`), a wildcard of the form `*.DOMAIN`
 *                   with at least one label after the leading
 *                   `*.`, or malformed (silently skipped).
 *
 * @return `true` on the first matching entry; `false` if the loop
 *         exhausts without a match (including the empty-allowlist
 *         case).
 *
 * @pre  @p allowlist may be `nullptr` (returns false defensively)
 *       but in production the caller checks
 *       `redirect_host_allowlist != nullptr` before this call.
 * @post Side-effect-free.  No allocations on any code path.
 *
 * @details Wildcard semantics:
 *
 *   - `*.example.com` matches `foo.example.com`, `a.b.example.com`,
 *     etc. (one or more labels before the dot).
 *   - `*.example.com` does NOT match the apex `example.com`; callers
 *     who want the apex too must add a second non-wildcard entry.
 *   - Only a single leading `*.` is supported.  `foo.*.com`,
 *     `**.example.com`, `*example.com`, and bare `*` are malformed
 *     and silently skipped.
 *   - Exact-match entries behave identically to the pre-wildcard
 *     dispatcher (ASCII case-insensitive byte equality).
 */
extern bool
host_in_allowlist(n00b_string_t                *host,
                  n00b_list_t(n00b_string_t *) *allowlist);
