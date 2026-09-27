#ifndef IR_LOWER_H
#define IR_LOWER_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "parse/annotation.h"
#include "parse/tree_visit.h"

typedef struct ir_lowerer_t ir_lowerer_t;

typedef enum : uint8_t {
    IR_LOWER_VALUE,
    IR_LOWER_NO_VALUE,
    IR_LOWER_TERMINATED,
    IR_LOWER_FAILED,
} ir_lower_status_t;

typedef struct {
    ir_lower_status_t status;
    ir_value_t *value;
} ir_lower_result_t;

typedef struct {
    ir_module_t *module;
    ir_func_t *function;
    ir_block_t *block;
    ir_diags_t *diagnostics;
    void *user;
    uint64_t next_value_id;
    bool reachable;
} ir_lower_context_t;

typedef ir_lower_result_t (*ir_lower_action_fn)(
    ir_lower_context_t *context, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count, th_type *result_type,
    ir_span_t span, void *state);

ir_lowerer_t *ir_lowerer_new(ir_ctx_t *ctx);
bool ir_lowerer_register(ir_lowerer_t *lowerer, const char *action,
                         ir_lower_action_fn callback, void *state);
ir_lower_result_t ir_lower_emit(
    ir_lowerer_t *lowerer, ir_lower_context_t *context,
    const parse_annotation_t *annotation, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count, th_type *result_type,
    ir_span_t span);

#endif
