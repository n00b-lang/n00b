#ifndef IR_RANGES_H
#define IR_RANGES_H

#include "ir/ir.h"

typedef struct ir_ranges_t ir_ranges_t;

typedef struct {
    ir_int_t lo;
    ir_int_t hi;
} ir_interval_t;

typedef enum : uint8_t {
    IR_OVERFLOW_NEVER,
    IR_OVERFLOW_MAY,
    IR_OVERFLOW_ALWAYS,
    IR_OVERFLOW_UNKNOWN,
} ir_overflow_t;

ir_ranges_t  *ir_ranges_build(ir_func_t *f);
ir_ranges_t  *ir_func_ranges(ir_func_t *f);
void          ir_ranges_free(ir_ranges_t *ranges);

bool          ir_range_of(ir_ranges_t *ranges, const ir_value_t *value,
                          const ir_instr_t *at, ir_interval_t *out);
bool          ir_range_type_bounds(const ir_func_t *f, const ir_value_t *value,
                                   ir_interval_t *out);
ir_overflow_t ir_range_overflow(ir_ranges_t *ranges, const ir_instr_t *in,
                                ir_interval_t *left, ir_interval_t *right);
bool          ir_range_index_proven(ir_ranges_t *ranges,
                                    const ir_instr_t *index);
bool          ir_range_unsigned_sub_safe(ir_ranges_t *ranges,
                                         const ir_instr_t *sub);

#endif
