#ifndef N00BB_FLAGSET_VALUE_H
#define N00BB_FLAGSET_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"

typedef struct {
    ir_uint_t mask;
    uint32_t declaration_order;
    bool atomic;
} n00bb_flagset_value_t;

typedef struct {
    ir_uint_t all_mask;
    uint32_t atomic_count;
} n00bb_flagset_info_t;

#endif
