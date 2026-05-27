#pragma once

/**
 * @file private/audit/_audit_internal.h
 * @brief n00b-audit internal helpers shared across `src/audit/`.
 *
 * Placeholder for WP-001 Phase 1. Internal-only declarations land
 * here as Phase 2+ subsystems grow shared helpers. Per
 * n00b-api-guidelines § 3.14, internal symbols use the leading-
 * underscore `_n00b_audit_` prefix to mark them as not-callable
 * from outside the project.
 */

#include "n00b.h"
#include "parsers/scanner.h"

/**
 * @brief WP-005 — register the `"audit_rule_file"` custom tokenizer
 *        with libn00b's tokenizer registry.
 *
 * Called once from `n00b_audit_module_init`. Re-entrant: the
 * underlying `n00b_tokenizer_register` is idempotent.
 */
extern void n00b_audit_register_rule_file_tokenizer(void);

/**
 * @brief WP-005 — allocate a fresh scanner-state struct for the
 *        audit-rule-file tokenizer.
 *
 * The returned pointer is opaque to the caller; pass it as the
 * `.state` kwarg to `n00b_scanner_new` and accompany it with the
 * reset callback from `_n00b_audit_rule_file_reset_cb`.
 */
extern void *_n00b_audit_rule_file_scanner_state_new(void);

/**
 * @brief WP-005 — accessor for the audit-rule-file scan callback.
 */
extern n00b_scan_cb_t _n00b_audit_rule_file_scan_cb(void);

/**
 * @brief WP-005 — accessor for the audit-rule-file scanner-reset
 *        callback (forwarded to `n00b_scanner_new`'s `.reset_cb`
 *        kwarg).
 */
extern n00b_scan_reset_cb_t _n00b_audit_rule_file_reset_cb(void);

