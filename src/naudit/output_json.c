/*
 * WP-001 Phase 5 — JSON-format output renderer.
 *
 * Implements `n00b_audit_print_json`, which emits the violation list
 * as a single JSON document conforming to the v1 schema pinned below.
 *
 * ================================================================
 * v1 output schema (pinned in WP-001 Phase 5; do NOT change without
 * a schema-version bump):
 *
 *   {
 *     "violations": [
 *       {
 *         "file":         <string>,
 *         "line":         <int64>,
 *         "column":       <int64>,
 *         "rule_id":      <string>,
 *         "message":      <string>,
 *         "doc_section":  <string>,
 *         "good_example": <string>
 *       },
 *       ...
 *     ],
 *     "summary": {
 *       "error_count": <int64>
 *     }
 *   }
 *
 * No `severity` field anywhere per D-005. `summary.error_count` is
 * the violation count (every violation is implicitly `error`).
 *
 * Field mapping (rule struct -> JSON):
 *   file         <- v->file
 *   line         <- v->line
 *   column       <- v->column
 *   rule_id      <- v->rule->id
 *   message      <- v->rule->title (terminal output uses the same
 *                   field for the human-readable header — keep
 *                   parity)
 *   doc_section  <- v->rule->section
 *   good_example <- v->rule->good_example
 *
 * Schema v1 pinned in WP-001 Phase 5; WP-002+ extensions are
 * ADDITIVE — new keys are fine (e.g., a future `project` /
 * `source_doc` envelope, or per-violation `bad_example`,
 * `rationale`, `guidance`), but existing-key removal or type
 * changes are a schema-version bump.
 * ================================================================
 *
 * Capture-mechanism note (Phase 4 W-4 settlement, unchanged for
 * Phase 5). The JSON renderer writes to libn00b's runtime stdout
 * topic via `n00b_print` (fd 1). The Phase 5 regression test
 * captures stdout via POSIX `pipe()` + `dup2()` in the test harness
 * — same mechanism as Phase 4's `test_audit_cli.c`, no production
 * code changes required.
 *
 * Per project DECISIONS.md D-005, this implementation's public
 * function carries no `_kargs` block — n00b-audit's own surface does
 * not expose `.allocator` keyword arguments. Per D-005 there is also
 * no `severity` field anywhere in the emitted JSON.
 *
 * Per D-008, null-pointer guards use the `!ptr` boolean-conversion
 * idiom.
 *
 * Internal note on libn00b interop: `n00b_json_encode` returns
 * `char *` (NUL-terminated, GC-allocated — libn00b's own ABI
 * surface). We wrap that via `n00b_string_from_cstr` before passing
 * to `n00b_print`, matching Phase 2's loader interop pattern between
 * the JSON parser's strings and the libn00b string surface.
 *
 * Integers pass to `n00b_json_int_new` BY VALUE (header:
 * `n00b_json_int_new(int64_t val)`). This is distinct from the
 * `n00b_cformat` footgun the Phase 4 sub-agent surfaced (where
 * integer substitutions also pass by value cast through `void *`,
 * NOT by pointer). For the JSON builder we cast `(int64_t)v->line`
 * etc. — passing `&v->line` would be a type error.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "conduit/print.h"
#include "parsers/json.h"

#include "naudit/output.h"
#include "naudit/violation.h"
#include "naudit/rule.h"
#include "naudit/guidance.h"
#include "naudit/errors.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Build one per-violation JSON object. Returns nullptr on a
 * required-field-missing problem; the caller surfaces that as
 * `N00B_AUDIT_ERR_CLI_RENDER`.
 *
 * Required fields, per the schema mapping above: v->file,
 * v->rule, v->rule->id, v->rule->title, v->rule->section,
 * v->rule->good_example. v->line and v->column are int64_t (not
 * pointers), so no null guard applies to them.
 */
static n00b_json_node_t *
build_violation_obj(n00b_audit_violation_t *v)
{
    if (!v || !v->file || !v->rule) {
        return nullptr;
    }
    n00b_audit_rule_t *rule = v->rule;
    if (!rule->id || !rule->title || !rule->section || !rule->good_example) {
        return nullptr;
    }

    n00b_json_node_t *obj = n00b_json_object_new();
    if (!obj) {
        return nullptr;
    }

    n00b_json_object_put(obj, "file",
                         n00b_json_string_new_from_n00b(v->file));
    n00b_json_object_put(obj, "line",
                         n00b_json_int_new((int64_t)v->line));
    n00b_json_object_put(obj, "column",
                         n00b_json_int_new((int64_t)v->column));
    n00b_json_object_put(obj, "rule_id",
                         n00b_json_string_new_from_n00b(rule->id));
    /* `message` mirrors the terminal renderer's header `title`
     * choice — same human-readable label, both formats. */
    n00b_json_object_put(obj, "message",
                         n00b_json_string_new_from_n00b(rule->title));
    n00b_json_object_put(obj, "doc_section",
                         n00b_json_string_new_from_n00b(rule->section));
    n00b_json_object_put(obj, "good_example",
                         n00b_json_string_new_from_n00b(rule->good_example));

    /*
     * WP-007 Phase 2: optional `rewrite` field. The key is only
     * emitted when the violation carries a non-empty rewrite
     * suggestion from slay's `n00b_production_rewrite_text` (rules
     * 1, 4, 7 of the canonical guidance populate this; rules
     * needing structural transforms leave it nullptr). v1 schema's
     * forward-compat contract treats new keys as additive.
     */
    if (v->rewrite && v->rewrite->u8_bytes > 0) {
        n00b_json_object_put(obj, "rewrite",
                             n00b_json_string_new_from_n00b(v->rewrite));
    }

    /*
     * WP-011: emit the rule's content hash and the violation's
     * region fingerprint so JSON consumers (CI surfaces, IDE
     * integrations) can craft exemption records keyed off the
     * same primitives the suppression engine matches against.
     * Both fields are additive per the v1 schema's forward-compat
     * contract.
     */
    if (rule->content_hash) {
        n00b_json_object_put(obj, "rule_content_hash",
                             n00b_json_string_new_from_n00b(
                                 rule->content_hash));
    }
    if (v->region_fingerprint) {
        n00b_json_object_put(obj, "region_fingerprint",
                             n00b_json_string_new_from_n00b(
                                 v->region_fingerprint));
    }

    return obj;
}

/* ---------------------------------------------------------------- */
/* Public entry                                                     */
/* ---------------------------------------------------------------- */

n00b_result_t(int)
n00b_audit_print_json(n00b_list_t(n00b_audit_violation_t *) *violations,
                      n00b_audit_guidance_t                  *guidance)
{
    /* `guidance` is accepted for API uniformity; v1 doesn't read any
     * field off it, but we still require non-null per the contract. */
    (void)guidance;

    if (!violations) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_RENDER);
    }

    n00b_json_node_t *root = n00b_json_object_new();
    n00b_json_node_t *arr  = n00b_json_array_new();
    if (!root || !arr) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_RENDER);
    }

    size_t n = n00b_list_len(*violations);
    for (size_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        n00b_json_node_t *vobj = build_violation_obj(v);
        if (!vobj) {
            return n00b_result_err(int, N00B_AUDIT_ERR_CLI_RENDER);
        }
        n00b_json_array_push(arr, vobj);
    }

    n00b_json_node_t *summary = n00b_json_object_new();
    if (!summary) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_RENDER);
    }
    n00b_json_object_put(summary, "error_count",
                         n00b_json_int_new((int64_t)n));

    n00b_json_object_put(root, "violations", arr);
    n00b_json_object_put(root, "summary",    summary);

    /* Encode + emit. The test parses the captured bytes directly, so
     * either pretty or compact output round-trips fine; we leave the
     * encode defaults (compact). */
    char *encoded = n00b_json_encode(root);
    if (!encoded) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_RENDER);
    }
    n00b_string_t *out_str = n00b_string_from_cstr(encoded);
    n00b_print(out_str);

    return n00b_result_ok(int, (int)n);
}
