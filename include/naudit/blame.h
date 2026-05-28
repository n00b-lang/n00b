#pragma once

/**
 * @file naudit/blame.h
 * @brief WP-013 — blame-lineage anchor for exemption matching.
 *
 * White paper § 4 (Anchoring) describes the discipline's lineage-anchor
 * approach: an exemption is considered to cover a finding if blame on
 * the finding's lines traces back, through git's lineage chain, to
 * lines that were within the exempted region at the *signing commit*.
 * The signing commit is NOT stored in the exemption record (§ 5.2);
 * it is derived at verification time as the commit that introduced the
 * exemption file into the repository.
 *
 * This module is the libgit2-backed implementation of that anchor.
 * The libgit2 dependency is contained entirely to `src/naudit/blame.c`:
 * this header never includes `<git2.h>`. Callers see only opaque
 * commit-OID strings (40-character lowercase hex) and `n00b_string_t *`
 * paths.
 *
 * # Decisions in scope
 *
 * - **D-X2** (libgit2 validated) — direct git CLI invocation is out.
 * - **D-X5** (matching key formally `(rule_content_hash, blame_anchor)`)
 *   — this module supplies the `blame_anchor` half. The fingerprint
 *   stays in the record as a cross-check + as the fallback for the
 *   pre-commit case (white paper § 4.4).
 * - **path-canonicalization rule** (PR #72) — every helper that
 *   takes a path arg canonicalizes via `n00b_path_canonical` on
 *   entry.
 *
 * Per project DECISIONS.md D-005, naudit functions carry no
 * `_kargs { allocator }` block — naudit's public surface does not
 * expose `.allocator` keyword arguments. Per D-006 / WP-008, naudit
 * headers under `include/naudit/` are unprefixed; symbol-level prefix
 * discipline (`n00b_audit_*`) remains in force.
 *
 * # Decision-points-resolved-inline (sub-agent picks)
 *
 * - **DF-AA — layered HEAD + working-tree blame.** Resolved in
 *   `blame.c` via a two-step libgit2 sequence: first
 *   `git_blame_file` computes a HEAD-only blame to use as the
 *   `base`; then `git_blame_buffer` overlays that base with the
 *   audited file's current working-tree bytes. Committed lines
 *   retain their historical attribution while uncommitted edits
 *   show up with a zero `final_commit_id`. We always perform the
 *   overlay, which subsumes the committed-only single-step case
 *   while keeping the public surface one entry point deep. Both
 *   functions have been present in libgit2 since well before 1.0,
 *   so the meson `>=1.7.0` pin is gated by
 *   `git_blame_options_init` (the most recent API call), not by
 *   the blame entry points themselves.
 * - **DF-AB — blame caching.** Resolved in `blame.c`: PER-CALL
 *   recompute, NO cache state. Rationale: libgit2's blame is fast
 *   enough for naudit's workload (typical exemption file count <100,
 *   typical audit file count similar), and adding a cache means
 *   inventing an invalidation policy against working-tree mutation
 *   that the engine can't observe. If a future WP measures a hot
 *   path we revisit.
 *
 * # Note on libgit2 1.9.3 copy-tracking flags
 *
 * `git_blame_options.min_match_characters` is documented as taking
 * effect only when at least one `GIT_BLAME_TRACK_COPIES_*` flag is
 * set in `git_blame_options.flags`. In libgit2 1.9.3 (per `blame.h`
 * line 38/45/53/62), those flags are explicitly marked "not yet
 * implemented and reserved for future use." We still pass the flag
 * + threshold so the integration is forward-compatible when libgit2
 * ships the implementation; we do NOT rely on copy detection for
 * correctness. The blame heuristic in 1.9.3 still tracks line moves
 * within a file and renames via index-similarity, which covers the
 * core exemption-follow-the-line use cases.
 */

#include <n00b.h>
#include "adt/option.h"

/**
 * @brief One-shot libgit2 library initialization.
 *
 * Calls `git_libgit2_init` once per process; subsequent calls are
 * no-ops (idempotent via a per-process flag). Returns `false` when
 * libgit2 itself reports an init failure — in that case callers
 * should degrade gracefully (treat blame as unavailable + fall back
 * to fingerprint-only matching per the pre-commit path).
 *
 * Every blame-API entry point on this module calls
 * `n00b_audit_blame_init` itself before issuing libgit2 work, so
 * callers usually do NOT need to call it explicitly. Exposed as
 * public surface in case a future host needs to gate startup on
 * library availability.
 *
 * @return `true` on success (or no-op on subsequent calls);
 *         `false` if `git_libgit2_init` returned a negative error
 *         code on the first call.
 */
extern bool
n00b_audit_blame_init(void);

/**
 * @brief Derive the *signing commit* — the commit that introduced
 *        the exemption file — from VCS history.
 *
 * Opens the repository rooted at `repo_path` and runs
 * `git_blame_file` on `exemption_file_path` (made relative to the
 * repo root). Returns the OID of the hunk whose commit has the
 * OLDEST committer timestamp across all hunks in the file. This is
 * the root-introducing commit per white paper § 4.3 — NOT
 * `hunk[0]`, which is the hunk covering line 1 in source-line
 * order rather than chronological order.
 *
 * Both path arguments are canonicalized via `n00b_path_canonical`
 * on entry (per the path-canonicalization rule).
 *
 * The exemption file's *signing commit* is what anchors blame
 * lineage at audit time; it does NOT live in the exemption record
 * (§ 5.2 — embedding a SHA would create a tampering target, would
 * not exist at signing time, and would conflict with history
 * rewrites).
 *
 * Returns an empty `n00b_option_t(n00b_string_t *)` in any of these
 * cases (the caller falls back to fingerprint-only matching, the
 * pre-commit path):
 *   - libgit2 init failure;
 *   - repository can't be opened (path is not in a git tree);
 *   - file is not committed (no hunks; the working-tree-only
 *     "exemption signed locally but not yet committed" case
 *     described in § 4.4);
 *   - blame reports zero hunks (defensive — shouldn't happen
 *     for a committed file but the API contract permits it).
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param repo_path             Path to a directory inside the
 *                              repository (the working-tree root or
 *                              any subdirectory — opened via
 *                              `git_repository_discover` to handle
 *                              both). Must be non-null.
 * @param exemption_file_path   Absolute or relative path to the
 *                              exemption file whose introducing
 *                              commit we want. Must be non-null
 *                              and must resolve to a path inside
 *                              the repository's working tree.
 *
 * @return A "some" option holding the 40-character lowercase hex
 *         `n00b_string_t *` on success, or a "none" option on any
 *         failure / not-found. Test with `n00b_option_is_set` and
 *         unwrap with `n00b_option_get`.
 */
extern n00b_option_t(n00b_string_t *)
n00b_audit_blame_signing_commit(n00b_string_t *repo_path,
                                n00b_string_t *exemption_file_path);

/**
 * @brief Verify the lineage anchor: does the audited finding trace
 *        back to the signing commit's exempted range?
 *
 * Computes blame on `audited_file_path` with a working-tree overlay
 * (via `git_blame_file` for the HEAD base followed by
 * `git_blame_buffer` to overlay the working-tree bytes), then for
 * every source line in `[finding_start_line, finding_end_line]`:
 *
 *   1. Look up the line's blame hunk.
 *   2. Check whether the hunk's `orig_commit_id` matches
 *      `signing_commit_oid_hex` AND the hunk's
 *      `orig_start_line_number .. (orig_start_line_number +
 *      lines_in_hunk - 1)` overlaps with `[exempt_start_line,
 *      exempt_end_line]`.
 *
 * Returns `true` iff EVERY line in the finding's region maps to
 * the signing commit's exempted range (paper § 13.2, "strict"
 * variant — the all-lines-covered rule; configurable later if a
 * looser quorum becomes useful).
 *
 * The `similarity_threshold` parameter is plumbed through to
 * `git_blame_options.min_match_characters` (uint16_t; default 20
 * when the project provides no override). The white paper §§
 * 4.6 / 13.1 explicitly delegate the "appropriately stable" bar
 * to libgit2's heuristics; we expose the knob via the
 * `@blame_similarity` top-level directive in `audit-rules.bnf`.
 * Note: libgit2 1.9.3 only honors this value when a
 * `GIT_BLAME_TRACK_COPIES_*` flag is set in `flags`; we set
 * `GIT_BLAME_TRACK_COPIES_SAME_FILE` for forward-compat (the
 * flag is documented as reserved for future implementation in
 * 1.9.3). Rename detection still works in 1.9.3 via the index
 * similarity machinery driven by `git_diff_options` (libgit2
 * internals) — this gives us the white paper § 4.5 "rename and
 * move are handled by the VCS library" property today.
 *
 * Both path arguments are canonicalized on entry. The
 * `signing_commit_oid_hex` must be the 40-char lowercase hex
 * produced by `n00b_audit_blame_signing_commit` (we parse it via
 * `git_oid_fromstr`).
 *
 * Returns `false` on any of:
 *   - libgit2 init failure;
 *   - repository can't be opened;
 *   - audited file can't be read (the working-tree overlay needs
 *     the current bytes);
 *   - blame call fails (corrupt repo, etc.);
 *   - any finding line lies outside the file's line count;
 *   - any finding line's hunk doesn't trace to the signing
 *     commit's exempted range.
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param repo_path                 Path to a directory inside the
 *                                  repository.
 * @param audited_file_path         Path to the file the finding
 *                                  is in (the working-tree state
 *                                  is what blame layers over HEAD).
 * @param finding_start_line        1-based first source line of
 *                                  the finding.
 * @param finding_end_line          1-based last source line of
 *                                  the finding (inclusive).
 * @param signing_commit_oid_hex    40-char hex OID of the signing
 *                                  commit (the commit that
 *                                  introduced the exemption file).
 * @param exempt_start_line         1-based first source line of
 *                                  the exempted region at the
 *                                  signing commit's view of
 *                                  the exempted file.
 * @param exempt_end_line           1-based last source line of
 *                                  the exempted region (inclusive).
 * @param similarity_threshold      libgit2's
 *                                  `min_match_characters` knob.
 *                                  Pass `0` to let libgit2's
 *                                  default of 20 apply; rule files
 *                                  may set this to any value in
 *                                  `[0, 65535]` via
 *                                  `@blame_similarity`.
 *
 * @return `true` iff blame traces every finding line back to the
 *         exempted range at the signing commit; `false` otherwise.
 */
extern bool
n00b_audit_blame_traces_to(n00b_string_t *repo_path,
                           n00b_string_t *audited_file_path,
                           int64_t        finding_start_line,
                           int64_t        finding_end_line,
                           n00b_string_t *signing_commit_oid_hex,
                           int64_t        exempt_start_line,
                           int64_t        exempt_end_line,
                           int            similarity_threshold);
