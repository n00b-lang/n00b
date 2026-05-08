/**
 * @file crayon_features.h
 * @brief Rule extraction + auto-labeling for warehouse events.
 *
 * Walks the parsed n00b JSON view of a warehouse event and records its
 * salient fields as matches against the four PII-style rule groups
 * (LEX/FLOW/GEOM/ENV) registered on a trainer.  Auto-labels each event
 * by reading `actor.classification.categories`.
 *
 * Used by the dev-activity binary classifier (and is structured so the
 * follow-on API-CRUD and secret-in-argv demos can extend the rule mix
 * with their own).
 */
#pragma once

#ifdef __APPLE__

#include <stdbool.h>
#include "parsers/json.h"
#include "ml/ml.h"

/**
 * @brief Rule-group IDs for the four PII-style groups.
 *
 * Populated by @ref crayon_features_register_rule_groups.
 */
typedef struct {
    n00b_ml_rule_group_id_t lex;
    n00b_ml_rule_group_id_t flow;
    n00b_ml_rule_group_id_t geom;
    n00b_ml_rule_group_id_t env;
} crayon_features_t;

/**
 * @brief Register the four rule groups on @p trainer with sane sizes.
 *
 * Sizes follow `pii-api.md`'s recommendations: LEX 16K, FLOW 4K,
 * GEOM 256, ENV 1K.
 */
crayon_features_t
crayon_features_register_rule_groups(n00b_ml_trainer_t *trainer);

/**
 * @brief Record warehouse-event rule matches into an input.
 *
 * @param input     Input to populate (caller resets it first).
 * @param ids       Rule-group IDs from
 *                  @ref crayon_features_register_rule_groups.
 * @param event     Parsed event JSON tree.
 *
 * The event is expected to have at least an `actor` sub-object; events
 * without one (e.g. host-level rollups) record nothing and the caller
 * should skip them.
 *
 * @return true if any rule matches were recorded; false if @p event
 *         has no actor sub-object.
 */
bool crayon_features_project(n00b_ml_input_t         *input,
                             const crayon_features_t *ids,
                             n00b_json_node_t        *event);

/**
 * @brief Read the actor classification bitmask off an event.
 *
 * @param event  Parsed event JSON tree.
 * @param out    Receives the bitmask on success.
 * @return true if the bitmask was found, false if absent or malformed.
 */
bool crayon_features_classification(n00b_json_node_t *event, uint64_t *out);

/**
 * @brief Auto-label an event for the dev-activity classifier.
 *
 * @return true (positive) if the actor's classification bitmask has
 *         AI or EDITOR set; false (negative) otherwise.
 */
bool crayon_features_dev_activity_label(uint64_t classification_mask);

#endif // __APPLE__
