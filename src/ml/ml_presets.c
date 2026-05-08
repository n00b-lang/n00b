/**
 * @file ml_presets.c
 * @brief Common rule-group layouts.  Currently the PII set from `pii-api.md`.
 */

#include "ml/ml.h"

n00b_ml_pii_rule_groups_t
n00b_ml_pii_register_rule_groups(n00b_ml_trainer_t *trainer)
{
    n00b_ml_pii_rule_groups_t s = {
        .lex  = n00b_ml_trainer_define_rule_group_cstr(trainer, "LEX",  1u << 14),
        .flow = n00b_ml_trainer_define_rule_group_cstr(trainer, "FLOW", 1u << 12),
        .geom = n00b_ml_trainer_define_rule_group_cstr(trainer, "GEOM", 256),
        .env  = n00b_ml_trainer_define_rule_group_cstr(trainer, "ENV",  1u << 10),
    };
    return s;
}
