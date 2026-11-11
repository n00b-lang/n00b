#ifndef IR_BUILDER_H
#define IR_BUILDER_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "lib/string.h"

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

typedef struct ir_lower_context_t ir_lower_context_t;

struct ir_lower_context_t {
    ir_module_t *module;
    ir_func_t *function;
    ir_block_t *block;
    ir_diags_t *diagnostics;
    int32_t file_id;
    void *user;
    uint64_t next_value_id;
    bool reachable;
};

ir_lower_result_t ir_lower_fail(ir_lower_context_t *context,
                                ir_diagcode_t code, ir_span_t span,
                                core_string_t message);
ir_instr_t *ir_lower_emit_instruction(ir_lower_context_t *context,
                                      ir_op_t op, ir_span_t span);
ir_instr_t *ir_lower_emit_terminator(ir_lower_context_t *context,
                                     ir_op_t op, ir_span_t span);
bool ir_lower_set_temp_dest(ir_lower_context_t *context,
                            ir_instr_t *instruction, core_string_t prefix,
                            th_type *type);
ir_value_t *ir_lower_dispatch(ir_lower_context_t *context,
                              core_string_t method,
                              ir_value_t *const *operands, size_t count,
                              th_type *result_type, ir_span_t span);

#endif
