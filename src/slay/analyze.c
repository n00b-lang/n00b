// analyze.c — Static analysis module.
//
// Uses CFG reachability, DFG reaching-definitions, and symtab
// information to detect common programming errors.

#include "slay/analyze.h"
#include "core/alloc.h"
#include "strings/string_ops.h"

#include <stdio.h>
#include <string.h>

// ============================================================================
// W001: Dead code (unreachable CFG blocks)
// ============================================================================

void
n00b_analyze_dead_code(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->cfg || !ctx->diag) {
        return;
    }

    n00b_cfg_t *cfg    = ctx->cfg;
    int32_t     nblocks = n00b_cfg_block_count(cfg);

    if (nblocks <= 0) {
        return;
    }

    // BFS from entry to mark reachable blocks.
    bool *reachable = n00b_alloc_array(bool, nblocks);

    // Simple BFS queue using the block IDs.
    int32_t *queue = n00b_alloc_array(int32_t, nblocks);
    int32_t  head  = 0;
    int32_t  tail  = 0;

    reachable[cfg->entry_id] = true;
    queue[tail++]            = cfg->entry_id;

    while (head < tail) {
        int32_t cur = queue[head++];

        n00b_list_t(n00b_cfg_edge_t) succs = n00b_cfg_successors(cfg, cur);
        size_t ns = n00b_list_len(succs);

        for (size_t i = 0; i < ns; i++) {
            n00b_cfg_edge_t e = n00b_list_get(succs, i);

            if (!reachable[e.to_id]) {
                reachable[e.to_id] = true;
                queue[tail++]      = e.to_id;
            }
        }

        n00b_list_free(succs);
    }

    // Report unreachable blocks (skip entry and exit).
    for (int32_t i = 0; i < nblocks; i++) {
        if (reachable[i]) {
            continue;
        }

        n00b_cfg_block_t *blk = n00b_cfg_block(cfg, i);

        if (!blk || blk->is_entry || blk->is_exit) {
            continue;
        }

        // Get span from first statement in the block.
        n00b_diag_span_t span = {0};

        if (n00b_list_len(blk->stmts) > 0) {
            n00b_parse_tree_t *first = n00b_list_get(blk->stmts, 0);
            span = n00b_diag_span_from_node(first);
        }

        n00b_diag_push(ctx->diag,
                      N00B_DIAG_WARNING,
                      N00B_STAGE_ANALYSIS,
                      *r"W001",
                      *r"unreachable code",
                      span);
    }

    n00b_free(reachable);
    n00b_free(queue);
}

// ============================================================================
// W002: Use before definition
// ============================================================================

void
n00b_analyze_use_before_def(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->dfg || !ctx->diag) {
        return;
    }

    n00b_dfg_t *dfg    = ctx->dfg;
    int32_t     nfacts = n00b_dfg_fact_count(dfg);

    for (int32_t i = 0; i < nfacts; i++) {
        n00b_du_fact_t fact = n00b_list_get(dfg->facts, i);

        if (fact.is_def) {
            continue;
        }

        // This is a USE. Check if any definition reaches it.
        n00b_list_t(n00b_dd_edge_t) reaching = n00b_dfg_reaching_defs(dfg, fact.id);
        size_t nreach = n00b_list_len(reaching);
        n00b_list_free(reaching);

        if (nreach > 0) {
            continue;  // At least one def reaches this use.
        }

        // No defs reach this use. Check if it's even declared.
        if (ctx->symtab) {
            n00b_sym_entry_t *sym = n00b_symtab_lookup_all(
                ctx->symtab, n00b_string_empty(), fact.var_name);

            if (!sym) {
                continue;  // Undefined — handled by E001.
            }

            if (sym->kind == N00B_SYM_PARAM
                || sym->kind == N00B_SYM_FUNCTION) {
                continue;  // Parameters and functions are always defined.
            }
        }

        n00b_diag_span_t span = n00b_diag_span_from_node(fact.node);

        char msg_buf[256];
        snprintf(msg_buf, sizeof(msg_buf),
                 "variable '%.*s' used before definition",
                 (int)fact.var_name.u8_bytes, fact.var_name.data);

        n00b_diag_push(ctx->diag,
                      N00B_DIAG_WARNING,
                      N00B_STAGE_ANALYSIS,
                      *r"W002",
                      n00b_string_from_cstr(msg_buf),
                      span);
    }
}

// ============================================================================
// W003: Unused variables
// ============================================================================

void
n00b_analyze_unused_vars(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->dfg || !ctx->diag) {
        return;
    }

    n00b_dfg_t *dfg    = ctx->dfg;
    int32_t     nfacts = n00b_dfg_fact_count(dfg);

    for (int32_t i = 0; i < nfacts; i++) {
        n00b_du_fact_t fact = n00b_list_get(dfg->facts, i);

        if (!fact.is_def) {
            continue;
        }

        // Skip synthetic entry-block defs (parameters).
        if (fact.stmt_ix < 0) {
            continue;
        }

        // Check if this def reaches any use.
        n00b_list_t(n00b_dd_edge_t) uses = n00b_dfg_reached_uses(dfg, fact.id);
        size_t nuses = n00b_list_len(uses);
        n00b_list_free(uses);

        if (nuses > 0) {
            continue;  // This def is used somewhere.
        }

        // Skip if the variable is a parameter (convention).
        if (ctx->symtab) {
            n00b_sym_entry_t *sym = n00b_symtab_lookup_all(
                ctx->symtab, n00b_string_empty(), fact.var_name);

            if (sym && (sym->kind == N00B_SYM_PARAM
                        || sym->kind == N00B_SYM_FUNCTION)) {
                continue;
            }
        }

        n00b_diag_span_t span = n00b_diag_span_from_node(fact.node);

        char msg_buf[256];
        snprintf(msg_buf, sizeof(msg_buf),
                 "variable '%.*s' is assigned but never used",
                 (int)fact.var_name.u8_bytes, fact.var_name.data);

        n00b_diag_push(ctx->diag,
                      N00B_DIAG_WARNING,
                      N00B_STAGE_ANALYSIS,
                      *r"W003",
                      n00b_string_from_cstr(msg_buf),
                      span);
    }
}

// ============================================================================
// E001: Undefined variables
// ============================================================================

void
n00b_analyze_undefined_vars(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->dfg || !ctx->symtab || !ctx->diag) {
        return;
    }

    n00b_dfg_t *dfg    = ctx->dfg;
    int32_t     nfacts = n00b_dfg_fact_count(dfg);

    for (int32_t i = 0; i < nfacts; i++) {
        n00b_du_fact_t fact = n00b_list_get(dfg->facts, i);

        if (fact.is_def) {
            continue;
        }

        // Look up the variable in the symtab.
        n00b_sym_entry_t *sym = n00b_symtab_lookup_all(
            ctx->symtab, n00b_string_empty(), fact.var_name);

        if (sym) {
            continue;  // Declared — not undefined.
        }

        n00b_diag_span_t span = n00b_diag_span_from_node(fact.node);

        char msg_buf[256];
        snprintf(msg_buf, sizeof(msg_buf),
                 "use of undeclared identifier '%.*s'",
                 (int)fact.var_name.u8_bytes, fact.var_name.data);

        n00b_diag_push(ctx->diag,
                      N00B_DIAG_ERROR,
                      N00B_STAGE_ANALYSIS,
                      *r"E001",
                      n00b_string_from_cstr(msg_buf),
                      span);
    }
}

// ============================================================================
// W004: Unreachable after jump
// ============================================================================

void
n00b_analyze_unreachable_after_jump(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->cfg || !ctx->cf_labels || !ctx->diag) {
        return;
    }

    n00b_cfg_t *cfg    = ctx->cfg;
    int32_t     nblocks = n00b_cfg_block_count(cfg);

    for (int32_t b = 0; b < nblocks; b++) {
        n00b_cfg_block_t *blk = n00b_cfg_block(cfg, b);

        if (!blk) {
            continue;
        }

        size_t nstmts = n00b_list_len(blk->stmts);

        for (size_t s = 0; s < nstmts; s++) {
            n00b_parse_tree_t *stmt = n00b_list_get(blk->stmts, s);
            n00b_cf_label_t   *lbl  = n00b_cf_label_lookup(ctx->cf_labels, stmt);

            if (lbl && lbl->kind == N00B_CF_JUMP && s + 1 < nstmts) {
                // There are statements after this jump in the same block.
                n00b_parse_tree_t *next_stmt = n00b_list_get(blk->stmts, s + 1);
                n00b_diag_span_t   span      = n00b_diag_span_from_node(next_stmt);

                n00b_diag_push(ctx->diag,
                              N00B_DIAG_WARNING,
                              N00B_STAGE_ANALYSIS,
                              *r"W004",
                              *r"code after return/break/continue is unreachable",
                              span);

                break;  // Only report once per block.
            }
        }
    }
}

// ============================================================================
// W005: Shadowed variables
// ============================================================================

void
n00b_analyze_shadowed_vars(n00b_analyze_ctx_t *ctx)
{
    if (!ctx || !ctx->annot || !ctx->diag) {
        return;
    }

    n00b_list_t(n00b_sym_entry_t *) *entries = ctx->annot->shadowed_entries;

    if (!entries) {
        return;
    }

    size_t count = n00b_list_len(*entries);

    for (size_t i = 0; i < count; i++) {
        n00b_sym_entry_t *entry = n00b_list_get(*entries, i);

        if (!entry || !entry->shadowed) {
            continue;
        }

        n00b_diag_span_t span    = n00b_diag_span_from_node(entry->decl_node);
        n00b_diag_span_t related = n00b_diag_span_from_node(entry->shadowed->decl_node);

        char msg_buf[256];
        snprintf(msg_buf, sizeof(msg_buf),
                 "declaration of '%.*s' shadows previous declaration",
                 (int)entry->name.u8_bytes, entry->name.data);

        n00b_diag_push_related(ctx->diag,
                              N00B_DIAG_WARNING,
                              N00B_STAGE_ANALYSIS,
                              *r"W005",
                              n00b_string_from_cstr(msg_buf),
                              span,
                              related);
    }
}

// ============================================================================
// Run all analyses
// ============================================================================

void
n00b_analyze_all(n00b_analyze_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    n00b_analyze_dead_code(ctx);
    n00b_analyze_use_before_def(ctx);
    n00b_analyze_unused_vars(ctx);
    n00b_analyze_undefined_vars(ctx);
    n00b_analyze_unreachable_after_jump(ctx);
    n00b_analyze_shadowed_vars(ctx);
}
