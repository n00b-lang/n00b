#pragma once

/**
 * @file audit/rule.h
 * @brief Public surface for a single n00b-audit guidance rule.
 *
 * A `n00b_audit_rule_t` describes one enforceable rule loaded from a
 * `.agents/.audit-guidance.json` file. Each rule pairs a BNF fragment
 * (consumed by the Phase 3 engine to extend the C-ncc base grammar)
 * with human-facing metadata: id, title, doc section reference,
 * rationale, before/after examples, and remediation guidance. The
 * fragment is composed with the c_ncc.bnf base grammar; the
 * `violation_nt` field names the nonterminal whose appearance in the
 * parse forest signals a violation.
 *
 * Per project DECISIONS.md D-005, **there is no `severity` field**.
 * v1 treats every matched violation as `error`; any violation produces
 * a non-zero CLI exit code. A severity model may be re-introduced in a
 * future DECISIONS entry + schema-version bump.
 *
 * Per project DECISIONS.md D-006, the header filename is unprefixed
 * (`audit/rule.h`, not `audit/n00b_audit_rule.h`); symbol-level prefix
 * discipline remains in force.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively for the `n00b_string_t` /
 * `n00b_list_t` declarations.
 */

#include <n00b.h>
#include "adt/list.h"

/**
 * @brief One n00b-audit guidance rule.
 *
 * Loaded by `n00b_audit_load_guidance` from the `rules` array of a
 * `.agents/.audit-guidance.json` file. All string fields are required
 * by the v1 schema; the `applies_to_*` lists are populated from the
 * optional per-rule `applies_to.{include,exclude}` arrays (nullable —
 * `nullptr` when the JSON omits the corresponding sub-key).
 *
 * Field meanings:
 *  - `id`            stable rule identifier (e.g. `r"n00b.s2_1.null"`).
 *  - `title`         short human-readable label.
 *  - `section`       reference to the source doc section
 *                    (e.g. `r"§ 2.1"`).
 *  - `bnf_fragment`  BNF productions appended to the base grammar.
 *  - `violation_nt`  nonterminal whose match signals a violation.
 *  - `rationale`     why the rule exists.
 *  - `bad_example`   non-conforming snippet.
 *  - `good_example`  conforming snippet.
 *  - `guidance`      remediation guidance.
 *  - `applies_to_include` file globs the rule is scoped to
 *                    (nullable; nullptr means "all files").
 *  - `applies_to_exclude` file globs the rule skips
 *                    (nullable; nullptr means "no exclusions").
 *  - `language`      (WP-009 Phase 1) list of language names the
 *                    rule is scoped to. Populated from one or more
 *                    `@language <name>` per-rule annotations.
 *                    Empty / null = applies to every language
 *                    whose grammar defines the rule's
 *                    `violation_nt` (the engine still skips rules
 *                    whose `violation_nt` is missing from the
 *                    loaded grammar).
 *  - `filter_name`   (WP-009 Phase 4) optional name referencing a
 *                    top-level `@filter_def <name>` block. When
 *                    set, the engine compiles the filter's n00b
 *                    expression to a JIT'd predicate and invokes
 *                    it once per match; only matches where the
 *                    predicate returns true become violations.
 *                    `nullptr` means "no filter — every match is
 *                    a violation."
 *  - `captures`      (WP-009 Phase 4) list of capture declarations.
 *                    Each entry pairs a capture name (without the
 *                    leading `$`) with an NT name. The engine binds
 *                    each declaration to a descendant of the match
 *                    in document order at violation-emission time
 *                    so the filter expression (and future rewrite
 *                    expansion) can reference the captured nodes
 *                    via `arg.capture("name")`.
 */

/**
 * @brief One `@captures $name:<nt>` declaration on a rule.
 *
 * Parsed from the rule's `@captures` directive(s). `name` is the
 * capture identifier (without the leading `$`); `nt` is the NT name
 * (without angle brackets). The engine binds captures by walking the
 * matched node's descendants in document order (pre-order DFS); the
 * Nth declaration with NT X binds to the Nth descendant matching X.
 */
typedef struct {
    n00b_string_t *name;
    n00b_string_t *nt;
} n00b_audit_capture_decl_t;

/*
 * `content_hash` (WP-011, D-X3) — XXH3-128 hex digest of the rule's
 * canonical BNF text. Populated by `n00b_audit_load_guidance` at load
 * time via `n00b_audit_compute_rule_content_hash`; surfaces in
 * violation output alongside the human-readable `id`. Exemption
 * records key off this field (see `naudit/exemption.h`).
 */
typedef struct {
    n00b_string_t                                  *id;
    n00b_string_t                                  *title;
    n00b_string_t                                  *section;
    n00b_string_t                                  *bnf_fragment;
    n00b_string_t                                  *violation_nt;
    n00b_string_t                                  *rationale;
    n00b_string_t                                  *bad_example;
    n00b_string_t                                  *good_example;
    n00b_string_t                                  *guidance;
    n00b_list_t(n00b_string_t *)                   *applies_to_include;
    n00b_list_t(n00b_string_t *)                   *applies_to_exclude;
    n00b_list_t(n00b_string_t *)                   *language;
    n00b_string_t                                  *filter_name;
    n00b_list_t(n00b_audit_capture_decl_t *)       *captures;
    n00b_string_t                                  *content_hash;
} n00b_audit_rule_t;
