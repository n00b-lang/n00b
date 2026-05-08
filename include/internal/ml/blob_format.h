/**
 * @file internal/ml/blob_format.h
 * @brief On-disk layout of an n00b_ml_save() blob.  Internal-only.
 *
 * Both `ml_io.c` (single-layer) and `ml_layered.c` (base + correction)
 * need access to the same header layout and flag bits.  Public callers
 * go through `n00b_ml_save` / `n00b_ml_scorer_load` /
 * `n00b_ml_correctable_save` / `n00b_ml_correctable_load` — they don't
 * see this file.
 */
#pragma once

#include <stdint.h>
#include <assert.h>

#define N00B_ML_BLOB_MAGIC   ((uint32_t)('N') | ((uint32_t)'M' << 8) \
                              | ((uint32_t)'L' << 16) | ((uint32_t)'B' << 24))
#define N00B_ML_BLOB_VERSION ((uint16_t)1)

/** @brief Layered (base + delta) blob.  Set in @c flags. */
#define N00B_ML_BLOB_FLAG_HAS_DELTA         ((uint16_t)0x0001)
/** @brief Trainer-state tail present.  Set in @c flags. */
#define N00B_ML_BLOB_FLAG_HAS_TRAINER_STATE ((uint16_t)0x0002)

/**
 * @brief Trainer-state tail layout (all little-endian).
 *
 * Written after the (optional) delta tail when
 * `N00B_ML_BLOB_FLAG_HAS_TRAINER_STATE` is set.
 */
typedef struct {
    uint32_t loss;          /**< n00b_ml_loss_t. */
    float    learning_rate;
    float    l2;
    uint32_t reserved;      /**< Zero. */
    uint64_t observations;
} n00b_ml_blob_trainer_state_t;

static_assert(sizeof(n00b_ml_blob_trainer_state_t) == 24,
              "ml blob trainer state must be exactly 24 bytes");

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t num_subspaces;
    uint32_t total_size;
    uint32_t reserved;
    float    bias;
} n00b_ml_blob_header_t;

static_assert(sizeof(n00b_ml_blob_header_t) == 24,
              "ml blob header must be exactly 24 bytes");
