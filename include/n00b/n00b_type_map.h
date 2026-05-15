#pragma once

/**
 * @file n00b_type_map.h
 * @brief Maps n00b type-checker types to MIR codegen type tags.
 *
 * Provides the `n00b_type_map` callback for `n00b_cg_session_new(.type_map)`.
 * Translates `n00b_tc_type_t *` (union-find-based inference types) into
 * `n00b_cg_type_tag_t` (machine-level MIR type tags).
 */

#include "slay/codegen.h"
#include "typecheck/types.h"

/**
 * @brief Translate an n00b type-checker type to a MIR type tag.
 *
 * Chases the union-find chain, then dispatches on type kind:
 * - **Var** (unresolved): `N00B_CG_I64` (fallback; deferred: `n00b_any_t`/PTR)
 * - **Primitive**: maps by name (`int`→I64, `bool`→BOOL, `string`→PTR, etc.)
 * - **Param**: maps `list`, `dict`, `set`, `result`, and `option` by name
 * - **Fn/Record/Tuple/Sum**: `N00B_CG_PTR` (heap-allocated)
 *
 * @param session  Codegen session (unused currently; available for future use).
 * @param type     Type-checker type to translate.
 * @return         The corresponding MIR type tag.
 */
n00b_cg_type_tag_t n00b_type_map(n00b_cg_session_t *session, n00b_tc_type_t *type);
