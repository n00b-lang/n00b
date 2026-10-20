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

typedef bool (*ir_instr_predicate_fn)(void *context, const ir_instr_t *in);

enum {
    IR_PATH_EXIT_RETURN = 1u << 0,
    IR_PATH_EXIT_FAIL   = 1u << 1,
};

bool ir_paths_can_return(ir_func_t *f, ir_instr_predicate_fn diverges,
                         void *context);
bool ir_paths_all_pass(ir_func_t *f, ir_instr_predicate_fn passes,
                       ir_instr_predicate_fn diverges, void *context);
bool ir_paths_all_pass_to(ir_func_t *f, uint32_t exits,
                          ir_instr_predicate_fn passes,
                          ir_instr_predicate_fn diverges, void *context);

#endif
