#pragma once

/**
 * @file audit/engine.h
 * @brief Grammar-composing audit engine.
 *
 * The engine loads the C-ncc base grammar (`c_ncc.bnf` from the
 * vendored ncc subproject, path resolved via the
 * `N00B_AUDIT_GRAMMAR_PATH` macro emitted at configure time by the
 * top-level meson build), merges every rule's `bnf_fragment` into
 * that grammar, parses target source files with slay, and emits one
 * `n00b_audit_violation_t` per match of any rule's
 * `violation_nt` nonterminal.
 *
 * The engine struct is **opaque** to consumers — only the typedef is
 * exposed here; the full definition lives in `src/audit/engine.c`.
 * This lets the engine's internals evolve (grammar caching, rule-
 * name → nt-handle map, etc.) without breaking ABI.
 *
 * Per project DECISIONS.md D-005, the public functions below carry
 * no `_kargs` block — n00b-audit's own exported surface does not
 * expose `.allocator` keyword arguments.
 *
 * Per project DECISIONS.md D-006, this header's filename is
 * unprefixed (`audit/engine.h`, not `audit/n00b_audit_engine.h`);
 * symbol-level prefix discipline remains in force.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively, and `adt/list.h` + `adt/result.h`
 * explicitly because `<n00b.h>` does NOT pull those in transitively
 * (Phase 2 surfaced this footgun — see STATUS.md).
 */

#include <n00b.h>
#include "adt/list.h"
#include "adt/result.h"

#include "naudit/guidance.h"
#include "naudit/violation.h"

/**
 * @brief Opaque engine handle.
 *
 * The struct's full layout (merged grammar pointer, guidance back-
 * pointer, any precomputed rule indices) lives in `src/audit/engine.c`
 * and is not part of the public ABI.
 */
typedef struct n00b_audit_engine n00b_audit_engine_t;

/**
 * @brief Construct an engine from a loaded guidance struct.
 *
 * Loads the base C-ncc grammar from `N00B_AUDIT_GRAMMAR_PATH`
 * (emitted into `audit_paths.h` at configure time by the top-level
 * meson build — no path string literals appear in C source per the
 * NCC.md "Build system" rule), merges every rule's `bnf_fragment`
 * into that grammar, and returns the constructed engine.
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param guidance  Loaded guidance struct (typically from
 *                  `n00b_audit_load_guidance`). The engine borrows
 *                  the guidance pointer; the caller must keep it
 *                  alive for the engine's lifetime.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null
 *         `n00b_audit_engine_t *`. On failure, `n00b_result_err`
 *         carrying one of:
 *         `N00B_AUDIT_ERR_ENGINE_BAD_ARGS` (null `guidance` or its
 *         `rules` list),
 *         `N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD` (base grammar failed
 *         to load — e.g., `c_ncc.bnf` unreachable or syntactically
 *         invalid),
 *         `N00B_AUDIT_ERR_ENGINE_RULE_MERGE` (one of the rule
 *         fragments failed to compose with the base grammar — e.g.,
 *         a malformed fragment or a conflicting production), or
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA` (a rule's `@violation_nt`
 *         does not resolve to any nonterminal in the merged grammar —
 *         the guidance file is structurally valid but its
 *         `@violation_nt` declarations are inconsistent with the
 *         supplied BNF fragments).
 *         All round-trip through `n00b_audit_err_str`.
 */
extern n00b_result_t(n00b_audit_engine_t *)
n00b_audit_engine_new(n00b_audit_guidance_t *guidance);

/**
 * @brief Run the engine against one target source file.
 *
 * Opens `path`, parses its contents against the merged grammar,
 * walks the resulting parse tree looking for each rule's
 * `violation_nt` nonterminal, and emits one
 * `n00b_audit_violation_t *` per match into a returned list.
 *
 * Line and column on each emitted violation come from slay's parse-
 * tree source-location surface (the leftmost terminal leaf's
 * `n00b_token_info_t.line` and `.column` fields) — never invented
 * numbers (per the cross-project numeric-claims rule).
 *
 * Per project DECISIONS.md D-005, this function carries no
 * `_kargs` block.
 *
 * @param engine  Engine built via `n00b_audit_engine_new`.
 * @param path    Filesystem path to the target source file.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null
 *         `n00b_list_t(n00b_audit_violation_t *) *` (the list may be
 *         empty — meaning "no violations"). On failure,
 *         `n00b_result_err` carrying one of:
 *         `N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND` (path can't be
 *         opened — ENOENT, EACCES, etc.), or
 *         `N00B_AUDIT_ERR_ENGINE_PARSE` (the file's contents do not
 *         parse against the merged grammar). Both round-trip through
 *         `n00b_audit_err_str`.
 */
extern n00b_result_t(n00b_list_t(n00b_audit_violation_t *) *)
n00b_audit_engine_check_file(n00b_audit_engine_t *engine,
                             n00b_string_t       *path);

/**
 * @brief WP-011 — toggle baseline suppression on the engine.
 *
 * When @p ignore is true, `n00b_audit_engine_check_file` skips the
 * `guidance->baseline` suppression check (per-record exemptions in
 * `guidance->exemptions` still apply). The default is false; the
 * `--ignore-baseline` CLI flag drives this setter.
 *
 * @param engine  Engine to configure.
 * @param ignore  True to bypass the baseline; false (default) to
 *                honor it.
 */
extern void
n00b_audit_engine_set_ignore_baseline(n00b_audit_engine_t *engine,
                                       bool                 ignore);

/**
 * @brief WP-012 — toggle the unsigned-exemption acceptance policy.
 *
 * When @p allow is true, the engine instructs the exemption / baseline
 * verification gate to warn-and-accept records whose detached signature
 * is missing, invalid, or whose signer is not present in
 * `audit/allowed_signers`. The default (false) drops those records
 * from the suppression set, causing the underlying findings to fire.
 *
 * Mirrors the `--allow-unsigned` CLI flag.
 *
 * @param engine  Engine to configure.
 * @param allow   True to bypass signature verification; false
 *                (default) to enforce.
 */
extern void
n00b_audit_engine_set_allow_unsigned(n00b_audit_engine_t *engine,
                                      bool                 allow);

/**
 * @brief WP-015 — assert the running environment is a protected
 *        CI / pre-commit context (not an agent-writable working
 *        tree).
 *
 * When @p protected_ is true, two diagnostics downgrade from
 * prominent to informational:
 *
 *   1. The REPO-source-roster warning (the trust roster lives at
 *      `<project_root>/audit/allowed_signers` — only safe if commit
 *      signing is enforced via CI per white paper § 9.2).
 *   2. The unsigned-rule-file warning (`audit-rules.bnf.sig`
 *      absent — only safe if rule-file changes are themselves
 *      gated by CI / pre-commit per § 6.3).
 *
 * Mirrors the `--repo-protected` CLI flag. Default false (the
 * conservative agent-writable assumption).
 *
 * @param engine      Engine to configure.
 * @param protected_  True to downgrade warnings; false (default)
 *                    to emit prominent warnings.
 */
extern void
n00b_audit_engine_set_repo_protected(n00b_audit_engine_t *engine,
                                      bool                 protected_);

/**
 * @brief WP-017: set the extra args passed to `cc -E` for the
 * preprocessor pre-pass. Used by languages whose registry entry
 * sets `preprocess = true` (currently: C).
 *
 * @param engine     Engine handle (must be non-null).
 * @param cpp_args   Whitespace-separated arguments to forward to
 *                   the preprocessor (e.g. `r"-I /path -DFOO"`).
 *                   nullptr or empty means: no extra args.
 */
extern void
n00b_audit_engine_set_cpp_args(n00b_audit_engine_t *engine,
                                n00b_string_t       *cpp_args);
