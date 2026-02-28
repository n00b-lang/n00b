#pragma once

/**
 * @file annot_phases.h
 * @internal
 * @brief Internal types and phase-handler declarations for the annotation walk.
 *
 * Shared between `annot_walk.c` and the phase implementation files
 * (`annot_scope.c`, `annot_symtab.c`, `annot_cf.c`, `annot_types.c`).
 */

#include "slay/annot_walk.h"
#include "slay/annotation.h"
#include "slay/tree_util.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"
#include "text/strings/string_ops.h"

#include "typecheck/types.h"
#include "typecheck/construct.h"
#include "typecheck/context.h"
#include "typecheck/unify.h"

#include <stdio.h>

// ============================================================================
// Per-node context passed between phases
// ============================================================================

/** @brief Per-node state accumulated during the annotation walk. */
typedef struct {
    n00b_parse_tree_t   *node;
    n00b_nt_node_t      *pn;
    n00b_nonterm_t      *nt;
    n00b_parse_rule_t   *rule;
    n00b_annotation_t  **annots;      /**< Annotation pointer array (stack). */
    size_t               annot_count;
    n00b_sym_entry_t    *last_sym;    /**< Set by pass A, read by pass B. */
    bool                 opened_scope;
    n00b_string_t       *scope_ns;
} annot_node_ctx_t;

// ============================================================================
// Phase handlers
// ============================================================================

/**
 * @brief Phase 1: Open scopes (SCOPE_OPEN, ADT scope push).
 */
void annot_phase_scope_open(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc);

/**
 * @brief Phases 2-3: Two-pass symbol registration + type binding.
 *
 * Pass A runs symbol-creating annotations (DECLARES, TYPE_DECL, ADT, FIELD,
 * METHOD) to set `last_sym`. Pass B runs symbol-reading annotations (TYPE,
 * LITERAL) that depend on `last_sym`. Then binds explicit type annotations.
 */
void annot_phase_symtab(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc);

/**
 * @brief Phase 4: Control flow label creation (BRANCH, LOOP, SWITCH, etc.).
 */
void annot_phase_cf(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc);

/**
 * @brief Phase 6: Post-order type inference.
 *
 * Handles @infer evaluation, auto-propagation from sole NT children,
 * symbol↔node-type unification, and @assigns type unification.
 */
void annot_phase_types_post(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc);

/**
 * @brief Phase 7: Close scope if one was opened.
 */
void annot_phase_scope_close(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc);
