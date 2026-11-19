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

// A construct with code to run on every way out of it: a `with` manager
// (`__exit`) or a loop over a container (`__cleanup`). While lowering is
// inside it, every way out sets `reason` and `failure` and jumps to its one
// `exit` block, which runs that code once and goes on by `reason`:
// N00BB_EXIT_FALLTHROUGH, past the construct; N00BB_EXIT_FAILURE, on to the
// next failure handler (`failure` holds the failure, and is `nil` for every
// other reason); or, for a `return`, `break` or `continue` leaving through
// it, N00BB_EXIT_RESUMES plus the index of the block in `resumes` that goes
// on from there. A failure handler is the innermost of these and the `try`s
// whose bodies lowering is in.
typedef struct {
    ir_block_t *exit;
    ir_local_t *reason;
    th_type *failure_type;
    ir_local_t *failure;
    core_list_t(n00bb_ir_block_ptr_t) resumes;
    // Against the other control frames' orders: a smaller order encloses.
    uint64_t order;
    // Lowering is inside the construct, so ways out go through `exit`.
    bool open;
    // A `with` manager handles the failures it is given; a loop passes
    // them on, so a failure goes through a loop only on its way to a
    // handler outside it.
    bool handles;
    bool fallthrough;
    bool failure_reachable;
} n00bb_exit_scope_t;

// A failure's reason is above all the others, so when no failure can reach
// an exit block, the range of its reason shows the failure test is false.
#define N00BB_EXIT_FALLTHROUGH UINT64_C(0)
#define N00BB_EXIT_RESUMES UINT64_C(1)
#define N00BB_EXIT_FAILURE UINT64_C(0xffffffff)

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
    // Set while the body reads a variable narrowed by `has_type`.
    void *binding_checkpoint;
    bool narrowed;
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
    // The cursor lives in a local: `__next` takes it by reference.
    ir_local_t *iteration_cursor;
    core_list_t(parse_sym_entry_ptr_t) iteration_bindings;
    // The loop's binding pattern, which the bindings are projected from.
    parse_tree_t *iteration_pattern;
    void *binding_checkpoint;
    // The bindings when the loop began: a variable bound since then is
    // declared inside the loop.
    void *entry_bindings;
    // A loop over a container calls `__cleanup` on every way out of it.
    n00bb_exit_scope_t scope;
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
    parse_tree_t *flags;
    ir_uint_t mask;
    bool is_range;
    bool inclusive;
    bool exact;
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
    bool flags_dispatched;
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
    parse_tree_t *node;
    parse_tree_t *body;
    parse_tree_t *error_clause;
    parse_sym_entry_t *error_symbol;
    ir_block_t *handler;
    ir_block_t *join;
    ir_local_t *error_value;
    ir_local_t *result_value;
    // When the try was entered, against the other control frames' orders:
    // a frame with a smaller order encloses it.
    uint64_t order;
    bool expression;
    bool error_reachable;
    bool join_reachable;
    bool active;
    // Lowering is still in the try's body, so failures go to its handler.
    bool in_body;
    // An option's `x!` routed `nil` here (with that option's source text
    // and span); and the first failure routed here that `nil` doesn't fit.
    // A handler that binds the error can't hold both.
    bool routes_nil;
    core_string_t nil_text;
    ir_span_t nil_span;
    th_type *other_failure;
} n00bb_try_frame_t;

core_list_decl(n00bb_try_frame_t);

// One manager of a `with` statement. Its exit block runs `__exit`, given
// `value` (what `__enter` returned) and the failure, if any.
typedef struct {
    parse_tree_t *node;
    ir_local_t *manager;
    ir_value_t *value;
    n00bb_exit_scope_t scope;
} n00bb_with_item_t;

core_list_decl(n00bb_with_item_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    core_list_t(n00bb_with_item_t) items;
    bool active;
    // A `with` used as a value: its body's `->` gives the value.
    bool expression;
} n00bb_with_frame_t;

core_list_decl(n00bb_with_frame_t);

// A place passed to an `out` or `modify` parameter. When it is a whole
// variable, `local` or `storage` (a parameter or global) names it, for the
// check that one call is not given the same variable twice. An element a
// container reaches through its accessors has no address, so it is passed as
// the address of `temporary`, which is written back to `container` at
// `index` once the call returns or fails.
typedef struct {
    ir_local_t *local;
    ir_value_t *storage;
    ir_value_t *container;
    ir_value_t *index;
    parse_tree_t *source_node;
    ir_shape_t *required_shape;
    ir_local_t *temporary;
} n00bb_reference_argument_t;

core_list_decl(n00bb_reference_argument_t);

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
    parse_tree_t *node;
    core_list_t(parse_tree_ptr_t) expressions;
    core_list_t(n00bb_ir_value_ptr_t) values;
    // The field names of a named-field literal (empty for a tuple).
    core_list_t(core_string_t) names;
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
    bool reachable;
} n00bb_literal_frame_t;

struct n00bb_lower_context {
    parse_grammar_t *grammar;
    ir_lowerer_t *lowerer;
    void *walk;
    bool bind_external;
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
    bool write_back_open_index;
    core_dict_t node_types;
    bool node_types_ready;
    // The field use each field read (and each place formed from one) was
    // lowered from.
    core_dict_t field_reads;
    bool field_reads_ready;
    core_list_t(n00bb_try_frame_t) tries;
    core_list_t(n00bb_with_frame_t) withs;
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

ir_module_t *
unit_module(n00bb_tu_state_t *state);

parse_tree_t *
value_body_expression(parse_grammar_t *grammar, parse_tree_t *body);

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
                         parse_sym_entry_t *variable, parse_tree_t *argument,
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

// Reports an error when `value`'s length is known while lowering and
// differs from a static extent of `expected`; the shape checks call it.
void
n00bb_report_static_shape_mismatch(ir_lower_context_t *context,
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
                          ir_span_t span);

th_type *
n00bb_indirect_type(th_ctx *types, th_type *referent);

bool
n00bb_set_reference_access(ir_ctx_t *context, ir_value_t *value,
                           th_type *type);

ir_mutability_t
local_binding_mutability(const parse_sym_entry_t *symbol);

bool
n00bb_set_flag(ir_lower_context_t *context, ir_local_t *flag, bool value,
               ir_span_t span);

ir_value_t *
n00bb_emit_temp(ir_lower_context_t *context, ir_op_t op, th_type *type,
                ir_value_t *operand, ir_span_t span);

bool
n00bb_emit_local_finalizer(ir_lower_context_t *context, ir_local_t *handle,
                           const th_env_method_t *method, ir_span_t span);

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
jumping_condition(parse_tree_t *node);

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
n00bb_route_try_error(ir_lower_context_t *context, ir_value_t *error,
                      ir_span_t span);

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
                 parse_tree_t *callee, parse_sym_entry_t *symbol,
                 core_list_t(parse_sym_entry_ptr_t) *candidates);
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

ir_lower_result_t
n00bb_emit_type_literal(ir_lower_context_t *context, th_type *described,
                        ir_span_t span);

ir_lower_result_t
n00bb_lower_typespec_literal(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state);

bool
foreign_pointer_type(th_type *type);

ir_lower_result_t
n00bb_lower_cast(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state);
ir_lower_result_t
n00bb_lower_type_annotation(ir_lower_context_t *context, parse_visit_t *visit,
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

// Starts `scope` for the construct entered with `order`.
bool
n00bb_exit_scope_open(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                      uint64_t order, bool handles, ir_span_t span);

void
n00bb_exit_scope_free(n00bb_exit_scope_t *scope);

// Leaves `scope` for its exit block for `reason`, with `error` when the
// reason is a failure.
bool
n00bb_exit_scope_jump(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                      uint64_t reason, ir_value_t *error, ir_span_t span);

// Closes `scope` once lowering has left the construct, first leaving it by
// falling through when that is reachable. When anything reaches the exit
// block, lowering continues there with `*reached` set, for the construct's
// own exit code; `*failure` is then the failure, or nullptr when no failure
// reaches the block.
bool
n00bb_exit_scope_close(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                       ir_span_t span, bool *reached, ir_value_t **failure);

// Goes on from `scope`'s exit block once its exit code has run: to `after`
// when falling through, or for a failure that `verdict` (if given: what
// the exit code returned) drops by being `true`; back to the resume
// blocks; and otherwise on to the failure handlers outside the construct.
// Lowering continues in `after` if anything reaches it.
bool
n00bb_exit_scope_finish(ir_lower_context_t *context, n00bb_exit_scope_t *scope,
                        ir_block_t *after, ir_value_t *verdict,
                        parse_tree_t *source, n00bb_text_t phrase);

// Fails the function with `error`, raised where only the frames entered
// before `before_order` are open, leaving through the loops still open.
bool
n00bb_emit_function_failure(ir_lower_context_t *context, ir_value_t *error,
                            ir_span_t span, uint64_t before_order);

// Sends `error`, raised where only the frames entered before `before_order`
// are open, to its handler, or out of the function.
bool
n00bb_propagate_failure(ir_lower_context_t *context, parse_tree_t *source,
                        ir_value_t *error, ir_span_t span,
                        uint64_t before_order, n00bb_text_t phrase);

// Leaves through the exit blocks of the constructs entered after
// `stop_order`, innermost first, for a `return`, `break`, `continue` or a
// block's value. `*value`, when there is one, is carried across them, and
// is what was carried after.
bool
n00bb_emit_scoped_success(ir_lower_context_t *context,
                          parse_tree_t *exit_node, uint64_t stop_order,
                          ir_value_t **value);

// Keeps `value` across exit blocks that other paths also reach: in an
// option that is `nil` from the function's entry, so every path has
// written it. n00bb_kept_value reads it back as `type`.
ir_local_t *
n00bb_keep_across_exits(ir_lower_context_t *context, ir_value_t *value,
                        ir_span_t span);

ir_value_t *
n00bb_kept_value(ir_lower_context_t *context, ir_local_t *kept,
                 th_type *type, ir_span_t span);

void
n00bb_free_with_frame(n00bb_with_frame_t *frame);

ir_value_t *
n00bb_pack_rest_values(ir_lower_context_t *context, th_type *element,
                       const core_list_t(n00bb_ir_value_ptr_t) *values,
                       ir_span_t span);

bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state);

bool
n00bb_lower_with_manager(ir_lower_context_t *context, parse_visit_t *visit,
                         bool entering, void *state);

// The innermost exit scope open between `stop_order` and `before_order`.
n00bb_exit_scope_t *
n00bb_innermost_exit_scope(n00bb_lower_context_t *lower, uint64_t stop_order,
                           uint64_t before_order);

// The handler a failure raised where only the frames entered before
// `before_order` are open goes to: the innermost `try` whose body lowering
// is in, or open exit scope. At most one of `*try_frame` and `*scope` is
// set.
void
n00bb_failure_handler(n00bb_lower_context_t *lower, uint64_t before_order,
                      n00bb_try_frame_t **try_frame,
                      n00bb_exit_scope_t **scope);

// As n00bb_route_try_error, for the handlers entered before `before_order`.
int
n00bb_route_failure_before(ir_lower_context_t *context, ir_value_t *error,
                           ir_span_t span, uint64_t before_order);


bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state);


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
                          ir_value_t *receiver, bool write_back,
                          th_type *return_type, parse_tree_t *source);

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
n00bb_shape_has_constraints(ir_shape_t *shape);


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



// A container element reached through `__read` has no address. Assigning
// to one of its fields copies the element into a temporary, updates the
// field there, and stores the temporary back through `__write`.



void
n00bb_note_field_read(ir_lower_context_t *context, ir_value_t *value,
                      parse_tree_t *node);

void
n00bb_note_place_written(ir_lower_context_t *context, ir_value_t *target,
                         bool reads);

ir_value_t *
n00bb_place_of(ir_lower_context_t *context, ir_value_t *value, bool writing,
               ir_span_t span);

ir_value_t *
n00bb_emit_reference(ir_lower_context_t *context, ir_value_t *storage,
                     core_string_t text, ir_span_t span);

ir_value_t *
n00bb_referent_address(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *node, ir_span_t span);

ir_value_t *
n00bb_lower_slice_read(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *index_node, ir_value_t *container,
                       th_type *result_type, ir_span_t span);

ir_value_t *
n00bb_field_storage(ir_lower_context_t *context, ir_value_t *base,
                    core_string_t name, th_type *type, ir_span_t span);

// An element a container reaches through `__read` has no address, so writing
// part of it goes through `element`, a copy, which is stored back to the
// container at `index` with `__write` afterwards. The container is read
// again from `container_place` then, when it has one (it may itself be such
// a copy), and is the value `container` otherwise.
typedef struct {
    ir_value_t *container;
    ir_value_t *container_place;
    ir_value_t *index;
    ir_local_t *element;
} n00bb_element_writeback_t;

core_list_decl(n00bb_element_writeback_t);

ir_value_t *
n00bb_place_for_write(ir_lower_context_t *context, ir_value_t *value,
                      ir_span_t span,
                      core_list_t(n00bb_element_writeback_t) *writebacks);

bool
n00bb_emit_writebacks(ir_lower_context_t *context,
                      core_list_t(n00bb_element_writeback_t) *writebacks,
                      ir_span_t span);

bool
n00bb_store_to(ir_lower_context_t *context, ir_value_t *target,
               ir_value_t *value, bool checked, ir_span_t span);

bool
n00bb_place_root(ir_value_t *place, ir_local_t **local, ir_value_t **storage);

ir_shape_t *
n00bb_place_shape(ir_value_t *place);

bool
n00bb_place_writable(const ir_value_t *place);

bool
n00bb_accessed_element(const ir_value_t *value, ir_value_t **container,
                       ir_value_t **index);



bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state);



ir_lower_result_t
n00bb_lower_compound_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit,
                                ir_value_t *const *operands,
                                size_t operand_count,
                                th_type *result_type, ir_span_t span,
                                void *state);


const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node);

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
n00bb_check_required_fields(n00bb_link_analysis_t *analysis);

// Whether module variable `global`, an array with a declared size, has no
// static storage: its size is only known when the program runs, or its
// elements can't be zeroed statically (objects, which each need one). Its
// module's initializer then allocates it.
static inline bool
n00bb_global_needs_runtime_array(const ir_global_t *global)
{
    ir_shape_storage_t storage;
    return global && !global->has_static_initializer && global->shape
        && ir_global_storage_duration(global) == IR_STORAGE_DURATION_MODULE
        && ir_shape_storage(global->shape, &storage)
        && storage.flat_prefix_rank;
}

bool
n00bb_allocate_runtime_global(ir_lower_context_t *context,
                              parse_sym_entry_t *symbol, ir_span_t span);

// Whether the zero representation of `type` is one of its values, for code
// in `site`: what an array's elements hold when it is created without a
// value for them. When not, `*blame` is the type (it, or a part of it) whose
// zero isn't a value.
bool
n00bb_zero_tolerant(core_translation_unit_t *site, th_type *type,
                    th_type **blame, uint32_t depth);
bool
n00bb_check_zeroed_elements(ir_lower_context_t *context, th_type *element,
                            ir_span_t span);
ir_value_t *
n00bb_emit_let_default_array(ir_lower_context_t *context, th_type *type,
                             ir_span_t span);

// The bounds of slice `contents` (`a:b`, `a until b`, `a to b`, with either
// bound left out) as `__slice` and `__write_slice` take them: each an
// `int?`, nil when left out, then whether `to` includes the end. Reports
// constant bounds in the wrong order, and returns false. (Bounds outside an
// array of known length are checked on the IR: n00bb_check_known_ranges.)
bool
n00bb_lower_slice_arguments(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *contents,
                            ir_value_t *arguments[3]);

// The length of array `value`, in `function`, when it is known without
// running the program: a declared size, a literal's, or the one every whole
// assignment to the variable holding it gives.
bool
n00bb_static_array_length(const ir_func_t *function, const ir_value_t *value,
                          uint64_t *length);

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
bool
n00bb_summary_global_named(void *context, const ir_func_t *function,
                           const ir_global_t *global);
uint8_t
n00bb_summary_global_hidden(void *context, const ir_func_t *caller,
                            const ir_global_t *global);
core_translation_unit_t *
n00bb_visibility_unit(n00bb_link_analysis_t *analysis,
                      const ir_module_t *module);

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
                         const ir_splace_t *place, uint8_t entry_state,
                         bool named_only);

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

bool
validate_unset_locals(n00bb_link_analysis_t *analysis);

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
check_field_array_lengths(n00bb_link_analysis_t *analysis);

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
