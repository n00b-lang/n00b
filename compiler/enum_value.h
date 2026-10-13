#pragma once

#include "ir/ir.h"
#include "lib/arena.h"
#include "parse/symtab.h"

typedef struct {
    ir_uint_t magnitude;
    bool negative;
} n00bb_enum_value_t;

typedef struct {
    uint64_t magnitude_bits;
    bool is_signed;
} n00bb_enum_range_t;

void n00bb_enum_range_include(n00bb_enum_range_t *range,
                              n00bb_enum_value_t value);
bool n00bb_bind_enum_range(th_ctx *types, core_arena_t *arena,
                           parse_sym_entry_t *symbol,
                           n00bb_enum_range_t range);
