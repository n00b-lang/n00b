#include "text/regex/regex.h"
#include "text/regex/optimize.h"
#include "core/alloc.h"
#include "core/string.h"
#include "core/buffer.h"
#include "text/unicode/encoding.h"

#include <limits.h>
#include <string.h>

// Forward declarations from deriv.c
extern uint32_t
n00b_regex_dfa_step(n00b_regex_dfa_t *dfa, uint32_t state_id,
                    n00b_regex_minterm_id_t mt_id);

extern uint32_t
n00b_regex_dfa_step_end(n00b_regex_dfa_t *dfa, uint32_t state_id,
                        n00b_regex_minterm_id_t mt_id);

extern bool
n00b_regex_node_is_nullable(n00b_regex_builder_t *b, uint32_t node_id, int loc);

// ============================================================================
// Resharp three-phase matching algorithm
//
// Phase 1 (HandleInputEnd): Check end-of-input anchor conditions, possibly
//         take one End-location transition backward from last codepoint.
//
// Phase 2 (collect): Scan right-to-left with reverse DFA (.*reverse(R)),
//         collecting candidate match-start byte positions into an accumulator.
//         NullKind and pending nullable positions control which positions are
//         added and at what offset.
//
// Phase 3 (match_ends): Iterate collected starts from leftmost to rightmost,
//         run forward DFA (raw R) from each start to find longest match end.
//         Skip starts that overlap a previous match.
// ============================================================================

// Location kind constants (must match deriv.c)
#define LOC_BEGIN  0
#define LOC_CENTER 1
#define LOC_END    2

// Get transition_state from an accelerator (0 if not applicable)
static inline uint32_t
accel_transition_state(const n00b_regex_accelerator_t *accel)
{
    switch (accel->kind) {
    case N00B_RE_ACCEL_FIXED_PREFIX:
    case N00B_RE_ACCEL_FIXED_PREFIX_CI:
        return accel->fixed_prefix.transition_state;
    case N00B_RE_ACCEL_MINTERM_PREFIX:
        return accel->minterm_prefix.transition_state;
    case N00B_RE_ACCEL_SINGLE_MINTERM:
        return accel->single_minterm.transition_state;
    default:
        return 0;
    }
}

// Get prefix length (in codepoints) from an accelerator
static inline uint32_t
accel_prefix_len(const n00b_regex_accelerator_t *accel)
{
    switch (accel->kind) {
    case N00B_RE_ACCEL_FIXED_PREFIX:
    case N00B_RE_ACCEL_FIXED_PREFIX_CI:
        return accel->fixed_prefix.len;
    case N00B_RE_ACCEL_MINTERM_PREFIX:
        return accel->minterm_prefix.len;
    case N00B_RE_ACCEL_SINGLE_MINTERM:
        return 1;
    default:
        return 0;
    }
}

// Forward declarations for optimization helpers
static int64_t accel_find_prefix(const char *data, uint32_t data_len,
                                  const n00b_regex_accelerator_t *accel,
                                  n00b_regex_minterm_table_t *mt,
                                  n00b_regex_solver_t *solver,
                                  uint32_t start);
static int64_t try_length_lookup(const n00b_regex_len_lookup_t *len,
                                  n00b_regex_dfa_t *fwd_dfa,
                                  const char *data, uint32_t data_len,
                                  n00b_regex_solver_t *solver,
                                  int64_t match_start);
static int64_t forward_longest_from(n00b_regex_dfa_t *fwd_dfa,
                                     const char *data, uint32_t data_len,
                                     uint32_t start, uint32_t init_state);

// ============================================================================
// Accumulator — growable int64_t array for collected positions
// ============================================================================

typedef struct {
    int64_t  *data;
    uint32_t  len;
    uint32_t  cap;
} acc_t;

static acc_t
acc_new(uint32_t cap)
{
    return (acc_t){
        .data = n00b_alloc_array(int64_t, cap),
        .len  = 0,
        .cap  = cap,
    };
}

static void
acc_push(acc_t *a, int64_t val)
{
    if (a->len >= a->cap) {
        uint32_t new_cap = a->cap * 2;
        int64_t *new_data = n00b_alloc_array(int64_t, new_cap);
        for (uint32_t i = 0; i < a->len; i++) new_data[i] = a->data[i];
        n00b_free(a->data);
        a->data = new_data;
        a->cap = new_cap;
    }
    a->data[a->len++] = val;
}

static void
acc_free(acc_t *a)
{
    if (a->data) n00b_free(a->data);
    a->data = nullptr;
    a->len = a->cap = 0;
}

// ============================================================================
// Helper: is a state nullable (by NullKind)?
// ============================================================================

static inline bool
state_is_null(n00b_regex_dfa_state_t *st)
{
    return st->null_kind != N00B_RE_NULL_NOT;
}

// ============================================================================
// Helper: add nullable positions for a state to accumulator (resharp's setNullFull)
// ============================================================================

static void
add_null_positions(n00b_regex_dfa_state_t *st, acc_t *acc, int64_t pos)
{
    switch (st->null_kind) {
    case N00B_RE_NULL_CURRENT:
        acc_push(acc, pos);
        break;
    case N00B_RE_NULL_PREV:
        acc_push(acc, pos + 1);
        break;
    case N00B_RE_NULL_BOTH_01:
        acc_push(acc, pos + 1);
        acc_push(acc, pos);
        break;
    case N00B_RE_NULL_PENDING:
        if (st->pending_ranges && st->n_pending > 0) {
            // Iterate ranges in reverse order (highest first)
            for (int32_t i = (int32_t)st->n_pending - 1; i >= 0; i--) {
                for (int32_t j = (int32_t)st->pending_ranges[i].hi;
                     j >= (int32_t)st->pending_ranges[i].lo; j--) {
                    acc_push(acc, j + pos);
                }
            }
        }
        else {
            acc_push(acc, pos);
        }
        break;
    case N00B_RE_NULL_NOT:
        break;
    }
}

// ============================================================================
// Helper: add pending positions for reverse scan (resharp's AddPendingRev)
// ============================================================================

static void
add_pending_rev(n00b_regex_dfa_state_t *st, acc_t *acc, int64_t pos)
{
    if (!st->pending_ranges || st->n_pending == 0) return;
    for (int32_t i = (int32_t)st->n_pending - 1; i >= 0; i--) {
        for (int32_t j = (int32_t)st->pending_ranges[i].hi;
             j >= (int32_t)st->pending_ranges[i].lo; j--) {
            acc_push(acc, j + pos);
        }
    }
}

// ============================================================================
// UTF-8 backward decode helper
// ============================================================================

typedef struct {
    n00b_codepoint_t cp;
    int64_t          prev_pos; // byte offset of start of this codepoint
} backward_decode_t;

static backward_decode_t
utf8_decode_backward(const char *data, int64_t pos)
{
    int64_t prev = pos - 1;
    while (prev > 0 && ((uint8_t)data[prev] & 0xC0) == 0x80) {
        prev--;
    }
    uint32_t dp = (uint32_t)prev;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)pos, &dp);
    return (backward_decode_t){.cp = (n00b_codepoint_t)(cp >= 0 ? cp : 0xFFFD), .prev_pos = prev};
}

// ============================================================================
// Phase 1: HandleInputEnd — check end-of-input conditions
//
// Returns the start position for the reverse scan (usually input.Length - 1
// or input.Length if no backward step needed).
// ============================================================================

static int64_t
handle_input_end(n00b_regex_dfa_t *rev_dfa,
                 const char *data, uint32_t data_len,
                 uint32_t *state_id, acc_t *acc)
{
    n00b_regex_dfa_state_t *st = &rev_dfa->states.data[*state_id];
    int64_t pos = (int64_t)data_len;

    // Check if nullable at End location
    bool nullable_at_end = (st->flags & N00B_RE_SF_END_NULLABLE) != 0
                        || (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) != 0;
    bool depends_on_anchor = (st->flags & N00B_RE_SF_ANCHOR_NULLABLE) != 0;

    if (!nullable_at_end && !depends_on_anchor) {
        // Not nullable at end and no anchor dependency — start reverse scan from end
        return pos;
    }

    if (nullable_at_end) {
        // State is nullable at end — add position(s)
        if (st->flags & N00B_RE_SF_PENDING_NULLABLE) {
            add_pending_rev(st, acc, pos);
        }
        else {
            acc_push(acc, pos);
        }
    }

    // Take one End-location transition backward from the last codepoint
    if (pos > 0) {
        backward_decode_t bd = utf8_decode_backward(data, pos);
        n00b_regex_minterm_id_t mt = n00b_regex_minterm_classify(
            rev_dfa->minterms, bd.cp);
        *state_id = n00b_regex_dfa_step_end(rev_dfa, *state_id, mt);
        pos = bd.prev_pos;

        // Check new state
        st = &rev_dfa->states.data[*state_id];
        if (state_is_null(st)) {
            add_null_positions(st, acc, pos);
        }
    }

    return pos;
}

// ============================================================================
// Phase 1.5: HandleInputStart — check begin-of-input conditions
// ============================================================================

static void
handle_input_start(n00b_regex_dfa_t *rev_dfa,
                   uint32_t state_id, acc_t *acc)
{
    n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state_id];

    // Check if nullable at Begin location
    bool nullable_at_begin = (st->flags & N00B_RE_SF_BEGIN_NULLABLE) != 0
                          || (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) != 0;

    if (!nullable_at_begin) return;

    // Avoid duplicate: don't add 0 if last entry is already 0
    if (acc->len > 0 && acc->data[acc->len - 1] == 0) return;

    // Add position 0 with NullKind offset
    switch (st->null_kind) {
    case N00B_RE_NULL_CURRENT:
        acc_push(acc, 0);
        break;
    case N00B_RE_NULL_PREV:
        acc_push(acc, 1);
        break;
    case N00B_RE_NULL_BOTH_01:
        acc_push(acc, 1);
        acc_push(acc, 0);
        break;
    case N00B_RE_NULL_PENDING:
        if (st->pending_ranges && st->n_pending > 0) {
            for (int32_t i = (int32_t)st->n_pending - 1; i >= 0; i--) {
                for (int32_t j = (int32_t)st->pending_ranges[i].hi;
                     j >= (int32_t)st->pending_ranges[i].lo; j--) {
                    acc_push(acc, j);
                }
            }
        }
        else {
            acc_push(acc, 0);
        }
        break;
    default:
        break;
    }
}

// ============================================================================
// Phase 2: Reverse scan — collect candidate match starts (right-to-left)
// ============================================================================

static void
collect_starts(n00b_regex_dfa_t *rev_dfa,
               const char *data, uint32_t data_len,
               acc_t *acc, uint32_t init_state, int64_t start_pos)
{
    uint32_t state = init_state;
    int64_t  pos = start_pos;

    // Fast path: flat transition table + bit-shift indexing + ASCII fast path
    if (rev_dfa->is_flat) {
        const uint32_t *flat = rev_dfa->flat_transitions;
        uint8_t mt_log = rev_dfa->mt_log2;
        const uint8_t *byte_lut = rev_dfa->minterms->byte_lut;

        while (pos > 0) {
            if (state == 0) break; // Dead state

            // ASCII fast path: avoid utf8_decode_backward + minterm_classify
            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(rev_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = flat[(state << mt_log) | mt] & 0x7FFFFFFFu;

            n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
            if (state_is_null(st)) {
                add_null_positions(st, acc, prev_pos);
            }
            pos = prev_pos;
        }
    } else {
        while (pos > 0) {
            n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
            if (st->node_id == N00B_RE_ID_NOTHING) break;

            // ASCII fast path for reverse scan
            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)rev_dfa->minterms->byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(rev_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = n00b_regex_dfa_step(rev_dfa, state, mt);

            st = &rev_dfa->states.data[state];
            if (state_is_null(st)) {
                add_null_positions(st, acc, prev_pos);
            }
            pos = prev_pos;
        }
    }

    // Phase 1.5: check begin-of-input conditions
    handle_input_start(rev_dfa, state, acc);
}

// ============================================================================
// Targeted reverse scan — find leftmost match start from a given end position
//
// Used when prefix acceleration found a suffix position but the regex has a
// nullable prefix (e.g., .*Holmes). Scans backward from `end_pos` to
// `lower_bound` using the reverse DFA to find the leftmost nullable position.
// ============================================================================

static int64_t
reverse_find_start(n00b_regex_t *re, const char *data, uint32_t data_len,
                   int64_t end_pos, int64_t lower_bound)
{
    n00b_regex_dfa_t *rev_dfa = re->reverse_dfa;

    // Run HandleInputEnd-equivalent for this sub-range:
    // Start the reverse DFA from its start state
    uint32_t state = rev_dfa->start_state;
    int64_t  pos   = end_pos;
    int64_t  leftmost = -1;

    // Check initial state at end position
    {
        n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
        bool nullable_at_end = (st->flags & N00B_RE_SF_END_NULLABLE) != 0
                            || (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) != 0;
        if (nullable_at_end) {
            leftmost = pos;
        }

        // Take one End-location transition
        if (pos > lower_bound) {
            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)rev_dfa->minterms->byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(rev_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = n00b_regex_dfa_step_end(rev_dfa, state, mt);
            pos = prev_pos;

            n00b_regex_dfa_state_t *st2 = &rev_dfa->states.data[state];
            if (state_is_null(st2)) {
                leftmost = pos;
            }
        }
    }

    // Reverse scan using flat table if available
    if (rev_dfa->is_flat) {
        const uint32_t *flat = rev_dfa->flat_transitions;
        uint8_t mt_log = rev_dfa->mt_log2;
        const uint8_t *byte_lut = rev_dfa->minterms->byte_lut;

        while (pos > lower_bound) {
            if (state == 0) break;

            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(rev_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = flat[(state << mt_log) | mt] & 0x7FFFFFFFu;

            n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
            if (state_is_null(st)) {
                leftmost = prev_pos;
            }
            pos = prev_pos;
        }
    } else {
        while (pos > lower_bound) {
            n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
            if (st->node_id == N00B_RE_ID_NOTHING) break;

            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)rev_dfa->minterms->byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(rev_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = n00b_regex_dfa_step(rev_dfa, state, mt);

            st = &rev_dfa->states.data[state];
            if (state_is_null(st)) {
                leftmost = prev_pos;
            }
            pos = prev_pos;
        }
    }

    // Check begin-of-input
    if (pos <= lower_bound && lower_bound == 0) {
        n00b_regex_dfa_state_t *st = &rev_dfa->states.data[state];
        bool nullable_at_begin = (st->flags & N00B_RE_SF_BEGIN_NULLABLE) != 0
                              || (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) != 0;
        if (nullable_at_begin) {
            leftmost = 0;
        }
    }

    return leftmost;
}

// ============================================================================
// Phase 3: Forward scan — find match end from a given start
// ============================================================================

static int64_t
forward_longest_from(n00b_regex_dfa_t *fwd_dfa,
                     const char *data, uint32_t data_len,
                     uint32_t start, uint32_t init_state)
{
    uint32_t state = init_state;
    int64_t  last_final = -1;
    uint32_t pos = start;

    // Check if initial state is final
    n00b_regex_dfa_state_t *st = &fwd_dfa->states.data[state];
    if (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) {
        last_final = (int64_t)pos;
    }

    // Fast path: flat transition table + ASCII byte LUT + per-state skip + bit-shift indexing
    //
    // The flat table packs a nullable flag into bit 31 of each entry:
    //   bit 31 = always-nullable flag, bits 0-30 = next state ID
    // This avoids a states.data[] lookup on every iteration just to check nullability.
    if (fwd_dfa->is_flat) {
        const uint32_t *flat = fwd_dfa->flat_transitions;
        uint8_t mt_log = fwd_dfa->mt_log2;
        const uint8_t *byte_lut = fwd_dfa->minterms->byte_lut;

        while (pos < data_len) {
            if (state == 0) break; // Dead state

            st = &fwd_dfa->states.data[state];

            // Per-state skip: only when state is not nullable at all.
            if (st->can_skip && !state_is_null(st)
                && !(st->flags & (N00B_RE_SF_ALWAYS_NULLABLE
                                  | N00B_RE_SF_END_NULLABLE
                                  | N00B_RE_SF_BEGIN_NULLABLE
                                  | N00B_RE_SF_ANCHOR_NULLABLE))) {
                const uint8_t *smap = st->skip_map;
                while (pos < data_len) {
                    uint8_t b = (uint8_t)data[pos];
                    if (b >= 0x80 || (smap[b >> 3] & (1u << (b & 7)))) break;
                    pos++;
                }
                if (pos >= data_len) break;
            }

            uint8_t b = (uint8_t)data[pos];
            n00b_regex_minterm_id_t mt;

            if (b < 0x80) {
                // ASCII fast path: single byte, direct LUT lookup
                mt = (n00b_regex_minterm_id_t)byte_lut[b];
                pos++;
            } else {
                // Multi-byte UTF-8: decode + classify
                int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
                if (cp < 0) break;
                mt = n00b_regex_minterm_classify(fwd_dfa->minterms, (n00b_codepoint_t)cp);
            }

            state = flat[(state << mt_log) | mt] & 0x7FFFFFFFu;

            st = &fwd_dfa->states.data[state];
            if (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) {
                last_final = (int64_t)pos;
            }
            else if (st->null_kind == N00B_RE_NULL_PENDING) {
                int64_t candidate = (int64_t)pos - st->min_pending;
                if (candidate > last_final) {
                    last_final = candidate;
                }
            }
        }
    } else {
        // Slow path: per-state transition arrays with lazy computation
        while (pos < data_len) {
            st = &fwd_dfa->states.data[state];
            if (st->node_id == N00B_RE_ID_NOTHING) break;

            uint8_t b = (uint8_t)data[pos];
            n00b_regex_minterm_id_t mt;

            if (b < 0x80) {
                mt = (n00b_regex_minterm_id_t)fwd_dfa->minterms->byte_lut[b];
                pos++;
            } else {
                int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
                if (cp < 0) break;
                mt = n00b_regex_minterm_classify(fwd_dfa->minterms, (n00b_codepoint_t)cp);
            }

            state = n00b_regex_dfa_step(fwd_dfa, state, mt);

            st = &fwd_dfa->states.data[state];
            if (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) {
                last_final = (int64_t)pos;
            }
            else if (st->null_kind == N00B_RE_NULL_PENDING) {
                int64_t candidate = (int64_t)pos - st->min_pending;
                if (candidate > last_final) {
                    last_final = candidate;
                }
            }
        }
    }

    // Check end-of-input nullability
    if (pos >= data_len) {
        st = &fwd_dfa->states.data[state];
        if ((st->flags & N00B_RE_SF_END_NULLABLE)
            && (int64_t)pos > last_final) {
            last_final = (int64_t)pos;
        }
        else if (st->null_kind == N00B_RE_NULL_PENDING) {
            int64_t candidate = (int64_t)pos - st->min_pending;
            if (candidate > last_final) {
                last_final = candidate;
            }
        }
    }

    return last_final;
}

static int64_t
forward_longest(n00b_regex_dfa_t *fwd_dfa,
                const char *data, uint32_t data_len,
                uint32_t start)
{
    return forward_longest_from(fwd_dfa, data, data_len, start, fwd_dfa->start_state);
}

// ============================================================================
// Core: find all non-overlapping matches
// ============================================================================

typedef struct {
    int64_t start;
    int64_t end;
} match_pair_t;

static uint32_t
find_all_matches(n00b_regex_t *re, const char *data, uint32_t data_len,
                 match_pair_t **out_matches)
{
    // --- Override: fixed-string matching ---
    if (re->optimizations.override_.kind == N00B_RE_OVERRIDE_FIXED_STRING) {
        const n00b_codepoint_t *needle = re->optimizations.override_.fixed_string.codepoints;
        uint32_t needle_len = re->optimizations.override_.fixed_string.len;

        // Encode needle once
        char needle_utf8[256];
        uint32_t needle_bytes = 0;
        for (uint32_t i = 0; i < needle_len && needle_bytes < sizeof(needle_utf8) - 4; i++) {
            needle_bytes += n00b_unicode_utf8_encode(needle[i], needle_utf8 + needle_bytes);
        }

        match_pair_t *matches = n00b_alloc_array(match_pair_t, 64);
        uint32_t n_matches = 0;
        uint32_t cap = 64;

        for (uint32_t pos = 0; pos + needle_bytes <= data_len; ) {
            if (memcmp(data + pos, needle_utf8, needle_bytes) == 0) {
                if (n_matches >= cap) {
                    cap *= 2;
                    match_pair_t *new_m = n00b_alloc_array(match_pair_t, cap);
                    for (uint32_t i = 0; i < n_matches; i++) new_m[i] = matches[i];
                    n00b_free(matches);
                    matches = new_m;
                }
                matches[n_matches++] = (match_pair_t){
                    .start = (int64_t)pos,
                    .end   = (int64_t)(pos + needle_bytes),
                };
                pos += needle_bytes; // Non-overlapping
                continue;
            }
            uint8_t b = (uint8_t)data[pos];
            if (b < 0x80) pos += 1;
            else if (b < 0xE0) pos += 2;
            else if (b < 0xF0) pos += 3;
            else pos += 4;
        }

        *out_matches = matches;
        return n_matches;
    }

    // --- Prefix acceleration for find-all ---
    const n00b_regex_accelerator_t *accel = &re->optimizations.accelerator;
    const n00b_regex_len_lookup_t  *llen  = &re->optimizations.len_lookup;
    n00b_regex_minterm_table_t     *mt_t  = re->minterms;
    n00b_regex_solver_t            *solv  = &re->solver;

    if (accel->kind != N00B_RE_ACCEL_NONE) {
        bool can_use_len = (accel->kind == N00B_RE_ACCEL_FIXED_PREFIX
                            || accel->kind == N00B_RE_ACCEL_FIXED_PREFIX_CI
                            || accel->kind == N00B_RE_ACCEL_MINTERM_PREFIX);
        uint32_t ts = accel_transition_state(accel);
        uint32_t plen = accel_prefix_len(accel);

        match_pair_t *matches = n00b_alloc_array(match_pair_t, 64);
        uint32_t n_matches = 0;
        uint32_t cap = 64;
        uint32_t search_start = 0;

        while (search_start < data_len) {
            int64_t found = accel_find_prefix(data, data_len, accel, mt_t, solv, search_start);
            if (found < 0) break;

            int64_t end = -1;
            if (can_use_len) {
                end = try_length_lookup(llen, re->forward_dfa, data, data_len, solv, found);
            }
            if (end < 0) {
                if (ts != 0 && can_use_len) {
                    uint32_t after = (uint32_t)found;
                    for (uint32_t i = 0; i < plen && after < data_len; i++) {
                        n00b_unicode_utf8_decode(data, data_len, &after);
                    }
                    end = forward_longest_from(re->forward_dfa, data, data_len, after, ts);
                }
                else {
                    end = forward_longest(re->forward_dfa, data, data_len, (uint32_t)found);
                }
            }

            if (end >= 0) {
                int64_t match_start = found;
                if (accel->needs_reverse_start) {
                    // Prefix was stripped from a nullable head — find true start
                    // by running reverse DFA backward from end.
                    int64_t rev_start = reverse_find_start(re, data, data_len,
                                                            end, (int64_t)search_start);
                    if (rev_start >= 0) {
                        match_start = rev_start;
                    }
                }
                if (n_matches >= cap) {
                    cap *= 2;
                    match_pair_t *new_m = n00b_alloc_array(match_pair_t, cap);
                    for (uint32_t i = 0; i < n_matches; i++) new_m[i] = matches[i];
                    n00b_free(matches);
                    matches = new_m;
                }
                matches[n_matches++] = (match_pair_t){.start = match_start, .end = end};
                search_start = (end > match_start) ? (uint32_t)end : (uint32_t)match_start + 1;
            }
            else {
                uint32_t p = (uint32_t)found;
                n00b_unicode_utf8_decode(data, data_len, &p);
                search_start = p;
            }
        }

        *out_matches = matches;
        return n_matches;
    }

    // --- Standard three-phase matching ---

    // Phase 1: HandleInputEnd
    acc_t acc = acc_new(64);
    uint32_t rev_state = re->reverse_dfa->start_state;

    int64_t scan_start = handle_input_end(re->reverse_dfa, data, data_len,
                                           &rev_state, &acc);

    // Phase 2: Reverse scan from scan_start
    collect_starts(re->reverse_dfa, data, data_len, &acc, rev_state, scan_start);

    if (acc.len == 0) {
        acc_free(&acc);
        *out_matches = nullptr;
        return 0;
    }

    // Phase 3: Forward scan from collected starts
    // Starts are in right-to-left order (highest offset first, lowest last).
    // Iterate from last (leftmost) to first (rightmost).
    match_pair_t *matches = n00b_alloc_array(match_pair_t, acc.len);
    uint32_t n_matches = 0;
    int64_t next_valid = 0;

    for (int64_t i = (int64_t)acc.len - 1; i >= 0; i--) {
        int64_t s = acc.data[i];
        if (s < next_valid) continue;

        int64_t end = try_length_lookup(llen, re->forward_dfa, data, data_len, solv, s);
        if (end < 0) {
            end = forward_longest(re->forward_dfa, data, data_len, (uint32_t)s);
        }
        if (end < 0) continue;

        matches[n_matches++] = (match_pair_t){.start = s, .end = end};
        next_valid = (end > s) ? end : s + 1;
    }

    acc_free(&acc);
    *out_matches = matches;
    return n_matches;
}

// ============================================================================
// Override: fixed-string matching (bypasses DFA entirely)
// ============================================================================

static int64_t
fixed_string_find(const char *data, uint32_t data_len,
                  const n00b_codepoint_t *needle, uint32_t needle_len,
                  uint32_t start)
{
    // Encode needle to UTF-8 for memcmp
    char needle_utf8[256];
    uint32_t needle_bytes = 0;
    for (uint32_t i = 0; i < needle_len && needle_bytes < sizeof(needle_utf8) - 4; i++) {
        needle_bytes += n00b_unicode_utf8_encode(needle[i], needle_utf8 + needle_bytes);
    }

    // Use memchr to skip to first-byte candidates
    uint32_t pos = start;
    while (pos + needle_bytes <= data_len) {
        const char *found = memchr(data + pos, needle_utf8[0], data_len - pos - needle_bytes + 1);
        if (!found) return -1;
        pos = (uint32_t)(found - data);
        if (pos + needle_bytes > data_len) return -1;
        if (needle_bytes == 1 || memcmp(found, needle_utf8, needle_bytes) == 0) {
            return (int64_t)pos;
        }
        pos++;
    }
    return -1;
}

// ============================================================================
// Accelerator: skip to first possible match start
// ============================================================================

// Scan forward for a position where the acceleration condition matches.
// Returns byte offset or -1 if not found.
static int64_t
accel_find_prefix(const char *data, uint32_t data_len,
                  const n00b_regex_accelerator_t *accel,
                  n00b_regex_minterm_table_t *mt_table,
                  n00b_regex_solver_t *solver,
                  uint32_t start)
{
    switch (accel->kind) {
    case N00B_RE_ACCEL_FIXED_PREFIX:
    case N00B_RE_ACCEL_FIXED_PREFIX_CI: {
        bool ci = (accel->kind == N00B_RE_ACCEL_FIXED_PREFIX_CI);
        const n00b_codepoint_t *prefix = accel->fixed_prefix.codepoints;
        uint32_t plen = accel->fixed_prefix.len;
        if (plen == 0) return (int64_t)start;

        // Pre-encode the prefix to UTF-8 for memchr/memcmp fast path
        char prefix_utf8[256];
        uint32_t prefix_bytes = 0;
        bool all_ascii = true;
        for (uint32_t i = 0; i < plen && prefix_bytes < sizeof(prefix_utf8) - 4; i++) {
            if (prefix[i] >= 0x80) all_ascii = false;
            prefix_bytes += n00b_unicode_utf8_encode(prefix[i], prefix_utf8 + prefix_bytes);
        }

        // Fast path: all-ASCII prefix — use memchr + memcmp
        if (all_ascii && !ci) {
            uint32_t pos = start;
            while (pos + prefix_bytes <= data_len) {
                // memchr skips to first byte match (SIMD-accelerated in libc)
                const char *found = memchr(data + pos, prefix_utf8[0], data_len - pos - prefix_bytes + 1);
                if (!found) return -1;
                uint32_t fpos = (uint32_t)(found - data);
                if (fpos + prefix_bytes > data_len) return -1;
                if (prefix_bytes == 1 || memcmp(found, prefix_utf8, prefix_bytes) == 0) {
                    return (int64_t)fpos;
                }
                pos = fpos + 1;
            }
            return -1;
        }

        // Fast path: ASCII case-insensitive — dual memchr + fast verify
        if (all_ascii && ci) {
            char first_lower = (char)prefix[0]; // already lowercase from parser
            char first_upper = (first_lower >= 'a' && first_lower <= 'z')
                             ? first_lower - 32 : first_lower;
            uint32_t pos = start;
            while (pos + prefix_bytes <= data_len) {
                // Use memchr for lower and upper variants of first char
                uint32_t remaining = data_len - pos - prefix_bytes + 1;
                const char *fl = memchr(data + pos, first_lower, remaining);
                const char *fu = (first_lower != first_upper)
                               ? memchr(data + pos, first_upper, remaining) : nullptr;
                uint32_t best = data_len;
                if (fl) best = (uint32_t)(fl - data);
                if (fu && (uint32_t)(fu - data) < best) best = (uint32_t)(fu - data);
                if (best + prefix_bytes > data_len) return -1;

                // Verify remaining prefix with fast case-fold
                bool full_match = true;
                for (uint32_t i = 1; i < plen; i++) {
                    uint8_t db = (uint8_t)data[best + i];
                    uint8_t pb = (uint8_t)prefix[i];
                    if (db == pb) continue;
                    // Only letters differ in case; fold uppercase to lower
                    if ((db >= 'A' && db <= 'Z') && (db + 32) == pb) continue;
                    full_match = false;
                    break;
                }
                if (full_match) return (int64_t)best;
                pos = best + 1;
            }
            return -1;
        }

        // Slow path: non-ASCII prefix — codepoint-by-codepoint
        for (uint32_t pos = start; pos < data_len; ) {
            uint32_t saved = pos;
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) break;

            n00b_codepoint_t target = prefix[0];
            bool hit;
            if (ci) {
                n00b_codepoint_t lcp = (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
                hit = (lcp == target);
            }
            else {
                hit = ((n00b_codepoint_t)cp == target);
            }

            if (hit) {
                uint32_t check_pos = pos;
                bool full_match = true;
                for (uint32_t i = 1; i < plen; i++) {
                    int32_t c2 = n00b_unicode_utf8_decode(data, data_len, &check_pos);
                    if (c2 < 0) { full_match = false; break; }
                    n00b_codepoint_t t2 = prefix[i];
                    if (ci) {
                        n00b_codepoint_t lc2 = (c2 >= 'A' && c2 <= 'Z') ? c2 + 32 : c2;
                        if (lc2 != t2) { full_match = false; break; }
                    }
                    else {
                        if ((n00b_codepoint_t)c2 != t2) { full_match = false; break; }
                    }
                }
                if (full_match) return (int64_t)saved;
            }
        }
        return -1;
    }

    case N00B_RE_ACCEL_SINGLE_MINTERM: {
        // Scan for first codepoint that belongs to the minterm charset.
        // Uses pre-computed ASCII bitmap for O(1) byte classification.
        n00b_regex_charset_t cs = accel->single_minterm.minterm;
        const uint8_t *ascii_map = accel->single_minterm.ascii_map;

        for (uint32_t pos = start; pos < data_len; ) {
            uint8_t b = (uint8_t)data[pos];
            if (b < 0x80) {
                if (ascii_map[b]) return (int64_t)pos;
                pos++;
            } else {
                uint32_t saved = pos;
                int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
                if (cp < 0) break;
                if (n00b_regex_charset_contains(solver, cs, (n00b_codepoint_t)cp)) {
                    return (int64_t)saved;
                }
            }
        }
        return -1;
    }

    case N00B_RE_ACCEL_MINTERM_PREFIX: {
        // Scan for first position where the minterm prefix sequence matches.
        // Uses pre-computed ASCII bitmap for first minterm.
        const n00b_regex_charset_t *minterms = accel->minterm_prefix.minterms;
        uint32_t plen = accel->minterm_prefix.len;
        if (plen == 0) return (int64_t)start;

        const uint8_t *first_map = accel->minterm_prefix.first_ascii_map;

        for (uint32_t pos = start; pos < data_len; ) {
            uint8_t b = (uint8_t)data[pos];
            bool first_hit;
            uint32_t saved = pos;

            if (b < 0x80) {
                first_hit = first_map[b];
                pos++;
            } else {
                int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
                if (cp < 0) break;
                first_hit = n00b_regex_charset_contains(solver, minterms[0], (n00b_codepoint_t)cp);
            }

            if (first_hit) {
                uint32_t check_pos = pos;
                bool full_match = true;
                for (uint32_t i = 1; i < plen; i++) {
                    int32_t c2 = n00b_unicode_utf8_decode(data, data_len, &check_pos);
                    if (c2 < 0) { full_match = false; break; }
                    if (!n00b_regex_charset_contains(solver, minterms[i], (n00b_codepoint_t)c2)) {
                        full_match = false;
                        break;
                    }
                }
                if (full_match) return (int64_t)saved;
            }
        }
        return -1;
    }

    case N00B_RE_ACCEL_POTENTIAL_START: {
        // Scan for first codepoint that belongs to ANY of the potential-start charsets.
        // Uses pre-computed merged ASCII bitmap.
        const n00b_regex_charset_t *sets = accel->potential_start.sets;
        uint32_t n_sets = accel->potential_start.len;
        const uint8_t *ascii_map = accel->potential_start.ascii_map;

        for (uint32_t pos = start; pos < data_len; ) {
            uint8_t b = (uint8_t)data[pos];
            if (b < 0x80) {
                if (ascii_map[b]) return (int64_t)pos;
                pos++;
            } else {
                uint32_t saved = pos;
                int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
                if (cp < 0) break;
                for (uint32_t s = 0; s < n_sets; s++) {
                    if (n00b_regex_charset_contains(solver, sets[s], (n00b_codepoint_t)cp)) {
                        return (int64_t)saved;
                    }
                }
            }
        }
        return -1;
    }

    case N00B_RE_ACCEL_NONE:
        return (int64_t)start;
    }
    return (int64_t)start;
}

// ============================================================================
// Length lookup: determine match end without full forward DFA scan
// ============================================================================

static int64_t
try_length_lookup(const n00b_regex_len_lookup_t *len,
                  n00b_regex_dfa_t *fwd_dfa,
                  const char *data, uint32_t data_len,
                  n00b_regex_solver_t *solver,
                  int64_t match_start)
{
    switch (len->kind) {
    case N00B_RE_LEN_FIXED: {
        // Match is always exactly N codepoints
        int32_t n = len->fixed.length;
        uint32_t pos = (uint32_t)match_start;
        for (int32_t i = 0; i < n && pos < data_len; i++) {
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) return -1;
        }
        return (int64_t)pos;
    }

    case N00B_RE_LEN_SET_LOOKUP: {
        // Skip prefix_length codepoints, then check if next codepoint is in minterm
        int32_t prefix_n = len->set_lookup.prefix_length;
        uint32_t pos = (uint32_t)match_start;
        for (int32_t i = 0; i < prefix_n && pos < data_len; i++) {
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) return -1;
        }
        // Next codepoint must be in the set — if so, match ends after prefix + 1
        if (pos < data_len) {
            uint32_t saved = pos;
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp >= 0 && n00b_regex_charset_contains(solver, len->set_lookup.minterm, (n00b_codepoint_t)cp)) {
                return (int64_t)pos;
            }
            (void)saved;
        }
        return -1;
    }

    case N00B_RE_LEN_REMAINING_SETS: {
        // Skip prefix_length codepoints, then consume up to max_remaining codepoints
        // that match the minterm
        int32_t prefix_n = len->remaining.prefix_length;
        uint32_t pos = (uint32_t)match_start;
        for (int32_t i = 0; i < prefix_n && pos < data_len; i++) {
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) return -1;
        }
        // Consume up to max_remaining matching codepoints
        int64_t last_match = -1;
        for (uint8_t i = 0; i < len->remaining.max_remaining && pos < data_len; i++) {
            uint32_t saved = pos;
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) break;
            if (!n00b_regex_charset_contains(solver, len->remaining.minterm, (n00b_codepoint_t)cp)) {
                break;
            }
            last_match = (int64_t)pos;
            (void)saved;
        }
        return last_match;
    }

    case N00B_RE_LEN_FIXED_PREFIX_MATCH_END: {
        // Skip prefix_length codepoints, then run forward DFA from transition_state
        int32_t prefix_n = len->prefix_match_end.prefix_length;
        uint32_t ts = len->prefix_match_end.transition_state;
        if (ts == 0) return -1;  // No valid transition state — full DFA needed

        uint32_t pos = (uint32_t)match_start;
        for (int32_t i = 0; i < prefix_n && pos < data_len; i++) {
            int32_t cp = n00b_unicode_utf8_decode(data, data_len, &pos);
            if (cp < 0) return -1;
        }
        return forward_longest_from(fwd_dfa, data, data_len, pos, ts);
    }

    case N00B_RE_LEN_MATCH_END:
        return -1; // Full DFA needed
    }
    return -1;
}

// ============================================================================
// Public API: is_match (short-circuits after first match)
// ============================================================================

bool
n00b_regex_is_match(n00b_regex_t *re, n00b_string_t *input)
{
    const char *data     = input->data;
    uint32_t    data_len = (uint32_t)input->u8_bytes;

    // --- Override: fixed-string match bypasses DFA entirely ---
    if (re->optimizations.override_.kind == N00B_RE_OVERRIDE_FIXED_STRING) {
        return fixed_string_find(data, data_len,
                                 re->optimizations.override_.fixed_string.codepoints,
                                 re->optimizations.override_.fixed_string.len, 0) >= 0;
    }

    // --- Prefix acceleration fast path ---
    const n00b_regex_accelerator_t *accel = &re->optimizations.accelerator;
    const n00b_regex_len_lookup_t  *llen  = &re->optimizations.len_lookup;
    n00b_regex_minterm_table_t     *mt_t  = re->minterms;
    n00b_regex_solver_t            *solv  = &re->solver;

    if (accel->kind != N00B_RE_ACCEL_NONE) {
        // Length lookup is only safe when the accelerator confirms a full prefix match.
        // POTENTIAL_START and SINGLE_MINTERM only identify candidate positions.
        bool can_use_len = (accel->kind == N00B_RE_ACCEL_FIXED_PREFIX
                            || accel->kind == N00B_RE_ACCEL_FIXED_PREFIX_CI
                            || accel->kind == N00B_RE_ACCEL_MINTERM_PREFIX);
        uint32_t ts = accel_transition_state(accel);
        uint32_t plen = accel_prefix_len(accel);

        uint32_t search_start = 0;
        while (search_start < data_len) {
            int64_t found = accel_find_prefix(data, data_len, accel, mt_t, solv, search_start);
            if (found < 0) return false;

            int64_t end = -1;
            if (can_use_len) {
                end = try_length_lookup(llen, re->forward_dfa, data, data_len, solv, found);
            }
            if (end < 0) {
                // Use transition_state to skip the confirmed prefix
                if (ts != 0 && can_use_len) {
                    uint32_t after = (uint32_t)found;
                    for (uint32_t i = 0; i < plen && after < data_len; i++) {
                        n00b_unicode_utf8_decode(data, data_len, &after);
                    }
                    end = forward_longest_from(re->forward_dfa, data, data_len, after, ts);
                }
                else {
                    end = forward_longest(re->forward_dfa, data, data_len, (uint32_t)found);
                }
            }
            if (end >= 0) return true;

            // Advance past this position
            uint32_t p = (uint32_t)found;
            n00b_unicode_utf8_decode(data, data_len, &p);
            search_start = p;
        }
        return false;
    }

    // --- Standard three-phase matching ---

    // Phase 1: HandleInputEnd
    acc_t acc = acc_new(16);
    uint32_t rev_state = re->reverse_dfa->start_state;
    int64_t scan_start = handle_input_end(re->reverse_dfa, data, data_len,
                                           &rev_state, &acc);

    // Phase 2: Reverse scan — but we can stop early
    uint32_t state = rev_state;
    int64_t  pos = scan_start;

    // Use flat table if available for reverse DFA
    if (re->reverse_dfa->is_flat) {
        const uint32_t *flat = re->reverse_dfa->flat_transitions;
        uint8_t mt_log = re->reverse_dfa->mt_log2;
        const uint8_t *byte_lut = re->reverse_dfa->minterms->byte_lut;

        while (pos > 0) {
            if (state == 0) break;

            // ASCII fast path for reverse scan
            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(re->reverse_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = flat[(state << mt_log) | mt] & 0x7FFFFFFFu;

            n00b_regex_dfa_state_t *st = &re->reverse_dfa->states.data[state];
            if (state_is_null(st)) {
                int64_t end = forward_longest(re->forward_dfa, data, data_len,
                                               (uint32_t)prev_pos);
                if (end >= 0) {
                    acc_free(&acc);
                    return true;
                }
            }
            pos = prev_pos;
        }
    } else {
        while (pos > 0) {
            n00b_regex_dfa_state_t *st = &re->reverse_dfa->states.data[state];
            if (st->node_id == N00B_RE_ID_NOTHING) break;

            // ASCII fast path for reverse scan
            uint8_t prev_byte = (uint8_t)data[pos - 1];
            n00b_regex_minterm_id_t mt;
            int64_t prev_pos;
            if (prev_byte < 0x80) {
                mt = (n00b_regex_minterm_id_t)re->reverse_dfa->minterms->byte_lut[prev_byte];
                prev_pos = pos - 1;
            } else {
                backward_decode_t bd = utf8_decode_backward(data, pos);
                mt = n00b_regex_minterm_classify(re->reverse_dfa->minterms, bd.cp);
                prev_pos = bd.prev_pos;
            }
            state = n00b_regex_dfa_step(re->reverse_dfa, state, mt);

            st = &re->reverse_dfa->states.data[state];
            if (state_is_null(st)) {
                int64_t end = forward_longest(re->forward_dfa, data, data_len,
                                               (uint32_t)prev_pos);
                if (end >= 0) {
                    acc_free(&acc);
                    return true;
                }
            }
            pos = prev_pos;
        }
    }

    // Check begin-of-input
    {
        n00b_regex_dfa_state_t *st = &re->reverse_dfa->states.data[state];
        bool nullable_at_begin = (st->flags & N00B_RE_SF_BEGIN_NULLABLE) != 0
                              || (st->flags & N00B_RE_SF_ALWAYS_NULLABLE) != 0;
        if (nullable_at_begin) {
            int64_t end = forward_longest(re->forward_dfa, data, data_len, 0);
            if (end >= 0) {
                acc_free(&acc);
                return true;
            }
        }
    }

    // Also check any positions collected during HandleInputEnd
    for (int64_t i = (int64_t)acc.len - 1; i >= 0; i--) {
        int64_t end = forward_longest(re->forward_dfa, data, data_len,
                                       (uint32_t)acc.data[i]);
        if (end >= 0) {
            acc_free(&acc);
            return true;
        }
    }

    acc_free(&acc);
    return false;
}

// ============================================================================
// Public API: count, matches
// ============================================================================

// ============================================================================
// Count-only: same algorithm as find_all_matches but only counts, no alloc
// ============================================================================

static uint32_t
count_all_matches(n00b_regex_t *re, const char *data, uint32_t data_len)
{
    // --- Override: fixed-string counting ---
    if (re->optimizations.override_.kind == N00B_RE_OVERRIDE_FIXED_STRING) {
        const n00b_codepoint_t *needle = re->optimizations.override_.fixed_string.codepoints;
        uint32_t needle_len = re->optimizations.override_.fixed_string.len;

        char needle_utf8[256];
        uint32_t needle_bytes = 0;
        for (uint32_t i = 0; i < needle_len && needle_bytes < sizeof(needle_utf8) - 4; i++) {
            needle_bytes += n00b_unicode_utf8_encode(needle[i], needle_utf8 + needle_bytes);
        }

        uint32_t count = 0;
        uint32_t pos = 0;
        while (pos + needle_bytes <= data_len) {
            const char *found = memchr(data + pos, needle_utf8[0], data_len - pos - needle_bytes + 1);
            if (!found) break;
            pos = (uint32_t)(found - data);
            if (pos + needle_bytes > data_len) break;
            if (needle_bytes == 1 || memcmp(found, needle_utf8, needle_bytes) == 0) {
                count++;
                pos += needle_bytes;
                continue;
            }
            pos++;
        }
        return count;
    }

    // --- Prefix acceleration counting ---
    const n00b_regex_accelerator_t *accel = &re->optimizations.accelerator;
    const n00b_regex_len_lookup_t  *llen  = &re->optimizations.len_lookup;
    n00b_regex_minterm_table_t     *mt_t  = re->minterms;
    n00b_regex_solver_t            *solv  = &re->solver;

    if (accel->kind != N00B_RE_ACCEL_NONE) {
        bool can_use_len = (accel->kind == N00B_RE_ACCEL_FIXED_PREFIX
                            || accel->kind == N00B_RE_ACCEL_FIXED_PREFIX_CI
                            || accel->kind == N00B_RE_ACCEL_MINTERM_PREFIX);
        uint32_t ts = accel_transition_state(accel);
        uint32_t plen = accel_prefix_len(accel);

        uint32_t count = 0;
        uint32_t search_start = 0;

        while (search_start < data_len) {
            int64_t found = accel_find_prefix(data, data_len, accel, mt_t, solv, search_start);
            if (found < 0) break;

            int64_t end = -1;
            if (can_use_len) {
                end = try_length_lookup(llen, re->forward_dfa, data, data_len, solv, found);
            }
            if (end < 0) {
                if (ts != 0 && can_use_len) {
                    uint32_t after = (uint32_t)found;
                    for (uint32_t i = 0; i < plen && after < data_len; i++) {
                        n00b_unicode_utf8_decode(data, data_len, &after);
                    }
                    end = forward_longest_from(re->forward_dfa, data, data_len, after, ts);
                } else {
                    end = forward_longest(re->forward_dfa, data, data_len, (uint32_t)found);
                }
            }

            if (end >= 0) {
                int64_t match_start = found;
                if (accel->needs_reverse_start) {
                    int64_t rev_start = reverse_find_start(re, data, data_len,
                                                            end, (int64_t)search_start);
                    if (rev_start >= 0) {
                        match_start = rev_start;
                    }
                }
                count++;
                search_start = (end > match_start) ? (uint32_t)end : (uint32_t)match_start + 1;
            } else {
                uint32_t p = (uint32_t)found;
                n00b_unicode_utf8_decode(data, data_len, &p);
                search_start = p;
            }
        }
        return count;
    }

    // --- Standard three-phase counting ---
    acc_t acc = acc_new(64);
    uint32_t rev_state = re->reverse_dfa->start_state;
    int64_t scan_start = handle_input_end(re->reverse_dfa, data, data_len,
                                           &rev_state, &acc);
    collect_starts(re->reverse_dfa, data, data_len, &acc, rev_state, scan_start);

    if (acc.len == 0) {
        acc_free(&acc);
        return 0;
    }

    uint32_t count = 0;
    int64_t next_valid = 0;

    for (int64_t i = (int64_t)acc.len - 1; i >= 0; i--) {
        int64_t s = acc.data[i];
        if (s < next_valid) continue;

        int64_t end = try_length_lookup(llen, re->forward_dfa, data, data_len, solv, s);
        if (end < 0) {
            end = forward_longest(re->forward_dfa, data, data_len, (uint32_t)s);
        }
        if (end < 0) continue;

        count++;
        next_valid = (end > s) ? end : s + 1;
    }

    acc_free(&acc);
    return count;
}

int64_t
n00b_regex_count(n00b_regex_t *re, n00b_string_t *input)
{
    return (int64_t)count_all_matches(re, input->data, (uint32_t)input->u8_bytes);
}

n00b_list_t(n00b_regex_match_t) *
n00b_regex_matches(n00b_regex_t *re, n00b_string_t *input)
{
    n00b_list_t(n00b_regex_match_t) *result = n00b_alloc(n00b_list_t(n00b_regex_match_t));
    *result = n00b_list_new_private(n00b_regex_match_t);

    match_pair_t *matches;
    uint32_t n = find_all_matches(re, input->data, (uint32_t)input->u8_bytes, &matches);

    for (uint32_t i = 0; i < n; i++) {
        n00b_regex_match_t m = {.index = matches[i].start, .length = matches[i].end - matches[i].start};
        n00b_list_push(*result, m);
    }

    if (matches) n00b_free(matches);
    return result;
}

n00b_string_t *
n00b_regex_replace(n00b_regex_t  *re,
                   n00b_string_t *input,
                   n00b_string_t *replacement)
{
    const char *data     = input->data;
    uint32_t    data_len = (uint32_t)input->u8_bytes;

    match_pair_t *matches;
    uint32_t n = find_all_matches(re, data, data_len, &matches);

    if (n == 0) {
        if (matches) n00b_free(matches);
        return input;
    }

    // Check for $0 in replacement
    const char *repl_data = replacement->data;
    int64_t     repl_len  = replacement->u8_bytes;
    bool        has_dollar_zero = false;
    for (int64_t i = 0; i < repl_len - 1; i++) {
        if (repl_data[i] == '$' && repl_data[i + 1] == '0') {
            has_dollar_zero = true;
            break;
        }
    }

    n00b_buffer_t *buf = n00b_buffer_new(data_len + 64, .no_lock = true);
    buf->byte_len = 0;

    uint32_t pos = 0;

    for (uint32_t i = 0; i < n; i++) {
        int64_t s = matches[i].start;
        int64_t e = matches[i].end;

        if (s > (int64_t)pos) {
            n00b_buffer_t *seg = n00b_buffer_from_bytes(
                (char *)(data + pos), s - (int64_t)pos, .no_lock = true);
            n00b_buffer_concat(buf, seg);
            n00b_buffer_free(seg);
        }

        if (has_dollar_zero) {
            n00b_buffer_t *exp = n00b_buffer_new(repl_len + 64, .no_lock = true);
            exp->byte_len = 0;
            for (int64_t j = 0; j < repl_len; j++) {
                if (j < repl_len - 1 && repl_data[j] == '$' && repl_data[j + 1] == '0') {
                    if (e > s) {
                        n00b_buffer_t *mseg = n00b_buffer_from_bytes(
                            (char *)(data + s), e - s, .no_lock = true);
                        n00b_buffer_concat(exp, mseg);
                        n00b_buffer_free(mseg);
                    }
                    j++;
                }
                else {
                    n00b_buffer_t *ch = n00b_buffer_from_bytes(
                        (char *)&repl_data[j], 1, .no_lock = true);
                    n00b_buffer_concat(exp, ch);
                    n00b_buffer_free(ch);
                }
            }
            n00b_buffer_concat(buf, exp);
            n00b_buffer_free(exp);
        }
        else {
            if (repl_len > 0) {
                n00b_buffer_t *rseg = n00b_buffer_from_bytes(
                    (char *)repl_data, repl_len, .no_lock = true);
                n00b_buffer_concat(buf, rseg);
                n00b_buffer_free(rseg);
            }
        }

        pos = (uint32_t)e;
    }

    if (pos < data_len) {
        n00b_buffer_t *tail = n00b_buffer_from_bytes(
            (char *)(data + pos), data_len - pos, .no_lock = true);
        n00b_buffer_concat(buf, tail);
        n00b_buffer_free(tail);
    }

    n00b_free(matches);

    n00b_string_t *result;
    int64_t buflen = (int64_t)n00b_buffer_len(buf);
    if (buflen > 0) {
        result = n00b_string_from_raw(buf->data, buflen);
    }
    else {
        result = n00b_string_empty();
    }

    n00b_buffer_free(buf);
    return result;
}

n00b_list_t(n00b_string_t *) *
n00b_regex_split(n00b_regex_t *re, n00b_string_t *input)
{
    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new_private(n00b_string_t *);

    const char *data     = input->data;
    uint32_t    data_len = (uint32_t)input->u8_bytes;

    match_pair_t *matches;
    uint32_t n = find_all_matches(re, data, data_len, &matches);

    if (n == 0) {
        n00b_list_push(*result, input);
        if (matches) n00b_free(matches);
        return result;
    }

    uint32_t pos = 0;

    for (uint32_t i = 0; i < n; i++) {
        int64_t s = matches[i].start;
        int64_t e = matches[i].end;

        uint32_t seg_len = (uint32_t)(s - (int64_t)pos);
        n00b_string_t *seg = n00b_string_from_raw(data + pos, (int64_t)seg_len);
        n00b_list_push(*result, seg);

        pos = (uint32_t)e;
    }

    if (pos <= data_len) {
        uint32_t seg_len = data_len - pos;
        n00b_string_t *seg = n00b_string_from_raw(data + pos, (int64_t)seg_len);
        n00b_list_push(*result, seg);
    }

    n00b_free(matches);
    return result;
}
