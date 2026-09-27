#ifndef IR_LOWER_H
#define IR_LOWER_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "parse/annotation.h"
#include "parse/tree_visit.h"

typedef struct ir_lowerer_t ir_lowerer_t;
typedef struct parse_sym_entry_t parse_sym_entry_t;

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
    int32_t file_id;
    void *symbol_bindings;
    void *user;
    uint64_t next_value_id;
    bool reachable;
} ir_lower_context_t;

typedef ir_lower_result_t (*ir_lower_action_fn)(
    ir_lower_context_t *context, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count, th_type *result_type,
    ir_span_t span, void *state);
typedef bool (*ir_lower_visit_fn)(ir_lower_context_t *context,
                                  parse_visit_t *visit, bool entering,
                                  void *state);

ir_lowerer_t *ir_lowerer_new(ir_ctx_t *ctx);
bool ir_lower_bind_value(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_value_t *value);
bool ir_lower_bind_local(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_local_t *local);
bool ir_lowerer_register(ir_lowerer_t *lowerer, const char *action,
                         ir_lower_action_fn callback, void *state);
bool ir_lowerer_register_visit(ir_lowerer_t *lowerer,
                               parse_annot_kind_t kind,
                               ir_lower_visit_fn callback, void *state);
ir_lower_result_t ir_lower_emit(
    ir_lowerer_t *lowerer, ir_lower_context_t *context,
    const parse_annotation_t *annotation, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count, th_type *result_type,
    ir_span_t span);
ir_lower_result_t ir_lower_tree(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                parse_grammar_t *grammar, parse_tree_t *root);

#endif
