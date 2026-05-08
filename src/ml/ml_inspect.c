/**
 * @file ml_inspect.c
 * @brief Strongest-rules ranking and per-rule match drill-down.
 */

#include "ml/ml.h"
#include "adt/heap.h"

#include <math.h>

typedef struct {
    uint32_t index;
    float    magnitude;
    float    signed_value;
} weight_entry_t;

static int
weight_min_cmp(const void *a, const void *b)
{
    float am = ((const weight_entry_t *)a)->magnitude;
    float bm = ((const weight_entry_t *)b)->magnitude;
    return (am > bm) - (am < bm);
}

// Find which rule group owns the global weight index `idx`.
static n00b_ml_rule_group_t
locate_group(const n00b_ml_rules_t *rules,
             uint32_t               idx,
             uint32_t              *out_group_id)
{
    size_t n = n00b_list_len(rules->groups);
    for (uint32_t i = 0; i < n; i++) {
        n00b_ml_rule_group_t g = n00b_list_get(rules->groups, i);
        if (idx >= g.offset && idx < g.offset + g.size) {
            *out_group_id = i;
            return g;
        }
    }
    *out_group_id = (uint32_t)-1;
    return (n00b_ml_rule_group_t){0};
}

// Most-frequent recorded match value for a rule, if match-tracking
// is enabled.  Returns nullptr otherwise.
static n00b_string_t *
most_common_match(const n00b_ml_rules_t *rules,
                  uint32_t               group_id,
                  uint32_t               rule_id,
                  uint32_t              *out_count)
{
    *out_count = 0;
    if (!rules->match_log) return nullptr;
    uint64_t key = ((uint64_t)group_id << 32) | (uint64_t)rule_id;
    bool     found;
    n00b_dict_t(n00b_string_t *, uint32_t) *vd
        = n00b_dict_get(rules->match_log, key, &found);
    if (!found || !vd) return nullptr;

    n00b_string_t *best        = nullptr;
    uint32_t       best_count  = 0;
    n00b_dict_foreach(vd, k, v, {
        if (v > best_count) {
            best_count = v;
            best       = k;
        }
    });
    *out_count = best_count;
    return best;
}

n00b_list_t(n00b_ml_learned_rule_t)
n00b_ml_strongest_rules(const n00b_ml_model_t *model,
                        const n00b_ml_rules_t *rules,
                        uint32_t               k)
{
    n00b_list_t(n00b_ml_learned_rule_t) out
        = n00b_list_new(n00b_ml_learned_rule_t);
    if (k == 0 || model->weights.length == 0) {
        return out;
    }

    n00b_heap_t(weight_entry_t) heap = n00b_heap_new(weight_entry_t,
                                                    weight_min_cmp);

    uint32_t n = model->weights.length;
    float   *w = model->weights.data;

    for (uint32_t i = 0; i < n; i++) {
        float    s   = w[i];
        float    mag = fabsf(s);
        if (mag == 0.0f) continue;
        weight_entry_t e = {.index = i, .magnitude = mag, .signed_value = s};
        if (n00b_heap_len(heap) < k) {
            n00b_heap_push(heap, e);
        } else {
            weight_entry_t dropped;
            n00b_heap_pushpop(heap, e, &dropped);
        }
    }

    size_t          got = n00b_heap_len(heap);
    weight_entry_t *buf = n00b_alloc_array(weight_entry_t, got);
    for (size_t i = 0; i < got; i++) {
        n00b_heap_pop(heap, &buf[i]);
    }
    for (size_t i = got; i > 0; i--) {
        weight_entry_t e = buf[i - 1];
        uint32_t       gid;
        n00b_ml_rule_group_t group = locate_group(rules, e.index, &gid);

        uint32_t       match_count;
        n00b_string_t *match
            = most_common_match(rules, gid, e.index - group.offset,
                                &match_count);

        n00b_ml_learned_rule_t row = {
            .group_name        = group.name,
            .rule_id           = e.index - group.offset,
            .weight            = e.signed_value,
            .most_common_match = match,
            .match_count       = match_count,
        };
        n00b_list_push(out, row);
    }
    return out;
}

// ----------------------------------------------------------------------------
// Per-rule: top-K most-common match values
// ----------------------------------------------------------------------------

static int
match_min_cmp(const void *a, const void *b)
{
    uint32_t ac = ((const n00b_ml_match_t *)a)->count;
    uint32_t bc = ((const n00b_ml_match_t *)b)->count;
    return (ac > bc) - (ac < bc);
}

n00b_list_t(n00b_ml_match_t)
n00b_ml_matches_for_rule(n00b_ml_rules_t         *rules,
                         n00b_ml_rule_group_id_t  group_id,
                         uint32_t                 rule_id,
                         uint32_t                 k)
{
    n00b_list_t(n00b_ml_match_t) out
        = n00b_list_new(n00b_ml_match_t);
    if (!rules->match_log || k == 0) return out;

    uint64_t key = ((uint64_t)group_id << 32) | (uint64_t)rule_id;
    bool     found;
    n00b_dict_t(n00b_string_t *, uint32_t) *vd
        = n00b_dict_get(rules->match_log, key, &found);
    if (!found || !vd) return out;

    n00b_heap_t(n00b_ml_match_t) heap
        = n00b_heap_new(n00b_ml_match_t, match_min_cmp);
    n00b_dict_foreach(vd, str, cnt, {
        n00b_ml_match_t e;
        e.value = str;
        e.count = cnt;
        if (n00b_heap_len(heap) < k) {
            n00b_heap_push(heap, e);
        } else {
            n00b_ml_match_t dropped;
            n00b_heap_pushpop(heap, e, &dropped);
        }
    });

    size_t got                       = n00b_heap_len(heap);
    n00b_ml_match_t *buf             = n00b_alloc_array(n00b_ml_match_t, got);
    for (size_t i = 0; i < got; i++) {
        n00b_heap_pop(heap, &buf[i]);
    }
    for (size_t i = got; i > 0; i--) {
        n00b_list_push(out, buf[i - 1]);
    }
    return out;
}
