#pragma once

/**
 * @file audit/guidance.h
 * @brief Loader for audit rule files (`audit-rules.bnf` format v2).
 *
 * The guidance-file loader is n00b-audit's entry into the rule set:
 * read a v1 audit-rule file (slay-format `.bnf` text carrying
 * `@directive` metadata; WP-005) from disk, validate it against the
 * schema, walk (the empty-for-WP-001) `dependencies` list, and
 * return a populated `n00b_audit_guidance_t` via `n00b_result_t`.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively. It also pulls `audit/rule.h` so
 * consumers reach `n00b_audit_rule_t` transitively without needing to
 * remember a second include.
 *
 * Per project DECISIONS.md D-005, the loader's exported function
 * carries no `_kargs` block — n00b-audit's own surface does not expose
 * `.allocator` keyword arguments.
 *
 * Per project DECISIONS.md D-006, the header filename is unprefixed
 * (`audit/guidance.h`, not `audit/n00b_audit_guidance.h`); symbol-level
 * prefix discipline remains in force.
 */

#include <n00b.h>
#include "adt/list.h"
#include "adt/dict.h"
#include "adt/result.h"

#include "naudit/rule.h"

/**
 * @brief Parsed contents of an audit-rule file (v1 schema).
 *
 * Populated by `n00b_audit_load_guidance`. Every field is non-null on
 * success: `dependencies` and `rules` are always-non-null lists (they
 * may be empty), and the four `n00b_string_t *` fields always carry
 * the value read from the file (empty strings are valid, the schema
 * does not enforce non-emptiness).
 *
 * Field meanings:
 *  - `schema_version`  always `1` on success — other values yield
 *                      `N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION`.
 *  - `project`         the project name declared by the file.
 *  - `description`     short human-readable summary.
 *  - `source_doc`      path or reference to the document the rules
 *                      derive from (for example, the project's API
 *                      guidelines doc).
 *  - `dependencies`    paths to upstream audit-rule files whose rules
 *                      should compose with this one's. WP-001 supports
 *                      only the empty case; non-empty lists return
 *                      `N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED`
 *                      (DF-B; WP-002+ work).
 *  - `rules`           rules declared directly by this file (may be
 *                      empty).
 *  - `extension_overrides` (WP-009 Phase 1) optional project-level
 *                      overrides mapping a file extension string
 *                      (with leading `.`) to a registered language
 *                      name. Populated from the `@extensions`
 *                      top-level directive. May be `nullptr` when
 *                      the rule file declares no overrides; in
 *                      that case the engine uses each registered
 *                      language's default-extensions list from
 *                      `naudit/languages.h`. Validated at
 *                      guidance-load time — unknown language names
 *                      surface as `N00B_AUDIT_ERR_GUIDANCE_SCHEMA`
 *                      (DF-W resolution from the WP-009 spec).
 */
/**
 * @brief One top-level `@filter_def <name>` block (WP-009 Phase 4).
 *
 * Filter blocks declare reusable n00b predicates that rules opt into
 * via the per-rule `@filter <name>` annotation. The engine compiles
 * each referenced filter's `expr` text to a JIT'd predicate via
 * `n00b_naudit_filter_compile` on first audit invocation and invokes
 * it once per match.
 *
 * Field meanings:
 *  - `name`        filter name (declared after the `@filter_def`
 *                  directive on the same line).
 *  - `expr`        n00b source expression returning bool. References
 *                  the bound match handle as `arg` (e.g.,
 *                  `arg.capture("callee").starts_with("n00b_")`).
 *  - `description` optional human-readable prose. May be the empty
 *                  string when the rule file omits it.
 */
typedef struct {
    n00b_string_t *name;
    n00b_string_t *expr;
    n00b_string_t *description;
} n00b_audit_filter_t;

/*
 * Per WP-011, the guidance struct carries loaded exemption + baseline
 * lists. The element type lives in `naudit/exemption.h`; we store
 * the lists as `n00b_list_t(void *) *` here so this header stays
 * acyclic against the exemption surface, and the loader / engine
 * cast back to `n00b_audit_exemption_t *` on access. The cast is
 * sound because every element in the list is an exemption struct
 * pointer (the loader is the sole producer).
 */
typedef struct {
    int64_t                              schema_version;
    n00b_string_t                       *project;
    n00b_string_t                       *description;
    n00b_string_t                       *source_doc;
    n00b_list_t(n00b_string_t *)        *dependencies;
    n00b_list_t(n00b_audit_rule_t *)    *rules;
    n00b_dict_t(n00b_string_t *,
                n00b_string_t *)        *extension_overrides;
    n00b_dict_t(n00b_string_t *,
                n00b_audit_filter_t *)  *filters;
    /*
     * WP-011: loaded exemption records from `audit/exemptions/(*).bnf`
     * relative to the guidance file's directory. nullptr when the
     * directory is absent or empty; otherwise a non-empty list of
     * `n00b_audit_exemption_t *` (cast through `void *` to keep
     * this header acyclic with `naudit/exemption.h`).
     */
    n00b_list_t(void *)                 *exemptions;
    /*
     * WP-011: loaded baseline records from
     * `audit/baseline/baseline.bnf`. nullptr when absent. Same
     * `void *` shape as `exemptions` for the same reason. The
     * `--ignore-baseline` CLI flag skips this list at suppression
     * time.
     */
    n00b_list_t(void *)                 *baseline;
    /*
     * WP-011: directory of the loaded guidance file (the project
     * root for exemption + baseline discovery). nullptr until the
     * loader sets it.
     */
    n00b_string_t                       *project_root;
} n00b_audit_guidance_t;

/**
 * @brief Load and parse an audit-rule file (WP-005 `.bnf` format).
 *
 * Reads the file at `path` via libn00b's `core/file.h` MMAP substrate
 * (one-shot configuration read), tokenizes via the registered
 * `"audit_rule_file"` callback, parses against the embedded
 * `audit-rule-file.bnf` metagrammar via slay's `n00b_parse`, walks
 * the resulting parse tree to populate the v1 guidance struct, and
 * returns it.
 *
 * Validation order:
 *   1. file open    — fail with `N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND`.
 *   2. tokenize + parse against metagrammar — fail with
 *      `N00B_AUDIT_ERR_GUIDANCE_PARSE`.
 *   3. schema_version directive present + integer + value == 1 —
 *      fail with `N00B_AUDIT_ERR_GUIDANCE_SCHEMA` for missing /
 *      wrong-type, with `N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION`
 *      for wrong value. Version is checked *before* the rest of
 *      the directive walk so wrong-version files reject fast.
 *   4. required top-level directives, then required per-rule
 *      directives — fail with `N00B_AUDIT_ERR_GUIDANCE_SCHEMA`.
 *   5. non-empty `dependencies` — fail with
 *      `N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED` (DF-B).
 *
 * Unknown top-level or per-rule directives are tolerated silently
 * (forward-compat). The hard schema-version check at step 3 is the
 * only forward-compat gate.
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block.
 *
 * @param path  Filesystem path to the audit-rule file.
 *
 * @return On success, `n00b_result_ok` wrapping a populated
 *         `n00b_audit_guidance_t *`. On failure, `n00b_result_err`
 *         carrying one of:
 *         `N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND`,
 *         `N00B_AUDIT_ERR_GUIDANCE_PARSE`,
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA`,
 *         `N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION`, or
 *         `N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED`.
 *         All five round-trip through `n00b_audit_err_str`.
 */
extern n00b_result_t(n00b_audit_guidance_t *)
n00b_audit_load_guidance(n00b_string_t *path);

/**
 * @brief Discover the nearest `audit-rules.bnf` file.
 *
 * Walks parents from `start_dir` looking for a file at
 * `<dir>/audit-rules.bnf` (git-style discovery walk).
 * Returns the discovered absolute path or
 * `N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND` if the walk reaches the
 * filesystem root with no hit.
 *
 * Termination contract: the walk stops when `n00b_path_canonical`'s
 * parent computation produces the same path as the current iteration
 * (the filesystem root condition under libn00b's path primitives),
 * guaranteeing no infinite loop on platforms where `parent('/') == '/'`
 * (D-001 platform matrix).
 *
 * Per project DECISIONS.md D-005, this function carries no `_kargs`
 * block — n00b-audit's own surface does not expose `.allocator`
 * keyword arguments.
 *
 * @param start_dir  Directory to begin the walk from (typically the
 *                   process's current working directory). Must be
 *                   non-null.
 *
 * @return On success, `n00b_result_ok` wrapping a non-null
 *         `n00b_string_t *` carrying the absolute path of the
 *         discovered guidance file. On failure, `n00b_result_err`
 *         carrying `N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND` (no guidance
 *         file found anywhere up to the filesystem root).
 */
extern n00b_result_t(n00b_string_t *)
n00b_audit_find_guidance_file(n00b_string_t *start_dir);
