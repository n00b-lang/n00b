/*
 * n00b-audit module entry.
 *
 * WP-001 Phase 1 stub returned `true`; WP-005 adds the custom-
 * tokenizer registration step (registers the `"audit_rule_file"`
 * scan callback with libn00b's tokenizer registry so the guidance
 * loader can use it when parsing `.bnf` rule files).
 *
 * Per D-005, the exported function takes no `_kargs` block — the
 * project-wide allocator-policy carveout means n00b-audit's own
 * surface is free of `.allocator` keyword arguments.
 */

#include "naudit/naudit.h"
#include "internal/naudit/_naudit_internal.h"

bool
n00b_audit_module_init(void)
{
    n00b_audit_register_rule_file_tokenizer();
    return true;
}
