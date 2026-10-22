#ifndef IR_INTERVAL_H
#define IR_INTERVAL_H

#include "ir/ir.h"
#include "lib/buffer.h"

typedef struct {
    ir_int_t lo;
    ir_int_t hi;
} ir_interval_t;

extern const ir_int_t ir_int_max;
extern const ir_int_t ir_int_min;

void ir_uint_append(core_buffer_t *out, ir_uint_t value);
void ir_int_append(core_buffer_t *out, ir_int_t value);

static inline ir_int_t
ir_int_lesser(ir_int_t a, ir_int_t b)
{
    return a < b ? a : b;
}

static inline ir_int_t
ir_int_greater(ir_int_t a, ir_int_t b)
{
    return a > b ? a : b;
}

ir_interval_t ir_interval_point(ir_int_t value);
ir_interval_t ir_interval_join(ir_interval_t a, ir_interval_t b);
bool          ir_interval_meet(ir_interval_t a, ir_interval_t b,
                               ir_interval_t *out);
bool          ir_interval_contains(ir_interval_t outer, ir_interval_t inner);
ir_interval_t ir_interval_of_width(uint64_t width, bool is_signed);
bool          ir_interval_of_integer_type(ir_module_t *module, th_type *type,
                                          ir_interval_t *out);
bool          ir_interval_fits(ir_interval_t range, uint64_t width,
                               bool is_signed);
ir_interval_t ir_interval_apply(ir_op_t op, ir_interval_t left,
                                ir_interval_t right);
ir_interval_t ir_interval_constrain(ir_interval_t range, ir_op_t comparison,
                                    ir_interval_t other);

#endif
