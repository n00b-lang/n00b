#pragma once

/**
 * @file naudit/signing_ux.h
 * @brief WP-014 — interactive signing UX for exemption proposals.
 *
 * Implements the human-side workflow described in the signed-exemption-
 * records white paper § 11: discover proposal files (`.bnf` exemption
 * drafts lacking a `.sig` sibling), walk the developer through each
 * pending proposal one at a time, BLANK the rationale before
 * prompting, prompt for new rationale + expiration date + approve /
 * decline, then sign the approved record via the WP-012 signing API
 * (`n00b_audit_exemption_sign`).
 *
 * Defining properties (per the white paper and per the WP-014
 * preflight):
 *
 *   1. Per-item review — no bulk-sign in the standard flow. The user
 *      sees, reads, and decides on each proposal individually.
 *   2. Rationale BLANKED before each prompt — the agent's authored
 *      rationale must not pre-fill the developer's input (§ 11.2).
 *   3. `--initial-adoption` is the only bulk-sign path — bounded to
 *      one moment (project adoption) per the white paper § 11.3.
 *   4. Expiration enforced — exemptions whose `expires_at` is in the
 *      past are no longer matched by the engine, even with a valid
 *      signature (§ 11.4). Implemented as a check at the top of
 *      `n00b_audit_exemption_match`.
 *
 * # DF-BA resolution — buffer-backed input source for tests
 *
 * The interactive flow reads lines from an abstracted input source
 * (`n00b_naudit_input_source_t`) with two implementations: one that
 * wraps an fd (real stdin) and one that wraps a pre-loaded
 * `n00b_buffer_t *` (scripted test input). Tests stay in-process and
 * deterministic — no subprocess spawning required. See `signing_ux.c`
 * for the full design rationale.
 *
 * # DF-BB resolution — multiline rationale terminator
 *
 * Resolved as `.`-on-its-own-line (mutt-style) for the multiline
 * rationale prompt. This avoids the silent-truncation hazard a
 * blank-line terminator carries (a developer's multi-paragraph
 * rationale separated by blank lines would be cut short at the first
 * blank line). The terminator is documented in the prompt text the
 * user sees.
 *
 * Per project DECISIONS.md D-005, naudit functions carry no
 * `_kargs { allocator }` block — naudit's public surface does not
 * expose `.allocator` keyword arguments. Per D-006 / WP-008, naudit
 * headers under `include/naudit/` are unprefixed (e.g.,
 * `signing_ux.h`, not `n00b_audit_signing_ux.h`); symbol-level prefix
 * discipline (`n00b_audit_*` on exported public symbols,
 * `n00b_naudit_*` on naudit-internal symbols) remains in force per
 * n00b-api-guidelines § 3.14.
 */

#include <n00b.h>
#include "adt/list.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "core/string.h"

/* ================================================================ */
/* Input-source abstraction (DF-BA resolution)                      */
/* ================================================================ */

/**
 * @brief Kind discriminator for the input-source abstraction.
 *
 * The interactive signing ceremony reads developer input from a
 * line-at-a-time source. The two production modes are:
 *
 *   - `N00B_NAUDIT_INPUT_FROM_FD` — the source wraps a POSIX file
 *     descriptor (typically `STDIN_FILENO` == 0). Used in the real
 *     CLI; reads bytes with `read(2)` and buffers per-line.
 *   - `N00B_NAUDIT_INPUT_FROM_BUFFER` — the source wraps a
 *     pre-loaded `n00b_buffer_t *` carrying scripted input bytes.
 *     Used in unit tests so the entire flow stays in-process and
 *     deterministic.
 *
 * Both kinds present an identical line-reader contract via
 * `n00b_naudit_input_read_line` so the signing flow doesn't branch
 * on the kind at use time.
 */
typedef enum {
    N00B_NAUDIT_INPUT_FROM_FD,
    N00B_NAUDIT_INPUT_FROM_BUFFER,
} n00b_naudit_input_kind_t;

/**
 * @brief Opaque-from-callers input source for the signing ceremony.
 *
 * Construct via `n00b_naudit_input_from_fd` or
 * `n00b_naudit_input_from_buffer`. The struct is heap-allocated so
 * the kind / buffer / fd / cursor state can be carried by reference;
 * callers do not need to inspect the fields.
 *
 * The struct is naudit-internal (`n00b_naudit_*` prefix) per
 * n00b-api-guidelines § 3.14 — the type is visible in the public
 * header because the public sign-proposal entry point takes one as
 * an argument, but the implementation is not part of the audited
 * "public" API surface contract.
 */
typedef struct {
    n00b_naudit_input_kind_t kind;
    /* FD source */
    int            fd;
    /* Buffer source */
    n00b_buffer_t *buffer;
    size_t         cursor;
} n00b_naudit_input_source_t;

/**
 * @brief Construct an input source wrapping a POSIX file descriptor.
 *
 * The returned source reads bytes from `fd` (typically `0` for
 * stdin) one byte at a time on each `read_line` call, accumulating
 * until a `\n` is seen or EOF is reached.
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block.
 *
 * @param fd  File descriptor to read from. `0` for stdin in the
 *            production CLI. The descriptor's lifetime is the
 *            caller's responsibility — the source never closes it.
 *
 * @return Non-null `n00b_naudit_input_source_t *` ready for
 *         `n00b_naudit_input_read_line`.
 */
extern n00b_naudit_input_source_t *
n00b_naudit_input_from_fd(int fd);

/**
 * @brief Construct an input source wrapping a pre-loaded
 *        `n00b_buffer_t *`.
 *
 * Used in tests so scripted input can be provided in-process without
 * subprocess spawning or pipe wiring. Each call to
 * `n00b_naudit_input_read_line` advances an internal cursor through
 * the buffer and returns the next `\n`-terminated line (or the
 * remaining bytes when the buffer's tail has no trailing newline).
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block.
 *
 * @param buffer  Buffer containing the scripted input bytes. May be
 *                empty; `read_line` returns `none` on EOF.
 *
 * @return Non-null `n00b_naudit_input_source_t *`.
 */
extern n00b_naudit_input_source_t *
n00b_naudit_input_from_buffer(n00b_buffer_t *buffer);

/**
 * @brief Read the next line from the input source.
 *
 * Reads bytes up to (and consuming) the next `\n`, or to EOF when
 * the source's tail has no trailing newline. The returned string
 * does NOT include the terminating `\n`. Returns a "some" option
 * holding the line as a fresh `n00b_string_t *`, or a "none" option
 * on EOF.
 *
 * An empty line (a bare `\n`) is returned as a "some" option
 * wrapping an empty `n00b_string_t *` — this is how the multiline
 * rationale loop detects the blank-line case if a future caller
 * chooses that terminator. The mutt-style `.`-on-its-own-line
 * terminator (DF-BB resolution) is detected by callers comparing
 * the returned line text against `"."`.
 *
 * Per n00b-api-guidelines § 5.4, "absence of a line" (EOF) is a
 * valid outcome and is signaled via the option type rather than a
 * nullable pointer.
 *
 * @param src  Input source constructed via
 *             `n00b_naudit_input_from_fd` or
 *             `n00b_naudit_input_from_buffer`. Must be non-null.
 *
 * @return Option wrapping the line text on success; `none` on EOF.
 */
extern n00b_option_t(n00b_string_t *)
n00b_naudit_input_read_line(n00b_naudit_input_source_t *src);

/* ================================================================ */
/* Proposal discovery                                                */
/* ================================================================ */

/**
 * @brief Discover all "proposal" exemption files — `.bnf` files
 *        under `<project_root>/audit/exemptions/` that lack a `.sig`
 *        sibling.
 *
 * A draft exemption written by an agent (per D-X4: proposals are
 * separate from signing) lands in `audit/exemptions/<id>.bnf`
 * without a corresponding `.sig`. The signing-ceremony tool
 * processes those drafts into signed records.
 *
 * The walk:
 *   1. Canonicalize `project_root` via `n00b_path_canonical`.
 *   2. Iterate `.bnf` files in `<root>/audit/exemptions/` (returns
 *      an empty list when the directory is absent).
 *   3. For each `.bnf`, check whether `<file>.sig` exists; if NOT,
 *      include the absolute path of the `.bnf` in the result.
 *   4. Sort the result alphabetically for determinism.
 *
 * Per n00b-api-guidelines § 5.4, I/O errors are surfaced via the
 * result-type error branch rather than via a nullable pointer.
 *
 * @param project_root  Directory rooted at the project (the
 *                      directory containing `audit-rules.bnf`).
 *                      Must be non-null.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null list
 *         (possibly empty) of absolute proposal paths. On failure,
 *         `n00b_result_err` carrying a naudit error code.
 */
extern n00b_result_t(n00b_list_t(n00b_string_t *) *)
n00b_audit_discover_proposals(n00b_string_t *project_root);

/* ================================================================ */
/* Interactive single-proposal sign                                  */
/* ================================================================ */

/**
 * @brief Walk the developer through approving (or declining) one
 *        exemption proposal and, on approve, sign it.
 *
 * Flow (per the WP-014 preflight + white paper § 11):
 *
 *   1. Load the proposal via `n00b_audit_load_exemptions`.
 *   2. Resolve the rule's human name + summary text via the
 *      guidance loader (so the developer sees what they're
 *      exempting).
 *   3. Read the exempted file region (per the locator on the
 *      record); print [start-3 .. end+3] lines with the exempted
 *      lines prefixed with `>` for clarity.
 *   4. Print the draft record's other fields (file_path, locator,
 *      region_fingerprint).
 *   5. BLANK the rationale field via
 *      `n00b_audit_exemption_blank_rationale` (§ 11.2 — must not
 *      pre-fill from agent-authored text).
 *   6. Prompt for the new rationale (multiline; terminator is
 *      `.`-on-its-own-line per DF-BB). Read lines from
 *      `input_source` until the terminator is seen.
 *   7. Prompt for the expiration date. Accept `YYYY-MM-DD`,
 *      `Nd` / `Nm` / `Ny` shorthand, or a blank line for the
 *      default (today + `default_expiry_days`).
 *   8. Prompt approve / decline. Accept `y` / `Y` / `yes` / `YES`
 *      as approve; anything else is decline.
 *   9a. On approve: rewrite the `.bnf` (atomic: write to a `.tmp`
 *       then rename) with the developer's rationale + expiration,
 *       then call `n00b_audit_exemption_sign` to drop the `.sig`.
 *       Return ok-0.
 *   9b. On decline: leave the `.bnf` unchanged; return ok-1.
 *
 * Per n00b-api-guidelines § 5.4, the return shape uses the result
 * type so I/O and signing failures surface explicitly.
 *
 * @param proposal_path        Path to the proposal `.bnf` file.
 *                             Must be non-null.
 * @param key_path             SSH private key path used for
 *                             signing (forwarded verbatim to
 *                             `n00b_audit_exemption_sign`).
 *                             Must be non-null.
 * @param signer_id            Signer principal id (forwarded
 *                             verbatim to
 *                             `n00b_audit_exemption_sign` and
 *                             written into the record before
 *                             signing). Must be non-null.
 * @param input_source         Where to read developer input from
 *                             (typically an fd-wrapping source in
 *                             production; a buffer-wrapping source
 *                             in tests). Must be non-null.
 * @param default_expiry_days  Number of days from today to use as
 *                             the default expiration when the
 *                             developer types a blank line at the
 *                             expiration prompt. The WP-014 plan
 *                             specifies 365.
 *
 * @return On success, `n00b_result_ok` wrapping `0` (signed) or
 *         `1` (declined). On failure, `n00b_result_err` carrying
 *         a naudit error code (load failure, write failure, sign
 *         failure, etc.).
 */
extern n00b_result_t(int)
n00b_audit_sign_proposal_interactive(
    n00b_string_t              *proposal_path,
    n00b_string_t              *key_path,
    n00b_string_t              *signer_id,
    n00b_naudit_input_source_t *input_source,
    int                         default_expiry_days);

/* ================================================================ */
/* Initial-adoption bulk-sign                                        */
/* ================================================================ */

/**
 * @brief Bulk-sign every pending proposal under `audit/exemptions/`
 *        with a standardized rationale + 90-day expiry.
 *
 * This is the `--initial-adoption` path (white paper § 11.3). The
 * paper acknowledges that at project adoption a backlog of
 * pre-existing findings will exist; rather than forcing the team to
 * walk through hundreds of records on day one, the bulk-sign
 * accepts every pending proposal with:
 *
 *   - Rationale: `"preexisting; scheduled for review by <today +
 *     expiry_days, ISO-8601>"`.
 *   - `expires_at`: today + `expiry_days` (ISO-8601 calendar date).
 *
 * The friction that would have happened at adoption is moved to
 * the expiration date — every adopted record must be re-reviewed
 * before it expires.
 *
 * No prompting. Discovers proposals via
 * `n00b_audit_discover_proposals`; for each, rewrites the `.bnf`
 * with the standardized rationale + expiration, then signs via
 * `n00b_audit_exemption_sign`. Prints one line per signed record.
 *
 * @param project_root  Project root directory (containing
 *                      `audit/exemptions/`). Must be non-null.
 * @param key_path      SSH private key path. Must be non-null.
 * @param signer_id     Signer principal id. Must be non-null.
 * @param expiry_days   Number of days from today to use as the
 *                      expiration. WP-014 specifies 90.
 *
 * @return On success, `n00b_result_ok` wrapping the count of
 *         records successfully signed. On any failure
 *         (discovery, write, sign), `n00b_result_err` carrying a
 *         naudit error code (records signed up to the failure
 *         point remain signed).
 */
extern n00b_result_t(int)
n00b_audit_sign_initial_adoption_bulk(n00b_string_t *project_root,
                                       n00b_string_t *key_path,
                                       n00b_string_t *signer_id,
                                       int            expiry_days);

/* ================================================================ */
/* ISO-8601 today + N days (for callers that need the same date)    */
/* ================================================================ */

/**
 * @brief Format the current UTC date plus an offset of `days_offset`
 *        days as an ISO-8601 `YYYY-MM-DD` calendar string.
 *
 * Used by `--initial-adoption`'s rationale + `expires_at` to keep
 * both fields in sync.
 *
 * @param days_offset  Offset from today (positive = future). May
 *                     be zero or negative.
 *
 * @return Non-null `n00b_string_t *` carrying the formatted date.
 */
extern n00b_string_t *
n00b_audit_today_plus_days_iso(int days_offset);
