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
    void *tree_values;
    void *tree_state;
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
ir_value_t *ir_lower_node_value(ir_lower_context_t *context,
                                parse_tree_t *node);
bool ir_lower_set_node_value(ir_lower_context_t *context,
                             parse_tree_t *node, ir_value_t *value);
bool ir_lower_mark_lvalue(ir_lower_context_t *context, parse_tree_t *owner,
                          parse_tree_t *node);
/* Suppress only this node's annotation-driven emit action. Children are
 * still lowered and remain available to visit callbacks. */
bool ir_lower_suppress_emit(ir_lower_context_t *context, parse_tree_t *node);
/* Claim a custom assignment on node entry; its lowering callback must emit
 * the assignment operation before the node exits. */
bool ir_lower_handle_assignment(ir_lower_context_t *context,
                                parse_tree_t *assignment);
bool ir_lower_finish_assignment(ir_lower_context_t *context,
                                parse_tree_t *assignment,
                                parse_tree_t *completed_node);
/* Keep an assignment target in value context while leaving the assignment's
 * annotation-driven emit operation active. Useful for read-modify-write. */
bool ir_lower_evaluate_assignment_target(ir_lower_context_t *context,
                                         parse_tree_t *assignment);
bool ir_lower_skip_value(ir_lower_context_t *context, parse_tree_t *owner,
                         parse_tree_t *node);
ir_value_t *ir_lower_child_value(ir_lower_context_t *context,
                                 parse_visit_t *visit,
                                 parse_child_ref_t child);
/* Append one non-terminator or terminator to the current block, attaching its
 * source span and updating reachability for terminators. */
ir_instr_t *ir_lower_emit_instruction(ir_lower_context_t *context,
                                      ir_op_t op, ir_span_t span);
ir_instr_t *ir_lower_emit_terminator(ir_lower_context_t *context,
                                     ir_op_t op, ir_span_t span);
bool ir_lower_bind_value(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_value_t *value);
bool ir_lower_bind_local(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_local_t *local);
/* Replace the active binding for a symbol after a staged lowering step. */
bool ir_lower_rebind_local(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol, ir_local_t *local);
/* Bind a source local to an explicit address-backed storage value. */
bool ir_lower_bind_local_storage(ir_lower_context_t *context,
                                 parse_sym_entry_t *symbol,
                                 ir_local_t *local, ir_value_t *address);
/* Scoped bindings shadow earlier entries until restored to a checkpoint. */
void *ir_lower_bindings_checkpoint(ir_lower_context_t *context);
bool ir_lower_bindings_restore(ir_lower_context_t *context, void *checkpoint);
bool ir_lower_bind_scoped_value(ir_lower_context_t *context,
                               parse_sym_entry_t *symbol,
                               ir_value_t *value, ir_local_t *local);
ir_local_t *ir_lower_bound_local(ir_lower_context_t *context,
                                parse_sym_entry_t *symbol);
ir_value_t *ir_lower_bound_address(ir_lower_context_t *context,
                                   parse_sym_entry_t *symbol);
ir_value_t *ir_lower_local_address(ir_lower_context_t *context,
                                   ir_local_t *local);
ir_value_t *ir_lower_bound_value(ir_lower_context_t *context,
                                 parse_sym_entry_t *symbol);
bool ir_lowerer_register(ir_lowerer_t *lowerer, const char *action,
                         ir_lower_action_fn callback, void *state);
bool ir_lowerer_register_visit(ir_lowerer_t *lowerer,
                               parse_annot_kind_t kind,
                               ir_lower_visit_fn callback, void *state);
/* Nonterminal callbacks run before annotations on entry and after child
 * lowering on exit. A callback may prune its current subtree on entry. */
bool ir_lowerer_register_nt_visit(ir_lowerer_t *lowerer,
                                 const char *nonterminal,
                                 ir_lower_visit_fn callback, void *state);
bool ir_lower_prune_node(ir_lower_context_t *context, parse_tree_t *node);
ir_lower_result_t ir_lower_emit(
    ir_lowerer_t *lowerer, ir_lower_context_t *context,
    const parse_annotation_t *annotation, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count, th_type *result_type,
    ir_span_t span);
ir_lower_result_t ir_lower_tree(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                parse_grammar_t *grammar, parse_tree_t *root);

#endif
