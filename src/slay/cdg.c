// cdg.c — Control dependence graph via post-dominator tree.
//
// Computes post-dominators using the Cooper/Harvey/Kennedy iterative
// fixpoint on the reversed CFG, then derives control dependence edges.

#include "slay/cdg.h"
#include "core/alloc.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// DFS on reversed CFG (from exit following predecessors)
// ============================================================================

// Fills rpo_order[0..count-1] with block IDs in reverse-postorder
// and rpo_number[block_id] with the RPO index.  Returns count of
// reachable blocks.

static int32_t
reverse_postorder_dfs(n00b_cfg_t *cfg, int32_t exit_id,
                      int32_t *rpo_order, int32_t *rpo_number)
{
    int32_t nb = n00b_cfg_block_count(cfg);

    bool *visited = n00b_alloc_array(bool, nb);

    // Iterative post-order DFS using an explicit stack.
    // Stack entries: (block_id, phase).  phase=0 means "push successors
    // in reverse graph" (= predecessors in CFG), phase=1 means "emit".
    typedef struct {
        int32_t id;
        int32_t phase;
    } frame_t;

    frame_t *stack = n00b_alloc_array(frame_t, nb * 2);
    int      sp    = 0;

    // Post-order buffer (we'll reverse at the end).
    int32_t *po    = n00b_alloc_array(int32_t, nb);
    int32_t  po_ix = 0;

    stack[sp++] = (frame_t){exit_id, 0};
    visited[exit_id] = true;

    while (sp > 0) {
        frame_t *top = &stack[sp - 1];

        if (top->phase == 1) {
            // Emit post-order.
            po[po_ix++] = top->id;
            sp--;
            continue;
        }

        // Mark for emit on return.
        top->phase = 1;

        // Push predecessors (= successors in reverse graph).
        n00b_list_t(n00b_cfg_edge_t) preds = n00b_cfg_predecessors(cfg, top->id);
        int32_t                       np    = (int32_t)n00b_list_len(preds);

        for (int32_t i = 0; i < np; i++) {
            int32_t pred = preds.data[i].from_id;

            if (!visited[pred]) {
                visited[pred] = true;
                stack[sp++]   = (frame_t){pred, 0};
            }
        }

        n00b_list_free(preds);
    }

    // Reverse post-order = reversed po array.
    for (int32_t i = 0; i < po_ix; i++) {
        int32_t block_id     = po[po_ix - 1 - i];
        rpo_order[i]         = block_id;
        rpo_number[block_id] = i;
    }

    // Mark unreachable blocks.
    for (int32_t i = 0; i < nb; i++) {
        if (!visited[i]) {
            rpo_number[i] = -1;
        }
    }

    n00b_free(visited);
    n00b_free(stack);
    n00b_free(po);

    return po_ix;
}

// ============================================================================
// CHK intersection
// ============================================================================

// Walk up the idom chain from whichever has higher RPO number until
// both fingers point to the same block.

static int32_t
intersect(int32_t a, int32_t b, int32_t *idom, int32_t *rpo_number)
{
    while (a != b) {
        while (rpo_number[a] > rpo_number[b]) {
            a = idom[a];
        }

        while (rpo_number[b] > rpo_number[a]) {
            b = idom[b];
        }
    }

    return a;
}

// ============================================================================
// Build post-dominator tree
// ============================================================================

static void
build_pdom(n00b_cfg_t *cfg, n00b_array_t(n00b_pdom_info_t) *pdom)
{
    int32_t nb      = n00b_cfg_block_count(cfg);
    int32_t exit_id = cfg->exit_id;

    // RPO numbering on reversed CFG.
    int32_t *rpo_order  = n00b_alloc_array(int32_t, nb);
    int32_t *rpo_number = n00b_alloc_array(int32_t, nb);
    int32_t  rpo_count  = reverse_postorder_dfs(cfg, exit_id,
                                                 rpo_order, rpo_number);

    // idom table.  idom[exit] = exit, all others = -1.
    int32_t *idom = n00b_alloc_array(int32_t, nb);

    for (int32_t i = 0; i < nb; i++) {
        idom[i] = -1;
    }

    idom[exit_id] = exit_id;

    // Iterative fixpoint.
    bool changed = true;

    while (changed) {
        changed = false;

        for (int32_t ri = 0; ri < rpo_count; ri++) {
            int32_t b = rpo_order[ri];

            if (b == exit_id) {
                continue;
            }

            // In the reversed CFG, successors of B are CFG successors of B.
            n00b_list_t(n00b_cfg_edge_t) succs = n00b_cfg_successors(cfg, b);
            int32_t                       ns    = (int32_t)n00b_list_len(succs);

            int32_t new_idom = -1;

            for (int32_t i = 0; i < ns; i++) {
                int32_t s = succs.data[i].to_id;

                // Skip unreachable or unprocessed successors.
                if (rpo_number[s] < 0 || idom[s] < 0) {
                    continue;
                }

                if (new_idom < 0) {
                    new_idom = s;
                }
                else {
                    new_idom = intersect(new_idom, s, idom, rpo_number);
                }
            }

            n00b_list_free(succs);

            if (new_idom >= 0 && new_idom != idom[b]) {
                idom[b]  = new_idom;
                changed  = true;
            }
        }
    }

    // Fill the pdom array with idom and depth.
    for (int32_t i = 0; i < nb; i++) {
        n00b_pdom_info_t info = {.idom = idom[i], .depth = 0};
        n00b_array_set(*pdom, i, info);
    }

    // Compute depths by walking idom chains.
    for (int32_t i = 0; i < nb; i++) {
        if (idom[i] < 0) {
            // Unreachable block.
            n00b_pdom_info_t info = {.idom = -1, .depth = -1};
            n00b_array_set(*pdom, i, info);
            continue;
        }

        int32_t depth = 0;
        int32_t cur   = i;

        while (cur != idom[cur]) {
            depth++;
            cur = idom[cur];
        }

        n00b_pdom_info_t info = {.idom = idom[i], .depth = depth};
        n00b_array_set(*pdom, i, info);
    }

    n00b_free(rpo_order);
    n00b_free(rpo_number);
    n00b_free(idom);
}

// ============================================================================
// Build control dependence edges
// ============================================================================

// For each CFG edge A→B where B does NOT post-dominate A:
//   walk from B up the idom chain until reaching idom[A],
//   marking every visited node as control-dependent on A.

static void
build_cd_edges(n00b_cfg_t *cfg,
               n00b_array_t(n00b_pdom_info_t) *pdom,
               n00b_list_t(n00b_cd_edge_t) *cd_edges)
{
    int32_t ne = n00b_cfg_edge_count(cfg);
    int32_t nb = n00b_cfg_block_count(cfg);

    for (int32_t i = 0; i < ne; i++) {
        n00b_cfg_edge_t *e = &cfg->edges.data[i];
        int32_t          a = e->from_id;
        int32_t          b = e->to_id;

        // Skip if either end is unreachable.
        if (a < 0 || b < 0 || a >= nb || b >= nb) {
            continue;
        }

        n00b_pdom_info_t a_info = n00b_array_get(*pdom, a);

        if (a_info.idom < 0) {
            continue;  // A unreachable.
        }

        n00b_pdom_info_t b_info = n00b_array_get(*pdom, b);

        if (b_info.idom < 0) {
            continue;  // B unreachable.
        }

        // Check: does B post-dominate A?
        // If B's idom chain reaches A, then B post-dominates A — skip.
        int32_t idom_a = a_info.idom;

        // Walk from B up to idom[A], adding CD edges.
        int32_t cur = b;

        while (cur >= 0 && cur != idom_a) {
            n00b_cd_edge_t cd = {
                .controller_id = a,
                .dependent_id  = cur,
                .edge_kind     = e->kind,
            };

            n00b_list_push(*cd_edges, cd);

            n00b_pdom_info_t cur_info = n00b_array_get(*pdom, cur);

            if (cur_info.idom == cur) {
                break;  // Reached exit (idom[exit] = exit).
            }

            cur = cur_info.idom;
        }
    }
}

// ============================================================================
// Public API — construction
// ============================================================================

n00b_cdg_t *
n00b_build_cdg(n00b_cfg_t *cfg)
{
    if (!cfg) {
        return NULL;
    }

    int32_t nb = n00b_cfg_block_count(cfg);

    if (nb <= 0) {
        return NULL;
    }

    n00b_cdg_t *cdg = n00b_alloc(n00b_cdg_t);
    cdg->cfg      = cfg;
    cdg->name     = cfg->name;
    cdg->pdom     = n00b_array_new(n00b_pdom_info_t, nb);
    cdg->cd_edges = n00b_list_new_private(n00b_cd_edge_t);

    // Initialize array length to nb (all slots used).
    for (int32_t i = 0; i < nb; i++) {
        n00b_array_set(cdg->pdom, i, ((n00b_pdom_info_t){.idom = -1, .depth = -1}));
    }

    build_pdom(cfg, &cdg->pdom);
    build_cd_edges(cfg, &cdg->pdom, &cdg->cd_edges);

    return cdg;
}

// ============================================================================
// Public API — query
// ============================================================================

n00b_list_t(n00b_cd_edge_t)
n00b_cdg_controllers(n00b_cdg_t *cdg, int32_t block_id)
{
    n00b_list_t(n00b_cd_edge_t) result = n00b_list_new_private(n00b_cd_edge_t);

    if (!cdg) {
        return result;
    }

    size_t ne = n00b_list_len(cdg->cd_edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_cd_edge_t *e = &cdg->cd_edges.data[i];

        if (e->dependent_id == block_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

n00b_list_t(n00b_cd_edge_t)
n00b_cdg_dependents(n00b_cdg_t *cdg, int32_t block_id)
{
    n00b_list_t(n00b_cd_edge_t) result = n00b_list_new_private(n00b_cd_edge_t);

    if (!cdg) {
        return result;
    }

    size_t ne = n00b_list_len(cdg->cd_edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_cd_edge_t *e = &cdg->cd_edges.data[i];

        if (e->controller_id == block_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

// ============================================================================
// Public API — cleanup
// ============================================================================

void
n00b_cdg_free(n00b_cdg_t *cdg)
{
    if (!cdg) {
        return;
    }

    n00b_array_free(cdg->pdom);
    n00b_list_free(cdg->cd_edges);
    n00b_free(cdg);
}
