/**
 * @file optimize.c
 * @brief Regex match-time optimization heuristics.
 *
 * Port of resharp's Optimizations.fs — analyzes regex ASTs to select
 * fast-path matching strategies (prefix acceleration, fixed-length
 * detection, match override).
 */

#include "text/regex/optimize.h"
#include "text/regex/regex.h"
#include "core/alloc.h"

#include <limits.h>

// Forward declarations from deriv.c
extern uint32_t
n00b_regex_dfa_step(n00b_regex_dfa_t       *dfa,
                    uint32_t                state_id,
                    n00b_regex_minterm_id_t mt_id);

// Forward declaration (defined later in this file)
static uint32_t
compute_transition_state(struct n00b_regex_t  *re,
                         n00b_regex_charset_t *prefix_sets,
                         uint32_t              n_prefix);

// ============================================================================
// Configuration limits (matching resharp defaults)
// ============================================================================

#define MAX_PREFIX_LENGTH     64
#define MAX_POTENTIAL_SETS    16
#define POTENTIAL_SIZE_LIMIT  32

// ============================================================================
// Internal: derivative for optimization analysis
// ============================================================================

// Forward declaration — defined in deriv.c
extern uint32_t
n00b_regex_derive_non_initial(n00b_regex_builder_t       *b,
                               n00b_regex_dfa_t           *dfa,
                               uint32_t                    node_id,
                               n00b_regex_charset_t        minterm);

// ============================================================================
// getPrefixNode — strip anchors, lookarounds, optional prefixes
// ============================================================================

uint32_t
n00b_regex_get_prefix_node(n00b_regex_builder_t *b, uint32_t node)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, node);

    switch (n->kind) {
    case N00B_RE_BEGIN:
    case N00B_RE_END:
        return N00B_RE_ID_EPSILON;

    case N00B_RE_LOOKAROUND:
        if (n->lookaround.look_back) {
            // Lookbehind: strip it (doesn't affect prefix)
            return N00B_RE_ID_EPSILON;
        }
        // Lookahead: extract inner node
        return n00b_regex_get_prefix_node(b, n->lookaround.body);

    case N00B_RE_LOOP: {
        uint32_t body = n->loop.body;
        if (n->loop.lo == n->loop.hi) {
            // Fixed repetition {n,n}: keep body × n
            return node;
        }
        if (n->loop.lo == 0) {
            // Optional: skip entirely (might not match)
            return N00B_RE_ID_EPSILON;
        }
        // {lo, hi} where lo > 0: fix to {lo, lo} (the guaranteed part)
        return n00b_regex_mk_loop(b, body, n->loop.lo, n->loop.lo);
    }

    case N00B_RE_CONCAT: {
        uint32_t head = n->concat.head;
        uint32_t tail = n->concat.tail;
        n00b_regex_node_t *hn = n00b_regex_node_get(b, head);

        // Skip optional/star head elements
        if (hn->kind == N00B_RE_LOOP && hn->loop.lo == 0) {
            return n00b_regex_get_prefix_node(b, tail);
        }
        if (hn->kind == N00B_RE_LOOKAROUND && hn->lookaround.look_back) {
            return n00b_regex_get_prefix_node(b, tail);
        }
        if (hn->kind == N00B_RE_BEGIN || hn->kind == N00B_RE_END) {
            return n00b_regex_get_prefix_node(b, tail);
        }
        if (hn->is_always_nullable && hn->kind != N00B_RE_SINGLETON) {
            return n00b_regex_get_prefix_node(b, tail);
        }

        // Recurse on head, keep tail
        uint32_t new_head = n00b_regex_get_prefix_node(b, head);
        if (new_head == head) return node;  // No change
        return n00b_regex_mk_concat(b, new_head, tail);
    }

    case N00B_RE_OR: {
        // Recurse on all children.
        // Anchor-dependent children are replaced with NOTHING — they can't
        // contribute to a deterministic prefix (resharp anchorOpt).
        uint32_t *new_children = n00b_alloc_array(uint32_t, n->multi.count);
        bool changed = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, n->multi.children[i]);
            if (cn->depends_on_anchor) {
                new_children[i] = N00B_RE_ID_NOTHING;
                changed = true;
            }
            else {
                new_children[i] = n00b_regex_get_prefix_node(b, n->multi.children[i]);
                if (new_children[i] != n->multi.children[i]) changed = true;
            }
        }
        if (!changed) {
            n00b_free(new_children);
            return node;
        }
        uint32_t result = n00b_regex_mk_or(b, new_children, n->multi.count);
        n00b_free(new_children);
        return result;
    }

    default:
        return node;
    }
}

// ============================================================================
// getImmediateDerivativesMerged — compute derivatives, merge by successor
// ============================================================================

typedef struct {
    n00b_regex_charset_t merged_minterm;
    uint32_t             deriv_node;
} merged_deriv_t;

// Compute derivatives for all minterms, merging those that lead to the same node.
// Returns count of unique derivatives. Results stored in out (caller-allocated, >= mt->count).
static uint32_t
get_merged_derivatives(n00b_regex_builder_t       *b,
                       n00b_regex_dfa_t           *dfa,
                       n00b_regex_minterm_table_t *mt,
                       uint32_t                    node_id,
                       merged_deriv_t             *out)
{
    uint32_t n_unique = 0;

    for (uint16_t mi = 0; mi < mt->count; mi++) {
        n00b_regex_charset_t minterm = mt->minterms[mi];
        uint32_t deriv = n00b_regex_derive_non_initial(b, dfa, node_id, minterm);

        if (deriv == N00B_RE_ID_NOTHING) continue;  // Dead derivative

        // Check if this derivative already seen
        bool found = false;
        for (uint32_t i = 0; i < n_unique; i++) {
            if (out[i].deriv_node == deriv) {
                out[i].merged_minterm = n00b_regex_charset_or(
                    b->solver, out[i].merged_minterm, minterm);
                found = true;
                break;
            }
        }
        if (!found) {
            out[n_unique].merged_minterm = minterm;
            out[n_unique].deriv_node     = deriv;
            n_unique++;
        }
    }
    return n_unique;
}

// Filter out redundant nodes from derivative results.
static uint32_t
get_non_redundant_derivatives(n00b_regex_builder_t       *b,
                              n00b_regex_dfa_t           *dfa,
                              n00b_regex_minterm_table_t *mt,
                              uint32_t                    node_id,
                              uint32_t                   *redundant,
                              uint32_t                    n_redundant,
                              merged_deriv_t             *out)
{
    merged_deriv_t all[256];
    uint32_t n_all = get_merged_derivatives(b, dfa, mt, node_id, all);

    uint32_t n_out = 0;
    for (uint32_t i = 0; i < n_all; i++) {
        bool is_redundant = false;
        for (uint32_t r = 0; r < n_redundant; r++) {
            if (all[i].deriv_node == redundant[r]) {
                is_redundant = true;
                break;
            }
        }
        if (!is_redundant) {
            out[n_out++] = all[i];
        }
    }
    return n_out;
}

// ============================================================================
// calcPrefixSets — longest deterministic prefix
// ============================================================================

uint32_t
n00b_regex_calc_prefix_sets(struct n00b_regex_t  *re,
                             uint32_t              start_node,
                             n00b_regex_charset_t *out_sets,
                             uint32_t              max_len)
{
    n00b_regex_builder_t *b   = &re->builder;
    n00b_regex_dfa_t     *dfa = re->forward_dfa;
    n00b_regex_minterm_table_t *mt = re->minterms;

    uint32_t prefix_node = n00b_regex_get_prefix_node(b, start_node);

    // Redundant set: BOT, start_node, prefix_node
    uint32_t redundant[8];
    uint32_t n_redundant = 0;
    redundant[n_redundant++] = N00B_RE_ID_NOTHING;
    if (start_node != N00B_RE_ID_NOTHING)
        redundant[n_redundant++] = start_node;
    if (prefix_node != N00B_RE_ID_NOTHING && prefix_node != start_node)
        redundant[n_redundant++] = prefix_node;

    uint32_t cur_node = prefix_node;
    uint32_t count = 0;

    while (count < max_len) {
        // Check if current node is nullable — stop
        n00b_regex_node_t *cn = n00b_regex_node_get(b, cur_node);
        if (cn->is_always_nullable || cn->can_be_nullable) break;

        // Get non-redundant derivatives
        merged_deriv_t derivs[256];
        uint32_t n_derivs = get_non_redundant_derivatives(
            b, dfa, mt, cur_node, redundant, n_redundant, derivs);

        // Must be exactly one unique derivative for deterministic prefix
        if (n_derivs != 1) break;

        // Check for self-loop
        if (derivs[0].deriv_node == cur_node) break;

        out_sets[count++] = derivs[0].merged_minterm;

        // Add current node to redundant set
        if (n_redundant < 8) redundant[n_redundant++] = cur_node;

        cur_node = derivs[0].deriv_node;
    }

    return count;
}

// ============================================================================
// calcPotentialMatchStart — multi-set potential starts
// ============================================================================

static uint32_t
calc_potential_match_start(struct n00b_regex_t  *re,
                           uint32_t              start_node,
                           n00b_regex_charset_t *out_sets,
                           uint32_t              max_len)
{
    n00b_regex_builder_t *b   = &re->builder;
    n00b_regex_dfa_t     *dfa = re->forward_dfa;
    n00b_regex_minterm_table_t *mt = re->minterms;

    // Use a simpler prefix node (no anchor optimization)
    uint32_t prefix_node = n00b_regex_get_prefix_node(b, start_node);

    // Track current frontier of nodes
    uint32_t nodes[POTENTIAL_SIZE_LIMIT];
    uint32_t n_nodes = 1;
    nodes[0] = prefix_node;

    uint32_t redundant[64];
    uint32_t n_redundant = 1;
    redundant[0] = N00B_RE_ID_NOTHING;

    uint32_t count = 0;

    while (count < max_len && n_nodes > 0 && n_nodes <= POTENTIAL_SIZE_LIMIT) {
        // Check if any node is nullable — stop
        bool any_nullable = false;
        for (uint32_t i = 0; i < n_nodes; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, nodes[i]);
            if (cn->is_always_nullable || cn->can_be_nullable) {
                any_nullable = true;
                break;
            }
        }
        if (any_nullable) break;

        // Compute merged minterms across all frontier nodes
        n00b_regex_charset_t merged_mt = b->solver->false_id;
        uint32_t next_nodes[POTENTIAL_SIZE_LIMIT];
        uint32_t n_next = 0;

        for (uint32_t i = 0; i < n_nodes; i++) {
            merged_deriv_t derivs[256];
            uint32_t n_derivs = get_non_redundant_derivatives(
                b, dfa, mt, nodes[i], redundant, n_redundant, derivs);

            for (uint32_t d = 0; d < n_derivs && n_next < POTENTIAL_SIZE_LIMIT; d++) {
                merged_mt = n00b_regex_charset_or(b->solver, merged_mt, derivs[d].merged_minterm);

                // Add derivative to next frontier (deduplicate)
                bool already = false;
                for (uint32_t k = 0; k < n_next; k++) {
                    if (next_nodes[k] == derivs[d].deriv_node) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    next_nodes[n_next++] = derivs[d].deriv_node;
                }
            }
        }

        if (n00b_regex_charset_is_empty(b->solver, merged_mt)) break;

        out_sets[count++] = merged_mt;

        // Update frontier
        n_nodes = n_next;
        for (uint32_t i = 0; i < n_next; i++) {
            nodes[i] = next_nodes[i];
            if (n_redundant < 64) redundant[n_redundant++] = next_nodes[i];
        }
    }

    return count;
}

// ============================================================================
// getFixedPrefixLength — detect constant-length prefix
// ============================================================================

int32_t
n00b_regex_get_fixed_prefix_length(n00b_regex_builder_t *b,
                                    uint32_t              node,
                                    uint32_t             *out_remain)
{
    *out_remain = UINT32_MAX;
    int32_t acc = 0;
    uint32_t cur = node;

    for (int depth = 0; depth < 128; depth++) {
        n00b_regex_node_t *n = n00b_regex_node_get(b, cur);

        switch (n->kind) {
        case N00B_RE_CONCAT: {
            // Recurse on head
            uint32_t head_remain;
            int32_t head_len = n00b_regex_get_fixed_prefix_length(b, n->concat.head, &head_remain);
            if (head_len < 0) return -1;

            if (head_remain == UINT32_MAX) {
                // Head is fully fixed — continue with tail
                acc += head_len;
                cur = n->concat.tail;
                continue;
            }
            else {
                // Head has a remaining part — combine with tail
                acc += head_len;
                *out_remain = n00b_regex_mk_concat(b, head_remain, n->concat.tail);
                return acc;
            }
        }

        case N00B_RE_SINGLETON:
            acc += 1;
            *out_remain = UINT32_MAX;  // Fully consumed
            return acc;

        case N00B_RE_LOOP:
            if (n->loop.lo == n->loop.hi && n->loop.lo == 1) {
                // {1,1} = body itself
                uint32_t body_remain;
                int32_t body_len = n00b_regex_get_fixed_prefix_length(b, n->loop.body, &body_remain);
                if (body_len < 0) return -1;
                acc += body_len;
                *out_remain = (body_remain == UINT32_MAX) ? UINT32_MAX : body_remain;
                return acc;
            }
            if (n->loop.lo == n->loop.hi) {
                // Fixed repetition {n,n}
                n00b_regex_node_t *body = n00b_regex_node_get(b, n->loop.body);
                if (body->kind == N00B_RE_SINGLETON) {
                    acc += n->loop.lo;
                    *out_remain = UINT32_MAX;
                    return acc;
                }
                return -1;
            }
            if (n->loop.lo > 0) {
                // {lo, hi}: fixed part is lo, remaining is Loop(body, 0, hi-lo)
                n00b_regex_node_t *body = n00b_regex_node_get(b, n->loop.body);
                if (body->kind == N00B_RE_SINGLETON) {
                    acc += n->loop.lo;
                    int32_t rem_hi = (n->loop.hi == INT32_MAX) ? INT32_MAX
                                   : n->loop.hi - n->loop.lo;
                    *out_remain = n00b_regex_mk_loop(b, n->loop.body, 0, rem_hi);
                    return acc;
                }
            }
            return -1;

        case N00B_RE_LOOKAROUND:
        case N00B_RE_BEGIN:
        case N00B_RE_END:
            // Zero-width: contribute 0 to prefix
            *out_remain = UINT32_MAX;
            return acc;

        case N00B_RE_NOT:
            return -1;

        case N00B_RE_OR:
        case N00B_RE_AND:
            return -1;

        default:
            return -1;
        }
    }

    return -1;
}

// ============================================================================
// mkNodeWithoutLookbackPrefix — strip lookbehinds for reverse matching
// ============================================================================

static uint32_t
strip_lookback_prefix(n00b_regex_builder_t *b, uint32_t node)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, node);

    switch (n->kind) {
    case N00B_RE_LOOKAROUND:
        if (n->lookaround.look_back) return N00B_RE_ID_EPSILON;
        return node;

    case N00B_RE_BEGIN:
    case N00B_RE_END:
        return N00B_RE_ID_EPSILON;

    case N00B_RE_CONCAT: {
        n00b_regex_node_t *hn = n00b_regex_node_get(b, n->concat.head);
        if (hn->is_always_nullable || hn->kind == N00B_RE_LOOKAROUND
            || hn->kind == N00B_RE_BEGIN || hn->kind == N00B_RE_END) {
            return strip_lookback_prefix(b, n->concat.tail);
        }
        uint32_t new_head = strip_lookback_prefix(b, n->concat.head);
        if (new_head == n->concat.head) return node;
        return n00b_regex_mk_concat(b, new_head, n->concat.tail);
    }

    case N00B_RE_OR: {
        uint32_t *children = n00b_alloc_array(uint32_t, n->multi.count);
        bool changed = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            children[i] = strip_lookback_prefix(b, n->multi.children[i]);
            if (children[i] != n->multi.children[i]) changed = true;
        }
        if (!changed) { n00b_free(children); return node; }
        uint32_t result = n00b_regex_mk_or(b, children, n->multi.count);
        n00b_free(children);
        return result;
    }

    case N00B_RE_AND: {
        uint32_t *children = n00b_alloc_array(uint32_t, n->multi.count);
        bool changed = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            children[i] = strip_lookback_prefix(b, n->multi.children[i]);
            if (children[i] != n->multi.children[i]) changed = true;
        }
        if (!changed) { n00b_free(children); return node; }
        uint32_t result = n00b_regex_mk_and(b, children, n->multi.count);
        n00b_free(children);
        return result;
    }

    default:
        return node;
    }
}

// ============================================================================
// inferLengthLookup — select match-end detection strategy
// ============================================================================

static n00b_regex_len_lookup_t
infer_length_lookup(struct n00b_regex_t *re, uint32_t node)
{
    n00b_regex_builder_t *b = &re->builder;
    n00b_regex_len_lookup_t result = {.kind = N00B_RE_LEN_MATCH_END};

    // Check for fixed-length match
    n00b_regex_node_t *n = n00b_regex_node_get(b, node);
    if (n->min_length == n->max_length && n->min_length >= 0) {
        result.kind = N00B_RE_LEN_FIXED;
        result.fixed.length = n->min_length;
        return result;
    }

    // Check for fixed-length prefix with remaining pattern
    uint32_t remain = UINT32_MAX;
    int32_t prefix_len = n00b_regex_get_fixed_prefix_length(b, node, &remain);
    if (prefix_len < 0 || remain == UINT32_MAX) return result;

    // Pre-compute transition state for skipping the prefix in forward DFA
    n00b_regex_charset_t len_prefix_sets[MAX_PREFIX_LENGTH];
    uint32_t len_n_prefix = n00b_regex_calc_prefix_sets(re, node, len_prefix_sets, MAX_PREFIX_LENGTH);
    uint32_t ts = 0;
    if (len_n_prefix >= (uint32_t)prefix_len) {
        ts = compute_transition_state(re, len_prefix_sets, (uint32_t)prefix_len);
    }

    n00b_regex_node_t *rem_node = n00b_regex_node_get(b, remain);

    // Check if remaining is Loop(Singleton, 0, hi) — bounded character loop
    if (rem_node->kind == N00B_RE_LOOP
        && rem_node->loop.lo == 0
        && rem_node->loop.hi != INT32_MAX) {
        n00b_regex_node_t *body = n00b_regex_node_get(b, rem_node->loop.body);
        if (body->kind == N00B_RE_SINGLETON && rem_node->loop.hi <= 255) {
            result.kind = N00B_RE_LEN_REMAINING_SETS;
            result.remaining.prefix_length  = prefix_len;
            result.remaining.minterm        = body->singleton.set;
            result.remaining.max_remaining  = (uint8_t)rem_node->loop.hi;
            return result;
        }
    }

    // Check if remaining is Loop(Singleton, 0, MAX) — unbounded, but single-char end
    if (rem_node->kind == N00B_RE_LOOP
        && rem_node->loop.lo == 0
        && rem_node->loop.hi == INT32_MAX) {
        n00b_regex_node_t *body = n00b_regex_node_get(b, rem_node->loop.body);
        if (body->kind == N00B_RE_SINGLETON) {
            // Analyze derivatives to see if we can use SetLookup.
            // SetLookup is only valid when the remaining pattern matches exactly
            // 0 or 1 characters (i.e., `x?` not `x*`). For `x*`, the nullable
            // derivative is a self-loop and SetLookup would miss characters.
            merged_deriv_t derivs[256];
            uint32_t n_derivs = get_merged_derivatives(
                b, re->forward_dfa, re->minterms, remain, derivs);

            // Look for: exactly one nullable derivative that leads to NOTHING
            // (meaning the match ends after that one character, no self-loop).
            uint32_t nullable_count = 0;
            n00b_regex_charset_t nullable_mt = b->solver->false_id;
            uint32_t nullable_deriv_node = N00B_RE_ID_NOTHING;
            for (uint32_t i = 0; i < n_derivs; i++) {
                n00b_regex_node_t *dn = n00b_regex_node_get(b, derivs[i].deriv_node);
                if (dn->is_always_nullable) {
                    nullable_count++;
                    nullable_mt = derivs[i].merged_minterm;
                    nullable_deriv_node = derivs[i].deriv_node;
                }
            }

            if (nullable_count == 1 && nullable_deriv_node != remain) {
                // The nullable derivative is NOT a self-loop — check if it
                // leads to only NOTHING (no further matches possible)
                n00b_regex_node_t *nd = n00b_regex_node_get(b, nullable_deriv_node);
                if (nd->max_length == 0) {
                    result.kind = N00B_RE_LEN_SET_LOOKUP;
                    result.set_lookup.prefix_length = prefix_len;
                    result.set_lookup.minterm       = nullable_mt;
                    return result;
                }
            }

            // Fallback: skip prefix transitions
            result.kind = N00B_RE_LEN_FIXED_PREFIX_MATCH_END;
            result.prefix_match_end.prefix_length    = prefix_len;
            result.prefix_match_end.transition_state = ts;
            return result;
        }
    }

    // Generic: if we have a prefix, use prefix-skip
    if (prefix_len > 0) {
        result.kind = N00B_RE_LEN_FIXED_PREFIX_MATCH_END;
        result.prefix_match_end.prefix_length    = prefix_len;
        result.prefix_match_end.transition_state = ts;
    }

    return result;
}

// ============================================================================
// inferOverrideRegex — detect if regex can be replaced with literal search
// ============================================================================

static n00b_regex_override_t
infer_override(struct n00b_regex_t          *re,
               n00b_regex_accelerator_t     *accel,
               n00b_regex_len_lookup_t      *len_lookup,
               uint32_t                      node)
{
    n00b_regex_override_t result = {.kind = N00B_RE_OVERRIDE_NONE};

    n00b_regex_node_t *n = n00b_regex_node_get(&re->builder, node);

    // Can't override if depends on anchors or contains lookarounds
    if (n->depends_on_anchor || n->contains_lookaround) return result;

    // Check: fixed-length match + literal prefix of matching length
    if (len_lookup->kind == N00B_RE_LEN_FIXED
        && accel->kind == N00B_RE_ACCEL_FIXED_PREFIX
        && (int32_t)accel->fixed_prefix.len == len_lookup->fixed.length) {
        result.kind = N00B_RE_OVERRIDE_FIXED_STRING;
        result.fixed_string.codepoints = accel->fixed_prefix.codepoints;
        result.fixed_string.len        = accel->fixed_prefix.len;
    }

    return result;
}

// ============================================================================
// isTooCommon — heuristic: is a minterm too frequent to be useful for accel?
// ============================================================================

// Returns true if using this charset for prefix scanning would waste CPU
// because it matches too many characters (e.g., [a-z], whitespace, .).
static bool
is_too_common(n00b_regex_solver_t *s, n00b_regex_charset_t cs)
{
    // Full set (.) — always too common
    if (n00b_regex_charset_is_full(s, cs)) return true;

    // Build well-known "common" ranges
    n00b_regex_charset_t az    = n00b_regex_charset_range(s, 'a', 'z');
    n00b_regex_charset_t AZ    = n00b_regex_charset_range(s, 'A', 'Z');
    n00b_regex_charset_t azAZ  = n00b_regex_charset_or(s, az, AZ);
    n00b_regex_charset_t digit = n00b_regex_charset_range(s, '0', '9');

    // [a-z] or [A-Z] or [a-zA-Z] — too common in text
    if (cs == az || cs == AZ || cs == azAZ) return true;

    // Supersets of [a-zA-Z] — also too common (e.g., \w)
    if (n00b_regex_charset_contains_set(s, cs, azAZ)) return true;

    // [0-9] — digits are rare enough to be useful
    if (cs == digit) return false;

    // Subsets of [a-z] (e.g., [a-f]) with > 10 chars — too common
    if (n00b_regex_charset_contains_set(s, az, cs)
        && !n00b_regex_charset_contains_set(s, digit, cs)) {
        // Count chars in the ASCII range to estimate
        uint32_t count = 0;
        for (n00b_codepoint_t cp = 0; cp < 128; cp++) {
            if (n00b_regex_charset_contains(s, cs, cp)) count++;
        }
        if (count >= 10) return true;
    }

    // Small sets (< 10 ASCII chars) — rare enough to use
    {
        uint32_t count = 0;
        for (n00b_codepoint_t cp = 0; cp < 128; cp++) {
            if (n00b_regex_charset_contains(s, cs, cp)) {
                count++;
                if (count >= 100) return true;  // Too many
            }
        }
        if (count < 10) return false;  // Rare enough
    }

    // Default: too common
    return true;
}

// ============================================================================
// Compute DFA transition state after consuming a prefix minterm sequence.
// Walks the forward DFA from its start state through each minterm in order.
// Returns the resulting DFA state ID, or 0 if any step leads to a dead state.
// ============================================================================

static uint32_t
compute_transition_state(struct n00b_regex_t  *re,
                         n00b_regex_charset_t *prefix_sets,
                         uint32_t              n_prefix)
{
    if (n_prefix == 0) return re->forward_dfa->start_state;

    n00b_regex_dfa_t *dfa = re->forward_dfa;
    n00b_regex_minterm_table_t *mt = re->minterms;
    uint32_t state = dfa->start_state;

    for (uint32_t i = 0; i < n_prefix; i++) {
        // Find the minterm ID for this charset
        n00b_regex_minterm_id_t mt_id = UINT16_MAX;
        for (uint16_t m = 0; m < mt->count; m++) {
            if (mt->minterms[m] == prefix_sets[i]) {
                mt_id = m;
                break;
            }
        }
        if (mt_id == UINT16_MAX) return 0;  // Charset not a minterm

        state = n00b_regex_dfa_step(dfa, state, mt_id);
        n00b_regex_dfa_state_t *st = &dfa->states.data[state];
        if (st->node_id == N00B_RE_ID_NOTHING) return 0;  // Dead state
    }

    return state;
}

// ============================================================================
// findInitialOptimizations — select prefix acceleration strategy
// ============================================================================

static n00b_regex_accelerator_t
find_initial_optimizations(struct n00b_regex_t *re, uint32_t node)
{
    n00b_regex_accelerator_t result = {.kind = N00B_RE_ACCEL_NONE, .needs_reverse_start = false};
    n00b_regex_builder_t *b = &re->builder;

    // Check if get_prefix_node will strip a nullable head (e.g., .* in .*Holmes).
    // If so, prefix acceleration finds the suffix position, not the true match start.
    // We flag this so find_all_matches uses a reverse scan to find the real start.
    bool stripped_nullable = false;
    {
        uint32_t stripped = n00b_regex_get_prefix_node(b, node);
        if (stripped != node) {
            n00b_regex_node_t *root = n00b_regex_node_get(b, node);
            if (root->kind == N00B_RE_CONCAT) {
                n00b_regex_node_t *head = n00b_regex_node_get(b, root->concat.head);
                if (head->can_be_nullable || head->is_always_nullable) {
                    stripped_nullable = true;
                }
            }
        }
    }
    result.needs_reverse_start = stripped_nullable;

    // Compute prefix sets
    n00b_regex_charset_t prefix_sets[MAX_PREFIX_LENGTH];
    uint32_t n_prefix = n00b_regex_calc_prefix_sets(re, node, prefix_sets, MAX_PREFIX_LENGTH);

    if (n_prefix > 1) {
        // Multi-character prefix: try to extract literal codepoints.
        // Also detect case-insensitive pairs: minterms with exactly 2
        // ASCII chars that are upper/lower variants (e.g., [Aa]).
        n00b_codepoint_t *cps = n00b_alloc_array(n00b_codepoint_t, n_prefix);
        bool all_single = true;
        bool any_ci     = false;

        for (uint32_t i = 0; i < n_prefix; i++) {
            n00b_codepoint_t found[2];
            uint32_t         n_found = 0;

            for (n00b_codepoint_t cp = 0; cp < 128 && n_found <= 2; cp++) {
                if (n00b_regex_charset_contains(b->solver, prefix_sets[i], cp)) {
                    if (n_found < 2) found[n_found] = cp;
                    n_found++;
                }
            }

            if (n_found == 1) {
                cps[i] = found[0];
            }
            else if (n_found == 2) {
                // Check if they're a case pair
                n00b_codepoint_t lo = found[0], hi = found[1];
                if ((lo >= 'A' && lo <= 'Z' && hi == lo + 32)
                    || (lo >= 'a' && lo <= 'z' && hi == lo - 32)) {
                    // Case pair — store lowercase
                    cps[i] = (lo >= 'a' && lo <= 'z') ? lo : hi;
                    any_ci  = true;
                }
                else {
                    all_single = false;
                    break;
                }
            }
            else {
                all_single = false;
                break;
            }
        }

        if (all_single) {
            result.kind = any_ci ? N00B_RE_ACCEL_FIXED_PREFIX_CI
                                 : N00B_RE_ACCEL_FIXED_PREFIX;
            result.fixed_prefix.codepoints      = cps;
            result.fixed_prefix.len              = n_prefix;
            result.fixed_prefix.transition_state = compute_transition_state(re, prefix_sets, n_prefix);
            return result;
        }
        n00b_free(cps);

        // Fall back to minterm prefix — but only if not all too common
        bool all_too_common = true;
        for (uint32_t i = 0; i < n_prefix; i++) {
            if (!is_too_common(b->solver, prefix_sets[i])) {
                all_too_common = false;
                break;
            }
        }
        if (!all_too_common) {
            n00b_regex_charset_t *owned = n00b_alloc_array(n00b_regex_charset_t, n_prefix);
            for (uint32_t i = 0; i < n_prefix; i++) owned[i] = prefix_sets[i];

            result.kind = N00B_RE_ACCEL_MINTERM_PREFIX;
            result.minterm_prefix.minterms         = owned;
            result.minterm_prefix.len              = n_prefix;
            result.minterm_prefix.transition_state = compute_transition_state(re, prefix_sets, n_prefix);
            // Pre-compute ASCII bitmap for first minterm
            for (uint32_t c = 0; c < 128; c++) {
                result.minterm_prefix.first_ascii_map[c] =
                    n00b_regex_charset_contains(b->solver, prefix_sets[0], (n00b_codepoint_t)c) ? 1 : 0;
            }
            return result;
        }
    }

    if (n_prefix == 1) {
        // Single minterm at start — only if not too common
        if (!is_too_common(b->solver, prefix_sets[0])) {
            result.kind = N00B_RE_ACCEL_SINGLE_MINTERM;
            result.single_minterm.minterm          = prefix_sets[0];
            result.single_minterm.transition_state = compute_transition_state(re, prefix_sets, 1);
            // Pre-compute ASCII bitmap
            for (uint32_t c = 0; c < 128; c++) {
                result.single_minterm.ascii_map[c] =
                    n00b_regex_charset_contains(b->solver, prefix_sets[0], (n00b_codepoint_t)c) ? 1 : 0;
            }
            return result;
        }
    }

    // No deterministic prefix or prefix too common — try potential match start
    n00b_regex_charset_t potential_sets[MAX_POTENTIAL_SETS];
    uint32_t n_potential = calc_potential_match_start(re, node, potential_sets, MAX_POTENTIAL_SETS);

    if (n_potential > 0) {
        // Filter out too-common sets
        bool any_useful = false;
        for (uint32_t i = 0; i < n_potential; i++) {
            if (!is_too_common(b->solver, potential_sets[i])) {
                any_useful = true;
                break;
            }
        }
        if (any_useful) {
            n00b_regex_charset_t *owned = n00b_alloc_array(n00b_regex_charset_t, n_potential);
            for (uint32_t i = 0; i < n_potential; i++) owned[i] = potential_sets[i];

            result.kind = N00B_RE_ACCEL_POTENTIAL_START;
            result.potential_start.sets = owned;
            result.potential_start.len  = n_potential;

            // Pre-compute merged ASCII bitmap
            for (uint32_t c = 0; c < 128; c++) {
                result.potential_start.ascii_map[c] = 0;
                for (uint32_t si = 0; si < n_potential; si++) {
                    if (n00b_regex_charset_contains(b->solver, potential_sets[si], (n00b_codepoint_t)c)) {
                        result.potential_start.ascii_map[c] = 1;
                        break;
                    }
                }
            }
        }
    }

    return result;
}

// ============================================================================
// Public API: n00b_regex_find_optimizations
// ============================================================================

void
n00b_regex_find_optimizations(struct n00b_regex_t         *re,
                               n00b_regex_optimizations_t *out)
{
    if (!re || !re->forward_dfa || !re->minterms) {
        *out = (n00b_regex_optimizations_t){
            .accelerator = {.kind = N00B_RE_ACCEL_NONE},
            .len_lookup  = {.kind = N00B_RE_LEN_MATCH_END},
            .override_   = {.kind = N00B_RE_OVERRIDE_NONE},
        };
        return;
    }

    // Get the root node from the forward DFA's start state
    n00b_regex_dfa_state_t *start = &re->forward_dfa->states.data[re->forward_dfa->start_state];
    uint32_t root_node = start->node_id;

    // Phase 1: Find initial acceleration
    out->accelerator = find_initial_optimizations(re, root_node);

    // Phase 2: Infer length lookup
    out->len_lookup = infer_length_lookup(re, root_node);

    // Phase 3: Check for match override
    out->override_ = infer_override(re, &out->accelerator, &out->len_lookup, root_node);
}
