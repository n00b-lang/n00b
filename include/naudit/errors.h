#pragma once

/**
 * @file audit/errors.h
 * @brief n00b-audit domain error codes + their string accessor.
 *
 * Negative integers (all magnitudes >= 9000) to stay clear of:
 *   - libc `errno` values (small positives), which `n00b_file_*` and
 *     friends pass back through `n00b_result_get_err` unchanged;
 *   - libn00b's own domain codes, which occupy the -1..-10 single-
 *     digit range and the -220..-223 / -1001+ / -2005 / -3001+ /
 *     -3100+ / -4001+ / -5001+ / -6001+ / -7001+ multi-thousand bands
 *     (verified by grep over `subprojects/n00b/include/`).
 *
 * n00b-audit owns the -9001+ range, leaving the -7xxx (chalk) and
 * -8xxx (currently unused; reserved by libn00b for future use) bands
 * intact. Future n00b-audit phases that add ENGINE_* and CLI_* codes
 * continue in this range.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively for the `n00b_string_t` declaration
 * used by `n00b_audit_err_str`.
 */

#include <n00b.h>

/**
 * @brief The guidance file at the supplied path does not exist or
 *        cannot be opened.
 *
 * Returned by `n00b_audit_load_guidance` when `n00b_file_open` fails
 * (ENOENT, EACCES, etc.). The underlying errno is not preserved on
 * this path — callers needing the precise OS-level reason should
 * verify file existence before invoking the loader.
 */
#define N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND       (-9001)

/**
 * @brief The guidance file's bytes cannot be parsed.
 *
 * Returned by `n00b_audit_load_guidance` when the file's text
 * fails to parse against the audit-rule-file metagrammar (WP-005;
 * format v2 — slay-format `.bnf` rule files with `@directive`
 * metadata). Pre-WP-005 this code was `N00B_AUDIT_ERR_GUIDANCE_JSON`
 * and corresponded to `n00b_json_parse` returning nullptr; the
 * numeric value (-9002) is preserved across the rename for the
 * v2 file-format swap.
 */
#define N00B_AUDIT_ERR_GUIDANCE_PARSE           (-9002)

/**
 * @brief The guidance JSON parses, but does not match the v1 schema.
 *
 * Returned by `n00b_audit_load_guidance` when the root value is not
 * a JSON object, when a required top-level field is missing or has
 * the wrong type, or when any required per-rule field is missing or
 * has the wrong type. `schema_version` is validated against the
 * separate `N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION` code rather than
 * this one.
 */
#define N00B_AUDIT_ERR_GUIDANCE_SCHEMA          (-9003)

/**
 * @brief The guidance JSON's `schema_version` field is present and
 *        well-typed, but its value is not `1`.
 *
 * Returned by `n00b_audit_load_guidance`. v1 hard-rejects all other
 * schema versions; there is no forward-compat tolerance. Bump the
 * schema-version contract here (and in the loader) when the v2
 * schema lands.
 */
#define N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION  (-9004)

/**
 * @brief The guidance file declares a non-empty `dependencies` list,
 *        and the loader does not yet support recursive dependency
 *        resolution.
 *
 * Returned by `n00b_audit_load_guidance`. WP-001 ships only the
 * empty-list path; non-empty `dependencies` lists are deferred to
 * WP-002+ (DF-B in the project's deferrals list: cycle / conflict /
 * path-expansion semantics need their own design pass before being
 * implemented). The loader returns this specific code rather than
 * silently merging or silently skipping.
 */
#define N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED (-9005)

/**
 * @brief The audit engine could not load the base C-ncc grammar.
 *
 * Returned by `n00b_audit_engine_new` when `n00b_bnf_load` on the
 * vendored `c_ncc.bnf` (path resolved via `N00B_AUDIT_GRAMMAR_PATH`
 * from `audit_paths.h`) returns false, or when the grammar file
 * can't be opened. If this fires, the project's vendored ncc subtree
 * is likely out of sync with the slay BNF loader's expectations —
 * surface to the orchestrator rather than papering over.
 */
#define N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD    (-9101)

/**
 * @brief One of the guidance's rules has a `bnf_fragment` that
 *        failed to compose with the base grammar.
 *
 * Returned by `n00b_audit_engine_new` when the combined-base+fragments
 * BNF text loads cleanly on its own but the merged variant fails —
 * indicating a syntactic error in a fragment or a production conflict
 * with the base grammar. The specific rule isn't surfaced in the
 * error code; callers needing per-rule diagnostics should validate
 * fragments individually in a future WP.
 */
#define N00B_AUDIT_ERR_ENGINE_RULE_MERGE      (-9102)

/**
 * @brief The target source file failed to parse against the merged
 *        grammar.
 *
 * Returned by `n00b_audit_engine_check_file` when `n00b_parse`
 * reports failure on the file's contents. Common causes: the file
 * uses extensions outside the C-ncc grammar's coverage, or the file
 * has a syntax error.
 */
#define N00B_AUDIT_ERR_ENGINE_PARSE           (-9103)

/**
 * @brief The target source file could not be opened.
 *
 * Returned by `n00b_audit_engine_check_file` when `n00b_file_open`
 * fails (ENOENT, EACCES, etc.). The underlying errno is not
 * preserved on this path — callers needing the precise OS-level
 * reason should verify file existence before invoking the engine.
 */
#define N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND (-9104)

/**
 * @brief A required argument to an engine API was null or
 *        uninitialized.
 *
 * Returned by `n00b_audit_engine_new` when the supplied guidance
 * struct is null or carries no `rules` list, and by
 * `n00b_audit_engine_check_file` when either the engine handle or
 * the target path is null. Distinct from GRAMMAR_LOAD and
 * TARGET_NOT_FOUND so the caller's diagnostic does not misattribute
 * a programming error to a file-system or grammar failure.
 */
#define N00B_AUDIT_ERR_ENGINE_BAD_ARGS         (-9105)

/**
 * @brief No language matches the audited file's extension.
 *
 * Returned by `n00b_audit_engine_check_file` (WP-009 Phase 1) when
 * the engine's extension → language lookup (built-in registry +
 * project overrides) yields no match for the file's extension.
 * The engine cannot proceed because it has no grammar to parse
 * against. Distinct from `TARGET_NOT_FOUND` so the caller's
 * diagnostic does not misattribute a registry / configuration
 * issue to a missing file.
 */
#define N00B_AUDIT_ERR_ENGINE_UNKNOWN_LANGUAGE (-9106)

/**
 * @brief Argv parsing failed in the CLI driver.
 *
 * Returned by `n00b_audit_run_cli` when libn00b's `slay/commander.h`
 * rejects the supplied argv (missing required positional, unknown
 * flag, malformed value, or any other shape error). A human-readable
 * diagnostic is written to stderr before this code is returned. Use
 * Phase 4's CLI usage line as the recovery hint.
 */
#define N00B_AUDIT_ERR_CLI_ARGS                (-9201)

/**
 * @brief A required argument to `n00b_audit_run_cli` was null or
 *        out-of-shape.
 *
 * Returned by `n00b_audit_run_cli` when the supplied `argv` pointer
 * is null or `argc <= 0`. Distinct from `CLI_ARGS` so the caller's
 * diagnostic does not misattribute a programming error to a
 * user-supplied argv shape problem. Mirrors the ENGINE_BAD_ARGS /
 * CLI_BAD_ARGS pattern.
 */
#define N00B_AUDIT_ERR_CLI_BAD_ARGS            (-9202)

/**
 * @brief Output renderer failed because a violation depended on a
 *        required-but-missing field of its rule.
 *
 * Returned by `n00b_audit_print_terminal` (and Phase 5's
 * `n00b_audit_print_json`) when a violation's `rule->good_example`
 * (or any other field the format contract requires) is null. This is
 * an internal-consistency failure — the loader's schema-version
 * gate should normally reject the guidance JSON before the renderer
 * ever sees a malformed rule — but the renderer still surfaces a
 * clean error code rather than dereferencing a null.
 */
#define N00B_AUDIT_ERR_CLI_RENDER              (-9203)

/**
 * @brief An exemption record carries no detached signature.
 *
 * Returned by `n00b_audit_exemption_verify` when the corresponding
 * `<exemption-file>.sig` file is absent. In normal audit operation
 * the exemption loader downgrades this to "drop the exemption with a
 * stderr warning" — unless `--allow-unsigned` is set, in which case
 * the exemption is kept and a warning is still emitted. The error
 * code itself is the primitive returned by the verify helper; the
 * loader applies the policy.
 *
 * WP-012.
 */
#define N00B_AUDIT_ERR_EXEMPTION_NO_SIGNATURE  (-9301)

/**
 * @brief An exemption record's detached signature exists, but
 *        `ssh-keygen -Y verify` rejected it.
 *
 * Returned by `n00b_audit_exemption_verify` when the `.sig` file is
 * present but verification fails — the exemption content was modified
 * after signing, the signature was forged, or the signing key was
 * different from any key in the trust roster but the loader couldn't
 * cleanly distinguish UNKNOWN_SIGNER from a regular bad-sig (see the
 * note on N00B_AUDIT_ERR_EXEMPTION_UNKNOWN_SIGNER).
 *
 * WP-012.
 */
#define N00B_AUDIT_ERR_EXEMPTION_BAD_SIGNATURE (-9302)

/**
 * @brief The signer named in the exemption record is not present in
 *        the trust roster (`audit/allowed_signers`).
 *
 * Returned by `n00b_audit_exemption_verify` when `ssh-keygen -Y
 * verify` reports the principal as unknown. The distinction between
 * BAD_SIGNATURE and UNKNOWN_SIGNER is best-effort: when ssh-keygen's
 * exit code or stderr does not let us cleanly separate the two cases,
 * verify collapses both into BAD_SIGNATURE and documents the
 * conservative choice — the caller's diagnostic remains correct
 * ("refused").
 *
 * WP-012.
 */
#define N00B_AUDIT_ERR_EXEMPTION_UNKNOWN_SIGNER (-9303)

/**
 * @brief Sign/verify subprocess could not be spawned, did not return,
 *        or failed for an OS-level reason unrelated to signature
 *        correctness.
 *
 * Returned by `n00b_audit_exemption_sign` and
 * `n00b_audit_exemption_verify` when `posix_spawn`, `waitpid`, or
 * `pipe`/`dup2` setup fails. Distinct from BAD_SIGNATURE so the
 * caller's diagnostic does not misattribute an OS failure (e.g.,
 * ssh-keygen not installed, no permission to spawn) to a security
 * verdict.
 *
 * WP-012.
 */
#define N00B_AUDIT_ERR_SIGN_SUBPROCESS          (-9304)

/**
 * @brief Return a human-readable description of a n00b-audit error
 *        code.
 *
 * Per n00b-api-guidelines § 5.5, every domain error code in n00b-audit
 * must round-trip through this accessor. Returned strings are rich
 * literals (`r"..."`) with process-lifetime storage; callers must not
 * free them. Unknown codes return a non-null `r"(unknown n00b-audit
 * error code)"` placeholder — this function never returns `nullptr`.
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block — n00b-audit's own surface does not expose
 * `.allocator` keyword arguments.
 *
 * @param code  Error code (typically obtained via
 *              `n00b_result_get_err` on a failed `n00b_result_t`
 *              returned by a n00b-audit API).
 *
 * @return Non-null `n00b_string_t *` describing the code.
 */
extern n00b_string_t *n00b_audit_err_str(int code);
