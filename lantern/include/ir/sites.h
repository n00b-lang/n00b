#ifndef IR_SITES_H
#define IR_SITES_H

#include "ir/ir.h"

typedef enum : uint8_t {
    IR_SITE_BRANCH,
    IR_SITE_LOOP_BOUND,
    IR_SITE_ADDRESS,
    IR_SITE_DIVISOR,
    IR_SITE_SHIFT_AMOUNT,
    IR_SITE_BOXED,
    IR_SITE_CALL_ARGUMENT,
    IR_SITE_CALL_RESULT,
    IR_SITE_COPY_OUT,
    IR_SITE_RETURNED,
    IR_SITE_TRAP,
    IR_SITE_FAILURE,
} ir_site_kind_t;

typedef struct {
    ir_instr_t    *instr;
    uint32_t       operand;
    ir_site_kind_t kind;
} ir_site_t;

typedef struct ir_sites_t ir_sites_t;

ir_sites_t      *ir_sites_build(ir_func_t *f);
ir_sites_t      *ir_func_sites(ir_func_t *f);
void             ir_sites_free(ir_sites_t *sites);
uint32_t         ir_sites_count(const ir_sites_t *sites);
const ir_site_t *ir_site_at(const ir_sites_t *sites, uint32_t index);

enum {
    IR_PATH_UNPASSED = 1u << 0,
    IR_PATH_PASSED   = 1u << 1,
};

typedef enum : uint8_t {
    IR_PATH_ANY,
    IR_PATH_SUCCESS,
    IR_PATH_FAILURE,
} ir_path_outcome_t;

typedef struct {
    uint8_t returned;
    uint8_t failed;
} ir_path_exits_t;

typedef uint8_t (*ir_path_step_fn)(void *context, const ir_instr_t *in,
                                   ir_path_outcome_t outcome);

ir_instr_t *ir_call_outcome_branch(const ir_instr_t *terminator,
                                   bool *true_is_success);
uint8_t     ir_path_apply(uint8_t state, uint8_t effect);
bool        ir_paths_flow(ir_func_t *f, uint8_t entry, ir_path_step_fn step,
                          void *context, ir_path_exits_t *exits);

#endif
