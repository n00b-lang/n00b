#ifndef N00BB_COMPILER_EFFECTS_H
#define N00BB_COMPILER_EFFECTS_H

#include "ir/summary.h"

typedef struct {
    core_string_t name;
    uint64_t      bits;
    bool          group;
} n00bb_effect_name_t;

enum {
    N00BB_ROLE_READS  = 1u << 0,
    N00BB_ROLE_WRITES = 1u << 1,
    N00BB_ROLE_LENDS  = 1u << 2,
};

size_t n00bb_effect_name_count(void);
const n00bb_effect_name_t *n00bb_effect_name_at(size_t index);
const n00bb_effect_name_t *n00bb_effect_named(core_string_t name);
uint64_t n00bb_named_effect_bits(void);
uint8_t n00bb_parameter_role(const ir_func_t *function,
                             const ir_summary_t *summary, uint32_t param);

#endif
