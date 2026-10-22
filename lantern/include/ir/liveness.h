#ifndef IR_LIVENESS_H
#define IR_LIVENESS_H

#include "ir/ir.h"

typedef struct ir_live_t ir_live_t;

typedef enum : uint8_t {
    IR_LIVE_FLOW_NONE,
    IR_LIVE_FLOW_CARRY,
    IR_LIVE_FLOW_HOLD,
} ir_live_flow_t;

typedef ir_live_flow_t (*ir_live_flow_fn)(void *context,
                                          const ir_instr_t *user,
                                          uint32_t operand);
typedef bool (*ir_live_point_fn)(void *context, const ir_instr_t *point);

ir_live_t    *ir_live_build(ir_func_t *f);
void          ir_live_free(ir_live_t *live);
ir_live_t    *ir_func_liveness(ir_func_t *f);
bool          ir_live_valid(const ir_live_t *live);
uint32_t      ir_live_pred_count(const ir_live_t *live,
                                 const ir_block_t *block);
ir_block_t   *ir_live_pred_at(const ir_live_t *live, const ir_block_t *block,
                              uint32_t i);

ir_liveness_t ir_live_after(ir_live_t *live, const ir_value_t *value,
                            const ir_instr_t *point);
void          ir_live_after_each(ir_live_t *live, const ir_value_t *value,
                                 const ir_block_t *block,
                                 ir_liveness_t *after);
ir_liveness_t ir_live_on_edge(ir_live_t *live, const ir_value_t *value,
                              const ir_block_t *from, const ir_block_t *to);
bool          ir_live_set_after_each(ir_live_t *live,
                                     ir_value_t *const *values, size_t count,
                                     const ir_block_t *block, bool *after);
bool          ir_live_set_on_edge(ir_live_t *live, ir_value_t *const *values,
                                  size_t count, const ir_block_t *from,
                                  const ir_block_t *to);
bool          ir_live_block_on_cycle(ir_live_t *live, const ir_block_t *block);
bool          ir_live_across_points(ir_live_t *live,
                                    ir_value_t *const *values, size_t count,
                                    ir_live_point_fn point, void *context,
                                    bool *across);
ir_liveness_t ir_live_carried_after(ir_live_t *live, const ir_value_t *root,
                                    ir_live_flow_fn flow, void *context,
                                    const ir_instr_t *point);

#endif
