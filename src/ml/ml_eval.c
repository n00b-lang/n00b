/**
 * @file ml_eval.c
 * @brief Held-out evaluation: confusion / F1 / AUC / threshold pick.
 */

#include "ml/ml.h"
#include "adt/heap.h"

#include <math.h>

// ----------------------------------------------------------------------------
// Confusion matrix
// ----------------------------------------------------------------------------

double
n00b_ml_confusion_precision(const n00b_ml_confusion_t *c)
{
    uint64_t denom = c->tp + c->fp;
    return denom == 0 ? 0.0 : (double)c->tp / (double)denom;
}

double
n00b_ml_confusion_recall(const n00b_ml_confusion_t *c)
{
    uint64_t denom = c->tp + c->fn;
    return denom == 0 ? 0.0 : (double)c->tp / (double)denom;
}

double
n00b_ml_confusion_f1(const n00b_ml_confusion_t *c)
{
    double p = n00b_ml_confusion_precision(c);
    double r = n00b_ml_confusion_recall(c);
    if (p + r == 0.0) return 0.0;
    return 2.0 * p * r / (p + r);
}

double
n00b_ml_confusion_accuracy(const n00b_ml_confusion_t *c)
{
    uint64_t total = c->tp + c->tn + c->fp + c->fn;
    return total == 0 ? 0.0 : (double)(c->tp + c->tn) / (double)total;
}

// ----------------------------------------------------------------------------
// Evaluation accumulator
// ----------------------------------------------------------------------------

// Internal record: one (score, label) pair.
typedef struct {
    float score;
    bool  label;
} eval_record_t;

struct n00b_ml_evaluation {
    n00b_list_t(eval_record_t) records;
    n00b_rwlock_t             *lock;
};

n00b_ml_evaluation_t *
n00b_ml_evaluation_new()
    _kargs { bool no_lock = false; }
{
    n00b_ml_evaluation_t *e = n00b_alloc(n00b_ml_evaluation_t);
    e->records              = n00b_list_new(eval_record_t);
    e->lock                 = no_lock ? nullptr : n00b_data_lock_new();
    return e;
}

void
n00b_ml_evaluation_record(n00b_ml_evaluation_t *evaluation,
                          float                 score,
                          bool                  label)
{
    n00b_data_write_lock(evaluation->lock);
    eval_record_t r = {.score = score, .label = label};
    n00b_list_push(evaluation->records, r);
    n00b_data_unlock(evaluation->lock);
}

uint64_t
n00b_ml_evaluation_count(n00b_ml_evaluation_t *evaluation)
{
    n00b_data_read_lock(evaluation->lock);
    uint64_t r = (uint64_t)n00b_list_len(evaluation->records);
    n00b_data_unlock(evaluation->lock);
    return r;
}

n00b_ml_confusion_t
n00b_ml_evaluation_confusion(n00b_ml_evaluation_t *evaluation, float threshold)
{
    n00b_ml_confusion_t c = {0};
    n00b_data_read_lock(evaluation->lock);
    size_t n = n00b_list_len(evaluation->records);
    for (size_t i = 0; i < n; i++) {
        eval_record_t r = n00b_list_get(evaluation->records, i);
        bool predicted = r.score > threshold;
        if (predicted && r.label)        c.tp++;
        else if (predicted && !r.label)  c.fp++;
        else if (!predicted && r.label)  c.fn++;
        else                              c.tn++;
    }
    n00b_data_unlock(evaluation->lock);
    return c;
}

double
n00b_ml_evaluation_log_loss(n00b_ml_evaluation_t *evaluation)
{
    n00b_data_read_lock(evaluation->lock);
    size_t n = n00b_list_len(evaluation->records);
    if (n == 0) {
        n00b_data_unlock(evaluation->lock);
        return 0.0;
    }
    double sum = 0.0;
    const double eps = 1e-9;
    for (size_t i = 0; i < n; i++) {
        eval_record_t r  = n00b_list_get(evaluation->records, i);
        double        s  = (double)r.score;
        if (s < eps)       s = eps;
        if (s > 1.0 - eps) s = 1.0 - eps;
        sum += r.label ? -log(s) : -log(1.0 - s);
    }
    n00b_data_unlock(evaluation->lock);
    return sum / (double)n;
}

// ----------------------------------------------------------------------------
// AUC (rank-formula, ties get average rank)
// ----------------------------------------------------------------------------

static int
score_cmp(const void *a, const void *b)
{
    float as = ((const eval_record_t *)a)->score;
    float bs = ((const eval_record_t *)b)->score;
    return (as > bs) - (as < bs);
}

double
n00b_ml_evaluation_auc(n00b_ml_evaluation_t *evaluation)
{
    n00b_data_read_lock(evaluation->lock);
    size_t n = n00b_list_len(evaluation->records);
    if (n == 0) {
        n00b_data_unlock(evaluation->lock);
        return 0.0;
    }

    eval_record_t *arr = n00b_alloc_array(eval_record_t, n);
    for (size_t i = 0; i < n; i++) arr[i] = n00b_list_get(evaluation->records, i);
    n00b_data_unlock(evaluation->lock);

    qsort(arr, n, sizeof(*arr), score_cmp);

    double rank_sum_pos = 0.0;
    uint64_t pos = 0, neg = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && arr[j].score == arr[i].score) j++;
        double avg_rank = ((double)(i + 1) + (double)j) / 2.0;
        for (size_t k = i; k < j; k++) {
            if (arr[k].label) {
                rank_sum_pos += avg_rank;
                pos++;
            } else {
                neg++;
            }
        }
        i = j;
    }
    if (pos == 0 || neg == 0) return 0.5;
    return (rank_sum_pos - (double)pos * ((double)pos + 1.0) / 2.0)
           / ((double)pos * (double)neg);
}

// ----------------------------------------------------------------------------
// Threshold selection
// ----------------------------------------------------------------------------

float
n00b_ml_evaluation_pick_threshold(n00b_ml_evaluation_t       *evaluation,
                                  n00b_ml_threshold_policy_t  policy)
    _kargs { float target = 0.95f; }
{
    n00b_data_read_lock(evaluation->lock);
    size_t n = n00b_list_len(evaluation->records);
    if (n == 0) {
        n00b_data_unlock(evaluation->lock);
        return 0.5f;
    }
    eval_record_t *arr = n00b_alloc_array(eval_record_t, n);
    for (size_t i = 0; i < n; i++) arr[i] = n00b_list_get(evaluation->records, i);
    n00b_data_unlock(evaluation->lock);

    qsort(arr, n, sizeof(*arr), score_cmp);

    uint64_t total_pos = 0, total_neg = 0;
    for (size_t i = 0; i < n; i++) {
        if (arr[i].label) total_pos++; else total_neg++;
    }

    uint64_t tp = 0, fp = 0;
    float    best_thr   = 0.5f;
    double   best_score = -1.0;

    for (ssize_t k = (ssize_t)n - 1; k >= 0; k--) {
        if (arr[k].label) tp++; else fp++;
        if (k > 0 && arr[k - 1].score == arr[k].score) continue;

        uint64_t fn = total_pos - tp;
        double   precision = (tp + fp) == 0 ? 0.0
                              : (double)tp / (double)(tp + fp);
        double   recall    = (tp + fn) == 0 ? 0.0
                              : (double)tp / (double)(tp + fn);
        double   tpr       = recall;
        double   fpr       = total_neg == 0 ? 0.0
                              : (double)fp / (double)total_neg;
        float thr;
        if (k == 0) {
            thr = arr[k].score - 1e-6f;
        } else {
            thr = (arr[k - 1].score + arr[k].score) * 0.5f;
        }

        double cur = -1.0;
        switch (policy) {
        case N00B_ML_THRESHOLD_BY_F1:
            cur = (precision + recall == 0.0)
                      ? 0.0
                      : 2.0 * precision * recall / (precision + recall);
            break;
        case N00B_ML_THRESHOLD_BY_YOUDEN:
            cur = tpr - fpr;
            break;
        case N00B_ML_THRESHOLD_BY_PRECISION:
            if (precision >= (double)target) {
                cur = recall;
            }
            break;
        case N00B_ML_THRESHOLD_BY_RECALL:
            if (recall >= (double)target) {
                cur = precision;
            }
            break;
        }
        if (cur > best_score) {
            best_score = cur;
            best_thr   = thr;
        }
    }
    return best_thr;
}
