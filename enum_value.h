#pragma once

#include "ir/ir.h"

typedef struct {
    ir_uint_t magnitude;
    bool negative;
} n00bb_enum_value_t;

typedef struct {
    uint64_t magnitude_bits;
    bool is_signed;
} n00bb_enum_range_t;
