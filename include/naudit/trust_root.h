#pragma once

/**
 * @file naudit/trust_root.h
 * @brief WP-015 — system-install roster-chain + trust-root fingerprint
 *        binding + rule-file signature surface.
 *
 * The signed-exemption-records discipline (white paper § 9, § 10,
 * § 12) requires the audit binary and its trust roster to live
 * outside the agent's reach. WP-011..WP-014 built the data-plane,
 * signing, blame anchor, and signing UX entirely in-repo. WP-015
 * lifts the trust root out of the repository:
 *
 *  1. **Roster lookup chain.** Replace the WP-012 single-path
 *     repo-only roster discovery with a 3-slot chain:
 *
 *       - **ENV slot** (`NAUDIT_ROSTER` env var): user/CI top-level
 *         override; wins over everything else.
 *       - **SYSTEM slot**: `/etc/naudit/allowed_signers` by default;
 *         tests override via `NAUDIT_SYSTEM_ROSTER` env var (test-
 *         injection-only — it substitutes the system-slot path
 *         without affecting the ENV-slot semantics).
 *       - **REPO slot**: `<project_root>/audit/allowed_signers`.
 *
 *     First slot whose path exists wins. The engine emits a warning
 *     when the REPO slot is the source (downgrade to informational
 *     via `--repo-protected`).
 *
 *  2. **Fingerprint binding.** When the SYSTEM slot is the source
 *     AND the repo declares an `@expected_roster_sha256` directive
 *     in `audit-rules.bnf`, the engine hashes the on-disk roster
 *     and refuses on mismatch. This binds the repo to a known
 *     trust root: an attacker who substitutes a different roster
 *     on another machine fails the binding check.
 *
 *  3. **Optional rule-file signatures (§ 6.3).** When
 *     `audit-rules.bnf.sig` exists next to the rule file, the
 *     loader verifies it against the trust roster and either
 *     silently accepts (signed-by-roster-signer) or refuses
 *     (signed-by-non-roster-signer). Unsigned rule files warn
 *     (prominently without `--repo-protected`, informationally
 *     with).
 *
 * # Decision points (resolved inline for WP-015 Phase 1)
 *
 *  - **DF-CA — Fingerprint location.** Resolved as a top-level
 *    directive (`@expected_roster_sha256 <hex>`) in
 *    `audit-rules.bnf`, not a separate `audit/trust-root.bnf`
 *    file. Rationale: the existing audit-rule-file metagrammar
 *    already supports new file-level directives via the generic
 *    DIRECTIVE/REST handler (the `@extensions`, `@blame_similarity`
 *    precedent in `guidance.c`); adding a separate file would
 *    duplicate the load/parse plumbing for a single 64-char value.
 *
 *  - **DF-CB — SHA-256 implementation.** Resolved as libn00b's
 *    own `n00b_sha256_hash` (declared in
 *    `<core/sha256.h>`). The helper ships as a standalone
 *    libc-only SHA-256 already used elsewhere in the libn00b
 *    tree; no shell-out to `shasum -a 256` and no inline
 *    reimplementation is needed. Documented in `trust_root.c`.
 *
 *  - **DF-CC — `sign-rules` CLI shape.** Resolved as a flag-based
 *    subcommand (`naudit --sign-rules` + `--key` + `--signer`)
 *    rather than a true commander subcommand. Rationale: the
 *    existing WP-012 `--sign` / `--verify` and WP-014
 *    `--sign-pending` / `--initial-adoption` are all flag-based;
 *    keeping `--sign-rules` in the same idiom keeps the CLI's
 *    parse-shape uniform.
 *
 * Per project DECISIONS.md D-005, functions in this header carry
 * no `_kargs` block. Per D-006, naudit headers under
 * `include/naudit/` are unprefixed; exported symbols carry the
 * `n00b_audit_` prefix per § 3.14 of the API guidelines.
 *
 * Per § 5.4 (no sentinel pointer returns), every "absence is a
 * valid outcome" return uses `n00b_option_t(T)` or
 * `n00b_result_t(T)` rather than a bare pointer-or-null.
 */

#include <n00b.h>
#include "adt/option.h"
#include "adt/result.h"
#include "core/string.h"

/* ================================================================ */
/* Roster lookup chain                                              */
/* ================================================================ */

/**
 * @brief Discriminator for the roster-lookup chain's source slot.
 *
 * Used by the engine (and tests) to inspect which slot of the
 * chain produced the active roster:
 *
 *   - `N00B_AUDIT_ROSTER_SOURCE_ENV` — `NAUDIT_ROSTER` env var
 *     set and pointed at an existing file. User/CI top-level
 *     override.
 *   - `N00B_AUDIT_ROSTER_SOURCE_SYSTEM` — the SYSTEM slot
 *     (`/etc/naudit/allowed_signers` by default, or
 *     `NAUDIT_SYSTEM_ROSTER` when set). Fingerprint-binding
 *     applies only to this slot.
 *   - `N00B_AUDIT_ROSTER_SOURCE_REPO` — the REPO slot
 *     (`<project_root>/audit/allowed_signers`). Engine emits a
 *     warning (downgrade via `--repo-protected`).
 *   - `N00B_AUDIT_ROSTER_SOURCE_NONE` — no slot's path exists.
 */
typedef enum {
    N00B_AUDIT_ROSTER_SOURCE_ENV    = 0,
    N00B_AUDIT_ROSTER_SOURCE_SYSTEM = 1,
    N00B_AUDIT_ROSTER_SOURCE_REPO   = 2,
    N00B_AUDIT_ROSTER_SOURCE_NONE   = 3,
} n00b_audit_roster_source_t;

/**
 * @brief Walk the roster-lookup chain rooted at @p project_root
 *        and return the first slot whose path exists.
 *
 * Chain (highest priority first; first slot whose path exists
 * wins):
 *
 *   1. `NAUDIT_ROSTER` env var → ENV slot.
 *   2. `NAUDIT_SYSTEM_ROSTER` env var if set, else
 *      `/etc/naudit/allowed_signers` → SYSTEM slot.
 *   3. `<project_root>/audit/allowed_signers` → REPO slot.
 *
 * The returned path is absolute and canonicalized via
 * `n00b_path_canonical` per the project-wide path-handling rule.
 *
 * Per n00b-api-guidelines § 5.4, "no roster anywhere on the
 * chain" is a valid outcome (the WP-012 contract: the engine
 * applies the `--allow-unsigned` policy in that case); it is
 * surfaced via the inner option type, not via a nullable
 * pointer.
 *
 * The outer result-err branch is reserved for unrecoverable
 * I/O failures (e.g., env-var read failure on a misconfigured
 * runtime); none of the WP-015 implementation's normal paths
 * surface a non-ok outer result, but the shape lets future
 * implementations escalate without changing the signature.
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param project_root  Project root directory (typically the
 *                       directory containing `audit-rules.bnf`).
 *                       May be `nullptr` — in that case the
 *                       REPO slot is skipped (only ENV +
 *                       SYSTEM slots are considered).
 *
 * @return On success, `n00b_result_ok` wrapping
 *         `n00b_option_some(path)` when a slot's path exists,
 *         or `n00b_option_none()` when no slot matched. The
 *         returned path is absolute + canonicalized.
 */
extern n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_audit_resolve_roster_path(n00b_string_t *project_root);

/**
 * @brief Return the source-slot discriminator for the active
 *        roster, given the same @p project_root.
 *
 * Mirrors the chain walk in `n00b_audit_resolve_roster_path` —
 * the two functions produce consistent answers for the same
 * input (or, more precisely: this function tells the engine
 * which slot the path came from so the engine can apply slot-
 * specific policy without re-encoding the chain).
 *
 * WARNING: calling this separately from
 * `n00b_audit_resolve_roster_path` walks the chain a second time;
 * env-var mutations between the two calls can produce
 * inconsistent answers. Production code paths that need BOTH the
 * path and the source kind should use
 * `n00b_audit_resolve_roster` (below) and cache both off a single
 * chain walk.
 *
 * @param project_root  Same as `_resolve_roster_path`.
 *
 * @return One of `N00B_AUDIT_ROSTER_SOURCE_ENV`,
 *         `N00B_AUDIT_ROSTER_SOURCE_SYSTEM`,
 *         `N00B_AUDIT_ROSTER_SOURCE_REPO`, or
 *         `N00B_AUDIT_ROSTER_SOURCE_NONE`.
 */
extern n00b_audit_roster_source_t
n00b_audit_roster_source_kind(n00b_string_t *project_root);

/**
 * @brief Single-walk variant: resolve the active roster path and
 *        report which slot produced it via the same chain walk.
 *
 * Walks the chain (ENV > SYSTEM > REPO) ONCE and returns both the
 * resolved path and the discriminator. Production code paths that
 * need both values must use this entry rather than calling
 * `n00b_audit_resolve_roster_path` + `n00b_audit_roster_source_kind`
 * back-to-back; the latter pair walks the chain twice and an
 * env-var mutation between the calls can leave the path + kind
 * inconsistent (e.g., path came from ENV slot but the second walk
 * reports SYSTEM).
 *
 * On a NONE outcome, `.path` is `nullptr` and `.source` is
 * `N00B_AUDIT_ROSTER_SOURCE_NONE`.
 *
 * Per the project-wide path-handling rule, the returned `.path`
 * is canonicalized via `n00b_path_canonical` (delegated to the
 * per-slot helpers).
 *
 * @param project_root  Same as `_resolve_roster_path`.
 *
 * @return `n00b_audit_roster_resolution_t` carrying the path
 *         (nullptr on NONE) and the source discriminator.
 */
typedef struct {
    n00b_string_t             *path;
    n00b_audit_roster_source_t source;
} n00b_audit_roster_resolution_t;

extern n00b_audit_roster_resolution_t
n00b_audit_resolve_roster(n00b_string_t *project_root);

/* ================================================================ */
/* SHA-256 helpers                                                   */
/* ================================================================ */

/**
 * @brief Compute the lowercase-hex SHA-256 of the file at
 *        @p roster_path.
 *
 * Reads the file (via libn00b's mmap-backed file substrate) and
 * runs `n00b_sha256_hash` over its bytes. Returns the 32-byte
 * digest as a 64-character lowercase hex string.
 *
 * Per the path-handling rule, @p roster_path is canonicalized
 * via `n00b_path_canonical` before use.
 *
 * Per n00b-api-guidelines § 5.4, I/O failures surface via the
 * result's err branch rather than a nullable string return.
 *
 * @param roster_path  Path to the roster file (or any file).
 *                     Must be non-null.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null
 *         64-character lowercase-hex string. On I/O failure,
 *         `n00b_result_err` carrying
 *         `N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND` (file
 *         could not be opened or read).
 */
extern n00b_result_t(n00b_string_t *)
n00b_audit_roster_sha256(n00b_string_t *roster_path);

/**
 * @brief Verify that the SHA-256 of @p roster_path matches the
 *        expected hex digest @p expected_sha256_hex.
 *
 * The expected digest is compared byte-wise after lowercasing —
 * the directive in `audit-rules.bnf` should already carry a
 * lowercase 64-character hex value, but uppercase / mixed-case
 * input is tolerated.
 *
 * @param roster_path          Path to the on-disk roster.
 * @param expected_sha256_hex  Expected lowercase-hex digest.
 *
 * @return On success, `n00b_result_ok` wrapping `0` (matched)
 *         or `1` (mismatched). On I/O failure (cannot read the
 *         roster), `n00b_result_err` carrying the underlying
 *         error code from `n00b_audit_roster_sha256`.
 */
extern n00b_result_t(int)
n00b_audit_verify_roster_fingerprint(n00b_string_t *roster_path,
                                      n00b_string_t *expected_sha256_hex);

/* ================================================================ */
/* Rule-file signatures (§ 6.3)                                      */
/* ================================================================ */

/**
 * @brief Verify the detached signature on @p audit_rules_path
 *        against the roster + signer.
 *
 * Wraps the existing WP-012 `n00b_audit_exemption_verify`
 * primitive — the namespace + key shape are reused unchanged.
 * The function looks for `<audit_rules_path>.sig` next to the
 * rule file and feeds the rule file's bytes to ssh-keygen via
 * STDIN.
 *
 * Per the path-handling rule, both @p audit_rules_path and
 * @p roster_path are canonicalized via `n00b_path_canonical`
 * before use.
 *
 * Return codes (int payload of the ok branch):
 *   - `0` signature verified against the roster.
 *   - `1` rule file is unsigned (`.sig` sibling absent).
 *   - `2` signature exists but does not verify, OR signer is
 *         not present in the roster. (The underlying
 *         `n00b_audit_exemption_verify` collapses these two
 *         cases when ssh-keygen's output doesn't allow the
 *         distinction; this wrapper preserves that contract.)
 *
 * Distinct return codes (rather than err-branch codes) let the
 * engine apply graded policy:
 *   - `0` → silent accept.
 *   - `1` → warning (prominent without `--repo-protected`).
 *   - `2` → hard refusal.
 *
 * The err branch is reserved for unrecoverable I/O / subprocess
 * spawn failures.
 *
 * @param audit_rules_path  Path to `audit-rules.bnf`.
 * @param roster_path       Path to the OpenSSH `allowed_signers`
 *                           roster.
 * @param signer_id         Principal id expected on the signature.
 *
 * @return On success, `n00b_result_ok` wrapping `0` / `1` /
 *         `2` per above. On unrecoverable failure,
 *         `n00b_result_err` carrying
 *         `N00B_AUDIT_ERR_SIGN_SUBPROCESS`.
 */
extern n00b_result_t(int)
n00b_audit_rules_verify_signature(n00b_string_t *audit_rules_path,
                                   n00b_string_t *roster_path,
                                   n00b_string_t *signer_id);

/**
 * @brief Sign @p audit_rules_path with the SSH key at
 *        @p key_path, producing `<audit_rules_path>.sig`.
 *
 * Wraps `n00b_audit_exemption_sign` — same namespace
 * (`naudit-exemption-v1`), same `posix_spawn` of `ssh-keygen
 * -Y sign`. The shared namespace means a key authorized for
 * exemption signing is automatically authorized for rule-file
 * signing; the white paper § 6.3 + § 7.1 acknowledges this as
 * intentional — both roles speak the same "I am the audit
 * authority" claim.
 *
 * @param audit_rules_path  Path to the rule file to sign.
 * @param key_path          SSH private key path (`-f` arg).
 * @param signer_id         Signer principal id (`-I` arg via
 *                           the embedded signature record).
 *
 * @return Same return shape as
 *         `n00b_audit_exemption_sign`: ok-0 on success,
 *         err-`N00B_AUDIT_ERR_SIGN_SUBPROCESS` on failure.
 */
extern n00b_result_t(int)
n00b_audit_sign_rules(n00b_string_t *audit_rules_path,
                       n00b_string_t *key_path,
                       n00b_string_t *signer_id);
