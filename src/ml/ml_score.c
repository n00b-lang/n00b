/**
 * @file ml_score.c
 * @brief Read-only @ref n00b_ml_scorer_t for deployed inference.
 *
 * Depends only on `ml_core.c` + `ml_io.c`.  A deployed binary that
 * links those plus this file gets inference + load + classify with
 * no SGD code anywhere.
 */

#include "ml/ml.h"

n00b_ml_scorer_t *
n00b_ml_scorer_new()
    _kargs {
        n00b_ml_trainer_t *from_trainer = nullptr;
        n00b_ml_rules_t   *rules        = nullptr;
        n00b_ml_model_t   *model        = nullptr;
        bool               no_lock      = false;
    }
{
    if (from_trainer) {
        if (!from_trainer->sealed) {
            n00b_ml_trainer_seal(from_trainer);
        }
        rules = from_trainer->rules;
        model = from_trainer->model;
    }
    assert(rules && model
           && "scorer_new requires either .from_trainer or both .rules and .model");

    n00b_ml_scorer_t *s = n00b_alloc(n00b_ml_scorer_t);
    s->rules            = rules;
    s->model            = model;
    s->lock             = no_lock ? nullptr : n00b_data_lock_new();
    return s;
}

float
n00b_ml_scorer_predict(n00b_ml_scorer_t *scorer,
                       n00b_ml_input_t  *input)
{
    n00b_data_read_lock(scorer->lock);
    float r = n00b_ml_sigmoid(n00b_ml_score(scorer->model, input->matches));
    n00b_data_unlock(scorer->lock);
    return r;
}

bool
n00b_ml_scorer_classify(n00b_ml_scorer_t *scorer,
                        n00b_ml_input_t  *input,
                        float             threshold)
{
    return n00b_ml_scorer_predict(scorer, input) > threshold;
}
