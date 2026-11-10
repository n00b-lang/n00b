#ifndef N00BB_LOWER_WALK_H
#define N00BB_LOWER_WALK_H

#include "ir/builder.h"
#include "parse/annotation.h"
#include "parse/tree_visit.h"

typedef struct ir_lowerer_t ir_lowerer_t;


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
bool ir_lower_suppress_emit(ir_lower_context_t *context, parse_tree_t *node);
bool ir_lower_emit_suppressed(ir_lower_context_t *context,
                              parse_tree_t *node);
bool ir_lower_handle_assignment(ir_lower_context_t *context,
                                parse_tree_t *assignment);
bool ir_lower_finish_assignment(ir_lower_context_t *context,
                                parse_tree_t *assignment,
                                parse_tree_t *completed_node);
bool ir_lower_evaluate_assignment_target(ir_lower_context_t *context,
                                         parse_tree_t *assignment);
bool ir_lower_skip_value(ir_lower_context_t *context, parse_tree_t *owner,
                         parse_tree_t *node);
bool ir_lower_bind_value(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_value_t *value);
bool ir_lower_bind_local(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, ir_local_t *local);
bool ir_lower_rebind_local(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol, ir_local_t *local);
bool ir_lower_bind_local_storage(ir_lower_context_t *context,
                                 parse_sym_entry_t *symbol,
                                 ir_local_t *local, ir_value_t *address);
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
ir_lower_result_t ir_lower_assign_symbol(ir_lower_context_t *context,
                                         parse_sym_entry_t *symbol,
                                         ir_value_t *value, bool initializes,
                                         ir_span_t span);
bool ir_lowerer_register(ir_lowerer_t *lowerer, core_string_t action,
                         ir_lower_action_fn callback, void *state);
bool ir_lowerer_register_visit(ir_lowerer_t *lowerer,
                               parse_annot_kind_t kind,
                               ir_lower_visit_fn callback, void *state);
bool ir_lowerer_register_nt_visit(ir_lowerer_t *lowerer,
                                 core_string_t nonterminal,
                                 ir_lower_visit_fn callback, void *state);
bool ir_lower_prune_node(ir_lower_context_t *context, parse_tree_t *node);
ir_lower_result_t ir_lower_tree(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                parse_grammar_t *grammar, parse_tree_t *root);
void ir_lower_context_release(ir_lower_context_t *context);

#endif
