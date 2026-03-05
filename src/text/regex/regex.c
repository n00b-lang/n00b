#include "text/regex/regex.h"
#include "text/regex/parse.h"
#include "core/alloc.h"
#include "core/string.h"

#include <limits.h>
#include <stdio.h>

// Forward declarations from deriv.c
extern n00b_regex_dfa_t *
n00b_regex_dfa_new(n00b_regex_builder_t       *builder,
                   n00b_regex_minterm_table_t *minterms,
                   uint32_t                    root_node,
                   bool                        is_initial);

extern uint32_t
n00b_regex_dfa_step(n00b_regex_dfa_t       *dfa,
                    uint32_t                state_id,
                    n00b_regex_minterm_id_t mt_id);

// ============================================================================
// Node reversal (for building reverse DFA)
// ============================================================================

static uint32_t
reverse_node(n00b_regex_builder_t *b, uint32_t node_id)
{
    if (node_id < N00B_RE_SENTINEL_COUNT) return node_id;

    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);

    switch (n->kind) {
    case N00B_RE_SINGLETON:
        return node_id;

    case N00B_RE_CONCAT: {
        uint32_t rh = reverse_node(b, n->concat.head);
        uint32_t rt = reverse_node(b, n->concat.tail);
        return n00b_regex_mk_concat(b, rt, rh);
    }

    case N00B_RE_OR: {
        uint32_t *buf = n00b_alloc_array(uint32_t, n->multi.count);
        for (uint32_t i = 0; i < n->multi.count; i++) {
            buf[i] = reverse_node(b, n->multi.children[i]);
        }
        uint32_t result = n00b_regex_mk_or(b, buf, n->multi.count);
        n00b_free(buf);
        return result;
    }

    case N00B_RE_AND: {
        uint32_t *buf = n00b_alloc_array(uint32_t, n->multi.count);
        for (uint32_t i = 0; i < n->multi.count; i++) {
            buf[i] = reverse_node(b, n->multi.children[i]);
        }
        uint32_t result = n00b_regex_mk_and(b, buf, n->multi.count);
        n00b_free(buf);
        return result;
    }

    case N00B_RE_NOT: {
        auto not_r = n00b_regex_mk_not(b, reverse_node(b, n->not_.inner));
        return n00b_result_is_ok(not_r) ? n00b_result_get(not_r) : node_id;
    }

    case N00B_RE_LOOP: {
        uint32_t rb = reverse_node(b, n->loop.body);
        return n00b_regex_mk_loop(b, rb, n->loop.lo, n->loop.hi);
    }

    case N00B_RE_LOOKAROUND: {
        // Resharp reversal logic for lookarounds:
        // Lookahead(body) reverses to Lookbehind(rev(body))
        // Lookbehind(body) reverses to Lookahead(rev(body))
        // Strip .* prefix from body for lookback before reversing
        uint32_t body = n->lookaround.body;
        bool     lb   = n->lookaround.look_back;

        if (!lb) {
            // Lookahead → Lookbehind: strip trailing .*
            // SplitTail: walk concat chain, collect heads, get tail
            // If tail == DOTSTAR, reverse just the heads
            uint32_t cur = body;
            uint32_t heads[64];
            uint32_t n_heads = 0;

            while (n_heads < 63) {
                n00b_regex_node_t *cn = n00b_regex_node_get(b, cur);
                if (cn->kind != N00B_RE_CONCAT) break;
                heads[n_heads++] = cn->concat.head;
                cur = cn->concat.tail;
            }

            if (cur == N00B_RE_ID_DOTSTAR && n_heads > 0) {
                // Build reversed concat of just the heads
                uint32_t rev_body = reverse_node(b, heads[n_heads - 1]);
                for (int32_t i = (int32_t)n_heads - 2; i >= 0; i--) {
                    rev_body = n00b_regex_mk_concat(b, rev_body, reverse_node(b, heads[i]));
                }
                return n00b_regex_mk_lookaround(b, rev_body, true, 0, nullptr, 0);
            }
            else {
                uint32_t rb = reverse_node(b, body);
                return n00b_regex_mk_lookaround(b, rb, true, 0, nullptr, 0);
            }
        }
        else {
            // Lookbehind → Lookahead: strip leading .*
            n00b_regex_node_t *body_node = n00b_regex_node_get(b, body);
            if (body_node->kind == N00B_RE_CONCAT
                && body_node->concat.head == N00B_RE_ID_DOTSTAR) {
                uint32_t rb = reverse_node(b, body_node->concat.tail);
                return n00b_regex_mk_lookaround(b, rb, false, 0, nullptr, 0);
            }
            else {
                uint32_t rb = reverse_node(b, body);
                return n00b_regex_mk_lookaround(b, rb, false, 0, nullptr, 0);
            }
        }
    }

    case N00B_RE_BEGIN:
    case N00B_RE_END:
        return node_id;
    }
    return node_id;
}

// ============================================================================
// Strip leading/trailing anchors from a pattern for forward DFA use.
//
// The reverse scan's HandleInputEnd / HandleInputStart already verify
// anchor conditions at the boundaries.  The forward DFA only needs to
// match the non-anchor payload, so we erase Begin/End nodes (replacing
// them with EPSILON) and simplify lookarounds whose bodies are just
// anchors.
// ============================================================================

// Check if a node tree contains BEGIN or END anchors (iterative).
static bool
node_contains_anchor(n00b_regex_builder_t *b, uint32_t node_id)
{
    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = node_id;

    while (sp > 0) {
        uint32_t cur = stack[--sp];
        if (cur == N00B_RE_ID_BEGIN || cur == N00B_RE_ID_END) return true;
        if (cur < N00B_RE_SENTINEL_COUNT) continue;

        n00b_regex_node_t *n = n00b_regex_node_get(b, cur);
        switch (n->kind) {
        case N00B_RE_CONCAT:
            if (sp < 126) { stack[sp++] = n->concat.tail; stack[sp++] = n->concat.head; }
            break;
        case N00B_RE_OR:
        case N00B_RE_AND:
            for (uint32_t i = 0; i < n->multi.count && sp < 127; i++)
                stack[sp++] = n->multi.children[i];
            break;
        case N00B_RE_NOT:
            if (sp < 127) stack[sp++] = n->not_.inner;
            break;
        case N00B_RE_LOOP:
            if (sp < 127) stack[sp++] = n->loop.body;
            break;
        case N00B_RE_LOOKAROUND:
            // Don't recurse into lookarounds — anchors inside lookarounds
            // (e.g., \b word boundary uses BEGIN/END internally) don't make
            // the outer pattern "anchored" for prefix acceleration purposes.
            break;
        case N00B_RE_BEGIN:
        case N00B_RE_END:
            return true;
        default:
            break;
        }
    }
    return false;
}

static uint32_t
strip_anchors(n00b_regex_builder_t *b, uint32_t node_id)
{
    if (node_id < N00B_RE_SENTINEL_COUNT) {
        // Strip BEGIN and END to EPSILON — matches resharp's
        // mkNodeWithoutLookbackPrefix which strips Begin, End, and Lookbehind.
        // Both anchors are validated by the reverse scan, not the forward DFA.
        if (node_id == N00B_RE_ID_BEGIN || node_id == N00B_RE_ID_END) {
            return N00B_RE_ID_EPSILON;
        }
        return node_id;
    }

    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);

    switch (n->kind) {
    case N00B_RE_LOOKAROUND:
        if (n->lookaround.look_back) {
            // Resharp: LookAround(lookBack=true) → EPS
            // Lookbehinds are validated by the reverse scan, not the forward DFA.
            return N00B_RE_ID_EPSILON;
        }
        // Lookaheads: keep as-is (they have their own semantics)
        return node_id;
    case N00B_RE_CONCAT: {
        n00b_regex_node_t *head_n = n00b_regex_node_get(b, n->concat.head);
        // Resharp: Concat where head is lookbehind → skip head, recurse tail
        if (head_n->kind == N00B_RE_LOOKAROUND && head_n->lookaround.look_back) {
            return strip_anchors(b, n->concat.tail);
        }
        // Resharp: Concat where head is always-nullable → recurse tail, keep head
        if (head_n->is_always_nullable) {
            uint32_t new_tail = strip_anchors(b, n->concat.tail);
            if (new_tail == n->concat.tail) return node_id;
            return n00b_regex_mk_concat(b, n->concat.head, new_tail);
        }
        // Otherwise: recurse on head; if head becomes EPS, recurse on tail
        uint32_t new_head = strip_anchors(b, n->concat.head);
        if (new_head == N00B_RE_ID_EPSILON) {
            return strip_anchors(b, n->concat.tail);
        }
        if (new_head == n->concat.head) return node_id;
        return n00b_regex_mk_concat(b, new_head, n->concat.tail);
    }
    case N00B_RE_OR: {
        uint32_t *buf = n00b_alloc_array(uint32_t, n->multi.count);
        bool changed = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            buf[i] = strip_anchors(b, n->multi.children[i]);
            if (buf[i] != n->multi.children[i]) changed = true;
        }
        if (!changed) { n00b_free(buf); return node_id; }
        uint32_t result = n00b_regex_mk_or(b, buf, n->multi.count);
        n00b_free(buf);
        return result;
    }
    case N00B_RE_AND: {
        uint32_t *buf = n00b_alloc_array(uint32_t, n->multi.count);
        bool changed = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            buf[i] = strip_anchors(b, n->multi.children[i]);
            if (buf[i] != n->multi.children[i]) changed = true;
        }
        if (!changed) { n00b_free(buf); return node_id; }
        uint32_t result = n00b_regex_mk_and(b, buf, n->multi.count);
        n00b_free(buf);
        return result;
    }
    case N00B_RE_NOT:
        // Resharp: Not → keep as-is (don't recurse)
        return node_id;
    case N00B_RE_LOOP: {
        uint32_t new_body = strip_anchors(b, n->loop.body);
        if (new_body == n->loop.body) return node_id;
        return n00b_regex_mk_loop(b, new_body, n->loop.lo, n->loop.hi);
    }
    case N00B_RE_BEGIN:
    case N00B_RE_END:
        return N00B_RE_ID_EPSILON;
    default:
        return node_id;
    }
}

// ============================================================================
// Collect predicates from the AST for minterm computation
// ============================================================================

static void
collect_predicates(n00b_regex_builder_t *b, uint32_t node_id,
                   n00b_list_t(n00b_regex_charset_t) *preds)
{
    if (node_id < N00B_RE_SENTINEL_COUNT) return;

    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);

    switch (n->kind) {
    case N00B_RE_SINGLETON:
        for (uint32_t i = 0; i < preds->len; i++) {
            if (preds->data[i] == n->singleton.set) return;
        }
        n00b_list_push(*preds, n->singleton.set);
        break;
    case N00B_RE_CONCAT:
        collect_predicates(b, n->concat.head, preds);
        collect_predicates(b, n->concat.tail, preds);
        break;
    case N00B_RE_OR:
    case N00B_RE_AND:
        for (uint32_t i = 0; i < n->multi.count; i++) {
            collect_predicates(b, n->multi.children[i], preds);
        }
        break;
    case N00B_RE_NOT:
        collect_predicates(b, n->not_.inner, preds);
        break;
    case N00B_RE_LOOP:
        collect_predicates(b, n->loop.body, preds);
        break;
    case N00B_RE_LOOKAROUND:
        collect_predicates(b, n->lookaround.body, preds);
        break;
    case N00B_RE_BEGIN:
    case N00B_RE_END:
        break;
    }
}

// ============================================================================
// Public API: n00b_regex_new
// ============================================================================

n00b_result_t(n00b_regex_t *)
n00b_regex_new(n00b_string_t *pattern) _kargs {
    bool case_insensitive = false;
    bool multiline        = false;
    bool dot_all          = false;
}
{
    n00b_regex_t *re = n00b_alloc(n00b_regex_t);

    re->pattern     = pattern;
    re->solver      = n00b_alloc(n00b_regex_solver_t);
    *re->solver     = n00b_regex_solver_new();
    re->builder     = n00b_alloc(n00b_regex_builder_t);
    *re->builder    = n00b_regex_builder_new(re->solver);
    re->is_full_dfa = false;

    // Parse the pattern
    auto parse_result = n00b_regex_parse(re->builder, pattern,
                                          case_insensitive, multiline, dot_all);
    if (n00b_result_is_err(parse_result)) {
        return n00b_result_err(n00b_regex_t *, n00b_result_get_err(parse_result));
    }

    uint32_t root = n00b_result_get(parse_result);

    // Collect character-set predicates from the AST
    n00b_list_t(n00b_regex_charset_t) preds = n00b_list_new_cap_private(n00b_regex_charset_t, 64);
    collect_predicates(re->builder, root, &preds);

    // Compute minterms
    re->minterms = n00b_regex_compute_minterms(re->solver, preds.data, (uint32_t)preds.len);

    // Build forward DFA from anchor-stripped R — anchors are verified by the
    // reverse scan's HandleInputEnd / HandleInputStart, so the forward DFA
    // only needs to match the non-anchor payload.
    // Check if the original (pre-strip) pattern depends on anchors.
    // Walk the tree to detect BEGIN/END nodes, since depends_on_anchor may
    // not propagate through all concat normalization paths.
    uint32_t fwd_root = strip_anchors(re->builder, root);

    {
        static bool sa_dbg = false, sa_dbg_checked = false;
        if (!sa_dbg_checked) { sa_dbg = getenv("N00B_RE_DEBUG") != nullptr; sa_dbg_checked = true; }
        if (sa_dbg && fwd_root != root) {
            fprintf(stderr, "DBG strip_anchors CHANGED: root=%u fwd_root=%u\n", root, fwd_root);
            // Dump root node
            if (root >= N00B_RE_SENTINEL_COUNT) {
                n00b_regex_node_t *rn = n00b_regex_node_get(re->builder, root);
                fprintf(stderr, "  root kind=%d\n", rn->kind);
                if (rn->kind == N00B_RE_CONCAT) {
                    fprintf(stderr, "  head=%u tail=%u\n", rn->concat.head, rn->concat.tail);
                    if (rn->concat.head >= N00B_RE_SENTINEL_COUNT) {
                        n00b_regex_node_t *h = n00b_regex_node_get(re->builder, rn->concat.head);
                        fprintf(stderr, "    head kind=%d always_null=%d\n", h->kind, h->is_always_nullable);
                    }
                    if (rn->concat.tail >= N00B_RE_SENTINEL_COUNT) {
                        n00b_regex_node_t *t = n00b_regex_node_get(re->builder, rn->concat.tail);
                        fprintf(stderr, "    tail kind=%d\n", t->kind);
                    }
                } else if (rn->kind == N00B_RE_AND) {
                    fprintf(stderr, "  And(%u children):", rn->multi.count);
                    for (uint32_t x = 0; x < rn->multi.count; x++)
                        fprintf(stderr, " %u", rn->multi.children[x]);
                    fprintf(stderr, "\n");
                } else if (rn->kind == N00B_RE_OR) {
                    fprintf(stderr, "  Or(%u children)\n", rn->multi.count);
                }
            }
        }
    }

    // has_anchors gates whether prefix acceleration / fixed-string overrides
    // can bypass the three-phase (reverse+forward) path.  If strip_anchors
    // modified the pattern (stripped lookbehinds, BEGIN, END), the reverse
    // scan must validate positions — so bypass acceleration.
    // Also force three-phase when pattern contains lookarounds — the accel
    // path's reverse_find_start doesn't handle lookaround nullability correctly.
    re->has_anchors = (fwd_root != root);
    if (root >= N00B_RE_SENTINEL_COUNT) {
        n00b_regex_node_t *root_n = n00b_regex_node_get(re->builder, root);
        if (root_n->contains_lookaround) {
            re->has_anchors = true;
        }
    }
    re->forward_dfa = n00b_regex_dfa_new(re->builder, re->minterms, fwd_root, false);

    // Build reverse DFA: .*reverse(R) — used for finding match starts
    uint32_t rev_node = reverse_node(re->builder, root);
    uint32_t rev_with_prefix = n00b_regex_mk_concat(re->builder,
                                                     N00B_RE_ID_DOTSTAR,
                                                     rev_node);
    re->reverse_dfa = n00b_regex_dfa_new(re->builder, re->minterms, rev_with_prefix, true);

    n00b_list_free(preds);

    // Compute match-time optimizations (prefix acceleration, length lookup, etc.)
    n00b_regex_find_optimizations(re, &re->optimizations);

    return n00b_result_ok(n00b_regex_t *, re);
}

// ============================================================================
// Full DFA compilation — bidirectional BFS (resharp Optimizations.fs)
// ============================================================================

// Forward declaration of dfa_step_end from deriv.c
extern uint32_t
n00b_regex_dfa_step_end(n00b_regex_dfa_t       *dfa,
                         uint32_t                state_id,
                         n00b_regex_minterm_id_t mt_id);

/**
 * Bidirectional BFS precompilation matching resharp's `attemptCompileFullDFA`.
 *
 * 1. REVERSE PHASE: Start from the reverse DFA start state. Compute End-location
 *    AND Center-location transitions for it, then BFS all Center transitions from
 *    discovered states.
 *
 * 2. FORWARD PHASE: BFS from the forward DFA start state, Center transitions only.
 *
 * Aborts if max state ID exceeds threshold.
 */
void
n00b_regex_compile(n00b_regex_t *re) _kargs {
    uint32_t max_states = N00B_RE_DFA_THRESHOLD;
}
{
    if (re->is_full_dfa) return;

    // Patterns with lookarounds create unbounded unique states (each
    // derivative step increments relative_to), so BFS compilation will hit
    // the state limit and corrupt the lazy DFA cache.  Skip for these.
    {
        n00b_regex_node_t *fwd_root = n00b_regex_node_get(re->builder,
                                                          re->forward_dfa->states.data[re->forward_dfa->start_state].node_id);
        if (fwd_root && fwd_root->contains_lookaround) return;
    }

    uint32_t threshold = max_states;
    uint16_t n_minterms = re->minterms->count;

    // --- REVERSE PHASE ---
    {
        n00b_regex_dfa_t *rdfa = re->reverse_dfa;
        uint32_t rev_start = rdfa->start_state;

        // Seed: compute End + Center transitions from reverse start state
        for (uint16_t mt = 0; mt < n_minterms; mt++) {
            n00b_regex_dfa_step_end(rdfa, rev_start, mt);
            n00b_regex_dfa_step(rdfa, rev_start, mt);
        }

        // BFS Center transitions
        uint32_t frontier = 0;
        while (frontier < rdfa->states.len && rdfa->states.len < threshold) {
            n00b_regex_dfa_state_t *st = &rdfa->states.data[frontier];
            if (st->node_id != N00B_RE_ID_NOTHING) {
                for (uint16_t mt = 0; mt < n_minterms; mt++) {
                    n00b_regex_dfa_step(rdfa, frontier, mt);
                    if (rdfa->states.len >= threshold) break;
                }
            }
            frontier++;
        }
    }

    // --- FORWARD PHASE ---
    {
        n00b_regex_dfa_t *fdfa = re->forward_dfa;
        uint32_t frontier = 0;

        while (frontier < fdfa->states.len && fdfa->states.len < threshold) {
            n00b_regex_dfa_state_t *st = &fdfa->states.data[frontier];
            if (st->node_id != N00B_RE_ID_NOTHING) {
                for (uint16_t mt = 0; mt < n_minterms; mt++) {
                    n00b_regex_dfa_step(fdfa, frontier, mt);
                    if (fdfa->states.len >= threshold) break;
                }
            }
            frontier++;
        }
    }

    // Mark as fully compiled if both DFAs completed within threshold
    // and neither hit the internal lazy state limit
    re->is_full_dfa = (re->forward_dfa->states.len < threshold
                        && re->reverse_dfa->states.len < threshold
                        && !re->forward_dfa->hit_state_limit
                        && !re->reverse_dfa->hit_state_limit);

    // Build flat transition tables for compiled DFAs
    if (re->is_full_dfa) {
        n00b_regex_dfa_t *dfas[2] = {re->forward_dfa, re->reverse_dfa};
        for (int d = 0; d < 2; d++) {
            n00b_regex_dfa_t *dfa = dfas[d];
            uint32_t n_states = (uint32_t)dfa->states.len;
            uint16_t n_mt = dfa->minterms->count;

            // Round n_mt up to next power of 2 for bit-shift indexing
            uint16_t n_mt_padded = n_mt;
            uint8_t  mt_log = 0;
            {
                uint16_t v = n_mt_padded;
                if (v && !(v & (v - 1))) {
                    // already power of 2
                } else {
                    v--;
                    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8;
                    v++;
                    n_mt_padded = v;
                }
                uint16_t tmp = n_mt_padded;
                while (tmp > 1) { mt_log++; tmp >>= 1; }
            }

            uint32_t table_size = n_states * (uint32_t)n_mt_padded;
            uint32_t *flat = n00b_alloc_array(uint32_t, table_size);
            // Zero-fill handles padding slots (map to dead state 0)
            // Bit 31 encodes "destination is always-nullable" for fast checking.
            for (uint32_t s = 0; s < n_states; s++) {
                n00b_regex_dfa_state_t *st = &dfa->states.data[s];
                uint32_t row = s << mt_log;
                for (uint16_t m = 0; m < n_mt; m++) {
                    int32_t t = st->transitions[m];
                    uint32_t next = (t >= 0) ? (uint32_t)t : 0;
                    // Pack always-nullable flag into bit 31
                    if (next < n_states
                        && (dfa->states.data[next].flags & N00B_RE_SF_ALWAYS_NULLABLE)) {
                        next |= 0x80000000u;
                    }
                    flat[row + m] = next;
                }
            }
            dfa->flat_transitions = flat;
            dfa->n_minterms_cached = n_mt_padded;
            dfa->mt_log2 = mt_log;
            dfa->is_flat = true;

            // Compute per-state skip maps now that all transitions are known.
            // For each state, find which ASCII bytes cause a transition to a
            // different, non-dead state. If the set is sparse (< 96 of 128),
            // mark can_skip = true and populate skip_map.
            const uint8_t *byte_lut = dfa->minterms->byte_lut;
            for (uint32_t s = 0; s < n_states; s++) {
                n00b_regex_dfa_state_t *st = &dfa->states.data[s];

                // Dead state and NOTHING: no skipping
                if (st->node_id == N00B_RE_ID_NOTHING || s == 0) {
                    st->can_skip = false;
                    continue;
                }

                // Build bitmap of ASCII bytes that cause "interesting" transitions
                // (not self-loop and not dead) and a dead_map for bytes → dead state.
                uint8_t map[16] = {0};
                uint8_t dmap[16] = {0};
                uint32_t interesting_count = 0;
                uint32_t row = s << mt_log;

                for (uint8_t b = 0; b < 128; b++) {
                    n00b_regex_minterm_id_t mt_id = (n00b_regex_minterm_id_t)byte_lut[b];
                    uint32_t next = flat[row | mt_id] & 0x7FFFFFFFu;
                    if (next == 0) {
                        dmap[b >> 3] |= (1u << (b & 7));
                    } else if (next != s) {
                        map[b >> 3] |= (1u << (b & 7));
                        interesting_count++;
                    }
                }

                // Copy dead_map always (used by skip loop even if can_skip is false).
                for (int i = 0; i < 16; i++) st->dead_map[i] = dmap[i];

                // Only enable skip if most bytes are uninteresting.
                // Threshold: skip is worthwhile when < 75% of ASCII bytes are interesting.
                if (interesting_count > 0 && interesting_count < 96) {
                    st->can_skip = true;
                    for (int i = 0; i < 16; i++) st->skip_map[i] = map[i];
                } else {
                    st->can_skip = false;
                }
            }
        }
    }
}

bool
n00b_regex_is_compiled(n00b_regex_t *re)
{
    return re->is_full_dfa;
}
