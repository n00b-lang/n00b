#pragma once

/**
 * @file naudit/exemption.h
 * @brief WP-011 — exemption + baseline record schema, loader, matcher.
 *
 * Each exemption record carries the data needed to suppress a single
 * audit finding without modifying the offending source or relaxing the
 * rule. WP-011 ships the data-plane foundation: schema, file format,
 * the suppression engine in `n00b_audit_engine_check_file`, and the
 * `naudit baseline --finalize` + `--ignore-baseline` CLI surface.
 *
 * Phase 1 has **no signatures** (WP-012) and **no blame anchor**
 * (WP-013). Matching keys off the rule's content hash and the
 * matched region's normalized fingerprint per D-X5 / preflight § "In
 * scope" item 4 (region fingerprint is the provisional substitute
 * for the blame anchor; WP-013 replaces it with a real lineage
 * check).
 *
 * Per project DECISIONS.md D-006, headers under `include/naudit/` are
 * intentionally unprefixed. Per § 3.14 of n00b-api-guidelines, exported
 * symbols carry the `n00b_audit_` prefix — matching the existing
 * `n00b_audit_rule_t` / `n00b_audit_guidance_t` family. The
 * `n00b_naudit_` prefix is reserved for naudit-internal symbols.
 */

#include <n00b.h>
#include "adt/list.h"
#include "adt/result.h"

#include "naudit/rule.h"
#include "naudit/violation.h"

/**
 * @brief One exemption record loaded from an `audit/exemptions/(*).bnf`
 *        file (or one entry of `audit/baseline/baseline.bnf`).
 *
 * Fields mirror the preflight's "fields" list. All fields are
 * `n00b_string_t *`; the loader treats empty strings as "absent" for
 * the optional fields. The locator integers are parsed as int64 to
 * mirror `n00b_audit_violation_t.line` / `.column` shape.
 *
 * Field meanings:
 *  - `version`             schema version (start at `1`).
 *  - `rule_id`             D-X3 content hash of the rule's
 *                          canonical BNF (hex-encoded). The
 *                          finding's `rule->content_hash` must
 *                          equal this exactly to match.
 *  - `rule_name`           human-readable rule name (e.g.
 *                          `n00b.s2_1.null`). Informational only;
 *                          matching uses `rule_id` per D-X3.
 *  - `file_path`           repo-relative path at signing time.
 *                          Informational; WP-013's blame anchor
 *                          subsumes file-identity matching.
 *  - `locator_line`        1-based line of the exempted region's
 *                          first token. Informational in WP-011 —
 *                          matching keys off `region_fingerprint`,
 *                          not the locator.
 *  - `locator_col`         1-based column of the first token.
 *  - `locator_end_line`    1-based line of the last token.
 *  - `locator_end_col`     1-based exclusive end column.
 *  - `region_fingerprint`  hex-encoded XXH3-128 hash of the
 *                          matched region's canonicalized bytes
 *                          (see `exemption.c` for the
 *                          canonicalization spec). Phase 1 keys
 *                          matching off this field; WP-013
 *                          demotes it to a fallback.
 *  - `rationale`           human-readable justification.
 *  - `signer_id`           placeholder; WP-012 wires it.
 *  - `approved_at`         ISO-8601 timestamp; informational.
 *  - `expires_at`          optional ISO-8601; empty when absent.
 */
typedef struct {
    int64_t        version;
    n00b_string_t *rule_id;
    n00b_string_t *rule_name;
    n00b_string_t *file_path;
    int64_t        locator_line;
    int64_t        locator_col;
    int64_t        locator_end_line;
    int64_t        locator_end_col;
    n00b_string_t *region_fingerprint;
    n00b_string_t *rationale;
    n00b_string_t *signer_id;
    n00b_string_t *approved_at;
    n00b_string_t *expires_at;
    /*
     * WP-012: filesystem path of the file this exemption was loaded
     * from. The signature verification gate uses this to locate
     * the corresponding `<source_file>.sig` and to feed the file's
     * bytes to ssh-keygen via STDIN. Populated by the loader; never
     * appears as an `@directive` field in the record itself.
     */
    n00b_string_t *source_file;
} n00b_audit_exemption_t;

/**
 * @brief Compute the canonical-form content hash of a rule's BNF.
 *
 * Canonicalization (DF-Y, locked in `exemption.c`):
 *   1. Strip comment lines (lines whose first non-whitespace
 *      byte is `#`).
 *   2. For each remaining line: strip trailing whitespace,
 *      strip leading whitespace, normalize CR/LF endings.
 *   3. Drop fully blank lines.
 *   4. Join the surviving lines with a single `\n`.
 *
 * Hash primitive: XXH3-128 via `n00b_hash_raw` (the same primitive
 * backing `n00b_string_t.cached_hash`). The 128-bit result is
 * lowercase hex-encoded as a 32-character `n00b_string_t *`.
 *
 * The hash is stable across comment-only edits and across trivial
 * reformatting (leading / trailing whitespace, blank lines); it
 * changes whenever the production text itself changes.
 *
 * @param bnf_fragment  The rule's BNF body. May be nullptr or empty
 *                      (query-mode rules); in that case the hash is
 *                      computed over the empty canonical string.
 *
 * @return Non-null hex-encoded XXH3-128 hash as a 32-character
 *         lowercase hex string.
 */
extern n00b_string_t *
n00b_audit_compute_rule_content_hash(n00b_string_t *bnf_fragment);

/**
 * @brief Compute the canonical-form fingerprint of a region's bytes.
 *
 * Canonicalization (DF-Y, locked in `exemption.c`):
 *   1. Normalize CR/LF endings to `\n`.
 *   2. For each line: strip leading + trailing whitespace; collapse
 *      runs of internal whitespace (` `, `\t`) into a single space.
 *   3. Drop fully blank lines.
 *   4. Join surviving lines with a single `\n`.
 *
 * Hash primitive: XXH3-128 via `n00b_hash_raw`. The 128-bit result
 * is lowercase hex-encoded as a 32-character `n00b_string_t *`.
 *
 * Whitespace-only changes (extra blank lines, indentation tweaks,
 * trailing-space cleanups) do NOT change the fingerprint. Token-text
 * changes do.
 *
 * @param region_bytes  UTF-8 bytes of the matched span. May be
 *                      nullptr or empty.
 *
 * @return Non-null hex-encoded XXH3-128 hash as a 32-character
 *         lowercase hex string.
 */
extern n00b_string_t *
n00b_audit_compute_region_fingerprint(n00b_string_t *region_bytes);

/**
 * @brief Test whether `exemption` matches the candidate violation.
 *
 * Matching key (WP-013 final form, per D-X5
 * `(rule_content_hash, blame_anchor)`):
 *
 *   1. `exemption->rule_id == violation->rule->content_hash` —
 *      always required.
 *   2. Blame anchor (WP-013 — this revision):
 *      - When `repo_root` is non-null AND
 *        `n00b_audit_blame_signing_commit` resolves a signing
 *        commit for the exemption file: call
 *        `n00b_audit_blame_traces_to` to verify every line in the
 *        finding's range traces back to the exempted range at
 *        the signing commit. BOTH the blame trace AND the
 *        fingerprint must match (the fingerprint becomes a
 *        cross-check per § 4.4 — guards against libgit2's
 *        heuristic accepting drift the developer didn't intend).
 *      - When `repo_root` is null OR the signing commit lookup
 *        returns a "none" option (pre-commit case: exemption file
 *        signed locally but not yet committed): fall back to the
 *        pure-fingerprint match (the WP-011 behavior). The
 *        fingerprint becomes a redundant after the next commit
 *        per § 4.4 last paragraph.
 *
 * Returns `false` immediately if either side is nullptr or carries
 * a null required field; the caller treats false as "not exempted"
 * (the safe default).
 *
 * @param exemption             The exemption record to test against.
 * @param violation             The candidate violation.
 * @param repo_root             Repository root for blame lookup.
 *                              May be nullptr — in that case the
 *                              function takes the pre-commit
 *                              fallback path (pure fingerprint).
 * @param similarity_threshold  libgit2 `min_match_characters` knob;
 *                              0 leaves libgit2's default in place.
 *
 * @return `true` iff the exemption suppresses this finding.
 */
extern bool
n00b_audit_exemption_match(n00b_audit_exemption_t  *exemption,
                            n00b_audit_violation_t  *violation,
                            n00b_string_t           *repo_root,
                            int                      similarity_threshold);

/**
 * @brief Load all exemption records from a single file.
 *
 * The file format reuses the `audit-rule-file.bnf` metaformat and
 * tokenizer; the only difference is the section marker
 * (`@exemption <id>` instead of `@rule <id>`). A single file may
 * contain any number of `@exemption` sections; the loader returns
 * one struct per section.
 *
 * Per the preflight, both individual exemption files
 * (`audit/exemptions/<id>.bnf`) and the multi-entry baseline file
 * (`audit/baseline/baseline.bnf`) use this same format. The loader
 * makes no distinction.
 *
 * The function carries no `_kargs` block per D-005.
 *
 * @param path  Filesystem path to the exemption file.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null list
 *         (possibly empty) of `n00b_audit_exemption_t *`. On
 *         failure, `n00b_result_err` carrying one of:
 *         `N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND`,
 *         `N00B_AUDIT_ERR_GUIDANCE_PARSE`,
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA`,
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION`.
 */
extern n00b_result_t(n00b_list_t(n00b_audit_exemption_t *) *)
n00b_audit_load_exemptions(n00b_string_t *path);

/**
 * @brief Discover and load every exemption file under
 *        `audit/exemptions/(*).bnf` rooted at `project_root`.
 *
 * Reads every `.bnf` file in `<project_root>/audit/exemptions/`
 * (if the directory exists), concatenates all parsed records
 * into a single list. Returns an empty list when the directory
 * is absent or carries no `.bnf` files. Errors from individual
 * files surface as the first failing file's error code.
 *
 * @param project_root  Directory rooted at the project (typically
 *                       the directory containing `audit-rules.bnf`).
 *
 * @return On success, `n00b_result_ok` wrapping a list (possibly
 *         empty). On failure, `n00b_result_err` carrying the same
 *         codes as `n00b_audit_load_exemptions`.
 */
extern n00b_result_t(n00b_list_t(n00b_audit_exemption_t *) *)
n00b_audit_discover_exemptions(n00b_string_t *project_root);

/**
 * @brief Write a baseline file at
 *        `<project_root>/audit/baseline/baseline.bnf` containing one
 *        entry per supplied violation.
 *
 * The file format matches `n00b_audit_load_exemptions` — the same
 * `@exemption <id>` section shape, generated entries use deterministic
 * ids (`baseline_NNNN`). Each entry carries `version=1`, the rule's
 * content hash, the violation's region fingerprint + locator, and a
 * placeholder `signer_id` field (WP-012 fills it). `rationale` is set
 * to a fixed `"baselined at project adoption"` string.
 *
 * If the baseline file already exists, the function refuses with
 * `N00B_AUDIT_ERR_GUIDANCE_SCHEMA` unless `overwrite` is true.
 *
 * @param project_root  Directory rooted at the project.
 * @param violations    The findings to baseline (one entry per).
 * @param overwrite     When false, refuses to clobber an existing
 *                      baseline file.
 *
 * @return On success, `n00b_result_ok` wrapping the number of
 *         entries written. On failure, `n00b_result_err` carrying
 *         one of:
 *         `N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND` (could not
 *         create the baseline directory or write the file),
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA` (existing baseline +
 *         no overwrite).
 */
extern n00b_result_t(int)
n00b_audit_finalize_baseline(
    n00b_string_t                          *project_root,
    n00b_list_t(n00b_audit_violation_t *)  *violations,
    bool                                    overwrite);

/**
 * @brief WP-012 — write the baseline file and immediately sign it.
 *
 * Same shape as `n00b_audit_finalize_baseline` plus an optional
 * signing pass. When `key_path` + `signer_id` are both non-null and
 * non-empty, after the baseline file is written this helper invokes
 * `n00b_audit_exemption_sign` on the produced path to drop
 * `<baseline>.sig` next to it. When either is null/empty, the
 * baseline file is written but no signature is produced — the caller
 * is responsible for emitting a "baseline is unsigned" stderr
 * warning per the CLI contract.
 *
 * Failure to sign after a successful write leaves the baseline file
 * in place + returns `N00B_AUDIT_ERR_SIGN_SUBPROCESS` so the caller
 * can retry or sign manually.
 *
 * @param project_root  Directory rooted at the project.
 * @param violations    The findings to baseline.
 * @param overwrite     When false, refuses to clobber an existing
 *                      baseline file.
 * @param key_path      Optional SSH private key path; when set,
 *                      auto-signs after writing.
 * @param signer_id     Optional principal id; required alongside
 *                      `key_path`.
 *
 * @return Same shape as `n00b_audit_finalize_baseline`. Additional
 *         possible error: `N00B_AUDIT_ERR_SIGN_SUBPROCESS` on
 *         signing failure (file is left in place).
 */
extern n00b_result_t(int)
n00b_audit_finalize_baseline_signed(
    n00b_string_t                          *project_root,
    n00b_list_t(n00b_audit_violation_t *)  *violations,
    bool                                    overwrite,
    n00b_string_t                          *key_path,
    n00b_string_t                          *signer_id);

/**
 * @brief Compute the canonical region bytes for a finding from the
 *        violation's source span.
 *
 * Used by both the engine (to set `violation->region_fingerprint`
 * after a match) and the baseline writer (to emit the fingerprint
 * into the on-disk record).
 *
 * The function reads the file at `file_path` and slices the byte span
 * from (line, column) to (end_line, end_column) per the violation's
 * locator. Returns the canonicalized bytes per
 * `n00b_audit_compute_region_fingerprint`'s spec (whitespace-
 * normalized). Returns nullptr on any I/O failure or out-of-range
 * locator.
 *
 * @param file_path     Path to the audited source file.
 * @param line          1-based start line.
 * @param column        1-based start column.
 * @param end_line      1-based end line.
 * @param end_column    1-based exclusive end column.
 *
 * @return Canonicalized region bytes, or nullptr on failure.
 */
extern n00b_string_t *
n00b_audit_extract_region_bytes(n00b_string_t *file_path,
                                int64_t        line,
                                int64_t        column,
                                int64_t        end_line,
                                int64_t        end_column);

/**
 * @brief Slice a violation's region bytes from an in-memory source
 *        buffer (WP-021 / task #14).
 *
 * Identical span semantics to `n00b_audit_extract_region_bytes` but
 * operates on the already-parsed `src_text` rather than re-reading the
 * file. The engine MUST use this for preprocessed languages: the
 * parse-tree token coordinates index the post-preprocess buffer, which
 * the C preprocessor reflows relative to the on-disk source (e.g.
 * `int *p` → `int * p`), so slicing the raw file with preprocessed
 * coordinates yields the wrong bytes and a spurious per-location
 * fingerprint.
 *
 * @param src_text    The source buffer the parse tree was built from.
 * @param line        1-based start line.
 * @param column      1-based start column.
 * @param end_line    1-based end line.
 * @param end_column  1-based exclusive end column.
 *
 * @return Raw region bytes, or nullptr on out-of-range locator.
 */
extern n00b_string_t *
n00b_audit_extract_region_bytes_from_text(n00b_string_t *src_text,
                                          int64_t        line,
                                          int64_t        column,
                                          int64_t        end_line,
                                          int64_t        end_column);

/**
 * @brief WP-012 — sign an exemption (or baseline) file with the
 *        configured SSH key.
 *
 * Invokes `ssh-keygen -Y sign -f <key_path> -n naudit-exemption-v1
 * <file_path>` as a subprocess. On success, ssh-keygen writes
 * `<file_path>.sig` next to the input. The namespace string
 * (`naudit-exemption-v1`) is fixed per the white paper § 7.1; it
 * prevents cross-protocol reuse of the same SSH key against commit
 * signing or any other SSH-signed artifact.
 *
 * Both path arguments are canonicalized via `n00b_path_canonical`
 * before the subprocess is spawned (per the path-handling rule
 * shipped in PR #72).
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block. Per D-006 / WP-008 naudit headers under `include/naudit/`
 * are unprefixed.
 *
 * @param file_path  Path to the exemption / baseline file to sign.
 * @param key_path   Path to the SSH private key (`-f` argument).
 * @param signer_id  Identifier embedded in the signature record so
 *                   the verifier knows which roster entry to look up.
 *
 * @return On success, `n00b_result_ok` wrapping `0`. On failure,
 *         `n00b_result_err` carrying
 *         `N00B_AUDIT_ERR_SIGN_SUBPROCESS` (spawn / waitpid / non-
 *         zero exit). The caller emits the user-facing diagnostic
 *         via `n00b_audit_err_str`.
 */
extern n00b_result_t(int)
n00b_audit_exemption_sign(n00b_string_t *file_path,
                          n00b_string_t *key_path,
                          n00b_string_t *signer_id);

/**
 * @brief WP-012 — verify the detached signature of an exemption
 *        (or baseline) file against a trust roster.
 *
 * Invokes `ssh-keygen -Y verify -f <roster_path> -I <signer_id>
 * -n naudit-exemption-v1 -s <file_path>.sig`. The data to verify is
 * piped to ssh-keygen's STDIN — this function opens the exemption
 * file, dups its file descriptor to STDIN_FILENO in the child's
 * file_actions, and closes the parent-side copy after the spawn.
 * (The shell-shorthand `< <file>` is NOT implementable verbatim via
 * `posix_spawn`; explicit pipe/dup is required.)
 *
 * Both path arguments are canonicalized via `n00b_path_canonical`
 * before use.
 *
 * Distinct error codes let the loader produce differentiated stderr
 * diagnostics:
 *   - `N00B_AUDIT_ERR_EXEMPTION_NO_SIGNATURE`     missing `.sig`.
 *   - `N00B_AUDIT_ERR_EXEMPTION_BAD_SIGNATURE`    tampered content
 *                                                 or wrong key.
 *   - `N00B_AUDIT_ERR_EXEMPTION_UNKNOWN_SIGNER`   signer not in
 *                                                 roster; collapses
 *                                                 to BAD_SIGNATURE
 *                                                 when ssh-keygen's
 *                                                 output doesn't
 *                                                 allow the
 *                                                 distinction.
 *   - `N00B_AUDIT_ERR_SIGN_SUBPROCESS`            OS-level failure.
 *
 * @param file_path    Path to the exemption / baseline file. The
 *                     `.sig` sibling is derived as
 *                     `<file_path>.sig`.
 * @param roster_path  Path to an OpenSSH `allowed_signers` file
 *                     listing trusted principals + their public
 *                     keys.
 * @param signer_id    Principal identifier (`-I` argument). Must
 *                     match the value the file was signed with.
 *
 * @return On success, `n00b_result_ok` wrapping `0`. On failure,
 *         `n00b_result_err` carrying one of the error codes
 *         described above.
 */
extern n00b_result_t(int)
n00b_audit_exemption_verify(n00b_string_t *file_path,
                            n00b_string_t *roster_path,
                            n00b_string_t *signer_id);

/**
 * @brief WP-014 — clear an exemption record's `rationale` field
 *        in-place.
 *
 * Sets `exemption->rationale` to `n00b_string_empty()` (an empty
 * `n00b_string_t *`, NOT nullptr — matches the established idiom
 * in `exemption.c` so downstream string operations don't have to
 * special-case null). Idempotent: calling on an already-blanked
 * record is a no-op.
 *
 * Used by the WP-014 signing ceremony before each prompt so the
 * developer's input is not pre-filled by the agent's authored
 * rationale text (white paper § 11.2 — the rationale is the human
 * judgment artifact; pre-filling it from agent text would
 * compromise the discipline's premise).
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param exemption  Exemption record whose rationale to clear.
 *                   No-op when `exemption` is null (defensive).
 */
extern void
n00b_audit_exemption_blank_rationale(n00b_audit_exemption_t *exemption);

/**
 * @brief WP-014 — test whether an exemption has expired as of the
 *        supplied ISO-8601 time string.
 *
 * Returns `true` iff:
 *
 *   1. `exemption->expires_at` is non-null and non-empty AND
 *   2. `exemption->expires_at` is lexicographically less than
 *      `now_iso8601`.
 *
 * Lexicographic comparison is correct for both the calendar form
 * (`YYYY-MM-DD`) and the full ISO-8601 instant form
 * (`YYYY-MM-DDTHH:MM:SSZ`) since each more-precise field is
 * subordinate to the less-precise one in left-to-right order.
 *
 * An exemption with no `expires_at` field never expires; the
 * signing ceremony (WP-014) requires the field, but the engine
 * tolerates absence (the WP-011 record schema marked it optional).
 *
 * Pure function — easy to unit-test in isolation. The non-pure
 * "obtain the current time" step lives inside
 * `n00b_audit_exemption_match` so that the WP-013 signature of
 * `_match` (a D-024-preserved schema function) need not change.
 *
 * @param exemption    Exemption record to test. Must be non-null.
 * @param now_iso8601  Current time as an ISO-8601 string. Must be
 *                     non-null and non-empty.
 *
 * @return `true` iff the exemption has expired as of `now_iso8601`.
 */
extern bool
n00b_audit_exemption_is_expired(n00b_audit_exemption_t *exemption,
                                 n00b_string_t          *now_iso8601);
