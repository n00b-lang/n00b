#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "n00b.h"
#include "core/runtime.h"
#include "ml/ml.h"

static void
test_vec_allocation()
{
    n00b_ml_vec_t *vec = n00b_ml_vec_new(100);
    assert(vec->length == 100);
    assert(vec->data != NULL);

    uintptr_t addr = (uintptr_t)vec->data;
    assert((addr & 31) == 0);

    for (int i = 0; i < 100; i++) {
        assert(vec->data[i] == 0.0f);
    }
}

static void
test_record_match()
{
    n00b_ml_rules_t *rules = n00b_ml_rules_new();
    n00b_ml_rule_group_id_t lex
        = n00b_ml_define_rule_group_cstr(rules, "LEX",  100);
    n00b_ml_rule_group_id_t geom
        = n00b_ml_define_rule_group_cstr(rules, "GEOM", 50);
    assert(rules->total_size == 150);

    n00b_ml_vec_t *vec = n00b_ml_vec_new(rules->total_size);
    n00b_ml_record_match_cstr(rules, vec, lex,  "email");
    n00b_ml_record_match_cstr(rules, vec, geom, "depth_5");

    bool lex_hit = false;
    for (int i = 0; i < 100; i++) {
        if (vec->data[i] > 0) lex_hit = true;
    }
    assert(lex_hit);

    bool geom_hit = false;
    for (int i = 100; i < 150; i++) {
        if (vec->data[i] > 0) geom_hit = true;
    }
    assert(geom_hit);
}

static void
test_linear_model()
{
    uint32_t size = 10;
    n00b_ml_model_t *model = n00b_ml_model_new(size);
    n00b_ml_vec_t   *feat  = n00b_ml_vec_new(size);

    for (uint32_t i = 0; i < size; i++) {
        model->weights.data[i] = 1.0f;
        feat->data[i]          = 1.0f;
    }
    model->bias = 5.0f;

    float pred = n00b_ml_score(model, feat);
    assert(fabsf(pred - 15.0f) < 1e-5);
}

// Trainer facade: train sentiment-style toy classifier and assert
// separation > 0.9 / < 0.1 on training data.
static void
test_trainer_facade()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_rule_group_id_t word
        = n00b_ml_trainer_define_rule_group_cstr(t, "WORD", 64);

    const char *cls_pos[] = {"excellent", "great", "amazing"};
    const char *cls_neg[] = {"terrible", "awful", "bad"};

    n00b_ml_input_t *input = n00b_ml_input_new(t->rules);
    for (int epoch = 0; epoch < 200; epoch++) {
        for (int i = 0; i < 3; i++) {
            n00b_ml_input_reset(input);
            n00b_ml_input_match_cstr(input, word, cls_pos[i]);
            n00b_ml_trainer_observe(t, input, 1.0f);

            n00b_ml_input_reset(input);
            n00b_ml_input_match_cstr(input, word, cls_neg[i]);
            n00b_ml_trainer_observe(t, input, 0.0f);
        }
    }

    n00b_ml_input_reset(input);
    n00b_ml_input_match_cstr(input, word, cls_pos[0]);
    assert(n00b_ml_trainer_predict(t, input) > 0.9f);

    n00b_ml_input_reset(input);
    n00b_ml_input_match_cstr(input, word, cls_neg[0]);
    assert(n00b_ml_trainer_predict(t, input) < 0.1f);
}

static void
test_scorer_from_trainer()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_rule_group_id_t f
        = n00b_ml_trainer_define_rule_group_cstr(t, "F", 16);
    n00b_ml_input_t *x = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(x, f, "hit");
    for (int i = 0; i < 50; i++) {
        n00b_ml_trainer_observe(t, x, 1.0f);
    }

    n00b_ml_scorer_t *s = n00b_ml_scorer_new(.from_trainer = t);
    float trainer_p     = n00b_ml_trainer_predict(t, x);
    float scorer_p      = n00b_ml_scorer_predict(s, x);
    assert(fabsf(trainer_p - scorer_p) < 1e-6f);

    // Classify with a 0.5 threshold.
    assert(n00b_ml_scorer_classify(s, x, 0.5f) == true);
}

static void
test_save_load_roundtrip()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_rule_group_id_t lex
        = n00b_ml_trainer_define_rule_group_cstr(t, "LEX",  64);
    n00b_ml_rule_group_id_t flow
        = n00b_ml_trainer_define_rule_group_cstr(t, "FLOW", 32);

    n00b_ml_input_t *x = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(x, lex,  "email");
    n00b_ml_input_match_cstr(x, flow, "source->sink");
    for (int i = 0; i < 80; i++) {
        n00b_ml_trainer_observe(t, x, 1.0f);
    }
    float trainer_p = n00b_ml_trainer_predict(t, x);

    n00b_buffer_t *blob = n00b_ml_save(t->rules, t->model);
    assert(blob->byte_len > 24);

    n00b_result_t(n00b_ml_scorer_t *) loaded = n00b_ml_scorer_load(blob);
    assert(n00b_result_is_ok(loaded));
    n00b_ml_scorer_t *s = n00b_result_get(loaded);

    n00b_ml_rule_group_id_t lex2, flow2;
    assert(n00b_ml_lookup_rule_group(s->rules,
                                     n00b_string_from_cstr("LEX"),  &lex2));
    assert(n00b_ml_lookup_rule_group(s->rules,
                                     n00b_string_from_cstr("FLOW"), &flow2));

    n00b_ml_input_t *x2 = n00b_ml_input_new(s->rules);
    n00b_ml_input_match_cstr(x2, lex2,  "email");
    n00b_ml_input_match_cstr(x2, flow2, "source->sink");
    float scorer_p = n00b_ml_scorer_predict(s, x2);

    assert(fabsf(trainer_p - scorer_p) < 1e-5f);
}

static void
test_load_rejects_corruption()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new();
    n00b_ml_rule_group_id_t s_id
        = n00b_ml_trainer_define_rule_group_cstr(t, "S", 8);
    n00b_ml_input_t *x = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(x, s_id, "v");
    n00b_ml_trainer_observe(t, x, 1.0f);

    n00b_buffer_t *good = n00b_ml_save(t->rules, t->model);

    n00b_buffer_t *bad_magic = n00b_buffer_from_bytes(good->data,
                                                     (int64_t)good->byte_len);
    bad_magic->data[0] ^= 0xff;
    n00b_result_t(n00b_ml_scorer_t *) r1 = n00b_ml_scorer_load(bad_magic);
    assert(n00b_result_is_err(r1));
    assert(n00b_result_get_err(r1) == N00B_ML_ERR_BAD_MAGIC);
    assert(n00b_ml_err_str(N00B_ML_ERR_BAD_MAGIC) != nullptr);

    n00b_buffer_t *bad_ver = n00b_buffer_from_bytes(good->data,
                                                   (int64_t)good->byte_len);
    bad_ver->data[4] = 0xff;
    n00b_result_t(n00b_ml_scorer_t *) r2 = n00b_ml_scorer_load(bad_ver);
    assert(n00b_result_get_err(r2) == N00B_ML_ERR_BAD_VERSION);

    n00b_buffer_t *trunc = n00b_buffer_from_bytes(good->data, 10);
    n00b_result_t(n00b_ml_scorer_t *) r3 = n00b_ml_scorer_load(trunc);
    assert(n00b_result_get_err(r3) == N00B_ML_ERR_TRUNCATED);
}

// strongest_rules returns the most-frequent value per rule (with
// match-tracking on).
static void
test_strongest_rules()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f,
                                               .track_matches = true);
    n00b_ml_rule_group_id_t lex
        = n00b_ml_trainer_define_rule_group_cstr(t, "LEX",  64);
    n00b_ml_rule_group_id_t flow
        = n00b_ml_trainer_define_rule_group_cstr(t, "FLOW", 32);

    n00b_ml_input_t *pos = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(pos, lex,  "email");
    n00b_ml_input_match_cstr(pos, flow, "source->sink");

    n00b_ml_input_t *neg = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(neg, lex, "uninteresting");

    for (int i = 0; i < 100; i++) {
        n00b_ml_trainer_observe(t, pos, 1.0f);
        n00b_ml_trainer_observe(t, neg, 0.0f);
    }

    n00b_list_t(n00b_ml_learned_rule_t) ranked
        = n00b_ml_strongest_rules(t->model, t->rules, 5);
    assert(n00b_list_len(ranked) > 0);
    assert(n00b_list_len(ranked) <= 5);
    float prev = 1.0f / 0.0f;
    for (size_t i = 0; i < n00b_list_len(ranked); i++) {
        n00b_ml_learned_rule_t s = n00b_list_get(ranked, i);
        assert(fabsf(s.weight) <= prev + 1e-6f);
        prev = fabsf(s.weight);
        assert(s.most_common_match != nullptr);
        assert(s.match_count > 0);
    }
}

// Drift detection: in-distribution traffic produces small residuals;
// flipping labels makes residuals and drift jump sharply.
static void
test_monitor_drift_detection()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_rule_group_id_t w
        = n00b_ml_trainer_define_rule_group_cstr(t, "W", 32);
    n00b_ml_input_t *xg = n00b_ml_input_new(t->rules);
    n00b_ml_input_t *xb = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(xg, w, "good");
    n00b_ml_input_match_cstr(xb, w, "bad");
    for (int i = 0; i < 100; i++) {
        n00b_ml_trainer_observe(t, xg, 1.0f);
        n00b_ml_trainer_observe(t, xb, 0.0f);
    }

    n00b_buffer_t *blob = n00b_ml_save(t->rules, t->model);
    n00b_ml_scorer_t *frozen
        = n00b_result_get(n00b_ml_scorer_load(blob));

    n00b_ml_monitor_t *m = n00b_ml_monitor_new(frozen,
                                               .recency_weight       = 0.2,
                                               .shadow_learning_rate = 0.5f);

    n00b_ml_input_t *xg_mon = n00b_ml_input_new(frozen->rules);
    n00b_ml_input_t *xb_mon = n00b_ml_input_new(frozen->rules);
    n00b_ml_input_match_cstr(xg_mon, w, "good");
    n00b_ml_input_match_cstr(xb_mon, w, "bad");

    for (int i = 0; i < 100; i++) {
        n00b_ml_monitor_observe(m, xg_mon, 1.0f);
        n00b_ml_monitor_observe(m, xb_mon, 0.0f);
    }
    double good_residual = n00b_ml_monitor_residual_ewma(m);
    double good_drift    = n00b_ml_monitor_weight_drift(m);
    assert(good_residual < 0.05);

    for (int i = 0; i < 100; i++) {
        n00b_ml_monitor_observe(m, xg_mon, 0.0f);
        n00b_ml_monitor_observe(m, xb_mon, 1.0f);
    }
    double bad_residual = n00b_ml_monitor_residual_ewma(m);
    double bad_drift    = n00b_ml_monitor_weight_drift(m);
    assert(bad_residual > good_residual * 5.0);
    assert(bad_drift    > good_drift    * 2.0);
}

// Correctable model: feedback shifts the corrected prediction but leaves
// the base unchanged.
static void
test_correctable_feedback()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_rule_group_id_t w
        = n00b_ml_trainer_define_rule_group_cstr(t, "W", 32);
    n00b_ml_input_t *xg = n00b_ml_input_new(t->rules);
    n00b_ml_input_t *xb = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(xg, w, "good");
    n00b_ml_input_match_cstr(xb, w, "bad");
    for (int i = 0; i < 100; i++) {
        n00b_ml_trainer_observe(t, xg, 1.0f);
        n00b_ml_trainer_observe(t, xb, 0.0f);
    }

    n00b_ml_correctable_t *c
        = n00b_ml_trainer_freeze(t, .feedback_learning_rate = 0.5f);

    n00b_ml_input_t *xg_c = n00b_ml_input_new(c->base->rules);
    n00b_ml_input_match_cstr(xg_c, w, "good");

    float corrected_p0 = n00b_ml_correctable_predict(c, xg_c);
    float base_p0      = n00b_ml_scorer_predict(c->base, xg_c);
    assert(fabsf(corrected_p0 - base_p0) < 1e-6f);
    assert(corrected_p0 > 0.9f);

    for (int i = 0; i < 30; i++) {
        n00b_ml_correctable_correct(c, xg_c, N00B_ML_FB_FALSE_POSITIVE);
    }
    float corrected_p1 = n00b_ml_correctable_predict(c, xg_c);
    float base_p1      = n00b_ml_scorer_predict(c->base, xg_c);
    assert(corrected_p1 < corrected_p0);
    assert(fabsf(base_p1 - base_p0) < 1e-6f);

    // classify uses the threshold.
    assert(n00b_ml_correctable_classify(c, xg_c, 0.5f)
           == (corrected_p1 > 0.5f));
}

static void
test_correctable_save_load()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new();
    n00b_ml_rule_group_id_t lex
        = n00b_ml_trainer_define_rule_group_cstr(t, "LEX", 32);
    n00b_ml_input_t *x = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(x, lex, "hello");
    for (int i = 0; i < 50; i++) {
        n00b_ml_trainer_observe(t, x, 1.0f);
    }
    n00b_ml_correctable_t *c
        = n00b_ml_trainer_freeze(t, .feedback_learning_rate = 0.5f);
    for (int i = 0; i < 10; i++) {
        n00b_ml_correctable_correct(c, x, N00B_ML_FB_FALSE_POSITIVE);
    }
    float pre = n00b_ml_correctable_predict(c, x);

    n00b_buffer_t *blob = n00b_ml_correctable_save(c);
    n00b_result_t(n00b_ml_correctable_t *) reloaded
        = n00b_ml_correctable_load(blob);
    assert(n00b_result_is_ok(reloaded));
    n00b_ml_correctable_t *c2 = n00b_result_get(reloaded);

    n00b_ml_rule_group_id_t lex2;
    assert(n00b_ml_lookup_rule_group(c2->base->rules,
                                     n00b_string_from_cstr("LEX"), &lex2));
    n00b_ml_input_t *x2 = n00b_ml_input_new(c2->base->rules);
    n00b_ml_input_match_cstr(x2, lex2, "hello");
    float post = n00b_ml_correctable_predict(c2, x2);
    assert(fabsf(pre - post) < 1e-5f);

    // Single-layer scorer reads the same blob.
    n00b_ml_scorer_t *base_only
        = n00b_result_get(n00b_ml_scorer_load(blob));
    float base_p     = n00b_ml_scorer_predict(base_only, x2);
    float exp_base_p = n00b_ml_scorer_predict(c->base, x2);
    assert(fabsf(base_p - exp_base_p) < 1e-5f);
}

// Trainer round-trip: save→load preserves hyperparameters and
// observation count, and resumed training continues to converge.
static void
test_trainer_save_load()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(
        .learning_rate = 0.3f, .weight_decay = 1e-4f);
    n00b_ml_rule_group_id_t f
        = n00b_ml_trainer_define_rule_group_cstr(t, "F", 16);
    n00b_ml_input_t *x = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(x, f, "v");

    for (int i = 0; i < 25; i++) {
        n00b_ml_trainer_observe(t, x, 1.0f);
    }
    float trainer_p_before = n00b_ml_trainer_predict(t, x);
    uint64_t obs_before     = t->observations;

    n00b_buffer_t *blob = n00b_ml_trainer_save(t);
    n00b_result_t(n00b_ml_trainer_t *) r = n00b_ml_trainer_load(blob);
    assert(n00b_result_is_ok(r));
    n00b_ml_trainer_t *t2 = n00b_result_get(r);

    assert(t2->observations == obs_before);
    assert(fabsf(t2->learning_rate - 0.3f) < 1e-9f);
    assert(fabsf(t2->weight_decay - 1e-4f) < 1e-9f);

    // Prediction matches.
    n00b_ml_rule_group_id_t f2;
    assert(n00b_ml_lookup_rule_group(t2->rules,
                                     n00b_string_from_cstr("F"), &f2));
    n00b_ml_input_t *x2 = n00b_ml_input_new(t2->rules);
    n00b_ml_input_match_cstr(x2, f2, "v");
    float trainer_p_after = n00b_ml_trainer_predict(t2, x2);
    assert(fabsf(trainer_p_before - trainer_p_after) < 1e-5f);

    // A scorer can read a trainer blob too (just sees the base).
    n00b_ml_scorer_t *s = n00b_result_get(n00b_ml_scorer_load(blob));
    assert(fabsf(n00b_ml_scorer_predict(s, x2) - trainer_p_after) < 1e-5f);

    // Loading a non-trainer blob as a trainer fails cleanly.
    n00b_buffer_t *plain = n00b_ml_save(t->rules, t->model);
    n00b_result_t(n00b_ml_trainer_t *) r2 = n00b_ml_trainer_load(plain);
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == N00B_ML_ERR_NOT_TRAINER);
}

// Class imbalance via .weight kwarg: a heavy positive weight pushes the
// model toward predicting positive even with majority-negative training.
static void
test_class_imbalance_weight()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.3f);
    n00b_ml_rule_group_id_t f
        = n00b_ml_trainer_define_rule_group_cstr(t, "F", 16);
    n00b_ml_input_t *xp = n00b_ml_input_new(t->rules);
    n00b_ml_input_t *xn = n00b_ml_input_new(t->rules);
    n00b_ml_input_match_cstr(xp, f, "pos");
    n00b_ml_input_match_cstr(xn, f, "neg");

    // 10 negatives for every positive — heavy imbalance.
    for (int epoch = 0; epoch < 200; epoch++) {
        for (int i = 0; i < 10; i++) {
            n00b_ml_trainer_observe(t, xn, 0.0f);
        }
        // Up-weight the positive sample by 10× to compensate.
        n00b_ml_trainer_observe(t, xp, 1.0f, .weight = 10.0f);
    }

    assert(n00b_ml_trainer_predict(t, xp) > 0.5f);
    assert(n00b_ml_trainer_predict(t, xn) < 0.5f);
}

// L2 weight decay: with weight_decay > 0, weights stay smaller.
static void
test_weight_decay()
{
    n00b_ml_trainer_t *plain = n00b_ml_trainer_new(.learning_rate = 0.5f);
    n00b_ml_trainer_t *reg   = n00b_ml_trainer_new(.learning_rate = 0.5f,
                                                   .weight_decay  = 0.05f);
    n00b_ml_trainer_define_rule_group_cstr(plain, "F", 8);
    n00b_ml_rule_group_id_t f
        = n00b_ml_trainer_define_rule_group_cstr(reg, "F", 8);
    n00b_ml_input_t *xp = n00b_ml_input_new(plain->rules);
    n00b_ml_input_t *xr = n00b_ml_input_new(reg->rules);
    n00b_ml_input_match_cstr(xp, f, "v");
    n00b_ml_input_match_cstr(xr, f, "v");

    for (int i = 0; i < 50; i++) {
        n00b_ml_trainer_observe(plain, xp, 1.0f);
        n00b_ml_trainer_observe(reg,   xr, 1.0f);
    }

    // Sum |w| of regularized model is smaller.
    float sum_plain = 0.0f, sum_reg = 0.0f;
    for (uint32_t i = 0; i < plain->model->weights.length; i++) {
        sum_plain += fabsf(plain->model->weights.data[i]);
    }
    for (uint32_t i = 0; i < reg->model->weights.length; i++) {
        sum_reg += fabsf(reg->model->weights.data[i]);
    }
    assert(sum_reg < sum_plain);
}

// Eval / metrics / threshold selection.
static void
test_eval_and_threshold()
{
    n00b_ml_evaluation_t *e = n00b_ml_evaluation_new();

    // Synthetic perfectly-separable predictions.
    float pos_scores[] = {0.95f, 0.92f, 0.88f, 0.83f, 0.81f};
    float neg_scores[] = {0.10f, 0.20f, 0.25f, 0.34f, 0.40f};
    for (size_t i = 0; i < sizeof(pos_scores)/sizeof(pos_scores[0]); i++) {
        n00b_ml_evaluation_record(e, pos_scores[i], true);
        n00b_ml_evaluation_record(e, neg_scores[i], false);
    }

    assert(n00b_ml_evaluation_count(e) == 10);

    // At threshold 0.5: all positives above, all negatives below.
    n00b_ml_confusion_t c = n00b_ml_evaluation_confusion(e, 0.5f);
    assert(c.tp == 5 && c.tn == 5 && c.fp == 0 && c.fn == 0);
    assert(fabs(n00b_ml_confusion_precision(&c) - 1.0) < 1e-9);
    assert(fabs(n00b_ml_confusion_recall(&c)    - 1.0) < 1e-9);
    assert(fabs(n00b_ml_confusion_f1(&c)        - 1.0) < 1e-9);
    assert(fabs(n00b_ml_confusion_accuracy(&c)  - 1.0) < 1e-9);

    // Perfectly separable data → AUC = 1.0.
    assert(fabs(n00b_ml_evaluation_auc(e) - 1.0) < 1e-9);

    // Maximum-F1 threshold should land between the two clusters and give
    // a perfect F1 of 1.0.
    float thr_f1 = n00b_ml_evaluation_pick_threshold(
        e, N00B_ML_THRESHOLD_BY_F1);
    assert(thr_f1 > 0.4f && thr_f1 < 0.81f);
    n00b_ml_confusion_t cf = n00b_ml_evaluation_confusion(e, thr_f1);
    assert(fabs(n00b_ml_confusion_f1(&cf) - 1.0) < 1e-9);

    // Target-recall = 1.0 should pick a low threshold (catches all
    // positives).
    float thr_recall = n00b_ml_evaluation_pick_threshold(
        e, N00B_ML_THRESHOLD_BY_RECALL, .target = 1.0f);
    n00b_ml_confusion_t cr = n00b_ml_evaluation_confusion(e, thr_recall);
    assert(cr.fn == 0);
}

// PII preset registers four named rule groups with the right names.
static void
test_pii_preset()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new();
    n00b_ml_pii_rule_groups_t s = n00b_ml_pii_register_rule_groups(t);
    // IDs are assigned in registration order.
    assert(s.lex == 0 && s.flow == 1 && s.geom == 2 && s.env == 3);
    n00b_ml_rule_group_id_t found;
    assert(n00b_ml_lookup_rule_group(t->rules,
                                     n00b_string_from_cstr("LEX"), &found)
           && found == s.lex);
    assert(n00b_ml_lookup_rule_group(t->rules,
                                     n00b_string_from_cstr("ENV"), &found)
           && found == s.env);
}

// Per-rule matches — ensure the most-frequent value is reported.
static void
test_matches_for_rule()
{
    n00b_ml_trainer_t *t = n00b_ml_trainer_new(.track_matches = true);
    n00b_ml_rule_group_id_t f
        = n00b_ml_trainer_define_rule_group_cstr(t, "F", 4);

    n00b_ml_input_t *s = n00b_ml_input_new(t->rules);

    // "alpha" three times, "beta" once.
    for (int i = 0; i < 3; i++) {
        n00b_ml_input_reset(s);
        n00b_ml_input_match_cstr(s, f, "alpha");
        n00b_ml_trainer_observe(t, s, 1.0f);
    }
    n00b_ml_input_reset(s);
    n00b_ml_input_match_cstr(s, f, "beta");
    n00b_ml_trainer_observe(t, s, 0.0f);

    // Walk all rules in group 0 and find the one with "alpha".
    bool found_alpha = false;
    for (uint32_t r = 0; r < 4; r++) {
        n00b_list_t(n00b_ml_match_t) lst
            = n00b_ml_matches_for_rule(t->rules, f, r, 4);
        for (size_t i = 0; i < n00b_list_len(lst); i++) {
            n00b_ml_match_t e = n00b_list_get(lst, i);
            if (e.value && e.value->u8_bytes == 5
                && memcmp(e.value->data, "alpha", 5) == 0) {
                assert(e.count == 3);
                found_alpha = true;
            }
        }
    }
    assert(found_alpha);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_vec_allocation();
    test_record_match();
    test_linear_model();
    test_trainer_facade();
    test_scorer_from_trainer();
    test_save_load_roundtrip();
    test_load_rejects_corruption();
    test_strongest_rules();
    test_monitor_drift_detection();
    test_correctable_feedback();
    test_correctable_save_load();
    test_trainer_save_load();
    test_class_imbalance_weight();
    test_weight_decay();
    test_eval_and_threshold();
    test_pii_preset();
    test_matches_for_rule();

    printf("All ML tests passed.\n");
    n00b_shutdown();
    return 0;
}
