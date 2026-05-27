#pragma once

/**
 * @file audit/violation.h
 * @brief One audit-violation event the engine emits per match.
 *
 * A `n00b_audit_violation_t` is the per-match record produced by
 * `n00b_audit_engine_check_file`. The engine walks the parse forest
 * for each rule's `violation_nt` nonterminal; every match yields one
 * violation carrying the source location (file, line, column) plus a
 * back-pointer to the matched `n00b_audit_rule_t`. The output layer
 * (Phase 4 terminal / Phase 5 JSON) renders these records.
 *
 * Per project DECISIONS.md D-005, there is **no `severity` field** —
 * v1 treats every match as `error` and any violation produces a
 * non-zero CLI exit code.
 *
 * Per project DECISIONS.md D-006, this header's filename is
 * unprefixed (`audit/violation.h`, not `audit/n00b_audit_violation.h`);
 * symbol-level prefix discipline remains in force.
 *
 * Headers under `include/audit/` may be #included standalone, so this
 * file pulls `<n00b.h>` defensively for the `n00b_string_t` declaration
 * and includes `audit/rule.h` so consumers reach `n00b_audit_rule_t`
 * transitively without remembering a second include.
 */

#include <n00b.h>

#include "naudit/rule.h"

/**
 * @brief One audit-violation event.
 *
 * Produced by `n00b_audit_engine_check_file`. Line and column are
 * 1-based and come from slay's `n00b_token_info_t.line` /
 * `.column` fields on the parse-tree's leftmost terminal leaf — never
 * invented numbers (per the cross-project numeric-claims rule).
 *
 * Field meanings:
 *  - `file`    path to the source file the violation was found in
 *              (the same `path` `check_file` was called with).
 *  - `line`    1-based source line of the matched construct's first
 *              token.
 *  - `column`  1-based source column of the matched construct's first
 *              token.
 *  - `rule`    back-pointer to the matched rule (one of the
 *              `n00b_audit_rule_t *` entries in the guidance's
 *              `rules` list).
 */
typedef struct {
    n00b_string_t     *file;
    int64_t            line;
    int64_t            column;
    n00b_audit_rule_t *rule;
} n00b_audit_violation_t;
