#pragma once

/**
 * @file audit/output.h
 * @brief Audit output renderers — terminal (Phase 4) + JSON (Phase 5).
 *
 * Phase 4 introduces `n00b_audit_print_terminal`, which emits one
 * header line per violation followed by the rule's good-example
 * block (each line prefixed with `  | ` for grep-stability — per
 * the WP-001 preflight Q7 settlement).
 *
 * Phase 5 adds `n00b_audit_print_json`, which emits the violation
 * list as a single JSON document conforming to the v1 schema pinned
 * verbatim in `src/audit/output_json.c`'s prologue comment block.
 *
 * Per project DECISIONS.md D-005, the exported functions carry no
 * `_kargs` block — n00b-audit's own surface does not expose
 * `.allocator` keyword arguments. There is also no `severity` field
 * anywhere in the rendered output (D-005 v1 collapses to "every
 * violation is an error").
 *
 * Per project DECISIONS.md D-006, the header filename is unprefixed
 * (`audit/output.h`, not `audit/n00b_audit_output.h`); symbol-level
 * prefix discipline remains in force.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively, plus `adt/list.h` / `adt/result.h`
 * (these are NOT transitively pulled by `<n00b.h>` — Phase 2 surfaced
 * this footgun, see STATUS.md) and the audit headers carrying the
 * violation + guidance struct definitions.
 */

#include <n00b.h>
#include "adt/list.h"
#include "adt/result.h"

#include "naudit/guidance.h"
#include "naudit/violation.h"

/**
 * @brief Render a violation list to stdout in human-readable
 *        terminal format.
 *
 * Emits one block per violation:
 *
 *   - One **header line**:
 *     `<file>:<line>:<col>: <rule_id>: <title> (<source_doc> <section>)`
 *     where `<title>` is the rule's `title` field (chosen over the
 *     longer `guidance` text for grep-friendliness — WP-001 preflight
 *     Q7 settled on header brevity).
 *   - The rule's **good-example block**, split on `\n` boundaries,
 *     each line prefixed with `  | ` (two spaces, pipe, space — the
 *     grep-stable prefix per the preflight Q7 settlement).
 *
 * Output is emitted through libn00b's conduit / print substrate
 * (`n00b_print` / `n00b_printf`), targeting the runtime's stdout
 * topic (`fd = 1`). No libc I/O.
 *
 * The `guidance` parameter is accepted for symmetry with the future
 * `n00b_audit_print_json` (Phase 5 needs project-level metadata in
 * the JSON envelope); the terminal renderer ignores it for Phase 4
 * since every render-time field comes off the per-violation `rule`
 * back-pointer.
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block — n00b-audit's own surface does not expose `.allocator`
 * keyword arguments.
 *
 * @param violations  Non-null list of `n00b_audit_violation_t *`.
 *                    May be empty (the renderer is a no-op then,
 *                    returning ok-0).
 * @param guidance    Non-null guidance back-pointer (reserved for
 *                    Phase 5 JSON envelope; ignored by the Phase 4
 *                    terminal path but required for API uniformity).
 *
 * @return On success, `n00b_result_ok` wrapping the number of
 *         violations rendered (which is also the violation-list
 *         length). On a required-field-missing failure (e.g., a
 *         violation whose rule lacks a `good_example`), returns
 *         `n00b_result_err` carrying `N00B_AUDIT_ERR_CLI_RENDER`.
 *         Both round-trip through `n00b_audit_err_str`.
 */
extern n00b_result_t(int)
n00b_audit_print_terminal(n00b_list_t(n00b_audit_violation_t *) *violations,
                          n00b_audit_guidance_t                  *guidance);

/**
 * @brief Render a violation list to stdout as a single JSON document.
 *
 * Emits a JSON object whose v1 schema is pinned verbatim at the top
 * of `src/audit/output_json.c` (the JSON-emission source file). The
 * top-level shape is:
 *
 *     {
 *       "violations": [
 *         { "file": ..., "line": ..., "column": ..., "rule_id": ...,
 *           "message": ..., "doc_section": ..., "good_example": ... },
 *         ...
 *       ],
 *       "summary": { "error_count": <int64> }
 *     }
 *
 * Per project DECISIONS.md D-005, there is **no `severity` field**
 * anywhere in the emitted document; `summary.error_count` is the
 * count of `violations` (every violation is implicitly `error`).
 *
 * Output goes through libn00b's `n00b_print` (the same conduit /
 * print substrate the terminal renderer uses, so the Phase 4
 * stdout-capture mechanism — POSIX pipe + dup2 of fd 1 in the test
 * harness — continues to observe this path without harness changes).
 * No libc I/O.
 *
 * The `guidance` parameter is accepted for symmetry with
 * `n00b_audit_print_terminal` and for future v2-schema extensions
 * that may want to embed project-level metadata (`project`,
 * `source_doc`) in the JSON envelope. The v1 path does not read any
 * field off `guidance`; pass non-null anyway so the contract stays
 * uniform.
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block — n00b-audit's own surface does not expose `.allocator`
 * keyword arguments.
 *
 * @param violations  Non-null list of `n00b_audit_violation_t *`.
 *                    May be empty (`violations` array is emitted
 *                    empty and `summary.error_count` is `0`).
 * @param guidance    Non-null guidance back-pointer (reserved for
 *                    future-schema envelope use; required for API
 *                    uniformity).
 *
 * @return On success, `n00b_result_ok` wrapping the number of
 *         violations rendered (which is also the violation-list
 *         length and the value of `summary.error_count`). On a
 *         required-field-missing failure (e.g., a violation whose
 *         rule lacks a `good_example` or whose `file` is null),
 *         returns `n00b_result_err` carrying
 *         `N00B_AUDIT_ERR_CLI_RENDER`. Both round-trip through
 *         `n00b_audit_err_str`.
 */
extern n00b_result_t(int)
n00b_audit_print_json(n00b_list_t(n00b_audit_violation_t *) *violations,
                      n00b_audit_guidance_t                  *guidance);
