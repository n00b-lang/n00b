#ifndef IR_VN_H
#define IR_VN_H

#include "ir/interval.h"
#include "ir/ir.h"

typedef struct ir_value_numbers_t ir_value_numbers_t;
typedef struct ir_memssa_t ir_memssa_t;

ir_value_numbers_t *ir_value_numbers_build(ir_func_t *f, ir_memssa_t *memssa);
ir_value_numbers_t *ir_func_value_numbers(ir_func_t *f);
void                ir_value_numbers_free(ir_value_numbers_t *numbers);

uint32_t ir_value_number(const ir_value_numbers_t *numbers,
                         const ir_value_t *value);
bool     ir_value_numbers_same(const ir_value_numbers_t *numbers,
                               const ir_value_t *a, const ir_value_t *b);
bool     ir_value_number_constant(const ir_value_numbers_t *numbers,
                                  uint32_t number, ir_int_t *value);
uint32_t ir_value_number_count(const ir_value_numbers_t *numbers);

#endif
