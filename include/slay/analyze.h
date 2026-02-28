#pragma once

/**
 * @file analyze.h
 * @brief Static analysis module: detects common programming errors
 *        using CFG, DFG, and symtab information.
 *
 * Analyses:
 * - **W001** Dead code (unreachable CFG blocks)
 * - **W002** Use before definition (DFG reaching-defs empty)
 * - **W003** Unused variable (DFG def with no reached uses)
 * - **E001** Undefined variable (use with no symtab entry)
 * - **W004** Unreachable after jump (statements after break/continue/return)
 * - **W005** Shadowed variable (name shadows outer-scope decl)
 */

#include "slay/cfg.h"
#include "slay/cdg.h"
#include "slay/dfg.h"
#include "slay/diagnostic.h"
#include "slay/annot_walk.h"

// ============================================================================
// Analysis context
// ============================================================================

typedef struct {
    n00b_cfg_t           *cfg;
    n00b_cdg_t           *cdg;        /**< May be NULL. */
    n00b_dfg_t           *dfg;
    n00b_symtab_t        *symtab;
    n00b_cf_labels_t     *cf_labels;
    n00b_annot_result_t  *annot;
    n00b_grammar_t       *grammar;
    n00b_diag_ctx_t      *diag;       /**< Output: diagnostics pushed here. */
    n00b_string_t        *func_name;
} n00b_analyze_ctx_t;

// ============================================================================
// Run all analyses
// ============================================================================

void n00b_analyze_all(n00b_analyze_ctx_t *ctx);

// ============================================================================
// Individual analyses
// ============================================================================

void n00b_analyze_dead_code(n00b_analyze_ctx_t *ctx);             /**< W001 */
void n00b_analyze_use_before_def(n00b_analyze_ctx_t *ctx);        /**< W002 */
void n00b_analyze_unused_vars(n00b_analyze_ctx_t *ctx);           /**< W003 */
void n00b_analyze_undefined_vars(n00b_analyze_ctx_t *ctx);        /**< E001 */
void n00b_analyze_unreachable_after_jump(n00b_analyze_ctx_t *ctx);/**< W004 */
void n00b_analyze_shadowed_vars(n00b_analyze_ctx_t *ctx);         /**< W005 */
