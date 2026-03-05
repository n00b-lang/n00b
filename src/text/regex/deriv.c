#include "text/regex/regex.h"
#include "core/alloc.h"
#include "core/hash.h"

#include <limits.h>

// ============================================================================
// Location kind for anchor-sensitive derivatives
// ============================================================================

typedef enum {
    LOC_BEGIN  = 0,
    LOC_CENTER = 1,
    LOC_END    = 2,
} loc_kind_t;

// ============================================================================
// Derivative cache key — pack node_id + minterm + location into 64 bits
// ============================================================================

static inline uint64_t
deriv_cache_key(uint32_t node_id, n00b_regex_charset_t minterm, loc_kind_t loc)
{
    // node_id in upper 30 bits, minterm in next 30 bits, loc in bottom 2 bits
    return ((uint64_t)node_id << 34) | ((uint64_t)minterm << 2) | (uint64_t)loc;
}

// ============================================================================
// Nullability (location-sensitive)
// ============================================================================

bool
n00b_regex_node_is_nullable(n00b_regex_builder_t *b, uint32_t node_id, int loc)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);

    if (!n->can_be_nullable) return false;
    if (n->is_always_nullable) return true;

    switch (n->kind) {
    case N00B_RE_SINGLETON:
        return false;
    case N00B_RE_OR:
        for (uint32_t i = 0; i < n->multi.count; i++) {
            if (n00b_regex_node_is_nullable(b, n->multi.children[i], loc)) {
                return true;
            }
        }
        return false;
    case N00B_RE_AND:
        for (uint32_t i = 0; i < n->multi.count; i++) {
            if (!n00b_regex_node_is_nullable(b, n->multi.children[i], loc)) {
                return false;
            }
        }
        return true;
    case N00B_RE_NOT:
        return !n00b_regex_node_is_nullable(b, n->not_.inner, loc);
    case N00B_RE_CONCAT:
        return n00b_regex_node_is_nullable(b, n->concat.head, loc)
            && n00b_regex_node_is_nullable(b, n->concat.tail, loc);
    case N00B_RE_LOOP:
        return n->loop.lo == 0 || n00b_regex_node_is_nullable(b, n->loop.body, loc);
    case N00B_RE_LOOKAROUND:
        return n00b_regex_node_is_nullable(b, n->lookaround.body, loc);
    case N00B_RE_BEGIN:
        return loc == LOC_BEGIN;
    case N00B_RE_END:
        return loc == LOC_END;
    }
    return false;
}

// ============================================================================
// Helper: check if a derivative result is "trivially done" for lookahead
// ============================================================================

static bool
is_anchor_surrounded_by_dotstar(n00b_regex_builder_t *b, uint32_t node_id)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);
    if (n->kind != N00B_RE_CONCAT) return false;
    if (n->concat.head != N00B_RE_ID_DOTSTAR) return false;

    n00b_regex_node_t *tail = n00b_regex_node_get(b, n->concat.tail);
    if (tail->kind == N00B_RE_BEGIN || tail->kind == N00B_RE_END) return true;
    if (tail->kind == N00B_RE_CONCAT) {
        n00b_regex_node_t *th = n00b_regex_node_get(b, tail->concat.head);
        if ((th->kind == N00B_RE_BEGIN || th->kind == N00B_RE_END)
            && tail->concat.tail == N00B_RE_ID_DOTSTAR) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Brzozowski derivative (location-sensitive)
// ============================================================================

static uint32_t
derivative(n00b_regex_builder_t *b, n00b_regex_dfa_t *dfa,
           uint32_t node_id, n00b_regex_charset_t minterm, loc_kind_t loc)
{
    // Check cache
    uint64_t cache_key = deriv_cache_key(node_id, minterm, loc);
    bool     found;
    uint32_t cached = n00b_dict_get(dfa->deriv_cache, cache_key, &found);
    if (found) return cached;

    n00b_regex_node_t *n = n00b_regex_node_get(b, node_id);
    uint32_t result;

    switch (n->kind) {
    case N00B_RE_SINGLETON:
        if (n00b_regex_charset_intersects_minterm(b->solver, n->singleton.set, minterm)) {
            result = N00B_RE_ID_EPSILON;
        }
        else {
            result = N00B_RE_ID_NOTHING;
        }
        break;

    case N00B_RE_CONCAT: {
        uint32_t dh = derivative(b, dfa, n->concat.head, minterm, loc);
        uint32_t dh_concat = n00b_regex_mk_concat(b, dh, n->concat.tail);

        bool head_nullable = n00b_regex_node_is_nullable(b, n->concat.head, loc);

        // Also treat head as nullable if its derivative is a lookaround
        // with pending nullables — the assertion just fired, so the tail
        // should also be consumed at this step.
        if (!head_nullable && dh >= N00B_RE_SENTINEL_COUNT) {
            n00b_regex_node_t *dh_node = n00b_regex_node_get(b, dh);
            if (dh_node->kind == N00B_RE_LOOKAROUND
                && !dh_node->lookaround.look_back
                && dh_node->lookaround.n_pending > 0) {
                head_nullable = true;
            }
        }

        if (head_nullable) {
            uint32_t dt = derivative(b, dfa, n->concat.tail, minterm, loc);
            if (dt == N00B_RE_ID_NOTHING) {
                result = dh_concat;
            }
            else if (dh_concat == N00B_RE_ID_NOTHING) {
                result = dt;
            }
            else {
                result = n00b_regex_mk_or2(b, dh_concat, dt);
            }
        }
        else {
            result = dh_concat;
        }
        break;
    }

    case N00B_RE_OR: {
        uint32_t *buf   = n00b_alloc_array(uint32_t, n->multi.count);
        uint32_t  count = 0;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            uint32_t d = derivative(b, dfa, n->multi.children[i], minterm, loc);
            if (d != N00B_RE_ID_NOTHING) {
                buf[count++] = d;
            }
        }
        if (count == 0) {
            result = N00B_RE_ID_NOTHING;
        }
        else if (count == 1) {
            result = buf[0];
        }
        else {
            // Sort children before calling mk_or for canonical form (Phase 9)
            for (uint32_t i = 1; i < count; i++) {
                uint32_t key_val = buf[i];
                uint32_t j = i;
                while (j > 0 && buf[j-1] > key_val) {
                    buf[j] = buf[j-1];
                    j--;
                }
                buf[j] = key_val;
            }
            result = n00b_regex_mk_or(b, buf, count);
        }
        n00b_free(buf);
        break;
    }

    case N00B_RE_AND: {
        uint32_t *buf   = n00b_alloc_array(uint32_t, n->multi.count);
        uint32_t  count = 0;
        bool      has_nothing = false;
        for (uint32_t i = 0; i < n->multi.count; i++) {
            uint32_t d = derivative(b, dfa, n->multi.children[i], minterm, loc);
            if (d == N00B_RE_ID_NOTHING) {
                has_nothing = true;
                break;
            }
            if (d != N00B_RE_ID_DOTSTAR) {
                buf[count++] = d;
            }
        }
        if (has_nothing) {
            result = N00B_RE_ID_NOTHING;
        }
        else if (count == 0) {
            result = N00B_RE_ID_DOTSTAR;
        }
        else if (count == 1) {
            result = buf[0];
        }
        else {
            // Sort children before calling mk_and for canonical form (Phase 9)
            for (uint32_t i = 1; i < count; i++) {
                uint32_t key_val = buf[i];
                uint32_t j = i;
                while (j > 0 && buf[j-1] > key_val) {
                    buf[j] = buf[j-1];
                    j--;
                }
                buf[j] = key_val;
            }
            result = n00b_regex_mk_and(b, buf, count);
        }
        n00b_free(buf);
        break;
    }

    case N00B_RE_NOT: {
        auto not_r = n00b_regex_mk_not(b, derivative(b, dfa, n->not_.inner, minterm, loc));
        result = n00b_result_is_ok(not_r) ? n00b_result_get(not_r) : N00B_RE_ID_NOTHING;
        break;
    }

    case N00B_RE_LOOP: {
        int32_t new_lo = n->loop.lo > 0 ? n->loop.lo - 1 : 0;
        int32_t new_hi = n->loop.hi == INT32_MAX ? INT32_MAX : n->loop.hi - 1;
        uint32_t decr = n00b_regex_mk_loop(b, n->loop.body, new_lo, new_hi);
        uint32_t dr = derivative(b, dfa, n->loop.body, minterm, loc);
        result = n00b_regex_mk_concat(b, dr, decr);
        break;
    }

    case N00B_RE_LOOKAROUND: {
        uint32_t body_id    = n->lookaround.body;
        bool     look_back  = n->lookaround.look_back;
        int32_t  rel        = n->lookaround.relative_to;
        int32_t *pending    = n->lookaround.pending_nullable_pos;
        uint16_t n_pending  = n->lookaround.n_pending;

        if (look_back) {
            // Lookbehind: strip leading .* prefix from body before deriving
            n00b_regex_node_t *body_node = n00b_regex_node_get(b, body_id);
            uint32_t to_derive = body_id;
            if (body_node->kind == N00B_RE_CONCAT
                && body_node->concat.head == N00B_RE_ID_DOTSTAR) {
                to_derive = body_node->concat.tail;
            }
            uint32_t dr = derivative(b, dfa, to_derive, minterm, loc);
            result = n00b_regex_mk_lookaround(b, dr, true, 0, nullptr, 0);
        }
        else {
            // Lookahead derivative — resharp Algorithm.fs lines 180-230
            uint32_t der_body = derivative(b, dfa, body_id, minterm, loc);

            if (n_pending == 0) {
                // No pending nullables yet — check if body derivative is nullable
                if (n00b_regex_node_is_nullable(b, der_body, loc)) {
                    // Body became nullable: initialize pending with position 0
                    int32_t zero = 0;
                    result = n00b_regex_mk_lookaround(b, der_body, false,
                                                       rel + 1, &zero, 1);
                }
                else if (der_body == N00B_RE_ID_NOTHING) {
                    result = N00B_RE_ID_NOTHING;
                }
                else {
                    // Check for .*anchor.* pattern
                    n00b_regex_node_t *dr_node = n00b_regex_node_get(b, der_body);
                    if (dr_node->kind == N00B_RE_CONCAT
                        && dr_node->concat.head == N00B_RE_ID_DOTSTAR) {
                        uint32_t tail_id = dr_node->concat.tail;
                        n00b_regex_node_t *tail_node = n00b_regex_node_get(b, tail_id);
                        if (tail_node->kind == N00B_RE_BEGIN
                            || tail_node->kind == N00B_RE_END
                            || is_anchor_surrounded_by_dotstar(b, tail_id)) {
                            int32_t zero2 = 0;
                            result = n00b_regex_mk_lookaround(b, N00B_RE_ID_EPSILON,
                                                               false, rel + 1, &zero2, 1);
                        }
                        else {
                            result = n00b_regex_mk_lookaround(b, der_body, false,
                                                               rel + 1, nullptr, 0);
                        }
                    }
                    else {
                        result = n00b_regex_mk_lookaround(b, der_body, false,
                                                           rel + 1, nullptr, 0);
                    }
                }
            }
            else {
                // Already have pending nullables — propagate
                result = n00b_regex_mk_lookaround(b, der_body, false,
                                                   rel + 1, pending, n_pending);
            }
        }
        break;
    }

    case N00B_RE_BEGIN:
    case N00B_RE_END:
        result = N00B_RE_ID_NOTHING;
        break;

    default:
        result = N00B_RE_ID_NOTHING;
        break;
    }

    n00b_dict_put(dfa->deriv_cache, cache_key, result);
    return result;
}

// ============================================================================
// Helpers for pending nullable extraction from node info
// ============================================================================

/**
 * Collect pending nullable positions from lookaround nodes within a node tree.
 * Uses an iterative work stack to avoid infinite recursion on derivative nodes.
 */
static void
collect_pending_from_node(n00b_regex_builder_t *b, uint32_t node_id,
                          n00b_regex_pending_range_t **out_ranges,
                          uint16_t *out_count, uint16_t max_count)
{
    // Iterative work stack to avoid recursion on derivative node graphs
    uint32_t stack[256];
    int      sp = 0;

    if (node_id >= N00B_RE_SENTINEL_COUNT) {
        stack[sp++] = node_id;
    }

    // Track visited nodes to prevent cycles
    uint32_t visited[256];
    int      n_visited = 0;

    while (sp > 0 && *out_count < max_count) {
        uint32_t cur = stack[--sp];

        // Check if already visited
        bool seen = false;
        for (int i = 0; i < n_visited; i++) {
            if (visited[i] == cur) { seen = true; break; }
        }
        if (seen) continue;
        if (n_visited < 256) visited[n_visited++] = cur;

        if (cur < N00B_RE_SENTINEL_COUNT) continue;
        n00b_regex_node_t *n = n00b_regex_node_get(b, cur);

        switch (n->kind) {
        case N00B_RE_LOOKAROUND:
            if (n->lookaround.n_pending > 0 && n->lookaround.pending_nullable_pos) {
                // Resharp's refSetAddAll: add relative_to to each pending position.
                // The node stores raw positions; the offset converts them to
                // the number of input characters consumed since the lookaround
                // started being tracked.
                int32_t offset = n->lookaround.relative_to;
                for (uint16_t i = 0; i < n->lookaround.n_pending && *out_count < max_count; i++) {
                    int32_t pos = n->lookaround.pending_nullable_pos[i] + offset;
                    (*out_ranges)[*out_count] = (n00b_regex_pending_range_t){
                        .lo = (uint16_t)pos, .hi = (uint16_t)pos,
                    };
                    (*out_count)++;
                }
            }
            if (sp < 256) stack[sp++] = n->lookaround.body;
            break;
        case N00B_RE_CONCAT:
            if (sp < 255) {
                stack[sp++] = n->concat.tail;
                stack[sp++] = n->concat.head;
            }
            break;
        case N00B_RE_OR:
        case N00B_RE_AND:
            for (uint32_t i = 0; i < n->multi.count && sp < 256; i++) {
                stack[sp++] = n->multi.children[i];
            }
            break;
        case N00B_RE_NOT:
            if (sp < 256) stack[sp++] = n->not_.inner;
            break;
        case N00B_RE_LOOP:
            if (sp < 256) stack[sp++] = n->loop.body;
            break;
        default:
            break;
        }
    }
}

// ============================================================================
// DFA state management
// ============================================================================

// Safety limit for lazy DFA state creation during matching.
// Lookaround derivatives can produce unique nodes indefinitely
// (each step increments relative_to). Cap to prevent runaway.
#define DFA_LAZY_STATE_LIMIT 5000

static uint32_t
dfa_get_or_create_state(n00b_regex_dfa_t *dfa, uint32_t node_id, bool is_initial)
{
    bool     found;
    uint32_t cached = n00b_dict_get(dfa->node_to_state, node_id, &found);
    if (found) return cached;

    uint32_t state_id = (uint32_t)dfa->states.len;
    if (state_id >= DFA_LAZY_STATE_LIMIT) {
        // Too many states — return dead state to prevent runaway
        dfa->hit_state_limit = true;
        return 0;
    }
    n00b_regex_builder_t *b = dfa->builder;

    // Compute state flags (matches resharp's _getOrCreateState)
    n00b_regex_state_flags_t flags = N00B_RE_SF_NONE;
    n00b_regex_node_t *node = (node_id < b->nodes.len) ? n00b_regex_node_get(b, node_id) : nullptr;

    bool always_nullable = (node && node->is_always_nullable);
    bool can_be_nullable = (node && node->can_be_nullable);
    bool depends_on_anchor = (node && node->depends_on_anchor);

    if (always_nullable) flags |= N00B_RE_SF_ALWAYS_NULLABLE;
    if (can_be_nullable && depends_on_anchor) flags |= N00B_RE_SF_ANCHOR_NULLABLE;
    if (is_initial) flags |= N00B_RE_SF_INITIAL;

    if (can_be_nullable) {
        if (n00b_regex_node_is_nullable(b, node_id, LOC_END)) {
            flags |= N00B_RE_SF_END_NULLABLE;
        }
        if (n00b_regex_node_is_nullable(b, node_id, LOC_BEGIN)) {
            flags |= N00B_RE_SF_BEGIN_NULLABLE;
        }
    }

    // Compute pending nullable positions
    n00b_regex_pending_range_t *pending_ranges = nullptr;
    uint16_t n_pending = 0;
    int32_t  min_pending = 0;

    if (node && node->contains_lookaround && can_be_nullable && !is_initial) {
        uint16_t max_pending = 64;
        pending_ranges = n00b_alloc_array(n00b_regex_pending_range_t, max_pending);
        collect_pending_from_node(b, node_id, &pending_ranges, &n_pending, max_pending);

        if (n_pending > 0) {
            flags |= N00B_RE_SF_PENDING_NULLABLE;

            // Check if current node is also directly nullable (not via pending)
            bool is_curr_nullable = false;
            if (can_be_nullable && node) {
                if (node->kind == N00B_RE_OR) {
                    for (uint32_t i = 0; i < node->multi.count; i++) {
                        n00b_regex_node_t *ch = n00b_regex_node_get(b, node->multi.children[i]);
                        if (ch->can_be_nullable && !ch->contains_lookaround) {
                            is_curr_nullable = true;
                            break;
                        }
                    }
                }
                else if (node->kind == N00B_RE_LOOP && node->loop.lo == 0) {
                    is_curr_nullable = true;
                }
            }

            // Add (0,0) if directly nullable and not already present
            if (is_curr_nullable) {
                bool has_zero = false;
                for (uint16_t i = 0; i < n_pending; i++) {
                    if (pending_ranges[i].lo == 0) { has_zero = true; break; }
                }
                if (!has_zero && n_pending < max_pending) {
                    pending_ranges[n_pending++] = (n00b_regex_pending_range_t){.lo = 0, .hi = 0};
                }
            }

            // Sort by lo
            for (uint16_t i = 1; i < n_pending; i++) {
                for (uint16_t j = i; j > 0 && pending_ranges[j].lo < pending_ranges[j-1].lo; j--) {
                    n00b_regex_pending_range_t tmp = pending_ranges[j];
                    pending_ranges[j] = pending_ranges[j-1];
                    pending_ranges[j-1] = tmp;
                }
            }

            min_pending = (int32_t)pending_ranges[0].lo;
        }
        else {
            n00b_free(pending_ranges);
            pending_ranges = nullptr;
        }
    }

    // Compute NullKind
    n00b_regex_null_kind_t null_kind;
    if ((flags & N00B_RE_SF_PENDING_NULLABLE) && n_pending > 0) {
        // Has pending nullable positions from lookarounds — even if not always-nullable
        if ((flags & N00B_RE_SF_ALWAYS_NULLABLE) && !(n_pending == 1 && pending_ranges[0].lo == 0)) {
            // Always nullable AND has pending: use PENDING to cover both
            null_kind = N00B_RE_NULL_PENDING;
        }
        else if (n_pending == 1 && pending_ranges[0].lo == 1 && pending_ranges[0].hi == 1) {
            null_kind = N00B_RE_NULL_PREV;
        }
        else {
            null_kind = N00B_RE_NULL_PENDING;
        }
    }
    else if (!(flags & N00B_RE_SF_ALWAYS_NULLABLE)) {
        null_kind = N00B_RE_NULL_NOT;
    }
    else {
        null_kind = N00B_RE_NULL_CURRENT;
    }

    n00b_regex_dfa_state_t state = {
        .node_id        = node_id,
        .flags          = flags,
        .null_kind      = null_kind,
        .transitions    = n00b_alloc_array(int32_t, dfa->minterms->count),
        .n_transitions  = dfa->minterms->count,
        .pending_ranges = pending_ranges,
        .n_pending      = n_pending,
        .min_pending    = min_pending,
        .skip_map       = {0},
        .can_skip       = false,
    };

    for (uint16_t i = 0; i < state.n_transitions; i++) {
        state.transitions[i] = -1;
    }

    n00b_list_push(dfa->states, state);
    n00b_dict_put(dfa->node_to_state, node_id, state_id);
    return state_id;
}

// ============================================================================
// DFA step — location-sensitive, with anchor transition support
// ============================================================================

/**
 * Step the DFA on a minterm at a given location.
 * LOC_CENTER uses the standard transition table.
 * LOC_END for anchor-dependent nodes uses a separate anchor_transitions cache.
 */
static uint32_t
dfa_step_at_loc(n00b_regex_dfa_t *dfa, uint32_t state_id,
                n00b_regex_minterm_id_t mt_id, loc_kind_t loc)
{
    n00b_regex_dfa_state_t *st = &dfa->states.data[state_id];

    // For End location with anchor dependencies, use separate cache.
    // Must check depends_on_anchor (not just ANCHOR_NULLABLE) because
    // the End-location derivative differs from Center even for non-nullable
    // states (e.g., _.*·END·\d·BEGIN is not nullable but its End derivative
    // differs from its Center derivative due to END being nullable at LOC_END).
    bool has_anchor_dep = (st->flags & N00B_RE_SF_ANCHOR_NULLABLE) != 0;
    if (!has_anchor_dep && st->node_id >= N00B_RE_SENTINEL_COUNT) {
        n00b_regex_node_t *node = n00b_regex_node_get(dfa->builder, st->node_id);
        has_anchor_dep = node->depends_on_anchor;
    }
    if (loc == LOC_END && has_anchor_dep) {
        uint32_t idx = state_id * dfa->minterms->count + mt_id;

        // Ensure anchor_transitions is large enough
        n00b_data_write_lock(dfa->lock);

        if (idx >= dfa->anchor_trans_size) {
            uint32_t new_size = (idx + 1) * 2;
            int32_t *new_arr = n00b_alloc_array(int32_t, new_size);
            for (uint32_t i = 0; i < new_size; i++) new_arr[i] = -1;
            if (dfa->anchor_transitions) {
                for (uint32_t i = 0; i < dfa->anchor_trans_size; i++) {
                    new_arr[i] = dfa->anchor_transitions[i];
                }
                n00b_free(dfa->anchor_transitions);
            }
            dfa->anchor_transitions = new_arr;
            dfa->anchor_trans_size = new_size;
        }

        if (dfa->anchor_transitions[idx] >= 0) {
            uint32_t r = (uint32_t)dfa->anchor_transitions[idx];
            n00b_data_unlock(dfa->lock);
            return r;
        }

        n00b_regex_charset_t minterm = dfa->minterms->minterms[mt_id];
        uint32_t node_id = dfa->states.data[state_id].node_id;
        uint32_t der_node = derivative(dfa->builder, dfa, node_id, minterm, LOC_END);
        uint32_t next_state = dfa_get_or_create_state(dfa, der_node, false);

        dfa->anchor_transitions[idx] = (int32_t)next_state;
        n00b_data_unlock(dfa->lock);
        return next_state;
    }

    // Standard Center-location path
    int32_t *trans = st->transitions;
    if (trans[mt_id] >= 0) {
        return (uint32_t)trans[mt_id];
    }

    n00b_data_write_lock(dfa->lock);

    trans = dfa->states.data[state_id].transitions;
    if (trans[mt_id] >= 0) {
        n00b_data_unlock(dfa->lock);
        return (uint32_t)trans[mt_id];
    }

    n00b_regex_charset_t minterm = dfa->minterms->minterms[mt_id];
    uint32_t node_id = dfa->states.data[state_id].node_id;
    uint32_t der_node = derivative(dfa->builder, dfa, node_id, minterm, loc);
    uint32_t next_state = dfa_get_or_create_state(dfa, der_node, false);

    dfa->states.data[state_id].transitions[mt_id] = (int32_t)next_state;
    n00b_data_unlock(dfa->lock);
    return next_state;
}

/**
 * Public DFA step — always Center location (used by match.c hot loops).
 */
uint32_t
n00b_regex_dfa_step(n00b_regex_dfa_t *dfa, uint32_t state_id,
                    n00b_regex_minterm_id_t mt_id)
{
    return dfa_step_at_loc(dfa, state_id, mt_id, LOC_CENTER);
}

/**
 * DFA step at End location — for anchor-sensitive transitions at input boundary.
 */
uint32_t
n00b_regex_dfa_step_end(n00b_regex_dfa_t *dfa, uint32_t state_id,
                        n00b_regex_minterm_id_t mt_id)
{
    return dfa_step_at_loc(dfa, state_id, mt_id, LOC_END);
}

/**
 * Non-initial derivative — Center location, node-level (used by optimize.c).
 * Computes the Brzozowski derivative of a regex node w.r.t. a minterm charset.
 */
uint32_t
n00b_regex_derive_non_initial(n00b_regex_builder_t  *b,
                               n00b_regex_dfa_t      *dfa,
                               uint32_t               node_id,
                               n00b_regex_charset_t   minterm)
{
    return derivative(b, dfa, node_id, minterm, LOC_CENTER);
}

// ============================================================================
// DFA construction
// ============================================================================

n00b_regex_dfa_t *
n00b_regex_dfa_new(n00b_regex_builder_t       *builder,
                   n00b_regex_minterm_table_t *minterms,
                   uint32_t                    root_node,
                   bool                        is_initial)
{
    n00b_regex_dfa_t *dfa = n00b_alloc(n00b_regex_dfa_t);

    dfa->states        = n00b_list_new_cap_private(n00b_regex_dfa_state_t, 64);
    dfa->node_to_state = n00b_alloc(n00b_dict_t(uint32_t, uint32_t));
    n00b_dict_init(dfa->node_to_state, .hash = n00b_hash_word, .skip_obj_hash = true);
    dfa->builder       = builder;
    dfa->minterms      = minterms;
    dfa->deriv_cache   = n00b_alloc(n00b_dict_t(uint64_t, uint32_t));
    n00b_dict_init(dfa->deriv_cache, .hash = n00b_hash_word, .skip_obj_hash = true);
    dfa->lock          = n00b_data_lock_new();
    dfa->anchor_transitions = nullptr;
    dfa->anchor_trans_size  = 0;
    dfa->flat_transitions   = nullptr;
    dfa->n_minterms_cached  = 0;
    dfa->mt_log2            = 0;
    dfa->is_flat            = false;

    // Create the dead state first (state 0 is always dead)
    dfa_get_or_create_state(dfa, N00B_RE_ID_NOTHING, false);

    // Create start state
    dfa->start_state = dfa_get_or_create_state(dfa, root_node, is_initial);

    return dfa;
}
