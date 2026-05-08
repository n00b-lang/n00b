/**
 * @file ml_layered.c
 * @brief `n00b_ml_correctable_t` — frozen base + small correction layer.
 */

#include "ml/ml.h"
#include "internal/ml/blob_format.h"

#include <math.h>

extern size_t
_n00b_ml_base_blob_length(const n00b_ml_rules_t *rules,
                          const n00b_ml_model_t *model);

extern bool
_n00b_ml_load_base(n00b_buffer_t    *blob,
                   n00b_ml_rules_t **out_rules,
                   n00b_ml_model_t **out_model,
                   uint16_t         *out_flags,
                   n00b_ml_err_t    *out_err);

n00b_ml_correctable_t *
n00b_ml_correctable_new(n00b_ml_scorer_t *base)
    _kargs {
        float feedback_learning_rate = 0.001f;
        bool  no_lock                = false;
    }
{
    n00b_ml_correctable_t *c = n00b_alloc(n00b_ml_correctable_t);
    c->base                  = base;
    c->correction            = n00b_ml_model_new(base->rules->total_size);
    c->feedback_learning_rate = feedback_learning_rate;
    c->lock                  = no_lock ? nullptr : n00b_data_lock_new();
    return c;
}

n00b_ml_correctable_t *
n00b_ml_trainer_freeze(n00b_ml_trainer_t *trainer)
    _kargs {
        float feedback_learning_rate = 0.001f;
        bool  no_lock                = false;
    }
{
    if (!trainer->sealed) {
        n00b_ml_trainer_seal(trainer);
    }

    // Snapshot the trainer's weights into a fresh model so the
    // correctable is independent of any subsequent training.
    n00b_data_read_lock(trainer->lock);
    n00b_ml_model_t *snap
        = n00b_ml_model_new(trainer->rules->total_size);
    memcpy(snap->weights.data,
           trainer->model->weights.data,
           (size_t)trainer->model->weights.length * sizeof(float));
    snap->bias = trainer->model->bias;
    n00b_data_unlock(trainer->lock);

    n00b_ml_scorer_t *base
        = n00b_ml_scorer_new(.rules   = trainer->rules,
                             .model   = snap,
                             .no_lock = no_lock);
    return n00b_ml_correctable_new(base,
                                   .feedback_learning_rate = feedback_learning_rate,
                                   .no_lock                = no_lock);
}

float
n00b_ml_correctable_predict(n00b_ml_correctable_t *correctable,
                            n00b_ml_input_t       *input)
{
    n00b_data_read_lock(correctable->lock);
    float r = n00b_ml_sigmoid(
        n00b_ml_score(correctable->base->model, input->matches)
        + n00b_ml_score(correctable->correction,  input->matches));
    n00b_data_unlock(correctable->lock);
    return r;
}

bool
n00b_ml_correctable_classify(n00b_ml_correctable_t *correctable,
                             n00b_ml_input_t       *input,
                             float                  threshold)
{
    return n00b_ml_correctable_predict(correctable, input) > threshold;
}

void
n00b_ml_correctable_correct(n00b_ml_correctable_t *correctable,
                            n00b_ml_input_t       *input,
                            n00b_ml_feedback_t     kind)
{
    float target = (kind == N00B_ML_FB_FALSE_NEGATIVE) ? 1.0f : 0.0f;
    n00b_data_write_lock(correctable->lock);
    float raw   = n00b_ml_score(correctable->base->model, input->matches)
                 + n00b_ml_score(correctable->correction,  input->matches);
    float p     = n00b_ml_sigmoid(raw);
    float error = p - target;
    float eta   = correctable->feedback_learning_rate;

    uint32_t n = correctable->correction->weights.length;
    float   *w = correctable->correction->weights.data;
    float   *x = input->matches->data;
    for (uint32_t i = 0; i < n; i++) {
        w[i] -= eta * error * x[i];
    }
    correctable->correction->bias -= eta * error;
    n00b_data_unlock(correctable->lock);
}

n00b_list_t(n00b_ml_learned_rule_t)
n00b_ml_correctable_strongest_rules(n00b_ml_correctable_t *correctable,
                                    uint32_t               k)
{
    n00b_data_read_lock(correctable->lock);
    uint32_t n  = correctable->base->model->weights.length;
    float   *cw = n00b_alloc_array(float, n);
    float   *bw = correctable->base->model->weights.data;
    float   *dw = correctable->correction->weights.data;
    for (uint32_t i = 0; i < n; i++) {
        cw[i] = bw[i] + dw[i];
    }
    n00b_ml_model_t combined = {
        .weights = {.data = cw, .length = n},
        .bias    = correctable->base->model->bias + correctable->correction->bias,
    };
    n00b_list_t(n00b_ml_learned_rule_t) r
        = n00b_ml_strongest_rules(&combined, correctable->base->rules, k);
    n00b_data_unlock(correctable->lock);
    return r;
}

// ----------------------------------------------------------------------------
// Save / Load
// ----------------------------------------------------------------------------

n00b_buffer_t *
n00b_ml_correctable_save(n00b_ml_correctable_t *correctable)
{
    n00b_data_read_lock(correctable->lock);
    n00b_buffer_t *blob = n00b_ml_save(correctable->base->rules,
                                       correctable->base->model);

    uint16_t flags;
    memcpy(&flags, blob->data + 6, 2);
    flags |= N00B_ML_BLOB_FLAG_HAS_DELTA;
    memcpy(blob->data + 6, &flags, 2);

    size_t old_len     = blob->byte_len;
    size_t weights_sz  = (size_t)correctable->correction->weights.length * sizeof(float);
    size_t delta_bytes = sizeof(float) + weights_sz;

    n00b_buffer_resize(blob, old_len + delta_bytes);
    char *p = blob->data + old_len;

    memcpy(p, &correctable->correction->bias, sizeof(float));
    p += sizeof(float);
    memcpy(p, correctable->correction->weights.data, weights_sz);
    n00b_data_unlock(correctable->lock);
    return blob;
}

n00b_result_t(n00b_ml_correctable_t *)
n00b_ml_correctable_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; }
{
    n00b_ml_rules_t *rules;
    n00b_ml_model_t *model;
    uint16_t         flags;
    n00b_ml_err_t    err;
    if (!_n00b_ml_load_base(blob, &rules, &model, &flags, &err)) {
        return n00b_result_err(n00b_ml_correctable_t *, err);
    }
    n00b_ml_scorer_t *scorer = n00b_ml_scorer_new(.rules   = rules,
                                                  .model   = model,
                                                  .no_lock = no_lock);

    n00b_ml_correctable_t *c = n00b_ml_correctable_new(scorer,
                                                       .no_lock = no_lock);

    if (flags & N00B_ML_BLOB_FLAG_HAS_DELTA) {
        size_t base_len   = _n00b_ml_base_blob_length(rules, model);
        size_t weights_sz = (size_t)rules->total_size * sizeof(float);
        size_t need       = base_len + sizeof(float) + weights_sz;
        if (blob->byte_len < need) {
            return n00b_result_err(n00b_ml_correctable_t *, N00B_ML_ERR_TRUNCATED);
        }
        const char *p = blob->data + base_len;
        memcpy(&c->correction->bias, p, sizeof(float));
        p += sizeof(float);
        memcpy(c->correction->weights.data, p, weights_sz);
    }

    return n00b_result_ok(n00b_ml_correctable_t *, c);
}
