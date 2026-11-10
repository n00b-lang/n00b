#ifndef IR_ABCD_H
#define IR_ABCD_H

#include "ir/interval.h"
#include "ir/ir.h"
#include "ir/sites.h"

typedef struct ir_abcd_t ir_abcd_t;

typedef enum : uint8_t {
    IR_OVERFLOW_NEVER,
    IR_OVERFLOW_MAY,
    IR_OVERFLOW_ALWAYS,
    IR_OVERFLOW_UNKNOWN,
} ir_overflow_t;

typedef struct {
    ir_op_t           op;
    const ir_value_t *left;
    const ir_value_t *right;
    bool              trapping;
} ir_abcd_view_t;

typedef struct {
    void *context;
    bool (*view)(void *context, const ir_instr_t *in, ir_abcd_view_t *out);
} ir_abcd_hooks_t;

typedef struct {
    ir_interval_t seen;
    bool          unbounded_min;
    bool          unbounded_max;
} ir_abcd_range_t;

ir_abcd_t *ir_abcd_build(ir_func_t *f);
ir_abcd_t *ir_abcd_build_with(ir_func_t *f, const ir_abcd_hooks_t *hooks);
ir_abcd_t *ir_func_abcd(ir_func_t *f);
void       ir_abcd_free(ir_abcd_t *abcd);

bool ir_abcd_check_redundant(ir_abcd_t *abcd, const ir_instr_t *access);
bool ir_abcd_at_most(ir_abcd_t *abcd, const ir_value_t *value,
                     const ir_value_t *bound, ir_int_t offset,
                     const ir_instr_t *at);
bool ir_abcd_at_least(ir_abcd_t *abcd, const ir_value_t *value,
                      const ir_value_t *bound, ir_int_t offset,
                      const ir_instr_t *at);
bool ir_abcd_overflow_safe(ir_abcd_t *abcd, const ir_instr_t *arithmetic);
int  ir_abcd_compare(ir_abcd_t *abcd, const ir_instr_t *comparison);
uint32_t ir_abcd_fold(ir_func_t *f);

bool          ir_abcd_range_of(ir_abcd_t *abcd, const ir_value_t *value,
                               const ir_instr_t *at, ir_abcd_range_t *out);
bool          ir_abcd_range_of_conditions(ir_abcd_t *abcd,
                                          const ir_value_t *value,
                                          const ir_instr_t *at,
                                          ir_abcd_range_t *out);
bool          ir_abcd_type_bounds(const ir_func_t *f, const ir_value_t *value,
                                  ir_interval_t *out);
ir_overflow_t ir_abcd_overflow(ir_abcd_t *abcd, const ir_instr_t *in,
                               ir_interval_t *left, ir_interval_t *right);
bool          ir_abcd_index_proven(ir_abcd_t *abcd, const ir_instr_t *index);
bool          ir_abcd_unsigned_sub_safe(ir_abcd_t *abcd,
                                        const ir_instr_t *sub);
bool          ir_abcd_trap_proven(ir_abcd_t *abcd, const ir_instr_t *in);
bool          ir_abcd_site_proven(ir_abcd_t *abcd, const ir_instr_t *in,
                                  ir_site_kind_t kind);
uint32_t ir_abcd_hoist(ir_func_t *f);

#endif
