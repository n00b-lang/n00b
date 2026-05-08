/**
 * @file ml.h
 * @brief Online binary classifier for n00b.
 *
 * The user proposes "rules" — string-typed observations about an
 * input ("the exe basename is `vim`", "the path depth is 7+", "the
 * signing identity is `com.apple.vim`").  The library learns a
 * weight per rule from labeled examples and exposes a probability
 * via @ref n00b_ml_scorer_predict at inference time.  See
 * `docs/ml.md` for the conceptual walkthrough.
 *
 * Modules:
 *
 *   - **Primitives** (`ml_core.c`): rule groups, rules, models,
 *     inputs, raw inference.
 *   - **Training** (`ml_train.c`): one online step (`train_step`)
 *     plus the @ref n00b_ml_trainer_t facade.
 *   - **Scoring** (`ml_score.c`): read-only @ref n00b_ml_scorer_t.
 *   - **Persistence** (`ml_io.c`): versioned save/load.
 *   - **Inspection** (`ml_inspect.c`): strongest-rules ranking +
 *     per-rule match drill-down.
 *   - **Drift monitor** (`ml_monitor.c`): @ref n00b_ml_monitor_t.
 *   - **Correctable model** (`ml_layered.c`): frozen base + small
 *     trainable correction layer for in-field feedback.
 *   - **Evaluation** (`ml_eval.c`): confusion / F1 / AUC /
 *     threshold selection.
 *   - **Presets** (`ml_presets.c`): common rule-group layouts (PII).
 *
 * Concurrency: every facade type carries an optional
 * `n00b_rwlock_t *lock`.  Default constructors allocate one; pass
 * `.no_lock = true` for single-threaded use.
 */

#pragma once

#include "n00b.h"
#include "adt/list.h"
#include "adt/dict.h"
#include "adt/result.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/hash.h"
#include "core/stats.h"
#include "core/data_lock.h"

#include <math.h>

// ============================================================================
// Feature Vectors (internal representation; users rarely touch these)
// ============================================================================

/**
 * @brief SIMD-aligned weight or rule-match vector.
 */
typedef struct {
    float   *data;   /**< Aligned to 32-bytes for AVX2. */
    uint32_t length; /**< Number of float elements. */
} n00b_ml_vec_t;

extern n00b_ml_vec_t *n00b_ml_vec_new(uint32_t length)
    _kargs { n00b_allocator_t *allocator = nullptr; };

extern n00b_ml_vec_t *n00b_ml_vec_copy(const n00b_ml_vec_t *src)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ============================================================================
// Rules: groups, ids, registry
// ============================================================================

/**
 * @brief Identifier for a rule group within a @ref n00b_ml_rules_t.
 *
 * Returned by @ref n00b_ml_define_rule_group.  Use it on every
 * @ref n00b_ml_input_match call — it's faster than looking up by
 * name.
 */
typedef uint32_t n00b_ml_rule_group_id_t;

/**
 * @brief One rule group (e.g., "LEX", "FLOW", "GEOM", "ENV").
 *
 * A rule group owns a fixed-size pool of rules, each addressed by
 * an in-pool id (`0..size-1`).  When you propose a rule with a value
 * like `"vim"`, the value is hashed into that pool to pick a rule.
 */
typedef struct {
    n00b_string_t *name;   /**< Group name (e.g. "LEX"). */
    uint32_t       offset; /**< Start index of this group's rules in the global vector. */
    uint32_t       size;   /**< Number of rules in the pool. */
} n00b_ml_rule_group_t;

/**
 * @brief Per-bucket value frequency in match-tracking mode.
 */
typedef struct {
    n00b_string_t *value;
    uint32_t       count;
} n00b_ml_match_t;

/**
 * @brief Registry of rule groups for an `(rules, model)` pair.
 *
 * @c match_log is only populated when `track_matches` was set at
 * construction time; in production it stays null and projection
 * skips the bookkeeping.  Key encodes
 * `(rule_group_id << 32) | rule_id`.
 */
typedef struct {
    n00b_list_t(n00b_ml_rule_group_t) groups;
    n00b_dict_t(n00b_string_t *, uint32_t) group_ids;
    uint32_t total_size;
    n00b_dict_t(uint64_t,
                n00b_dict_t(n00b_string_t *, uint32_t) *) *match_log;
} n00b_ml_rules_t;

extern n00b_ml_rules_t *n00b_ml_rules_new()
    _kargs {
        n00b_allocator_t *allocator     = nullptr;
        bool              track_matches = false;
    };

/**
 * @brief Declare a rule group; returns its id for use in
 *        @ref n00b_ml_input_match.
 */
extern n00b_ml_rule_group_id_t
n00b_ml_define_rule_group(n00b_ml_rules_t *rules,
                          n00b_string_t   *name,
                          uint32_t         size);

/** @brief Convenience: name as a C string. */
extern n00b_ml_rule_group_id_t
n00b_ml_define_rule_group_cstr(n00b_ml_rules_t *rules,
                               const char      *name,
                               uint32_t         size);

/** @brief Look up an existing group by name (used at load time). */
extern bool n00b_ml_lookup_rule_group(n00b_ml_rules_t         *rules,
                                      n00b_string_t           *name,
                                      n00b_ml_rule_group_id_t *out_id);

/**
 * @brief Record a rule match into a vector.  Lower-level than
 *        @ref n00b_ml_input_match; usually you'll use the input form.
 *
 * @kw weight  Increment to add to the rule's slot (default 1.0).
 */
extern void n00b_ml_record_match(n00b_ml_rules_t         *rules,
                                 n00b_ml_vec_t           *vec,
                                 n00b_ml_rule_group_id_t  group,
                                 n00b_string_t           *value)
    _kargs { float weight = 1.0f; };

/** @brief As @ref n00b_ml_record_match but @p value is a C string. */
extern void n00b_ml_record_match_cstr(n00b_ml_rules_t         *rules,
                                      n00b_ml_vec_t           *vec,
                                      n00b_ml_rule_group_id_t  group,
                                      const char              *value)
    _kargs { float weight = 1.0f; };

// ============================================================================
// Linear Models
// ============================================================================

typedef struct {
    n00b_ml_vec_t weights;
    float         bias;
} n00b_ml_model_t;

extern n00b_ml_model_t *n00b_ml_model_new(uint32_t length)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Compute the raw linear score `(W · X) + b`.  No sigmoid. */
extern float n00b_ml_score(const n00b_ml_model_t *model,
                           const n00b_ml_vec_t   *input_vec);

/** @brief `1 / (1 + exp(-score))`. */
static inline float n00b_ml_sigmoid(float score) {
    return 1.0f / (1.0f + expf(-score));
}

// ============================================================================
// Inputs (one input = one set of rule matches)
// ============================================================================

/**
 * @brief A vector of rule matches representing one input we want
 *        to train on or score.
 *
 * Allocate once, fill with @ref n00b_ml_input_match calls, then pass
 * to @ref n00b_ml_trainer_observe / @ref n00b_ml_scorer_predict /
 * etc.  Reusable across observations via @ref n00b_ml_input_reset.
 */
typedef struct {
    n00b_ml_rules_t *rules;    /**< Borrowed; outlives the input. */
    n00b_ml_vec_t   *matches;  /**< Owned. */
} n00b_ml_input_t;

extern n00b_ml_input_t *n00b_ml_input_new(n00b_ml_rules_t *rules)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Record a rule match: "I propose rule `(group, value)` for
 *        this input."
 *
 * @kw weight  How strongly this rule fired (default 1.0).
 */
extern void n00b_ml_input_match(n00b_ml_input_t         *input,
                                n00b_ml_rule_group_id_t  group,
                                n00b_string_t           *value)
    _kargs { float weight = 1.0f; };

/** @brief As @ref n00b_ml_input_match but @p value is a C string. */
extern void n00b_ml_input_match_cstr(n00b_ml_input_t         *input,
                                     n00b_ml_rule_group_id_t  group,
                                     const char              *value)
    _kargs { float weight = 1.0f; };

/** @brief Zero the input's match vector for reuse on the next observation. */
extern void n00b_ml_input_reset(n00b_ml_input_t *input);

// ============================================================================
// Online Learning — one training step
// ============================================================================

/**
 * @brief One online SGD step on a raw model.  Most callers use the
 *        trainer.  The library is binary classification only;
 *        squared error is not supported here.
 *
 * @kw learning_rate  Step size η (default 0.01).
 * @kw weight         Per-sample weight, e.g. for class imbalance
 *                    (default 1.0).
 * @kw weight_decay   L2 weight-decay coefficient (default 0.0 = none).
 *                    Larger values keep weights from growing
 *                    unboundedly when feature counts are sparse.
 */
extern void n00b_ml_train_step(n00b_ml_model_t     *model,
                               const n00b_ml_vec_t *input_vec,
                               float                target)
    _kargs {
        float learning_rate = 0.01f;
        float weight        = 1.0f;
        float weight_decay  = 0.0f;
    };

// ============================================================================
// Trainer facade
// ============================================================================

/**
 * @brief Owns rules + model + training hyperparameters.
 *
 * Lifecycle: `new → define_rule_group × N → first observe seals it
 * → repeated observe/predict`.  After sealing,
 * @ref n00b_ml_define_rule_group on the trainer's rules will assert.
 *
 * All public methods auto-acquire `lock` when non-null.  Pass
 * `.no_lock = true` for single-threaded use.
 */
typedef struct {
    n00b_ml_rules_t *rules;
    n00b_ml_model_t *model;          /**< Allocated on seal. */
    float            learning_rate;
    float            weight_decay;
    uint64_t         observations;
    bool             sealed;
    n00b_rwlock_t   *lock;
} n00b_ml_trainer_t;

/**
 * @brief Construct a trainer.
 *
 * @kw rules          Optional existing @ref n00b_ml_rules_t to adopt.
 *                    nullptr (default) creates a fresh registry.
 * @kw learning_rate  SGD step size (default 0.01).
 * @kw weight_decay   L2 coefficient (default 0.0).
 * @kw track_matches  Track rule-match counts for inspection (debug
 *                    builds, demos).  Default false.  Ignored if
 *                    you pass an existing `.rules`.
 * @kw no_lock        Skip rwlock allocation (single-threaded use).
 */
extern n00b_ml_trainer_t *n00b_ml_trainer_new()
    _kargs {
        n00b_ml_rules_t *rules         = nullptr;
        float            learning_rate = 0.01f;
        float            weight_decay  = 0.0f;
        bool             track_matches = false;
        bool             no_lock       = false;
    };

extern n00b_ml_rule_group_id_t
n00b_ml_trainer_define_rule_group(n00b_ml_trainer_t *trainer,
                                  n00b_string_t     *name,
                                  uint32_t           size);

extern n00b_ml_rule_group_id_t
n00b_ml_trainer_define_rule_group_cstr(n00b_ml_trainer_t *trainer,
                                       const char        *name,
                                       uint32_t           size);

/** @brief Lock the rules and allocate the model.  Idempotent. */
extern void n00b_ml_trainer_seal(n00b_ml_trainer_t *trainer);

/**
 * @brief One online step against a labeled input.
 *
 * @kw weight  Per-sample weight (default 1.0).  Use this for class
 *             imbalance: scale rare-class observations up.
 */
extern void n00b_ml_trainer_observe(n00b_ml_trainer_t *trainer,
                                    n00b_ml_input_t   *input,
                                    float              label)
    _kargs { float weight = 1.0f; };

/** @brief Probability of the positive class for an input (0..1). */
extern float n00b_ml_trainer_predict(n00b_ml_trainer_t *trainer,
                                     n00b_ml_input_t   *input);

// ============================================================================
// Scorer facade (read-only / deploy)
// ============================================================================

typedef struct {
    n00b_ml_rules_t *rules; /**< Borrowed. */
    n00b_ml_model_t *model; /**< Borrowed. */
    n00b_rwlock_t   *lock;
} n00b_ml_scorer_t;

/**
 * @brief Construct a scorer.
 *
 * Pass either `.from_trainer` (borrows the trainer's rules+model)
 * or both `.rules` and `.model`.
 */
extern n00b_ml_scorer_t *n00b_ml_scorer_new()
    _kargs {
        n00b_ml_trainer_t *from_trainer = nullptr;
        n00b_ml_rules_t   *rules        = nullptr;
        n00b_ml_model_t   *model        = nullptr;
        bool               no_lock      = false;
    };

extern float n00b_ml_scorer_predict(n00b_ml_scorer_t *scorer,
                                    n00b_ml_input_t  *input);

/**
 * @brief Binary classification: `predict(input) > threshold`.
 */
extern bool n00b_ml_scorer_classify(n00b_ml_scorer_t *scorer,
                                    n00b_ml_input_t  *input,
                                    float             threshold);

// ============================================================================
// Persistence (versioned binary blob)
// ============================================================================

typedef enum {
    N00B_ML_ERR_BAD_MAGIC      = 1,
    N00B_ML_ERR_BAD_VERSION    = 2,
    N00B_ML_ERR_TRUNCATED      = 3,
    N00B_ML_ERR_BAD_HEADER     = 4,
    N00B_ML_ERR_NOT_TRAINER    = 5,
} n00b_ml_err_t;

extern const char *n00b_ml_err_str(n00b_ml_err_t err);

/**
 * @brief Serialize an `(rules, model)` pair to a self-describing blob.
 *
 * Format: little-endian, fixed header followed by a rule-group
 * table and the raw weights array.  See `internal/ml/blob_format.h`
 * for the byte layout.
 */
extern n00b_buffer_t *n00b_ml_save(const n00b_ml_rules_t *rules,
                                   const n00b_ml_model_t *model);

extern n00b_buffer_t *n00b_ml_trainer_save(n00b_ml_trainer_t *trainer);

extern n00b_result_t(n00b_ml_trainer_t *)
n00b_ml_trainer_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; };

extern n00b_buffer_t *n00b_ml_scorer_save(n00b_ml_scorer_t *scorer);

extern n00b_result_t(n00b_ml_scorer_t *)
n00b_ml_scorer_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; };

// ============================================================================
// Inspection
// ============================================================================

/**
 * @brief One row of "what rules did the model learn from?"
 */
typedef struct {
    n00b_string_t *group_name;          /**< Rule group name. */
    uint32_t       rule_id;             /**< Rule index within the group. */
    float          weight;              /**< Learned weight (signed). */
    n00b_string_t *most_common_match;   /**< First-seen match value, or null. */
    uint32_t       match_count;         /**< Times the match was seen, or 0. */
} n00b_ml_learned_rule_t;

/**
 * @brief Turn on per-rule match tracking after construction.
 *        Costs memory.  Idempotent.
 */
extern void n00b_ml_rules_track_matches(n00b_ml_rules_t *rules);

/**
 * @brief Return the @p k highest-magnitude rules, sorted by
 *        |weight| descending.  Both classes' rules can appear in
 *        the result.
 */
extern n00b_list_t(n00b_ml_learned_rule_t)
n00b_ml_strongest_rules(const n00b_ml_model_t *model,
                        const n00b_ml_rules_t *rules,
                        uint32_t               k);

/**
 * @brief Top-@p k most-common match values for one rule.
 *        Empty list when match-tracking is off or the rule has no
 *        recorded hits.
 */
extern n00b_list_t(n00b_ml_match_t)
n00b_ml_matches_for_rule(n00b_ml_rules_t         *rules,
                         n00b_ml_rule_group_id_t  group,
                         uint32_t                 rule_id,
                         uint32_t                 k);

// ============================================================================
// Drift monitor
// ============================================================================

typedef struct {
    n00b_ml_scorer_t     *deployed;       /**< Borrowed; not modified. */
    n00b_ml_trainer_t    *shadow_trainer; /**< Owned; shares deployed->rules. */
    n00b_running_stats_t  residual;
    n00b_ewma_t           residual_ewma;
    n00b_rwlock_t        *lock;
} n00b_ml_monitor_t;

/**
 * @brief Watch a deployed scorer for drift.
 *
 * @kw recency_weight        EWMA smoothing factor (default 0.05).
 *                           Larger reacts faster to recent events;
 *                           smaller smooths more.
 * @kw shadow_learning_rate  SGD step size for the shadow trainer
 *                           (default 0.01).
 */
extern n00b_ml_monitor_t *n00b_ml_monitor_new(n00b_ml_scorer_t *deployed)
    _kargs {
        double recency_weight        = 0.05;
        float  shadow_learning_rate  = 0.01f;
        bool   no_lock               = false;
    };

extern void n00b_ml_monitor_observe(n00b_ml_monitor_t *monitor,
                                    n00b_ml_input_t   *input,
                                    float              label);

/** @brief Long-term mean squared error over all observations. */
extern double n00b_ml_monitor_residual_mean(const n00b_ml_monitor_t *monitor);

/** @brief Fast-reacting EWMA of squared error. */
extern double n00b_ml_monitor_residual_ewma(const n00b_ml_monitor_t *monitor);

/**
 * @brief L2 distance between deployed and shadow weights.
 *
 * Read together with the residuals: drift alone is just an activity
 * counter; drift + climbing residual is "the world has shifted."
 */
extern double n00b_ml_monitor_weight_drift(const n00b_ml_monitor_t *monitor);

// ============================================================================
// Correctable model (frozen base + small trainable correction layer)
// ============================================================================

typedef enum {
    N00B_ML_FB_FALSE_POSITIVE = 0, /**< Model said "yes", should have been "no". */
    N00B_ML_FB_FALSE_NEGATIVE = 1, /**< Model said "no", should have been "yes". */
} n00b_ml_feedback_t;

/**
 * @brief A deployed scorer plus a small "correction layer" you can
 *        update from feedback in the field.
 *
 *      `prediction = sigmoid( score(base) + score(correction) )`
 *
 * The base never changes; the correction layer absorbs site-specific
 * fixes via SGD with a small learning rate.  Inference cost is two
 * SIMD dot products on the same input vector.
 */
typedef struct {
    n00b_ml_scorer_t *base;                    /**< Borrowed; frozen. */
    n00b_ml_model_t  *correction;              /**< Owned; same shape as base->model. */
    float             feedback_learning_rate;
    n00b_rwlock_t    *lock;
} n00b_ml_correctable_t;

extern n00b_ml_correctable_t *
n00b_ml_correctable_new(n00b_ml_scorer_t *base)
    _kargs {
        float feedback_learning_rate = 0.001f;
        bool  no_lock                = false;
    };

/**
 * @brief Convenience: snapshot a trainer and wrap it as a
 *        correctable whose correction layer starts at zero.
 */
extern n00b_ml_correctable_t *
n00b_ml_trainer_freeze(n00b_ml_trainer_t *trainer)
    _kargs {
        float feedback_learning_rate = 0.001f;
        bool  no_lock                = false;
    };

extern float n00b_ml_correctable_predict(n00b_ml_correctable_t *correctable,
                                         n00b_ml_input_t       *input);

extern bool n00b_ml_correctable_classify(n00b_ml_correctable_t *correctable,
                                         n00b_ml_input_t       *input,
                                         float                  threshold);

/**
 * @brief Apply user feedback as one SGD step on the correction layer.
 *        The base scorer is not touched.
 */
extern void n00b_ml_correctable_correct(n00b_ml_correctable_t *correctable,
                                        n00b_ml_input_t       *input,
                                        n00b_ml_feedback_t     kind);

/** @brief Strongest rules using `(base + correction)` weights. */
extern n00b_list_t(n00b_ml_learned_rule_t)
n00b_ml_correctable_strongest_rules(n00b_ml_correctable_t *correctable,
                                    uint32_t               k);

extern n00b_buffer_t *n00b_ml_correctable_save(n00b_ml_correctable_t *correctable);
extern n00b_result_t(n00b_ml_correctable_t *)
n00b_ml_correctable_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; };

// ============================================================================
// Evaluation: confusion / F1 / AUC / threshold selection
// ============================================================================

/** @brief Counts at one decision threshold. */
typedef struct {
    uint64_t tp; /**< True positives. */
    uint64_t fp; /**< False positives. */
    uint64_t tn; /**< True negatives. */
    uint64_t fn; /**< False negatives. */
} n00b_ml_confusion_t;

extern double n00b_ml_confusion_precision(const n00b_ml_confusion_t *c);
extern double n00b_ml_confusion_recall   (const n00b_ml_confusion_t *c);
extern double n00b_ml_confusion_f1       (const n00b_ml_confusion_t *c);
extern double n00b_ml_confusion_accuracy (const n00b_ml_confusion_t *c);

/**
 * @brief Accumulator for held-out `(score, label)` pairs.  Feed
 *        predictions on a held-out set, then call any of the metric
 *        / threshold-selection helpers below.
 */
typedef struct n00b_ml_evaluation n00b_ml_evaluation_t;

extern n00b_ml_evaluation_t *n00b_ml_evaluation_new()
    _kargs { bool no_lock = false; };

extern void n00b_ml_evaluation_record(n00b_ml_evaluation_t *evaluation,
                                      float                 score,
                                      bool                  label);

extern uint64_t n00b_ml_evaluation_count(n00b_ml_evaluation_t *evaluation);

/** @brief Confusion at a chosen threshold. */
extern n00b_ml_confusion_t
n00b_ml_evaluation_confusion(n00b_ml_evaluation_t *evaluation,
                             float                 threshold);

/** @brief Log loss over all recorded observations. */
extern double n00b_ml_evaluation_log_loss(n00b_ml_evaluation_t *evaluation);

/** @brief Area under the ROC curve.  O(n log n). */
extern double n00b_ml_evaluation_auc(n00b_ml_evaluation_t *evaluation);

typedef enum {
    N00B_ML_THRESHOLD_BY_F1,         /**< Maximize F1. */
    N00B_ML_THRESHOLD_BY_YOUDEN,     /**< Maximize TPR − FPR. */
    N00B_ML_THRESHOLD_BY_PRECISION,  /**< Smallest threshold meeting `target` precision. */
    N00B_ML_THRESHOLD_BY_RECALL,     /**< Largest threshold meeting `target` recall. */
} n00b_ml_threshold_policy_t;

extern float
n00b_ml_evaluation_pick_threshold(n00b_ml_evaluation_t       *evaluation,
                                  n00b_ml_threshold_policy_t  policy)
    _kargs { float target = 0.95f; };

// ============================================================================
// PII preset — common rule-group layout from `pii-api.md`
// ============================================================================

/** @brief Rule-group ids returned from @ref n00b_ml_pii_register_rule_groups. */
typedef struct {
    n00b_ml_rule_group_id_t lex;
    n00b_ml_rule_group_id_t flow;
    n00b_ml_rule_group_id_t geom;
    n00b_ml_rule_group_id_t env;
} n00b_ml_pii_rule_groups_t;

/**
 * @brief Register the LEX/FLOW/GEOM/ENV rule groups on @p trainer.
 *
 * Sizes follow the design doc: LEX 16K, FLOW 4K, GEOM 256, ENV 1K.
 */
extern n00b_ml_pii_rule_groups_t
n00b_ml_pii_register_rule_groups(n00b_ml_trainer_t *trainer);
