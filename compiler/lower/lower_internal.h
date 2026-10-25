#ifndef N00BB_LOWER_INTERNAL_H
#define N00BB_LOWER_INTERNAL_H

#include <stdarg.h>

#include <stdio.h>

#include <fcntl.h>

#include <limits.h>

#include <pthread.h>

#include <stdlib.h>

#include <string.h>

#include <time.h>

#include <sys/stat.h>

#include <unistd.h>

double n00bb_profile_seconds(void);

#include "call_plan.h"

#include "driver.h"

#include "lib/os.h"

#include "compiler_cli.h"

#include "compiler_tu.h"

#include "compiler_link.h"

#include "compiler_axioms.h"

#include "compiler_buffer_slices.h"

#include "compiler_ir_setup.h"

#include "compiler_lower.h"

#include "compiler_lower_emit.h"

#include "compiler_rewrite.h"

#include "compiler_types.h"

#include "config.h"

#include "diagnostics.h"

#include "runtime_panic.h"

#include "interface_summaries.h"

#include "storage_placement.h"

#include "compiler_cache.h"

#include "compiler_layout.h"

#include "compiler_tu_interface.h"

#include "abi_layout.h"

#include "abi_emit.h"

#include "abi_link.h"

#include "abi_semantics.h"


#include "abi_record_layout.h"

#include "record_layout_request.h"

#include "enum_value.h"

#include "flagset_value.h"

#include "frame_roots.h"

#include "global_metadata.h"

#include "qualified_names.h"

#include "llvm_backend.h"

#include "native_frame_maps.h"

#include "n00b-abi-v1.h"

#include "compiler_native_roots.h"

#include "storage_selection.h"

#include "specialize.h"

#include "lib/alloc.h"

#include "lib/arena.h"

#include "lib/buffer.h"

#include "lib/dict.h"

#include "lib/list.h"

#include "lib/link_unit.h"

#include "lib/indexed_table.h"

#include "lib/target.h"

#include "lib/object_section.h"

#include "lib/work_queue.h"

#include "diag/diagnostics.h"

#include "parse/commander.h"

#include "parse/bnf.h"

#include "parse/grammar.h"

#include "parse/pwz.h"

#include "parse/tree_dump.h"

#include "parse/tree_print.h"

#include "parse/tree.h"

#include "parse/tree_util.h"

#include "parse/annot_walk.h"

#include "ir/ir.h"

#include "ir/analyses.h"

#include "ir/doms.h"

#include "ir/summary.h"

#include "ir/summary_facts.h"

#include "ir/races.h"

#include "ir/liveness.h"

#include "ir/analyze.h"

#include "ir/operands.h"

#include "cached_body.h"

#include "ir/layout.h"

#include "ir/typelang.h"

#include "th/environment.h"

#include "scanner/token_stream.h"

#include "scanner/tokenizer.h"

#include "scanner/lex.h"

#include "unicode/encoding.h"

#include "preprocessor.h"

#include "signature_typevars.h"

#include "field_extent.h"

bool
n00bb_prepare_ir_bindings(core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          uint32_t stage_kind);

enum { N00BB_JOB_LOCATE_TU = 1, N00BB_JOB_PREPROCESS = 2 };

typedef ir_value_t *n00bb_ir_value_ptr_t;

core_list_decl(n00bb_ir_value_ptr_t);

typedef ir_block_t *n00bb_ir_block_ptr_t;

core_list_decl(n00bb_ir_block_ptr_t);

typedef struct {
    parse_tree_t *node;
    size_t parameter_index;
} n00bb_call_actual_t;

core_list_decl(n00bb_call_actual_t);

bool
n00bb_output_target_profile(core_target_desc_t *target);

bool
n00bb_cross_target(core_target_desc_t *target);

core_list_decl(size_t);

core_list_decl(th_kwparam);

core_list_decl(th_member);

typedef bool (*n00bb_expression_result_hook_t)(ir_lower_context_t *context,
    ir_value_t **value, void *data);

typedef struct {
    parse_tree_t *expression;
    ir_global_t *result_global;
} n00bb_array_extent_lowering_t;

typedef struct {
    core_list_t(n00bb_ir_func_ptr_t) initializer_roots;
    core_list_t(n00bb_ir_func_ptr_t) entry_functions;
    core_dict_t targets;
    core_dict_t reachable;
    ir_summaries_t *summaries;
} n00bb_runtime_init_analysis_t;

typedef struct {
    core_translation_unit_t *source;
    core_translation_unit_t *target;
    parse_sym_entry_t *symbol;
    ir_global_t *global;
    ir_func_t *initializer;
    ir_instr_t *read;
} n00bb_initializer_read_t;

core_list_decl(n00bb_initializer_read_t);

[[gnu::format(printf, 3, 4)]] core_string_t
n00bb_format_name(char *buffer, size_t size, const char *format, ...);

typedef struct {
    parse_tree_t *condition;
    parse_tree_t *body;
    ir_block_t *test_block;
    ir_block_t *body_block;
} n00bb_branch_arm_t;

core_list_decl(n00bb_branch_arm_t);

typedef struct {
    parse_tree_t *node;
    core_list_t(n00bb_branch_arm_t) arms;
    parse_tree_t *else_body;
    ir_block_t *join;
    ir_block_t *else_block;
    ir_local_t *result_local;
    ir_block_t *jump_true;
    ir_block_t *jump_false;
    ir_block_t *short_block;
    bool expression;
    bool short_circuit;
    bool jumping;
    bool join_reachable;
    bool active;
} n00bb_branch_frame_t;

core_list_decl(n00bb_branch_frame_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *condition;
    parse_tree_t *body;
    parse_tree_t *range_start;
    parse_tree_t *range_end;
    parse_tree_t *range_step;
    ir_value_t *range_step_value;
    ir_block_t *header;
    ir_block_t *body_block;
    ir_block_t *latch;
    ir_block_t *increment;
    ir_block_t *exit;
    ir_block_t *continue_block;
    ir_local_t *iterator;
    ir_local_t *range_binding;
    parse_sym_entry_t *iterator_symbol;
    parse_tree_t *iteration_source;
    n00bb_iteration_plan_t *iteration_plan;
    ir_instr_t *iteration_cursor_phi;
    ir_value_t *iteration_cursor_value;
    core_list_t(parse_sym_entry_ptr_t) iteration_bindings;
    void *binding_checkpoint;
    bool cleanup_active;
    bool binding_scope_active;
    bool range_inclusive;
    bool is_range;
    bool is_container;
    uint64_t order;
    bool active;
} n00bb_loop_frame_t;

core_list_decl(n00bb_loop_frame_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *condition;
    ir_block_t *join;
    ir_block_t *failure;
} n00bb_require_frame_t;

core_list_decl(n00bb_require_frame_t);

core_list_decl(ir_lit_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *upper;
    core_string_t name;
    bool is_range;
    bool inclusive;
} n00bb_switch_pattern_t;

core_list_decl(n00bb_switch_pattern_t);

typedef struct {
    parse_tree_t *list;
    parse_tree_t *body;
    ir_block_t *block;
    core_list_t(n00bb_switch_pattern_t) patterns;
} n00bb_switch_case_t;

core_list_decl(n00bb_switch_case_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *scrutinee;
    core_list_t(n00bb_switch_case_t) cases;
    parse_tree_t *else_body;
    ir_block_t *default_block;
    ir_block_t *join;
    ir_block_t *test_block;
    ir_value_t *subject;
    size_t tested;
    size_t total;
    uint64_t order;
    ir_local_t *result_local;
    bool expression;
    bool subject_variable;
    bool emitted;
    bool join_reachable;
    bool active;
} n00bb_switch_frame_t;

core_list_decl(n00bb_switch_frame_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    th_type *type;
    th_type *test;
    parse_sym_entry_t *subject_shadow;
    ir_block_t *block;
} n00bb_typecase_arm_t;

core_list_decl(n00bb_typecase_arm_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *subject_node;
    parse_sym_entry_t *subject_symbol;
    ir_value_t *subject_value;
    core_list_t(n00bb_typecase_arm_t) arms;
    parse_tree_t *else_body;
    ir_block_t *else_block;
    ir_block_t *unmatched_block;
    ir_block_t *join;
    ir_local_t *result_local;
    void *binding_checkpoint;
    uint64_t order;
    bool expression;
    bool emitted;
    bool join_reachable;
    bool active;
} n00bb_typecase_frame_t;

core_list_decl(n00bb_typecase_frame_t);

typedef struct {
    parse_tree_t *node;
    ir_local_t *result;
    ir_block_t *continuation;
    uint64_t order;
    bool active;
    bool value_written;
} n00bb_value_block_frame_t;

core_list_decl(n00bb_value_block_frame_t);

typedef struct {
    core_string_t name;
    ir_local_t *flag;
} n00bb_init_field_t;

core_list_decl(n00bb_init_field_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    parse_tree_t *error_clause;
    parse_sym_entry_t *error_symbol;
    ir_block_t *handler;
    ir_block_t *join;
    ir_local_t *error_value;
    ir_local_t *result_value;
    bool expression;
    bool error_reachable;
    bool join_reachable;
    bool active;
} n00bb_try_frame_t;

core_list_decl(n00bb_try_frame_t);

typedef struct {
    parse_sym_entry_t *context_symbol;
    core_translation_unit_t *context_unit;
    parse_tree_t *call_node;
    core_list_t(n00bb_ir_value_ptr_t) captures;
    ir_value_t *provided_value;
    uint64_t order;
    bool active;
} n00bb_with_item_t;

core_list_decl(n00bb_with_item_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    core_list_t(n00bb_with_item_t) items;
    bool active;
} n00bb_with_frame_t;

core_list_decl(n00bb_with_frame_t);

typedef struct {
    parse_tree_t *scope;
    parse_sym_entry_t *symbol;
    ir_local_t *owned;
    th_type *type;
    ir_local_t *storage;
    ir_value_t *value;
    bool fields;
    bool moved;
    uint64_t order;
} n00bb_final_frame_t;

core_list_decl(n00bb_final_frame_t);

typedef struct {
    parse_sym_entry_t *symbol;
    ir_local_t *local;
    ir_value_t *global;
    ir_value_t *address;
    parse_grammar_t *grammar;
    parse_tree_t *site_node;
    parse_tree_t *index_target;
    parse_tree_t *source_node;
    ir_shape_t *required_shape;
    size_t output_index;
} n00bb_copyout_t;

core_list_decl(n00bb_copyout_t);

typedef struct {
    parse_sym_entry_t *symbol;
    parse_tree_t *expression;
    ir_local_t *local;
    ir_value_t *argument;
    bool lowering;
    bool emitted;
} n00bb_default_param_t;

core_list_decl(n00bb_default_param_t);

typedef struct {
    ir_value_t *outcome;
    core_list_t(n00bb_copyout_t) items;
} n00bb_pending_copyouts_t;

core_list_decl(n00bb_pending_copyouts_t);

void
n00bb_order_copyouts(core_list_t(n00bb_copyout_t) *copyouts);

typedef struct {
    parse_tree_t *node;
    core_list_t(parse_tree_ptr_t) expressions;
    core_list_t(n00bb_ir_value_ptr_t) values;
} n00bb_tuple_parts_t;

th_type *
n00bb_node_result_type(ir_lower_context_t *context, parse_tree_t *node);
th_type *
n00bb_node_type(ir_lower_context_t *context, parse_tree_t *node);

ir_value_t *
n00bb_open_record_literal(ir_lower_context_t *context,
                          n00bb_tuple_parts_t *parts, ir_span_t span);

core_list_decl(n00bb_tuple_parts_t);

typedef struct {
    parse_tree_t *node;
    ir_module_t *module;
    ir_func_t *function;
    ir_block_t *block;
    ir_func_t *producer;
    int32_t file_id;
    size_t with_count;
    size_t try_count;
    size_t final_base;
    bool reachable;
} n00bb_literal_frame_t;

struct n00bb_lower_context {
    parse_grammar_t *grammar;
    ir_lowerer_t *lowerer;
    void *walk;
    bool bind_external;
    bool track_writes;
    ir_ctx_t *ir_context;
    core_list_t(core_translation_unit_ptr_t) *units;
    core_translation_unit_t *unit;
    parse_tree_t *function_body;
    parse_sym_entry_t *function_symbol;
    ir_shape_t *bound_return_shape;
    core_list_t(n00bb_branch_frame_t) branches;
    core_list_t(n00bb_loop_frame_t) loops;
    core_list_t(n00bb_require_frame_t) requires;
    core_list_t(n00bb_switch_frame_t) switches;
    core_list_t(n00bb_typecase_frame_t) typecases;
    core_list_t(n00bb_value_block_frame_t) value_blocks;
    core_list_t(n00bb_init_field_t) init_fields;
    bool reference_write;
    bool write_back_open_index;
    core_dict_t node_types;
    bool node_types_ready;
    core_list_t(n00bb_try_frame_t) tries;
    core_list_t(n00bb_with_frame_t) withs;
    core_list_t(n00bb_final_frame_t) finals;
    size_t final_base;
    core_list_t(n00bb_copyout_t) parameters_to_copy_out;
    core_list_t(n00bb_pending_copyouts_t) pending_call_copyouts;
    core_list_t(n00bb_default_param_t) default_params;
    n00bb_default_param_t *active_default_param;
    core_list_t(n00bb_tuple_parts_t) tuple_parts;
    n00bb_literal_frame_t active_literal;
    bool has_active_literal;
    ir_block_t *condition_scratch;
    uint64_t next_block_id;
    uint64_t next_control_order;
    bool solved;
};

void
n00bb_rewrite_later(ir_lower_context_t *context, ir_instr_t *in);

bool
n00bb_open_type(ir_lower_context_t *context, th_type *type);

ir_value_t *
n00bb_dispatch(ir_lower_context_t *context, core_string_t method,
               ir_value_t *const *operands, size_t count,
               th_type *result_type, ir_span_t span);

bool
n00bb_add_iteration_backedge(n00bb_loop_frame_t *frame,
                              ir_block_t *predecessor);

ir_module_t *
unit_module(n00bb_tu_state_t *state);

parse_tree_t *
value_body_expression(parse_grammar_t *grammar, parse_tree_t *body);

core_string_t
n00bb_control_label(parse_grammar_t *grammar, parse_tree_t *node,
                    core_string_t labeled_form);

void
append_module_symbol_name(core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol, core_buffer_t *buffer);

ir_lower_result_t
lower_error_text(ir_lower_context_t *context, ir_diagcode_t code,
                 ir_span_t span, core_string_t message);

#define lower_error(context, code, span, literal) \
    lower_error_text((context), (code), (span), CORE_STRING_STATIC(literal))

core_diagnostics_t *
lower_diagnostics(ir_lower_context_t *context);

size_t
lower_failures(ir_lower_context_t *context);

const core_translation_unit_t *
lower_unit(ir_lower_context_t *context);

ir_lower_result_t
lower_report(ir_lower_context_t *context, n00bb_diag_t id, ir_span_t span,
             n00bb_text_t text, n00bb_args_t args);

void
lower_secondary(ir_lower_context_t *context, ir_span_t span,
                n00bb_text_t text, n00bb_args_t args);

void
lower_declared_secondary(ir_lower_context_t *context,
                         parse_sym_entry_t *symbol, n00bb_text_t text,
                         n00bb_args_t args);

void
lower_help(ir_lower_context_t *context, n00bb_text_t text, n00bb_args_t args);

ir_lower_result_t
lower_internal_string(ir_lower_context_t *context, ir_span_t span,
                      core_string_t what);

#define lower_internal(context, span, literal) \
    lower_internal_string((context), (span), CORE_STRING_STATIC(literal))

ir_lower_result_t
lower_placeholder(ir_lower_context_t *context, th_type *type, ir_span_t span);

ir_lower_result_t
lower_failed_after(ir_lower_context_t *context, ir_span_t span,
                   core_string_t what);

parse_token_info_t *
last_tree_terminal(parse_tree_t *node);

ir_span_t
lower_span_for_tree(ir_lower_context_t *context, parse_tree_t *node);

ir_span_t
tree_span(int32_t file_id, parse_tree_t *node);

ir_span_t
lower_span_for_first_token(ir_lower_context_t *context, parse_tree_t *node);

ir_span_t
lower_symbol_name_span(ir_lower_context_t *context, parse_sym_entry_t *symbol);

core_string_t
source_text_of(const core_translation_unit_t *unit, parse_tree_t *node);

core_string_t
lower_type_text(ir_lower_context_t *context, th_type *type);

parse_sym_entry_t *
lower_user_function(ir_lower_context_t *context);

bool
lower_mark_failed(ir_lower_context_t *context);

bool
report_uncaught_failure(ir_lower_context_t *context, ir_span_t span,
                        th_type *error_type, n00bb_text_t source);

core_string_t
mode_keyword(th_mode mode);

void
report_unwritable_output(ir_lower_context_t *context,
                         parse_grammar_t *grammar, parse_tree_t *argument,
                         th_mode mode, parse_sym_entry_t *callee,
                         core_string_t parameter);

extern const core_string_t effects_subject_key;

extern const core_string_t effects_name_key;

bool
effects_query_builtin(const parse_sym_entry_t *symbol);

bool
annotate_effects_query(ir_ctx_t *ctx, ir_instr_t *call, ir_value_t *subject,
                       core_string_t name);

ir_lower_result_t
lower_builtin_failure(ir_lower_context_t *context, bool valid, ir_span_t span,
                      parse_sym_entry_t *builtin, n00bb_text_t expectation);

ir_value_t *
option_present(ir_lower_context_t *context, ir_value_t *option,
               th_type *bool_type, ir_span_t span);

ir_value_t *
condition_value(ir_lower_context_t *context, parse_tree_t *node);

ir_lower_result_t
lower_condition_error(ir_lower_context_t *context, parse_tree_t *condition,
                      ir_value_t *value, n00bb_text_t construct);

n00bb_text_t
logical_construct(parse_grammar_t *grammar, parse_tree_t *node);

bool
n00bb_mark_condition(ir_lower_context_t *context, n00bb_text_t construct,
                     parse_tree_t *condition);

ir_block_t *
n00bb_branch_block_named(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower, core_string_t role);

#define n00bb_branch_block(context, lower, literal) \
    n00bb_branch_block_named((context), (lower), CORE_STRING_STATIC(literal))

bool
n00bb_atomic_native_support(ir_module_t *module, th_type *atomic_type,
                            const core_target_desc_t *target);

bool
n00bb_lower_atomic_reference(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering, void *state);

ir_value_t *
n00bb_emit_local_read(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span);

bool
n00bb_emit_sequence_shape_check_impl(ir_lower_context_t *context,
                                     ir_value_t *value, ir_shape_t *expected,
                                     ir_span_t span, bool force);

bool
n00bb_emit_sequence_shape_check(ir_lower_context_t *context,
                                ir_value_t *value, ir_shape_t *expected,
                                ir_span_t span);

bool
n00bb_attach_expression_shape(ir_lower_context_t *context,
                              parse_grammar_t *grammar,
                              parse_tree_t *expression, ir_value_t *value);

bool
n00bb_emit_local_write(ir_lower_context_t *context, ir_local_t *local,
                       ir_value_t *value, ir_span_t span);

ir_value_t *
n00bb_emit_indirect_read(ir_lower_context_t *context, ir_value_t *address,
                        th_type *type, ir_span_t span);

bool
n00bb_emit_indirect_write(ir_lower_context_t *context, ir_value_t *address,
                          ir_value_t *value, ir_span_t span);

ir_instr_t *
n00bb_emit_success_return(ir_lower_context_t *context, ir_value_t *result,
                          const core_list_t(n00bb_copyout_t) *copyouts,
                          ir_span_t span);

th_type *
n00bb_indirect_type(th_ctx *types, th_type *referent);

bool
n00bb_set_reference_access(ir_ctx_t *context, ir_value_t *value,
                           th_type *type);

ir_mutability_t
local_binding_mutability(const parse_sym_entry_t *symbol);

n00bb_final_frame_t *
n00bb_final_frame(n00bb_lower_context_t *lower,
                  const parse_sym_entry_t *symbol);

bool
n00bb_set_flag(ir_lower_context_t *context, ir_local_t *flag, bool value,
               ir_span_t span);

bool
n00bb_set_owned(ir_lower_context_t *context, n00bb_final_frame_t *frame,
                bool owned, ir_span_t span);

enum { N00BB_FINALIZE_DEPTH = 16 };

ir_value_t *
n00bb_emit_temp(ir_lower_context_t *context, ir_op_t op, th_type *type,
                ir_value_t *operand, ir_span_t span);

bool
n00bb_emit_finalize_fields(ir_lower_context_t *context, ir_value_t *address,
                           ir_span_t span, size_t depth);

bool
n00bb_emit_finalize_value(ir_lower_context_t *context, ir_value_t *address,
                          th_type *type, ir_span_t span, size_t depth);

bool
n00bb_emit_scope_finalize(ir_lower_context_t *context,
                          n00bb_final_frame_t *frame, parse_tree_t *source);

bool
n00bb_begin_final_frame(ir_lower_context_t *context,
                        parse_sym_entry_t *symbol, ir_span_t span);

bool
n00bb_finish_final_scope(ir_lower_context_t *context,
                         parse_tree_t *scope, parse_tree_t *source);

bool
n00bb_lower_statement_scope(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state);

bool
n00bb_move_source(ir_lower_context_t *context, parse_grammar_t *grammar,
                  parse_tree_t *source);

bool
n00bb_write_binding(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    bool written);

bool
n00bb_bind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                 ir_local_t *local, ir_span_t span);

ir_value_t *
n00bb_emit_binary_ir_with(ir_lower_context_t *context, ir_op_t op,
                          bool wrapping, th_type *result_type,
                          ir_value_t *left, ir_value_t *right, ir_span_t span);

ir_value_t *
n00bb_emit_binary_ir(ir_lower_context_t *context, ir_op_t op,
                     th_type *result_type, ir_value_t *left,
                     ir_value_t *right, ir_span_t span);

bool
n00bb_machine_promotes(ir_machine_t source, ir_machine_t destination);

ir_value_t *
n00bb_widen_integer_operand(ir_lower_context_t *context, ir_value_t *value,
                            th_type *result_type, ir_span_t span);

th_type *
n00bb_ir_builtin_type(ir_module_t *module, core_string_t name);

ir_instr_t *
n00bb_branch_on_outcome(ir_lower_context_t *context, ir_value_t *outcome,
                        ir_block_t *ok_block, ir_block_t *error_block,
                        ir_span_t span);

bool
jumping_condition(parse_grammar_t *grammar, parse_tree_t *node);

bool
n00bb_describe_failure(n00bb_lower_context_t *lower, ir_instr_t *trap,
                       n00bb_failure_kind_t kind, core_string_t text,
                       core_string_t detail);

enum { N00BB_FAILURE_COMPARISONS = 8 };

ir_value_t *
n00bb_condition_operands(ir_lower_context_t *context,
                         n00bb_lower_context_t *lower,
                         parse_grammar_t *grammar, parse_tree_t *expression,
                         ir_block_t *failure, ir_span_t span,
                         core_string_t *texts);

bool
n00bb_close_condition_scratch(n00bb_lower_context_t *lower);

bool
n00bb_emit_branch(ir_lower_context_t *context, ir_value_t *condition,
                  ir_block_t *on_true, ir_block_t *on_false, ir_span_t span);

bool
n00bb_lower_branch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state);

ir_value_t *
n00bb_extract_step(ir_lower_context_t *context, ir_value_t *value,
                   n00bb_projection_step_t step, th_type *type, ir_span_t span);

bool
n00bb_lower_pattern(ir_lower_context_t *context, parse_visit_t *visit,
                    bool entering, void *state);

bool
n00bb_emit_jump(ir_lower_context_t *context, ir_block_t *target,
                ir_span_t span);

int
n00bb_route_try_error(ir_lower_context_t *context, parse_tree_t *source,
                      ir_value_t *error, ir_span_t span);

bool
n00bb_lower_try_scope(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *state);

bool
n00bb_lower_branch_body(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state);

bool
n00bb_resume_unreachable_statement(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *opaque);

bool
n00bb_enter_case_patterns(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *opaque);
bool
n00bb_overloaded(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                 parse_tree_t *callee, parse_sym_entry_t *symbol);
bool
n00bb_prune_case_name(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *opaque);


bool
n00bb_lower_typecase_scope(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state);

bool
n00bb_lower_switch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state);

bool
n00bb_lower_variant_payload(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state);

bool
n00bb_lower_loop_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state);

bool
n00bb_lower_jump_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state);

ir_lower_result_t
n00bb_lower_return(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state);

ir_value_t *
n00bb_comparison_is(ir_lower_context_t *context, ir_value_t *left,
                    ir_value_t *right, core_string_t member, ir_span_t span);
ir_value_t *
n00bb_emit_enum_member(ir_lower_context_t *context, parse_sym_entry_t *member,
                       ir_span_t span);

parse_sym_entry_t *
n00bb_comparison_member(n00bb_link_analysis_t *analysis, th_type *type,
                        core_string_t name);


ir_lower_result_t
n00bb_lower_operator_method(ir_lower_context_t *context,
                            core_string_t method,
                            ir_value_t *const *operands,
                            size_t operand_count, th_type *result_type,
                            ir_span_t span);

ir_lower_result_t
n00bb_lower_binary(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_nil_literal(ir_lower_context_t *context, parse_visit_t *visit,
                        ir_value_t *const *operands, size_t operand_count,
                        th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_typeof(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state);

bool
foreign_pointer_type(th_type *type);

ir_lower_result_t
n00bb_lower_cast(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_emit_cast(ir_lower_context_t *context, parse_grammar_t *grammar,
                parse_tree_t *operand_node, ir_value_t *const *operands,
                th_type *target, bool unsafe_cast, ir_span_t span);

ir_lower_result_t
n00bb_lower_bool_not(ir_lower_context_t *context, parse_visit_t *visit,
                     ir_value_t *const *operands, size_t operand_count,
                     th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_complement(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               ir_value_t *const *operands,
                               size_t operand_count, th_type *result_type,
                               ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_negate(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state);

bool
n00bb_skip_i128_min_magnitude(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state);

bool
n00bb_lower_scalar_litmod(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state);
ir_lower_result_t
n00bb_lower_user_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         n00bb_litmod_site_t *site, th_type *result_type,
                         ir_span_t span);

bool
n00bb_lower_tuple_parts(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state);

bool
n00bb_lower_named_field_parts(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state);

ir_value_t *
n00bb_object_allocate(ir_lower_context_t *context, th_type *object_type,
                      ir_value_t **storage, ir_span_t span);

ir_value_t *
n00bb_emit_fresh_object(ir_lower_context_t *context, ir_value_t *storage,
                        th_type *object_type, ir_span_t span);

ir_value_t *
n00bb_object_from_fields(ir_lower_context_t *context, ir_value_t *fields,
                         th_type *object_type, ir_span_t span);

bool
n00bb_lower_tuple_literal(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state);

ir_lower_result_t
n00bb_lower_char_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_value_t *const *operands, size_t operand_count,
                         th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_emit_char_array(ir_lower_context_t *context, const uint8_t *bytes,
                      size_t length, th_type *result_type, ir_span_t span);

ir_lower_result_t
n00bb_lower_string_literal(ir_lower_context_t *context,
                           parse_visit_t *visit,
                           ir_value_t *const *operands,
                           size_t operand_count, th_type *result_type,
                           ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_sequence_literal(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_brace_literal(ir_lower_context_t *context,
                          parse_visit_t *visit,
                          ir_value_t *const *operands,
                          size_t operand_count, th_type *result_type,
                          ir_span_t span, void *state);




ir_instr_t *
n00bb_emit_uint_constant(ir_lower_context_t *context, th_type *uint_type,
                         uint64_t value, ir_span_t span, core_string_t prefix);

ir_lower_result_t
n00bb_lower_callback_create(ir_lower_context_t *context,
                            parse_visit_t *visit,
                            ir_value_t *const *operands,
                            size_t operand_count,
                            th_type *result_type, ir_span_t span,
                            void *opaque);

ir_instr_t *
n00bb_emit_checked_product(ir_lower_context_t *context, ir_value_t *left,
                           ir_value_t *right, th_type *uint_type,
                           ir_span_t span);

ir_value_t *
n00bb_coerce_array_dimension_value(ir_lower_context_t *context,
                                   parse_tree_t *expression,
                                   ir_value_t *value, th_type *uint_type,
                                   ir_value_t *one, ir_span_t span,
                                   bool require_positive);

ir_value_t *
n00bb_lower_array_dimension_value(ir_lower_context_t *context,
                                  parse_tree_t *expression,
                                  th_type *uint_type, ir_value_t *one,
                                  ir_span_t span, bool require_positive);

ir_value_t *
n00bb_emit_array_allocation(ir_lower_context_t *context, th_type *leaf,
                            uint32_t rank, ir_shape_extent_t *extents,
                            th_type *result_type, ir_span_t span);

ir_lower_result_t
n00bb_lower_array_new(ir_lower_context_t *context, parse_visit_t *visit,
                      ir_value_t *const *operands, size_t operand_count,
                      th_type *result_type, ir_span_t span, void *state);

bool
n00bb_emit_call_copyout(ir_lower_context_t *context,
                        const n00bb_copyout_t *copyout,
                        ir_value_t *successful_result, ir_span_t span);

bool
n00bb_emit_scoped_cleanups(ir_lower_context_t *context,
                           parse_tree_t *exit_node, uint64_t stop_order,
                           n00bb_try_frame_t *handler, bool success,
                           uint64_t before_order);

bool
n00bb_emit_scoped_success(ir_lower_context_t *context,
                          parse_tree_t *exit_node,
                          n00bb_loop_frame_t *stop_before);

bool
n00bb_emit_with_success(ir_lower_context_t *context, parse_tree_t *exit_node);

bool
n00bb_emit_with_failure(ir_lower_context_t *context, parse_tree_t *exit_node);

ir_value_t *
n00bb_pack_rest_values(ir_lower_context_t *context, th_type *element,
                       const core_list_t(n00bb_ir_value_ptr_t) *values,
                       ir_span_t span);

int
n00bb_context_call(ir_lower_context_t *context, parse_visit_t *visit,
                   th_type *result_type, ir_span_t span,
                   ir_lower_result_t *result);

bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state);

bool
n00bb_mark_reference_target(ir_lower_context_t *context,
                            parse_grammar_t *grammar,
                            parse_tree_t *owner, parse_tree_t *target);

bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state);

bool
n00bb_mark_constructor_lvalues(ir_lower_context_t *context,
                               parse_visit_t *visit, bool entering,
                               void *state);

ir_lower_result_t
n00bb_lower_module_member(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *const *operands, size_t operand_count,
                          th_type *result_type, ir_span_t span, void *state);

ir_func_t *
n00bb_record_field_method(ir_lower_context_t *context,
                          const th_env_method_t *selected,
                          ir_module_t **owner_module);

ir_value_t *
n00bb_record_field_call(ir_lower_context_t *context, ir_block_t *block,
                        const th_env_method_t *selected,
                        ir_value_t *left, ir_value_t *right,
                        th_type *result_type, core_string_t name);

bool
n00bb_emit_panic(ir_lower_context_t *context, ir_value_t *message,
                 ir_span_t span);

bool
n00bb_generate_value_methods(n00bb_link_analysis_t *analysis);

bool
n00bb_generate_dispatch_adapters(n00bb_link_analysis_t *analysis);

ir_value_t *
n00bb_emit_iteration_call(ir_lower_context_t *context, core_string_t method,
                          ir_value_t *receiver, th_mode receiver_mode,
                          th_type *return_type, ir_value_t **copyout_value,
                          parse_tree_t *source);

bool
n00bb_emit_iteration_cleanup(ir_lower_context_t *context,
                             n00bb_loop_frame_t *frame,
                             parse_tree_t *source);

ir_value_t *
n00bb_emit_consume_value(ir_lower_context_t *context, ir_value_t *value,
                         ir_value_t *binding, ir_span_t span);

ir_value_t *
n00bb_emit_consume(ir_lower_context_t *context, parse_grammar_t *grammar,
                   core_translation_unit_t *unit, parse_tree_t *argument,
                   ir_value_t *value);

ir_value_t *
n00bb_typed_integer_literal(ir_lower_context_t *context,
                            parse_tree_t *node, th_type *target);

ir_value_t *
n00bb_emit_wrapping(ir_lower_context_t *context, ir_op_t op,
                    ir_value_t *payload, th_type *target, ir_span_t span);

ir_value_t *
widened_value(ir_lower_context_t *context, ir_value_t *value, th_type *target,
              ir_span_t span);

ir_value_t *
n00bb_materialize(ir_lower_context_t *context, parse_tree_t *node,
                  ir_value_t *value, th_type *target, ir_span_t span);

typedef struct {
    parse_sym_entry_t *symbol;
    ir_local_t *local;
    ir_value_t *global;
    ir_value_t *address;
    parse_tree_t *index_target;
    parse_tree_t *lvalue;
    th_type *type;
} n00bb_out_location_t;

n00bb_out_location_t
n00bb_out_argument_location(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *argument);

ir_value_t *
n00bb_out_location_read(ir_lower_context_t *context, parse_tree_t *argument,
                        const n00bb_out_location_t *location, ir_span_t span);

ir_lower_result_t
n00bb_emit_intrinsic(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                     ir_value_t *const *operands, size_t count,
                     bool copyouts, th_type *call_result_type,
                     ir_span_t span);

ir_value_t *
n00bb_comptime_flag(ir_module_t *module, th_type *type);
bool
n00bb_emit_comptime_call(ir_lower_context_t *context, ir_func_t *callee,
                         ir_span_t span);

ir_lower_result_t
n00bb_lower_call(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state);

ir_lower_result_t
n00bb_lower_constructor_new(ir_lower_context_t *context, parse_visit_t *visit,
                            th_type *result_type, ir_span_t span);

ir_value_t *
n00bb_emit_outcome_payload(ir_lower_context_t *context, ir_value_t *outcome,
                          core_string_t arm, th_type *type, ir_span_t span);

bool
n00bb_operator_error_text(ir_lower_context_t *context, ir_diagcode_t code,
                          ir_span_t span, core_string_t message);

#define n00bb_operator_error(context, code, span, literal) \
    n00bb_operator_error_text((context), (code), (span), \
                              CORE_STRING_STATIC(literal))

bool
names_symbol_directly(parse_tree_t *node);

bool
n00bb_unwrap_call_outcome(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *outcome, ir_span_t span,
                          ir_value_t **success, bool trap_unhandled);

bool
n00bb_lower_operator_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering,
                                void *state);

bool
n00bb_attach_symbol_array_shape(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_sym_entry_t *symbol, th_type *type,
                                ir_value_t *value, ir_local_t *local);

bool
n00bb_lower_signature_array_shape(ir_lowerer_t *lowerer,
                                  ir_lower_context_t *context,
                                  parse_grammar_t *grammar,
                                  ir_shape_t *shape);

bool
n00bb_lower_declare_local(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state);

bool
n00bb_has_index_store_target(parse_grammar_t *grammar, parse_tree_t *node);

bool
n00bb_lower_index_store_at(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           parse_tree_t *site_node,
                           parse_tree_t *target, parse_tree_t *source,
                           ir_value_t *source_value);

bool
n00bb_shape_has_constraints(ir_shape_t *shape);

parse_sym_entry_t *
n00bb_assignment_root_symbol(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *target);

ir_value_t *
n00bb_reference_view(ir_lower_context_t *context, ir_value_t *value,
                     ir_span_t span);

ir_value_t *
n00bb_reference_hook_address(ir_lower_context_t *context,
                             ir_value_t *reference, bool writable,
                             ir_span_t span);

ir_value_t *
n00bb_reference_placeholder(ir_lower_context_t *context,
                            ir_value_t *reference, th_type *address_type,
                            bool writable, ir_span_t span);

bool
n00bb_from_reference_address(ir_value_t *value);

bool
n00bb_call_argument(parse_grammar_t *grammar, parse_tree_t *node);

ir_value_t *
n00bb_record_field_address(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node, ir_span_t span);

// A container element reached through `__read` has no address. Assigning
// to one of its fields copies the element into a temporary, updates the
// field there, and stores the temporary back through `__write`.
typedef struct {
    parse_tree_t *index_node;
    ir_local_t *element;
} n00bb_index_writeback_t;

ir_value_t *
n00bb_index_writeback_place(ir_lower_context_t *context,
                            parse_grammar_t *grammar,
                            core_translation_unit_t *unit, parse_tree_t *node,
                            n00bb_index_writeback_t *writeback, ir_span_t span);

bool
n00bb_index_writeback_finish(ir_lower_context_t *context,
                             parse_grammar_t *grammar, parse_tree_t *site,
                             const n00bb_index_writeback_t *writeback,
                             ir_span_t span);

ir_value_t *
n00bb_addressable_expression_address(ir_lower_context_t *context,
                                     parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node, ir_span_t span);

bool
n00bb_lower_field_receiver(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state);

bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state);

bool
n00bb_simple_assignment_field_target(parse_grammar_t *grammar,
                                     parse_tree_t *node);


ir_lower_result_t
n00bb_lower_compound_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit,
                                ir_value_t *const *operands,
                                size_t operand_count,
                                th_type *result_type, ir_span_t span,
                                void *state);

bool
n00bb_capture_compound_target(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state);

const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node);

core_string_t
context_hook_binding_role(parse_grammar_t *grammar, parse_tree_t *hook);

bool
context_default_role(size_t index, char *buffer, size_t size,
                     core_string_t *out);

bool
context_interface_dependencies(parse_sym_entry_t *symbol, size_t target,
                                size_t *indices, size_t capacity,
                                size_t *count);

bool
n00bb_failure_channel_accepts(ir_lower_context_t *context,
                              th_type *failure);

typedef struct {
    const core_translation_unit_t *referrer;
    const core_link_declaration_t *target;
} reference_pair_t;

th_type *
lowered_rest_type(th_ctx *types, const th_param *source, th_mode mode,
                  th_param *lowered);

bool
attach_cached_body(ir_ctx_t *ir_context, ir_func_t *function, ir_func_t *body);

ir_module_t *
n00bb_source_unit_module(void *context, core_string_t name);

ir_module_t *
n00bb_linked_unit_module(n00bb_link_analysis_t *analysis, core_string_t name);

bool
initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                      core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis,
                      uint32_t producer_stage);

bool
validate_module_declarations(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             ir_ctx_t *ir_context);

bool
n00bb_initialize_default_params(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                core_list_t(n00bb_default_param_t) *params);

bool
n00bb_initialize_default_reference(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *state);

bool
bind_missing_external(ir_lower_context_t *context,
                      parse_sym_entry_t *symbol);

bool
n00bb_check_initialized_fields(n00bb_link_analysis_t *analysis);

bool
n00bb_open_result_variable(ir_lower_context_t *context,
                           parse_sym_entry_t *function);

bool
lower_ir_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
              core_translation_unit_t *unit, parse_sym_entry_t *symbol,
              parse_tree_t *body, ir_func_t *function,
              core_list_t(parse_sym_entry_ptr_t) *parameters,
              core_list_t(n00bb_tree_ptr_t) *roots,
              parse_sym_entry_t *rest_parameter,
              bool expression_body,
              ir_ctx_t *ir_context,
              core_list_t(core_translation_unit_ptr_t) *units,
              n00bb_expression_result_hook_t expression_result_hook,
              void *expression_result_data);

bool
span_contains(ir_span_t span, core_diag_location_t at);

bool
n00bb_cut_tagged_statement(n00bb_tu_state_t *state, ir_func_t *function,
                           uint64_t index);
bool
exclude_function(n00bb_link_analysis_t *analysis,
                 core_translation_unit_t *unit, ir_func_t *function);

void
exclude_failed_functions(n00bb_link_analysis_t *analysis,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         size_t *marks);

bool
explained_by_errors(core_list_t(core_translation_unit_ptr_t) *units);

bool
lower_function_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                    core_translation_unit_t *unit,
                    parse_sym_entry_t *symbol,
                    ir_ctx_t *ir_context,
                    core_list_t(core_translation_unit_ptr_t) *units);

bool
n00bb_lower_assert_statement(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering,
                             void *state);

bool
lower_context_hook(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                   core_translation_unit_t *unit,
                   parse_sym_entry_t *context_symbol,
                   parse_tree_t *hook, ir_ctx_t *ir_context,
                   core_list_t(core_translation_unit_ptr_t) *units);

bool
lower_context_default(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *context_symbol, size_t parameter_index,
                      ir_ctx_t *ir_context,
                      core_list_t(core_translation_unit_ptr_t) *units);

typedef struct {
    core_list_t(n00bb_ir_func_ptr_t) *functions;
    core_list_t(n00bb_ir_func_ptr_t) *cached;
    const n00bb_cache_t *cache;
} reachable_collection_t;

ir_func_t *
n00bb_root_entry(const n00bb_link_analysis_t *analysis, core_string_t name);


bool
build_reachable_functions(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_diagnostics_t *diagnostics);

bool
n00bb_boxed_signature_position(ir_module_t *module, th_type *type);

void
runtime_global_final_state(
    n00bb_runtime_init_analysis_t *runtime,
    ir_func_t *const *roots, size_t root_count, ir_global_t *global,
    bool *phase_succeeds, bool *definitely_initialized,
    bool *has_successful_write);

bool
lower_all_module_initializers(
    n00bb_link_analysis_t *analysis, ir_lowerer_t *lowerer,
    parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    ir_ctx_t *ir_context);

void
dump_ir_analysis(ir_module_t *module, bool control_flow, bool full_pdg);

typedef enum {
    EFFECTS_QUERY_NONE,
    EFFECTS_QUERY_HAS_EFFECT,
    EFFECTS_QUERY_READS,
    EFFECTS_QUERY_WRITES,
    EFFECTS_QUERY_USER,
} effects_query_t;

bool
record_trait_checks(parse_grammar_t *grammar, core_translation_unit_t *unit,
                    ir_module_t *module, ir_ctx_t *context, int32_t file_id);

bool
record_mark_annotations(parse_grammar_t *grammar,
                        core_translation_unit_t *unit,
                        n00bb_link_analysis_t *analysis, ir_ctx_t *context);

bool
restore_declarations(n00bb_link_analysis_t *analysis);

bool
publish_declarations(n00bb_link_analysis_t *analysis);

n00bb_user_effect_t *
user_effect(n00bb_link_analysis_t *analysis, core_string_t qualified);

void
collect_user_effects(n00bb_link_analysis_t *analysis);

bool
answer_query(n00bb_link_analysis_t *analysis, ir_func_t *subject,
             n00bb_query_t query, core_string_t effect);

ir_lower_result_t
n00bb_lower_effect_query(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_value_t *const *operands, size_t operand_count,
                         th_type *result_type, ir_span_t span, void *state);

bool
n00bb_lower_effect_declaration(ir_lower_context_t *context,
                               parse_visit_t *visit, bool entering,
                               void *opaque);

bool
n00bb_lower_mark_declaration(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering,
                             void *opaque);

bool
n00bb_lower_effect_check(ir_lower_context_t *context, parse_visit_t *visit,
                         bool entering, void *opaque);

bool
n00bb_open_hook(const ir_instr_t *call, core_string_t *trait,
                core_string_t *method);
bool
bounded_call_target(n00bb_link_analysis_t *analysis, const ir_instr_t *call,
                    ir_summary_target_fn add, void *sink);

void
push_effect_check_subjects(n00bb_link_analysis_t *analysis,
                           core_list_t(n00bb_ir_func_ptr_t) *roots);

bool
evaluate_effect_checks(n00bb_link_analysis_t *analysis);

bool
check_trait_implementations(n00bb_link_analysis_t *analysis);

bool
check_callback_bounds(n00bb_link_analysis_t *analysis);

bool
check_marks(n00bb_link_analysis_t *analysis);

ir_func_t *
callback_handler(const ir_instr_t *creation);

void
param_text(core_buffer_t *out, const ir_func_t *function, uint32_t p);

uint32_t
witness_chain(n00bb_link_analysis_t *analysis, const ir_summary_t *summary,
              uint32_t flag, const ir_switness_t **chain);

void
witness_text(const ir_switness_t *witness, core_buffer_t *out);

void
function_name_text(core_buffer_t *out, const ir_func_t *function);

core_translation_unit_t *
unit_for_module(n00bb_link_analysis_t *analysis, const ir_module_t *module);

bool
named_chain_notes(n00bb_link_analysis_t *analysis,
                  core_diagnostics_t *diagnostics, ir_func_t *subject,
                  core_string_t effect);

void
explain_chain_notes(n00bb_link_analysis_t *analysis,
                    core_diagnostics_t *diagnostics, ir_func_t *subject,
                    uint32_t flag, core_string_t effect);

uint32_t
bound_witness_flag(const ir_summary_t *summary, core_string_t effect_name);

bool
clause_applies(n00bb_link_analysis_t *analysis,
               const core_list_t(n00bb_effect_clause_t) *clauses,
               const ir_func_t *caller, const ir_func_t *callee);

bool
user_span(const ir_instr_t *in);

void
push_effect_query_subjects(n00bb_link_analysis_t *analysis,
                           core_list_t(n00bb_ir_func_ptr_t) *roots);

bool
fold_function_effects(n00bb_link_analysis_t *analysis);

void
collect_callback_handlers(n00bb_link_analysis_t *analysis);

bool
n00bb_summary_call_targets(void *context, const ir_instr_t *call,
                           ir_summary_target_fn add, void *sink);

uint32_t
n00bb_summary_call_flags(void *context, const ir_instr_t *call);

bool
n00bb_summary_call_finalizers(void *context, const ir_instr_t *call,
                              const ir_func_t *target,
                              ir_summary_target_fn add, void *sink);

ir_summary_t *
n00bb_summary_axiom(void *context, ir_summaries_t *summaries,
                    const ir_func_t *function);

typedef struct {
    ir_func_t *function;
    uint64_t   mask;
} entry_param_t;

core_list_decl(entry_param_t);

const ir_summary_t *
n00bb_summary_entries(void *context, ir_summaries_t *summaries,
                      uint32_t flag);

void
n00bb_push_summary_roots(n00bb_link_analysis_t *analysis,
                         core_list_t(n00bb_ir_func_ptr_t) *roots);

bool
n00bb_summary_global_reachable(void *context, const ir_global_t *global);

typedef core_list_t(core_string_t) text_list_t;

typedef struct {
    core_string_t name;
    ir_spath_t    to;
} reader_dest_t;

core_list_decl(reader_dest_t);

void
dump_function_summaries(n00bb_link_analysis_t *analysis);

void
push_effects_of_roots(n00bb_link_analysis_t *analysis,
                      core_list_t(n00bb_ir_func_ptr_t) *roots);

enum { WITNESS_DEPTH = 16 };

bool
dump_effects_of(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit);

bool
dump_effects_query(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit);

void
dump_layout_for_unit(parse_grammar_t *grammar, core_translation_unit_t *unit);

typedef struct n00bb_rule_facts_t {
    core_link_unit_t *link;
    ir_summaries_t *summaries;
    core_dict_t groups;
    size_t count;
    size_t *starts;
    ir_func_t **members;
    ir_summary_facts_t **facts;
    ir_summary_facts_t **explained;
} n00bb_rule_facts_t;

size_t
rule_group(n00bb_rule_facts_t *rules, const ir_func_t *function);

void
rule_facts_begin(n00bb_link_analysis_t *analysis, core_link_unit_t *link,
                 const core_list_t(n00bb_ir_func_ptr_t) *entries);

void
rule_facts_end(n00bb_link_analysis_t *analysis);

ir_summary_facts_t *
group_facts(n00bb_link_analysis_t *analysis, const ir_func_t *function,
            bool provenance);

ir_instr_t *
first_uninitialized_read(n00bb_link_analysis_t *analysis, ir_func_t *function,
                         const ir_splace_t *place, uint8_t entry_state);

bool
initializer_units_share_cycle(core_link_unit_t *link,
                              core_translation_unit_t *left,
                              core_translation_unit_t *right);

void
index_module_globals(n00bb_link_analysis_t *analysis, core_dict_t *out);

bool
collect_initializer_reads(n00bb_link_analysis_t *analysis,
                          core_list_t(n00bb_initializer_read_t) *reads);

bool
span_covers(ir_span_t span, uint32_t line, uint32_t column);

core_diag_location_t
initializer_read_location(const n00bb_initializer_read_t *read);

bool
summary_writes_global(const ir_summary_t *summary, const ir_global_t *global);

bool
order_initializers(core_link_unit_t *link, n00bb_link_analysis_t *analysis,
                   const core_list_t(n00bb_initializer_read_t) *reads,
                   core_list_t(core_translation_unit_ptr_t) *order);

void
runtime_init_analysis_release(n00bb_runtime_init_analysis_t *runtime);

bool
runtime_init_is_target(n00bb_runtime_init_analysis_t *runtime,
                       ir_global_t *global);

bool
runtime_init_entries(parse_grammar_t *grammar,
                     n00bb_link_analysis_t *analysis,
                     n00bb_runtime_init_analysis_t *runtime);

bool
runtime_init_analysis_build(
    core_list_t(core_translation_unit_ptr_t) *runtime_initializer_order,
    const core_list_t(n00bb_initializer_read_t) *reads,
    n00bb_runtime_init_analysis_t *runtime);

bool
build_program_graph(n00bb_link_analysis_t *analysis,
                    n00bb_runtime_init_analysis_t *runtime);

void
release_program_graph(n00bb_link_analysis_t *analysis,
                      n00bb_runtime_init_analysis_t *runtime);

bool
runtime_reaches(n00bb_runtime_init_analysis_t *runtime,
                const ir_func_t *function);

bool
report_bookkeeping_writes(n00bb_link_analysis_t *analysis);

bool
lease_storage_record(th_type *record);

bool
check_lease_storage(n00bb_link_analysis_t *analysis);

void
bind_summary_hooks(n00bb_link_analysis_t *analysis, bool defer_trap_proofs);

bool
build_function_summaries(n00bb_link_analysis_t *analysis,
    const core_list_t(n00bb_ir_func_ptr_t) *initializers,
    const core_list_t(n00bb_ir_func_ptr_t) *entries);

bool
build_escape_facts(n00bb_link_analysis_t *analysis);

bool
mark_diverging_calls(n00bb_link_analysis_t *analysis);

typedef struct {
    ir_sreturn_t returned;
    uint32_t     targets;
} call_return_t;

bool
mark_external_call_returns(n00bb_link_analysis_t *analysis);

core_string_t
ir_value_source_name(const ir_value_t *value);

bool
validate_consumed_bindings(n00bb_link_analysis_t *analysis);

const ir_instr_t *
assert_side_effect_call(ir_summary_facts_t *facts, n00bb_assert_site_t *site);

typedef struct {
    core_list_t(core_string_t) clauses;
    bool ignored;
    ir_summary_step_t seen[16];
    const ir_func_t *writer;
    const ir_global_t *written;
} n00bb_rule_chain_t;

void
report_chain(core_diagnostics_t *diagnostics,
             const core_translation_unit_t *unit, n00bb_diag_t id,
             core_diag_location_t at, n00bb_text_t plain,
             n00bb_text_t explained, n00bb_rule_chain_t *chain);

bool
describe_rule_step(void *context, const ir_summary_step_t *step);

typedef struct {
    ir_summary_facts_t      *facts;
    core_translation_unit_t *unit;
} dead_code_context_t;

bool
call_diverges_by_rule(void *context, const ir_instr_t *call);

struct n00bb_diagnostic_filter_t {
    core_dict_t dead_code_sites;
    bool dead_code_sites_ready;
    core_list_t(n00bb_failed_region_t) failed_regions;
};

n00bb_diagnostic_filter_t *
n00bb_diagnostic_filter_take(n00bb_link_analysis_t *analysis);

void
n00bb_diagnostic_filter_free(n00bb_diagnostic_filter_t *filter);

bool
n00bb_diagnostic_visible(void *context, const core_diagnostic_t *diagnostic);

bool
diagnose_overflow(n00bb_link_analysis_t *analysis);

typedef struct {
    core_diag_location_t location;
    uint64_t start_byte;
    uint64_t end_byte;
    ir_interval_t range;
} ast_range_t;

core_list_decl(ast_range_t);

void
dump_unit_ranges(core_translation_unit_t *unit);

typedef enum {
    COMPARISON_UNKNOWN,
    COMPARISON_TRUE,
    COMPARISON_FALSE,
} comparison_fact_t;

bool
diagnose_ranges(n00bb_link_analysis_t *analysis);

typedef void (*function_value_fn)(void *context, const ir_value_t *value);

typedef struct {
    core_list_t(n00bb_ir_func_ptr_t) *entries;
    core_dict_t                       listed;
} taken_entries_t;

void
add_address_taken_functions(const core_list_t(n00bb_ir_func_ptr_t) *functions,
                            core_list_t(n00bb_ir_func_ptr_t) *entries);

typedef struct {
    core_dict_t taken;
    bool        entry_natives;
} taken_values_t;

core_list_t(n00bb_ir_func_ptr_t)
thread_entries(const core_list_t(n00bb_ir_func_ptr_t) *functions,
               ir_summaries_t *summaries, bool *complete,
               core_list_t(entry_param_t) *params_out);

bool
atomic_type(th_type *type);

core_list_decl(ir_splace_t);

typedef struct {
    ir_func_t *entry;
    uint32_t starts;
    core_list_t(ir_splace_t) arguments;
} race_start_t;

core_list_decl(race_start_t);

core_list_decl(ir_race_entry_t);

core_list_decl(ir_race_t);

typedef struct {
    core_list_t(race_start_t) list;
    core_dict_t               index;
} race_starts_t;

void
warn_shared_global_races(n00bb_link_analysis_t *analysis,
    const core_list_t(n00bb_ir_func_ptr_t) *runtime_entries);

bool
annotated_true(const ir_func_t *function, core_string_t key);

core_string_t
named_type_name(th_type *type);

void
n00bb_report_unused_items(n00bb_link_analysis_t *analysis);

void
warn_dead_stores(n00bb_link_analysis_t *analysis);

typedef struct {
    int32_t  file;
    uint32_t start;
    uint32_t param;
} param_site_t;

void
warn_unused_params(n00bb_link_analysis_t *analysis);

void
warn_rule_diagnostics(n00bb_link_analysis_t *analysis);

void
warn_unbounded_recursion(n00bb_link_analysis_t *analysis);

bool
validate_missing_returns(n00bb_link_analysis_t *analysis);

bool
validate_assert_side_effects(n00bb_link_analysis_t *analysis);
bool
validate_frozen_writes(n00bb_link_analysis_t *analysis);

bool
place_unit_allocations(n00bb_link_analysis_t *analysis);

bool
n00bb_recompute_summaries(n00bb_link_analysis_t *analysis);

bool
validate_uninitialized_global_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    const core_list_t(n00bb_initializer_read_t) *reads);

bool
validate_thread_function_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    bool comptime_phase_succeeds);

bool
n00bb_dump_ir(const n00bb_link_analysis_t *analysis);

n00bb_lower_context_t *
n00bb_solved_lower_new(n00bb_link_analysis_t *analysis,
                       parse_grammar_t *grammar,
                       core_translation_unit_t *unit);

bool
lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                     n00bb_link_analysis_t *analysis,
                     ir_ctx_t *ir_context,
                     core_diagnostics_t *diagnostics,
                     bool dump_ir,
                     bool dump_control_flow, bool dump_pdg,
                     bool dump_summaries, bool dump_layout);

bool
n00bb_register_callback_representation(ir_module_t *module, th_type *type);

bool
n00bb_static_value_to_ir_initializer(const n00bb_static_value_t *value,
                                    th_type *type,
                                    n00bb_tu_state_t *state,
                                    ir_global_init_value_t *out,
                                    size_t depth);

bool n00bb_emit_store(ir_lower_context_t *context, ir_value_t *address,
                      ir_value_t *value, bool initializes, ir_span_t span);
bool n00bb_emit_local_assign(ir_lower_context_t *context, ir_local_t *local,
                             ir_value_t *value, bool initializes,
                             ir_span_t span);

#endif
