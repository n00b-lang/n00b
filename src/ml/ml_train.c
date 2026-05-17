/**
 * @file ml_train.c
 * @brief Online training step + `n00b_ml_trainer_t` facade.
 *
 * Splitting this from `ml_score.c` lets a deployed scorer link only
 * the inference code path and skip the training step entirely.
 */

#include "ml/ml.h"

#include <math.h>

#ifdef N00B_HAVE_VECTOR_BUILTINS
typedef float v8f32 __attribute__((vector_size(32)));
#endif

// ----------------------------------------------------------------------------
// One online step (logistic / binary classification)
// ----------------------------------------------------------------------------

void
n00b_ml_train_step(n00b_ml_model_t     *model,
                   const n00b_ml_vec_t *input_vec,
                   float                target)
    _kargs {
        float learning_rate = 0.01f;
        float weight        = 1.0f;
        float weight_decay  = 0.0f;
    }
{
    float p     = n00b_ml_sigmoid(n00b_ml_score(model, input_vec));
    float error = (p - target) * weight;

    float    eta = learning_rate;
    uint32_t n   = model->weights.length;
    float   *w   = model->weights.data;
    float   *x   = input_vec->data;

    // Weight decay: w *= (1 - eta * weight_decay).  Skipped when
    // weight_decay == 0 because decay == 1 makes it a no-op.
    float decay = 1.0f - eta * weight_decay;

#ifdef N00B_HAVE_VECTOR_BUILTINS
    v8f32 v_eta_err = {eta * error, eta * error, eta * error, eta * error,
                       eta * error, eta * error, eta * error, eta * error};
    v8f32 v_decay   = {decay, decay, decay, decay,
                       decay, decay, decay, decay};
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        v8f32 vw = *(v8f32 *)(w + i);
        v8f32 vx = *(v8f32 *)(x + i);
        vw       = vw * v_decay - v_eta_err * vx;
        *(v8f32 *)(w + i) = vw;
    }
    for (; i < n; i++) w[i] = w[i] * decay - eta * error * x[i];
#else
    for (uint32_t i = 0; i < n; i++) {
        w[i] = w[i] * decay - eta * error * x[i];
    }
#endif

    model->bias -= eta * error;
}

// ----------------------------------------------------------------------------
// Trainer facade
// ----------------------------------------------------------------------------

n00b_ml_trainer_t *
n00b_ml_trainer_new()
    _kargs {
        n00b_ml_rules_t *rules         = nullptr;
        float            learning_rate = 0.01f;
        float            weight_decay  = 0.0f;
        bool             track_matches = false;
        bool             no_lock       = false;
    }
{
    n00b_ml_trainer_t *t = n00b_alloc(n00b_ml_trainer_t);

    if (rules) {
        // Adopt an existing rules registry — the trainer is born
        // sealed because the rules and model dimensions are fixed.
        assert(rules->total_size > 0
               && "trainer_new with existing rules requires non-empty rules");
        t->rules  = rules;
        t->model  = n00b_ml_model_new(rules->total_size);
        t->sealed = true;
    } else {
        t->rules  = n00b_ml_rules_new(.track_matches = track_matches);
        t->model  = NULL;
        t->sealed = false;
    }

    t->learning_rate = learning_rate;
    t->weight_decay  = weight_decay;
    t->observations  = 0;
    t->lock          = no_lock ? nullptr : n00b_data_lock_new();
    return t;
}

n00b_ml_rule_group_id_t
n00b_ml_trainer_define_rule_group(n00b_ml_trainer_t *trainer,
                                  n00b_string_t     *name,
                                  uint32_t           size)
{
    n00b_data_write_lock(trainer->lock);
    assert(!trainer->sealed
           && "trainer_define_rule_group called after the trainer was sealed");
    n00b_ml_rule_group_id_t id
        = n00b_ml_define_rule_group(trainer->rules, name, size);
    n00b_data_unlock(trainer->lock);
    return id;
}

n00b_ml_rule_group_id_t
n00b_ml_trainer_define_rule_group_cstr(n00b_ml_trainer_t *trainer,
                                       const char        *name,
                                       uint32_t           size)
{
    return n00b_ml_trainer_define_rule_group(trainer,
                                             n00b_string_from_cstr(name),
                                             size);
}

void
n00b_ml_trainer_seal(n00b_ml_trainer_t *trainer)
{
    n00b_data_write_lock(trainer->lock);
    if (trainer->sealed) {
        n00b_data_unlock(trainer->lock);
        return;
    }
    assert(trainer->rules->total_size > 0
           && "trainer_seal called before any rule groups were defined");
    trainer->model  = n00b_ml_model_new(trainer->rules->total_size);
    trainer->sealed = true;
    n00b_data_unlock(trainer->lock);
}

void
n00b_ml_trainer_observe(n00b_ml_trainer_t *trainer,
                        n00b_ml_input_t   *input,
                        float              label)
    _kargs { float weight = 1.0f; }
{
    if (!trainer->sealed) {
        n00b_ml_trainer_seal(trainer);
    }
    n00b_data_write_lock(trainer->lock);
    n00b_ml_train_step(trainer->model, input->matches, label,
                       .learning_rate = trainer->learning_rate,
                       .weight        = weight,
                       .weight_decay  = trainer->weight_decay);
    trainer->observations++;
    n00b_data_unlock(trainer->lock);
}

float
n00b_ml_trainer_predict(n00b_ml_trainer_t *trainer,
                        n00b_ml_input_t   *input)
{
    if (!trainer->sealed) {
        n00b_ml_trainer_seal(trainer);
    }
    n00b_data_read_lock(trainer->lock);
    float r = n00b_ml_sigmoid(n00b_ml_score(trainer->model, input->matches));
    n00b_data_unlock(trainer->lock);
    return r;
}
