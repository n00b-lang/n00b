/**
 * @file ml_monitor.c
 * @brief Drift monitor: residual stats + shadow trainer over a
 *        deployed scorer.
 */

#include "ml/ml.h"

#include <math.h>

n00b_ml_monitor_t *
n00b_ml_monitor_new(n00b_ml_scorer_t *deployed)
    _kargs {
        double recency_weight       = 0.05;
        float  shadow_learning_rate = 0.01f;
        bool   no_lock              = false;
    }
{
    n00b_ml_monitor_t *m = n00b_alloc(n00b_ml_monitor_t);
    m->deployed          = deployed;
    m->shadow_trainer    = n00b_ml_trainer_new(.rules         = deployed->rules,
                                                .learning_rate = shadow_learning_rate,
                                                .no_lock       = no_lock);
    n00b_running_stats_init(&m->residual);
    n00b_ewma_init(&m->residual_ewma, recency_weight);
    m->lock = no_lock ? nullptr : n00b_data_lock_new();

    // Initialize shadow weights to match the deployed model so
    // weight_drift starts at zero and grows only as the data diverges.
    uint32_t n = deployed->model->weights.length;
    memcpy(m->shadow_trainer->model->weights.data,
           deployed->model->weights.data,
           (size_t)n * sizeof(float));
    m->shadow_trainer->model->bias = deployed->model->bias;

    return m;
}

void
n00b_ml_monitor_observe(n00b_ml_monitor_t *monitor,
                        n00b_ml_input_t   *input,
                        float              label)
{
    float p = n00b_ml_scorer_predict(monitor->deployed, input);
    n00b_data_write_lock(monitor->lock);
    double err = (double)p - (double)label;
    double sq  = err * err;
    n00b_running_stats_observe(&monitor->residual, sq);
    n00b_ewma_observe(&monitor->residual_ewma, sq);
    n00b_data_unlock(monitor->lock);
    n00b_ml_trainer_observe(monitor->shadow_trainer, input, label);
}

double
n00b_ml_monitor_residual_mean(const n00b_ml_monitor_t *monitor)
{
    n00b_data_read_lock(monitor->lock);
    double r = n00b_running_stats_mean(&monitor->residual);
    n00b_data_unlock(monitor->lock);
    return r;
}

double
n00b_ml_monitor_residual_ewma(const n00b_ml_monitor_t *monitor)
{
    n00b_data_read_lock(monitor->lock);
    double r = n00b_ewma_value(&monitor->residual_ewma);
    n00b_data_unlock(monitor->lock);
    return r;
}

double
n00b_ml_monitor_weight_drift(const n00b_ml_monitor_t *monitor)
{
    n00b_data_read_lock(monitor->shadow_trainer->lock);
    n00b_data_read_lock(monitor->deployed->lock);
    uint32_t n   = monitor->deployed->model->weights.length;
    float   *a   = monitor->deployed->model->weights.data;
    float   *b   = monitor->shadow_trainer->model->weights.data;
    double   sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double d  = (double)a[i] - (double)b[i];
        sum      += d * d;
    }
    double bias_d = (double)monitor->deployed->model->bias
                    - (double)monitor->shadow_trainer->model->bias;
    sum += bias_d * bias_d;
    n00b_data_unlock(monitor->deployed->lock);
    n00b_data_unlock(monitor->shadow_trainer->lock);
    return sqrt(sum);
}
