#pragma once

/**
 * @file audit/audit.h
 * @brief Public umbrella header for the n00b-audit module.
 *
 * Single include for callers that want the n00b-audit public surface.
 * Phase-N headers under `audit/` are pulled in here as they land; for
 * WP-001 Phases 1–4 this header exposes the module-init declaration,
 * the API-version macro, and the W-4-shaped `n00b_audit_run_cli`
 * library entry point.
 *
 * Per project DECISIONS.md D-006, headers under `include/audit/` are
 * intentionally unprefixed (`audit.h`, not `n00b_audit_audit.h`) —
 * the namespace lives in the include path. Exported symbols and
 * constants still carry the `n00b_audit_` / `N00B_AUDIT_` prefix per
 * n00b-api-guidelines § 3.14 / § 10.2.
 *
 * Per D-005, n00b-audit's own exported functions do **not** take
 * `.allocator` keyword arguments — the n00b-api-guidelines § 4.1
 * allocator rule is interpreted as scoped to libn00b and to libraries
 * built on it that are themselves consumed by further callers.
 * n00b-audit is a standalone CLI consumer with no in-sight performance
 * constraint demanding caller-selected arenas.
 */

#include <n00b.h>
#include "adt/result.h"

#include <naudit/errors.h>
#include <naudit/guidance.h>
#include <naudit/engine.h>
#include <naudit/violation.h>
#include <naudit/output.h>

/**
 * @brief Public API version of the n00b-audit module.
 *
 * Bumped on incompatible changes to any header under
 * `include/audit/`. Consumers can `#if N00B_AUDIT_API_VERSION >= N`
 * to gate feature use once future phases land.
 */
#define N00B_AUDIT_API_VERSION 1

/**
 * @brief Initialize the n00b-audit module.
 *
 * Phase 1 stub: returns `true` to signal the module's link / static-
 * init machinery wired up correctly. Later phases hang per-subsystem
 * initialization off this entry point.
 *
 * @return `true` on success, `false` if any initialization step
 *         failed. (Phase 1 never returns `false`.)
 *
 * No `.allocator` keyword argument — see D-005 above.
 */
extern bool n00b_audit_module_init(void);

/**
 * @brief Library-shaped CLI entry point.
 *
 * Drives the full `n00b-audit <file>` pipeline in-process so the CLI
 * is unit-testable without spawning a subprocess (per WP-001 W-4):
 * parses the supplied argv via libn00b's `slay/commander.h`, resolves
 * the guidance file (either the `--guidance <path>` flag override or
 * a discovery walk up from the current working directory), loads the
 * guidance via `n00b_audit_load_guidance`, builds the engine via
 * `n00b_audit_engine_new`, runs `n00b_audit_engine_check_file` on
 * the positional target, and emits terminal-format violations via
 * `n00b_audit_print_terminal`.
 *
 * Diagnostic output is written to stderr (via
 * `n00b_print(.., .fd = 2)` / `n00b_eprintf`); violation rendering
 * goes to stdout. The thin `main()` in `src/tools/n00b_audit.c`
 * unwraps the result and returns the integer payload as the process
 * exit code.
 *
 * Exit-code contract (the ok branch's int payload):
 *   - `0`  no violations found.
 *   - `1`  at least one violation found.
 *   - `2`  internal error (guidance not found / schema invalid /
 *          target unreadable / parse failure / etc.). A
 *          human-readable diagnostic has already been written to
 *          stderr in this case.
 *
 * The err branch carries one of `N00B_AUDIT_ERR_CLI_BAD_ARGS` (null
 * argv or non-positive argc) or `N00B_AUDIT_ERR_CLI_ARGS` (commander
 * rejected the argv shape). All other failure modes are converted to
 * an ok-2 with a stderr diagnostic so the contract above stays
 * uniform.
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block — n00b-audit's own surface does not expose `.allocator`
 * keyword arguments.
 *
 * @param argc  Argument count (the same value the C runtime hands to
 *              `main`; must be > 0, and `argv[0]` is treated as the
 *              program-name slot).
 * @param argv  Argument vector as `n00b_string_t *` per § 2.2.
 *              `argv[0]` is the program name; positional file argument
 *              and flag values live in subsequent slots.
 *
 * @return On success, `n00b_result_ok` wrapping the integer exit
 *         code (0 / 1 / 2 per the contract above). On argv-shape
 *         failure, `n00b_result_err` carrying
 *         `N00B_AUDIT_ERR_CLI_BAD_ARGS` or
 *         `N00B_AUDIT_ERR_CLI_ARGS`. Both round-trip through
 *         `n00b_audit_err_str`.
 */
extern n00b_result_t(int)
n00b_audit_run_cli(int argc, n00b_string_t *argv[]);
