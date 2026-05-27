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
 */
typedef struct {
    n00b_string_t                  *id;
    n00b_string_t                  *title;
    n00b_string_t                  *section;
    n00b_string_t                  *bnf_fragment;
    n00b_string_t                  *violation_nt;
    n00b_string_t                  *rationale;
    n00b_string_t                  *bad_example;
    n00b_string_t                  *good_example;
    n00b_string_t                  *guidance;
    n00b_list_t(n00b_string_t *)   *applies_to_include;
    n00b_list_t(n00b_string_t *)   *applies_to_exclude;
} n00b_audit_rule_t;
