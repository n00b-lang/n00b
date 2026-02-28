// dfg.c — Data flow graph: reaching definitions analysis.
//
// Extracts def/use facts from CFG blocks, computes reaching definitions
// via iterative worklist, and emits data dependence (DD) edges.

#include "slay/dfg.h"
#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include "core/alloc.h"
#include "text/strings/string_ops.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// Variable name extraction
// ============================================================================

// Extract the variable name string from a parse tree node.
// For token leaves: returns the token text as n00b_string_t.
// For interior nodes: walks to the leftmost token leaf.
static n00b_string_t *
extract_var_name(n00b_parse_tree_t *node)
{
    if (!node) {
        return n00b_string_empty();
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            return n00b_option_get(tok->value);
        }

        return n00b_string_empty();
    }

    // Recurse to leftmost token.
    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_string_t *s = extract_var_name(n00b_tree_child(node, i));

        if (s->u8_bytes > 0) {
            return s;
        }
    }

    return n00b_string_empty();
}

// ============================================================================
// Fact emission
// ============================================================================

static void
emit_fact(n00b_list_t(n00b_du_fact_t) *facts, n00b_string_t *var_name,
          n00b_parse_tree_t *node, int32_t block_id, int32_t stmt_ix,
          bool is_def)
{
    if (!var_name || var_name->u8_bytes == 0) {
        return;
    }

    n00b_du_fact_t fact = {
        .var_name = var_name,
        .node     = node,
        .block_id = block_id,
        .stmt_ix  = stmt_ix,
        .id       = (int32_t)n00b_list_len(*facts),
        .is_def   = is_def,
    };

    n00b_list_push(*facts, fact);
}

// ============================================================================
// Walk subtree for identifier uses
// ============================================================================

// Recursively find identifier token leaves in a subtree and emit uses.
// Skips nodes that have their own N00B_CF_ASSIGNS or N00B_CF_VARREF labels
// (they're handled separately in the main extraction).
static void
walk_for_uses(n00b_parse_tree_t *node, n00b_cf_labels_t *cf_labels,
              n00b_grammar_t *grammar,
              int32_t block_id, int32_t stmt_ix,
              n00b_list_t(n00b_du_fact_t) *facts)
{
    if (!node) {
        return;
    }

    if (n00b_tree_is_leaf(node)) {
        // Only emit uses for identifier-like tokens.
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (!tok || !n00b_option_is_set(tok->value)) {
            return;
        }

        // Skip EOF, literals, and operators.
        // Identifiers have non-negative tid or hash-based tid from grammar.
        // We use a simple heuristic: token text starts with [a-zA-Z_].
        n00b_string_t *val = n00b_option_get(tok->value);

        if (!val || val->u8_bytes == 0 || !val->data) {
            return;
        }

        char first = val->data[0];

        if (!((first >= 'a' && first <= 'z')
              || (first >= 'A' && first <= 'Z')
              || first == '_')) {
            return;
        }

        // Skip keywords/terminals registered in the grammar.
        // This is grammar-driven — any registered terminal (keywords,
        // operators, etc.) is not a variable reference.
        if (grammar && n00b_grammar_is_keyword(grammar, val)) {
            return;
        }

        emit_fact(facts, val, node, block_id, stmt_ix, false);
        return;
    }

    // Interior node — check if it has its own CF label (handled separately).
    n00b_cf_label_t *label = n00b_cf_label_lookup(cf_labels, node);

    if (label
        && (label->kind == N00B_CF_ASSIGNS || label->kind == N00B_CF_VARREF)) {
        return;  // Will be / was handled by the main extraction.
    }

    // Function definitions are opaque — don't recurse into them.
    // Their internal variables belong to a separate scope.
    // Detected via @scope("function", ...) tag from the annotation walk.
    n00b_nt_node_t *nt = &n00b_tree_node_value(node);

    if (nt->scope && nt->scope->scope_tag
        && nt->scope->scope_tag->u8_bytes > 0
        && n00b_unicode_str_eq(nt->scope->scope_tag, r"function")) {
        return;
    }

    // Recurse into children.
    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        walk_for_uses(n00b_tree_child(node, i), cf_labels, grammar,
                      block_id, stmt_ix, facts);
    }
}

// ============================================================================
// Def/use fact extraction
// ============================================================================

static void
extract_facts(n00b_cfg_t *cfg, n00b_cf_labels_t *cf_labels,
              n00b_grammar_t *grammar,
              n00b_list_t(n00b_du_fact_t) *facts)
{
    int32_t nb = n00b_cfg_block_count(cfg);

    for (int32_t bi = 0; bi < nb; bi++) {
        n00b_cfg_block_t *blk = n00b_cfg_block(cfg, bi);

        if (!blk) {
            continue;
        }

        size_t ns = n00b_list_len(blk->stmts);

        for (size_t si = 0; si < ns; si++) {
            n00b_parse_tree_t *stmt  = n00b_list_get(blk->stmts, si);
            n00b_cf_label_t   *label = n00b_cf_label_lookup(cf_labels, stmt);

            if (label && label->kind == N00B_CF_ASSIGNS) {
                // LHS = def, RHS = uses.
                n00b_string_t *lhs_name = extract_var_name(label->cond);
                emit_fact(facts, lhs_name, label->cond, bi, (int32_t)si, true);

                // Walk RHS for uses.
                if (label->then_body) {
                    walk_for_uses(label->then_body, cf_labels, grammar,
                                  bi, (int32_t)si, facts);
                }
            }
            else if (label && label->kind == N00B_CF_VARREF) {
                // Pure variable reference = use.
                n00b_string_t *name = extract_var_name(label->cond);
                emit_fact(facts, name, label->cond, bi, (int32_t)si, false);
            }
            else {
                // No specific label — walk the whole subtree for uses.
                walk_for_uses(stmt, cf_labels, grammar,
                              bi, (int32_t)si, facts);
            }
        }
    }
}

// ============================================================================
// Bitset helpers (for gen/kill/in/out sets)
// ============================================================================

// Number of uint64_t words needed for n bits.
static inline size_t
bitset_words(int32_t n)
{
    return ((size_t)n + 63) / 64;
}

static inline void
bitset_set(uint64_t *bs, int32_t bit)
{
    bs[bit / 64] |= (1ULL << (bit % 64));
}

static inline void
bitset_clear(uint64_t *bs, int32_t bit)
{
    bs[bit / 64] &= ~(1ULL << (bit % 64));
}

static inline bool
bitset_test(uint64_t *bs, int32_t bit)
{
    return (bs[bit / 64] & (1ULL << (bit % 64))) != 0;
}

// out = a | b, returns true if out changed.
static bool
bitset_union_into(uint64_t *out, uint64_t *src, size_t nwords)
{
    bool changed = false;

    for (size_t i = 0; i < nwords; i++) {
        uint64_t old = out[i];
        out[i] |= src[i];

        if (out[i] != old) {
            changed = true;
        }
    }

    return changed;
}

// out = a - b  (a AND NOT b)
static void
bitset_subtract(uint64_t *out, uint64_t *a, uint64_t *b, size_t nwords)
{
    for (size_t i = 0; i < nwords; i++) {
        out[i] = a[i] & ~b[i];
    }
}

// ============================================================================
// Reaching definitions — iterative worklist
// ============================================================================

// Compare variable names for equality.
static inline bool
var_name_eq(n00b_string_t *a, n00b_string_t *b)
{
    if (!a || !b) {
        return false;
    }

    return a->u8_bytes == b->u8_bytes
           && a->u8_bytes > 0
           && memcmp(a->data, b->data, a->u8_bytes) == 0;
}

static void
build_reaching_defs(n00b_cfg_t *cfg,
                    n00b_list_t(n00b_du_fact_t) *facts,
                    n00b_list_t(n00b_dd_edge_t) *edges)
{
    int32_t nf = (int32_t)n00b_list_len(*facts);
    int32_t nb = n00b_cfg_block_count(cfg);

    if (nf == 0 || nb == 0) {
        return;
    }

    // Count defs only (for bitset sizing).
    int32_t ndef = 0;

    for (int32_t i = 0; i < nf; i++) {
        if (facts->data[i].is_def) {
            ndef++;
        }
    }

    if (ndef == 0) {
        return;  // No defs — no reaching definitions to compute.
    }

    // Build def_index: maps fact index → def index (dense packing of defs).
    // Also build reverse: def_index → fact_index.
    int32_t *fact_to_def = n00b_alloc_array(int32_t, nf);
    int32_t *def_to_fact = n00b_alloc_array(int32_t, ndef);

    int32_t di = 0;

    for (int32_t i = 0; i < nf; i++) {
        if (facts->data[i].is_def) {
            fact_to_def[i]   = di;
            def_to_fact[di]  = i;
            di++;
        }
        else {
            fact_to_def[i] = -1;
        }
    }

    size_t bsw = bitset_words(ndef);

    // Per-block gen and kill bitsets.
    uint64_t *gen_sets  = n00b_alloc_array(uint64_t, (size_t)nb * bsw);
    uint64_t *kill_sets = n00b_alloc_array(uint64_t, (size_t)nb * bsw);

    #define GEN(b)  (&gen_sets[(size_t)(b) * bsw])
    #define KILL(b) (&kill_sets[(size_t)(b) * bsw])

    // Compute gen/kill per block.
    // gen[B] = defs in B not killed by a later def of the same var in B.
    // kill[B] = all defs (anywhere) of variables that B redefines.
    for (int32_t bi = 0; bi < nb; bi++) {
        // Collect defs in this block (in statement order).
        // For gen: only the last def of each variable in the block survives.
        // Process defs in reverse order — first seen in reverse = last in forward.
        for (int32_t fi = nf - 1; fi >= 0; fi--) {
            n00b_du_fact_t *f = &facts->data[fi];

            if (!f->is_def || f->block_id != bi) {
                continue;
            }

            int32_t dix = fact_to_def[fi];

            // Check if a later def of the same var already killed this one.
            bool killed_later = false;

            for (int32_t fj = fi + 1; fj < nf; fj++) {
                n00b_du_fact_t *fj_p = &facts->data[fj];

                if (fj_p->is_def && fj_p->block_id == bi
                    && var_name_eq(fj_p->var_name, f->var_name)) {
                    killed_later = true;
                    break;
                }
            }

            if (!killed_later) {
                bitset_set(GEN(bi), dix);
            }

            // kill[B] = all OTHER defs of the same variable (from any block).
            for (int32_t dk = 0; dk < ndef; dk++) {
                int32_t fk = def_to_fact[dk];

                if (fk != fi
                    && var_name_eq(facts->data[fk].var_name, f->var_name)) {
                    bitset_set(KILL(bi), dk);
                }
            }
        }
    }

    // in/out bitsets per block.
    uint64_t *in_sets  = n00b_alloc_array(uint64_t, (size_t)nb * bsw);
    uint64_t *out_sets = n00b_alloc_array(uint64_t, (size_t)nb * bsw);

    #define IN(b)  (&in_sets[(size_t)(b) * bsw])
    #define OUT(b) (&out_sets[(size_t)(b) * bsw])

    // Initialize out[B] = gen[B].
    for (int32_t bi = 0; bi < nb; bi++) {
        memcpy(OUT(bi), GEN(bi), bsw * sizeof(uint64_t));
    }

    // Iterative fixpoint.
    uint64_t *tmp = n00b_alloc_array(uint64_t, bsw);
    bool      changed = true;

    while (changed) {
        changed = false;

        for (int32_t bi = 0; bi < nb; bi++) {
            // in[B] = ∪ out[P] for predecessors P.
            n00b_list_t(n00b_cfg_edge_t) preds = n00b_cfg_predecessors(cfg, bi);
            int32_t                       np    = (int32_t)n00b_list_len(preds);

            memset(IN(bi), 0, bsw * sizeof(uint64_t));

            for (int32_t pi = 0; pi < np; pi++) {
                bitset_union_into(IN(bi), OUT(preds.data[pi].from_id), bsw);
            }

            n00b_list_free(preds);

            // out[B] = gen[B] ∪ (in[B] - kill[B])
            bitset_subtract(tmp, IN(bi), KILL(bi), bsw);
            bitset_union_into(tmp, GEN(bi), bsw);

            // Check if out changed.
            if (memcmp(OUT(bi), tmp, bsw * sizeof(uint64_t)) != 0) {
                memcpy(OUT(bi), tmp, bsw * sizeof(uint64_t));
                changed = true;
            }
        }
    }

    // ========================================================================
    // Build DD edges: for each use, find reaching defs of same variable.
    // ========================================================================

    for (int32_t fi = 0; fi < nf; fi++) {
        n00b_du_fact_t *use = &facts->data[fi];

        if (use->is_def) {
            continue;
        }

        int32_t bi = use->block_id;

        // Check in[B] for defs of the same variable from other blocks.
        for (int32_t dk = 0; dk < ndef; dk++) {
            if (!bitset_test(IN(bi), dk)) {
                continue;
            }

            int32_t         fk   = def_to_fact[dk];
            n00b_du_fact_t *def_f = &facts->data[fk];

            if (!var_name_eq(def_f->var_name, use->var_name)) {
                continue;
            }

            // Check if this def is killed by an earlier def in the same block.
            bool killed_before_use = false;

            for (int32_t fj = 0; fj < nf; fj++) {
                n00b_du_fact_t *fj_p = &facts->data[fj];

                if (fj_p->is_def && fj_p->block_id == bi
                    && fj_p->stmt_ix < use->stmt_ix
                    && var_name_eq(fj_p->var_name, use->var_name)) {
                    // A def in the same block strictly before this use
                    // kills any incoming def of the same variable.
                    // Note: must be < not <= because in `x = x + 1` the
                    // RHS use and LHS def share the same stmt_ix, but
                    // the RHS is evaluated before the LHS def takes effect.
                    killed_before_use = true;
                    break;
                }
            }

            if (!killed_before_use) {
                n00b_dd_edge_t edge = {.def_id = fk, .use_id = fi};
                n00b_list_push(*edges, edge);
            }
        }

        // Check defs in the same block that precede this use.
        // The most recent def of the same variable before this use reaches it.
        int32_t last_local_def = -1;

        for (int32_t fj = 0; fj < nf; fj++) {
            n00b_du_fact_t *fj_p = &facts->data[fj];

            if (fj_p->is_def && fj_p->block_id == bi
                && fj_p->stmt_ix < use->stmt_ix
                && var_name_eq(fj_p->var_name, use->var_name)) {
                last_local_def = fj;  // Later ones overwrite earlier.
            }
        }

        if (last_local_def >= 0) {
            n00b_dd_edge_t edge = {.def_id = last_local_def, .use_id = fi};
            n00b_list_push(*edges, edge);
        }

        // Also check defs at the same statement index (e.g., x = x + 1:
        // the use of x on the RHS is reached by preceding defs, not the
        // def on the LHS of the same statement). The LHS def does NOT
        // reach its own RHS uses — already handled by stmt_ix < check above.
    }

    #undef GEN
    #undef KILL
    #undef IN
    #undef OUT

    n00b_free(fact_to_def);
    n00b_free(def_to_fact);
    n00b_free(gen_sets);
    n00b_free(kill_sets);
    n00b_free(in_sets);
    n00b_free(out_sets);
    n00b_free(tmp);
}

// ============================================================================
// Public API — construction
// ============================================================================

n00b_dfg_t *
n00b_build_dfg(n00b_cfg_t          *cfg,
               n00b_cf_labels_t    *cf_labels,
               n00b_grammar_t      *grammar,
               n00b_annot_result_t *annot)
{
    if (!cfg) {
        return NULL;
    }

    int32_t nb = n00b_cfg_block_count(cfg);

    if (nb <= 0) {
        return NULL;
    }

    n00b_dfg_t *dfg = n00b_alloc(n00b_dfg_t);
    dfg->cfg       = cfg;
    dfg->cf_labels = cf_labels;
    dfg->name      = cfg->name;
    dfg->facts     = n00b_list_new_private(n00b_du_fact_t);
    dfg->edges     = n00b_list_new_private(n00b_dd_edge_t);

    // Emit synthetic DEF facts for function parameters at the entry block.
    // These go before normal block-walking facts so that parameter defs
    // have the lowest fact IDs and stmt_ix = -1.
    if (annot && annot->params) {
        size_t np = n00b_list_len(*annot->params);

        for (size_t i = 0; i < np; i++) {
            n00b_sym_entry_t *sym = n00b_list_get(*annot->params, i);

            emit_fact(&dfg->facts, sym->name, sym->decl_node,
                      cfg->entry_id, -1, true);
        }
    }

    extract_facts(cfg, cf_labels, grammar, &dfg->facts);
    build_reaching_defs(cfg, &dfg->facts, &dfg->edges);

    return dfg;
}

// ============================================================================
// Public API — query
// ============================================================================

n00b_list_t(n00b_dd_edge_t)
n00b_dfg_reaching_defs(n00b_dfg_t *dfg, int32_t use_id)
{
    n00b_list_t(n00b_dd_edge_t) result = n00b_list_new_private(n00b_dd_edge_t);

    if (!dfg) {
        return result;
    }

    size_t ne = n00b_list_len(dfg->edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_dd_edge_t *e = &dfg->edges.data[i];

        if (e->use_id == use_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

n00b_list_t(n00b_dd_edge_t)
n00b_dfg_reached_uses(n00b_dfg_t *dfg, int32_t def_id)
{
    n00b_list_t(n00b_dd_edge_t) result = n00b_list_new_private(n00b_dd_edge_t);

    if (!dfg) {
        return result;
    }

    size_t ne = n00b_list_len(dfg->edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_dd_edge_t *e = &dfg->edges.data[i];

        if (e->def_id == def_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

// ============================================================================
// Public API — cleanup
// ============================================================================

void
n00b_dfg_free(n00b_dfg_t *dfg)
{
    if (!dfg) {
        return;
    }

    n00b_list_free(dfg->facts);
    n00b_list_free(dfg->edges);
    n00b_free(dfg);
}
