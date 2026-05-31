// earley.c - Core Earley recognizer (predict / scan / complete)
//
// Ported from slop's src/slay/parser.c, adapted for n00b's allocator,
// container types, and token stream architecture.

#include "slay/earley.h"
#include "slay/n00b_parse.h"
#include "internal/slay/earley_internal.h"
#include "internal/slay/unicode_class.h"
#include "text/unicode/encoding.h"
#include "parsers/token_stream.h"
#include "core/string.h"
#include "adt/option.h"
#include "text/strings/string_ops.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

static inline n00b_match_t *
get_rule_match(n00b_parse_rule_t *r, int i)
{
    return &r->contents.data[i];
}

static inline n00b_match_t *
get_ei_match(n00b_earley_item_t *ei, int i)
{
    return get_rule_match(ei->rule, i);
}

static inline int32_t
ei_rule_len(n00b_earley_item_t *ei)
{
    return (int32_t)n00b_list_len(ei->rule->contents);
}

// ============================================================================
// Subtree info / next action
// ============================================================================

static inline void
set_subtree_info(n00b_earley_parser_t *p __attribute__((unused)),
                 n00b_earley_item_t   *ei)
{
    n00b_earley_item_t *start = ei->start_item;

    if (!ei->cursor) {
        if (start->group) {
            if (start->double_dot) {
                ei->subtree_info = N00B_SI_GROUP_START;
            }
            else {
                ei->subtree_info = N00B_SI_GROUP_ITEM_START;
            }
        }
        else {
            ei->subtree_info = N00B_SI_NT_RULE_START;
        }
    }

    if (ei->cursor == ei_rule_len(start)) {
        if (start->group) {
            if (start->double_dot) {
                ei->subtree_info = N00B_SI_GROUP_END;
                ei->double_dot   = true;
            }
            else {
                ei->subtree_info = N00B_SI_GROUP_ITEM_END;
            }
        }
        else {
            ei->subtree_info = N00B_SI_NT_RULE_END;
        }
    }
}

static inline void
set_next_action(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    n00b_earley_item_t *start = ei->start_item;

    if (ei->cursor == ei_rule_len(start)) {
        switch (ei->subtree_info) {
        case N00B_SI_GROUP_END:
        case N00B_SI_NT_RULE_END:
            ei->op = N00B_EO_COMPLETE_N;
            return;
        default:
            ei->op = N00B_EO_ITEM_END;
            return;
        }
    }

    if (!ei->cursor && ei->double_dot) {
        ei->op = N00B_EO_FIRST_GROUP_ITEM;
        return;
    }

    n00b_match_t *next = get_ei_match(ei->start_item, ei->cursor);

    switch (next->kind) {
    case N00B_MATCH_TERMINAL:
        ei->op = N00B_EO_SCAN_TOKEN;
        return;
    case N00B_MATCH_ANY:
        ei->op = N00B_EO_SCAN_ANY;
        return;
    case N00B_MATCH_CLASS:
        ei->op = N00B_EO_SCAN_CLASS;
        return;
    case N00B_MATCH_SET:
        ei->op = N00B_EO_SCAN_SET;
        return;
    case N00B_MATCH_NT: {
        n00b_nonterm_t *nt = n00b_get_nonterm(p->grammar, next->nt_id);
        if (nt && nt->nullable) {
            ei->null_prediction = true;
        }
        ei->op = N00B_EO_PREDICT_NT;
        return;
    }
    case N00B_MATCH_GROUP:
        ei->op = N00B_EO_PREDICT_G;
        return;
    case N00B_MATCH_EMPTY:
        ei->op = N00B_EO_SCAN_NULL;
        return;
    }
}

// ============================================================================
// Duplicate detection via hash map
// ============================================================================

static inline uint64_t
mix64(uint64_t n)
{
    n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ULL;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebULL;

    return n ^ (n >> 31);
}

uint64_t
n00b_earley_item_hash(n00b_earley_item_t *ei)
{
    n00b_earley_item_t *s = ei->start_item;

    uint64_t small_bits = ((uint64_t)ei->cursor << 0)
                        | ((uint64_t)ei->double_dot << 32)
                        | ((uint64_t)ei->null_prediction << 33);

    uint64_t h = mix64((uintptr_t)ei->previous_scan + 0x9e3779b97f4a7c15ULL);
    h         ^= mix64((uintptr_t)ei->rule + 0x517cc1b727220a95ULL);
    h         ^= mix64(small_bits + 0x6c62272e07bb0142ULL);
    h         ^= mix64((uintptr_t)ei->group + 0x85ebca6b2de36fc3ULL);
    h         ^= mix64((uintptr_t)ei->group_top + 0x27d4eb2f165b3321ULL);

    if (s && s != ei) {
        h ^= mix64((uintptr_t)s + 0x2545f4914f6cdd1dULL);
    }
    else if (s) {
        h ^= 0xc2b2ae3d27d4eb4fULL;
    }

    return h ? h : 1;
}

static inline uint64_t
item_hash(n00b_earley_item_t *ei)
{
    return n00b_earley_item_hash(ei);
}

#define prop_check_ei(prop)                                                    \
    if (((void *)(int64_t)old->prop) != ((void *)(int64_t)new_ei->prop)) {     \
        return false;                                                          \
    }
#define prop_check_start(prop)                                                 \
    if (os->prop != ns->prop) {                                                \
        return false;                                                          \
    }

static inline bool
are_dupes(n00b_earley_item_t *old, n00b_earley_item_t *new_ei)
{
    if (old->lr0_state_id >= 0 && new_ei->lr0_state_id >= 0
        && old->lr0_state_id != new_ei->lr0_state_id) {
        return false;
    }

    n00b_earley_item_t *os = old->start_item;
    n00b_earley_item_t *ns = new_ei->start_item;

    if (os && ns && os != old) {
        if (ns != os) {
            return false;
        }
    }

    if (os && os != old) {
        prop_check_start(previous_scan);
        prop_check_start(rule);
        prop_check_start(cursor);
        prop_check_start(group);
        prop_check_start(group_top);
        prop_check_start(double_dot);
        prop_check_start(null_prediction);
    }

    prop_check_ei(previous_scan);
    prop_check_ei(cursor);
    prop_check_ei(group);
    prop_check_ei(group_top);
    prop_check_ei(double_dot);
    prop_check_ei(rule);
    prop_check_ei(null_prediction);

    return true;
}

#undef prop_check_ei
#undef prop_check_start

static void
item_map_init(n00b_item_map_t *m)
{
    m->cap     = 64;
    m->len     = 0;
    m->buckets = n00b_alloc_array(n00b_earley_item_t *, m->cap);
}

static void
item_map_grow(n00b_item_map_t *m)
{
    int32_t              old_cap     = m->cap;
    n00b_earley_item_t **old_buckets = m->buckets;
    int32_t              new_cap     = old_cap * 2;
    int32_t              mask        = new_cap - 1;

    m->buckets = n00b_alloc_array(n00b_earley_item_t *, new_cap);
    m->cap     = new_cap;
    m->len     = 0;

    for (int32_t i = 0; i < old_cap; i++) {
        n00b_earley_item_t *ei = old_buckets[i];

        if (ei) {
            uint64_t h   = item_hash(ei);
            int32_t  idx = (int32_t)(h & (uint64_t)mask);

            while (m->buckets[idx]) {
                idx = (idx + 1) & mask;
            }

            m->buckets[idx] = ei;
            m->len++;
        }
    }

    n00b_free(old_buckets);
}

static inline n00b_earley_item_t *
item_map_find(n00b_item_map_t *m, n00b_earley_item_t *ei)
{
    if (!m->buckets) {
        return NULL;
    }

    uint64_t h    = item_hash(ei);
    int32_t  mask = m->cap - 1;
    int32_t  idx  = (int32_t)(h & (uint64_t)mask);

    while (m->buckets[idx]) {
        if (are_dupes(m->buckets[idx], ei)) {
            return m->buckets[idx];
        }

        idx = (idx + 1) & mask;
    }

    return NULL;
}

static inline void
item_map_insert(n00b_item_map_t *m, n00b_earley_item_t *ei)
{
    if (!m->buckets) {
        item_map_init(m);
    }

    if (m->len * 4 >= m->cap * 3) {
        item_map_grow(m);
    }

    uint64_t h    = item_hash(ei);
    int32_t  mask = m->cap - 1;
    int32_t  idx  = (int32_t)(h & (uint64_t)mask);

    while (m->buckets[idx]) {
        idx = (idx + 1) & mask;
    }

    m->buckets[idx] = ei;
    m->len++;
}

// ============================================================================
// Group penalty calculation
// ============================================================================

static inline void
calculate_group_end_penalties(n00b_earley_item_t *ei)
{
    int match_min    = ei->group->min;
    int match_max    = ei->group->max;
    int new_match_ct = ei->match_ct;

    if (new_match_ct < match_min) {
        ei->group_penalty = new_match_ct - match_min;
    }
    else {
        if (match_max && new_match_ct > match_max) {
            ei->group_penalty = (match_max - new_match_ct);
        }
    }

    ei->penalty = ei->group_penalty + ei->my_penalty + ei->sub_penalties;
}

// ============================================================================
// Earley item allocation and add_item
// ============================================================================

static inline n00b_earley_item_t *
new_earley_item(void)
{
    n00b_earley_item_t *result = n00b_alloc(n00b_earley_item_t);

    memset(result, 0, sizeof(n00b_earley_item_t));
    result->lr0_state_id = -1;

    return result;
}

static void
add_item(n00b_earley_parser_t  *p,
         n00b_earley_item_t    *from_state,
         n00b_earley_item_t   **newptr,
         bool                   next_state)
{
    (void)from_state;
    n00b_earley_item_t *new_ei = *newptr;

    if (new_ei->group && new_ei->cursor == ei_rule_len(new_ei)) {
        if (new_ei->group->min < new_ei->match_ct) {
            calculate_group_end_penalties(new_ei);
        }
    }

    if (new_ei->penalty > p->grammar->max_penalty) {
        return;
    }

    assert(new_ei->rule);

    n00b_earley_state_t *state = next_state ? p->next_state : p->current_state;
    int32_t              n     = (int32_t)n00b_list_len(state->items);

    new_ei->estate_id   = state->id;
    new_ei->eitem_index = n;

    n00b_earley_item_t *existing = item_map_find(&state->item_map, new_ei);

    if (existing) {
        switch (n00b_earley_cost_cmp(new_ei, existing)) {
        case N00B_CMP_LT:
            existing->completors    = new_ei->completors;
            existing->penalty       = new_ei->penalty;
            existing->sub_penalties = new_ei->sub_penalties;
            existing->my_penalty    = new_ei->penalty;
            existing->group_penalty = new_ei->group_penalty;
            existing->previous_scan = new_ei->previous_scan;
            existing->predictions   = new_ei->predictions;
            existing->cost          = new_ei->cost;
            break;
        case N00B_CMP_GT:
            return;
        default:
            break;
        }

        if (existing->completors != new_ei->completors) {
            existing->completors
                = n00b_item_set_union(existing->completors, new_ei->completors);
        }

        *newptr = existing;
        return;
    }

    n00b_list_push(state->items, new_ei);
    item_map_insert(&state->item_map, new_ei);
    set_subtree_info(p, new_ei);
    set_next_action(p, new_ei);
}

// ============================================================================
// LR(0) GOTO lookup
// ============================================================================

// Symbol encoding — must match grammar.c exactly.
//
// NT        -> nt_id (always >= 0 and small)
// TERMINAL  -> terminal_id with bit 62 set (avoids collision with small NTs)
// GROUP     -> -(1000+gid)
// ANY       -> INT64_MIN + 1
// EMPTY     -> INT64_MIN + 2
// CLASS     -> INT64_MIN + 100 + class_id
// SET       -> INT64_MIN + 3
//
// The INT64_MIN + N sentinels are chosen to never collide with hash-based
// terminal IDs (which have bit 63 set, making them negative but > INT64_MIN + 1000).
#define LR0_SYM_ANY    (INT64_MIN + 1)
#define LR0_SYM_EMPTY  (INT64_MIN + 2)
#define LR0_SYM_SET    (INT64_MIN + 3)

static inline int64_t
lr0_symbol_of_match(n00b_match_t *m)
{
    switch (m->kind) {
    case N00B_MATCH_NT:
        return m->nt_id;
    case N00B_MATCH_TERMINAL:
        // Set bit 62 to distinguish from NT ids when terminal_id >= 0.
        // Hash-based IDs (bit 63 set) already can't collide with NTs.
        if (m->terminal_id >= 0) {
            return m->terminal_id | (1LL << 62);
        }
        return m->terminal_id;
    case N00B_MATCH_GROUP: {
        n00b_rule_group_t *grp = (n00b_rule_group_t *)m->group;
        return -(1000LL + grp->gid);
    }
    case N00B_MATCH_ANY:
        return LR0_SYM_ANY;
    case N00B_MATCH_EMPTY:
        return LR0_SYM_EMPTY;
    case N00B_MATCH_CLASS:
        return INT64_MIN + 100LL + (int64_t)m->char_class;
    case N00B_MATCH_SET:
        return LR0_SYM_SET;
    }

    return INT64_MIN;
}

static inline int32_t
lr0_goto_lookup(n00b_grammar_t *g, int32_t state_id, int64_t symbol)
{
    if (state_id < 0 || state_id >= g->lr0_state_count) {
        return -1;
    }

    n00b_lr0_state_t *st = &g->lr0_states[state_id];

    for (int32_t i = 0; i < st->gotos_count; i++) {
        n00b_lr0_goto_t *gt = &g->lr0_gotos[st->gotos_start + i];

        if (gt->symbol == symbol) {
            return gt->dest_state;
        }
    }

    return -1;
}

// ============================================================================
// BSR emission
// ============================================================================

uint64_t
n00b_earley_bsr_hash(int32_t slot, int32_t left, int32_t pivot, int32_t right)
{
    uint64_t a = ((uint64_t)(uint32_t)slot << 32) | (uint64_t)(uint32_t)left;
    uint64_t b = ((uint64_t)(uint32_t)pivot << 32) | (uint64_t)(uint32_t)right;

    a = (a ^ (a >> 30)) * 0xbf58476d1ce4e5b9ULL;
    a = (a ^ (a >> 27)) * 0x94d049bb133111ebULL;
    b = (b ^ (b >> 30)) * 0xbf58476d1ce4e5b9ULL;
    b = (b ^ (b >> 27)) * 0x94d049bb133111ebULL;

    uint64_t h = a ^ b ^ (a >> 31) ^ (b >> 31);

    return (h < 2) ? (h + 2) : h;
}

static inline uint64_t
bsr_hash(int32_t slot, int32_t left, int32_t pivot, int32_t right)
{
    return n00b_earley_bsr_hash(slot, left, pivot, right);
}

static void
bsr_emit(n00b_earley_parser_t *p,
         int32_t               slot,
         int32_t               left,
         int32_t               pivot,
         int32_t               right)
{
    if (!p->grammar->lr0_rule_item_base || p->grammar->has_groups) {
        return;
    }

    uint64_t key = bsr_hash(slot, left, pivot, right);

    if (!p->bsr_dedup) {
        p->bsr_dedup = n00b_alloc(n00b_dict_t(uint64_t, bool));
        n00b_dict_init(p->bsr_dedup, .hash = n00b_hash_word, .skip_obj_hash = true);
    }

    bool true_val = true;
    if (!n00b_dict_add(p->bsr_dedup, key, true_val)) {
        return;
    }

    if (p->bsr_count >= p->bsr_cap) {
        int32_t new_cap = p->bsr_cap ? p->bsr_cap * 2 : 1024;

        n00b_bsr_element_t *new_set
            = n00b_alloc_array(n00b_bsr_element_t, new_cap);

        if (p->bsr_set && p->bsr_count > 0) {
            memcpy(new_set, p->bsr_set,
                   (size_t)p->bsr_count * sizeof(n00b_bsr_element_t));
        }

        n00b_free(p->bsr_set);
        p->bsr_set = new_set;
        p->bsr_cap = new_cap;
    }

    p->bsr_set[p->bsr_count++] = (n00b_bsr_element_t){
        .slot         = slot,
        .left_extent  = left,
        .pivot        = pivot,
        .right_extent = right,
    };
}

// ============================================================================
// Terminal scan
// ============================================================================

static void
terminal_scan(n00b_earley_parser_t *p,
              n00b_earley_item_t   *old,
              bool                  not_null)
{
    n00b_earley_item_t *new_ei = new_earley_item();
    n00b_earley_item_t *start  = old->start_item;

    new_ei->start_item    = start;
    new_ei->cursor        = old->cursor + 1;
    new_ei->parent_states = old->parent_states;
    new_ei->penalty       = old->penalty;
    new_ei->sub_penalties = old->sub_penalties;
    new_ei->my_penalty    = old->my_penalty;
    new_ei->previous_scan = old;
    new_ei->rule          = old->rule;
    new_ei->cost          = old->cost;
    new_ei->match_ct      = old->match_ct;
    new_ei->group         = old->group;
    new_ei->group_top     = old->group_top;

    if (old->lr0_state_id >= 0) {
        n00b_match_t *m   = get_ei_match(start, old->cursor);
        int64_t       sym = lr0_symbol_of_match(m);

        new_ei->lr0_state_id = lr0_goto_lookup(p->grammar, old->lr0_state_id, sym);
    }

    old->no_reprocessing = true;
    add_item(p, NULL, &new_ei, not_null);

    if (p->grammar->lr0_rule_item_base && not_null && !p->grammar->has_groups) {
        int32_t rule_ix = (int32_t)(start->rule - &p->grammar->rules.data[0]);
        int32_t slot    = p->grammar->lr0_rule_item_base[rule_ix]
                        + old->cursor + 1;

        bsr_emit(p, slot, start->estate_id, old->estate_id, new_ei->estate_id);
    }
}

// ============================================================================
// Prediction registration
// ============================================================================

static void
register_prediction(n00b_earley_parser_t *p,
                    n00b_earley_item_t   *predictor,
                    n00b_earley_item_t   *predicted)
{
    (void)p;

    if (!predictor) {
        return;
    }

    if (!predictor->predictions) {
        predictor->predictions = n00b_item_set_new();
    }

    if (!predicted->parent_states) {
        predicted->parent_states = n00b_item_set_new();
    }

    n00b_item_set_add(predictor->predictions, predicted);
    n00b_item_set_add(predicted->parent_states, predictor);
}

// ============================================================================
// Non-terminal prediction
// ============================================================================

static void
add_one_nt_prediction(n00b_earley_parser_t *p,
                      n00b_earley_item_t   *predictor,
                      n00b_nonterm_t       *nt,
                      int                   rule_ix)
{
    int32_t             rule_global_ix = nt->rule_ids.data[rule_ix];
    n00b_parse_rule_t  *rule           = n00b_get_rule(p->grammar, rule_global_ix);

    // Fast dedup: probe the item_map before allocating.
    int32_t lr0 = p->grammar->lr0_predict_state
                  ? p->grammar->lr0_predict_state[nt->id]
                  : -1;

    n00b_earley_item_t probe = {0};
    probe.start_item      = &probe;
    probe.rule            = rule;
    probe.ruleset_id      = nt->id;
    probe.rule_index      = rule_ix;
    probe.lr0_state_id    = lr0;

    // Replicate set_next_action's null_prediction logic for the probe.
    if (n00b_list_len(rule->contents) > 0
        && rule->contents.data[0].kind == N00B_MATCH_NT) {
        n00b_nonterm_t *fnt = n00b_get_nonterm(p->grammar,
                                                rule->contents.data[0].nt_id);
        if (fnt && fnt->nullable) {
            probe.null_prediction = true;
        }
    }

    n00b_earley_item_t *existing =
        item_map_find(&p->current_state->item_map, &probe);

    if (existing) {
        register_prediction(p, predictor, existing);
        return;
    }

    n00b_earley_item_t *ei = new_earley_item();

    ei->ruleset_id   = nt->id;
    ei->rule         = rule;
    ei->rule_index   = rule_ix;
    ei->start_item   = ei;
    ei->lr0_state_id = lr0;

    if (predictor) {
        n00b_earley_item_t *prestart = predictor->start_item;
        ei->predictor_ruleset_id    = prestart->ruleset_id;
        ei->predictor_rule_index    = prestart->rule_index;
        ei->predictor_cursor        = prestart->cursor;
    }

    if (ei->rule->penalty_rule) {
        ei->my_penalty += 1;
    }

    ei->penalty = ei->my_penalty;

    add_item(p, predictor, &ei, false);
    register_prediction(p, predictor, ei);
}

static inline bool
nt_first_matches(n00b_nonterm_t *nt, int64_t token_id)
{
    if (nt->first_has_any) {
        return true;
    }

    if (!nt->first_set || nt->first_set->length == 0) {
        return true;
    }

    return n00b_dict_contains(nt->first_set, token_id);
}

static inline void
predict_nt(n00b_earley_parser_t *p,
           n00b_nonterm_t       *nt,
           n00b_earley_item_t   *ei)
{
    size_t n = n00b_list_len(nt->rule_ids);

    // FIRST-set filtering: skip prediction if the current token
    // can't start this NT.  But don't filter predictions triggered
    // by completions — those are mid-rule and need to proceed
    // regardless of the current token.
    if (!ei || !ei->from_completion) {
        n00b_token_info_t *tok = p->current_state->token;

        if (tok && tok->tid != N00B_TOK_EOF && !nt_first_matches(nt, tok->tid)) {
            return;
        }
    }

    for (size_t i = 0; i < n; i++) {
        add_one_nt_prediction(p, ei, nt, (int)i);
    }
}

static inline void
predict_nt_via_ei(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    n00b_earley_item_t *s  = ei->start_item;
    n00b_match_t       *nx = get_ei_match(s, ei->cursor);
    n00b_nonterm_t     *nt = n00b_get_nonterm(p->grammar, nx->nt_id);

    if (ei->null_prediction) {
        terminal_scan(p, ei, true);
    }

    predict_nt(p, nt, ei);
}

// ============================================================================
// Group prediction
// ============================================================================

static n00b_earley_item_t *
add_one_group_prediction(n00b_earley_parser_t *p,
                         n00b_earley_item_t   *predictor)
{
    n00b_earley_item_t *ps   = predictor->start_item;
    n00b_match_t       *next = get_ei_match(ps, predictor->cursor);
    n00b_rule_group_t  *g    = (n00b_rule_group_t *)next->group;

    n00b_earley_item_t *gei = new_earley_item();
    gei->start_item        = gei;
    gei->cursor            = 0;
    gei->double_dot        = true;
    gei->group             = g;
    gei->ruleset_id        = g->gid;

    n00b_nonterm_t *gnt = n00b_get_nonterm(p->grammar, g->contents_id);
    assert(n00b_list_len(gnt->rule_ids) > 0);
    int32_t rule_ix = gnt->rule_ids.data[0];
    gei->rule       = n00b_get_rule(p->grammar, rule_ix);
    assert(gei->rule);

    gei->predictor_ruleset_id = ps->ruleset_id;
    gei->predictor_rule_index = ps->rule_index;
    gei->predictor_cursor     = ps->cursor;

    add_item(p, predictor, &gei, false);
    register_prediction(p, predictor, gei);

    return gei;
}

static void
add_first_group_item(n00b_earley_parser_t *p, n00b_earley_item_t *gei)
{
    n00b_earley_item_t *ei = new_earley_item();

    n00b_nonterm_t *gnt2 = n00b_get_nonterm(p->grammar, gei->group->contents_id);
    assert(n00b_list_len(gnt2->rule_ids) > 0);
    int32_t rule_ix = gnt2->rule_ids.data[0];

    ei->start_item = ei;
    ei->cursor     = 0;
    ei->rule       = n00b_get_rule(p->grammar, rule_ix);
    ei->ruleset_id = gei->group->gid;
    ei->rule_index = 0;
    ei->match_ct   = 0;
    ei->group      = gei->group;
    ei->group_top  = gei;
    ei->double_dot = false;

    assert(gei);

    ei->predictor_ruleset_id = ei->group_top->ruleset_id;
    ei->predictor_rule_index = ei->group_top->rule_index;
    ei->predictor_cursor     = ei->group_top->cursor;

    add_item(p, gei, &ei, false);
    register_prediction(p, gei, ei);
}

static void
empty_group_completion(n00b_earley_parser_t *p, n00b_earley_item_t *gei)
{
    if (!gei->group->min) {
        n00b_earley_item_t *gend = new_earley_item();

        gend->start_item    = gei;
        gend->cursor        = (int32_t)n00b_list_len(gei->rule->contents);
        gend->double_dot    = true;
        gend->group         = gei->group;
        gend->ruleset_id    = gei->group->gid;
        gend->rule          = gei->rule;
        gend->match_ct      = 0;
        gend->parent_states = gei->parent_states;

        add_item(p, gei, &gend, false);
    }

    gei->no_reprocessing = true;
}

static void
add_next_group_item(n00b_earley_parser_t *p, n00b_earley_item_t *last_end)
{
    n00b_earley_item_t *ei         = new_earley_item();
    n00b_earley_item_t *last_start = last_end->start_item;

    last_end->no_reprocessing = true;

    ei->start_item    = ei;
    ei->cursor        = 0;
    ei->previous_scan = last_end;
    ei->rule          = last_start->rule;
    ei->ruleset_id    = last_start->ruleset_id;
    ei->parent_states = n00b_item_set_copy(last_start->parent_states);
    ei->group_top     = last_start->group_top;
    ei->match_ct      = last_end->match_ct;
    ei->group         = last_start->group;
    ei->my_penalty    = last_end->my_penalty;
    ei->sub_penalties = last_end->sub_penalties;
    ei->cost          = last_end->cost;

    int max_items = ei->group_top->group->max;

    assert(ei->group_top);

    ei->predictor_ruleset_id = ei->group_top->ruleset_id;
    ei->predictor_rule_index = ei->group_top->rule_index;
    ei->predictor_cursor     = ei->group_top->cursor;

    if (max_items && ei->match_ct > max_items) {
        ei->group_penalty = ei->match_ct - max_items;
    }

    ei->penalty = ei->group_penalty + ei->my_penalty + ei->sub_penalties;

    add_item(p, last_end, &ei, false);

    if (!ei->completors) {
        ei->completors = n00b_item_set_new();
    }

    n00b_item_set_put(ei->completors, ei->group_top);
    register_prediction(p, ei->group_top, ei);
}

// ============================================================================
// Reclassify annotation processing
// ============================================================================

static inline n00b_token_info_t *
get_token(n00b_earley_parser_t *p, int32_t pos)
{
    return n00b_stream_get(p->stream, pos);
}

// ============================================================================
// Completion
// ============================================================================

static void
add_one_completion(n00b_earley_parser_t *p,
                   n00b_earley_item_t   *cur,
                   n00b_earley_item_t   *parent_ei)
{
    n00b_earley_item_t *ei           = new_earley_item();
    n00b_earley_item_t *parent_start = parent_ei->start_item;

    ei->start_item      = parent_start;
    ei->cursor          = parent_ei->cursor + 1;
    ei->previous_scan   = parent_ei;
    ei->from_completion = true;
    ei->group_top     = parent_ei->group_top;
    ei->my_penalty    = parent_ei->my_penalty;
    ei->sub_penalties = parent_ei->sub_penalties + cur->penalty;
    ei->penalty       = ei->sub_penalties + ei->my_penalty + ei->group_penalty;
    ei->rule          = parent_start->rule;
    ei->cost          = cur->start_item->rule->cost;
    ei->cost += parent_ei->cost + cur->cost;

    if (parent_ei->lr0_state_id >= 0) {
        int32_t completed_nt = cur->start_item->ruleset_id;
        ei->lr0_state_id = lr0_goto_lookup(p->grammar,
                                           parent_ei->lr0_state_id,
                                           completed_nt);
    }

    if (ei->group_top) {
        ei->match_ct = parent_start->match_ct;
    }

    if (ei->penalty < cur->penalty) {
        ei->penalty = cur->penalty;
    }

    // Don't allow empty group items.
    if (ei->group_top && ei->start_item->estate_id == p->current_state->id) {
        return;
    }

    ei->parent_states = n00b_item_set_copy(parent_ei->parent_states);

    add_item(p, cur, &ei, false);

    if (!ei->completors) {
        ei->completors = n00b_item_set_new();
    }

    n00b_item_set_put(ei->completors, cur);
    parent_ei->no_reprocessing = true;

    if (p->grammar->lr0_rule_item_base && !p->grammar->has_groups) {
        int32_t rule_ix = (int32_t)(parent_start->rule
                                    - &p->grammar->rules.data[0]);
        int32_t slot    = p->grammar->lr0_rule_item_base[rule_ix]
                        + parent_ei->cursor + 1;

        bsr_emit(p, slot, parent_start->estate_id,
                 cur->start_item->estate_id, ei->estate_id);
    }
}

static n00b_earley_item_t *
add_group_completion(n00b_earley_parser_t *p, n00b_earley_item_t *cur)
{
    n00b_earley_item_t *ei     = new_earley_item();
    n00b_earley_item_t *istart = cur->start_item;
    n00b_earley_item_t *gstart = istart->group_top;

    assert(gstart);

    ei->start_item    = gstart;
    ei->rule          = gstart->rule;
    ei->ruleset_id    = gstart->group->gid;
    ei->cursor        = (int32_t)n00b_list_len(ei->rule->contents);
    ei->match_ct      = cur->match_ct;
    ei->group         = gstart->group;
    ei->double_dot    = true;
    ei->completors    = n00b_item_set_new();
    ei->parent_states = gstart->parent_states;

    calculate_group_end_penalties(ei);
    ei->penalty += cur->my_penalty + cur->sub_penalties;
    ei->cost = cur->cost;

    add_item(p, cur, &ei, false);

    n00b_item_set_put(ei->completors, cur);
    cur->no_reprocessing = true;

    if (p->grammar->lr0_rule_item_base && !p->grammar->has_groups) {
        int32_t rule_ix = (int32_t)(gstart->rule - &p->grammar->rules.data[0]);
        int32_t slot    = p->grammar->lr0_rule_item_base[rule_ix]
                        + (int32_t)n00b_list_len(gstart->rule->contents);

        bsr_emit(p, slot, gstart->estate_id, istart->estate_id, ei->estate_id);
    }

    return ei;
}

// ============================================================================
// Leo optimization: deterministic right-recursive completion
// ============================================================================

static void
compute_leo_table(n00b_earley_parser_t *p)
{
    n00b_earley_state_t *state   = p->current_state;
    size_t               n_items = n00b_list_len(state->items);
    int32_t              num_nts = (int32_t)n00b_list_len(p->grammar->nt_list);

    static n00b_earley_item_t leo_conflict;

    for (size_t i = 0; i < n_items; i++) {
        n00b_earley_item_t *ei    = state->items.data[i];
        n00b_earley_item_t *start = ei->start_item;
        int32_t             rlen  = (int32_t)n00b_list_len(start->rule->contents);

        if (ei->cursor >= rlen || start->double_dot || start->group
            || ei->penalty > 0) {
            continue;
        }

        n00b_match_t *next = &start->rule->contents.data[ei->cursor];

        if (next->kind != N00B_MATCH_NT) {
            continue;
        }

        int32_t nt_id = (int32_t)next->nt_id;

        if (nt_id < 0 || nt_id >= num_nts) {
            continue;
        }

        if (!state->leo_table) {
            state->leo_table = n00b_alloc_array(
                n00b_earley_item_t *, (size_t)num_nts);
        }

        if (state->leo_table[nt_id] == &leo_conflict) {
            continue;
        }

        if (state->leo_table[nt_id] != NULL || ei->cursor + 1 != rlen) {
            state->leo_table[nt_id] = &leo_conflict;
            continue;
        }

        state->leo_table[nt_id] = ei;
    }

    if (!state->leo_table) {
        return;
    }

    for (int32_t nt_id = 0; nt_id < num_nts; nt_id++) {
        if (state->leo_table[nt_id] == &leo_conflict) {
            state->leo_table[nt_id] = NULL;
        }
    }

    for (int32_t nt_id = 0; nt_id < num_nts; nt_id++) {
        n00b_earley_item_t *ei = state->leo_table[nt_id];

        if (!ei) {
            continue;
        }

        n00b_earley_item_t  *start     = ei->start_item;
        int32_t              parent_nt = start->ruleset_id;
        n00b_earley_state_t *orig      = p->states.data[start->estate_id];

        if (orig->leo_table && parent_nt >= 0 && parent_nt < num_nts
            && orig->leo_table[parent_nt]) {
            state->leo_table[nt_id] = orig->leo_table[parent_nt];
        }
    }
}

static void
track_virt_item(n00b_earley_parser_t *p, void *item)
{
    if (p->virt_items_len >= p->virt_items_cap) {
        int32_t new_cap = p->virt_items_cap ? p->virt_items_cap * 2 : 64;
        void  **new_arr = n00b_alloc_array(void *, new_cap);

        if (p->virt_items && p->virt_items_len > 0) {
            memcpy(new_arr, p->virt_items,
                   (size_t)p->virt_items_len * sizeof(void *));
        }

        n00b_free(p->virt_items);
        p->virt_items     = new_arr;
        p->virt_items_cap = new_cap;
    }

    p->virt_items[p->virt_items_len++] = item;
}

static void
add_leo_completion(n00b_earley_parser_t *p,
                   n00b_earley_item_t   *cur,
                   n00b_earley_item_t   *leo_top,
                   n00b_earley_item_t   *direct_parent)
{
    n00b_earley_item_t *ei           = new_earley_item();
    n00b_earley_item_t *parent_start = leo_top->start_item;

    ei->start_item    = parent_start;
    ei->cursor        = leo_top->cursor + 1;
    ei->previous_scan = leo_top;
    ei->group_top     = leo_top->group_top;
    ei->my_penalty    = leo_top->my_penalty;
    ei->sub_penalties = leo_top->sub_penalties + cur->penalty;
    ei->penalty       = ei->sub_penalties + ei->my_penalty + ei->group_penalty;
    ei->rule          = parent_start->rule;
    ei->cost          = cur->start_item->rule->cost;
    ei->cost += leo_top->cost + cur->cost;

    if (leo_top->lr0_state_id >= 0) {
        int32_t completed_nt = cur->start_item->ruleset_id;
        ei->lr0_state_id = lr0_goto_lookup(p->grammar,
                                           leo_top->lr0_state_id,
                                           completed_nt);
    }

    if (ei->group_top) {
        ei->match_ct = parent_start->match_ct;
    }

    if (ei->penalty < cur->penalty) {
        ei->penalty = cur->penalty;
    }

    ei->parent_states     = n00b_item_set_copy(leo_top->parent_states);
    ei->leo_item          = true;
    ei->leo_direct_parent = direct_parent;

    add_item(p, cur, &ei, false);

    if (!ei->completors) {
        ei->completors = n00b_item_set_new();
    }

    n00b_item_set_put(ei->completors, cur);
    leo_top->no_reprocessing = true;

    // Emit BSR for all links in the Leo chain.
    if (p->grammar->lr0_rule_item_base && !p->grammar->has_groups) {
        int32_t             completed_nt_start = cur->start_item->estate_id;
        n00b_earley_item_t *link               = direct_parent;

        for (;;) {
            n00b_earley_item_t *link_start = link->start_item;
            int32_t rule_ix = (int32_t)(link_start->rule
                                        - &p->grammar->rules.data[0]);
            int32_t slot    = p->grammar->lr0_rule_item_base[rule_ix]
                            + link->cursor + 1;

            bsr_emit(p, slot, link_start->estate_id,
                     completed_nt_start, ei->estate_id);

            if (link_start == parent_start) {
                break;
            }

            completed_nt_start = link_start->estate_id;

            n00b_item_set_t *ps = link_start->parent_states;

            if (!ps || ps->length == 0) {
                break;
            }

            n00b_earley_item_t *next_link = NULL;

            n00b_dict_foreach(ps, k, v, {
                (void)v;
                next_link = k;
                break;
            });

            if (!next_link) {
                break;
            }

            link = next_link;
        }
    }
}

static void
complete(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    n00b_item_set_t *start_set = ei->start_item->parent_states;

    if (!start_set) {
        return;
    }

    n00b_earley_item_t  *start        = ei->start_item;
    int32_t              completed_nt = start->ruleset_id;
    n00b_earley_state_t *origin       = p->states.data[start->estate_id];

    if (start_set->length == 1 && origin->leo_table
        && !start->double_dot && !start->group) {
        int32_t num_nts = (int32_t)n00b_list_len(p->grammar->nt_list);

        if (completed_nt >= 0 && completed_nt < num_nts
            && origin->leo_table[completed_nt]) {
            n00b_earley_item_t *direct = NULL;

            n00b_dict_foreach(start_set, k, v, {
                (void)v;
                direct = k;
                break;
            });

            add_leo_completion(
                p, ei, origin->leo_table[completed_nt], direct);
            return;
        }
    }

    n00b_dict_foreach(start_set, k, v, {
        (void)v;
        add_one_completion(p, ei, k);
    });
}

static void
complete_group_item(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    ei->match_ct++;
    add_next_group_item(p, ei);
}

// ============================================================================
// Scan operations
// ============================================================================

static inline void
scan_terminal(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    int64_t       tid  = p->current_state->token->tid;
    n00b_match_t *next = get_ei_match(ei->start_item, ei->cursor);

    if (tid == next->terminal_id) {
        terminal_scan(p, ei, true);
    }
}

static inline void
scan_class(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    n00b_token_info_t *tok  = p->current_state->token;
    n00b_match_t      *next = get_ei_match(ei->start_item, ei->cursor);

    if (!n00b_option_is_set(tok->value)) {
        return;
    }

    n00b_string_t *val = n00b_option_get(tok->value);
    uint32_t       pos = 0;
    int32_t        cp  = n00b_unicode_utf8_decode(val->data,
                                                   (uint32_t)val->u8_bytes,
                                                  &pos);

    if (cp >= 0 && n00b_codepoint_matches_class(cp, next->char_class)) {
        terminal_scan(p, ei, true);
    }
}

static inline void
scan_set(n00b_earley_parser_t *p, n00b_earley_item_t *ei)
{
    /* N00B_MATCH_SET (set-of-terminals match) is reserved in the
     * grammar type system (n00b_match_t.set_items is `void *` —
     * shape not pinned down) but no BNF parser or vendored grammar
     * produces it today.  PWZ has the same gap (pwz.c:224 falls
     * through silently).  Until a real consumer lands — and decides
     * whether set_items holds a list of terminal IDs, a bitset, or
     * something else — we abort loudly rather than silently
     * parse-fail, so the first grammar that emits SET surfaces the
     * gap immediately. */
    (void)p;
    (void)ei;
    /* Matches the existing in-file fatal-error idiom (assert.h).  When
     * grammar parsing emits SET for the first time, this assertion
     * fires with a clear message instead of silent parse-fail. */
    assert(0 && "scan_set: N00B_MATCH_SET reserved but not implemented");
}

// ============================================================================
// Predict / Scan / Complete passes
// ============================================================================

static inline void
predict_group(n00b_earley_parser_t *p, n00b_earley_item_t *predictor)
{
    n00b_earley_item_t *gei = add_one_group_prediction(p, predictor);
    add_first_group_item(p, gei);
}

static inline void
run_state_completions(n00b_earley_parser_t *p)
{
    size_t i = 0;

    while (i < n00b_list_len(p->current_state->items)) {
        n00b_earley_item_t *ei = p->current_state->items.data[i];

        if (!ei->no_reprocessing) {
            switch (ei->op) {
            case N00B_EO_COMPLETE_N:
                complete(p, ei);
                break;
            case N00B_EO_ITEM_END:
                complete_group_item(p, ei);
                add_group_completion(p, ei);
                break;
            default:
                break;
            }
        }

        i++;
    }
}

static inline void
run_state_predictions(n00b_earley_parser_t *p)
{
    size_t i = 0;

    while (i < n00b_list_len(p->current_state->items)) {
        n00b_earley_item_t *ei = p->current_state->items.data[i];

        if (!ei->no_reprocessing) {
            switch (ei->op) {
            case N00B_EO_PREDICT_NT:
                predict_nt_via_ei(p, ei);
                break;
            case N00B_EO_PREDICT_G:
                predict_group(p, ei);
                ei->no_reprocessing = true;
                break;
            case N00B_EO_FIRST_GROUP_ITEM:
                add_first_group_item(p, ei);
                empty_group_completion(p, ei);
                break;
            default:
                break;
            }
        }

        i++;
    }
}

static inline void
run_state_scans(n00b_earley_parser_t *p)
{
    size_t i = 0;

    while (i < n00b_list_len(p->current_state->items)) {
        n00b_earley_item_t *ei = p->current_state->items.data[i];

        if (!ei->no_reprocessing) {
            switch (ei->op) {
            case N00B_EO_SCAN_TOKEN:
                scan_terminal(p, ei);
                break;
            case N00B_EO_SCAN_ANY:
                terminal_scan(p, ei, true);
                break;
            case N00B_EO_SCAN_NULL:
                terminal_scan(p, ei, false);
                break;
            case N00B_EO_SCAN_CLASS:
                scan_class(p, ei);
                break;
            case N00B_EO_SCAN_SET:
                scan_set(p, ei);
                break;
            default:
                break;
            }
        }

        i++;
    }
}

static void
process_current_state(n00b_earley_parser_t *p)
{
    size_t prev = n00b_list_len(p->current_state->items);
    size_t cur;

    while (true) {
        run_state_completions(p);
        run_state_predictions(p);
        run_state_scans(p);

        cur = n00b_list_len(p->current_state->items);

        if (cur == prev) {
            break;
        }

        prev = cur;
    }
}

// ============================================================================
// Token loading (lazy stream consumption)
// ============================================================================

static n00b_token_info_t *
pull_next_token(n00b_earley_parser_t *p)
{
    return n00b_stream_next(p->stream);
}

// ============================================================================
// State management
// ============================================================================

static n00b_earley_state_t *
new_earley_state(int id)
{
    n00b_earley_state_t *state = n00b_alloc(n00b_earley_state_t);

    memset(state, 0, sizeof(n00b_earley_state_t));
    state->id = id;

    return state;
}

static void
enter_next_state(n00b_earley_parser_t *p)
{
    if (p->next_state != NULL) {
        p->position++;
        p->current_state = p->next_state;
    }
    else {
        n00b_nonterm_t *start = n00b_get_nonterm(p->grammar, p->start);
        predict_nt(p, start, NULL);
    }

    p->next_state = new_earley_state((int32_t)n00b_list_len(p->states));
    n00b_list_push(p->states, p->next_state);

    // Load the token for the current state from the stream.
    n00b_token_info_t *tok = pull_next_token(p);

    if (tok) {
        p->current_state->token = tok;
    }
    else {
        // Create synthetic EOF token.
        n00b_token_info_t *eof = n00b_alloc(n00b_token_info_t);
        memset(eof, 0, sizeof(n00b_token_info_t));
        eof->tid                = N00B_TOK_EOF;
        eof->index              = p->current_state->id;
        p->current_state->token = eof;
    }
}

static void
run_parsing_mainloop(n00b_earley_parser_t *p)
{
    do {
        enter_next_state(p);
        process_current_state(p);

        if (!p->grammar->has_groups) {
            compute_leo_table(p);
        }

    } while (p->current_state->token->tid != N00B_TOK_EOF);
}

// ============================================================================
// Public API
// ============================================================================

n00b_earley_parser_t *
n00b_earley_new(n00b_grammar_t *g)
{
    n00b_grammar_finalize(g);
    // finalize computes first-sets/left-corners (PWZ needs them); the
    // Earley-only LR0 tables are not built there. Derive them here,
    // lazily, the first time Earley runs on this grammar.
    n00b_grammar_compute_earley_analysis(g);

    n00b_earley_parser_t *p = n00b_alloc(n00b_earley_parser_t);
    memset(p, 0, sizeof(n00b_earley_parser_t));

    p->grammar     = g;
    p->start       = -1;
    p->bsr_cap     = 1024;
    p->bsr_set     = n00b_alloc_array(n00b_bsr_element_t, 1024);
    p->bsr_dedup   = n00b_alloc(n00b_dict_t(uint64_t, bool));
    n00b_dict_init(p->bsr_dedup, .hash = n00b_hash_word, .skip_obj_hash = true);

    // Initialize the first state.
    p->current_state = new_earley_state(0);
    n00b_list_push(p->states, p->current_state);

    return p;
}

void
n00b_earley_free(n00b_earley_parser_t *p)
{
    if (!p) {
        return;
    }

    // Free virtual earley items from Leo chain expansion.
    for (int32_t i = 0; i < p->virt_items_len; i++) {
        n00b_free(p->virt_items[i]);
    }

    n00b_free(p->virt_items);

    // Free tree construction intermediates.
    n00b_earley_cleanup_intermediates(p);

    // Free BSR set.
    n00b_free(p->bsr_set);

    n00b_free(p);
}

void
n00b_earley_reset(n00b_earley_parser_t *p)
{
    // Free virtual earley items.
    for (int32_t i = 0; i < p->virt_items_len; i++) {
        n00b_free(p->virt_items[i]);
    }

    n00b_free(p->virt_items);
    p->virt_items     = NULL;
    p->virt_items_len = 0;
    p->virt_items_cap = 0;

    // Free tree construction intermediates.
    n00b_earley_cleanup_intermediates(p);

    // Reset state.
    memset(&p->states, 0, sizeof(p->states));

    p->position      = 0;
    p->start         = -1;
    p->run           = false;
    p->stream        = NULL;
    p->user_context  = NULL;
    p->current_state = NULL;
    p->next_state    = NULL;

    // Reset BSR.
    n00b_free(p->bsr_set);
    p->bsr_set   = n00b_alloc_array(n00b_bsr_element_t, 1024);
    p->bsr_count = 0;
    p->bsr_cap   = 1024;

    if (p->bsr_dedup) {
        n00b_free(p->bsr_dedup);
    }

    p->bsr_dedup = n00b_alloc(n00b_dict_t(uint64_t, bool));
    n00b_dict_init(p->bsr_dedup, .hash = n00b_hash_word, .skip_obj_hash = true);

    // Initialize first state.
    p->current_state = new_earley_state(0);
    n00b_list_push(p->states, p->current_state);
}

bool
n00b_earley_parse(n00b_earley_parser_t *p,
                   n00b_token_stream_t  *ts)
{
    if (!p->grammar) {
        return false;
    }

    n00b_earley_reset(p);

    p->stream = ts;

    if (p->start < 0) {
        p->start = p->grammar->default_start;
    }

    run_parsing_mainloop(p);
    p->run = true;

    return n00b_earley_parse_count(p) > 0;
}

int32_t
n00b_earley_parse_count(n00b_earley_parser_t *p)
{
    if (n00b_list_len(p->states) < 2) {
        return 0;
    }

    n00b_earley_state_t *final_state
        = p->states.data[n00b_list_len(p->states) - 2];
    int32_t count = 0;

    for (size_t i = 0; i < n00b_list_len(final_state->items); i++) {
        n00b_earley_item_t *ei = final_state->items.data[i];

        if (ei->op != N00B_EO_COMPLETE_N) {
            continue;
        }

        if (ei->subtree_info != N00B_SI_NT_RULE_END) {
            continue;
        }

        n00b_earley_item_t *start = ei->start_item;

        if (start->estate_id != 0) {
            continue;
        }

        if (start->ruleset_id != p->start) {
            continue;
        }

        count++;
    }

    return count;
}

// ============================================================================
// Results — get_tree wraps get_forest (implemented in earley_forest.c)
// ============================================================================

n00b_parse_tree_t *
n00b_earley_get_tree(n00b_earley_parser_t *p)
{
    n00b_parse_forest_t f = n00b_earley_get_forest(p);

    return n00b_parse_forest_best(&f);
}

// ============================================================================
// One-shot parse (implements n00b_parse_fn_t)
// ============================================================================

n00b_parse_forest_t
n00b_earley_parse_grammar(n00b_grammar_t      *g,
                           n00b_token_stream_t *ts)
{
    n00b_earley_parser_t *p = n00b_earley_new(g);
    bool ok = n00b_earley_parse(p, ts);

    if (!ok) {
        n00b_earley_free(p);
        return n00b_parse_forest_empty(g);
    }

    n00b_parse_forest_t forest = n00b_earley_get_forest(p);
    n00b_earley_free(p);

    return forest;
}

// ============================================================================
// Diagnostics extraction (for unified parse API)
// ============================================================================

void
n00b_earley_extract_diagnostics(n00b_earley_parser_t      *p,
                                n00b_earley_diagnostics_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!p || n00b_list_len(p->states) < 2) {
        return;
    }

    // Find the first state where error recovery was needed.
    // Walk forward looking for the earliest state with penalized items.
    // If no penalty items exist, fall back to the furthest scan-ready state.
    int32_t nstates     = (int32_t)n00b_list_len(p->states);
    int32_t first_error = -1;

    for (int32_t s = 1; s < nstates; s++) {
        n00b_earley_state_t *state = p->states.data[s];

        for (size_t i = 0; i < n00b_list_len(state->items); i++) {
            n00b_earley_item_t *ei = state->items.data[i];

            if (ei->my_penalty > 0) {
                first_error = s;
                goto found_first;
            }
        }
    }

found_first:;

    // Walk backward to find the furthest state with scan-ready items.
    int32_t last_ok = -1;

    for (int32_t s = nstates - 1; s >= 0; s--) {
        n00b_earley_state_t *state = p->states.data[s];

        for (size_t i = 0; i < n00b_list_len(state->items); i++) {
            n00b_earley_item_t *ei = state->items.data[i];

            switch (ei->op) {
            case N00B_EO_SCAN_TOKEN:
            case N00B_EO_SCAN_ANY:
            case N00B_EO_SCAN_CLASS:
            case N00B_EO_SCAN_SET:
                last_ok = s;
                goto found;
            default:
                break;
            }
        }
    }

found:
    if (last_ok < 0) {
        return;
    }

    n00b_grammar_t *g = p->grammar;

    // Use the first error location if available, otherwise the furthest
    // scan-ready position.
    int32_t fail_pos;

    if (first_error >= 0) {
        fail_pos = first_error;
    }
    else {
        fail_pos = last_ok;

        if (last_ok + 1 < nstates) {
            fail_pos = last_ok + 1;
        }
    }

    // The scan state for "expected" tokens is the state BEFORE the failure.
    int32_t scan_pos = fail_pos > 0 ? fail_pos - 1 : 0;

    n00b_earley_state_t *fail_state = p->states.data[fail_pos];
    n00b_earley_state_t *scan_state = p->states.data[scan_pos];

    // "got" comes from the failure position's token.
    if (fail_state->token) {
        out->error_loc.position = fail_pos;
        out->error_loc.line     = fail_state->token->line;
        out->error_loc.column   = fail_state->token->column;
        out->error_loc.got_id   = fail_state->token->tid;

        if (n00b_option_is_set(fail_state->token->value)) {
            out->error_loc.got = n00b_option_get(fail_state->token->value);
        }
    }

    // "expected" comes from the scan items at last_ok — these are the
    // terminals the parser was willing to accept.  When fail_pos ==
    // last_ok, these are the items that didn't match.  When fail_pos
    // == last_ok+1, these show what could have continued the parse.
    n00b_earley_state_t *state = scan_state;

    // Collect expected terminal IDs and descriptions from scan-ready items.
    size_t        nitems   = n00b_list_len(state->items);
    int64_t      *id_buf   = n00b_alloc_array(int64_t, nitems);
    n00b_string_t **desc_buf = n00b_alloc_array(n00b_string_t *, nitems);
    int32_t       count    = 0;

    for (size_t i = 0; i < n00b_list_len(state->items); i++) {
        n00b_earley_item_t *ei = state->items.data[i];

        int64_t       tid  = -1;
        n00b_string_t *desc = NULL;

        n00b_match_t *m = get_rule_match(ei->start_item->rule, ei->cursor);

        switch (ei->op) {
        case N00B_EO_SCAN_TOKEN:
            if (m) {
                tid = m->terminal_id;
                n00b_string_t *name = n00b_get_terminal_name(g, tid);

                if (name) {
                    desc = name;
                }
                else if (p->stream) {
                    // No registered name — find a token in the stream
                    // with this tid and use its text directly.
                    for (int32_t t = 0; t < p->stream->token_count; t++) {
                        n00b_token_info_t *tok = p->stream->tokens[t];

                        if (tok && tok->tid == tid
                            && n00b_option_is_set(tok->value)) {
                            desc = n00b_option_get(tok->value);
                            break;
                        }
                    }
                }
            }

            break;

        case N00B_EO_SCAN_ANY:
            // Use a synthetic ID so it sorts uniquely.
            tid  = -100;
            desc = r"<any>";
            break;

        case N00B_EO_SCAN_CLASS:
            // Use a synthetic ID per class.
            if (m) {
                tid = -(200 + (int64_t)m->char_class);

                switch (m->char_class) {
                case N00B_CC_ID_START:        desc = r"<id_start>";       break;
                case N00B_CC_ID_CONTINUE:     desc = r"<id_continue>";    break;
                case N00B_CC_ASCII_DIGIT:     desc = r"<digit>";          break;
                case N00B_CC_UNICODE_DIGIT:   desc = r"<unicode_digit>";  break;
                case N00B_CC_ASCII_UPPER:     desc = r"<upper>";          break;
                case N00B_CC_ASCII_LOWER:     desc = r"<lower>";          break;
                case N00B_CC_ASCII_ALPHA:     desc = r"<alpha>";          break;
                case N00B_CC_WHITESPACE:      desc = r"<whitespace>";     break;
                case N00B_CC_HEX_DIGIT:       desc = r"<hex_digit>";      break;
                case N00B_CC_NONZERO_DIGIT:   desc = r"<nonzero_digit>";  break;
                case N00B_CC_PRINTABLE:       desc = r"<printable>";      break;
                case N00B_CC_NON_WS_PRINTABLE:   desc = r"<non_ws_printable>"; break;
                case N00B_CC_NON_NL_WS:       desc = r"<non_nl_ws>";      break;
                case N00B_CC_NON_NL_PRINTABLE:   desc = r"<non_nl_printable>"; break;
                case N00B_CC_JSON_STRING_CHAR:   desc = r"<json_string_char>"; break;
                case N00B_CC_REGEX_BODY_CHAR:    desc = r"<regex_body_char>";  break;
                }
            }

            break;

        case N00B_EO_SCAN_SET:
            tid  = -300;
            desc = r"<set>";
            break;

        default:
            break;
        }

        if (tid == -1) {
            continue;
        }

        // Dedup by ID.
        bool dup = false;

        for (int32_t j = 0; j < count; j++) {
            if (id_buf[j] == tid) {
                dup = true;
                break;
            }
        }

        if (!dup) {
            id_buf[count]   = tid;
            desc_buf[count] = desc;
            count++;
        }
    }

    if (count > 0) {
        out->expected_ids   = n00b_alloc_array(int64_t, count);
        out->expected_desc  = n00b_alloc_array(n00b_string_t *, count);
        out->expected_count = count;
        memcpy(out->expected_ids, id_buf, count * sizeof(int64_t));
        memcpy(out->expected_desc, desc_buf, count * sizeof(n00b_string_t *));
    }

    n00b_free(id_buf);
    n00b_free(desc_buf);

    // Collect active NT context names from items at the failure state.
    size_t          fail_nitems = n00b_list_len(fail_state->items);
    n00b_string_t **ctx_buf     = n00b_alloc_array(n00b_string_t *, fail_nitems);
    int32_t         ctx_count   = 0;

    for (size_t i = 0; i < fail_nitems; i++) {
        n00b_earley_item_t *ei  = fail_state->items.data[i];
        n00b_nonterm_t     *nt  = n00b_get_nonterm(g, ei->ruleset_id);

        if (!nt || !nt->name || !nt->name->data) {
            continue;
        }

        // Skip internal group NTs.
        if (nt->group_nt) {
            continue;
        }

        // Dedup by name pointer (same NT → same pointer).
        bool dup = false;

        for (int32_t j = 0; j < ctx_count; j++) {
            if (ctx_buf[j] == nt->name) {
                dup = true;
                break;
            }
        }

        if (!dup) {
            ctx_buf[ctx_count++] = nt->name;
        }
    }

    if (ctx_count > 0) {
        out->active_ctx       = n00b_alloc_array(n00b_string_t *, ctx_count);
        out->active_ctx_count = ctx_count;
        memcpy(out->active_ctx, ctx_buf, ctx_count * sizeof(n00b_string_t *));
    }

    n00b_free(ctx_buf);
}
