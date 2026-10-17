#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

double n00bb_profile_seconds(void);

#include "driver.h"
#include "compiler_cli.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_link_lifetime.h"
#include "compiler_ir_setup.h"
#include "compiler_lower.h"
#include "compiler_layout.h"
#include "compiler_tu_interface.h"
#include "abi_layout.h"
#include "abi_emit.h"
#include "abi_link.h"
#include "abi_semantics.h"
#include "abi_static_graph.h"
#include "compiler_baked.h"
#include "abi_record_layout.h"
#include "record_layout_request.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "frame_roots.h"
#include "global_metadata.h"
#include "llvm_backend.h"
#include "llvm_native_cache.h"
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
#include "lib/diagnostic.h"
#include "logic/clpfd_store.h"
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
#include "ir/exec_graph.h"
#include "ir/doms.h"
#include "ir/analyze.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "th/environment.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "scanner/lex.h"
#include "unicode/encoding.h"
#include "preprocessor.h"
#include "rich_string.h"
#include "signature_typevars.h"

bool
n00bb_prepare_ir_bindings(core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          uint32_t stage_kind)
{
    if (!analysis->ir_context)
        return false;
    if (!initialize_ir_modules(analysis->ir_context, analysis->grammar,
                               link, analysis, stage_kind))
        return false;
    if (!bind_link_declarations_to_ir(analysis->grammar, link, analysis,
                                      stage_kind)) {
        if (!core_link_unit_has_fatal_diagnostics(link))
            core_diagnostics_report(core_translation_unit_diagnostics(
                    analysis->units.data[0]), analysis->units.data[0],
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0031"),
                CORE_STRING_STATIC("could not bind link declarations to Lantern IR"));
        return false;
    }
    if (!select_storage_placement(analysis)) return false;
    if (!core_link_unit_validate(link)) {
        if (!core_diagnostics_failed(core_translation_unit_diagnostics(
                analysis->units.data[0])) &&
            !core_link_unit_has_fatal_diagnostics(link))
            core_diagnostics_report(core_link_unit_diagnostics(link), nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0032"),
                CORE_STRING_STATIC("Lantern rejected the link unit"));
        return false;
    }
    if (!bind_link_references_to_nodes(analysis->grammar, link)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
                analysis->units.data[0]), analysis->units.data[0],
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0033"),
            CORE_STRING_STATIC("could not bind link references to source nodes"));
        return false;
    }
    for (size_t i = 0; i < analysis->units.len; i++)
        if (!validate_module_declarations(analysis->grammar,
                analysis->units.data[i], analysis->ir_context)) {
            if (!core_diagnostics_failed(core_translation_unit_diagnostics(
                    analysis->units.data[i])))
                core_diagnostics_report(core_translation_unit_diagnostics(
                        analysis->units.data[i]), analysis->units.data[i],
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0034"),
                    CORE_STRING_STATIC("could not validate module declarations in Lantern IR"));
            return false;
        }
    return true;
}

enum { N00BB_JOB_LOCATE_TU = 1, N00BB_JOB_PREPROCESS = 2 };

typedef ir_value_t *n00bb_ir_value_ptr_t;
core_list_decl(n00bb_ir_value_ptr_t);
typedef struct {
    parse_tree_t *node;
    size_t parameter_index;
} n00bb_call_actual_t;
core_list_decl(n00bb_call_actual_t);


static bool
host_target_profile(core_target_desc_t *target)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return core_target_profile("arm64-macos", 0, target);
#elif defined(__linux__) && defined(__x86_64__)
    return core_target_profile("x86_64-linux", 0, target);
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return core_target_profile("x86_64-windows", 0, target);
#else
    (void)target;
    return false;
#endif
}



core_list_decl(size_t);
core_list_decl(th_kwparam);
core_list_decl(th_member);

typedef bool (*n00bb_expression_result_hook_t)(ir_lower_context_t *context,
    ir_value_t **value, void *data);


core_list_decl(uint32_t);

typedef struct {
    parse_tree_t *expression;
    ir_global_t *result_global;
} n00bb_array_extent_lowering_t;

typedef struct {
    ir_func_t *function;
    uint8_t *success_states;
    uint8_t *failure_states;
    size_t target_count;
    uint8_t *parameter_success_states;
    uint8_t *parameter_failure_states;
} n00bb_function_effect_summary_t;
core_list_decl(n00bb_function_effect_summary_t);
typedef struct {
    core_list_t(n00bb_ir_func_ptr_t) initializer_roots;
    core_list_t(n00bb_ir_func_ptr_t) entry_functions;
    core_list_t(n00bb_ir_value_ptr_t) targets;
    core_list_t(n00bb_function_effect_summary_t) summaries;
    core_dict_t target_indices;
} n00bb_runtime_init_analysis_t;
typedef struct {
    ir_func_t *function;
    ir_instr_t *instruction;
    core_translation_unit_t *unit;
    core_diag_location_t location;
} n00bb_thread_read_site_t;
core_list_decl(n00bb_thread_read_site_t);

static bool context_hook_ir_name(parse_grammar_t *grammar,
                                 parse_sym_entry_t *symbol,
                                 parse_tree_t *hook,
                                 char *buffer, size_t buffer_size);
static bool context_default_role(size_t index, char *buffer, size_t size);
static bool context_interface_parameter(parse_sym_entry_t *symbol,
                                        size_t target, const char **name,
                                        size_t *name_size, uint8_t *flags,
                                        uint8_t *mode);
static bool context_interface_dependencies(parse_sym_entry_t *symbol,
                                           size_t target, size_t *indices,
                                           size_t capacity, size_t *count);
static bool context_interface_parameter_named(parse_sym_entry_t *symbol,
                                              core_string_t target,
                                              size_t *index);
static size_t context_interface_parameter_count(parse_sym_entry_t *symbol);
static const char *context_hook_binding_role(parse_grammar_t *grammar,
                                              parse_tree_t *hook);
static bool n00bb_context_has_hook(parse_grammar_t *grammar,
                                    parse_sym_entry_t *symbol,
                                    const char *name);
static bool n00bb_emit_with_success(ir_lower_context_t *context,
                                    parse_tree_t *exit_node);
static bool n00bb_emit_with_failure(ir_lower_context_t *context,
                                    parse_tree_t *exit_node);
static bool n00bb_emit_with_failure_before(ir_lower_context_t *context,
                                           parse_tree_t *exit_node,
                                           uint64_t before_order);
static bool n00bb_lower_index_store_at(ir_lower_context_t *context,
                                       parse_grammar_t *grammar,
                                       parse_tree_t *site_node,
                                       parse_tree_t *target,
                                       parse_tree_t *source,
                                       ir_value_t *source_value);
static parse_sym_entry_t *n00bb_assignment_root_symbol(
    parse_grammar_t *grammar, core_translation_unit_t *unit,
    parse_tree_t *target);
static ir_value_t *n00bb_record_field_address(
    ir_lower_context_t *context, parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node, ir_span_t span);
static ir_value_t *n00bb_addressable_expression_address(
    ir_lower_context_t *context, parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node, ir_span_t span);
static th_type *lowered_rest_type(th_ctx *types, const th_param *source,
                                  th_mode mode, th_param *lowered);
static ir_value_t *n00bb_emit_outcome_payload(ir_lower_context_t *context,
                                               ir_value_t *outcome,
                                               const char *arm,
                                               th_type *type,
                                               ir_span_t span);
static bool n00bb_unwrap_call_outcome(ir_lower_context_t *context,
                                      parse_visit_t *visit,
                                      ir_value_t *outcome, ir_span_t span,
                                      ir_value_t **success,
                                      bool trap_unhandled);

static bool n00bb_check_phase_read(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *state);
static bool n00bb_failure_channel_accepts(ir_lower_context_t *context,
                                           th_type *failure);
static bool n00bb_reject_runtime_initializer_failure(
    ir_lower_context_t *context, ir_span_t span);
static bool n00bb_reference_target(parse_grammar_t *grammar,
                                   parse_tree_t *node);


static bool n00bb_register_callback_representation(ir_module_t *module,
    th_type *type);


static bool n00bb_static_value_to_ir_initializer(
    const n00bb_static_value_t *value, th_type *type,
    n00bb_tu_state_t *state, ir_global_init_value_t *out, size_t depth);

static bool n00bb_simple_assignment_field_target(
    parse_grammar_t *grammar, parse_tree_t *node);
static bool n00bb_reference_target(parse_grammar_t *grammar,
                                   parse_tree_t *node);
static ir_value_t *n00bb_select_trait_method(
    ir_lower_context_t *context, parse_grammar_t *grammar,
    core_translation_unit_t *owner, parse_sym_entry_t *requirement,
    th_type *signature, ir_value_t *receiver, ir_value_t *contract,
    ir_span_t span);
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
    bool expression;
    bool short_circuit;
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

static bool n00bb_emit_scoped_success(ir_lower_context_t *context,
                                      parse_tree_t *exit_node,
                                      n00bb_loop_frame_t *stop_before);

core_list_decl(ir_lit_t);

typedef struct {
    ir_lit_t lower;
    ir_lit_t upper;
    parse_tree_t *expression;
    parse_tree_t *tag_node;
    parse_sym_entry_t *variant_arm;
    core_string_t variant_arm_name;
    bool is_range;
    bool inclusive;
    bool is_variant_tag;
} n00bb_switch_pattern_t;
core_list_decl(n00bb_switch_pattern_t);

typedef struct {
    parse_tree_t *body;
    ir_block_t *block;
    core_list_t(n00bb_switch_pattern_t) patterns;
} n00bb_switch_case_t;
core_list_decl(n00bb_switch_case_t);

typedef struct {
    parse_tree_t *node;
    core_string_t label;
    parse_tree_t *scrutinee;
    parse_sym_entry_t *variant_subject;
    parse_scope_t *variant_scope;
    core_list_t(n00bb_switch_case_t) cases;
    parse_tree_t *else_body;
    ir_block_t *default_block;
    ir_block_t *join;
    uint64_t order;
    ir_local_t *result_local;
    bool expression;
    bool variant_switch;
    bool emitted;
    bool join_reachable;
    bool active;
} n00bb_switch_frame_t;
core_list_decl(n00bb_switch_frame_t);

typedef struct {
    parse_tree_t *node;
    parse_tree_t *body;
    th_type *type;
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

static void
n00bb_order_copyouts(core_list_t(n00bb_copyout_t) *copyouts)
{
    for (size_t i = 1; i < copyouts->len; i++) {
        n00bb_copyout_t item = copyouts->data[i];
        size_t j = i;
        while (j && copyouts->data[j - 1].output_index > item.output_index) {
            copyouts->data[j] = copyouts->data[j - 1];
            j--;
        }
        copyouts->data[j] = item;
    }
}

typedef struct {
    parse_tree_t *node;
    core_list_t(parse_tree_ptr_t) expressions;
    core_list_t(n00bb_ir_value_ptr_t) values;
} n00bb_tuple_parts_t;
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


typedef struct {
    parse_grammar_t *grammar;
    ir_lowerer_t *lowerer;
    ir_ctx_t *ir_context;
    core_list_t(core_translation_unit_ptr_t) *units;
    core_translation_unit_t *unit;
    parse_tree_t *function_body;
    parse_sym_entry_t *function_symbol;
    ir_shape_t *bound_return_shape;
    core_list_t(n00bb_branch_frame_t) branches;
    core_list_t(n00bb_loop_frame_t) loops;
    core_list_t(n00bb_switch_frame_t) switches;
    core_list_t(n00bb_typecase_frame_t) typecases;
    core_list_t(n00bb_value_block_frame_t) value_blocks;
    core_list_t(n00bb_try_frame_t) tries;
    core_list_t(n00bb_with_frame_t) withs;
    core_list_t(n00bb_copyout_t) parameters_to_copy_out;
    core_list_t(n00bb_pending_copyouts_t) pending_call_copyouts;
    core_list_t(n00bb_default_param_t) default_params;
    n00bb_default_param_t *active_default_param;
    core_list_t(n00bb_tuple_parts_t) tuple_parts;
    n00bb_literal_frame_t active_literal;
    bool has_active_literal;
    uint64_t next_block_id;
    uint64_t next_control_order;
} n00bb_lower_context_t;

static ir_instr_t *n00bb_emit_closed_trait_call(
    ir_lower_context_t *context, n00bb_lower_context_t *lower,
    n00bb_trait_dispatch_plan_t *plan, parse_grammar_t *grammar,
    parse_sym_entry_t *requirement, parse_tree_t *formal_params,
    size_t parameter_count, th_type *signature, th_type *result_type,
    core_list_t(n00bb_ir_value_ptr_t) *values,
    core_list_t(n00bb_copyout_t) *copyouts, ir_span_t span);

static ir_value_t *n00bb_emit_iteration_trait_call(
    ir_lower_context_t *context, n00bb_loop_frame_t *frame,
    size_t site_index, ir_value_t *receiver, th_mode receiver_mode,
    th_type *return_type, ir_value_t **copyout_value, parse_tree_t *source);
static bool n00bb_emit_iteration_cleanup(ir_lower_context_t *context,
                                          n00bb_loop_frame_t *frame,
                                          parse_tree_t *source);
static ir_value_t *n00bb_emit_consume_value(
    ir_lower_context_t *context, ir_value_t *value, ir_value_t *binding,
    ir_span_t span);
static bool n00bb_lower_container_body_enter(
    ir_lower_context_t *context, n00bb_lower_context_t *lower,
    n00bb_loop_frame_t *frame);

static bool
n00bb_add_iteration_backedge(n00bb_loop_frame_t *frame,
                              ir_block_t *predecessor)
{
    return !frame || !frame->is_container
        || (frame->iteration_cursor_phi && frame->iteration_cursor_value
            && predecessor
            && ir_instr_operand_phi_arm(frame->iteration_cursor_phi,
                                        predecessor,
                                        frame->iteration_cursor_value));
}

static ir_module_t *
unit_module(n00bb_tu_state_t *state)
{
    return state ? state->ir_module : nullptr;
}

static parse_tree_t *
value_body_expression(parse_grammar_t *grammar, parse_tree_t *body)
{
    parse_tree_t *statements = child_nt(grammar, body, "value-body-stmts");
    parse_tree_t *value = child_nt(grammar, statements, "value-stmt");
    return child_nt(grammar, value, "expression");
}


static parse_tree_t *
n00bb_switch_tag_primary(parse_grammar_t *grammar, parse_tree_t *expression)
{
    parse_tree_t *node = expression;
    while (node && !core_tree_is_leaf(node) && !nt_is(grammar, node, "primary")
           && core_tree_num_children(node) == 1)
        node = core_tree_child(node, 0);
    return nt_is(grammar, node, "primary") ? node : nullptr;
}


static core_string_t
n00bb_control_label(parse_grammar_t *grammar, parse_tree_t *node,
                    const char *labeled_form)
{
    parse_tree_t *parent = node
        ? core_tree_node_value(node).parent : nullptr;
    if (!nt_is(grammar, parent, labeled_form)) return (core_string_t){0};
    return first_token_text(child_nt(grammar, parent, "label"));
}





static void
append_module_symbol_name(core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol, core_buffer_t *buffer)
{
    core_string_t module = unit->module.identifier;
    if (symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("package-interface"))
        && symbol->scope->name.data)
        module = symbol->scope->name;
    core_buffer_printf(buffer, "%.*s.%.*s",
        (int)module.u8_bytes, module.data,
        (int)symbol->name.u8_bytes, symbol->name.data);
}


static th_type *
function_with_return_type(th_ctx *types, th_type *function,
                          th_type *return_type)
{
    if (!types || !function || th_kind_of(function) != TH_FUNCTION
        || !return_type) return nullptr;
    core_list_t(th_param) params = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nparams(function); i++)
        core_list_push(params, *th_func_param(function, i));
    for (size_t i = 0; i < th_func_nkw(function); i++)
        core_list_push(keywords, *th_func_kw(function, i));
    th_type *result = th_func_with_error(types, return_type,
        th_func_error(function), params.data, params.len,
        th_func_rest(function), keywords.data, keywords.len);
    core_list_free(params);
    core_list_free(keywords);
    return result;
}



static ir_lower_result_t
lower_error(ir_lower_context_t *context, ir_diagcode_t code, ir_span_t span,
            const char *message)
{
    if (context && context->diagnostics) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics, code,
                                              span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message);
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static parse_token_info_t *
last_tree_terminal(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token =
            last_tree_terminal(core_tree_child(node, i - 1));
        if (token) return token;
    }
    return nullptr;
}

static ir_span_t
lower_span_for_tree(ir_lower_context_t *context, parse_tree_t *node)
{
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_tree_terminal(node);
    if (!context || context->file_id < 0 || !first || !last
        || first->start_byte > UINT32_MAX || last->end_byte > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id, (uint32_t)first->start_byte,
                        (uint32_t)last->end_byte, first->line, first->column,
                        last->endline, last->endcol);
}

static ir_block_t *
n00bb_branch_block(ir_lower_context_t *context, n00bb_lower_context_t *lower,
                   const char *role)
{
    char name[64];
    snprintf(name, sizeof(name), "__n00bb_%s_%llu", role,
             (unsigned long long)lower->next_block_id++);
    return ir_block_add(context->function, name);
}

static ir_value_t *n00bb_emit_indirect_read(
    ir_lower_context_t *context, ir_value_t *address, th_type *type,
    ir_span_t span);
static ir_value_t *n00bb_materialize(
    ir_lower_context_t *context, parse_tree_t *node, ir_value_t *value,
    th_type *target, ir_span_t span);
static ir_value_t *n00bb_emit_wrapping(
    ir_lower_context_t *context, ir_op_t op, ir_value_t *payload,
    th_type *target, ir_span_t span);
static ir_value_t *n00bb_emit_atomic_wrap(
    ir_lower_context_t *context, ir_value_t *value, th_type *atomic_type,
    ir_span_t span);
static bool n00bb_emit_indirect_write(
    ir_lower_context_t *context, ir_value_t *address, ir_value_t *value,
    ir_span_t span);

bool
n00bb_atomic_native_support(ir_module_t *module, th_type *atomic_type,
                            const core_target_desc_t *target)
{
    bool handled = false;
    if (!module || !target || !n00bb_atomic_element_type(atomic_type)
        || !n00bb_abi_publish_direct_layout(module, atomic_type, target,
                                            &handled)
        || !handled
        || !ir_layout_compute(module, atomic_type, nullptr)) return false;
    ir_descriptor_t *layout = ir_layout_of(module, atomic_type, nullptr);
    return layout && ir_layout_is_settled(&layout->size)
        && ir_layout_is_settled(&layout->align)
        && layout->size.value > 0
        && layout->size.value <= UINT32_MAX / 8
        && core_target_atomic_for_bits(
            target, (uint32_t)(layout->size.value * 8));
}

static ir_value_t *
n00bb_atomic_binding_address(ir_lower_context_t *context,
                             parse_sym_entry_t *symbol, ir_span_t span)
{
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    if (address) return address;
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    th_type *value_type = local ? local->type
        : binding ? binding->type : nullptr;
    if (!value_type || (!local && (!binding
            || (binding->kind != IR_VAL_PARAM
                && binding->kind != IR_VAL_GLOBAL)))) return nullptr;
    th_type *address_type = th_ind(ir_module_types(context->module), 0,
                                   value_type);
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                           IR_OP_ADDRESS_OF, span);
    if (!address_type || !instruction
        || !ir_lower_set_temp_dest(context, instruction,
                                   "__n00bb_atomic_address_", address_type)
        || !(local ? ir_instr_operand_local(instruction, local)
                   : ir_instr_operand_value(instruction, binding)))
        return nullptr;
    return instruction->dest;
}

static bool
n00bb_lower_atomic_reference(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    if (!entering) return true;
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = lower
        ? attached_symbol(visit->grammar, lower->unit, visit->node) : nullptr;
    if (!symbol || (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM)
        || !n00bb_atomic_element_type(symbol->type)) return true;
    if (n00bb_reference_target(visit->grammar, visit->node)) return true;
    if (n00bb_assignment_target(visit->grammar, visit->node, nullptr))
        return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *address = n00bb_atomic_binding_address(context, symbol, span);
    ir_value_t *value = address
        ? n00bb_emit_indirect_read(context, address, symbol->type, span)
        : nullptr;
    th_type *contextual_type = core_tree_is_leaf(visit->node) ? nullptr
        : core_tree_node_value(visit->node).type;
    for (parse_tree_t *parent = core_tree_node_value(visit->node).parent;
         parent; parent = core_tree_node_value(parent).parent) {
        if (!nt_is(visit->grammar, parent, "expression")
            || core_tree_is_leaf(parent)) continue;
        th_type *parent_type = core_tree_node_value(parent).type;
        if (parent_type) {
            contextual_type = parent_type;
            break;
        }
    }
    if (value && n00bb_atomic_element_type(contextual_type))
        value = n00bb_emit_atomic_wrap(context, value, contextual_type, span);
    if (!value || !ir_lower_set_node_value(context, visit->node, value)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "atomic binding has no addressable storage for an atomic load");
        return false;
    }
    return true;
}

static ir_value_t *
n00bb_emit_local_read(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span)
{
    ir_value_t *address = ir_lower_local_address(context, local);
    if (address)
        return n00bb_emit_indirect_read(context, address, local->type, span);
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", local->type)
        || !ir_instr_operand_local(instruction, local)) return nullptr;
    return instruction->dest;
}

static bool
n00bb_emit_sequence_shape_check_impl(ir_lower_context_t *context,
                                     ir_value_t *value, ir_shape_t *expected,
                                     ir_span_t span, bool force)
{
    if (!expected || !value) return true;
    bool constrained = false;
    bool opaque = false;
    for (uint32_t i = 0; i < ir_shape_rank(expected); i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(expected, i, &extent)) return false;
        bool unknown_runtime = extent.kind == IR_SHAPE_EXTENT_RUNTIME
            && (!extent.value || extent.value->owner != context->function);
        opaque |= unknown_runtime;
        constrained |= extent.kind != IR_SHAPE_EXTENT_UNRESOLVED
            && !unknown_runtime;
    }
    if (!constrained) return true;
    if (opaque) {
        n00bb_lower_context_t *lower = context->user;
        ir_shape_t *checkable = lower && lower->ir_context
            ? ir_shape_new(lower->ir_context,
                ir_shape_element_type(expected), ir_shape_rank(expected))
            : nullptr;
        if (!checkable) return false;
        for (uint32_t i = 0; i < ir_shape_rank(expected); i++) {
            ir_shape_extent_t extent;
            if (!ir_shape_extent(expected, i, &extent)) return false;
            if (extent.kind == IR_SHAPE_EXTENT_RUNTIME
                && (!extent.value
                    || extent.value->owner != context->function))
                extent = (ir_shape_extent_t){
                    .kind = IR_SHAPE_EXTENT_UNRESOLVED,
                };
            if (!ir_shape_set_extent(checkable, i, extent)) return false;
        }
        expected = checkable;
    }
    ir_shape_t *actual = ir_value_shape(value);
    if (actual == expected && !force) return true;
    ir_instr_t *check = ir_lower_emit_instruction(context,
                                     IR_OP_SEQUENCE_SHAPE_CHECK, span);
    if (!check || !ir_instr_operand_value(check, value)
        || !ir_instr_set_shape_constraint(check, expected)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit a sequence shape check");
        return false;
    }
    return true;
}

static bool
n00bb_emit_sequence_shape_check(ir_lower_context_t *context,
                                ir_value_t *value, ir_shape_t *expected,
                                ir_span_t span)
{
    return n00bb_emit_sequence_shape_check_impl(context, value, expected,
                                                 span, false);
}

static bool
n00bb_attach_expression_shape(ir_lower_context_t *context,
                              parse_grammar_t *grammar,
                              parse_tree_t *expression, ir_value_t *value)
{
    if (!context || !value || ir_value_shape(value)) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_return_source_t *source = state
        ? n00bb_node_return_source(expression) : nullptr;
    parse_sym_entry_t *symbol = source
        && source->kind == N00BB_RETURN_SOURCE_SYMBOL
        ? source->symbol : attached_symbol(grammar,
            lower ? lower->unit : nullptr, expression);
    if (!symbol) return true;
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    ir_shape_t *shape = local ? ir_local_shape(local)
        : binding ? ir_value_shape(binding) : nullptr;
    if (!shape)
        shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    return !shape || ir_value_set_shape(value, shape);
}

static bool
n00bb_emit_local_write(ir_lower_context_t *context, ir_local_t *local,
                       ir_value_t *value, ir_span_t span)
{
    if (!context || !local || !value
        || !ir_type_eq(context->module, local->type, value->type))
        return false;
    if (!n00bb_emit_sequence_shape_check(context, value,
            ir_local_shape(local), span)) return false;
    ir_value_t *address = ir_lower_local_address(context, local);
    if (address)
        return n00bb_emit_indirect_write(context, address, value, span);
    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!copy || !ir_instr_set_binding_initialization(copy, true)
        || !ir_instr_set_dest_local(copy, local)
        || !ir_instr_operand_value(copy, value)) return false;
    if (type_may_contain_reference_alias(ir_module_types(context->module),
                                         local->type)) {
        n00bb_lower_context_t *lower = context->user;
        ir_value_t *binding = ir_local_value(context->function, local);
        ir_annots_t *annots = binding ? ir_value_annots(binding) : nullptr;
        ir_annot_t *count = annots
            ? ir_annot_get(annots, "ir.alias_count") : nullptr;
        ir_annot_t *complete = annots
            ? ir_annot_get(annots, "ir.alias_analysis_complete") : nullptr;
        ir_alias_target_t source = {.value = value};
        bool immutable = binding
            && !ir_value_can_write(binding,
                ir_func_execution_phase(context->function));
        bool already_complete = immutable && count
            && count->kind == IR_ANNOT_INT && !count->i.neg
            && count->i.mag && complete
            && complete->kind == IR_ANNOT_BOOL && complete->b;
        bool first_initialization = immutable
            && (!count || (count->kind == IR_ANNOT_INT
                && !count->i.neg && !count->i.mag));
        bool attached = lower && binding && (already_complete ? true
            : first_initialization
            ? ir_value_set_alias_targets(lower->ir_context, binding,
                &source, 1, true)
            : ir_value_add_alias_target(lower->ir_context, binding,
                source));
        if (attached && immutable && !already_complete
            && !first_initialization)
            attached = ir_value_complete_alias_targets(lower->ir_context,
                                                        binding);
        if (!attached) return false;
    }
    return true;
}

static ir_value_t *
n00bb_emit_indirect_read(ir_lower_context_t *context, ir_value_t *address,
                        th_type *type, ir_span_t span)
{
    if (!address || !type) return nullptr;
    th_type *atomic_element = n00bb_atomic_element_type(type);
    th_type *result_type = atomic_element ? atomic_element : type;
    if (!result_type) return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_LOAD,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        || !ir_instr_operand_value(instruction, address)
        || (atomic_element
            && !ir_instr_add_mod(instruction, "atomic"))) return nullptr;
    if (th_kind_of(th_resolve(result_type)) == TH_INDIRECTION) {
        n00bb_lower_context_t *lower = context->user;
        if (!lower || !ir_value_set_indirect_access(lower->ir_context,
                instruction->dest, IR_INDIRECT_ACCESS_READ_WRITE))
            return nullptr;
    }
    return instruction->dest;
}

static ir_value_t *
n00bb_emit_atomic_wrap(ir_lower_context_t *context, ir_value_t *value,
                       th_type *atomic_type, ir_span_t span)
{
    if (!context || !value || !atomic_type) return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                                        IR_OP_ATOMIC_WRAP,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", atomic_type)
        || !ir_instr_operand_value(instruction, value)) return nullptr;
    return instruction->dest;
}

static bool
n00bb_emit_indirect_write(ir_lower_context_t *context, ir_value_t *address,
                          ir_value_t *value, ir_span_t span)
{
    ir_instr_t *instruction = address && value
        ? ir_lower_emit_instruction(context, IR_OP_STORE, span) : nullptr;
    th_type *referent = address ? th_ind_referent(address->type) : nullptr;
    if (!instruction || !ir_instr_set_binding_initialization(instruction, true)
        || !ir_instr_operand_value(instruction, address)
        || !ir_instr_operand_value(instruction, value)
        || (n00bb_atomic_element_type(referent)
            && !ir_instr_add_mod(instruction, "atomic"))) return false;
    return true;
}

static ir_instr_t *
n00bb_emit_success_return(ir_lower_context_t *context, ir_value_t *result,
                          const core_list_t(n00bb_copyout_t) *copyouts,
                          ir_span_t span)
{
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    for (size_t i = 0; i < copyouts->len; i++) {
        ir_value_t *value = n00bb_emit_local_read(context,
            copyouts->data[i].local, span);
        if (!value) {
            core_list_free(values);
            return nullptr;
        }
        core_list_push(values, value);
    }

    ir_instr_t *ret = ir_lower_emit_terminator(context, IR_OP_RETURN, span);
    bool emitted = ret && (!result || ir_instr_operand_value(ret, result));
    for (size_t i = 0; emitted && i < values.len; i++)
        emitted = ir_instr_operand_value(ret, values.data[i]);
    core_list_free(values);
    if (!emitted) return nullptr;
    return ret;
}

static th_type *
n00bb_indirect_type(th_ctx *types, th_type *referent)
{
    return th_ind(types, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE,
                  referent);
}

static bool
n00bb_set_reference_access(ir_ctx_t *context, ir_value_t *value,
                           th_type *type)
{
    th_ctx *types = value && value->owner && value->owner->owner
        ? ir_module_types(value->owner->owner) : nullptr;
    bool reference = type && (th_kind_of(th_resolve(type)) == TH_INDIRECTION
        || n00bb_reference_definition(types, type));
    return value && type
        && (!reference
            || ir_value_set_indirect_access(context, value,
                IR_INDIRECT_ACCESS_READ_WRITE));
}

static bool
n00bb_binding_in_loop_scope(parse_sym_entry_t *symbol,
                            parse_tree_t *loop)
{
    for (parse_scope_t *scope = symbol ? symbol->scope : nullptr;
         scope; scope = scope->parent)
        if (scope->node == loop) return true;
    return false;
}

static bool
n00bb_mark_binding_reentry(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!context || !lower || !symbol || !symbol->scope) return false;

    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || !frame->node || !frame->header) continue;
        if (!n00bb_binding_in_loop_scope(symbol, frame->node)) continue;

        ir_value_t *binding = ir_lower_bound_address(context, symbol);
        if (!binding) {
            ir_local_t *local = ir_lower_bound_local(context, symbol);
            binding = local ? ir_local_value(context->function, local)
                            : nullptr;
        }
        return binding && ir_value_set_binding_reentry(binding,
                                                       frame->header);
    }
    return true;
}

static bool
n00bb_bind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                 ir_local_t *local, ir_span_t span)
{
    if (!context || !symbol || !local) return false;
    core_string_t storage_class = symbol->selected_storage_class;
    bool dynamic = symbol->storage_duration == PARSE_SYM_STORAGE_DYNAMIC;
    if ((!storage_class.data || !storage_class.u8_bytes) && !dynamic) {
        return ir_lower_bind_local(context, symbol, local)
            && n00bb_mark_binding_reentry(context, symbol);
    }
    parse_sym_set_storage_duration(symbol, PARSE_SYM_STORAGE_DYNAMIC);
    th_type *address_type = th_ind(ir_module_types(context->module), 0,
                                   local->type);
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    return allocation
        && ir_lower_set_temp_dest(context, allocation, "__n00bb_storage_",
                                  address_type)
        && ir_instr_operand_type(allocation, local->type)
        && ((!storage_class.data || !storage_class.u8_bytes)
            || ir_instr_set_storage_class(allocation, storage_class.data,
                                          symbol->storage_class_required))
        && ir_lower_bind_local_storage(context, symbol, local,
                                       allocation->dest)
        && n00bb_mark_binding_reentry(context, symbol);
}

static ir_value_t *
n00bb_emit_binary_ir_with(ir_lower_context_t *context, ir_op_t op,
                          bool wrapping, th_type *result_type,
                          ir_value_t *left, ir_value_t *right, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        || !ir_instr_operand_value(instruction, left)
        || !ir_instr_operand_value(instruction, right)) return nullptr;
    ir_class_t result_class = ir_machine_of(context->module, result_type).class;
    ir_class_t operand_class = ir_machine_of(context->module, left->type).class;
    bool integer_operation = result_class == IR_CLASS_SINT
        || result_class == IR_CLASS_UINT || operand_class == IR_CLASS_SINT
        || operand_class == IR_CLASS_UINT;
    if (wrapping)
        return integer_operation && (!ir_op_accepts_mod(op, "wrap")
                || ir_instr_add_mod(instruction, "wrap"))
            ? instruction->dest : nullptr;
    if (integer_operation
        && ir_op_accepts_mod(op, "trap")) {
        if (!ir_instr_add_mod(instruction, "trap")) return nullptr;
        if ((op == IR_OP_DIV || op == IR_OP_REM)
            && (result_class == IR_CLASS_SINT
                || operand_class == IR_CLASS_SINT)
            && !ir_instr_add_mod(instruction, "trap")) return nullptr;
    }
    if (operand_class == IR_CLASS_FLOAT
        && (op == IR_OP_EQ || op == IR_OP_NE || op == IR_OP_LT
            || op == IR_OP_LE || op == IR_OP_GT || op == IR_OP_GE)
        && !ir_instr_add_mod(instruction, "ordered")) return nullptr;
    return instruction->dest;
}

static ir_value_t *
n00bb_emit_binary_ir(ir_lower_context_t *context, ir_op_t op,
                     th_type *result_type, ir_value_t *left,
                     ir_value_t *right, ir_span_t span)
{
    return n00bb_emit_binary_ir_with(context, op, false, result_type, left,
                                     right, span);
}

static th_type *
n00bb_ir_builtin_type(ir_module_t *module, const char *name)
{
    th_ctx *types = module ? ir_module_types(module) : nullptr;
    th_type *type = types && name ? th_name(types, name) : nullptr;
    return type ? ir_type_intern(module, type) : nullptr;
}

static bool
collect_elif_arms(parse_grammar_t *grammar, parse_annot_result_t *annotations,
                  parse_tree_t *node, const char *clause_kind,
                  core_list_t(n00bb_branch_arm_t) *arms,
                  parse_tree_t **else_body, bool *has_else_clause)
{
    if (!node || core_tree_is_leaf(node)) return true;
    const char *else_kind = !strcmp(clause_kind, "value-elif-clause")
        ? "value-else-clause" : "else-clause";
    if (nt_is(grammar, node, else_kind)) {
        *has_else_clause = true;
        *else_body = child_nt(grammar, node,
            !strcmp(else_kind, "value-else-clause")
                ? "value-body" : "body");
        return true;
    }
    if (nt_is(grammar, node, clause_kind)) {
        parse_cf_label_t *label = parse_cf_label_lookup(annotations, node);
        if (!label || label->kind != PARSE_CF_BRANCH
            || !label->cond || !label->then_body)
            return false;
        core_list_push(*arms, ((n00bb_branch_arm_t){
            .condition = label->cond,
            .body = label->then_body,
        }));
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!collect_elif_arms(grammar, annotations,
                               core_tree_child(node, i), clause_kind, arms,
                               else_body, has_else_clause))
            return false;
    return true;
}

static bool
n00bb_lower_branch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (lower->branches.len
            && lower->branches.data[lower->branches.len - 1].node == visit->node) {
            n00bb_branch_frame_t frame =
                lower->branches.data[--lower->branches.len];
            if (frame.active && !frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) return false;
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.active) {
                if (frame.expression && frame.join_reachable) {
                    ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                    if (!read || !ir_lower_set_temp_dest(context, read,
                        "__n00bb_if_value_", frame.result_local->type)
                        || !ir_instr_operand_local(read, frame.result_local)
                        || !ir_lower_set_node_value(context, frame.node,
                                                    read->dest)) {
                        core_list_free(frame.arms);
                        lower_error(context, IR_DIAG_INTERNAL,
                            lower_span_for_tree(context, frame.node),
                            "could not read the value from the if-expression join");
                        return false;
                    }
                    read->span = lower_span_for_tree(context, frame.node);
                }
                context->block = frame.join;
                context->reachable = frame.join_reachable;
            }
            core_list_free(frame.arms);
        }
        return true;
    }

    if (nt_is(visit->grammar, visit->node, "elif-clause")
        || nt_is(visit->grammar, visit->node, "value-elif-clause")) {
        for (parse_tree_t *parent = core_tree_node_value(visit->node).parent;
             parent; parent = core_tree_node_value(parent).parent) {
            if (nt_is(visit->grammar, visit->node, "elif-clause")
                && nt_is(visit->grammar, parent, "if-stmt")) return true;
            if (nt_is(visit->grammar, visit->node, "value-elif-clause")
                && nt_is(visit->grammar, parent, "if-value-expr")) return true;
        }
    }
    bool short_circuit = nt_is(visit->grammar, visit->node, "and-expr")
        || nt_is(visit->grammar, visit->node, "or-expr");
    bool expression_form = short_circuit
        || nt_is(visit->grammar, visit->node, "if-value-expr");
    if (!expression_form
        && !nt_is(visit->grammar, visit->node, "if-stmt")) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this branch form is not lowered yet");
        return false;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_BRANCH || !label->cond
        || !label->then_body)
        return false;
    core_list_t(n00bb_branch_arm_t) arms =
        core_list_new(n00bb_branch_arm_t);
    parse_tree_t *else_body = nullptr;
    bool has_else_clause = false;
    core_list_push(arms, ((n00bb_branch_arm_t){
        .condition = label->cond,
        .body = label->then_body,
    }));
    if (!collect_elif_arms(visit->grammar, tu->annotations,
                           label->else_body,
                           expression_form ? "value-elif-clause"
                                           : "elif-clause",
                           &arms, &else_body, &has_else_clause)) {
        core_list_free(arms);
        return false;
    }
    if (has_else_clause && !else_body) {
        core_list_free(arms);
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this branch form is not lowered yet");
        return false;
    }
    if (expression_form && !short_circuit && !else_body) {
        core_list_free(arms);
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "a value if-expression requires an else body");
        return false;
    }
    if (!context->reachable) {
        core_list_push(lower->branches, ((n00bb_branch_frame_t){
            .node = visit->node,
            .arms = arms,
            .else_body = else_body,
            .expression = expression_form,
            .short_circuit = short_circuit,
            .active = false,
        }));
        return true;
    }
    ir_block_t *join = n00bb_branch_block(context, lower, "if_join");
    ir_block_t *else_block = else_body
        ? n00bb_branch_block(context, lower, "if_else") : nullptr;
    th_type *result_type = expression_form
        ? core_tree_node_value(visit->node).type : nullptr;
    ir_local_t *result_local = nullptr;
    if (expression_form && result_type) {
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_if_result_%llu",
                 (unsigned long long)context->next_value_id++);
        result_local = ir_local_add(context->function, name, result_type);
    }
    bool blocks_ready = join && (!else_body || else_block)
        && (!expression_form || result_local);
    for (size_t i = 0; blocks_ready && i < arms.len; i++) {
        arms.data[i].body_block = n00bb_branch_block(context, lower,
                                                     "if_then");
        if (i) arms.data[i].test_block = n00bb_branch_block(
            context, lower, "if_test");
        blocks_ready = arms.data[i].body_block
            && (!i || arms.data[i].test_block);
    }
    if (!blocks_ready) {
        core_list_free(arms);
        return false;
    }
    core_list_push(lower->branches, ((n00bb_branch_frame_t){
        .node = visit->node,
        .arms = arms,
        .else_body = else_body,
        .join = join,
        .else_block = else_block,
        .result_local = result_local,
        .expression = expression_form,
        .short_circuit = short_circuit,
        .join_reachable = !else_body,
        .active = true,
    }));
    return true;
}

static bool
n00bb_lower_range_body_enter(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             n00bb_loop_frame_t *frame)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    ir_value_t *start = ir_lower_node_value(context, frame->range_start);
    ir_value_t *end = ir_lower_node_value(context, frame->range_end);
    ir_value_t *step = frame->range_step
        ? ir_lower_node_value(context, frame->range_step) : nullptr;
    th_type *iterator_type = frame->iterator ? frame->iterator->type : nullptr;
    th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
    if (!frame->range_step && iterator_type) {
        ir_instr_t *one = ir_lower_emit_instruction(context, IR_OP_CONST, span);
        if (one && ir_lower_set_temp_dest(context, one, "__n00bb_",
                                           iterator_type)
            && ir_instr_operand_int_lit(one, ir_lit_of(1))) {
            step = one->dest;
        }
    }
    if (!start || !end || !step || !iterator_type || !bool_type
        || !is_integer_type(iterator_type)
        || !ir_type_eq(context->module, start->type, iterator_type)
        || !ir_type_eq(context->module, end->type, iterator_type)
        || !ir_type_eq(context->module, step->type, iterator_type)) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "range loop endpoints and step must have one integer type");
        return false;
    }
    int64_t step_minimum = 0, step_maximum = 0;
    if (frame->range_step
        && parse_tree_integer_range(frame->range_step, &step_minimum,
                                    &step_maximum)
        && step_minimum == 0 && step_maximum == 0) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, frame->range_step),
                    "range step must not be zero");
        return false;
    }
    frame->range_step_value = step;
    ir_instr_t *initialize = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!initialize || !ir_instr_set_dest_local(initialize, frame->iterator)
        || !ir_instr_operand_value(initialize, start)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize range loop variable");
        return false;
    }
    ir_machine_t step_machine = ir_machine_of(context->module, step->type);
    ir_instr_t *zero = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    bool zero_ok = zero && ir_lower_set_temp_dest(context, zero, "__n00bb_",
                                                 iterator_type)
        && ir_instr_operand_int_lit(zero, ir_lit_of(0));
    ir_value_t *is_zero = zero_ok
        ? n00bb_emit_binary_ir(context, IR_OP_EQ, bool_type, step, zero->dest,
                               span) : nullptr;
    ir_block_t *zero_step = is_zero
        ? n00bb_branch_block(context, lower, "for_zero_step") : nullptr;
    ir_instr_t *step_branch = zero_step
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    ir_instr_t *trap = zero_step
        ? ir_instr_add(zero_step, IR_OP_TRAP) : nullptr;
    if (!step_branch || !ir_instr_operand_value(step_branch, is_zero)
        || !ir_instr_operand_block(step_branch, zero_step)
        || !ir_instr_operand_block(step_branch, frame->header) || !trap) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not guard the range step and enter its header");
        return false;
    }
    step_branch->span = span;
    trap->span = span;
    frame->range_step_value = step;
    context->block = frame->header;

    ir_value_t *current = n00bb_emit_local_read(context, frame->iterator, span);
    ir_value_t *ascending = current
        ? n00bb_emit_binary_ir(context,
            frame->range_inclusive ? IR_OP_LE : IR_OP_LT,
            bool_type, current, end, span) : nullptr;
    ir_value_t *descending = step_machine.class == IR_CLASS_SINT && current
        ? n00bb_emit_binary_ir(context,
            frame->range_inclusive ? IR_OP_GE : IR_OP_GT,
            bool_type, current, end, span) : nullptr;
    ir_value_t *negative = step_machine.class == IR_CLASS_SINT && zero_ok
        ? n00bb_emit_binary_ir(context, IR_OP_LT, bool_type, step, zero->dest,
                               span) : nullptr;
    ir_value_t *condition = ascending;
    if (step_machine.class == IR_CLASS_SINT) {
        ir_instr_t *selected = ir_lower_emit_instruction(context, IR_OP_SELECT,
                                                         span);
        if (selected && ir_lower_set_temp_dest(context, selected, "__n00bb_",
                                               bool_type)
            && ir_instr_operand_value(selected, negative)
            && ir_instr_operand_value(selected, descending)
            && ir_instr_operand_value(selected, ascending)) {
            condition = selected->dest;
        }
        else condition = nullptr;
    }
    ir_instr_t *branch = condition
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, frame->body_block)
        || !ir_instr_operand_block(branch, frame->exit)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop condition");
        return false;
    }
    branch->span = span;

    context->block = frame->body_block;
    frame->binding_checkpoint = ir_lower_bindings_checkpoint(context);
    frame->binding_scope_active = true;
    if (!frame->range_binding || !frame->iterator_symbol
        || !ir_lower_bind_scoped_value(context, frame->iterator_symbol,
                                       current, frame->range_binding)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize the range binding");
        return false;
    }

    context->block = frame->latch;
    current = n00bb_emit_local_read(context, frame->iterator, span);
    if (!current) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not read range loop variable at its latch");
        return false;
    }
    if (frame->range_inclusive) {
        ir_value_t *at_end = n00bb_emit_binary_ir(
            context, IR_OP_EQ, bool_type, current, end, span);
        ir_instr_t *last = at_end
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        if (!last || !ir_instr_operand_value(last, at_end)
            || !ir_instr_operand_block(last, frame->exit)
            || !ir_instr_operand_block(last, frame->increment)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not emit inclusive range loop latch");
            return false;
        }
        last->span = span;
        context->block = frame->increment;
    }
    ir_value_t *incremented = n00bb_emit_binary_ir(
        context, IR_OP_ADD, frame->iterator->type, current, step, span);
    ir_instr_t *store = incremented
        ? ir_lower_emit_instruction(context, IR_OP_COPY, span) : nullptr;
    ir_instr_t *backedge = store
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!store || !ir_instr_set_dest_local(store, frame->iterator)
        || !ir_instr_operand_value(store, incremented) || !backedge
        || !ir_instr_operand_block(backedge, frame->header)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit range loop increment");
        return false;
    }
    store->span = span;
    backedge->span = span;
    context->block = frame->body_block;
    context->reachable = true;
    return true;
}

static bool
n00bb_lower_container_body_enter(ir_lower_context_t *context,
                                 n00bb_lower_context_t *lower,
                                 n00bb_loop_frame_t *frame)
{
    ir_span_t span = lower_span_for_tree(context, frame->node);
    ir_value_t *source = frame->iteration_source
        ? ir_lower_node_value(context, frame->iteration_source) : nullptr;
    if (!source || !frame->iteration_plan) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "container loop has incomplete iteration semantics");
        return false;
    }

    ir_value_t *cursor = n00bb_emit_iteration_trait_call(context, frame,
        frame->iteration_plan->iter_site, source, TH_IN,
        frame->iteration_plan->cursor_type, nullptr, frame->node);
    if (!cursor) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not initialize the iteration cursor");
        return false;
    }
    ir_block_t *entry_block = context->block;
    frame->cleanup_active = true;
    ir_instr_t *enter = ir_instr_add(context->block, IR_OP_JUMP);
    if (!enter || !ir_instr_operand_block(enter, frame->header)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not enter the iteration header");
        return false;
    }
    enter->span = span;

    context->block = frame->header;
    context->reachable = true;
    frame->iteration_cursor_phi = ir_lower_emit_instruction(context, IR_OP_PHI,
                                                            span);
    ir_value_t *current = frame->iteration_cursor_phi
        && ir_lower_set_temp_dest(context, frame->iteration_cursor_phi,
                                  "__n00bb_cursor_",
                                  frame->iteration_plan->cursor_type)
        && ir_instr_operand_phi_arm(frame->iteration_cursor_phi, entry_block,
                                    cursor)
            ? frame->iteration_cursor_phi->dest : nullptr;
    if (!current) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not create the iteration cursor phi");
        return false;
    }
    frame->iteration_cursor_value = current;
    th_type *option_type = th_option(ir_module_types(context->module),
                                     frame->iteration_plan->item_type);
    ir_value_t *option = current && option_type
        ? n00bb_emit_iteration_trait_call(context, frame,
            frame->iteration_plan->next_site, current, TH_INOUT,
            option_type, &frame->iteration_cursor_value, frame->node)
        : nullptr;
    th_type *bool_type = th_name(ir_module_types(context->module), "bool");
    ir_block_t *cleanup = n00bb_branch_block(context, lower,
                                              "for_cleanup");
    ir_instr_t *test = option && bool_type && cleanup
        ? ir_lower_emit_instruction(context, IR_OP_OPTION_IS_NIL, span)
        : nullptr;
    ir_instr_t *branch = test ? ir_instr_add(context->block, IR_OP_BRANCH)
                              : nullptr;
    bool emitted = test
        && ir_lower_set_temp_dest(context, test, "__n00bb_iteration_done_",
                                  bool_type)
        && ir_instr_operand_value(test, option)
        && branch && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, cleanup)
        && ir_instr_operand_block(branch, frame->body_block);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not test the next iteration item");
        return false;
    }
    branch->span = span;

    context->block = cleanup;
    context->reachable = true;
    if (!n00bb_emit_iteration_cleanup(context, frame, frame->node))
        return false;
    ir_instr_t *done = ir_instr_add(context->block, IR_OP_JUMP);
    if (!done || !ir_instr_operand_block(done, frame->exit)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not leave an exhausted iteration");
        return false;
    }
    done->span = span;

    context->block = frame->body_block;
    context->reachable = true;
    ir_instr_t *get = ir_lower_emit_instruction(context, IR_OP_OPTION_GET,
                                                span);
    if (!get || !ir_lower_set_temp_dest(context, get,
                                        "__n00bb_iteration_item_",
                                        frame->iteration_plan->item_type)
        || !ir_instr_operand_value(get, option)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not extract the next iteration item");
        return false;
    }
    frame->binding_checkpoint = ir_lower_bindings_checkpoint(context);
    frame->binding_scope_active = true;
    for (size_t i = 0; i < frame->iteration_bindings.len; i++) {
        parse_sym_entry_t *binding = frame->iteration_bindings.data[i];
        ir_local_t *local = ir_lower_bound_local(context, binding);
        ir_value_t *value = get->dest;
        n00bb_loop_binding_projection_t *projection = parse_sym_binding(
            binding, CORE_STRING_STATIC("n00bb.loop-binding-projection"));
        if (projection) {
            ir_instr_t *extract = ir_lower_emit_instruction(context,
                projection->positional ? IR_OP_EXTRACT_INDEX : IR_OP_EXTRACT,
                span);
            if (!extract || !ir_lower_set_temp_dest(context, extract,
                                                    "__n00bb_iteration_field_",
                                                    binding->type)
                || !ir_instr_operand_value(extract, get->dest)
                || (projection->positional
                    ? !ir_instr_operand_uint(extract, projection->index)
                    : !ir_instr_operand_name(extract,
                                              projection->name))) {
                lower_error(context, IR_DIAG_INTERNAL, span,
                            "could not destructure the iteration item");
                return false;
            }
            value = extract->dest;
        }
        if (!local || !ir_lower_bind_scoped_value(context, binding, value,
                                                   local)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not bind the next iteration item");
            return false;
        }
    }
    return true;
}

static bool
n00bb_emit_jump(ir_lower_context_t *context, ir_block_t *target,
                ir_span_t span)
{
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !target || !ir_instr_operand_block(jump, target))
        return false;
    jump->span = span;
    return true;
}

static bool
n00bb_store_try_value(ir_lower_context_t *context,
                      n00bb_try_frame_t *frame, parse_grammar_t *grammar,
                      parse_tree_t *body)
{
    parse_tree_t *expression = value_body_expression(grammar, body);
    ir_value_t *value = expression
        ? ir_lower_node_value(context, expression) : nullptr;
    ir_instr_t *store = value
        ? ir_lower_emit_instruction(context, IR_OP_COPY,
                                    lower_span_for_tree(context, body))
        : nullptr;
    if (!store || !frame->result_value
        || !ir_instr_set_dest_local(store, frame->result_value)
        || !ir_instr_operand_value(store, value)) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, body),
            "try expression body does not produce a lowered value");
        return false;
    }
    return true;
}

static int
n00bb_route_try_error(ir_lower_context_t *context, parse_tree_t *source,
                      ir_value_t *error, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *frame = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && node_is_within(source, candidate->body)) {
            frame = candidate;
            break;
        }
    }
    if (!frame) return 0;

    parse_sym_entry_t *symbol = frame->error_symbol;
    th_ctx *types = tu_state(lower->unit)->types;
    if (!error || !symbol || !types) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "try error handler has no bound error value");
        return -1;
    }
    th_type *bound_type = symbol->type
        ? th_unify_in_place(types, symbol->type, error->type) : error->type;
    if (!bound_type) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "failure value does not match the try handler error type");
        return -1;
    }
    symbol->type = bound_type;
    if (!frame->error_value) {
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_try_error_%llu",
                 (unsigned long long)context->next_value_id++);
        frame->error_value = ir_local_add(context->function, name,
                                          bound_type);
        if (!frame->error_value) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not allocate the try handler error value");
            return -1;
        }
    }
    if (!ir_type_eq(context->module, frame->error_value->type, bound_type)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "failure paths reaching this handler have incompatible types");
        return -1;
    }
    ir_instr_t *store = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!store || !ir_instr_set_dest_local(store, frame->error_value)
        || !ir_instr_operand_value(store, error)
        || !n00bb_emit_jump(context, frame->handler, span)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not route failure to the try handler");
        return -1;
    }
    frame->error_reachable = true;
    context->reachable = false;
    return 1;
}

static bool
n00bb_lower_try_scope(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return true;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);

    bool try_statement = core_string_eq(name,
                                        CORE_STRING_STATIC("try-stmt"));
    bool try_expression = core_string_eq(name,
                                        CORE_STRING_STATIC("try-value-expr"));
    if (try_statement || try_expression) {
        if (entering) {
            n00bb_try_frame_t frame = {
                .node = visit->node,
                .body = child_nt(visit->grammar, visit->node,
                    try_expression ? "value-body" : "try-body"),
                .error_clause = child_nt(visit->grammar, visit->node,
                    try_expression ? "try-value-error-clause"
                                   : "try-error-clause"),
                .error_symbol = nullptr,
                .expression = try_expression,
                .active = context->reachable,
            };
            frame.error_symbol = n00bb_try_clause_symbol(visit->grammar,
                                                         frame.error_clause);
            if (frame.active) {
                frame.handler = n00bb_branch_block(context, lower,
                                                   "try_handler");
                frame.join = n00bb_branch_block(context, lower, "try_join");
                if (!frame.handler || !frame.join) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, visit->node),
                        "could not create try control-flow blocks");
                    return false;
                }
                if (frame.expression) {
                    th_type *result_type =
                        core_tree_node_value(visit->node).type;
                    char local_name[48];
                    snprintf(local_name, sizeof(local_name),
                             "__n00bb_try_value_%llu",
                             (unsigned long long)context->next_value_id++);
                    frame.result_value = result_type
                        ? ir_local_add(context->function, local_name,
                                       result_type) : nullptr;
                    if (!frame.result_value) {
                        lower_error(context, IR_DIAG_VERIFY_TYPED,
                            lower_span_for_tree(context, visit->node),
                            "try expression has no inferred result type");
                        return false;
                    }
                }
            }
            core_list_push(lower->tries, frame);
            return true;
        }
        if (!lower->tries.len
            || lower->tries.data[lower->tries.len - 1].node != visit->node)
            return false;
        n00bb_try_frame_t frame = lower->tries.data[--lower->tries.len];
        if (!frame.active) return true;
        if (frame.expression && frame.join_reachable) {
            ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
            char result_name[48];
            snprintf(result_name, sizeof(result_name),
                     "__n00bb_try_result_%llu",
                     (unsigned long long)context->next_value_id++);
            if (!read || !ir_instr_set_dest(read, result_name,
                                            frame.result_value->type)
                || !ir_instr_operand_local(read, frame.result_value)
                || !ir_lower_set_node_value(context, frame.node, read->dest)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame.node),
                    "could not read the value from the try-expression join");
                return false;
            }
            read->span = lower_span_for_tree(context, frame.node);
        }
        if (!frame.join_reachable && context->block == frame.join) {
            context->block = frame.join;
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable)
                return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
            context->reachable = false;
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("block"))) {
        if (entering || !lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->body != visit->node || !frame->active) return true;
        if (context->reachable) {
            if (frame->expression
                && !n00bb_store_try_value(context, frame, visit->grammar,
                                          visit->node))
                return false;
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        context->block = frame->handler;
        context->reachable = frame->error_reachable;
        if (!frame->error_reachable) {
            ir_instr_t *unreachable = ir_instr_add(context->block,
                                                   IR_OP_UNREACHABLE);
            if (!unreachable) return false;
            unreachable->span = lower_span_for_tree(context, visit->node);
        }
        return true;
    }

    if (core_string_eq(visit->annot->scope_tag,
                       CORE_STRING_STATIC("try-error"))
        && (core_string_eq(name, CORE_STRING_STATIC("try-error-clause"))
            || core_string_eq(name,
                              CORE_STRING_STATIC("try-value-error-clause")))) {
        if (!lower->tries.len) return true;
        n00bb_try_frame_t *frame = &lower->tries.data[lower->tries.len - 1];
        if (frame->error_clause != visit->node || !frame->active)
            return true;
        if (entering && context->reachable) {
            parse_sym_entry_t *symbol = frame->error_symbol;
            if (!frame->error_value || !symbol
                || !ir_lower_bind_local(context, symbol,
                                        frame->error_value)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not bind the try handler error value");
                return false;
            }
            return true;
        }
        if (entering) return true;
        if (context->reachable) {
            if (frame->expression) {
                parse_tree_t *body = child_nt(visit->grammar, visit->node,
                                               "value-body");
                if (!n00bb_store_try_value(context, frame, visit->grammar,
                                           body))
                    return false;
            }
            if (!n00bb_emit_jump(context, frame->join,
                                 lower_span_for_tree(context, visit->node)))
                return false;
            frame->join_reachable = true;
        }
        context->block = frame->join;
        context->reachable = frame->join_reachable;
        return true;
    }
    return true;
}

static ir_value_t *
n00bb_switch_integer_scrutinee(ir_lower_context_t *context,
                               n00bb_lower_context_t *lower,
                               ir_value_t *scrutinee, ir_span_t span);

static bool
n00bb_lower_branch_body(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    bool logical_rhs = nt_is(visit->grammar, visit->node, "and-rhs")
        || nt_is(visit->grammar, visit->node, "or-rhs");
    if (!lower || (!logical_rhs
        && (!visit->annot || !core_string_eq(visit->annot->scope_tag,
                                             CORE_STRING_STATIC("block")))))
        return true;
    if (nt_is(visit->grammar, visit->node, "block-value-expr")) {
        if (entering) {
            n00bb_value_block_frame_t frame = {
                .node = visit->node,
                .active = context->reachable,
            };
            if (frame.active) {
                th_type *result_type = core_tree_node_value(visit->node).type;
                char local_name[48];
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_block_value_%llu",
                         (unsigned long long)context->next_value_id++);
                frame.result = result_type
                    ? ir_local_add(context->function, local_name, result_type)
                    : nullptr;
                frame.continuation = n00bb_branch_block(
                    context, lower, "block_value_cont");
                if (!frame.result || !frame.continuation) {
                    lower_error(context, IR_DIAG_VERIFY_TYPED,
                        lower_span_for_tree(context, visit->node),
                        "block value has no result type or continuation");
                    return false;
                }
            }
            core_list_push(lower->value_blocks, frame);
            return true;
        }
        if (!lower->value_blocks.len
            || lower->value_blocks.data[lower->value_blocks.len - 1].node
                != visit->node) return false;
        n00bb_value_block_frame_t frame = lower->value_blocks.data[
            --lower->value_blocks.len];
        if (!frame.active) return true;
        if (!frame.value_written || !context->reachable) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "value block has no reachable value statement");
            return false;
        }
        ir_instr_t *read = ir_lower_emit_instruction(context, IR_OP_COPY,
            lower_span_for_tree(context, visit->node));
        if (!read || !ir_lower_set_temp_dest(context, read,
                                             "__n00bb_block_result_",
                                             frame.result->type)
            || !ir_instr_operand_local(read, frame.result)
            || !ir_lower_set_node_value(context, visit->node, read->dest)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not read the value block result");
            return false;
        }
        return true;
    }
    for (size_t i = lower->branches.len; i > 0; i--) {
        n00bb_branch_frame_t *frame = &lower->branches.data[i - 1];
        if (!frame->active) continue;
        bool else_body = frame->else_body == visit->node;
        size_t arm_index = frame->arms.len;
        for (size_t j = 0; j < frame->arms.len; j++)
            if (frame->arms.data[j].body == visit->node) {
                arm_index = j;
                break;
            }
        if (arm_index == frame->arms.len && !else_body) continue;
        if (entering) {
            if (else_body) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_branch_arm_t *arm = &frame->arms.data[arm_index];
            ir_value_t *condition =
                ir_lower_node_value(context, arm->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
            if (!condition || !bool_type
                || !ir_type_eq(context->module, condition->type, bool_type)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, frame->node),
                    "if condition must lower to bool");
                return false;
            }
            ir_block_t *false_block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_block ? frame->else_block : frame->join;
            if (frame->short_circuit) {
                ir_instr_t *store = ir_lower_emit_instruction(context,
                    IR_OP_COPY, lower_span_for_tree(context, frame->node));
                if (!store || !ir_instr_set_dest_local(store,
                        frame->result_local)
                    || !ir_instr_operand_value(store, condition)) return false;
            }
            bool shortcut_true = frame->short_circuit
                && nt_is(visit->grammar, frame->node, "or-expr");
            ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
            bool emitted = branch
                && ir_instr_operand_value(branch, condition)
                && ir_instr_operand_block(branch,
                    shortcut_true ? false_block : arm->body_block)
                && ir_instr_operand_block(branch,
                    shortcut_true ? arm->body_block : false_block);
            if (!emitted) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit conditional branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            context->block = arm->body_block;
            context->reachable = true;
        }
        else if (arm_index < frame->arms.len) {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = frame->short_circuit
                    ? core_tree_child(visit->node,
                        core_tree_num_children(visit->node) - 1)
                    : value_body_expression(
                        visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                if (frame->short_circuit && value
                    && !ir_type_eq(context->module, value->type,
                                   frame->result_local->type)) {
                    lower_error(context, IR_DIAG_TYPE_MISMATCH,
                        lower_span_for_tree(context, visit->node),
                        "logical operands must be bool");
                    return false;
                }
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "value if arm does not produce a lowered value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = arm_index + 1 < frame->arms.len
                ? frame->arms.data[arm_index + 1].test_block
                : frame->else_body ? frame->else_block : frame->join;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = value_body_expression(
                    visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "value if else arm does not produce a lowered value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit branch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->block = frame->join;
            context->reachable = frame->join_reachable;
        }
        return true;
    }
    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || frame->body != visit->node) continue;
        if (entering) {
            if (frame->is_range)
                return n00bb_lower_range_body_enter(context, lower, frame);
            if (frame->is_container)
                return n00bb_lower_container_body_enter(context, lower,
                                                        frame);
            ir_value_t *condition = ir_lower_node_value(
                context, frame->condition);
            th_type *bool_type = th_name(tu_state(lower->unit)->types, "bool");
            if (!condition || !bool_type
                || !ir_type_eq(context->module, condition->type, bool_type)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, frame->node),
                    "while condition must lower to bool");
                return false;
            }
            ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
            if (!branch || !ir_instr_operand_value(branch, condition)
                || !ir_instr_operand_block(branch, frame->body_block)
                || !ir_instr_operand_block(branch, frame->exit)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit while condition branch");
                return false;
            }
            branch->span = lower_span_for_tree(context, frame->node);
            context->block = frame->body_block;
            context->reachable = true;
        }
        else {
            if (context->reachable) {
                if (!n00bb_add_iteration_backedge(frame, context->block)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not add the iteration cursor backedge");
                    return false;
                }
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(
                                 jump, frame->continue_block)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit while back edge");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
            }
            if (frame->binding_scope_active
                && !ir_lower_bindings_restore(
                    context, frame->binding_checkpoint)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not restore bindings after loop body");
                return false;
            }
            frame->binding_scope_active = false;
            context->block = frame->exit;
            context->reachable = true;
        }
        return true;
    }
    for (size_t i = lower->switches.len; i > 0; i--) {
        n00bb_switch_frame_t *frame = &lower->switches.data[i - 1];
        if (!frame->active) continue;
        bool matched = false;
        size_t case_index = frame->cases.len;
        for (size_t j = 0; j < frame->cases.len; j++)
            if (frame->cases.data[j].body == visit->node) {
                case_index = j;
                matched = true;
                break;
            }
        bool is_default = frame->else_body == visit->node;
        if (!matched && !is_default) continue;
        if (entering) {
            if (!frame->emitted) {
                ir_value_t *scrutinee = ir_lower_node_value(
                    context, frame->scrutinee);
                bool has_chain = false;
                for (size_t c = 0; c < frame->cases.len; c++)
                    for (size_t p = 0;
                         p < frame->cases.data[c].patterns.len; p++)
                        has_chain |= frame->cases.data[c].patterns.data[p].is_range
                            || (frame->cases.data[c].patterns.data[p].expression
                                && !frame->cases.data[c].patterns.data[p].is_variant_tag);
                bool emitted = scrutinee != nullptr;
                ir_instr_t *instruction = nullptr;
                if (has_chain) {
                    size_t total = 0;
                    for (size_t c = 0; c < frame->cases.len; c++)
                        total += frame->cases.data[c].patterns.len;
                    size_t ordinal = 0;
                    ir_block_t *test_block = context->block;
                    th_type *scrutinee_type = scrutinee->type;
                    th_type *bool_type = th_name(
                        tu_state(lower->unit)->types, "bool");
                    for (size_t c = 0; emitted && c < frame->cases.len; c++) {
                        n00bb_switch_case_t *item = &frame->cases.data[c];
                        for (size_t p = 0; emitted
                                          && p < item->patterns.len; p++) {
                            n00bb_switch_pattern_t *pattern =
                                &item->patterns.data[p];
                            bool has_next = ++ordinal < total;
                            char next_name[64];
                            snprintf(next_name, sizeof(next_name),
                                "__n00bb_switch_test_%llu",
                                (unsigned long long)context->next_value_id++);
                            ir_block_t *next = has_next
                                ? ir_block_add(context->function, next_name)
                                : frame->default_block;
                            if (!next || !bool_type || !scrutinee_type) {
                                emitted = false;
                                break;
                            }
                            uint64_t id = context->next_value_id++;
                            char first_name[48], first_cmp_name[48];
                            snprintf(first_name, sizeof(first_name),
                                "__n00bb_case_bound_%llu",
                                (unsigned long long)id);
                            snprintf(first_cmp_name, sizeof(first_cmp_name),
                                "__n00bb_case_test_%llu",
                                (unsigned long long)id);
                            ir_instr_t *first_const = nullptr;
                            ir_value_t *pattern_value = nullptr;
                            if (pattern->expression)
                                pattern_value = ir_lower_node_value(
                                    context, pattern->expression);
                            else {
                                first_const = ir_instr_add(test_block,
                                                           IR_OP_CONST);
                                emitted = first_const
                                    && ir_instr_set_dest(first_const,
                                        first_name, scrutinee_type)
                                    && ir_instr_operand_int_lit(first_const,
                                        pattern->lower);
                                pattern_value = first_const
                                    ? first_const->dest : nullptr;
                            }
                            ir_instr_t *first_cmp = emitted
                                ? ir_instr_add(test_block,
                                    pattern->is_range ? IR_OP_GE : IR_OP_EQ)
                                : nullptr;
                            emitted = emitted && pattern_value && first_cmp
                                && ir_instr_set_dest(first_cmp,
                                    first_cmp_name, bool_type)
                                && ir_instr_operand_value(first_cmp,
                                                           scrutinee)
                                && ir_instr_operand_value(first_cmp,
                                                           pattern->is_range
                                                               ? pattern_value
                                                               : (pattern->expression
                                                                   ? pattern_value
                                                                   : first_const->dest));
                            ir_value_t *condition = emitted
                                ? first_cmp->dest : nullptr;
                            ir_instr_t *last_const = nullptr;
                            ir_instr_t *last_cmp = nullptr;
                            ir_block_t *upper_test = nullptr;
                            ir_instr_t *lower_branch = nullptr;
                            if (emitted && pattern->is_range) {
                                char upper_name[64];
                                snprintf(upper_name, sizeof(upper_name),
                                    "__n00bb_switch_upper_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                upper_test = ir_block_add(context->function,
                                                          upper_name);
                                last_const = upper_test
                                    ? ir_instr_add(upper_test, IR_OP_CONST)
                                    : nullptr;
                                last_cmp = upper_test
                                    ? ir_instr_add(upper_test,
                                        pattern->inclusive
                                            ? IR_OP_LE : IR_OP_LT)
                                    : nullptr;
                                char last_name[48], last_cmp_name[48];
                                snprintf(last_name, sizeof(last_name),
                                    "__n00bb_case_bound_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                snprintf(last_cmp_name,
                                    sizeof(last_cmp_name),
                                    "__n00bb_case_test_%llu",
                                    (unsigned long long)
                                        context->next_value_id++);
                                emitted = upper_test && last_const && last_cmp
                                    && ir_instr_set_dest(last_const,
                                        last_name, scrutinee_type)
                                    && ir_instr_operand_int_lit(last_const,
                                        pattern->upper)
                                    && ir_instr_set_dest(last_cmp,
                                        last_cmp_name, bool_type)
                                    && ir_instr_operand_value(last_cmp,
                                        scrutinee)
                                    && ir_instr_operand_value(last_cmp,
                                        last_const->dest);
                                condition = emitted ? last_cmp->dest : nullptr;
                            }
                            ir_instr_t *branch = emitted && !pattern->is_range
                                ? ir_instr_add(test_block, IR_OP_BRANCH)
                                : nullptr;
                            if (pattern->is_range) {
                                lower_branch = emitted
                                    ? ir_instr_add(test_block, IR_OP_BRANCH)
                                    : nullptr;
                                branch = emitted
                                    ? ir_instr_add(upper_test, IR_OP_BRANCH)
                                    : nullptr;
                                emitted = emitted && lower_branch && branch
                                    && ir_instr_operand_value(lower_branch,
                                        first_cmp->dest)
                                    && ir_instr_operand_block(lower_branch,
                                        upper_test)
                                    && ir_instr_operand_block(lower_branch,
                                        next)
                                    && ir_instr_operand_value(branch,
                                        condition)
                                    && ir_instr_operand_block(branch,
                                        item->block)
                                    && ir_instr_operand_block(branch, next);
                            }
                            else {
                                emitted = emitted && branch
                                    && ir_instr_operand_value(branch,
                                        condition)
                                    && ir_instr_operand_block(branch,
                                        item->block)
                                    && ir_instr_operand_block(branch, next);
                            }
                            ir_span_t span = lower_span_for_tree(
                                context, frame->node);
                            if (first_const) first_const->span = span;
                            if (first_cmp) first_cmp->span = span;
                            if (branch) branch->span = span;
                            if (lower_branch) lower_branch->span = span;
                            if (emitted && pattern->is_range) {
                                last_const->span = span;
                                last_cmp->span = span;
                            }
                            test_block = next;
                        }
                    }
                }
                else if (frame->variant_switch) {
                    th_type *tag_type = n00bb_ir_builtin_type(context->module, "uint");
                    ir_instr_t *tag = tag_type
                        ? ir_lower_emit_instruction(context,
                            IR_OP_VARIANT_TAG,
                            lower_span_for_tree(context, frame->node))
                        : nullptr;
                    instruction = tag
                        ? ir_instr_add(context->block, IR_OP_SWITCH) : nullptr;
                    emitted = tag && instruction
                        && ir_lower_set_temp_dest(context, tag,
                            "__n00bb_variant_tag_", tag_type)
                        && ir_instr_operand_value(tag, scrutinee)
                        && ir_instr_operand_value(instruction, tag->dest)
                        && ir_instr_operand_default(instruction,
                                                    frame->default_block);
                    for (size_t c = 0; emitted && c < frame->cases.len; c++)
                        for (size_t p = 0; emitted
                                && p < frame->cases.data[c].patterns.len; p++) {
                            n00bb_switch_pattern_t *pattern =
                                &frame->cases.data[c].patterns.data[p];
                            emitted = pattern->is_variant_tag
                                && ir_instr_operand_switch_arm_lit(
                                    instruction, pattern->lower,
                                    frame->cases.data[c].block);
                        }
                }
                else {
                    scrutinee = n00bb_switch_integer_scrutinee(context,
                        lower, scrutinee,
                        lower_span_for_tree(context, frame->node));
                    instruction = scrutinee
                        ? ir_instr_add(context->block, IR_OP_SWITCH) : nullptr;
                    emitted = instruction
                        && ir_instr_operand_value(instruction, scrutinee)
                        && ir_instr_operand_default(instruction,
                                                    frame->default_block);
                    for (size_t c = 0; emitted && c < frame->cases.len; c++)
                        for (size_t p = 0; emitted
                                          && p < frame->cases.data[c].patterns.len;
                             p++)
                            emitted = ir_instr_operand_switch_arm_lit(
                                instruction,
                                frame->cases.data[c].patterns.data[p].lower,
                                frame->cases.data[c].block);
                }
                if (!emitted) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame->node),
                        "could not emit switch dispatch");
                    return false;
                }
                if (instruction)
                    instruction->span = lower_span_for_tree(context,
                                                            frame->node);
                frame->emitted = true;
            }
            context->block = is_default ? frame->default_block
                : frame->cases.data[case_index].block;
            context->reachable = true;
        }
        else {
            if (frame->expression && context->reachable) {
                parse_tree_t *expression = value_body_expression(
                    visit->grammar, visit->node);
                ir_value_t *value = expression
                    ? ir_lower_node_value(context, expression) : nullptr;
                ir_instr_t *store = value
                    ? ir_lower_emit_instruction(context, IR_OP_COPY,
                        lower_span_for_tree(context, visit->node)) : nullptr;
                if (!store || !ir_instr_set_dest_local(
                                  store, frame->result_local)
                    || !ir_instr_operand_value(store, value)) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, visit->node),
                        "switch value arm does not produce a lowered value");
                    return false;
                }
            }
            if (context->reachable) {
                ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
                if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame->node),
                        "could not emit switch join");
                    return false;
                }
                jump->span = lower_span_for_tree(context, frame->node);
                frame->join_reachable = true;
            }
            context->reachable = false;
        }
        return true;
    }
    return true;
}

static parse_sym_entry_t *
n00bb_switch_enum_member(parse_grammar_t *grammar,
                         n00bb_lower_context_t *lower,
                         n00bb_switch_frame_t *frame, parse_tree_t *expression)
{
    if (!lower || !expression) return nullptr;
    parse_sym_entry_t *member = attached_symbol(grammar, lower->unit,
                                                expression);
    if (!member || member->kind != PARSE_SYM_ENUM_CONST) {
        member = nullptr;
        n00bb_tu_state_t *state = tu_state(lower->unit);
        parse_nt_node_t *parsed = &core_tree_node_value(expression);
        th_type *type = frame->scrutinee
            ? core_tree_node_value(frame->scrutinee).type : nullptr;
        parse_sym_entry_t *owner = state && type
            ? type_symbol_in_loaded_imports(lower->unit, state->types, type,
                                            nullptr)
            : nullptr;
        if (owner && owner->member_scope
            && parsed->end - parsed->start == 1)
            member = parse_symtab_lookup_in(owner->member_scope,
                                            first_token_text(expression));
    }
    if (!member || member->kind != PARSE_SYM_ENUM_CONST
        || n00bb_flagset_member(member) || !member->has_comptime_value
        || !member->comptime_value) return nullptr;
    return member;
}

static ir_value_t *
n00bb_switch_integer_scrutinee(ir_lower_context_t *context,
                               n00bb_lower_context_t *lower,
                               ir_value_t *scrutinee, ir_span_t span)
{
    if (!scrutinee || !lower) return scrutinee;
    ir_class_t class = ir_machine_of(context->module, scrutinee->type).class;
    if (class == IR_CLASS_SINT || class == IR_CLASS_UINT
        || class == IR_CLASS_BOOL) return scrutinee;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    parse_sym_entry_t *enum_symbol = state
        ? n00bb_enum_type_symbol(state->link_analysis, scrutinee->type)
        : nullptr;
    const n00bb_enum_range_t *range = enum_symbol
        ? parse_sym_binding(enum_symbol,
                            CORE_STRING_STATIC("n00bb.enum-range"))
        : nullptr;
    if (!range) return scrutinee;
    th_type *target = n00bb_ir_builtin_type(context->module,
                                            range->is_signed ? "int" : "uint");
    ir_instr_t *conversion = target
        ? ir_lower_emit_instruction(context, IR_OP_ENUM_VALUE, span)
        : nullptr;
    if (!conversion
        || !ir_lower_set_temp_dest(context, conversion, "__n00bb_", target)
        || !ir_instr_operand_value(conversion, scrutinee)) return nullptr;
    return conversion->dest;
}

static bool
n00bb_prune_enum_case(ir_lower_context_t *context, parse_visit_t *visit,
                      bool entering, void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!entering || !lower || !lower->switches.len) return true;
    n00bb_switch_frame_t *frame =
        &lower->switches.data[lower->switches.len - 1];
    if (frame->variant_scope
        || child_nt(visit->grammar, visit->node, "range-sep")) return true;
    parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                        "expression");
    if (!n00bb_switch_enum_member(visit->grammar, lower, frame, expression))
        return true;
    return ir_lower_prune_node(context, visit->node);
}

static bool
n00bb_switch_collect(parse_grammar_t *grammar, parse_tree_t *node,
                     n00bb_switch_frame_t *frame, ir_lower_context_t *context)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, "switch-case-block")
        || nt_is(grammar, node, "switch-value-case-block")) {
        bool expression = nt_is(grammar, node, "switch-value-case-block");
        n00bb_switch_case_t item = {
            .body = child_nt(grammar, node,
                             expression ? "value-body" : "body"),
            .block = n00bb_branch_block(context, context->user, "switch_case"),
            .patterns = core_list_new(n00bb_switch_pattern_t),
        };
        parse_tree_t *list = child_nt(grammar, node, "case-expr-list");
        if (!item.body || !item.block || !list) {
            core_list_free(item.patterns);
            return false;
        }
        core_list_t(parse_tree_ptr_t) stack = core_list_new(parse_tree_ptr_t);
        core_list_push(stack, list);
        while (stack.len) {
            parse_tree_t *current = stack.data[--stack.len];
            if (nt_is(grammar, current, "case-expr-item")) {
                parse_tree_t *separator = child_nt(
                    grammar, current, "range-sep");
                if (separator) {
                    parse_tree_t *bounds[2] = {0};
                    size_t bound_count = 0;
                    for (size_t i = 0; i < core_tree_num_children(current);
                         i++) {
                        parse_tree_t *child = core_tree_child(current, i);
                        if (nt_is(grammar, child, "expression")
                            && bound_count < 2)
                            bounds[bound_count++] = child;
                    }
                    n00bb_switch_pattern_t pattern = {
                        .is_range = true,
                        .inclusive = core_string_eq(first_token_text(separator),
                            CORE_STRING_STATIC("to")),
                    };
                    if (bound_count != 2
                        || !n00bb_switch_case_integer(grammar, bounds[0],
                                                       &pattern.lower)
                        || !n00bb_switch_case_integer(grammar, bounds[1],
                                                       &pattern.upper)) {
                        lower_error(context, IR_DIAG_VERIFY_PARSED,
                            lower_span_for_tree(context, current),
                            "switch ranges require integer literal bounds");
                        core_list_free(stack);
                        core_list_free(item.patterns);
                        return false;
                    }
                    core_list_push(item.patterns, pattern);
                }
                else {
                    ir_lit_t value = {0};
                    parse_tree_t *literal = n00bb_simple_switch_literal(
                        grammar, current, &(bool){false}, &(bool){false});
                    core_string_t text = first_token_text(literal);
                    parse_tree_t *expression = child_nt(
                        grammar, current, "expression");
                    bool literal_pattern = false;
                    if (core_string_eq(text, CORE_STRING_STATIC("true"))) {
                        value.mag = 1;
                        literal_pattern = true;
                    }
                    else if (core_string_eq(text,
                                            CORE_STRING_STATIC("false"))) {
                        value.mag = 0;
                        literal_pattern = true;
                    }
                    else literal_pattern = n00bb_switch_case_integer(
                        grammar, current, &value);
                    parse_sym_entry_t *variant_arm = nullptr;
                    core_string_t variant_arm_name = {0};
                    n00bb_lower_context_t *lower = context->user;
                    parse_sym_entry_t *enum_member = !literal_pattern
                        && !frame->variant_scope
                        ? n00bb_switch_enum_member(grammar, lower, frame,
                                                   expression)
                        : nullptr;
                    if (enum_member) {
                        n00bb_enum_value_t *ordinal =
                            enum_member->comptime_value;
                        value = (ir_lit_t){.mag = ordinal->magnitude,
                                           .neg = ordinal->negative};
                        literal_pattern = true;
                    }
                    if (!literal_pattern && frame->variant_scope && expression
                        && lower) {
                        variant_arm = attached_symbol(grammar, lower->unit,
                                                      expression);
                        if (n00bb_variant_arm_symbol(grammar, variant_arm)
                            && variant_arm->scope == frame->variant_scope) {
                            variant_arm_name = variant_arm->name;
                            value.mag = th_arm_tag(variant_arm_name.data);
                            literal_pattern = true;
                        }
                    }
                    if (!literal_pattern && !expression) {
                        lower_error(context, IR_DIAG_VERIFY_PARSED,
                            lower_span_for_tree(context, current),
                            "switch cases must be integer or boolean literals");
                        core_list_free(stack);
                        core_list_free(item.patterns);
                        return false;
                    }
                    core_list_push(item.patterns, ((n00bb_switch_pattern_t){
                        .lower = value,
                        .expression = variant_arm ? expression
                            : literal_pattern ? nullptr : expression,
                        .tag_node = variant_arm
                            ? n00bb_switch_tag_primary(grammar, expression)
                            : nullptr,
                        .variant_arm = variant_arm,
                        .variant_arm_name = variant_arm_name,
                        .is_variant_tag = variant_arm != nullptr,
                    }));
                }
                continue;
            }
            for (size_t c = core_tree_num_children(current); c > 0; c--)
                core_list_push(stack, core_tree_child(current, c - 1));
        }
        core_list_free(stack);
        if (frame->variant_scope && item.patterns.len != 1) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, node),
                "variant switch cases must name exactly one arm");
            core_list_free(item.patterns);
            return false;
        }
        core_list_push(frame->cases, item);
        return true;
    }
    if (nt_is(grammar, node, "case-else")
        || nt_is(grammar, node, "switch-value-else")) {
        frame->else_body = child_nt(grammar, node,
            nt_is(grammar, node, "switch-value-else")
                ? "value-body" : "body");
        return frame->else_body != nullptr;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_switch_collect(grammar, core_tree_child(node, i), frame,
                                  context)) return false;
    return true;
}

static bool
n00bb_variant_switch_exhaustive(parse_grammar_t *grammar,
                                parse_scope_t *variant_scope,
                                core_list_t(n00bb_switch_case_t) cases)
{
    if (!variant_scope) return false;
    for (size_t c = 0; c < cases.len; c++) {
        if (cases.data[c].patterns.len != 1) return false;
        parse_sym_entry_t *arm = cases.data[c].patterns.data[0].variant_arm;
        if (!arm) return false;
        for (size_t previous = 0; previous < c; previous++)
            if (cases.data[previous].patterns.len == 1
                && cases.data[previous].patterns.data[0].variant_arm == arm)
                return false;
    }
    for (parse_sym_entry_t *arm = variant_scope->first_in_scope; arm;
         arm = arm->next_in_scope) {
        if (!arm->decl_node
            || !nt_is(grammar, arm->decl_node, "variant-decl-one"))
            continue;
        bool covered = false;
        for (size_t c = 0; c < cases.len; c++)
            covered |= cases.data[c].patterns.len == 1
                && cases.data[c].patterns.data[0].variant_arm == arm;
        if (!covered) return false;
    }
    return true;
}

static bool
n00bb_typecase_collect(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_typecase_frame_t *frame,
                       ir_lower_context_t *context)
{
    if (nt_is(grammar, node, "typecase-case-block")
        || nt_is(grammar, node, "typecase-value-case-block")) {
        n00bb_lower_context_t *lower = context->user;
        n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
        n00bb_node_facts_t *facts = n00bb_node_facts(node);
        n00bb_typecase_arm_types_t *types = state && facts
            ? facts->typecase_arm_types : nullptr;
        th_type *type = types && types->count
            ? types->count == 1 ? types->types[0]
                : th_sum(state->types, TH_UNION, types->types, types->count)
            : nullptr;
        parse_tree_t *body = child_nt(grammar, node, "body");
        if (!body) body = child_nt(grammar, node, "value-body");
        ir_block_t *block = frame->active
            ? n00bb_branch_block(context, context->user, "typecase_arm")
            : nullptr;
        if (!type || !body || (frame->active && !block)) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, node),
                "typecase arm needs resolved types and a body");
            return false;
        }
        core_list_push(frame->arms, ((n00bb_typecase_arm_t){
            .node = node,
            .body = body,
            .type = type,
            .subject_shadow = parse_tree_resolution(node).kind
                == PARSE_NODE_RESOLUTION_SYMBOL
                ? parse_tree_resolution(node).target.symbol : nullptr,
            .block = block,
        }));
        return true;
    }
    if (nt_is(grammar, node, "case-else")
        || nt_is(grammar, node, "typecase-value-else")) {
        frame->else_body = child_nt(grammar, node, "body");
        if (!frame->else_body)
            frame->else_body = child_nt(grammar, node, "value-body");
        return frame->else_body != nullptr;
    }
    if (!node || core_tree_is_leaf(node)) return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_typecase_collect(grammar, core_tree_child(node, i), frame,
                                    context)) return false;
    return true;
}

static bool
n00bb_typecase_case_contains(ir_module_t *module, th_type *case_type,
                             th_type *wanted)
{
    if (ir_type_eq(module, case_type, wanted)
        || th_env_type_pattern_accepts(ir_module_types(module), case_type,
                                       wanted)) return true;
    th_comb comb;
    if (!th_sum_comb(case_type, &comb) || comb != TH_UNION) return false;
    for (size_t i = 0; i < th_sum_len(case_type); i++)
        if (n00bb_typecase_case_contains(module, th_sum_arm(case_type, i),
                                         wanted)) return true;
    return false;
}

static bool
n00bb_typecase_exhaustive(ir_module_t *module, th_type *subject,
                          core_list_t(n00bb_typecase_arm_t) arms)
{
    th_comb comb;
    if (th_sum_comb(subject, &comb) && comb == TH_UNION) {
        for (size_t i = 0; i < th_sum_len(subject); i++) {
            bool covered = false;
            for (size_t c = 0; c < arms.len; c++)
                covered |= n00bb_typecase_case_contains(
                    module, arms.data[c].type, th_sum_arm(subject, i));
            if (!covered) return false;
        }
        return true;
    }
    for (size_t c = 0; c < arms.len; c++)
        if (n00bb_typecase_case_contains(module, arms.data[c].type, subject))
            return true;
    return false;
}

static bool
n00bb_typecase_arm_has_fresh_match(ir_module_t *module, th_type *subject,
                                   core_list_t(n00bb_typecase_arm_t) arms,
                                   size_t arm_index)
{
    th_comb subject_comb;
    if (th_sum_comb(subject, &subject_comb) && subject_comb == TH_UNION) {
        for (size_t i = 0; i < th_sum_len(subject); i++) {
            th_type *candidate = th_sum_arm(subject, i);
            if (!n00bb_typecase_case_contains(
                    module, arms.data[arm_index].type, candidate))
                continue;
            bool prior = false;
            for (size_t p = 0; p < arm_index; p++)
                prior |= n00bb_typecase_case_contains(
                    module, arms.data[p].type, candidate);
            if (!prior) return true;
        }
        return false;
    }
    if (n00bb_typecase_case_contains(
            module, arms.data[arm_index].type, subject)) {
        for (size_t p = 0; p < arm_index; p++)
            if (n00bb_typecase_case_contains(module, arms.data[p].type,
                                             subject)) return false;
        return true;
    }
    th_type *current = th_resolve(arms.data[arm_index].type);
    size_t current_count = 1;
    th_comb current_comb;
    if (th_sum_comb(current, &current_comb) && current_comb == TH_UNION)
        current_count = th_sum_len(current);
    for (size_t i = 0; i < current_count; i++) {
        th_type *candidate = current_count == 1 ? current
                                                : th_sum_arm(current, i);
        bool prior = false;
        for (size_t p = 0; p < arm_index; p++)
            prior |= n00bb_typecase_case_contains(
                module, arms.data[p].type, candidate);
        if (!prior) return true;
    }
    return false;
}

static bool
n00bb_typecase_emit_dispatch(ir_lower_context_t *context,
                             n00bb_typecase_frame_t *frame)
{
    ir_value_t *subject = ir_lower_node_value(context, frame->subject_node);
    th_type *bool_type = th_name(tu_state(
        ((n00bb_lower_context_t *)context->user)->unit)->types, "bool");
    if (!subject || !bool_type) return false;
    frame->subject_value = subject;
    ir_block_t *test_block = context->block;
    ir_span_t span = lower_span_for_tree(context, frame->node);
    for (size_t i = 0; i < frame->arms.len; i++) {
        n00bb_typecase_arm_t *arm = &frame->arms.data[i];
        ir_block_t *next = i + 1 < frame->arms.len
            ? n00bb_branch_block(context, context->user, "typecase_test")
            : frame->else_body ? frame->else_block : frame->unmatched_block;
        ir_instr_t *test = ir_instr_add(test_block, IR_OP_TYPE_TEST);
        ir_instr_t *branch = test
            ? ir_instr_add(test_block, IR_OP_BRANCH) : nullptr;
        if (!next || !test || !ir_lower_set_temp_dest(context, test,
            "__n00bb_type_test_", bool_type)
            || !ir_instr_operand_value(test, subject)
            || !ir_instr_operand_type(test, arm->type)
            || !branch || !ir_instr_operand_value(branch, test->dest)
            || !ir_instr_operand_block(branch, arm->block)
            || !ir_instr_operand_block(branch, next)) return false;
        test->span = span;
        branch->span = span;
        test_block = next;
    }
    if (!frame->else_body) {
        ir_instr_t *unreachable = ir_instr_add(test_block, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = span;
    }
    frame->emitted = true;
    return true;
}

static bool
n00bb_lower_typecase_scope(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !visit->annot
        || !core_string_eq(visit->annot->scope_tag,
                           CORE_STRING_STATIC("block"))) return true;
    for (size_t i = lower->typecases.len; i > 0; i--) {
        n00bb_typecase_frame_t *frame = &lower->typecases.data[i - 1];
        size_t arm_index = frame->arms.len;
        for (size_t a = 0; a < frame->arms.len; a++)
            if (frame->arms.data[a].body == visit->node) {
                arm_index = a;
                break;
            }
        bool is_else = frame->else_body == visit->node;
        if (arm_index == frame->arms.len && !is_else) continue;
        if (!frame->active) return true;
        if (entering) {
            if (!frame->emitted && !n00bb_typecase_emit_dispatch(context,
                                                                   frame)) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase dispatch");
                return false;
            }
            if (is_else) {
                context->block = frame->else_block;
                context->reachable = true;
                return true;
            }
            n00bb_typecase_arm_t *arm = &frame->arms.data[arm_index];
            context->block = arm->block;
            context->reachable = true;
            ir_instr_t *narrow = ir_lower_emit_instruction(context,
                IR_OP_TYPE_NARROW, lower_span_for_tree(context, visit->node));
            if (!narrow || !ir_lower_set_temp_dest(context, narrow,
                                                    "__n00bb_narrow_",
                                                    arm->type)
                || !ir_instr_operand_value(narrow, frame->subject_value)
                || !ir_instr_operand_type(narrow, arm->type)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not narrow the typecase subject");
                return false;
            }
            frame->binding_checkpoint =
                ir_lower_bindings_checkpoint(context);
            ir_local_t *local = ir_lower_bound_local(context,
                                                     frame->subject_symbol);
            if (!arm->subject_shadow
                || !ir_lower_bind_scoped_value(context, arm->subject_shadow,
                                            narrow->dest, local)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not bind the narrowed typecase subject");
                return false;
            }
            return true;
        }
        if (frame->expression && context->reachable) {
            parse_tree_t *expression = value_body_expression(
                visit->grammar, visit->node);
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression) : nullptr;
            ir_instr_t *store = value
                ? ir_lower_emit_instruction(context, IR_OP_COPY,
                    lower_span_for_tree(context, visit->node)) : nullptr;
            if (!store || !frame->result_local
                || !ir_type_eq(context->module, value->type,
                               frame->result_local->type)
                || !ir_instr_set_dest_local(store, frame->result_local)
                || !ir_instr_operand_value(store, value)) {
                lower_error(context, IR_DIAG_TYPE_MISMATCH,
                    lower_span_for_tree(context, visit->node),
                    "typecase value arm does not match its result type");
                return false;
            }
        }
        if (!is_else
            && !ir_lower_bindings_restore(context,
                                            frame->binding_checkpoint)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, frame->node),
                "could not restore the typecase subject binding");
            return false;
        }
        if (context->reachable) {
            ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
            if (!jump || !ir_instr_operand_block(jump, frame->join)) {
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, frame->node),
                    "could not emit typecase join");
                return false;
            }
            jump->span = lower_span_for_tree(context, frame->node);
            frame->join_reachable = true;
        }
        return true;
    }
    return true;
}

static bool
n00bb_lower_typecase_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering)
{
    n00bb_lower_context_t *lower = context->user;
    if (entering) {
        bool expression = nt_is(visit->grammar, visit->node,
                                "typecase-value-expr");
        parse_cf_label_t *label = parse_cf_label_lookup(
            tu_state(lower->unit)->annotations, visit->node);
        parse_tree_t *subject_node = label
            ? n00bb_simple_lvalue(label->cond) : nullptr;
        parse_sym_entry_t *symbol = subject_node
            ? core_tree_node_value(subject_node).sym : nullptr;
        if (!label || !subject_node || !symbol
            || (symbol->kind != PARSE_SYM_VARIABLE
                && symbol->kind != PARSE_SYM_PARAM)) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "typecase subject must be a variable");
            return false;
        }
        n00bb_typecase_frame_t frame = {
            .node = visit->node,
            .label = n00bb_control_label(visit->grammar, visit->node,
                                         "labeled-typecase"),
            .subject_node = subject_node,
            .subject_symbol = symbol,
            .arms = core_list_new(n00bb_typecase_arm_t),
            .order = ++lower->next_control_order,
            .expression = expression,
            .active = context->reachable,
        };
        if (!n00bb_typecase_collect(visit->grammar, label->then_body,
                                    &frame, context)) {
            core_list_free(frame.arms);
            return false;
        }
        if (!frame.arms.len) {
            core_list_free(frame.arms);
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "typecase requires at least one case");
            return false;
        }
        th_type *subject_type = th_resolve(symbol->type);
        for (size_t i = 0; i < frame.arms.len; i++) {
            if (n00bb_typecase_arm_has_fresh_match(
                    context->module, subject_type, frame.arms, i))
                continue;
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(lower->unit), lower->unit,
                CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-W0006"),
                CORE_STRING_STATIC(
                    "typecase case cannot match an unhandled subject type"),
                link_source_location(frame.arms.data[i].node));
        }
        if (!frame.else_body
            && !n00bb_typecase_exhaustive(context->module, subject_type,
                                           frame.arms)) {
            core_list_free(frame.arms);
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, visit->node),
                "typecase without else must cover every subject type");
            return false;
        }
        if (frame.active) {
            frame.join = n00bb_branch_block(context, lower, "typecase_join");
            if (expression) {
                th_type *result_type = core_tree_node_value(visit->node).type;
                char local_name[64];
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_typecase_result_%llu",
                         (unsigned long long)context->next_value_id++);
                frame.result_local = result_type
                    ? ir_local_add(context->function, local_name, result_type)
                    : nullptr;
                if (!frame.result_local) {
                    core_list_free(frame.arms);
                    lower_error(context, IR_DIAG_VERIFY_TYPED,
                        lower_span_for_tree(context, visit->node),
                        "typecase expression has no inferred result type");
                    return false;
                }
            }
            frame.else_block = frame.else_body
                ? n00bb_branch_block(context, lower, "typecase_else") : nullptr;
            frame.unmatched_block = frame.else_body ? frame.else_block
                : n00bb_branch_block(context, lower, "typecase_unmatched");
            if (!frame.join || (frame.else_body && !frame.else_block)
                || !frame.unmatched_block) {
                core_list_free(frame.arms);
                lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not allocate typecase control-flow blocks");
                return false;
            }
        }
        core_list_push(lower->typecases, frame);
        return true;
    }
    if (!lower->typecases.len
        || lower->typecases.data[lower->typecases.len - 1].node != visit->node)
        return true;
    n00bb_typecase_frame_t frame =
        lower->typecases.data[--lower->typecases.len];
    core_list_free(frame.arms);
    if (!frame.active) return true;
    if (!frame.join_reachable) {
        ir_instr_t *unreachable = ir_instr_add(frame.join, IR_OP_UNREACHABLE);
        if (!unreachable) return false;
        unreachable->span = lower_span_for_tree(context, frame.node);
    }
    if (frame.expression && frame.join_reachable) {
        ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
        if (!read || !ir_lower_set_temp_dest(context, read,
            "__n00bb_typecase_value_", frame.result_local->type)
            || !ir_instr_operand_local(read, frame.result_local)
            || !ir_lower_set_node_value(context, frame.node, read->dest)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, frame.node),
                "could not read the typecase expression result");
            return false;
        }
        read->span = lower_span_for_tree(context, frame.node);
    }
    context->block = frame.join;
    context->reachable = frame.join_reachable;
    return true;
}

static bool
n00bb_lower_switch_annotation(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (nt_is(visit->grammar, visit->node, "typecase-stmt")
        || nt_is(visit->grammar, visit->node, "typecase-value-expr"))
        return n00bb_lower_typecase_annotation(context, visit, entering);
    if (!entering) {
        if (!lower->switches.len
            || lower->switches.data[lower->switches.len - 1].node
                != visit->node) return true;
        n00bb_switch_frame_t frame =
            lower->switches.data[--lower->switches.len];
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        if (frame.active) {
            if (!frame.join_reachable) {
                ir_instr_t *unreachable =
                    ir_instr_add(frame.join, IR_OP_UNREACHABLE);
                if (!unreachable) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame.node),
                        "could not terminate unreachable switch join");
                    return false;
                }
                unreachable->span = lower_span_for_tree(context, frame.node);
            }
            if (frame.expression && frame.join_reachable) {
                ir_instr_t *read = ir_instr_add(frame.join, IR_OP_COPY);
                if (!read || !ir_lower_set_temp_dest(context, read,
                    "__n00bb_switch_value_", frame.result_local->type)
                    || !ir_instr_operand_local(read, frame.result_local)
                    || !ir_lower_set_node_value(context, frame.node,
                                                read->dest)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, frame.node),
                        "could not read the value from the switch-expression join");
                    return false;
                }
                read->span = lower_span_for_tree(context, frame.node);
            }
            context->block = frame.join;
            context->reachable = frame.join_reachable;
        }
        return true;
    }
    bool expression = nt_is(visit->grammar, visit->node,
                            "switch-value-expr");
    if (!expression && !nt_is(visit->grammar, visit->node, "switch-stmt")) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, visit->node),
            "typecase is not currently lowered");
        return false;
    }
    parse_cf_label_t *label = parse_cf_label_lookup(
        tu_state(lower->unit)->annotations, visit->node);
    if (!label || label->kind != PARSE_CF_SWITCH || !label->cond
        || !label->then_body) return false;
    n00bb_switch_frame_t frame = {
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    "labeled-switch"),
        .scrutinee = label->cond,
        .cases = core_list_new(n00bb_switch_case_t),
        .order = ++lower->next_control_order,
        .expression = expression,
        .active = context->reachable,
    };
    parse_tree_t *subject_lvalue = n00bb_simple_lvalue(label->cond);
    frame.variant_subject = subject_lvalue
        ? attached_symbol(visit->grammar, lower->unit, subject_lvalue)
        : nullptr;
    if (frame.variant_subject
        && frame.variant_subject->kind != PARSE_SYM_VARIABLE
        && frame.variant_subject->kind != PARSE_SYM_PARAM)
        frame.variant_subject = nullptr;
    th_type *subject_type = core_tree_node_value(label->cond).type;
    if (!subject_type && frame.variant_subject)
        subject_type = frame.variant_subject->type;
    frame.variant_scope = n00bb_variant_scope_for_type(
        lower->unit, tu_state(lower->unit), subject_type);
    frame.variant_switch = frame.variant_scope != nullptr;
    if (!frame.active) {
        core_list_push(lower->switches, frame);
        return true;
    }
    if (frame.variant_switch && !frame.variant_subject) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, frame.node),
            "variant switch subject must be a variable");
        core_list_free(frame.cases);
        return false;
    }
    frame.join = n00bb_branch_block(context, lower, "switch_join");
    if (expression) {
        th_type *result_type = core_tree_node_value(visit->node).type;
        char local_name[48];
        snprintf(local_name, sizeof(local_name),
                 "__n00bb_switch_result_%llu",
                 (unsigned long long)context->next_value_id++);
        frame.result_local = result_type
            ? ir_local_add(context->function, local_name, result_type) : nullptr;
        if (!frame.result_local) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "switch expression has no inferred result type");
            return false;
        }
    }
    if (!frame.join || !n00bb_switch_collect(visit->grammar, label->then_body,
                                             &frame, context)) {
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        return false;
    }
    if (frame.variant_switch) {
        for (size_t c = 0; c < frame.cases.len; c++) {
            n00bb_switch_case_t *item = &frame.cases.data[c];
            parse_sym_entry_t *arm = item->patterns.len == 1
                ? item->patterns.data[0].variant_arm : nullptr;
            if (!arm) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, item->body),
                    "variant switch cases must name variant arms");
                for (size_t i = 0; i < frame.cases.len; i++)
                    core_list_free(frame.cases.data[i].patterns);
                core_list_free(frame.cases);
                return false;
            }
            for (size_t previous = 0; previous < c; previous++)
                if (frame.cases.data[previous].patterns.len == 1
                    && frame.cases.data[previous].patterns.data[0].variant_arm
                        == arm) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame.node),
                        "variant switch arm appears more than once");
                    for (size_t i = 0; i < frame.cases.len; i++)
                        core_list_free(frame.cases.data[i].patterns);
                    core_list_free(frame.cases);
                    return false;
                }
        }
        bool exhaustive = n00bb_variant_switch_exhaustive(
            visit->grammar, frame.variant_scope, frame.cases);
        if (!frame.else_body && !exhaustive) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, frame.node),
                "variant switch without else must cover every arm exactly once");
            for (size_t i = 0; i < frame.cases.len; i++)
                core_list_free(frame.cases.data[i].patterns);
            core_list_free(frame.cases);
            return false;
        }
        th_type *tag_type = n00bb_ir_builtin_type(context->module, "uint");
        for (size_t c = 0; tag_type && c < frame.cases.len; c++) {
            for (size_t p = 0; p < frame.cases.data[c].patterns.len; p++) {
                n00bb_switch_pattern_t *pattern =
                    &frame.cases.data[c].patterns.data[p];
                if (!pattern->is_variant_tag) {
                    lower_error(context, IR_DIAG_VERIFY_PARSED,
                        lower_span_for_tree(context, frame.node),
                        "variant switch cases must name variant arms");
                    return false;
                }
                parse_tree_t *primary = pattern->tag_node;
                ir_instr_t *tag_value = primary
                    ? ir_lower_emit_instruction(context, IR_OP_ARMTAG,
                        lower_span_for_tree(context, pattern->expression))
                    : nullptr;
                if (!tag_value || !ir_lower_set_temp_dest(context, tag_value,
                        "__n00bb_arm_tag_", tag_type)
                    || !ir_instr_operand_name(tag_value,
                        pattern->variant_arm_name.data)
                    || !ir_lower_set_node_value(context, primary,
                                                tag_value->dest)) {
                    lower_error(context, IR_DIAG_INTERNAL,
                        lower_span_for_tree(context, pattern->expression),
                        "could not bind variant case arm tag");
                    return false;
                }
            }
        }
    }
    bool exhaustive_variant = frame.variant_switch
        && n00bb_variant_switch_exhaustive(visit->grammar,
                                           frame.variant_scope, frame.cases);
    frame.default_block = frame.else_body
        ? n00bb_branch_block(context, lower, "switch_else")
        : exhaustive_variant
            ? n00bb_branch_block(context, lower, "switch_unreachable")
            : frame.join;
    frame.join_reachable = frame.else_body == nullptr
        && !exhaustive_variant;
    if (!frame.default_block) {
        for (size_t i = 0; i < frame.cases.len; i++)
            core_list_free(frame.cases.data[i].patterns);
        core_list_free(frame.cases);
        return false;
    }
    if (exhaustive_variant && !frame.else_body) {
        ir_instr_t *unreachable = ir_instr_add(frame.default_block,
                                               IR_OP_UNREACHABLE);
        if (!unreachable) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, frame.node),
                "could not terminate exhaustive variant switch default");
            for (size_t i = 0; i < frame.cases.len; i++)
                core_list_free(frame.cases.data[i].patterns);
            core_list_free(frame.cases);
            return false;
        }
        unreachable->span = lower_span_for_tree(context, frame.node);
    }
    core_list_push(lower->switches, frame);
    return true;
}

static bool
n00bb_lower_variant_payload(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, "postfix-expr")
        || core_tree_num_children(visit->node) < 3
        || !parse_tree_leaf_is(core_tree_child(visit->node, 1),
                               CORE_STRING_STATIC("."))) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower) return false;
    parse_tree_t *receiver_node = core_tree_child(visit->node, 0);
    parse_sym_entry_t *receiver = attached_symbol(visit->grammar,
        lower->unit, receiver_node);
    if (!receiver) return true;
    parse_sym_entry_t *accessed_arm = attached_symbol(visit->grammar,
        lower->unit, visit->node);
    parse_tree_t *parent = core_tree_node_value(visit->node).parent;
    while (parent) {
        for (size_t i = lower->switches.len; i > 0; i--) {
            n00bb_switch_frame_t *frame = &lower->switches.data[i - 1];
            if (!frame->active || !frame->variant_switch
                || frame->variant_subject != receiver) continue;
            for (size_t c = 0; c < frame->cases.len; c++) {
                n00bb_switch_case_t *item = &frame->cases.data[c];
                if (item->body != parent) continue;
                for (size_t p = 0; p < item->patterns.len; p++) {
                    n00bb_switch_pattern_t *pattern =
                        &item->patterns.data[p];
                    if (!pattern->is_variant_tag
                        || !pattern->variant_arm
                        || pattern->variant_arm != accessed_arm)
                        continue;
                    th_ctx *types = tu_state(lower->unit)->types;
                    th_type *payload_type = th_apply_subst(types,
                        core_tree_node_value(visit->node).type);
                    if (payload_type && th_is_nil(payload_type)) {
                        lower_error(context, IR_DIAG_TYPE_MISMATCH,
                            lower_span_for_tree(context, visit->node),
                            "nil variant arm has no readable payload");
                        return false;
                    }
                    ir_value_t *value = ir_lower_node_value(context,
                                                             receiver_node);
                    if (!value) return true;
                    ir_instr_t *payload = ir_lower_emit_instruction(context,
                        IR_OP_VARIANT_PAYLOAD,
                        lower_span_for_tree(context, visit->node));
                    if (!payload || !pattern->variant_arm || !payload_type
                        || !ir_lower_set_temp_dest(context, payload,
                            "__n00bb_payload_", payload_type)
                        || !ir_instr_operand_value(payload, value)
                        || !ir_instr_operand_name(payload,
                            pattern->variant_arm_name.data)
                        || !ir_lower_set_node_value(context, visit->node,
                                                    payload->dest)) {
                        lower_error(context, IR_DIAG_INTERNAL,
                            lower_span_for_tree(context, visit->node),
                            "could not lower variant payload access");
                        return false;
                    }
                    return true;
                }
            }
        }
        parent = core_tree_node_value(parent).parent;
    }
    return true;
}

static bool
n00bb_lower_loop_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    if (!entering) {
        if (!lower->loops.len
            || lower->loops.data[lower->loops.len - 1].node != visit->node)
            return true;
        n00bb_loop_frame_t frame =
            lower->loops.data[--lower->loops.len];
        core_list_free(frame.iteration_bindings);
        if (frame.active) {
            context->block = frame.exit;
            context->reachable = true;
        }
        return true;
    }

    n00bb_tu_state_t *tu = tu_state(lower->unit);
    parse_cf_label_t *label = tu
        ? parse_cf_label_lookup(tu->annotations, visit->node) : nullptr;
    if (!label || label->kind != PARSE_CF_LOOP
        || !label->cond || !label->then_body)
        return false;
    if (!context->reachable) {
        core_list_push(lower->loops, ((n00bb_loop_frame_t){
            .node = visit->node,
            .iteration_bindings = core_list_new(parse_sym_entry_ptr_t),
            .active = false,
        }));
        return true;
    }

    bool while_loop = nt_is(visit->grammar, visit->node, "while-stmt");
    bool range_loop = nt_is(visit->grammar, visit->node, "for-stmt")
        && nt_is(visit->grammar, label->cond, "range-loop");
    bool container_loop = nt_is(visit->grammar, visit->node, "for-stmt")
        && child_nt(visit->grammar, visit->node, "for-in-target-list");
    if (!while_loop && !range_loop && !container_loop) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "non-range for loops are not lowered yet");
        return false;
    }

    parse_tree_t *range_start = nullptr;
    parse_tree_t *range_end = nullptr;
    parse_tree_t *range_step = nullptr;
    parse_sym_entry_t *iterator_symbol = nullptr;
    ir_local_t *iterator = nullptr;
    ir_local_t *range_binding = nullptr;
    parse_tree_t *iteration_source = nullptr;
    n00bb_iteration_plan_t *iteration_plan = nullptr;
    core_list_t(parse_sym_entry_ptr_t) iteration_bindings =
        core_list_new(parse_sym_entry_ptr_t);
    bool inclusive = false;
    if (range_loop) {
        parse_tree_t *vars = child_nt(visit->grammar, visit->node,
                                      "for-range-var");
        parse_tree_t *parts = child_nt(visit->grammar, label->cond,
                                       "range-parts");
        parse_tree_t *step_node = child_nt(visit->grammar, label->cond,
                                           "range-step");
        core_list_t(parse_tree_ptr_t) range_bindings =
            core_list_new(parse_tree_ptr_t);
        collect_for_bindings(visit->grammar, vars, &range_bindings);
        if (!vars || range_bindings.len != 1 || !parts
            || core_tree_num_children(parts) < 3) {
            core_list_free(range_bindings);
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "range loops currently require one loop variable and two endpoints");
            return false;
        }
        for (size_t i = 0; i < core_tree_num_children(parts); i++) {
            parse_tree_t *child = core_tree_child(parts, i);
            if (nt_is(visit->grammar, child, "expression")) {
                if (!range_start) range_start = child;
                else range_end = child;
            }
            if (nt_is(visit->grammar, child, "range-sep"))
                inclusive = core_string_eq(first_token_text(child),
                                             CORE_STRING_STATIC("to"));
        }
        range_step = step_node
            ? child_nt(visit->grammar, step_node, "expression") : nullptr;
        iterator_symbol = core_tree_node_value(range_bindings.data[0]).sym;
        core_list_free(range_bindings);
        th_type *iterator_type = iterator_symbol
            ? iterator_symbol->type : nullptr;
        if (!range_start || !range_end || !iterator_symbol || !iterator_type) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "range loop variable or endpoint types are unresolved");
            return false;
        }
        char local_name[48];
        snprintf(local_name, sizeof(local_name), "__n00bb_for_%llu",
                 (unsigned long long)context->next_value_id++);
        iterator = ir_local_add(context->function, local_name, iterator_type);
        snprintf(local_name, sizeof(local_name), "__n00bb_for_binding_%llu",
                 (unsigned long long)context->next_value_id++);
        range_binding = iterator
            ? ir_local_add(context->function, local_name, iterator_type)
            : nullptr;
        if (!range_binding || !n00bb_bind_local(context, iterator_symbol,
                range_binding, lower_span_for_tree(context, visit->node))) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not bind range loop variable");
            return false;
        }
    }
    if (container_loop) {
        parse_tree_t *target = child_nt(visit->grammar, visit->node,
                                         "for-in-target-list");
        iteration_source = child_nt(visit->grammar, target, "expression");
        n00bb_node_facts_t *facts = n00bb_node_facts(visit->node);
        iteration_plan = facts ? facts->iteration_plan : nullptr;
        parse_tree_t *vars = child_nt(visit->grammar, visit->node,
                                      "for-var-list");
        core_list_t(parse_tree_ptr_t) binding_nodes =
            core_list_new(parse_tree_ptr_t);
        collect_for_bindings(visit->grammar, vars, &binding_nodes);
        bool valid = iteration_source && iteration_plan && binding_nodes.len;
        for (size_t i = 0; valid && i < binding_nodes.len; i++) {
            parse_sym_entry_t *symbol =
                core_tree_node_value(binding_nodes.data[i]).sym;
            char local_name[64];
            snprintf(local_name, sizeof(local_name),
                     "__n00bb_for_item_%llu",
                     (unsigned long long)context->next_value_id++);
            ir_local_t *local = symbol && symbol->type
                ? ir_local_add(context->function, local_name, symbol->type)
                : nullptr;
            valid = local && n00bb_bind_local(context, symbol, local,
                lower_span_for_tree(context, binding_nodes.data[i]));
            if (valid) core_list_push(iteration_bindings, symbol);
        }
        core_list_free(binding_nodes);
        if (!valid) {
            core_list_free(iteration_bindings);
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "container loop bindings or iterator type are unresolved");
            return false;
        }
    }

    ir_block_t *header = n00bb_branch_block(context, lower, "while_header");
    ir_block_t *body = n00bb_branch_block(context, lower, "while_body");
    ir_block_t *latch = range_loop
        ? n00bb_branch_block(context, lower, "for_latch") : header;
    ir_block_t *increment = range_loop && inclusive
        ? n00bb_branch_block(context, lower, "for_increment") : nullptr;
    ir_block_t *exit = n00bb_branch_block(context, lower, "while_exit");
    ir_instr_t *jump = header && !range_loop && !container_loop
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!header || !body || !latch || (range_loop && inclusive && !increment)
        || !exit || (while_loop && (!jump
            || !ir_instr_operand_block(jump, header)))) {
        core_list_free(iteration_bindings);
        lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not create while control-flow blocks");
        return false;
    }
    if (jump) jump->span = lower_span_for_tree(context, visit->node);
    core_list_push(lower->loops, ((n00bb_loop_frame_t){
        .node = visit->node,
        .label = n00bb_control_label(visit->grammar, visit->node,
                                    while_loop ? "labeled-while"
                                               : "labeled-for"),
        .condition = label->cond,
        .body = label->then_body,
        .range_start = range_start,
        .range_end = range_end,
        .range_step = range_step,
        .header = header,
        .body_block = body,
        .latch = latch,
        .increment = increment,
        .exit = exit,
        .continue_block = range_loop ? latch : header,
        .iterator = iterator,
        .range_binding = range_binding,
        .iterator_symbol = iterator_symbol,
        .iteration_source = iteration_source,
        .iteration_plan = iteration_plan,
        .iteration_bindings = iteration_bindings,
        .range_inclusive = inclusive,
        .is_range = range_loop,
        .is_container = container_loop,
        .order = ++lower->next_control_order,
        .active = true,
    }));
    if (while_loop) context->block = header;
    context->reachable = true;
    return true;
}

static bool n00bb_shape_has_constraints(ir_shape_t *shape);

static bool
n00bb_lower_jump_annotation(ir_lower_context_t *context, parse_visit_t *visit,
                            bool entering, void *state)
{
    (void)state;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))
        && !entering && context->reachable) {
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_shape_t *expected = lower && lower->bound_return_shape
            ? lower->bound_return_shape
            : lower && lower->function_symbol
                ? parse_sym_binding(lower->function_symbol,
                    CORE_STRING_STATIC("ir.return-shape")) : nullptr;
        if (expression && n00bb_shape_has_constraints(expected)) {
            ir_value_t *value = ir_lower_node_value(context, expression);
            if (!value || !n00bb_attach_expression_shape(context,
                    visit->grammar, expression, value)
                || !n00bb_emit_sequence_shape_check(context, value,
                    expected, lower_span_for_tree(context, visit->node)))
                return false;
        }
        return n00bb_emit_with_success(context, visit->node);
    }
    if (core_string_eq(name, CORE_STRING_STATIC("value-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        for (size_t i = lower->value_blocks.len; i > 0; i--) {
            n00bb_value_block_frame_t *frame =
                &lower->value_blocks.data[i - 1];
            if (!frame->active
                || !node_is_within(visit->node, frame->node)) continue;
            parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                                "expression");
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression) : nullptr;
            ir_instr_t *store = value
                ? ir_lower_emit_instruction(context, IR_OP_COPY,
                    lower_span_for_tree(context, visit->node)) : nullptr;
            ir_instr_t *jump = store
                ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
            if (!store || !ir_instr_set_dest_local(store, frame->result)
                || !ir_instr_operand_value(store, value) || !jump
                || !ir_instr_operand_block(jump, frame->continuation)) {
                lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "could not store a block value and continue");
                return false;
            }
            jump->span = lower_span_for_tree(context, visit->node);
            frame->value_written = true;
            context->block = frame->continuation;
            context->reachable = true;
            return true;
        }
        bool handled_by_value_expression = false;
        for (size_t i = 0; i < lower->tries.len; i++) {
            n00bb_try_frame_t *frame = &lower->tries.data[i];
            if (frame->active && frame->expression
                && node_is_within(visit->node, frame->node)) {
                handled_by_value_expression = true;
                break;
            }
        }
        for (parse_tree_t *ancestor = visit->node;
             ancestor && !handled_by_value_expression;
             ancestor = core_tree_node_value(ancestor).parent) {
            for (size_t i = 0; i < lower->branches.len; i++) {
                n00bb_branch_frame_t *frame = &lower->branches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->arms.len; arm++)
                    if (frame->arms.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
            for (size_t i = 0; i < lower->switches.len; i++) {
                n00bb_switch_frame_t *frame = &lower->switches.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->cases.len; arm++)
                    if (frame->cases.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
            for (size_t i = 0; i < lower->typecases.len; i++) {
                n00bb_typecase_frame_t *frame = &lower->typecases.data[i];
                if (!frame->active || !frame->expression) continue;
                for (size_t arm = 0; arm < frame->arms.len; arm++)
                    if (frame->arms.data[arm].body == ancestor)
                        handled_by_value_expression = true;
                if (frame->else_body == ancestor)
                    handled_by_value_expression = true;
            }
        }
        if (handled_by_value_expression) return true;

        bool function_body_value = false;
        bool nested_value_body = false;
        for (parse_tree_t *ancestor = core_tree_node_value(visit->node).parent;
             ancestor; ancestor = core_tree_node_value(ancestor).parent) {
            if (nt_is(visit->grammar, ancestor, "value-body"))
                nested_value_body = true;
            if (nt_is(visit->grammar, ancestor, "body")) {
                function_body_value = ancestor == lower->function_body;
                break;
            }
        }
        if (!function_body_value || nested_value_body) {
            lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "value statement is not in a supported value-producing block");
            return false;
        }
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        th_type *result_type = lower->function_symbol
            ? th_func_ret(lower->function_symbol->type) : nullptr;
        if (!value || !result_type
            || !ir_type_eq(context->module, value->type, result_type)) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, visit->node),
                "function value does not match its success result type");
            return false;
        }
        if (!n00bb_emit_with_success(context, visit->node)) return false;
        if (!n00bb_emit_success_return(context, value,
                &lower->parameters_to_copy_out,
                lower_span_for_tree(context, visit->node))) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not emit function value return");
            return false;
        }
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("fail-stmt"))) {
        if (entering || !context->reachable) return true;
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        if (!value) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "fail statement has no lowered failure value");
            return false;
        }
        if (!n00bb_emit_with_failure(context, visit->node))
            return false;
        int routed = n00bb_route_try_error(context, visit->node, value, span);
        if (routed) return routed > 0;
        th_type *failure_type = lower->function_symbol
            ? th_func_error(lower->function_symbol->type) : nullptr;
        if (!failure_type || th_is_nil(failure_type)) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                "fail statement requires a non-nil function failure type");
            return false;
        }
        if (!value || !ir_type_eq(context->module, value->type,
                                  failure_type)) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                span,
                "failure value does not match this function's failure type");
            return false;
        }
        ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
        if (!fail || !ir_instr_operand_value(fail, value)) {
            lower_error(context, IR_DIAG_INTERNAL,
                span,
                "could not emit function failure exit");
            return false;
        }
        fail->span = span;
        context->reachable = false;
        return true;
    }
    if (!entering) return true;
    if (core_string_eq(name, CORE_STRING_STATIC("return-stmt"))) return true;
    if (!core_string_eq(name, CORE_STRING_STATIC("break-stmt"))
        && !core_string_eq(name, CORE_STRING_STATIC("continue-stmt"))) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this jump form is not lowered yet");
        return false;
    }
    parse_cf_label_t *cf_label = parse_cf_label_lookup(
        tu_state(((n00bb_lower_context_t *)context->user)->unit)->annotations,
        visit->node);
    if (!cf_label || cf_label->kind != PARSE_CF_JUMP) return false;
    n00bb_lower_context_t *lower = context->user;
    core_string_t target_name = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    n00bb_loop_frame_t *target = nullptr;
    for (size_t i = lower->loops.len; i > 0; i--) {
        if (lower->loops.data[i - 1].active
            && (!target_name.data || core_string_eq(
                    lower->loops.data[i - 1].label, target_name))
            && (!target || lower->loops.data[i - 1].order > target->order)) {
            target = &lower->loops.data[i - 1];
        }
    }
    n00bb_switch_frame_t *switch_target = nullptr;
    n00bb_typecase_frame_t *typecase_target = nullptr;
    bool is_break = core_string_eq(name, CORE_STRING_STATIC("break-stmt"));
    if (is_break) {
        uint64_t best_order = target ? target->order : 0;
        for (size_t i = lower->switches.len; i > 0; i--) {
            n00bb_switch_frame_t *candidate = &lower->switches.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name)) && (!switch_target
                || candidate->order > best_order)) {
                switch_target = candidate;
                typecase_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
        for (size_t i = lower->typecases.len; i > 0; i--) {
            n00bb_typecase_frame_t *candidate =
                &lower->typecases.data[i - 1];
            if (candidate->active && (!target_name.data || core_string_eq(
                    candidate->label, target_name))
                && candidate->order > best_order) {
                typecase_target = candidate;
                switch_target = nullptr;
                target = nullptr;
                best_order = candidate->order;
            }
        }
    }
    if (!target && !switch_target && !typecase_target) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    target_name.data
                        ? "jump target does not name an active lowered construct"
                        : "loop jump appears outside a lowered loop, switch, or typecase");
        return false;
    }
    ir_block_t *destination = typecase_target ? typecase_target->join
        : switch_target ? switch_target->join
        : is_break ? target->exit : target->continue_block;
    if (target) {
        if (!n00bb_emit_scoped_success(context, visit->node, target)
            || (is_break && !n00bb_emit_iteration_cleanup(
                    context, target, visit->node)))
            return false;
        if (!is_break
            && !n00bb_add_iteration_backedge(target, context->block)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not add the continued iteration cursor backedge");
            return false;
        }
    }
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, destination)) {
        lower_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not emit loop jump");
        return false;
    }
    jump->span = lower_span_for_tree(context, visit->node);
    if (switch_target) switch_target->join_reachable = true;
    if (typecase_target) typecase_target->join_reachable = true;
    context->reachable = false;
    return true;
}

static ir_value_t *n00bb_typed_integer_literal(ir_lower_context_t *context,
                                               parse_tree_t *node,
                                               th_type *target);
static ir_value_t *n00bb_typed_integer_value(ir_lower_context_t *context,
                                             parse_tree_t *node,
                                             ir_value_t *value,
                                             th_type *target);

static ir_lower_result_t
n00bb_lower_return(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)result_type;
    (void)state;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower || operand_count > 1 || (operand_count && !operands[0]))
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
            "return lowering received an invalid success value");
    ir_value_t *value = operand_count ? operands[0] : nullptr;
    th_type *expected = lower->function_symbol
        ? th_func_ret(lower->function_symbol->type) : nullptr;
    parse_tree_t *expression = visit
        ? child_nt(visit->grammar, visit->node, "expression") : nullptr;
    if (value && expected) {
        value = n00bb_materialize(context, expression, value, expected, span);
        if (!value)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not materialize the returned value");
    }
    if (!n00bb_emit_success_return(context, value,
            &lower->parameters_to_copy_out, span))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the function success result and copy-out values");
    return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
}

static ir_lower_result_t
n00bb_emit_comparison_test(ir_lower_context_t *context, ir_value_t *value,
                           parse_sym_entry_t *member, ir_span_t span)
{
    n00bb_enum_value_t *ordinal = member && member->has_comptime_value
        ? member->comptime_value : nullptr;
    if (!ordinal || !member->type
        || !ir_type_eq(context->module, member->type, value->type))
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            "comparison member has no matching enum value and type");
    ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                     IR_OP_ENUM_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    if (!constant || !test || !bool_type
        || !ir_lower_set_temp_dest(context, constant,
                                   "__n00bb_", member->type)
        || !ir_instr_operand_name(constant, member->name.data)
        || !ir_instr_operand_int_lit(constant,
            (ir_lit_t){.mag = ordinal->magnitude, .neg = ordinal->negative})
        || !ir_lower_set_temp_dest(context, test, "__n00bb_", bool_type)
        || !ir_instr_operand_value(test, value)
        || !ir_instr_operand_value(test, constant->dest))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not test the comparison result");
    return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
}

static parse_sym_entry_t *
n00bb_comparison_member(n00bb_link_analysis_t *analysis, th_type *type,
                        const char *name)
{
    if (!analysis || !type || !name) return nullptr;
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        parse_namespace_t *type_ns = symtab
            ? parse_symtab_ns(symtab, CORE_STRING_STATIC("type")) : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next) {
            if (!type_ns || scope->ns != type_ns) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (!symbol->type || !symbol->member_scope
                    || !core_string_eq(symbol->member_scope->scope_tag,
                                       CORE_STRING_STATIC("enum"))
                    || th_hash(state->types, symbol->type)
                        != th_hash(state->types, type)) continue;
                parse_sym_entry_t *member = parse_symtab_lookup_in(
                    symbol->member_scope, core_string_from_cstr(name));
                if (member && member->kind == PARSE_SYM_ENUM_CONST)
                    return member;
            }
        }
    }
    return nullptr;
}


static ir_lower_result_t n00bb_lower_operator_method(
    ir_lower_context_t *context, parse_visit_t *visit,
    ir_value_t *const *operands, size_t operand_count,
    th_type *result_type, ir_span_t span);
static ir_func_t *n00bb_closed_builtin_operator(
    ir_lower_context_t *context, parse_sym_entry_t *implementation,
    parse_sym_entry_t *requirement, th_type *receiver_type);

static ir_lower_result_t
n00bb_lower_comparison(ir_lower_context_t *context, parse_visit_t *visit,
                       ir_value_t *const *operands, th_type *result_type,
                       ir_span_t span, core_string_t spelling)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    bool equality = core_string_eq(spelling, CORE_STRING_STATIC("=="))
        || core_string_eq(spelling, CORE_STRING_STATIC("!="));
    bool direct_equal = equality && site
        && core_string_eq(site->dispatch_method,
                          CORE_STRING_STATIC("__equal"));
    n00bb_link_analysis_t *analysis = caller
        ? caller->link_analysis : nullptr;
    th_type *comparison_type = direct_equal ? result_type
        : th_name(ir_module_types(context->module), "Comparison");
    if (!comparison_type || !ir_type_eq(context->module, result_type,
            n00bb_ir_builtin_type(context->module, "bool")))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "comparison expression must yield bool");
    ir_lower_result_t called = n00bb_lower_operator_method(context, visit,
        operands, 2, comparison_type, span);
    if (called.status != IR_LOWER_VALUE) return called;
    ir_value_t *comparison = called.value;

    if (equality) {
        ir_lower_result_t equal = {IR_LOWER_VALUE, comparison};
        if (!direct_equal) {
            parse_sym_entry_t *member = n00bb_comparison_member(
                analysis, comparison_type, "equal");
            equal = member
                ? n00bb_emit_comparison_test(context, comparison, member, span)
                : lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                    "Comparison is missing the equal member");
        }
        if (equal.status != IR_LOWER_VALUE
            || core_string_eq(spelling, CORE_STRING_STATIC("==")))
            return equal;
        ir_instr_t *false_value = ir_lower_emit_instruction(context,
                                                             IR_OP_CONST,
                                                             span);
        if (!false_value
            || !ir_lower_set_temp_dest(context, false_value,
                                       "__n00bb_", result_type)
            || !ir_instr_operand_int(false_value, 0))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit inequality's false value");
        ir_instr_t *not_equal = ir_lower_emit_instruction(context, IR_OP_EQ,
                                                           span);
        if (!not_equal
            || !ir_lower_set_temp_dest(context, not_equal,
                                       "__n00bb_", result_type)
            || !ir_instr_operand_value(not_equal, equal.value)
            || !ir_instr_operand_value(not_equal, false_value->dest))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not negate the equality result");
        return (ir_lower_result_t){IR_LOWER_VALUE, not_equal->dest};
    }

    const char *first = core_string_eq(spelling, CORE_STRING_STATIC("<"))
        || core_string_eq(spelling, CORE_STRING_STATIC("<="))
            ? "less" : "greater";
    parse_sym_entry_t *first_member = n00bb_comparison_member(
        analysis, comparison_type, first);
    ir_lower_result_t test = first_member
        ? n00bb_emit_comparison_test(context, comparison, first_member, span)
        : lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "Comparison is missing the required ordering member");
    if (test.status != IR_LOWER_VALUE) return test;
    if (!core_string_eq(spelling, CORE_STRING_STATIC("<="))
        && !core_string_eq(spelling, CORE_STRING_STATIC(">=")))
        return test;
    parse_sym_entry_t *equal_member = n00bb_comparison_member(
        analysis, comparison_type, "equal");
    ir_lower_result_t equal = equal_member
        ? n00bb_emit_comparison_test(context, comparison, equal_member, span)
        : lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "Comparison is missing the equal member");
    if (equal.status != IR_LOWER_VALUE) return equal;
    ir_instr_t *truth = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *either = ir_lower_emit_instruction(context, IR_OP_SELECT,
                                                   span);
    if (!truth || !either
        || !ir_lower_set_temp_dest(context, truth,
                                   "__n00bb_", result_type)
        || !ir_instr_operand_int(truth, 1)
        || !ir_lower_set_temp_dest(context, either,
                                   "__n00bb_", result_type)
        || !ir_instr_operand_value(either, test.value)
        || !ir_instr_operand_value(either, truth->dest)
        || !ir_instr_operand_value(either, equal.value))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not combine comparison outcomes");
    return (ir_lower_result_t){IR_LOWER_VALUE, either->dest};
}

static ir_value_t *n00bb_promoted_operand(ir_lower_context_t *context,
                                          parse_tree_t *node,
                                          ir_value_t *value, bool comparison,
                                          ir_span_t span);
static bool n00bb_attach_call_return_alias(ir_lower_context_t *context,
    n00bb_lower_context_t *lower, n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *function, ir_value_t *const *arguments,
    size_t argument_count, bool escapes, ir_value_t *result);
static bool n00bb_tree_returns_value(parse_grammar_t *grammar,
                                     parse_tree_t *node);

static ir_value_t *
n00bb_abstract_trait_requirement(ir_lower_context_t *context,
                                 n00bb_link_use_site_t *site,
                                 th_type *requirement_contract,
                                 th_type *signature, ir_value_t *receiver,
                                 ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    core_buffer_t requirement_name = {0};
    core_buffer_printf(&requirement_name, "__trait_req_%s_%.*s",
        site->resolved_trait_name,
        (int)site->dispatch_method.u8_bytes,
        site->dispatch_method.data);
    ir_func_t *requirement = requirement_name.data
        ? ir_func_find(context->module, requirement_name.data) : nullptr;
    if (!requirement && requirement_name.data) {
        th_type *requirement_type = th_type_clone(types, requirement_contract);
        requirement = ir_func_add(context->module,
            requirement_name.data, requirement_type, IR_LINK_EXTERN);
        if (requirement && !th_is_closed(types, requirement_contract)
            && !ir_func_set_callable_contract(requirement,
                                              requirement_contract))
            requirement = nullptr;
    }
    core_buffer_release(&requirement_name);
    ir_value_t *requirement_address = requirement
        ? ir_func_address(context->module, requirement) : nullptr;
    ir_instr_t *selection = requirement_address
        ? ir_lower_emit_instruction(context, IR_OP_TRAIT_METHOD, span)
        : nullptr;
    if (!selection || !ir_lower_set_temp_dest(context, selection,
                                              "__n00bb_", signature)
        || !ir_instr_operand_name(selection, site->resolved_trait_name)
        || !ir_instr_operand_name(selection, site->dispatch_method.data)
        || !ir_instr_operand_value(selection, receiver)
        || !ir_instr_operand_value(selection, requirement_address))
        return nullptr;
    return selection->dest;
}

static ir_lower_result_t
n00bb_lower_operator_method(ir_lower_context_t *context,
                          parse_visit_t *visit,
                          ir_value_t *const *operands,
                          size_t operand_count, th_type *result_type,
                          ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    n00bb_trait_dispatch_plan_t *closed_dispatch = site
        ? site->closed_dispatch : nullptr;
    n00bb_link_analysis_t *analysis = caller
        ? caller->link_analysis : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    bool builtin_operator_trait = site && site->resolved_trait_name
        && site->dispatch_trait.data
        && !strcmp(site->resolved_trait_name, site->dispatch_trait.data);
    bool open_dispatch = n00bb_open_dispatch(analysis);
    parse_sym_entry_t *requirement = linkable_trait_requirement(analysis,
        lower->unit, site, symbol);
    symbol = requirement ? requirement
        : builtin_operator_trait ? symbol : nullptr;
    bool builtin_requirement = symbol && parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.builtin-trait-method"));
    core_translation_unit_t *owner = symbol
        ? unit_for_symbol(visit->grammar, lower->units, symbol) : nullptr;
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    ir_func_t *function = owner_state
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_module_t *module = owner_state
        ? unit_module(owner_state) : nullptr;
    ir_value_t *callee = module && function
        ? ir_func_address(module, function) : nullptr;
    if (!callee && symbol && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"))) {
        parse_sym_entry_t *requirement = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"));
        ir_func_t *builtin = n00bb_closed_builtin_operator(context, symbol,
            requirement, operands[0]->type);
        callee = builtin ? ir_func_address(context->module, builtin) : nullptr;
    }
    bool abstract_builtin = builtin_operator_trait
        && (open_dispatch || builtin_requirement);
    if (!callee && !abstract_builtin && !closed_dispatch)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "operator has no resolved trait method in this execution phase");
    th_ctx *types = ir_module_types(context->module);
    th_type *requirement_contract = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    parse_sym_entry_t *implementation = builtin_requirement && site->symbol
        && !parse_sym_binding(site->symbol,
            CORE_STRING_STATIC("n00bb.builtin-trait-method"))
            ? site->symbol : symbol;
    th_type *contract = implementation->callable_contract
        ? implementation->callable_contract : implementation->type;
    th_type *callable = th_type_clone(types, contract);
    th_param parameters[2] = {
        {.mode = TH_IN, .type = operands[0]->type},
        {.mode = TH_IN, .type = operand_count == 2
            ? operands[1]->type : nullptr},
    };
    th_type *call_failure = callable ? th_func_error(callable) : nullptr;
    if (builtin_requirement && implementation == symbol && call_failure
        && th_kind_of(th_resolve(call_failure)) == TH_VAR
        && !lower->tries.len)
        call_failure = th_func_error(context->function->type);
    th_type *shape = callable
        ? th_func_with_error(types, result_type, call_failure,
            parameters, operand_count, nullptr, nullptr, 0) : nullptr;
    th_type *signature = shape ? th_unify(types, callable, shape) : nullptr;
    // Bind the copied call instance to caller nodes, never to the contract.
    if (signature) signature = th_unify_in_place(types, signature, shape);
    if (!signature || th_func_nparams(signature) != operand_count
        || th_func_rest(signature))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "operator operands or result do not match its trait method");
    bool semantic_trait_call = !closed_dispatch && !builtin_requirement && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("trait"));
    if (abstract_builtin && !closed_dispatch) {
        callee = n00bb_abstract_trait_requirement(context, site,
            requirement_contract, signature, operands[0], span);
        if (!callee)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not retain the builtin operator trait selection");
    }
    if (semantic_trait_call) {
        callee = n00bb_select_trait_method(context, visit->grammar,
            owner, symbol, signature, operands[0], callee, span);
        if (!callee)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not select the operator trait requirement");
    }
    th_type *failure = th_func_error(signature);
    bool can_fail = failure && !th_is_nil(failure);
    th_type *call_type = can_fail
        ? ir_call_outcome_type(context->module, signature)
        : th_func_ret(signature);
    if (closed_dispatch) {
        n00bb_trait_dispatch_plan_t specialized = *closed_dispatch;
        n00bb_trait_dispatch_entry_t *selected = core_alloc_array(
            n00bb_trait_dispatch_entry_t, closed_dispatch->count);
        if (!selected)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not allocate the closed operator dispatch");
        size_t selected_count = 0;
        bool has_instantiations = false;
        bool all_matched = true;
        for (size_t f = 0; f < analysis->trait_specializations.len; f++) {
            n00bb_trait_specialization_t *fact =
                analysis->trait_specializations.data[f];
            if (fact->trait_site != visit->node
                || fact->generic_function != lower->function_symbol)
                continue;
            has_instantiations = true;
            bool matched = false;
            for (size_t i = 0; i < closed_dispatch->count; i++) {
                n00bb_trait_dispatch_entry_t *entry =
                    &closed_dispatch->entries[i];
                if (!ir_type_eq(context->module, fact->receiver_type,
                                entry->receiver_type)) continue;
                bool duplicate = false;
                for (size_t j = 0; j < selected_count; j++)
                    if (selected[j].implementation == entry->implementation)
                        duplicate = true;
                if (!duplicate) selected[selected_count++] = *entry;
                matched = true;
                break;
            }
            if (!matched) all_matched = false;
        }
        if (has_instantiations && all_matched && selected_count) {
            specialized.entries = selected;
            specialized.count = selected_count;
            specialized.direct = selected_count == 1;
        }
        core_list_t(n00bb_ir_value_ptr_t) values =
            core_list_new(n00bb_ir_value_ptr_t);
        for (size_t i = 0; i < operand_count; i++)
            core_list_push(values, operands[i]);
        ir_instr_t *dispatched = n00bb_emit_closed_trait_call(context, lower,
            &specialized, visit->grammar, symbol, nullptr, operand_count,
            signature, call_type, &values, nullptr, span);
        core_list_free(values);
        core_free(selected);
        if (!dispatched)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the closed operator trait dispatch");
        ir_value_t *result = dispatched->dest;
        if (can_fail && !n00bb_unwrap_call_outcome(context, visit, result,
                                                   span, &result, false))
            return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
        return (ir_lower_result_t){IR_LOWER_VALUE, result};
    }
    ir_instr_t *call = call_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool call_ok = call && ir_lower_set_temp_dest(context, call,
                                                  "__n00bb_", call_type)
        && ir_instr_operand_value(call, callee);
    for (size_t i = 0; call_ok && i < operand_count; i++)
        call_ok = ir_instr_operand_value(call, operands[i]);
    if (!call_ok || !ir_instr_set_call_signature(call, signature))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the operator trait method call");
    if (!n00bb_attach_call_return_alias(context, lower, analysis, symbol,
            operands, operand_count, n00bb_tree_returns_value(visit->grammar,
                visit->node), call->dest))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not retain the operator result alias provenance");
    ir_value_t *result = call->dest;
    if (can_fail && !n00bb_unwrap_call_outcome(context, visit, result,
                                               span, &result, false))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    return (ir_lower_result_t){IR_LOWER_VALUE, result};
}

static ir_lower_result_t
n00bb_lower_in_place_method(ir_lower_context_t *context, parse_visit_t *visit,
                            n00bb_link_use_site_t *site,
                            ir_value_t *const *operands, th_type *target_type,
                            ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = tu_state(lower->unit);
    n00bb_link_analysis_t *analysis = caller ? caller->link_analysis : nullptr;
    parse_sym_entry_t *symbol = site->symbol;
    core_translation_unit_t *owner = symbol
        ? unit_for_symbol(visit->grammar, lower->units, symbol) : nullptr;
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    ir_func_t *function = owner_state
        ? link_ir_function_for_symbol(analysis, symbol)
        : nullptr;
    ir_module_t *module = owner_state
        ? unit_module(owner_state) : nullptr;
    ir_value_t *callee = module && function
        ? ir_func_address(module, function) : nullptr;
    if (!callee)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "in-place assignment method has no function in this phase");
    th_ctx *types = ir_module_types(context->module);
    th_type *contract = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    th_type *callable = th_type_clone(types, contract);
    th_param parameters[2] = {
        {.mode = TH_INOUT, .type = target_type},
        {.mode = TH_IN, .type = operands[1]->type},
    };
    th_type *shape = callable
        ? th_func_with_error(types, th_func_ret(callable),
            th_func_error(callable), parameters, 2, nullptr, nullptr, 0)
        : nullptr;
    th_type *signature = shape ? th_unify(types, callable, shape) : nullptr;
    if (signature) signature = th_unify_in_place(types, signature, shape);
    if (!signature || th_func_nparams(signature) != 2)
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "operands do not match the in-place assignment method");
    th_type *failure = th_func_error(signature);
    bool can_fail = failure && !th_is_nil(failure);
    th_type *call_type = can_fail
        ? ir_call_outcome_type(context->module, signature)
        : th_func_ret(signature);
    ir_instr_t *call = call_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    if (!call || !ir_lower_set_temp_dest(context, call, "__n00bb_", call_type)
        || !ir_instr_operand_value(call, callee)
        || !ir_instr_operand_value(call, operands[0])
        || !ir_instr_operand_value(call, operands[1])
        || !ir_instr_set_call_signature(call, signature))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the in-place assignment call");
    ir_value_t *completed = call->dest;
    if (can_fail && !n00bb_unwrap_call_outcome(context, visit, completed,
                                               span, &completed, false))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    ir_instr_t *updated = ir_lower_emit_instruction(context,
                                                    IR_OP_CALL_OUTPUT, span);
    if (!updated || !ir_lower_set_temp_dest(context, updated, "__n00bb_",
                                            target_type)
        || !ir_instr_operand_value(updated, completed)
        || !ir_instr_operand_uint(updated, 0))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not read the in-place assignment result");
    return (ir_lower_result_t){IR_LOWER_VALUE, updated->dest};
}

static ir_value_t *
n00bb_lower_shift_count(ir_lower_context_t *context, parse_tree_t *node,
                        ir_value_t *value, th_type *receiver,
                        ir_span_t span)
{
    th_type *target = n00bb_shift_count_type(ir_module_types(context->module),
                                             receiver);
    if (!target || !value || !n00bb_unsigned_shift_count_type(value->type)) {
        lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "built-in shift count must be unsigned");
        return nullptr;
    }
    if (ir_type_eq(context->module, value->type, target)) return value;
    ir_value_t *proved = node
        ? n00bb_typed_integer_literal(context, node, target) : nullptr;
    if (proved && ir_type_eq(context->module, proved->type, target))
        return proved;

    ir_machine_t source = ir_machine_of(context->module, value->type);
    ir_machine_t destination = ir_machine_of(context->module, target);
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (!cast || !ir_lower_set_temp_dest(context, cast, "__n00bb_", target)
        || !ir_instr_operand_value(cast, value)
        || (source.width > destination.width
            && !ir_instr_add_mod(cast, "trap"))) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not lower the shift count conversion");
        return nullptr;
    }
    return cast->dest;
}

static ir_lower_result_t
n00bb_lower_binary(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    if (operand_count != 2 || !operands || !operands[0] || !operands[1]
        || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "binary operation has incomplete operands or type");
    core_string_t spelling = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    bool flagset = caller && n00bb_flagset_type(lower->unit, caller,
                                                operands[0]->type, nullptr);
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_spelled(spelling);
    ir_op_t op;
    if (flagset && core_string_eq(spelling, CORE_STRING_STATIC("+")))
        op = IR_OP_BITSET_UNION;
    else if (flagset && core_string_eq(spelling, CORE_STRING_STATIC("-")))
        op = IR_OP_BITSET_DIFFERENCE;
    else if (flagset && core_string_eq(spelling, CORE_STRING_STATIC("&")))
        op = IR_OP_BITSET_INTERSECTION;
    else if (flagset && core_string_eq(spelling, CORE_STRING_STATIC("|")))
        op = IR_OP_BITSET_UNION;
    else if (flagset && core_string_eq(spelling, CORE_STRING_STATIC("^")))
        op = IR_OP_BITSET_XOR;
    else if (operator) op = operator->op;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("==")))
        op = IR_OP_EQ;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("!=")))
        op = IR_OP_NE;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("<")))
        op = IR_OP_LT;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("<=")))
        op = IR_OP_LE;
    else if (core_string_eq(spelling, CORE_STRING_STATIC(">")))
        op = IR_OP_GT;
    else if (core_string_eq(spelling, CORE_STRING_STATIC(">=")))
        op = IR_OP_GE;
    else return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                            "this binary operator is not lowered yet");

    bool comparison = op == IR_OP_EQ || op == IR_OP_NE || op == IR_OP_LT
        || op == IR_OP_LE || op == IR_OP_GT || op == IR_OP_GE;
    bool equality = op == IR_OP_EQ || op == IR_OP_NE;
    th_ctx *types = ir_module_types(context->module);
    bool left_nil = th_is_nil(operands[0]->type);
    bool right_nil = th_is_nil(operands[1]->type);
    if (equality && left_nil != right_nil) {
        ir_value_t *option = left_nil ? operands[1] : operands[0];
        if (!n00bb_option_type_element(option->type, nullptr))
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "nil can only be compared with an option value");
        ir_instr_t *test = ir_lower_emit_instruction(context,
                                                     IR_OP_OPTION_IS_NIL,
                                                     span);
        if (!test || !ir_lower_set_temp_dest(context, test,
                                             "__n00bb_", result_type)
            || !ir_instr_operand_value(test, option))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not lower option comparison with nil");
        if (op == IR_OP_EQ)
            return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
        ir_instr_t *false_value = ir_lower_emit_instruction(context,
                                                             IR_OP_CONST,
                                                             span);
        bool constant_ok = false_value
            && ir_lower_set_temp_dest(context, false_value,
                                      "__n00bb_", result_type)
            && ir_instr_operand_int(false_value, 0);
        ir_instr_t *negated = constant_ok
            ? ir_lower_emit_instruction(context, IR_OP_EQ, span) : nullptr;
        if (!negated || !ir_lower_set_temp_dest(context, negated,
                                                "__n00bb_", result_type)
            || !ir_instr_operand_value(negated, test->dest)
            || !ir_instr_operand_value(negated, false_value->dest))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not lower option inequality with nil");
        return (ir_lower_result_t){IR_LOWER_VALUE, negated->dest};
    }
    if (left_nil && right_nil && equality) {
        ir_instr_t *same = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
        bool is_equal = op == IR_OP_EQ;
        if (!same || !ir_lower_set_temp_dest(context, same,
                                             "__n00bb_", result_type)
            || !ir_instr_operand_int(same, is_equal ? 1 : 0))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not lower nil equality");
        return (ir_lower_result_t){IR_LOWER_VALUE, same->dest};
    }
    ir_value_t *promoted[2] = {
        n00bb_promoted_operand(context, core_tree_child(visit->node, 0),
                               operands[0], comparison, span),
        n00bb_promoted_operand(context, core_tree_child(visit->node, 2),
                               operands[1], comparison, span),
    };
    if (!promoted[0] || !promoted[1])
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    if (site && site->operator_dispatch
        && !primitive_operator_site(caller, site))
        return comparison
            ? n00bb_lower_comparison(context, visit, operands,
                                     result_type, span, spelling)
            : n00bb_lower_operator_method(context, visit, promoted, 2,
                                          result_type, span);

    ir_value_t *left = promoted[0];
    ir_value_t *right = promoted[1];
    th_type *left_value = n00bb_operand_value_type(left->type, right->type,
                                                   comparison);
    th_type *right_value = n00bb_operand_value_type(right->type, left->type,
                                                    comparison);
    left = ir_lower_value_as(context, left, left_value, span);
    right = right ? ir_lower_value_as(context, right, right_value, span)
                  : nullptr;
    if (!left || !right)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not read through an operator operand");
    bool shift = op == IR_OP_SHL || op == IR_OP_SHR;
    if (!shift && !ir_type_eq(context->module, left->type, right->type)) {
        ir_value_t *coerced = n00bb_typed_integer_literal(context,
            core_tree_child(visit->node, 2), left->type);
        if (coerced) right = coerced;
        if (!ir_type_eq(context->module, left->type, right->type)) {
            coerced = n00bb_typed_integer_literal(context,
                core_tree_child(visit->node, 0), right->type);
            if (coerced) left = coerced;
        }
    }
    if (shift && !n00bb_bitwise_type(left->type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "built-in shifts require an unsigned or bit left operand");
    if (shift) {
        right = n00bb_lower_shift_count(context,
            core_tree_child(visit->node, 2), right, left->type, span);
        if (!right) return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    }
    if (!comparison
        && ((!shift && !th_unify_in_place(types, left->type, right->type))
            || !th_unify_in_place(types, result_type, left->type)))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "built-in operator operands and result have incompatible types");

    ir_value_t *value = n00bb_emit_binary_ir(context, op, result_type,
                                              left, right, span);
    if (!value)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a binary IR operation");
    return (ir_lower_result_t){IR_LOWER_VALUE, value};
}

static ir_lower_result_t
n00bb_lower_nil_literal(ir_lower_context_t *context, parse_visit_t *visit,
                        ir_value_t *const *operands, size_t operand_count,
                        th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)operands;
    (void)state;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    if (tu && visit && n00bb_node_literal_site(visit->node))
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    th_type *flagset_type = nullptr;
    for (parse_tree_t *parent = visit && visit->node
            ? core_tree_node_value(visit->node).parent : nullptr;
         lower && parent; parent = core_tree_node_value(parent).parent) {
        if (!nt_is(visit->grammar, parent, "sym-decl")
            && !nt_is(visit->grammar, parent, "assign-stmt")) continue;
        const parse_rule_t *rule = parse_tree_rule(visit->grammar, parent);
        parse_annotation_t *assignment = rule
            ? parse_rule_find_annot(visit->grammar, rule,
                                    PARSE_ANNOT_ASSIGNS) : nullptr;
        parse_tree_t *target = assignment
            ? parse_tree_resolve_child_ref(visit->grammar, parent,
                                           assignment->name_ref) : nullptr;
        parse_tree_t *source = assignment
            ? parse_tree_resolve_child_ref(visit->grammar, parent,
                                           assignment->value_ref) : nullptr;
        if (!target || !source || !node_is_within(visit->node, source))
            continue;
        parse_sym_entry_t *symbol = attached_symbol(visit->grammar,
                                                     lower->unit, target);
        if (!symbol) symbol = core_tree_node_value(parent).sym;
        th_type *expected = symbol && symbol->type ? symbol->type
            : core_tree_is_leaf(target) ? nullptr
            : core_tree_node_value(target).type;
        n00bb_tu_state_t *caller = tu_state(lower->unit);
        if (caller && n00bb_flagset_type(lower->unit, caller, expected,
                                         nullptr)) flagset_type = expected;
        break;
    }
    if (flagset_type) {
        ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                          IR_OP_BITSET_CONST,
                                                          span);
        if (!constant || !ir_lower_set_temp_dest(context, constant,
                                                 "__n00bb_", flagset_type)
            || !ir_instr_operand_name(constant, "nil")
            || !ir_instr_operand_uint(constant, 0))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit an empty flagset value");
        return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
    }
    if (operand_count || !result_type
        || !th_is_nil(result_type))
        return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                           "nil literal must have the nil sentinel type");
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_NIL,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the nil sentinel");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_lower_result_t
n00bb_lower_typeof(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)state;
    if (operand_count != 1 || !operands || !operands[0] || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "typeof operand or descriptor type is incomplete");
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_TYPE_OF,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        || !ir_instr_operand_value(instruction, operands[0]))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit typeof expression");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static bool
n00bb_literal_fits_machine(ir_lit_t literal, ir_machine_t machine)
{
    if (machine.width == 0 || machine.width > 128) return false;
    if (machine.class == IR_CLASS_UINT)
        return !literal.neg && (machine.width == 128
            || literal.mag < ((ir_uint_t)1 << machine.width));
    if (machine.class != IR_CLASS_SINT) return false;
    ir_uint_t limit = (ir_uint_t)1 << (machine.width - 1);
    return literal.neg ? literal.mag <= limit : literal.mag < limit;
}

static ir_value_t *
n00bb_emit_value_instruction(ir_lower_context_t *context, ir_op_t op,
                             th_type *type, ir_value_t *left,
                             ir_value_t *right, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction
        || !ir_lower_set_temp_dest(context, instruction, "__n00bb_", type)
        || (left && !ir_instr_operand_value(instruction, left))
        || (right && !ir_instr_operand_value(instruction, right)))
        return nullptr;
    return instruction->dest;
}

static ir_lower_result_t
n00bb_lower_enum_from_value(ir_lower_context_t *context,
                            n00bb_lower_context_t *lower,
                            parse_sym_entry_t *enum_symbol,
                            ir_value_t *operand, th_type *target,
                            ir_span_t span)
{
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    ir_machine_t machine = ir_machine_of(context->module, operand->type);
    ir_block_t *join = n00bb_branch_block(context, lower, "enum_cast_join");
    ir_block_t *failure = n00bb_branch_block(context, lower,
                                              "enum_cast_failed");
    ir_instr_t *trap = failure ? ir_instr_add(failure, IR_OP_TRAP) : nullptr;
    if (!join || !failure || !trap)
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the enum range check");
    trap->span = span;
    for (parse_sym_entry_t *member = enum_symbol->member_scope
             ? enum_symbol->member_scope->first_in_scope : nullptr;
         member; member = member->next_in_scope) {
        if (member->kind != PARSE_SYM_ENUM_CONST || !member->has_comptime_value
            || !member->comptime_value) continue;
        n00bb_enum_value_t *ordinal = member->comptime_value;
        ir_lit_t literal = {.mag = ordinal->magnitude, .neg = ordinal->negative};
        if (!n00bb_literal_fits_machine(literal, machine)) continue;
        ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                         span);
        if (!constant
            || !ir_lower_set_temp_dest(context, constant, "__n00bb_",
                                       operand->type)
            || !ir_instr_operand_int_lit(constant, literal))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the enum member value");
        ir_value_t *equal = n00bb_emit_value_instruction(context, IR_OP_EQ,
            bool_type, operand, constant->dest, span);
        ir_block_t *next = n00bb_branch_block(context, lower,
                                               "enum_cast_test");
        ir_instr_t *branch = equal && next
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        if (!branch || !ir_instr_operand_value(branch, equal)
            || !ir_instr_operand_block(branch, join)
            || !ir_instr_operand_block(branch, next))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the enum membership test");
        branch->span = span;
        context->block = next;
    }
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, failure))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the enum range check");
    jump->span = span;
    context->block = join;
    ir_value_t *converted = n00bb_emit_value_instruction(context,
        IR_OP_ENUM_FROM_VALUE, target, operand, nullptr, span);
    if (!converted)
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the integer-to-enum conversion");
    return (ir_lower_result_t){IR_LOWER_VALUE, converted};
}

static ir_lower_result_t
n00bb_lower_cast(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    parse_tree_t *spec = child_nt(visit->grammar, visit->node, "type-spec");
    th_type *target = spec ? core_tree_node_value(spec).type : nullptr;
    if (operand_count != 1 || !operands || !operands[0] || !result_type
        || !target || !ir_type_eq(context->module, target, result_type))
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "cast target and expression type are incomplete");
    if (ir_type_eq(context->module, operands[0]->type, target)) {
        if (operands[0]->owner == context->function)
            return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
        ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                     span);
        if (!copy || !ir_lower_set_temp_dest(context, copy, "__n00bb_cast_",
                                             target)
            || !ir_instr_operand_value(copy, operands[0]))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit an identity cast");
        return (ir_lower_result_t){IR_LOWER_VALUE, copy->dest};
    }
    th_ctx *types = ir_module_types(context->module);
    th_type *from = operands[0]->type;
    bool unsafe_cast = parse_tree_leaf_is(core_tree_child(visit->node, 0),
                                          CORE_STRING_STATIC("unsafe_cast"));
    uint8_t source_attrs = 0, target_attrs = 0;
    bool plain_reference_cast = th_ind_attrs(from, &source_attrs)
        && th_ind_attrs(target, &target_attrs)
        && source_attrs == 0 && target_attrs == 0;
    if (unsafe_cast && is_integer_type(from) && is_integer_type(target)) {
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST,
                                                     span);
        if (!cast || !ir_lower_set_temp_dest(context, cast, "__n00bb_",
                                             target)
            || !ir_instr_operand_value(cast, operands[0])
            || !ir_instr_add_mod(cast, "wrap"))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit an unchecked integer cast");
        return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
    }
    if (unsafe_cast && !plain_reference_cast)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "unsafe_cast requires plain references or integers");
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    parse_sym_entry_t *from_flagset = nullptr;
    parse_sym_entry_t *to_flagset = nullptr;
    bool source_flagset = caller && n00bb_flagset_type(lower->unit, caller,
        from, &from_flagset);
    bool target_flagset = caller && n00bb_flagset_type(lower->unit, caller,
        target, &to_flagset);
    parse_sym_entry_t *flagset_symbol = source_flagset
        ? from_flagset : target_flagset ? to_flagset : nullptr;
    if (flagset_symbol
        && ((source_flagset && is_integer_type(target))
            || (target_flagset && is_integer_type(from)))) {
        const n00bb_flagset_info_t *info = parse_sym_binding(flagset_symbol,
            CORE_STRING_STATIC("n00bb.flagset-info"));
        th_type *integer_type = source_flagset ? target : from;
        ir_machine_t integer = ir_machine_of(context->module, integer_type);
        bool sufficient = info
            && ((integer.class == IR_CLASS_UINT
                    && integer.width >= info->atomic_count)
                || (integer.class == IR_CLASS_SINT
                    && integer.width > info->atomic_count));
        if (!sufficient)
            return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                "integer type is too narrow to represent every flagset value");
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
            source_flagset ? IR_OP_BITSET_VALUE
                           : IR_OP_BITSET_FROM_VALUE, span);
        if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                                   "__n00bb_", target)
            || !ir_instr_operand_value(conversion, operands[0])
            || !ir_instr_add_mod(conversion, "trap"))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit the flagset integer conversion");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    parse_sym_entry_t *enum_symbol = caller
        ? n00bb_enum_type_symbol(caller->link_analysis, from) : nullptr;
    if (enum_symbol && is_integer_type(target)) {
        const n00bb_enum_range_t *range = parse_sym_binding(enum_symbol,
            CORE_STRING_STATIC("n00bb.enum-range"));
        ir_machine_t destination = ir_machine_of(context->module, target);
        bool represents_range = range && (range->is_signed
            ? destination.class == IR_CLASS_SINT
                && destination.width >= range->magnitude_bits + 1
            : (destination.class == IR_CLASS_UINT
                    && destination.width >= range->magnitude_bits)
                || (destination.class == IR_CLASS_SINT
                    && destination.width > range->magnitude_bits));
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
                                                            IR_OP_ENUM_VALUE,
                                                            span);
        if (!range || !conversion
            || !ir_lower_set_temp_dest(context, conversion,
                                       "__n00bb_", target)
            || !ir_instr_operand_value(conversion, operands[0])
            || (!represents_range && !ir_instr_add_mod(conversion, "trap")))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the semantic enum-to-integer conversion");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    parse_sym_entry_t *target_enum = caller
        ? n00bb_enum_type_symbol(caller->link_analysis, target) : nullptr;
    if (target_enum && is_integer_type(from))
        return n00bb_lower_enum_from_value(context, lower, target_enum,
                                           operands[0], target, span);
    th_type *from_definition = th_type_definition(types, from);
    th_type *to_definition = th_type_definition(types, target);
    if (unsafe_cast) {
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_BITCAST,
                                                     span);
        ir_alias_target_t alias = {
            .value = operands[0],
            .escapes = n00bb_tree_returns_value(visit->grammar, visit->node),
        };
        if (!cast || !ir_lower_set_temp_dest(context, cast,
                                             "__n00bb_", target)
            || !ir_instr_operand_value(cast, operands[0])
            || !ir_value_set_alias_targets(lower->ir_context, cast->dest,
                                           &alias, 1, true)
            || (ir_value_has_external_writable_storage(operands[0])
                && !ir_value_set_external_writable_storage(lower->ir_context,
                                                             cast->dest)))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not preserve unsafe reference cast provenance");
        return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
    }
    bool boxing = th_builtin_kind(types, target) == TH_BUILTIN_ANY;
    bool nominal = (from_definition
            && ir_type_eq(context->module, from_definition, target))
        || (to_definition
            && ir_type_eq(context->module, to_definition, from));
    if (boxing || nominal) {
        parse_tree_t *operand_node = core_tree_child(visit->node, 4);
        if (!n00bb_attach_expression_shape(context, visit->grammar,
                                          operand_node, operands[0]))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not retain the cast operand shape");
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
            boxing ? IR_OP_BOX : IR_OP_NOMINAL_CAST, span);
        if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                                   "__n00bb_", target)
            || !ir_instr_operand_value(conversion, operands[0])
            || !ir_value_copy_alias_targets(lower->ir_context,
                conversion->dest, operands[0]))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not retain semantic cast information");
        if (boxing && th_kind_of(th_resolve(from)) == TH_INDIRECTION
            && !ir_value_add_alias_target(lower->ir_context, conversion->dest,
                (ir_alias_target_t){.value = operands[0],
                    .escapes = n00bb_tree_returns_value(visit->grammar,
                                                        visit->node)}))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not retain boxed reference storage");
        if (ir_value_shape(operands[0])
            && !ir_value_set_shape(conversion->dest, ir_value_shape(operands[0])))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not retain cast array shape");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    if (from_definition || to_definition)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "this cast has no defined conversion");
    ir_machine_t source = ir_machine_of(context->module, operands[0]->type);
    ir_machine_t destination = ir_machine_of(context->module, target);
    bool source_integer = source.class == IR_CLASS_SINT
        || source.class == IR_CLASS_UINT;
    bool target_integer = destination.class == IR_CLASS_SINT
        || destination.class == IR_CLASS_UINT;
    bool source_float = source.class == IR_CLASS_FLOAT;
    bool target_float = destination.class == IR_CLASS_FLOAT;
    if (ir_type_eq(context->module, from, th_name(types, "byte")))
        target_integer = is_integer_type(target);
    bool float_to_float = source_float && target_float;
    if (!(source_integer && target_integer)
        && !(source_integer && target_float)
        && !(source_float && target_integer)
        && !float_to_float)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "this cast has no defined conversion");
    bool signedness_change = source.class != destination.class;
    bool needs_check = (source_float && !float_to_float)
        || (source_integer && target_integer
            && (destination.width < source.width || signedness_change));
    const char *rounding = float_to_float
        ? destination.width < source.width ? "nearest" : nullptr
        : source_float ? "to_zero" : target_float ? "nearest" : nullptr;
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (!cast || !ir_lower_set_temp_dest(context, cast,
                                         "__n00bb_", target)
        || !ir_instr_operand_value(cast, operands[0])
        || (needs_check && !ir_instr_add_mod(cast, "trap"))
        || (rounding && !ir_instr_add_mod(cast, rounding)))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a checked cast");
    return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
}

static ir_lower_result_t
n00bb_lower_bool_not(ir_lower_context_t *context, parse_visit_t *visit,
                     ir_value_t *const *operands, size_t operand_count,
                     th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    if (caller && operand_count == 1 && operands && operands[0]
        && result_type && n00bb_flagset_type(lower->unit, caller,
                                             operands[0]->type, nullptr)) {
        th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
        ir_instr_t *test = bool_type
            && ir_type_eq(context->module, result_type, bool_type)
                ? ir_lower_emit_instruction(context, IR_OP_BITSET_IS_EMPTY,
                                            span)
                : nullptr;
        if (!test || !ir_lower_set_temp_dest(context, test,
                                             "__n00bb_", result_type)
            || !ir_instr_operand_value(test, operands[0]))
            return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                               "flagset ! requires a flagset operand and bool result");
        return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
    }
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    if (site && site->operator_dispatch
        && !primitive_operator_site(caller, site)) {
        if (operand_count != 1 || !operands || !operands[0] || !result_type)
            return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                               "logical negation has incomplete operands or type");
        return n00bb_lower_operator_method(context, visit, operands, 1,
                                           result_type, span);
    }
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    if (operand_count != 1 || !operands || !operands[0] || !result_type
        || !bool_type
        || !ir_type_eq(context->module, operands[0]->type, bool_type)
        || !ir_type_eq(context->module, result_type, bool_type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "logical negation requires a bool operand");
    ir_instr_t *false_value = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                         span);
    if (!false_value
        || !ir_lower_set_temp_dest(context, false_value,
                                   "__n00bb_", bool_type)
        || !ir_instr_operand_int(false_value, 0))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit false for logical negation");
    ir_instr_t *equal = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    if (!equal || !ir_lower_set_temp_dest(context, equal,
                                          "__n00bb_", bool_type)
        || !ir_instr_operand_value(equal, operands[0])
        || !ir_instr_operand_value(equal, false_value->dest))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit logical negation");
    return (ir_lower_result_t){IR_LOWER_VALUE, equal->dest};
}

static ir_lower_result_t
n00bb_lower_complement(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               ir_value_t *const *operands,
                               size_t operand_count, th_type *result_type,
                               ir_span_t span, void *state)
{
    (void)visit;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    if (!caller || operand_count != 1 || !operands || !operands[0]
        || !result_type
        || !ir_type_eq(context->module, operands[0]->type, result_type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "~ requires a flagset or integer operand");
    bool flagset = n00bb_flagset_type(lower->unit, caller,
                                     operands[0]->type, nullptr);
    if (!flagset && !n00bb_bitwise_type(operands[0]->type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "~ requires a flagset or unsigned integer operand; use "
            "unsafe_not for signed integers");
    ir_instr_t *instruction = ir_lower_emit_instruction(context, flagset
        ? IR_OP_BITSET_COMPLEMENT : IR_OP_NOT, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        || !ir_instr_operand_value(instruction, operands[0]))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit complement");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_lower_result_t
n00bb_lower_negate(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    bool negative = false;
    bool saw_sign = false;
    ir_lit_t magnitude = {0};
    parse_tree_t *literal = lower && lower->grammar && visit
        ? n00bb_simple_switch_literal(lower->grammar, visit->node,
                                      &negative, &saw_sign) : nullptr;
    bool folded_literal = literal && negative && saw_sign
        && !literal_modifier(lower->grammar, literal).data
        && n00bb_parse_switch_integer(first_token_text(literal), &magnitude);
    if (folded_literal && operand_count == 1 && operands && operands[0])
        return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
    if (operand_count != 1 || !operands || !operands[0] || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "unary negation has incomplete operands or type");
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    if (site && site->operator_dispatch
        && !primitive_operator_site(caller, site))
        return n00bb_lower_operator_method(context, visit, operands, 1,
                                           result_type, span);
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_NEG,
                                                        span);
    ir_class_t operand_class = ir_machine_of(context->module,
                                             operands[0]->type).class;
    bool checked_integer = operand_class == IR_CLASS_SINT
        || operand_class == IR_CLASS_UINT;
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        || !ir_instr_operand_value(instruction, operands[0])
        || (checked_integer && !ir_instr_add_mod(instruction, "trap")))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit unary negation");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static bool
n00bb_mark_builtin_index_receiver(ir_lower_context_t *context,
                                  parse_visit_t *visit, bool entering,
                                  void *state)
{
    (void)state;
    if (!entering || !visit->node || !visit->annot
        || !core_string_eq(visit->annot->emit_action,
                           CORE_STRING_STATIC("call"))) return true;
    core_string_t trait = {0}, method = {0};
    if (!index_dispatch_for_node(visit->grammar, visit->node,
                                 &trait, &method)) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = tu
        ? link_use_site_for_dispatch(tu, visit->node, trait) : nullptr;
    if (!site || !n00bb_builtin_array_read_function(site->symbol))
        return true;
    parse_tree_t *receiver = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    for (parse_tree_t *member = member_access_node(visit->grammar, receiver);
         member; member = member_access_node(visit->grammar,
                                             core_tree_child(member, 0)))
        if (n00bb_option_type_element(
                core_tree_node_value(core_tree_child(member, 0)).type,
                nullptr)) return true;
    parse_tree_t *member = member_access_node(visit->grammar, receiver);
    while (member) {
        if (!ir_lower_suppress_emit(context, member)) return false;
        receiver = core_tree_child(member, 0);
        member = member_access_node(visit->grammar, receiver);
    }
    return true;
}

static bool
n00bb_skip_i128_min_magnitude(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state)
{
    (void)state;
    if (entering || !visit->annot || !visit->node
        || !core_string_eq(visit->annot->emit_action,
                           CORE_STRING_STATIC("literal"))) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower || !lower->grammar
        || !nt_is(lower->grammar, visit->node, "simple-lit")) return true;
    bool negative = false;
    bool saw_sign = false;
    ir_lit_t magnitude = {0};
    parse_tree_t *literal = nullptr;
    for (parse_tree_t *ancestor = core_tree_node_value(visit->node).parent;
         ancestor && !literal;
         ancestor = core_tree_node_value(ancestor).parent) {
        if (!nt_is(lower->grammar, ancestor, "unary-expr")) continue;
        bool candidate_negative = false;
        bool candidate_saw_sign = false;
        parse_tree_t *candidate = n00bb_simple_switch_literal(
            lower->grammar, ancestor, &candidate_negative,
            &candidate_saw_sign);
        if (!candidate || !candidate_saw_sign) continue;
        literal = candidate;
        negative = candidate_negative;
        saw_sign = candidate_saw_sign;
        break;
    }
    if (!literal || !negative || !saw_sign
        || literal_modifier(lower->grammar, literal).data
        || !n00bb_parse_switch_integer(first_token_text(literal), &magnitude))
        return true;
    th_type *type = core_tree_node_value(visit->node).type;
    if (!type) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_lower_set_temp_dest(context, constant, "__n00bb_",
                                              type)
        || !ir_instr_operand_int_lit(constant, (ir_lit_t){
            .mag = magnitude.mag, .neg = true})) return false;
    return ir_lower_set_node_value(context, visit->node, constant->dest);
}

static bool
collect_sequence_literal_values(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *node,
                                core_list_t(n00bb_ir_value_ptr_t) *values)
{
    if (!node) return true;
    parse_tree_t *entries = nullptr;
    if (nt_is(visit->grammar, node, "list-lit")) {
        entries = child_nt(visit->grammar, node, "list-elems");
    }
    else if (nt_is(visit->grammar, node, "dict-or-set-lit")) {
        entries = child_nt(visit->grammar, node, "set-entries");
        if (!entries)
            entries = child_nt(visit->grammar, node, "dict-entries");
    }
    else return false;
    if (!entries) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_ordered_nodes_t *cached = tu
        ? n00bb_node_ordered_nodes(entries) : nullptr;
    if (!cached) return false;
    for (size_t i = 0; i < cached->count; i++) {
        ir_value_t *value = ir_lower_node_value(context, cached->nodes[i]);
        if (!value) return false;
        core_list_push(*values, value);
    }
    return true;
}

static n00bb_tuple_parts_t *
n00bb_tuple_parts(n00bb_lower_context_t *lower, parse_tree_t *node,
                 bool create)
{
    if (!lower || !node) return nullptr;
    for (size_t i = 0; i < lower->tuple_parts.len; i++)
        if (lower->tuple_parts.data[i].node == node)
            return &lower->tuple_parts.data[i];
    if (!create) return nullptr;
    if (!lower->tuple_parts.data)
        lower->tuple_parts = core_list_new(n00bb_tuple_parts_t);
    core_list_push(lower->tuple_parts, ((n00bb_tuple_parts_t){
        .node = node,
        .expressions = core_list_new(parse_tree_ptr_t),
        .values = core_list_new(n00bb_ir_value_ptr_t),
    }));
    return &lower->tuple_parts.data[lower->tuple_parts.len - 1];
}

static bool
n00bb_lower_tuple_parts(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, "expr-comma-list"))
        return true;
    n00bb_lower_context_t *lower = context->user;
    size_t child_count = core_tree_num_children(visit->node);
    if (child_count != 1 && child_count != 3)
        return lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, visit->node),
            "tuple expression list has an unsupported grammar shape").status
            != IR_LOWER_FAILED;
    parse_tree_t *head = core_tree_child(visit->node, 0);
    ir_value_t *head_value = ir_lower_node_value(context, head);
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    if (!parts) return false;
    core_list_push(parts->expressions, head);
    core_list_push(parts->values, head_value);
    if (child_count == 3) {
        parse_tree_t *tail_node = core_tree_child(visit->node, 2);
        n00bb_tuple_parts_t *tail = n00bb_tuple_parts(lower, tail_node, false);
        if (!tail)
            return lower_error(context, IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(context, visit->node),
                "tuple expression list lost its lowered suffix").status
                != IR_LOWER_FAILED;
        for (size_t i = 0; i < tail->expressions.len; i++) {
            core_list_push(parts->expressions, tail->expressions.data[i]);
            core_list_push(parts->values, tail->values.data[i]);
        }
    }
    return true;
}

static bool
n00bb_lower_named_field_parts(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node,
                           "named-field-list")) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    if (!parts) return false;
    for (size_t i = 0; i < core_tree_num_children(visit->node); i++) {
        parse_tree_t *child = core_tree_child(visit->node, i);
        if (nt_is(visit->grammar, child, "named-field-list")) {
            n00bb_tuple_parts_t *tail = n00bb_tuple_parts(lower, child, false);
            if (!tail) return false;
            for (size_t j = 0; j < tail->expressions.len; j++) {
                core_list_push(parts->expressions, tail->expressions.data[j]);
                core_list_push(parts->values, tail->values.data[j]);
            }
        }
        else if (nt_is(visit->grammar, child, "named-field")) {
            parse_tree_t *expression = nullptr;
            for (size_t j = 0; j < core_tree_num_children(child); j++) {
                parse_tree_t *part = core_tree_child(child, j);
                if (nt_is(visit->grammar, part, "expression")) {
                    expression = part;
                    break;
                }
            }
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression) : nullptr;
            if (!expression || !value) return false;
            core_list_push(parts->expressions, expression);
            core_list_push(parts->values, value);
        }
    }
    return parts->values.len != 0;
}

static ir_lower_result_t
n00bb_lower_user_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         n00bb_litmod_site_t *site, th_type *result_type,
                         ir_span_t span);
static ir_lower_result_t
n00bb_lower_literal_element(ir_lower_context_t *context, ir_value_t *value,
                            th_type *target, ir_span_t span);

static bool
n00bb_lower_tuple_literal(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, "tuple-or-paren"))
        return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    if (site) {
        if ((lower->has_active_literal
                && lower->active_literal.node == visit->node)
            || ir_lower_node_value(context, visit->node)) return true;
        ir_lower_result_t result = n00bb_lower_user_literal(context, visit,
            site, core_tree_node_value(visit->node).type,
            lower_span_for_tree(context, visit->node));
        return result.status == IR_LOWER_VALUE && result.value
            && ir_lower_set_node_value(context, visit->node, result.value);
    }
    size_t child_count = core_tree_num_children(visit->node);
    parse_tree_t *named_fields = child_nt(visit->grammar, visit->node,
                                          "named-field-list");
    if (named_fields) {
        bool compound = false;
        if (n00bb_assignment_target(visit->grammar, visit->node, &compound))
            return true;
        n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, named_fields,
                                                       false);
        th_type *type = core_tree_node_value(visit->node).type;
        if (!parts || !parts->values.len || !type)
            return lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "named-field literal is missing its inferred type or values")
                .status != IR_LOWER_FAILED;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        bool variant = false;
        (void)th_named_info(th_resolve(type), nullptr, nullptr, &variant,
                            nullptr);
        if (variant) {
            n00bb_ordered_nodes_t *ordered = tu
                ? n00bb_node_ordered_nodes(named_fields) : nullptr;
            bool valid = parts->values.len == 1 && ordered
                && ordered->count == 1;
            core_string_t arm = valid
                ? first_token_text(ordered->nodes[0]) : (core_string_t){0};
            if (!arm.data)
                return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                    "variant literal requires exactly one named arm")
                    .status != IR_LOWER_FAILED;
            ir_instr_t *make = ir_lower_emit_instruction(context,
                                                          IR_OP_VARIANT_MAKE,
                                                          span);
            bool ok = make && ir_lower_set_temp_dest(context, make,
                "__n00bb_variant_", type)
                && ir_instr_operand_name(make, arm.data)
                && ir_instr_operand_value(make, parts->values.data[0])
                && ir_value_copy_alias_targets(lower->ir_context,
                    make->dest, parts->values.data[0]);
            if (!ok)
                return lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower the selected variant arm")
                    .status != IR_LOWER_FAILED;
            return ir_lower_set_node_value(context, visit->node, make->dest);
        }
        th_type *definition = th_kind_of(th_resolve(type)) == TH_RECORD
            ? th_resolve(type)
            : th_type_definition(ir_module_types(context->module), type);
        for (size_t i = 0; definition && i < parts->values.len; i++) {
            parse_tree_t *field = core_tree_node_value(
                parts->expressions.data[i]).parent;
            core_string_t name = field ? first_token_text(field)
                                       : (core_string_t){0};
            th_type *field_type = name.data
                ? th_record_lookup(definition, name.data) : nullptr;
            if (!field_type) continue;
            ir_lower_result_t promoted = n00bb_lower_literal_element(context,
                parts->values.data[i], field_type, span);
            if (promoted.status != IR_LOWER_VALUE || !promoted.value)
                return false;
            parts->values.data[i] = promoted.value;
        }
        ir_instr_t *aggregate = ir_lower_emit_instruction(context,
                                                           IR_OP_AGGREGATE,
                                                           span);
        bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
                                                       "__n00bb_", type);
        for (size_t i = 0; ok && i < parts->values.len; i++)
            ok = ir_instr_operand_value(aggregate, parts->values.data[i]);
        if (!ok)
            return lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not emit named-field aggregate")
                .status != IR_LOWER_FAILED;
        return ir_lower_set_node_value(context, visit->node, aggregate->dest);
    }
    if (child_count == 3
        && nt_is(visit->grammar, core_tree_child(visit->node, 1),
                 "expression")) {
        bool compound = false;
        if (n00bb_assignment_target(visit->grammar, visit->node, &compound))
            return true;
        ir_value_t *value = ir_lower_node_value(context,
                                                core_tree_child(visit->node, 1));
        return value && ir_lower_set_node_value(context, visit->node, value);
    }
    if (child_count != 5
        || !nt_is(visit->grammar, core_tree_child(visit->node, 1),
                  "expression")) return true;
    bool compound = false;
    if (n00bb_assignment_target(visit->grammar, visit->node, &compound))
        return true;
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    parse_tree_t *first = core_tree_child(visit->node, 1);
    parse_tree_t *rest_node = core_tree_child(visit->node, 3);
    n00bb_tuple_parts_t *rest = n00bb_tuple_parts(lower, rest_node, false);
    ir_value_t *first_value = ir_lower_node_value(context, first);
    th_type *type = core_tree_node_value(visit->node).type;
    if (!parts || !rest || !first_value || !type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, visit->node),
            "tuple literal is missing an inferred type or lowered element")
            .status != IR_LOWER_FAILED;
    core_list_push(parts->expressions, first);
    core_list_push(parts->values, first_value);
    for (size_t i = 0; i < rest->expressions.len; i++) {
        core_list_push(parts->expressions, rest->expressions.data[i]);
        core_list_push(parts->values, rest->values.data[i]);
    }
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_instr_t *aggregate = ir_lower_emit_instruction(context,
                                                       IR_OP_AGGREGATE, span);
    bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
                                                   "__n00bb_", type);
    for (size_t i = 0; ok && i < parts->values.len; i++) {
        if (!parts->values.data[i]) ok = false;
        else ok = ir_instr_operand_value(aggregate, parts->values.data[i]);
    }
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not emit tuple aggregate").status != IR_LOWER_FAILED;
    return ir_lower_set_node_value(context, visit->node, aggregate->dest);
}

static ir_lower_result_t
n00bb_lower_char_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_value_t *const *operands, size_t operand_count,
                         th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    parse_token_info_t *token = visit && visit->node
        ? parse_tree_find_first_terminal(visit->node) : nullptr;
    uint32_t scalar = 0;
    if (operand_count || !result_type || !token
        || !core_option_is_set(token->value)
        || !n00bb_decode_char_literal(core_option_get(token->value), &scalar))
        return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                           "character literal has no valid scalar value");
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_lower_set_temp_dest(context, constant, "__n00bb_",
                                              result_type)
        || !ir_instr_operand_uint(constant, scalar))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit character literal");
    return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
}

static ir_lower_result_t
n00bb_lower_string_literal(ir_lower_context_t *context,
                           parse_visit_t *visit,
                           ir_value_t *const *operands,
                           size_t operand_count, th_type *result_type,
                           ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
    if (operand_count || !token || !core_option_is_set(token->value))
        return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                           "string literal has no decoded token value");
    core_string_t spelling = core_option_get(token->value);
    core_string_t modifier = literal_modifier(visit->grammar, visit->node);
    const n00bb_literal_registration_t *registration = modifier.data
        ? n00bb_literal_handler(modifier, N00BB_LITERAL_STRING_SHAPE)
        : nullptr;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *user_site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    if (user_site)
        return n00bb_lower_user_literal(context, visit, user_site,
                                        result_type, span);
    bool rich = registration
        && registration->handler == N00BB_LITERAL_RICH;
    if (modifier.data && !registration && !user_site)
        return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
            n00bb_literal_prefix_registered(modifier)
                ? "literal modifier does not support a string payload"
                : "no literal handler is registered for this string payload shape");
    if (!result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "string literal has no inferred result type");
    core_buffer_t *decoded = core_buffer_empty();
    core_buffer_t *rich_metadata = nullptr;
    bool ok = decoded != nullptr;
    if (ok && rich) {
        n00bb_rich_string_error_t error;
        core_buffer_t *markup_text = nullptr;
        ok = n00bb_rich_string_parse(spelling, &markup_text,
                                     &rich_metadata, &error);
        core_buffer_free(decoded);
        decoded = markup_text;
        if (!ok) {
            const char *message = error == N00BB_RICH_STRING_BAD_ESCAPE
                ? "rich string contains an invalid escape sequence"
                : error == N00BB_RICH_STRING_UNCLOSED_ASCII_TAG
                    ? "rich string contains an unclosed ASCII tag"
                : error == N00BB_RICH_STRING_MIXED_NUMBERING
                    ? "rich string cannot mix automatic and explicit positional substitutions"
                : error == N00BB_RICH_STRING_INDEX_OVERFLOW
                    ? "rich string substitution index is too large"
                : error == N00BB_RICH_STRING_TOO_LARGE
                    ? "rich string exceeds the supported byte length"
                : "rich string could not be parsed";
            return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                               message);
        }
    }
    else if (ok) ok = n00bb_decode_string_literal(spelling, decoded);
    if (!ok) {
        core_buffer_free(decoded);
        return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                           "string literal has an invalid escape sequence");
    }
    th_type *resolved = th_resolve(result_type);
    th_type *element_type = th_kind_of(resolved) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(resolved)) : nullptr;
    bool char_array = n00bb_scalar_role_is(element_type, IR_ROLE_CHAR);
    if (char_array) {
        if (decoded->byte_len > UINT32_MAX) {
            core_buffer_free(decoded);
            core_buffer_free(rich_metadata);
            return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                               "u32 literal is too large to lower");
        }
        core_list_t(n00bb_ir_value_ptr_t) values =
            core_list_new(n00bb_ir_value_ptr_t);
        uint32_t position = 0;
        while (ok && position < decoded->byte_len) {
            int32_t codepoint = unicode_utf8_decode(decoded->data,
                (uint32_t)decoded->byte_len, &position);
            if (codepoint < 0) {
                ok = false;
                break;
            }
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                              IR_OP_CONST,
                                                              span);
            ok = constant
                && ir_lower_set_temp_dest(context, constant, "__n00bb_",
                                           element_type)
                && ir_instr_operand_uint(constant, (uint32_t)codepoint);
            if (ok) {
                core_list_push(values, constant->dest);
            }
        }
        ir_instr_t *aggregate = ok
            ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
            : nullptr;
        ok = ok && aggregate
            && ir_lower_set_temp_dest(context, aggregate, "__n00bb_",
                                      result_type);
        for (size_t i = 0; ok && i < values.len; i++)
            ok = ir_instr_operand_value(aggregate, values.data[i]);
        ir_shape_t *shape = ok
            ? ir_shape_new(((n00bb_lower_context_t *)context->user)->ir_context,
                           element_type, 1) : nullptr;
        ok = ok && shape && ir_shape_set_extent(shape, 0,
            (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_STATIC,
                .static_value = values.len,
            }) && ir_value_set_shape(aggregate->dest, shape);
        core_list_free(values);
        core_buffer_free(decoded);
        core_buffer_free(rich_metadata);
        if (!ok)
            return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                               "u32 literal contains invalid UTF-8 or could not be lowered");
        return (ir_lower_result_t){IR_LOWER_VALUE, aggregate->dest};
    }
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                        span);
    ok = instruction && ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", result_type)
        && ir_instr_set_const_bytes(instruction,
            (const uint8_t *)decoded->data, decoded->byte_len);
    if (ok && rich_metadata) {
        n00bb_lower_context_t *lower = context->user;
        ok = lower && lower->ir_context
            && ir_annot_set(lower->ir_context, &instruction->annots,
                (ir_annot_t){
                    .key = "n00bb.rich.metadata",
                    .kind = IR_ANNOT_STR,
                    .s = ir_strdup(lower->ir_context, rich_metadata->data),
                });
    }
    if (ok && registration && lower && lower->ir_context)
        ok = ir_annot_set(lower->ir_context, &instruction->annots,
            (ir_annot_t){
                .key = "n00bb.literal.modifier",
                .kind = IR_ANNOT_STR,
                .s = ir_strdup(lower->ir_context,
                               registration->canonical_name),
            });
    core_buffer_free(decoded);
    core_buffer_free(rich_metadata);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a byte-exact string constant");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_lower_result_t
n00bb_lower_literal_element(ir_lower_context_t *context, ir_value_t *value,
                            th_type *target, ir_span_t span)
{
    if (!value || !target)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "literal element has no value or target type");
    ir_value_t *materialized = n00bb_materialize(context, nullptr, value,
                                                 target, span);
    if (!materialized)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not materialize the literal element");
    if (!ir_type_eq(context->module, materialized->type, target))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                           "literal element does not match its target type");
    return (ir_lower_result_t){IR_LOWER_VALUE, materialized};
}


static ir_lower_result_t
n00bb_lower_literal_constructor(ir_lower_context_t *context,
                                th_type *result_type, ir_value_t *seed,
                                parse_tree_t *literal, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = caller ? caller->link_analysis : nullptr;
    th_ctx *types = ir_module_types(context->module);
    parse_sym_entry_t *selected = nullptr;
    core_translation_unit_t *owner = nullptr;
    th_type *signature = nullptr;
    if (!analysis || !result_type || !seed)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "literal constructor has no link context or seed");
    th_ctx *probe = th_ctx_new();
    core_buffer_t constraint_failure = {0};
    if (!probe || !analysis->environment
        || !th_ctx_set_environment(probe, analysis->environment,
                lower->unit->module.identifier.data,
                n00bb_package_id(lower->unit).data)) {
        th_ctx_free(probe);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not check the literal constructor contract");
    }
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION
                    || !core_string_eq(symbol->name,
                                       CORE_STRING_STATIC("__init"))
                    || !symbol_is_public_declaration(symbol)) continue;
                th_type *candidate = constructor_signature_instance(probe,
                                                                      symbol);
                th_type *receiver_type = candidate
                    ? th_type_clone(probe, result_type) : nullptr;
                th_type *seed_type = candidate
                    ? th_type_clone(probe, seed->type) : nullptr;
                const th_param *self = candidate
                    && th_kind_of(candidate) == TH_FUNCTION
                    && th_func_nparams(candidate) == 2
                    ? th_func_param(candidate, 0) : nullptr;
                const th_param *input = self
                    ? th_func_param(candidate, 1) : nullptr;
                if (!self || !input || self->mode != TH_INOUT
                    || input->mode != TH_IN
                    || !receiver_type || !seed_type
                    || !th_env_type_pattern_subsumes(self->type,
                                                     receiver_type))
                    continue;
                size_t errors_before = th_error_count(probe);
                if (!th_unify_in_place(probe, self->type, receiver_type)
                    || !th_unify_in_place(probe, input->type, seed_type)) {
                    th_error_t detail = {0};
                    if (!constraint_failure.data
                        && th_error_count(probe) > errors_before
                        && th_error_at(probe, th_error_count(probe) - 1,
                                       &detail)
                        && detail.constraint)
                        core_buffer_printf(&constraint_failure,
                            "literal constructor requires trait '%s' for %s",
                            detail.constraint,
                            detail.got
                                ? n00bb_print_type(probe, detail.got)
                                : n00bb_print_type(types, result_type));
                    continue;
                }
                if (selected) {
                    core_list_free(scopes);
                    th_ctx_free(probe);
                    core_buffer_release(&constraint_failure);
                    return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "multiple literal constructors match the seed");
                }
                selected = symbol;
                owner = unit;
                signature = th_type_clone(types,
                    th_apply_subst(probe, candidate));
                if (!signature) {
                    core_list_free(scopes);
                    th_ctx_free(probe);
                    core_buffer_release(&constraint_failure);
                    return lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not retain the literal constructor signature");
                }
            }
        }
        core_list_free(scopes);
    }
    th_ctx_free(probe);
    if (!selected) {
        ir_lower_result_t error = lower_error(context,
            constraint_failure.data ? IR_DIAG_TYPE_ARGS
                                    : IR_DIAG_UNDEFINED_SYMBOL,
            span, constraint_failure.data ? constraint_failure.data
                : "no visible literal constructor accepts the seed");
        core_buffer_release(&constraint_failure);
        return error;
    }
    core_buffer_release(&constraint_failure);
    if (!parse_sym_add_use(selected, literal))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not record the literal constructor use");
    n00bb_tu_state_t *owner_state = tu_state(owner);
    ir_func_t *function = link_ir_function_for_symbol(analysis, selected);
    ir_module_t *module = unit_module(owner_state);
    ir_value_t *callee = module && function
        ? ir_func_address(module, function) : nullptr;
    th_type *address_type = n00bb_indirect_type(types, result_type);
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    bool ok = callee && allocation
        && ir_lower_set_temp_dest(context, allocation,
                                  "__n00bb_literal_storage_", address_type)
        && ir_instr_operand_type(allocation, result_type);
    ir_instr_t *call = ok
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    th_type *nil_type = th_name(types, "nil");
    ok = call && nil_type && ir_lower_set_temp_dest(context, call,
        "__n00bb_literal_init_", nil_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_fresh(call, allocation->dest)
        && ir_instr_operand_value(call, seed)
        && ir_instr_set_call_signature(call, signature);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the literal constructor call");
    ir_value_t *value = n00bb_emit_indirect_read(context, allocation->dest,
                                                 result_type, span);
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_error(context, IR_DIAG_INTERNAL, span,
                      "could not read the constructed literal");
}

static ir_lower_result_t
n00bb_lower_sequence_aggregate(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    if (!result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "sequence literal has no inferred type");
    th_type *resolved_type = th_resolve(result_type);
    bool is_list = th_builtin_applied(resolved_type, TH_BUILTIN_CONTAINER_LIST, 1)
        != nullptr;
    bool is_array = th_kind_of(resolved_type) == TH_SEQUENCE;
    if (!is_array && !is_list)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "bracket literal aggregate lowering requires a resolved array or list type");
    const th_arg *list_element = is_list
        ? th_named_arg(resolved_type, 0) : nullptr;
    th_type *element_type = is_array ? th_seq_elem(resolved_type)
        : list_element && list_element->kind == TH_ARG_TYPE
            ? list_element->type : nullptr;
    th_type *aggregate_type = is_list && element_type
        ? th_array(ir_module_types(context->module), element_type)
        : result_type;
    if (!aggregate_type || !element_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "sequence literal element type is unresolved");
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool collected = collect_sequence_literal_values(context, visit,
                                                      visit->node, &values);
    if (!collected) {
        core_list_free(values);
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "sequence literal element has no lowered value");
    }
    core_list_t(n00bb_ir_value_ptr_t) promoted_values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = true;
    for (size_t i = 0; ok && i < values.len; i++) {
        ir_lower_result_t promoted = n00bb_lower_literal_element(context,
            values.data[i], element_type, span);
        ok = promoted.status == IR_LOWER_VALUE && promoted.value;
        if (ok) core_list_push(promoted_values, promoted.value);
    }
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
        : nullptr;
    ok = instruction && ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", aggregate_type);
    if (ok) ok = ir_instr_set_aggregate_form(instruction,
                                             IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < promoted_values.len; i++)
        ok = ir_instr_operand_value(instruction, promoted_values.data[i]);
    th_type *element = th_resolve(aggregate_type);
    uint32_t rank = 0;
    while (ok && th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        if (!element || rank == UINT32_MAX) ok = false;
        else rank++;
    }
    ir_shape_t *shape = ok && rank
        ? ir_shape_new(((n00bb_lower_context_t *)context->user)->ir_context,
                       element, rank) : nullptr;
    ok = ok && (!rank || shape);
    for (uint32_t d = 0; ok && d < rank; d++) {
        ir_shape_extent_t extent = {.kind = IR_SHAPE_EXTENT_UNRESOLVED};
        if (!d) {
            extent.kind = IR_SHAPE_EXTENT_STATIC;
            extent.static_value = values.len;
        }
        else if (values.len) {
            ir_shape_t *first_shape = ir_value_shape(values.data[0]);
            bool common_static = first_shape != nullptr;
            ir_shape_extent_t first_extent = {0};
            common_static &= ir_shape_extent(first_shape, d - 1,
                                               &first_extent)
                && first_extent.kind == IR_SHAPE_EXTENT_STATIC;
            for (size_t i = 1; common_static && i < values.len; i++) {
                ir_shape_extent_t candidate = {0};
                ir_shape_t *child_shape = ir_value_shape(values.data[i]);
                common_static = child_shape
                    && ir_shape_extent(child_shape, d - 1, &candidate)
                    && candidate.kind == IR_SHAPE_EXTENT_STATIC
                    && candidate.static_value == first_extent.static_value;
            }
            if (common_static) extent = first_extent;
        }
        ok = ir_shape_set_extent(shape, d, extent);
    }
    if (ok && shape) ok = ir_value_set_shape(instruction->dest, shape);
    core_list_free(promoted_values);
    core_list_free(values);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a sequence aggregate");
    instruction->span = span;
    return is_list
        ? n00bb_lower_literal_constructor(context, result_type,
                                          instruction->dest, visit->node, span)
        : (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_lower_result_t
n00bb_lower_sequence_literal(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    return site
        ? n00bb_lower_user_literal(context, visit, site, result_type, span)
        : n00bb_lower_sequence_aggregate(context, visit, operands,
                                         operand_count, result_type, span,
                                         state);
}

static ir_lower_result_t n00bb_lower_brace_payload(
    ir_lower_context_t *context, parse_visit_t *visit, bool dictionary,
    th_type *payload_type, ir_span_t span);

static ir_lower_result_t
n00bb_lower_brace_literal(ir_lower_context_t *context,
                          parse_visit_t *visit,
                          ir_value_t *const *operands,
                          size_t operand_count, th_type *result_type,
                          ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    if (site)
        return n00bb_lower_user_literal(context, visit, site,
                                        result_type, span);
    th_type *resolved = result_type ? th_resolve(result_type) : nullptr;
    bool is_set = th_builtin_applied(resolved, TH_BUILTIN_CONTAINER_SET, 1)
        != nullptr;
    bool is_dict = th_builtin_applied(resolved, TH_BUILTIN_CONTAINER_DICT, 2)
        != nullptr;
    if (dict_only_empty_literal(visit->grammar, visit->node) && is_set)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "{:} is a dictionary literal, not a set literal");
    if (!is_set && !is_dict)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "brace literal needs an inferred set or dictionary type");
    const th_arg *first = th_named_arg(resolved, 0);
    const th_arg *second = is_dict ? th_named_arg(resolved, 1) : nullptr;
    th_type *key = first && first->kind == TH_ARG_TYPE
        ? first->type : nullptr;
    th_type *value = second && second->kind == TH_ARG_TYPE
        ? second->type : nullptr;
    if (!key || (is_dict && !value))
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "brace literal has unresolved element types");
    th_type *members[] = {key, value};
    th_type *element = is_dict
        ? th_tuple(ir_module_types(context->module), members, 2) : key;
    th_type *payload_type = element
        ? th_array(ir_module_types(context->module), element) : nullptr;
    ir_lower_result_t payload = payload_type
        ? n00bb_lower_brace_payload(context, visit, is_dict,
                                    payload_type, span)
        : (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    return payload.status == IR_LOWER_VALUE && payload.value
        ? n00bb_lower_literal_constructor(context, result_type,
                                          payload.value, visit->node, span)
        : payload;
}

static ir_lower_result_t
n00bb_lower_brace_payload(ir_lower_context_t *context,
                          parse_visit_t *visit, bool dictionary,
                          th_type *payload_type, ir_span_t span)
{
    parse_tree_t *braces = visit->node;
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = braces && collect_sequence_literal_values(context, visit,
                                                          braces, &values)
        && (!dictionary || !(values.len & 1u));
    th_type *element_type = payload_type
        ? th_seq_elem(th_resolve(payload_type)) : nullptr;
    core_list_t(n00bb_ir_value_ptr_t) items =
        core_list_new(n00bb_ir_value_ptr_t);
    ok = ok && element_type;
    for (size_t i = 0; ok && i < values.len; i += dictionary ? 2 : 1) {
        th_type *first_type = element_type;
        th_type *second_type = nullptr;
        if (dictionary) {
            const th_arg *first = th_named_arg(th_resolve(element_type), 0);
            const th_arg *second = th_named_arg(th_resolve(element_type), 1);
            first_type = first && first->kind == TH_ARG_TYPE
                ? first->type : nullptr;
            second_type = second && second->kind == TH_ARG_TYPE
                ? second->type : nullptr;
        }
        ir_lower_result_t first = n00bb_lower_literal_element(context,
            values.data[i], first_type, span);
        ok = first.status == IR_LOWER_VALUE && first.value;
        if (!ok) break;
        ir_value_t *item = first.value;
        if (dictionary) {
            ir_lower_result_t second = ok
                ? n00bb_lower_literal_element(context, values.data[i + 1],
                                              second_type, span)
                : (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
            ok = second.status == IR_LOWER_VALUE && second.value;
            if (!ok) break;
            ir_instr_t *pair = ir_lower_emit_instruction(context,
                IR_OP_AGGREGATE, span);
            ok = pair && ir_lower_set_temp_dest(context, pair,
                                                 "__n00bb_pair_", element_type)
                && ir_instr_operand_value(pair, item)
                && ir_instr_operand_value(pair, second.value);
            if (!ok) break;
            item = pair->dest;
        }
        core_list_push(items, item);
    }
    core_list_free(values);
    ir_instr_t *array = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = array && ir_lower_set_temp_dest(context, array,
        "__n00bb_brace_payload_", payload_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < items.len; i++)
        ok = ir_instr_operand_value(array, items.data[i]);
    core_list_free(items);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not construct the brace handler payload");
    return (ir_lower_result_t){IR_LOWER_VALUE, array->dest};
}

static ir_lower_result_t
n00bb_lower_user_brace_payload(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               n00bb_litmod_site_t *site, ir_span_t span)
{
    bool dictionary = core_string_eq(site->registration->kind,
                                      CORE_STRING_STATIC("dict"));
    return n00bb_lower_brace_payload(context, visit, dictionary,
                                     site->payload_type, span);
}


static ir_lower_result_t
n00bb_lower_user_parenthesized_payload(ir_lower_context_t *context,
                                       parse_visit_t *visit,
                                       n00bb_litmod_site_t *site,
                                       ir_span_t span)
{
    bool record = core_string_eq(site->registration->kind,
                                 CORE_STRING_STATIC("record"));
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    parse_tree_t *fields = record
        ? child_nt(visit->grammar, visit->node, "named-field-list")
        : visit->node;
    core_list_t(parse_tree_ptr_t) expressions =
        core_list_new(parse_tree_ptr_t);
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = collect_ordered_children(fields, &expressions);
    th_type *element_type = site->payload_type
        ? th_seq_elem(th_resolve(site->payload_type)) : nullptr;
    th_type *string_type = record && tu
        ? th_name(tu->types, "string") : nullptr;
    ok = ok && element_type && (!record || string_type);
    for (size_t i = 0; ok && i < expressions.len; i++) {
        parse_tree_t *item = expressions.data[i];
        parse_tree_t *expression = record
            ? child_nt(visit->grammar, item, "expression") : item;
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        if (!value) { ok = false; break; }
        th_type *value_type = element_type;
        if (record) {
            const th_arg *member = th_named_arg(th_resolve(element_type), 1);
            value_type = member && member->kind == TH_ARG_TYPE
                ? member->type : nullptr;
        }
        ir_lower_result_t promoted = n00bb_lower_literal_element(context,
            value, value_type, span);
        if (promoted.status != IR_LOWER_VALUE || !promoted.value) {
            ok = false;
            break;
        }
        value = promoted.value;
        if (record) {
            core_string_t field = named_field_identifier(visit->grammar,
                                                         item);
            ir_instr_t *name = field.data
                ? ir_lower_emit_instruction(context, IR_OP_CONST, span)
                : nullptr;
            ok = name && ir_lower_set_temp_dest(context, name,
                                                 "__n00bb_field_", string_type)
                && ir_instr_set_const_bytes(name,
                    (const uint8_t *)field.data, field.u8_bytes);
            if (!ok) break;
            ir_instr_t *pair = ir_lower_emit_instruction(context,
                IR_OP_AGGREGATE, span);
            ok = pair && ir_lower_set_temp_dest(context, pair,
                "__n00bb_field_pair_", element_type)
                && ir_instr_operand_value(pair, name->dest)
                && ir_instr_operand_value(pair, value);
            if (!ok) break;
            value = pair->dest;
        }
        core_list_push(values, value);
    }
    ir_instr_t *array = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = array && ir_lower_set_temp_dest(context, array,
        "__n00bb_parenthesized_", site->payload_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(array, values.data[i]);
    core_list_free(values);
    core_list_free(expressions);
    if (!ok) return lower_error(context, IR_DIAG_INTERNAL, span,
        "could not construct the tuple or record handler payload");
    return (ir_lower_result_t){IR_LOWER_VALUE, array->dest};
}

static ir_lower_result_t
n00bb_lower_user_literal(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               n00bb_litmod_site_t *site,
                               th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    parse_sym_entry_t *symbol = site && site->registration
        ? site->registration->function : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    ir_func_t *function = symbol && analysis
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_value_t *callee = function
        ? ir_func_address(function->owner, function) : nullptr;
    bool no_payload = site && core_string_eq(site->registration->kind,
                                             CORE_STRING_STATIC("nil"));
    if (!symbol)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "literal handler has no function symbol");
    if (!function)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "literal handler function is absent from this phase");
    if (!callee)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "literal handler has no callable IR address");
    if (!site || (!no_payload && !site->payload_type) || !site->signature
        || !result_type || !ir_type_eq(context->module, result_type,
                                      th_func_ret(site->signature)))
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "literal handler call type does not match its result");
    if (!parse_sym_add_use(symbol, visit->node))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not record the literal handler use");
    ir_lower_result_t payload = {IR_LOWER_FAILED, nullptr};
    if (core_string_eq(site->registration->kind, CORE_STRING_STATIC("array")))
        payload = n00bb_lower_sequence_aggregate(context, visit, nullptr, 0,
            site->payload_type, span, nullptr);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("dict"))
        || core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("set")))
        payload = n00bb_lower_user_brace_payload(context, visit, site, span);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("tuple"))
        || core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("record")))
        payload = n00bb_lower_user_parenthesized_payload(context, visit,
                                                           site, span);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("bool"))) {
        parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
        core_string_t spelling = token && core_option_is_set(token->value)
            ? core_option_get(token->value) : (core_string_t){0};
        bool value = core_string_eq(spelling, CORE_STRING_STATIC("true"));
        if (value || core_string_eq(spelling, CORE_STRING_STATIC("false"))) {
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_CONST, span);
            if (constant && ir_lower_set_temp_dest(context, constant,
                "__n00bb_litmod_bool_", site->payload_type)
                && ir_instr_operand_int(constant, value ? 1 : 0)) {
                payload = (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
            }
        }
    }
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("quotes"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("int"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("hex"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("float"))) {
        parse_token_info_t *token =
            parse_tree_find_first_terminal(visit->node);
        core_buffer_t *decoded = core_buffer_empty();
        bool quotes = core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("quotes"));
        core_string_t spelling = token && core_option_is_set(token->value)
            ? core_option_get(token->value) : (core_string_t){0};
        bool valid = token && decoded && spelling.data;
        if (valid && quotes)
            valid = n00bb_decode_string_literal(spelling, decoded);
        else if (valid) {
            bool hex = core_string_eq(site->registration->kind,
                                      CORE_STRING_STATIC("hex"));
            valid = !hex || (spelling.u8_bytes >= 2
                && spelling.data[0] == '0' && spelling.data[1] == 'x');
            if (valid)
                core_buffer_append(decoded,
                    spelling.data + (hex ? 2 : 0),
                    spelling.u8_bytes - (hex ? 2 : 0));
        }
        ir_instr_t *constant = valid
            ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
        if (constant && ir_lower_set_temp_dest(context, constant,
            "__n00bb_litmod_raw_", site->payload_type)
            && ir_instr_set_const_bytes(constant,
                (const uint8_t *)decoded->data, decoded->byte_len)) {
            payload = (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
        }
        core_buffer_free(decoded);
    }
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("fence"))) {
        parse_token_info_t *token =
            parse_tree_find_first_terminal(visit->node);
        if (token && token->has_content_span
            && core_option_is_set(token->value)) {
            core_string_t spelling = core_option_get(token->value);
            if (token->content_start_byte < token->start_byte
                || token->content_end_byte < token->content_start_byte
                || token->content_end_byte - token->start_byte
                    > spelling.u8_bytes)
                return lower_error(context, IR_DIAG_TYPE_LITERAL, span,
                    "fence literal has invalid content offsets");
            size_t start = token->content_start_byte - token->start_byte;
            size_t end = token->content_end_byte - token->start_byte;
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_CONST, span);
            if (constant && ir_lower_set_temp_dest(context, constant,
                "__n00bb_fence_", site->payload_type)
                && ir_instr_set_const_bytes(constant,
                    (const uint8_t *)spelling.data + start, end - start)) {
                payload = (ir_lower_result_t){IR_LOWER_VALUE,
                                               constant->dest};
            }
        }
    }
    if (!no_payload
        && (payload.status != IR_LOWER_VALUE || !payload.value))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    th_type *outcome_type = ir_call_outcome_type(context->module,
                                                site->signature);
    ir_instr_t *call = outcome_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    if (!call || !ir_lower_set_temp_dest(context, call,
                                        "__n00bb_litmod_call_", outcome_type)
        || !ir_instr_operand_value(call, callee)
        || (!no_payload && !ir_instr_operand_value(call, payload.value))
        || !ir_instr_set_call_signature(call, site->signature))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the literal handler call");
    ir_value_t *value = nullptr;
    if (!n00bb_unwrap_call_outcome(context, visit, call->dest, span,
                                   &value, true))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    if (ir_machine_of(context->module, result_type).class == IR_CLASS_VOID)
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    return (ir_lower_result_t){IR_LOWER_VALUE, value};
}

static ir_lower_result_t
n00bb_lower_literal_handler(ir_lower_context_t *context,
                           parse_visit_t *visit,
                           ir_value_t *const *operands,
                           size_t operand_count, th_type *result_type,
                           ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *user_site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    return user_site
        ? n00bb_lower_user_literal(context, visit, user_site,
                                    result_type, span)
        : lower_error(context, IR_DIAG_TYPE_CLASS, span,
                      "no literal handler is registered for this shape");
}

static bool
n00bb_default_container_literal(parse_grammar_t *grammar,
                                parse_tree_t *node, th_type *type)
{
    bool sequence = nt_is(grammar, node, "list-lit");
    bool braces = nt_is(grammar, node, "dict-or-set-lit");
    if (!sequence && !braces) return false;
    return sequence
        ? th_builtin_applied(type, TH_BUILTIN_CONTAINER_LIST, 1) != nullptr
        : th_builtin_applied(type, TH_BUILTIN_CONTAINER_DICT, 2) != nullptr
          || th_builtin_applied(type, TH_BUILTIN_CONTAINER_SET, 1) != nullptr;
}

static void
n00bb_restore_literal_context(ir_lower_context_t *context,
                              n00bb_lower_context_t *lower)
{
    n00bb_literal_frame_t frame = lower->active_literal;
    context->module = frame.module;
    context->function = frame.function;
    context->block = frame.block;
    context->file_id = frame.file_id;
    context->reachable = frame.reachable;
    lower->withs.len = frame.with_count;
    lower->tries.len = frame.try_count;
    lower->has_active_literal = false;
}

static bool
n00bb_declare_literal_constant(n00bb_lower_context_t *lower,
                               ir_global_t *global)
{
    core_translation_unit_t *unit = lower->unit;
    n00bb_tu_state_t *tu = tu_state(unit);
    parse_symtab_t *symtab = tu->annotations ? tu->annotations->symtab
                                             : nullptr;
    parse_scope_t *scope = module_scope(tu);
    if (!symtab || !scope) return false;
    core_string_t name = core_string_from_cstr(global->name);
    parse_scope_t *saved = parse_symtab_current_scope(symtab,
                                                      CORE_STRING_STATIC(""));
    parse_symtab_enter_scope_all(symtab, scope);
    parse_sym_entry_t *symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC(""), name, PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_enter_scope_all(symtab, saved);
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-literal"), global))
        return false;
    symbol->type = global->type;
    symbol->mutability = PARSE_SYM_CONST;
    symbol->is_synthetic = true;
    parse_sym_set_storage_duration(symbol, PARSE_SYM_STORAGE_MODULE);
    parse_sym_set_phases(symbol, PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_COMPTIME, PARSE_SYM_PHASE_COMPTIME);
    global->mutability = n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME
        ? IR_MUT_MUTABLE : IR_MUT_CONST;
    if (!ir_global_set_storage_duration(lower->ir_context, global,
                                        IR_STORAGE_DURATION_MODULE))
        return false;
    if (n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME) {
        tu->link_analysis->build_time_capture = true;
        return true;
    }
    const char *identifier = n00bb_baked_root_identifier(tu->link_analysis,
        unit->module.identifier, name);
    if (!identifier) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0026"),
            CORE_STRING_STATIC(
                "a static literal has no value from the build-time run"));
        return false;
    }
    return ir_global_set_phase_result_initializer(tu->ir_module, global,
                                                  identifier);
}

static bool
n00bb_record_literal_constant(n00bb_lower_context_t *lower,
                              ir_func_t *producer, th_type *type,
                              ir_global_t **global_out)
{
    n00bb_tu_state_t *tu = tu_state(lower->unit);
    for (size_t i = 0; i < tu->literal_constants.len; i++)
        if (tu->literal_constants.data[i].producer == producer) {
            *global_out = tu->literal_constants.data[i].global;
            return true;
        }
    ir_global_t *global = nullptr;
    if (type) {
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s_value", producer->name);
        global = name.data ? ir_global_add(tu->ir_module, name.data, type)
                           : nullptr;
        core_buffer_release(&name);
        if (!global || !n00bb_declare_literal_constant(lower, global))
            return false;
    }
    core_list_push(tu->literal_constants, ((n00bb_literal_constant_t){
        .producer = producer,
        .global = global,
    }));
    if (n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME)
        tu->link_analysis->build_time_capture = true;
    *global_out = global;
    return true;
}

static bool
n00bb_bind_static_literal_result(ir_lower_context_t *context,
                                 parse_visit_t *visit,
                                 ir_func_t *producer)
{
    th_type *type = core_tree_node_value(visit->node).type;
    ir_global_t *global = nullptr;
    if (!n00bb_record_literal_constant(context->user, producer, type,
                                       &global))
        return false;
    ir_value_t *address = global
        ? ir_global_address(context->module, global) : nullptr;
    ir_instr_t *load = address
        ? ir_lower_emit_instruction(context, IR_OP_COPY,
                                    lower_span_for_tree(context, visit->node))
        : nullptr;
    n00bb_lower_context_t *lower = context->user;
    return load
        && ir_lower_set_temp_dest(context, load, "__n00bb_literal_", type)
        && ir_instr_operand_value(load, address)
        && ir_annot_set(lower->ir_context, &load->annots, (ir_annot_t){
            .key = "n00bb.literal_copy", .kind = IR_ANNOT_BOOL, .b = true})
        && ir_lower_set_node_value(context, visit->node, load->dest);
}

static bool
n00bb_lower_static_litmod(ir_lower_context_t *context,
                          parse_visit_t *visit, bool entering, void *opaque)
{
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_node_literal_site(visit->node) : nullptr;
    bool default_container = !site && n00bb_default_container_literal(
        visit->grammar, visit->node,
        core_tree_node_value(visit->node).type);
    if (!site && !default_container) return true;
    if (site && (!core_string_eq(site->registration->kind,
                                  CORE_STRING_STATIC("array"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("dict"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("set"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("quotes"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("int"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("hex"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("float"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("bool"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("nil"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("tuple"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("record"))
                  && !core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("fence")))) return true;
    if (entering) {
        if (lower->has_active_literal) return true;
        n00bb_node_facts_t *facts = n00bb_node_facts(visit->node);
        if (!facts || !facts->static_literal)
            return true;
        th_type *result_type = core_tree_node_value(visit->node).type;
        if (!result_type || !th_is_closed(tu->types, result_type))
            return lower_error(context, IR_DIAG_TYPE_CLASS,
                lower_span_for_tree(context, visit->node),
                "literal handler result is not concrete at this site")
                .status != IR_LOWER_FAILED;
        parse_token_info_t *token =
            parse_tree_find_first_terminal(visit->node);
        char name[96];
        snprintf(name, sizeof(name), "__n00bb_litmod_%llu",
            (unsigned long long)(token ? token->start_byte : 0));
        ir_module_t *module = tu->ir_module;
        ir_func_t *existing = module ? ir_func_find(module, name) : nullptr;
        if (existing) {
            if (!ir_lower_prune_node(context, visit->node)) return false;
            return ir_machine_of(module, result_type).class == IR_CLASS_VOID
                || n00bb_bind_static_literal_result(context, visit,
                                                     existing);
        }
        th_type *signature = th_func_with_error(tu->types, result_type,
            n00bb_ir_builtin_type(module, "nil"), nullptr, 0, nullptr,
            nullptr, 0);
        ir_func_t *producer = signature && module
            ? ir_func_add(module, name, signature, IR_LINK_INTERNAL)
            : nullptr;
        ir_block_t *entry = producer ? ir_block_add(producer, "entry")
                                     : nullptr;
        if (!entry)
            return lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not create the static literal producer")
                .status != IR_LOWER_FAILED;
        lower->active_literal = (n00bb_literal_frame_t){
            .node = visit->node,
            .module = context->module,
            .function = context->function,
            .block = context->block,
            .producer = producer,
            .file_id = context->file_id,
            .with_count = lower->withs.len,
            .try_count = lower->tries.len,
            .reachable = context->reachable,
        };
        lower->has_active_literal = true;
        lower->withs.len = 0;
        lower->tries.len = 0;
        context->module = module;
        context->function = producer;
        context->block = entry;
        context->file_id = (int32_t)ir_file_count(module) - 1;
        context->reachable = true;
        return true;
    }
    if (!lower->has_active_literal
        || lower->active_literal.node != visit->node) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    th_type *result_type = core_tree_node_value(visit->node).type;
    ir_lower_result_t result = site
        ? n00bb_lower_literal_handler(context, visit, nullptr, 0,
                                      result_type, span, nullptr)
        : nt_is(visit->grammar, visit->node, "list-lit")
            ? n00bb_lower_sequence_aggregate(context, visit, nullptr, 0,
                                              result_type, span, nullptr)
            : n00bb_lower_brace_literal(context, visit, nullptr, 0,
                                        result_type, span, nullptr);
    bool effect_only = result.status == IR_LOWER_NO_VALUE;
    ir_instr_t *ret = (effect_only
        || (result.status == IR_LOWER_VALUE && result.value))
        ? ir_lower_emit_terminator(context, IR_OP_RETURN, span) : nullptr;
    bool emitted = ret && (effect_only
        || ir_instr_operand_value(ret, result.value));
    n00bb_literal_frame_t frame = lower->active_literal;
    n00bb_restore_literal_context(context, lower);
    if (!emitted) return false;
    ir_global_t *unused = nullptr;
    if (effect_only)
        return ir_lower_suppress_emit(context, visit->node)
            && n00bb_record_literal_constant(lower, frame.producer,
                                             nullptr,
                                             &unused);
    return n00bb_bind_static_literal_result(context, visit,
                                            frame.producer);
}


static ir_instr_t *
n00bb_emit_uint_constant(ir_lower_context_t *context, th_type *uint_type,
                         uint64_t value, ir_span_t span, const char *prefix)
{
    char name[64];
    snprintf(name, sizeof(name), "%s_%llu", prefix,
             (unsigned long long)context->next_value_id++);
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_instr_set_dest(constant, name, uint_type)
        || !ir_instr_operand_uint(constant, value)) return nullptr;
    return constant;
}

static ir_value_t *
n00bb_emit_retained_reference(ir_lower_context_t *context,
                              ir_value_t *reference, ir_span_t span)
{
    if (!context || !reference
        || th_kind_of(th_resolve(reference->type)) != TH_INDIRECTION)
        return nullptr;
    ir_instr_t *retain = ir_lower_emit_instruction(context, IR_OP_RETAIN_REF,
                                                   span);
    n00bb_lower_context_t *lower = context->user;
    if (!retain || !ir_lower_set_temp_dest(context, retain,
                                           "__n00bb_retained_ref_",
                                           reference->type)
        || !ir_instr_operand_value(retain, reference)
        || !lower || !ir_value_add_alias_target(lower->ir_context, retain->dest,
            (ir_alias_target_t){.value = reference, .escapes = true}))
        return nullptr;
    return retain->dest;
}

static ir_lower_result_t
n00bb_lower_callback_create(ir_lower_context_t *context,
                            parse_visit_t *visit,
                            ir_value_t *const *operands,
                            size_t operand_count,
                            th_type *result_type, ir_span_t span,
                            void *opaque)
{
    (void)operands;
    (void)operand_count;
    (void)opaque;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_callback_contract_t *contract =
        n00bb_callback_contract_for_type(state, result_type);
    if (!contract || !contract->call || !contract->handler)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "callback construction has no resolved nominal contract");
    if (!n00bb_register_callback_representation(context->module, result_type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "callback construction has no registered value representation");

    parse_tree_t *args = child_nt(visit->grammar, visit->node, "call-args");
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    core_list_t(n00bb_tree_ptr_t) keywords =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, args, "positional-args"), &positional);
    collect_keyword_nodes(visit->grammar,
        child_nt(visit->grammar, args, "keyword-args"), &keywords);
    size_t hidden = contract->stateful ? 2u : 1u;
    bool emitted = positional.len >= hidden;
    ir_value_t *handler = emitted
        ? ir_lower_node_value(context, positional.data[0]) : nullptr;
    handler = handler
        ? n00bb_emit_retained_reference(context, handler,
                                        lower_span_for_tree(context,
                                            positional.data[0]))
        : nullptr;
    ir_value_t *state_reference = nullptr;
    if (emitted && contract->stateful) {
        state_reference = ir_lower_node_value(context, positional.data[1]);
        state_reference = state_reference
            ? n00bb_emit_retained_reference(context, state_reference,
                lower_span_for_tree(context, positional.data[1])) : nullptr;
    }
    emitted = emitted && handler
        && (!contract->stateful || state_reference);

    ir_value_t *setup_value = nullptr;
    parse_sym_entry_t *setup_symbol = nullptr;
    if (emitted && contract->stateful) {
        parse_node_resolution_t resolution =
            parse_tree_resolution(visit->node);
        setup_symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
            ? resolution.target.symbol : nullptr;
        core_translation_unit_t *owner = setup_symbol
            ? unit_for_symbol(visit->grammar, lower->units, setup_symbol)
            : nullptr;
        n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
        ir_func_t *setup_function = setup_symbol && state->link_analysis
            ? link_ir_function_for_symbol(state->link_analysis, setup_symbol)
            : nullptr;
        ir_module_t *owner_module = owner_state
            ? unit_module(owner_state) : nullptr;
        setup_value = owner_module && setup_function
            ? ir_func_address(owner_module, setup_function) : nullptr;
        emitted = setup_value != nullptr;
    }

    th_type *setup_type = contract->setup ? contract->setup->type : nullptr;
    th_type *failure = setup_type ? th_func_error(setup_type) : nullptr;
    bool can_fail = failure && !th_is_nil(failure);
    bool propagated = n00bb_call_is_error_propagated(visit->grammar,
                                                      visit->node);
    if (can_fail != propagated) {
        core_list_free(positional);
        core_list_free(keywords);
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            can_fail
                ? "callback setup failure must be propagated with postfix !"
                : "postfix ! requires a callback setup failure channel");
    }
    th_type *construction_signature = nullptr;
    th_type *ir_result = result_type;
    if (can_fail) {
        construction_signature = th_func_with_error(state->types,
            result_type, failure, nullptr, 0, nullptr, nullptr, 0);
        ir_result = construction_signature
            ? ir_call_outcome_type(context->module,
                                   construction_signature) : nullptr;
    }
    ir_instr_t *instruction = emitted && ir_result
        ? ir_lower_emit_instruction(context, IR_OP_CALLBACK_CREATE, span)
        : nullptr;
    th_type *call_contract = n00bb_specialize_callback_role(state->types,
        contract, result_type, contract->call->type);
    th_type *handler_contract = n00bb_specialize_callback_role(state->types,
        contract, result_type, contract->handler->type);
    th_type *setup_contract = setup_type
        ? n00bb_specialize_callback_role(state->types, contract,
                                          result_type, setup_type) : nullptr;
    emitted = instruction && ir_lower_set_temp_dest(context, instruction,
        "__n00bb_callback_", ir_result)
        && call_contract && handler_contract
        && (!setup_type || setup_contract)
        && ir_instr_set_callback_contract(instruction, call_contract,
            handler_contract, setup_contract);
    if (emitted && setup_value)
        emitted = ir_instr_operand_value(instruction, setup_value);
    if (emitted) emitted = ir_instr_operand_value(instruction, handler);
    if (emitted && state_reference)
        emitted = ir_instr_operand_value(instruction, state_reference);
    for (size_t i = hidden; emitted && i < positional.len; i++) {
        ir_value_t *value = ir_lower_node_value(context, positional.data[i]);
        emitted = value && ir_instr_operand_value(instruction, value);
    }
    for (size_t i = 0; emitted && i < keywords.len; i++) {
        parse_tree_t *expression = child_nt(visit->grammar,
            keywords.data[i], "expression");
        core_string_t keyword = call_keyword_name(keywords.data[i]);
        char *keyword_text = keyword.data
            ? core_strndup(keyword.data, keyword.u8_bytes) : nullptr;
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        emitted = keyword_text && value
            && ir_instr_operand_named(instruction, keyword_text, value);
        core_free(keyword_text);
    }
    core_list_free(positional);
    core_list_free(keywords);
    if (!emitted)
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit semantic callback construction");
    if (state_reference && !ir_value_add_alias_target(
            lower->ir_context, instruction->dest,
            (ir_alias_target_t){
                .value = state_reference, .escapes = true,
            }))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not retain callback state alias provenance");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_instr_t *
n00bb_emit_checked_product(ir_lower_context_t *context, ir_value_t *left,
                           ir_value_t *right, th_type *uint_type,
                           ir_span_t span)
{
    ir_instr_t *product = ir_lower_emit_instruction(context, IR_OP_MUL, span);
    if (!product || !ir_lower_set_temp_dest(context, product,
                                            "__n00bb_array_product_",
                                            uint_type)
        || !ir_instr_operand_value(product, left)
        || !ir_instr_operand_value(product, right)
        || !ir_instr_add_mod(product, "trap")) return nullptr;
    return product;
}

static ir_value_t *
n00bb_coerce_array_dimension_value(ir_lower_context_t *context,
                                   parse_tree_t *expression,
                                   ir_value_t *value, th_type *uint_type,
                                   ir_value_t *one, ir_span_t span,
                                   bool require_positive)
{
    th_type *source_type = core_tree_node_value(expression).type
        ? th_resolve(core_tree_node_value(expression).type) : nullptr;
    if (!is_integer_type(source_type) || !value) return nullptr;
    ir_instr_t *cast = value
        ? ir_lower_emit_instruction(context, IR_OP_CAST, span) : nullptr;
    ir_machine_t source_machine = ir_machine_of(context->module, source_type);
    ir_machine_t target_machine = ir_machine_of(context->module, uint_type);
    bool needs_fit = source_machine.width > target_machine.width
        || source_machine.class != target_machine.class;
    if (!cast || !ir_lower_set_temp_dest(context, cast,
                                         "__n00bb_array_extent_", uint_type)
        || !ir_instr_operand_value(cast, value)
        || (needs_fit && !ir_instr_add_mod(cast, "trap"))) return nullptr;
    if (!require_positive) return cast->dest;
    ir_instr_t *check = ir_lower_emit_instruction(context, IR_OP_SUB, span);
    if (!check || !ir_lower_set_temp_dest(context, check,
                                          "__n00bb_array_positive_",
                                          uint_type)
        || !ir_instr_operand_value(check, cast->dest)
        || !ir_instr_operand_value(check, one)
        || !ir_instr_add_mod(check, "trap")) return nullptr;
    return cast->dest;
}

static ir_value_t *
n00bb_lower_array_dimension_value(ir_lower_context_t *context,
                                  parse_tree_t *expression,
                                  th_type *uint_type, ir_value_t *one,
                                  ir_span_t span, bool require_positive)
{
    return n00bb_coerce_array_dimension_value(context, expression,
        ir_lower_node_value(context, expression), uint_type, one, span,
        require_positive);
}

static ir_lower_result_t n00bb_lower_constructor_new(
    ir_lower_context_t *context, parse_visit_t *visit, th_type *result_type,
    ir_span_t span);

static ir_lower_result_t
n00bb_lower_array_new(ir_lower_context_t *context, parse_visit_t *visit,
                      ir_value_t *const *operands, size_t operand_count,
                      th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    if (!result_type)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "new has no resolved result type");
    th_type *resolved_type = th_resolve(result_type);
    bool is_variant = false;
    if (th_named_info(resolved_type, nullptr, nullptr,
                      &is_variant, nullptr) && is_variant) {
        parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                           "call-args");
        core_list_t(n00bb_tree_ptr_t) positional =
            core_list_new(n00bb_tree_ptr_t);
        collect_positional_nodes(visit->grammar,
            child_nt(visit->grammar, call_args, "positional-args"),
            &positional);
        bool has_keywords = child_nt(visit->grammar, call_args,
                                     "keyword-args") != nullptr;
        if (positional.len || has_keywords) {
            core_list_free(positional);
            return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "new for a variant takes no arguments; assign a selected arm afterward");
        }
        core_list_free(positional);
        ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                                             IR_OP_VARIANT_NEW,
                                                             span);
        if (!instruction
            || !ir_lower_set_temp_dest(context, instruction,
                "__n00bb_variant_new_", resolved_type))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit unset variant storage");
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *constructor_site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    if (caller && (!constructor_site || !constructor_site->symbol
            || constructor_site->symbol->kind != PARSE_SYM_FUNCTION)
        && n00bb_flagset_type(lower->unit, caller, resolved_type, nullptr)) {
        ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                          IR_OP_BITSET_CONST,
                                                          span);
        if (!constant || !ir_lower_set_temp_dest(context, constant,
                                                 "__n00bb_", result_type)
            || !ir_instr_operand_name(constant, "nil")
            || !ir_instr_operand_uint(constant, 0))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit default flagset construction");
        return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
    }
    if (th_kind_of(resolved_type) != TH_SEQUENCE)
        return n00bb_lower_constructor_new(context, visit, result_type, span);

    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = tu ? tu->link_analysis : nullptr;
    if (!lower || !tu || !analysis)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "array construction has no LinkUnit analysis");
    parse_tree_t *type_node = child_nt(visit->grammar, visit->node,
                                       "type-spec");
    core_list_t(n00bb_tree_ptr_t) type_dimensions =
        core_list_new(n00bb_tree_ptr_t);
    bool type_shape_ok = !type_node
        || collect_array_dimension_nodes(visit->grammar, tu, type_node,
                                         &type_dimensions);
    if (!type_shape_ok) {
        core_list_free(type_dimensions);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                           "could not determine the array type dimensions");
    }
    uint32_t rank = 0;
    th_type *leaf = th_resolve(result_type);
    while (th_kind_of(leaf) == TH_SEQUENCE) {
        leaf = th_resolve(th_seq_elem(leaf));
        if (!leaf || rank == UINT32_MAX) {
            core_list_free(type_dimensions);
            return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                               "new has an invalid array type");
        }
        rank++;
    }
    parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                       "call-args");
    if (child_nt(visit->grammar, call_args, "keyword-args")) {
        core_list_free(type_dimensions);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                           "array dimensions must be positional arguments");
    }
    core_list_t(n00bb_tree_ptr_t) arguments =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, call_args, "positional-args"),
        &arguments);
    size_t argument_offset = 0;
    size_t dimension_argument_count = arguments.len - argument_offset;
    if (type_dimensions.len > rank) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                           "array type shape exceeds its sequence rank");
    }
    uint32_t unsized_count = rank;
    for (size_t i = 0; i < type_dimensions.len; i++)
        if (child_nt(visit->grammar, type_dimensions.data[i],
                     "expression")) unsized_count--;
    if (dimension_argument_count > unsized_count
        || (!dimension_argument_count && unsized_count)) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
            "array construction requires dimensions for a prefix of the unsized extents");
    }

    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    ir_instr_t *one = uint_type
        ? n00bb_emit_uint_constant(context, uint_type, 1, span,
                                   "__n00bb_array_one") : nullptr;
    if (!one) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not create the array extent constant");
    }
    core_list_t(n00bb_ir_value_ptr_t) dimension_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_tree_ptr_t) dimension_expressions =
        core_list_new(n00bb_tree_ptr_t);
    for (uint32_t i = 0; i < rank; i++) {
        core_list_push(dimension_values, (ir_value_t *)nullptr);
        core_list_push(dimension_expressions, (parse_tree_t *)nullptr);
    }
    bool ok = true;
    for (size_t i = 0; ok && i < type_dimensions.len; i++) {
        parse_tree_t *expression = child_nt(visit->grammar,
            type_dimensions.data[i], "expression");
        if (!expression) continue;
        dimension_expressions.data[i] = expression;
        ir_shape_extent_t declared_extent = array_dimension_extent(
            visit->grammar, expression);
        if (declared_extent.kind == IR_SHAPE_EXTENT_STATIC) {
            ir_instr_t *constant = n00bb_emit_uint_constant(context,
                uint_type, declared_extent.static_value,
                lower_span_for_tree(context, expression),
                "__n00bb_array_extent");
            if (!constant) {
                ok = false;
                break;
            }
            dimension_values.data[i] = constant->dest;
            continue;
        }
        if (declared_extent.kind == IR_SHAPE_EXTENT_RUNTIME) {
            dimension_values.data[i] = n00bb_lower_array_dimension_value(
                context, expression, uint_type, one->dest,
                lower_span_for_tree(context, expression), true);
            if (!dimension_values.data[i]) ok = false;
            continue;
        }
        core_string_t name = {0};
        simple_identifier_expression(visit->grammar, expression, &name);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "array extent '%.*s' is not a constant the compiler can fold",
            (int)name.u8_bytes, name.data ? name.data : "");
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, expression),
            message.data ? message.data : "array extent is not foldable");
        core_buffer_release(&message);
        ok = false;
    }
    if (!ok) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }

    for (size_t i = 0; ok && i < dimension_argument_count; i++) {
        parse_tree_t *argument = arguments.data[argument_offset + i];
        ir_value_t *extent = n00bb_lower_array_dimension_value(context,
            argument, uint_type, one->dest,
            lower_span_for_tree(context, argument), false);
        if (!extent) { ok = false; break; }
        uint32_t dimension = 0;
        while (dimension < rank && dimension_values.data[dimension])
            dimension++;
        if (dimension == rank) {
            ok = false;
            break;
        }
        dimension_values.data[dimension] = extent;
    }
    core_list_free(arguments);
    if (!ok) {
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
                           "could not lower array dimensions");
    }

    ir_shape_t *shape = ir_shape_new(context->user
        ? ((n00bb_lower_context_t *)context->user)->ir_context : nullptr,
        leaf, rank);
    ok = shape != nullptr;
    uint32_t known_prefix = 0;
    while (known_prefix < rank && dimension_values.data[known_prefix])
        known_prefix++;
    for (uint32_t i = known_prefix; i < rank; i++)
        if (dimension_values.data[i]) ok = false;
    for (uint32_t i = 0; ok && i < rank; i++) {
        ir_shape_extent_t extent = {0};
        if (dimension_expressions.data[i]) {
            extent = array_dimension_extent(visit->grammar,
                dimension_expressions.data[i]);
            extent.value = dimension_values.data[i];
        }
        else if (dimension_values.data[i]) {
            extent = (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_RUNTIME,
                .value = dimension_values.data[i],
            };
        }
        else extent.kind = IR_SHAPE_EXTENT_UNRESOLVED;
        ok = ir_shape_set_extent(shape, i, extent);
    }
    ok = ok && ir_shape_set_storage(shape, known_prefix, true);
    ir_value_t *count = one->dest;
    for (uint32_t i = 0; ok && i < known_prefix; i++) {
        ir_instr_t *product = n00bb_emit_checked_product(context, count,
            dimension_values.data[i], uint_type, span);
        if (!product) ok = false;
        else count = product->dest;
    }
    ok = ok && ir_shape_set_element_count(shape, (ir_shape_measure_t){
        .kind = IR_SHAPE_MEASURE_RUNTIME,
        .value = count,
    });
    ir_value_t *stride = one->dest;
    for (uint32_t i = known_prefix; ok && i > 0; i--) {
        uint32_t dimension = i - 1;
        ok = ir_shape_set_stride(shape, dimension,
            (ir_shape_measure_t){.kind = IR_SHAPE_MEASURE_RUNTIME,
                                  .value = stride});
        if (ok && dimension > 0) {
            ir_instr_t *product = n00bb_emit_checked_product(context, stride,
                dimension_values.data[dimension], uint_type, span);
            if (!product) ok = false;
            else stride = product->dest;
        }
    }
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_ARRAY_NEW, span) : nullptr;
    ok = ok && instruction
        && ir_lower_set_temp_dest(context, instruction,
                                  "__n00bb_array_new_", result_type)
        && lower && lower->ir_context
        && ir_instr_set_array_init(lower->ir_context, instruction,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO);
    for (uint32_t i = 0; ok && i < known_prefix; i++)
        ok = ir_instr_operand_value(instruction, dimension_values.data[i]);
    ok = ok && ir_value_set_shape(instruction->dest, shape);
    core_list_free(type_dimensions);
    core_list_free(dimension_values);
    core_list_free(dimension_expressions);
    if (!ok)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit semantic array construction");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static bool
n00bb_emit_copyout_write(ir_lower_context_t *context,
                         const n00bb_copyout_t *copyout,
                         ir_value_t *value, ir_span_t span)
{
    if (!n00bb_emit_sequence_shape_check(context, value,
            copyout->required_shape, span)) return false;
    if (copyout->index_target)
        return n00bb_lower_index_store_at(context, copyout->grammar,
            copyout->site_node, copyout->index_target,
            copyout->source_node, value);
    if (copyout->local)
        return n00bb_emit_local_write(context, copyout->local, value, span);
    if (copyout->address)
        return n00bb_emit_indirect_write(context, copyout->address, value,
                                         span);
    ir_shape_t *target_shape = ir_value_shape(copyout->global);
    if (!target_shape && copyout->symbol)
        target_shape = parse_sym_binding(copyout->symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (target_shape != copyout->required_shape
        && !n00bb_emit_sequence_shape_check(context, value, target_shape,
                                             span)) return false;
    if (!copyout->global || copyout->global->kind != IR_VAL_GLOBAL
        || !copyout->global->global) return false;
    ir_instr_t *store = ir_lower_emit_instruction(context, IR_OP_GLOBAL_STORE,
                                                  span);
    if (!store || !ir_instr_set_binding_initialization(store, true)
        || !ir_instr_operand_value(store, copyout->global)
        || !ir_instr_operand_value(store, value)) return false;
    return true;
}

static th_type *
n00bb_copyout_type(const n00bb_copyout_t *copyout)
{
    return copyout->local ? copyout->local->type
        : copyout->global ? copyout->global->type
        : copyout->address ? th_ind_referent(copyout->address->type)
        : copyout->index_target
            ? core_tree_node_value(copyout->index_target).type : nullptr;
}

static const th_param *
n00bb_output_formal(th_type *signature, size_t ordinal)
{
    for (size_t i = 0; i < th_func_nparams(signature); i++) {
        const th_param *formal = th_func_param(signature, i);
        if (formal && th_mode_produces_output(formal->mode)) {
            if (!ordinal) return formal;
            ordinal--;
        }
    }
    return nullptr;
}

static bool
n00bb_emit_call_copyout(ir_lower_context_t *context,
                        const n00bb_copyout_t *copyout,
                        ir_value_t *successful_result, ir_span_t span)
{
    th_type *type = n00bb_copyout_type(copyout);
    ir_instr_t *projection = type && successful_result
        ? ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT, span)
        : nullptr;
    if (!projection || !ir_lower_set_temp_dest(context, projection,
        "__n00bb_copyout_", type)
        || !ir_instr_operand_value(projection, successful_result)
        || !ir_instr_operand_uint(projection, copyout->output_index))
        return false;
    return n00bb_emit_copyout_write(context, copyout, projection->dest, span);
}


static bool
n00bb_context_has_hook(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                       const char *name)
{
    if (!context_symbol(grammar, symbol)) return false;
    const char *role = n00bb_context_hook_role(name);
    if (symbol->decl_node)
        return role && parse_sym_binding(symbol,
            core_string_borrow(role, strlen(role))) != nullptr;
    const char *suffix = !strcmp(name, "context-enter") ? "enter"
        : !strcmp(name, "context-exit") ? "exit"
        : !strcmp(name, "context-error") ? "error"
        : !strcmp(name, "context-ensure") ? "ensure" : nullptr;
    char hook_name[256];
    if (!suffix || !context_hook_name(symbol, suffix, hook_name,
                                      sizeof(hook_name))) return false;
    for (parse_sym_entry_t *candidate = symbol->scope
            ? symbol->scope->first_in_scope : nullptr;
         candidate; candidate = candidate->next_in_scope)
        if (candidate->kind == PARSE_SYM_FUNCTION
            && candidate->name.u8_bytes == strlen(hook_name)
            && !memcmp(candidate->name.data, hook_name,
                       candidate->name.u8_bytes)) return true;
    return false;
}

static bool
n00bb_emit_context_hook_call(ir_lower_context_t *context,
                             n00bb_with_item_t *item,
                             const char *hook_kind, ir_value_t *hook_value,
                             ir_span_t span, ir_value_t **outcome,
                             th_type **failure)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = item->context_unit
        ? tu_state(item->context_unit) : nullptr;
    const char *role = n00bb_context_hook_role(hook_kind);
    if (!lower || !n00bb_context_has_hook(lower->grammar, item->context_symbol,
                                hook_kind)
        || !state || !state->ir_module || !role) return false;
    core_link_declaration_t *declaration = link_declaration_for_symbol(
        state->link_analysis, item->context_symbol);
    ir_func_t *function = declaration && role
        ? core_link_declaration_binding(declaration,
                                        core_string_from_cstr(role))
        : nullptr;
    ir_value_t *address = function
        ? ir_func_address(state->ir_module, function) : nullptr;
    th_type *signature = function ? th_resolve(function->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION) {
        lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                    "context hook has no lowered function");
        return false;
    }
    th_type *error_type = th_func_error(signature);
    bool can_fail = error_type
        && !th_is_nil(error_type);
    if (can_fail && !failure) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "this context hook is not allowed to fail");
        return false;
    }
    size_t expected = th_func_nparams(signature);
    if (expected != item->captures.len + (hook_value ? 1 : 0)) {
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context hook capture count does not match its signature");
        return false;
    }
    th_type *result_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : nullptr;
    if (can_fail && !result_type) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not construct the context hook outcome type");
        return false;
    }
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    char result_name[48];
    if (can_fail)
        snprintf(result_name, sizeof(result_name), "__n00bb_hook_%llu",
                 (unsigned long long)context->next_value_id++);
    th_type *context_type = th_resolve(item->context_symbol->type);
    size_t fixed_captures = context_type
        ? th_func_nparams(context_type) : 0;
    bool has_rest_capture = context_type
        && th_func_rest(context_type);
    bool emitted = context_type
        && item->captures.len == fixed_captures + has_rest_capture
        && call && (!can_fail
        || ir_instr_set_dest(call, result_name, result_type))
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < fixed_captures; i++)
        emitted = ir_instr_operand_value(call, item->captures.data[i]);
    if (emitted && hook_value)
        emitted = ir_instr_operand_value(call, hook_value);
    if (emitted && has_rest_capture)
        emitted = ir_instr_operand_value(call,
            item->captures.data[fixed_captures]);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not invoke context hook");
        return false;
    }
    if (outcome) *outcome = can_fail ? call->dest : nullptr;
    if (failure) *failure = can_fail ? error_type : nullptr;
    return true;
}

static bool
n00bb_emit_context_hook(ir_lower_context_t *context,
                        n00bb_with_item_t *item, const char *hook_kind,
                        ir_value_t *hook_value, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, hook_kind)) return true;
    return n00bb_emit_context_hook_call(context, item, hook_kind,
        hook_value, span, nullptr, nullptr);
}

static bool
n00bb_emit_context_hook_outcome_branch(ir_lower_context_t *context,
                                      ir_value_t *outcome, ir_span_t span,
                                      ir_block_t **ok_block,
                                      ir_block_t **error_block)
{
    uint64_t id = context->next_value_id++;
    char ok_name[48], error_name[48], tag_name[48], literal_name[48];
    char test_name[48];
    snprintf(ok_name, sizeof(ok_name), "__n00bb_hook_ok_%llu",
             (unsigned long long)id);
    snprintf(error_name, sizeof(error_name), "__n00bb_hook_error_%llu",
             (unsigned long long)id);
    snprintf(tag_name, sizeof(tag_name), "__n00bb_hook_tag_%llu",
             (unsigned long long)id);
    snprintf(literal_name, sizeof(literal_name), "__n00bb_hook_tag_lit_%llu",
             (unsigned long long)id);
    snprintf(test_name, sizeof(test_name), "__n00bb_hook_is_ok_%llu",
             (unsigned long long)id);
    *ok_block = ir_block_add(context->function, ok_name);
    *error_block = ir_block_add(context->function, error_name);
    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool emitted = *ok_block && *error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag("ok"))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    emitted = emitted && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, *ok_block)
        && ir_instr_operand_block(branch, *error_block);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not branch on a context hook outcome");
        return false;
    }
    branch->span = span;
    return true;
}

static bool
n00bb_propagate_context_hook_failure(ir_lower_context_t *context,
                                     parse_tree_t *source,
                                     ir_value_t *error, ir_span_t span,
                                     uint64_t before_order)
{
    if (!n00bb_emit_with_failure_before(context, source, before_order))
        return false;
    int routed = n00bb_route_try_error(context, source, error, span);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error->type)) {
        if (!n00bb_reject_runtime_initializer_failure(context, span))
            lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "an uncaught context hook error must match the enclosing function's failure channel");
        return false;
    }
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not propagate the context hook error");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

static bool
n00bb_emit_context_error_handler(ir_lower_context_t *context,
                                 n00bb_with_item_t *item,
                                 parse_tree_t *source, ir_value_t *error,
                                 ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, "context-error"))
        return n00bb_propagate_context_hook_failure(context, source, error,
                                                    span, item->order);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, "context-error", error,
                                      span, &outcome, &failure)
        || !outcome || !failure)
        return false;
    ir_block_t *handled = nullptr, *failed = nullptr;
    if (!n00bb_emit_context_hook_outcome_branch(context, outcome, span,
                                                 &handled, &failed))
        return false;

    context->block = failed;
    ir_value_t *handler_error = n00bb_emit_outcome_payload(context, outcome,
        "error", failure, span);
    if (!handler_error || !n00bb_emit_context_hook(context, item,
            "context-ensure", item->provided_value, span)
        || !n00bb_propagate_context_hook_failure(context, source,
                                                  handler_error, span,
                                                  item->order))
        return false;

    context->block = handled;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                 item->provided_value, span)
        || !n00bb_propagate_context_hook_failure(context, source, error,
                                                  span, item->order))
        return false;
    return true;
}

static bool
n00bb_emit_context_exit(ir_lower_context_t *context,
                        n00bb_with_item_t *item, parse_tree_t *source,
                        ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_context_has_hook(lower->grammar,
            item->context_symbol, "context-exit"))
        return n00bb_emit_context_hook(context, item, "context-ensure",
                                       item->provided_value, span);

    ir_value_t *outcome = nullptr;
    th_type *failure = nullptr;
    if (!n00bb_emit_context_hook_call(context, item, "context-exit",
            item->provided_value, span, &outcome, &failure)) return false;
    if (!outcome)
        return n00bb_emit_context_hook(context, item, "context-ensure",
                                       item->provided_value, span);

    ir_block_t *normal = nullptr, *failed = nullptr;
    if (!failure || !n00bb_emit_context_hook_outcome_branch(context, outcome,
            span, &normal, &failed)) return false;
    uint64_t id = context->next_value_id++;
    char join_name[48];
    snprintf(join_name, sizeof(join_name), "__n00bb_hook_join_%llu",
             (unsigned long long)id);
    ir_block_t *join = ir_block_add(context->function, join_name);
    if (!join) return false;

    context->block = normal;
    context->reachable = true;
    if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                 item->provided_value, span)
        || !n00bb_emit_jump(context, join, span)) return false;

    context->block = failed;
    context->reachable = true;
    ir_value_t *error = n00bb_emit_outcome_payload(context, outcome, "error",
                                                    failure, span);
    if (!error) return false;
    if (!n00bb_context_has_hook(lower->grammar, item->context_symbol,
                                "context-error")) {
        if (!n00bb_emit_context_hook(context, item, "context-ensure",
                                     item->provided_value, span)
            || !n00bb_propagate_context_hook_failure(context, source, error,
                                                      span, item->order))
            return false;
    }
    else if (!n00bb_emit_context_error_handler(context, item, source, error,
                                                span)) return false;

    context->block = join;
    context->reachable = true;
    return true;
}

static bool
n00bb_emit_scoped_cleanups(ir_lower_context_t *context,
                           parse_tree_t *exit_node,
                           n00bb_loop_frame_t *stop_before,
                           n00bb_try_frame_t *handler, bool success,
                           uint64_t before_order)
{
    n00bb_lower_context_t *lower = context->user;
    ir_span_t span = lower_span_for_tree(context, exit_node);
    uint64_t stop_order = stop_before ? stop_before->order : 0;
    uint64_t before = before_order;
    for (;;) {
        n00bb_loop_frame_t *loop = nullptr;
        n00bb_with_item_t *with_item = nullptr;
        uint64_t with_order = 0;
        for (size_t i = lower->loops.len; i > 0; i--) {
            n00bb_loop_frame_t *candidate = &lower->loops.data[i - 1];
            if (!candidate->active || !candidate->is_container
                || !candidate->cleanup_active
                || candidate->order <= stop_order
                || candidate->order >= before
                || !node_is_within(exit_node, candidate->node)
                || (handler && node_is_within(handler->node,
                                              candidate->node)))
                continue;
            if (!loop || candidate->order > loop->order) loop = candidate;
        }
        for (size_t i = lower->withs.len; i > 0; i--) {
            n00bb_with_frame_t *candidate = &lower->withs.data[i - 1];
            if (!candidate->active
                || !node_is_within(exit_node, candidate->node)
                || (handler && node_is_within(handler->node,
                                              candidate->node)))
                continue;
            for (size_t j = candidate->items.len; j > 0; j--) {
                n00bb_with_item_t *item = &candidate->items.data[j - 1];
                if (!item->active || item->order <= stop_order
                    || item->order >= before
                    || (with_item && item->order <= with_order))
                    continue;
                with_item = item;
                with_order = item->order;
            }
        }
        if (!loop && !with_item) break;
        if (loop && (!with_item || loop->order > with_order)) {
            before = loop->order;
            if (!n00bb_emit_iteration_cleanup(context, loop, exit_node))
                return false;
            continue;
        }
        before = with_order;
        if (success) {
            if (!n00bb_emit_context_exit(context, with_item, exit_node, span))
                return false;
        }
        else if (!n00bb_emit_context_hook(context, with_item,
                "context-ensure", with_item->provided_value, span))
            return false;
    }
    return true;
}

static bool
n00bb_emit_scoped_success(ir_lower_context_t *context,
                          parse_tree_t *exit_node,
                          n00bb_loop_frame_t *stop_before)
{
    return n00bb_emit_scoped_cleanups(context, exit_node, stop_before,
                                      nullptr, true, UINT64_MAX);
}

static bool
n00bb_emit_with_success(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return n00bb_emit_scoped_success(context, exit_node, nullptr);
}

static bool
n00bb_emit_with_failure(ir_lower_context_t *context, parse_tree_t *exit_node)
{
    return n00bb_emit_with_failure_before(context, exit_node, UINT64_MAX);
}

static bool
n00bb_emit_with_failure_before(ir_lower_context_t *context,
                               parse_tree_t *exit_node,
                               uint64_t before_order)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_try_frame_t *handler = nullptr;
    for (size_t i = lower->tries.len; i > 0; i--) {
        n00bb_try_frame_t *candidate = &lower->tries.data[i - 1];
        if (candidate->active && candidate->body
            && node_is_within(exit_node, candidate->body)) {
            handler = candidate;
            break;
        }
    }
    return n00bb_emit_scoped_cleanups(context, exit_node, nullptr, handler,
                                      false, before_order);
}

static bool
n00bb_propagate_context_computation_failure(ir_lower_context_t *context,
                                            parse_tree_t *source,
                                            ir_value_t *error,
                                            th_type *error_type,
                                            ir_span_t span)
{
    if (!error || !error_type
        || !n00bb_emit_with_failure(context, source)) return false;
    int routed = n00bb_route_try_error(context, source, error, span);
    if (routed < 0) return false;
    if (routed) return true;
    if (!n00bb_failure_channel_accepts(context, error_type)) {
        if (!n00bb_reject_runtime_initializer_failure(context, span))
            lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "an uncaught context-constructor error must match the enclosing function's failure channel");
        return false;
    }
    ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
    if (!fail || !ir_instr_operand_value(fail, error)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not propagate context constructor failure");
        return false;
    }
    fail->span = span;
    context->reachable = false;
    return true;
}

static ir_value_t *
n00bb_call_context_default(ir_lower_context_t *context,
                           parse_tree_t *source,
                           core_translation_unit_t *owner,
                           ir_func_t *function,
                           ir_value_t **arguments, size_t argument_count,
                           ir_span_t span)
{
    n00bb_tu_state_t *owner_state = tu_state(owner);
    th_type *signature = function ? th_resolve(function->type) : nullptr;
    if (!owner_state || !signature || th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != argument_count
        || th_func_rest(signature)) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            "context default helper does not match its dependency contract");
        return nullptr;
    }
    for (size_t i = 0; i < argument_count; i++) {
        const th_param *parameter = th_func_param(signature, i);
        if (!parameter || !arguments[i]
            || !th_unify_in_place(ir_module_types(context->module),
                                  parameter->type, arguments[i]->type)) {
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "a context default dependency does not match its helper parameter");
            return nullptr;
        }
    }
    ir_value_t *address = ir_func_address(owner_state->ir_module, function);
    th_type *failure = th_func_error(signature);
    bool can_fail = failure
        && !th_is_nil(failure);
    th_type *result_type = th_func_ret(signature);
    if (!address || !result_type
        || th_is_nil(result_type)) {
        lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "context defaults must produce a value for their parameter");
        return nullptr;
    }
    th_type *outcome_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : result_type;
    uint64_t id = context->next_value_id++;
    char call_name[48];
    snprintf(call_name, sizeof(call_name), "__n00bb_context_default_%llu",
             (unsigned long long)id);
    ir_instr_t *call = outcome_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool emitted = call && ir_instr_set_dest(call, call_name, outcome_type)
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < argument_count; i++)
        emitted = ir_instr_operand_value(call, arguments[i]);
    if (!emitted) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not invoke a context default helper");
        return nullptr;
    }
    if (!can_fail) return call->dest;

    th_type *definition = ir_type_definition(context->module, outcome_type);
    bool variant = false;
    th_type *ok_type = th_record_lookup(definition, "ok");
    th_type *error_payload_type = th_record_lookup(definition, "error");
    if (!definition || th_kind_of(definition) != TH_RECORD
        || !th_named_info(outcome_type, nullptr, nullptr, &variant, nullptr)
        || !variant || !ok_type || !error_payload_type) {
        lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "context default outcome is not a complete variant");
        return nullptr;
    }
    char ok_name[48], error_name[48], tag_name[48], literal_name[48];
    char test_name[48];
    snprintf(ok_name, sizeof(ok_name), "__n00bb_default_ok_%llu",
             (unsigned long long)id);
    snprintf(error_name, sizeof(error_name), "__n00bb_default_error_%llu",
             (unsigned long long)id);
    snprintf(tag_name, sizeof(tag_name), "__n00bb_default_tag_%llu",
             (unsigned long long)id);
    snprintf(literal_name, sizeof(literal_name),
             "__n00bb_default_tag_lit_%llu", (unsigned long long)id);
    snprintf(test_name, sizeof(test_name), "__n00bb_default_is_ok_%llu",
             (unsigned long long)id);
    ir_block_t *ok_block = ir_block_add(context->function, ok_name);
    ir_block_t *error_block = ir_block_add(context->function, error_name);
    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool branched = ok_block && error_block && uint_type && bool_type
        && tag && ir_instr_set_dest(tag, tag_name, uint_type)
        && ir_instr_operand_value(tag, call->dest)
        && literal && ir_instr_set_dest(literal, literal_name, uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag("ok"))
        && test && ir_instr_set_dest(test, test_name, bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = branched
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    branched = branched && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, ok_block)
        && ir_instr_operand_block(branch, error_block);
    if (!branched) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not branch on context default outcome");
        return nullptr;
    }
    branch->span = span;
    context->block = error_block;
    ir_value_t *error = n00bb_emit_outcome_payload(context, call->dest,
        "error", error_payload_type, span);
    if (!n00bb_propagate_context_computation_failure(context, source, error,
                                                       failure, span))
        return nullptr;
    context->block = ok_block;
    context->reachable = true;
    ir_value_t *value = n00bb_emit_outcome_payload(context, call->dest,
        "ok", ok_type, span);
    if (th_is_nil(ok_type))
        value = nullptr;
    if (!value) {
        lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "context default helper returned no successful value");
        return nullptr;
    }
    return value;
}

static bool
n00bb_context_ensure_default(
    ir_lower_context_t *context, parse_visit_t *visit,
    core_translation_unit_t *owner, parse_sym_entry_t *symbol,
    parse_tree_t *formal_params,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    size_t parameter_count, size_t argument_count,
    ir_value_t **value_by_parameter, bool *provided_by_parameter,
    uint8_t *default_state, size_t index, ir_span_t span)
{
    if (provided_by_parameter[index]) return true;
    if (default_state[index] == 1) {
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context default parameter dependencies are cyclic");
        return false;
    }
    default_state[index] = 1;

    bool interface_context = !symbol->decl_node && symbol->user_data;
    parse_sym_entry_t *formal = interface_context ? nullptr
        : formal_symbol_at(visit->grammar, formal_params, index,
                           &(size_t){0});
    uint8_t archived_flags = 0;
    bool optional = interface_context
        ? context_interface_parameter(symbol, index, nullptr, nullptr,
            &archived_flags, nullptr) && (archived_flags & 1u)
        : formal && formal->is_optional;
    if (!optional) {
        default_state[index] = 0;
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context call omits a required parameter");
        return false;
    }

    parse_tree_t *default_expr = formal
        ? child_nt(visit->grammar, formal->decl_node, "expression") : nullptr;
    core_list_t(size_t) dependencies = core_list_new(size_t);
    bool valid = true;
    if (interface_context) {
        size_t *indices = core_alloc_array(size_t, argument_count);
        size_t dependency_count = 0;
        valid = context_interface_dependencies(symbol, index, indices,
                                               argument_count,
                                               &dependency_count);
        for (size_t d = 0; valid && d < dependency_count; d++) {
            if (indices[d] >= argument_count) valid = false;
            else core_list_push(dependencies, indices[d]);
        }
        core_free(indices);
    }
    else {
        core_list_t(parse_sym_entry_ptr_t) symbols =
            core_list_new(parse_sym_entry_ptr_t);
        context_default_dependencies(default_expr,
                                     parameters, &symbols);
        for (size_t d = 0; d < symbols.len; d++) {
            size_t dependency = 0;
            while (dependency < parameters->len
                   && parameters->data[dependency] != symbols.data[d])
                dependency++;
            if (dependency == parameters->len) valid = false;
            else core_list_push(dependencies, dependency);
        }
        core_list_free(symbols);
    }

    for (size_t d = 0; valid && d < dependencies.len; d++) {
        size_t dependency = dependencies.data[d];
        if (dependency >= argument_count || dependency == index) {
            valid = false;
            break;
        }
        bool dependency_provided = dependency >= parameter_count
            || provided_by_parameter[dependency];
        if (!dependency_provided
            && !n00bb_context_ensure_default(context, visit, owner, symbol,
                formal_params, parameters, parameter_count, argument_count,
                value_by_parameter, provided_by_parameter, default_state,
                dependency, span))
            valid = false;
    }

    char role[64];
    core_link_declaration_t *declaration = link_declaration_for_symbol(
        tu_state(owner)->link_analysis, symbol);
    ir_func_t *helper = context_default_role(index, role, sizeof(role))
        && declaration ? core_link_declaration_binding(declaration,
            core_string_from_cstr(role)) : nullptr;
    ir_value_t **dependency_values = valid
        ? core_alloc_array(ir_value_t *, dependencies.len) : nullptr;
    for (size_t d = 0; valid && d < dependencies.len; d++) {
        dependency_values[d] = value_by_parameter[dependencies.data[d]];
        if (!dependency_values[d]) valid = false;
    }
    ir_value_t *default_value = valid && helper
        ? n00bb_call_context_default(context, visit->node, owner, helper,
            dependency_values, dependencies.len, span) : nullptr;
    core_free(dependency_values);
    core_list_free(dependencies);
    if (!default_value) {
        default_state[index] = 0;
        return false;
    }
    value_by_parameter[index] = default_value;
    provided_by_parameter[index] = true;
    default_state[index] = 2;
    (void)parameter_count;
    return true;
}

static int
n00bb_context_call(ir_lower_context_t *context, parse_visit_t *visit,
                   th_type *result_type, ir_span_t span,
                   ir_lower_result_t *result)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->withs.len)
        return 0;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (!frame->active || !node_is_within(visit->node, frame->node))
        return 0;

    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *symbol = attached_symbol(visit->grammar, lower->unit,
                                                 callee);
    if (!context_symbol(visit->grammar, symbol)) return 0;
    core_translation_unit_t *owner = unit_for_symbol(visit->grammar,
                                                      lower->units, symbol);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    bool has_enter = n00bb_context_has_hook(visit->grammar, symbol,
                                            "context-enter");
    const char *hook_role = n00bb_context_hook_role("context-enter");
    core_link_declaration_t *context_declaration = owner_state
        ? link_declaration_for_symbol(owner_state->link_analysis, symbol)
        : nullptr;
    ir_func_t *function = context_declaration && hook_role
        ? core_link_declaration_binding(context_declaration,
                                        core_string_from_cstr(hook_role))
        : nullptr;
    if (!owner_state || !has_enter || !function) {
        lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "context enter hook has no canonical lowered function binding");
        return -1;
    }
    ir_value_t *address = function
        ? ir_func_address(owner_state->ir_module, function) : nullptr;
    th_type *signature = symbol && symbol->type
        ? th_resolve(symbol->type) : nullptr;
    if (!address || !signature || th_kind_of(signature) != TH_FUNCTION)
        return -1;
    th_type *failure = th_func_error(signature);
    bool can_fail = failure
        && !th_is_nil(failure);

    parse_tree_t *args = call_arguments_node(visit->grammar, visit->node);
    core_list_t(n00bb_ir_value_ptr_t) captures =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    size_t parameter_count = th_func_nparams(signature);
    bool interface_context = !symbol->decl_node && symbol->user_data;
    parse_tree_t *formal_params = interface_context ? nullptr
        : child_nt(visit->grammar, symbol->decl_node, "param-decl");
    size_t rest_index = 0;
    parse_sym_entry_t *rest_symbol = interface_context ? nullptr
        : formal_symbol_at(visit->grammar, formal_params, parameter_count,
                           &rest_index);
    const th_param *rest_parameter = th_func_rest(signature);
    bool archived_rest = false;
    if (interface_context &&
        context_interface_parameter_count(symbol) > parameter_count) {
        uint8_t flags = 0;
        archived_rest = context_interface_parameter(symbol, parameter_count,
            nullptr, nullptr, &flags, nullptr) && (flags & 2u);
    }
    bool has_rest = (rest_symbol || archived_rest) && rest_parameter;
    if (interface_context &&
        context_interface_parameter_count(symbol) != parameter_count
            + (rest_parameter != nullptr)) {
        core_list_free(captures);
        core_list_free(actuals);
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "archived context parameters do not match its callable contract");
        return -1;
    }
    if ((rest_symbol != nullptr) != (rest_parameter != nullptr)) {
        core_list_free(captures);
        core_list_free(actuals);
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "context rest parameter does not match its constructor signature");
        return -1;
    }
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    size_t argument_count = parameter_count + (has_rest ? 1 : 0);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, argument_count);
    bool *provided_by_parameter = core_alloc_array(bool, parameter_count);
    parse_tree_t *positional = child_nt(visit->grammar, args,
                                        "positional-args");
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(visit->grammar, positional,
                                 &positional_nodes);
    bool mapped = has_rest || positional_nodes.len <= parameter_count;
    for (size_t i = 0; mapped && i < positional_nodes.len; i++) {
        if (i < parameter_count) {
            provided_by_parameter[i] = true;
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], i}));
        }
        else if (has_rest)
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], SIZE_MAX}));
        else mapped = false;
    }
    core_list_free(positional_nodes);
    parse_tree_t *keyword_args = child_nt(visit->grammar, args,
                                          "keyword-args");
    core_list_t(n00bb_tree_ptr_t) keyword_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (keyword_args)
        collect_keyword_nodes(visit->grammar, keyword_args, &keyword_nodes);
    for (size_t i = 0; mapped && i < keyword_nodes.len; i++) {
        core_string_t name = call_keyword_name(keyword_nodes.data[i]);
        size_t index = 0;
        parse_sym_entry_t *formal = interface_context ? nullptr
            : formal_symbol_named(visit->grammar, formal_params, name,
                                  &index);
        bool named_parameter = interface_context
            ? context_interface_parameter_named(symbol, name, &index)
            : formal != nullptr;
        parse_tree_t *argument = child_nt(visit->grammar,
            keyword_nodes.data[i], "expression");
        if (!named_parameter || !argument || index >= parameter_count) {
            lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                        "named context argument does not match a parameter");
            mapped = false;
            break;
        }
        if (provided_by_parameter[index]) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, keyword_nodes.data[i]),
                "a context parameter is supplied more than once");
            mapped = false;
            break;
        }
        provided_by_parameter[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    core_list_free(keyword_nodes);
    size_t missing_required = 0;
    for (size_t i = 0; i < parameter_count; i++) {
        parse_sym_entry_t *formal = interface_context ? nullptr
            : formal_symbol_at(visit->grammar, formal_params, i,
                               &(size_t){0});
        const th_param *param = th_func_param(signature, i);
        uint8_t archived_flags = 0, archived_mode = TH_IN;
        if (interface_context && !context_interface_parameter(symbol, i,
                nullptr, nullptr, &archived_flags, &archived_mode))
            mapped = false;
        bool optional = interface_context
            ? (archived_flags & 1u) != 0
            : formal && formal->is_optional;
        bool mode_is_in = interface_context
            ? archived_mode == TH_IN
            : formal && formal->has_mode ? formal->mode == TH_IN
                                          : true;
        if ((!interface_context && !formal) || !param
            || param->mode != TH_IN || !mode_is_in) {
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "context constructor parameters must be in parameters");
            mapped = false;
            break;
        }
        if (!optional && !provided_by_parameter[i])
            missing_required++;
    }
    if (interface_context && mapped) {
        for (size_t i = 0; i < parameter_count; i++) {
            uint8_t flags = 0;
            context_interface_parameter(symbol, i, nullptr, nullptr,
                                        &flags, nullptr);
            if ((flags & 1u) && !(flags & 4u)) {
                lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "the imported context interface does not provide its default computation");
                mapped = false;
                break;
            }
        }
    }
    if (mapped && missing_required) {
        lower_error(context, IR_DIAG_TYPE_ARGS, span,
                    "context call omits a required parameter");
        mapped = false;
    }
    void *binding_checkpoint = ir_lower_bindings_checkpoint(context);
    bool collected = mapped;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        ir_value_t *value = ir_lower_node_value(context,
                                                 actuals.data[i].node);
        if (index == SIZE_MAX) {
            if (!rest_parameter || !value || !th_unify_in_place(
                    ir_module_types(context->module), rest_parameter->type,
                    value->type)) {
                if (rest_parameter && value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, actuals.data[i].node),
                        "context variadic argument does not match its element type");
                collected = false;
                break;
            }
            core_list_push(rest_values, value);
            continue;
        }
        parse_sym_entry_t *formal = formal_symbol_at(visit->grammar,
            formal_params, index, &(size_t){0});
        if ((!interface_context && !formal)
            || (interface_context && !value)) {
            collected = false;
            break;
        }
        value_by_parameter[index] = value;
    }

    if (collected && has_rest) {
        th_type *array_type = th_array(ir_module_types(context->module),
                                       rest_parameter->type);
        char array_name[48];
        snprintf(array_name, sizeof(array_name), "__n00bb_context_rest_%llu",
                 (unsigned long long)context->next_value_id++);
        ir_instr_t *array = array_type
            ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
            : nullptr;
        bool packed = array
            && ir_instr_set_dest(array, array_name, array_type);
        for (size_t i = 0; packed && i < rest_values.len; i++)
            packed = ir_instr_operand_value(array, rest_values.data[i]);
        if (!packed) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not pack context variadic arguments into an array");
            collected = false;
        }
        else {
            value_by_parameter[parameter_count] = array->dest;
            if (rest_symbol && !interface_context
                && !ir_lower_bind_scoped_value(context, rest_symbol,
                                                array->dest, nullptr))
                collected = false;
        }
    }
    core_list_t(parse_sym_entry_ptr_t) default_parameters =
        core_list_new(parse_sym_entry_ptr_t);
    if (!interface_context) {
        parse_sym_entry_t *default_rest = nullptr;
        collect_ir_parameters(visit->grammar, formal_params,
                              &default_parameters, &default_rest);
        if (default_rest) core_list_push(default_parameters, default_rest);
    }
    uint8_t *default_state = core_alloc_array(uint8_t, parameter_count);
    for (size_t i = 0; collected && i < parameter_count; i++)
        if (!provided_by_parameter[i])
            collected = n00bb_context_ensure_default(context, visit, owner,
                symbol, formal_params, &default_parameters, parameter_count,
                argument_count, value_by_parameter, provided_by_parameter,
                default_state, i, span);
    core_free(default_state);
    core_list_free(default_parameters);
    if (collected) {
        for (size_t i = 0; i < parameter_count; i++) {
            if (!value_by_parameter[i]) {
                collected = false;
                break;
            }
            core_list_push(captures, value_by_parameter[i]);
        }
    }
    if (collected && has_rest)
        core_list_push(captures, value_by_parameter[parameter_count]);
    core_list_free(rest_values);
    if (!ir_lower_bindings_restore(context, binding_checkpoint))
        collected = false;
    core_list_free(actuals);
    core_free(value_by_parameter);
    core_free(provided_by_parameter);
    if (!collected) {
        core_list_free(captures);
        if (!ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "context arguments do not match the constructor signature");
        return -1;
    }
    for (size_t i = 0; i < parameter_count; i++) {
        const th_param *param = th_func_param(signature, i);
        if (!param || param->mode != TH_IN) {
            core_list_free(captures);
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                        "context constructor parameters must be in parameters");
            return -1;
        }
    }
    if (rest_parameter && rest_parameter->mode != TH_IN) {
        core_list_free(captures);
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "context variadic parameters must be in parameters");
        return -1;
    }
    th_type *provided_type = th_func_ret(signature);
    if (!ir_type_eq(context->module, provided_type, result_type)) {
        core_list_free(captures);
        core_buffer_t message = {0};
        th_ctx *types = ir_module_types(context->module);
        core_buffer_printf(&message,
            "context enter result type %s does not match inferred context-call type %s",
            n00bb_print_type(types, provided_type), n00bb_print_type(types, result_type));
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics,
            IR_DIAG_TYPE_MISMATCH, span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message.data);
        core_buffer_release(&message);
        core_list_free(captures);
        return -1;
    }
    bool has_value = !th_is_nil(provided_type);
    th_type *outcome_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : provided_type;
    if (!outcome_type) {
        core_list_free(captures);
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not construct the context enter outcome type");
        return -1;
    }
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    ir_value_t *provided = nullptr;
    char value_name[48];
    snprintf(value_name, sizeof(value_name), "__n00bb_context_%llu",
             (unsigned long long)context->next_value_id++);
    bool emitted = call && ir_instr_set_dest(call, value_name, outcome_type)
        && ir_instr_operand_value(call, address);
    for (size_t i = 0; emitted && i < captures.len; i++)
        emitted = ir_instr_operand_value(call, captures.data[i]);
    if (!emitted) {
        core_list_free(captures);
    {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not invoke context enter hook");
        return -1;
    }
    }
    if (can_fail) {
        th_type *definition = ir_type_definition(context->module,
                                                  outcome_type);
        bool variant = false;
        th_type *ok_type = th_record_lookup(definition, "ok");
        th_type *error_type = th_record_lookup(definition, "error");
        if (!definition || th_kind_of(definition) != TH_RECORD
            || !th_named_info(outcome_type, nullptr, nullptr, &variant,
                              nullptr)
            || !variant || !ok_type || !error_type) {
            lower_error(context, IR_DIAG_TYPE_CLASS, span,
                        "context enter outcome is not a complete variant");
            return -1;
        }
        uint64_t id = context->next_value_id++;
        char ok_name[48], error_name[48];
        snprintf(ok_name, sizeof(ok_name), "__n00bb_enter_ok_%llu",
                 (unsigned long long)id);
        snprintf(error_name, sizeof(error_name), "__n00bb_enter_error_%llu",
                 (unsigned long long)id);
        ir_block_t *ok_block = ir_block_add(context->function, ok_name);
        ir_block_t *error_block = ir_block_add(context->function, error_name);
        th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
        th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
        char tag_name[48], literal_name[48], test_name[48];
        snprintf(tag_name, sizeof(tag_name), "__n00bb_enter_tag_%llu",
                 (unsigned long long)id);
        snprintf(literal_name, sizeof(literal_name),
                 "__n00bb_enter_tag_lit_%llu", (unsigned long long)id);
        snprintf(test_name, sizeof(test_name), "__n00bb_enter_is_ok_%llu",
                 (unsigned long long)id);
        ir_instr_t *tag = ir_lower_emit_instruction(context,
            IR_OP_VARIANT_TAG, span);
        ir_instr_t *literal = ir_lower_emit_instruction(context,
            IR_OP_CONST, span);
        ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
        bool branched = ok_block && error_block && uint_type && bool_type
            && tag && ir_instr_set_dest(tag, tag_name, uint_type)
            && ir_instr_operand_value(tag, call->dest)
            && literal && ir_instr_set_dest(literal, literal_name, uint_type)
            && ir_instr_operand_uint(literal, th_arm_tag("ok"))
            && test && ir_instr_set_dest(test, test_name, bool_type)
            && ir_instr_operand_value(test, tag->dest)
            && ir_instr_operand_value(test, literal->dest);
        ir_instr_t *branch = branched
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        branched = branched && branch
            && ir_instr_operand_value(branch, test->dest)
            && ir_instr_operand_block(branch, ok_block)
            && ir_instr_operand_block(branch, error_block);
        if (!branched) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not branch on context enter outcome");
            return -1;
        }
        branch->span = span;
        context->block = error_block;
        ir_value_t *enter_error = n00bb_emit_outcome_payload(
            context, call->dest, "error", error_type, span);
        if (!enter_error || !n00bb_emit_with_failure(context,
                                                     visit->node)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not unwind active contexts after failed enter");
            return -1;
        }
        int routed = n00bb_route_try_error(context, visit->node,
                                            enter_error, span);
        if (routed < 0) return -1;
        if (!routed) {
            if (!n00bb_failure_channel_accepts(context, error_type)) {
                if (!n00bb_reject_runtime_initializer_failure(context,
                                                               span))
                    lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                        "an uncaught context enter error must match the enclosing function's failure channel");
                return -1;
            }
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, enter_error)) {
                lower_error(context, IR_DIAG_INTERNAL, span,
                            "could not propagate context enter failure");
                return -1;
            }
            fail->span = span;
            context->reachable = false;
        }
        context->block = ok_block;
        context->reachable = true;
        if (!th_is_nil(ok_type))
            provided = n00bb_emit_outcome_payload(context, call->dest,
                                                   "ok", ok_type, span);
        if (!th_is_nil(ok_type)
            && !provided) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not extract successful context enter value");
            return -1;
        }
    }
    else provided = has_value ? call->dest : nullptr;

    n00bb_with_item_t item = {
        .context_symbol = symbol,
        .context_unit = owner,
        .call_node = visit->node,
        .captures = captures,
        .provided_value = provided,
        .order = ++lower->next_control_order,
        .active = true,
    };
    core_list_push(frame->items, item);

    *result = provided
        ? (ir_lower_result_t){IR_LOWER_VALUE, provided}
        : (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    return 1;
}

static bool
n00bb_bind_with_manager(ir_lower_context_t *context, parse_tree_t *node)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = tu_state(lower->unit);
    if (!tu || !tu->annotations || !tu->annotations->symtab
        || core_tree_num_children(node) < 3)
        return true;
    core_string_t separator = first_token_text(core_tree_child(node, 1));
    if (!core_string_eq(separator, CORE_STRING_STATIC("="))
        && !core_string_eq(separator, CORE_STRING_STATIC(":")))
        return true;
    parse_sym_entry_t *binding = parse_symtab_resolve_node(
        tu->annotations->symtab, node, CORE_STRING_STATIC(""));
    if (!binding || !binding->type) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, node),
            "named context binding has no inferred type");
        return false;
    }
    char local_name[48];
    snprintf(local_name, sizeof(local_name), "__n00bb_context_value_%llu",
             (unsigned long long)context->next_value_id++);
    ir_local_t *local = ir_local_add(context->function, local_name,
                                     binding->type);
    if (!local || !ir_lower_bind_local(context, binding, local)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, node),
            "could not bind the context-provided local");
        return false;
    }
    return true;
}

static bool
n00bb_bind_with_managers(ir_lower_context_t *context, parse_tree_t *node,
                         parse_tree_t *body)
{
    if (!node || node == body || core_tree_is_leaf(node)) return true;
    n00bb_lower_context_t *lower = context->user;
    if (nt_is(lower->grammar, node, "context-manager"))
        return n00bb_bind_with_manager(context, node);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!n00bb_bind_with_managers(context, core_tree_child(node, i),
                                      body))
            return false;
    return true;
}

static bool
n00bb_lower_with_scope(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !nt_is(visit->grammar, visit->node, "with-stmt"))
        return true;
    if (entering) {
        n00bb_with_frame_t frame = {
            .node = visit->node,
            .body = child_nt(visit->grammar, visit->node, "body"),
            .items = core_list_new(n00bb_with_item_t),
            .active = context->reachable,
        };
        if (frame.active && !n00bb_bind_with_managers(
                context, visit->node, frame.body))
            return false;
        core_list_push(lower->withs, frame);
        return true;
    }
    if (!lower->withs.len
        || lower->withs.data[lower->withs.len - 1].node != visit->node)
        return false;
    n00bb_with_frame_t *frame = &lower->withs.data[lower->withs.len - 1];
    if (frame->active && context->reachable) {
        ir_span_t span = lower_span_for_tree(context, visit->node);
        for (size_t i = frame->items.len; i > 0; i--) {
            n00bb_with_item_t *item = &frame->items.data[i - 1];
            if (!n00bb_emit_context_exit(context, item, visit->node, span))
                return false;
        }
    }
    for (size_t i = 0; i < frame->items.len; i++)
        core_list_free(frame->items.data[i].captures);
    core_list_free(frame->items);
    lower->withs.len--;
    return true;
}

static bool
n00bb_root_storage_writable(parse_grammar_t *grammar, th_ctx *types,
                            parse_tree_t *target, parse_sym_entry_t *root,
                            parse_sym_phase_t phase)
{
    for (;;) {
        parse_tree_t *member = member_access_node(grammar, target);
        parse_tree_t *step = member ? member
            : n00bb_index_expression_node(grammar, target);
        if (!step || !core_tree_num_children(step)) break;
        target = core_tree_child(step, 0);
        th_type *base = member
            ? th_resolve(core_tree_node_value(target).type) : nullptr;
        th_type *definition = base
            ? n00bb_reference_definition(types, base) : nullptr;
        if (base && th_kind_of(definition ? definition : base)
                        == TH_INDIRECTION) return true;
    }
    return root && parse_sym_can_write(root, phase);
}

static bool
n00bb_mark_addressable_argument(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_tree_t *owner,
                                parse_tree_t *argument, th_mode mode)
{
    parse_tree_t *index = n00bb_index_expression_node(grammar, argument);
    if (index)
        return mode != TH_OUT || ir_lower_suppress_emit(context, index);

    parse_tree_t *at = argument;
    parse_tree_t *member = member_access_node(grammar, at);
    if (member) {
        while (member) {
            if (!ir_lower_suppress_emit(context, member)) return false;
            at = core_tree_num_children(member)
                ? core_tree_child(member, 0) : nullptr;
            member = member_access_node(grammar, at);
        }
        parse_tree_t *root = n00bb_simple_lvalue(at);
        if (!root) {
            th_type *base = at ? th_resolve(core_tree_node_value(at).type)
                               : nullptr;
            return base && th_kind_of(base) == TH_INDIRECTION;
        }
        return ir_lower_skip_value(context, owner, root);
    }

    parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
    return lvalue && ir_lower_skip_value(context, owner, argument)
        && ir_lower_skip_value(context, owner, lvalue);
}

static bool
n00bb_mark_reference_target(ir_lower_context_t *context,
                            parse_grammar_t *grammar,
                            parse_tree_t *owner, parse_tree_t *target)
{
    parse_tree_t *index = n00bb_index_expression_node(grammar, target);
    if (index) {
        if (!ir_lower_suppress_emit(context, index)) return false;
        parse_tree_t *receiver = core_tree_num_children(index)
            ? core_tree_child(index, 0) : nullptr;
        parse_tree_t *root = n00bb_simple_lvalue(receiver);
        return !root || ir_lower_skip_value(context, owner, root);
    }
    return n00bb_mark_addressable_argument(context, grammar, owner, target,
                                            TH_INOUT);
}

static bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (!entering || !visit->annot || !visit->node
        || nt_is(visit->grammar, visit->node, "context-call")) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *call_site = tu
        ? link_use_site_for_node(tu, visit->node) : nullptr;
    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *symbol = call_site && call_site->init_call
        ? call_site->symbol : core_tree_node_value(visit->node).sym;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION)
        symbol = callee && lower
            ? bound_function_symbol(callee)
            : nullptr;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return true;
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    bool member_call = member
        && !call_receiver_is_module(visit->grammar, lower->unit, receiver);
    bool callback_call = member_call
        && n00bb_builtin_callback_function(symbol);
    bool trait_call = member_call && !callback_call;
    parse_tree_t *unused_callee = member_call
        ? core_tree_num_children(member) > 2
            ? core_tree_child(member, 2) : nullptr
        : callee;
    if (unused_callee
        && !ir_lower_skip_value(context, visit->node, unused_callee))
        return false;

    parse_tree_t *args = call_arguments_node(visit->grammar, visit->node);
    parse_tree_t *positional = args
        ? child_nt(visit->grammar, args, "positional-args") : nullptr;
    core_list_t(n00bb_tree_ptr_t) actuals = core_list_new(n00bb_tree_ptr_t);
    if (positional)
        collect_positional_nodes(visit->grammar, positional, &actuals);
    size_t formal_offset = trait_call && receiver ? 1 : 0;
    th_type *callable = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    callable = callable ? th_resolve(callable) : nullptr;
    size_t fixed_count = callable && th_kind_of(callable) == TH_FUNCTION
        ? th_func_nparams(callable) : 0;
    const th_param *rest = callable && th_kind_of(callable) == TH_FUNCTION
        ? th_func_rest(callable) : nullptr;
    parse_tree_t *formal_signature = child_nt(visit->grammar,
        symbol->decl_node, "func-type-signature");
    if (!formal_signature)
        formal_signature = child_nt(visit->grammar, symbol->decl_node,
                                    "trait-func-type-signature");
    parse_tree_t *formals = child_nt(visit->grammar, formal_signature,
                                     "param-decl");
    if (!formals)
        formals = child_nt(visit->grammar, formal_signature,
                            "trait-param-decl");
    bool ok = true;
    for (size_t i = 0; ok && i < actuals.len; i++) {
        size_t index = 0;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        index = i + formal_offset;
        if (index >= fixed_count && rest) {
            if (rest->mode == TH_INOUT) {
                ok = n00bb_mark_reference_target(context, visit->grammar,
                    visit->node, actuals.data[i]);
                if (!ok)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, actuals.data[i]),
                        "modify variadic arguments must be writable storage locations of the element type");
            }
            continue;
        }
        if (!function_parameter_info(visit->grammar, symbol, formals,
                index, nullptr, nullptr, nullptr, &flags, &mode)
            || !th_mode_produces_output(mode)) continue;
        ok = n00bb_mark_addressable_argument(context, visit->grammar,
            visit->node, actuals.data[i], mode);
        if (!ok)
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, actuals.data[i]),
                "out and modify arguments must be writable storage locations of the parameter type");
    }
    core_list_free(actuals);
    parse_tree_t *keywords = child_nt(visit->grammar, args, "keyword-args");
    core_list_t(n00bb_tree_ptr_t) named = core_list_new(n00bb_tree_ptr_t);
    if (keywords) collect_keyword_nodes(visit->grammar, keywords, &named);
    for (size_t i = 0; ok && i < named.len; i++) {
        core_string_t name = call_keyword_name(named.data[i]);
        size_t index = 0;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        if (!function_parameter_named(visit->grammar, symbol, formals,
                                      name, &index)
            || !function_parameter_info(visit->grammar, symbol, formals,
                index, nullptr, nullptr, nullptr, &flags, &mode)
            || !th_mode_produces_output(mode)) continue;
        parse_tree_t *argument = child_nt(visit->grammar, named.data[i],
                                           "expression");
        ok = n00bb_mark_addressable_argument(context, visit->grammar,
            visit->node, argument, mode);
        if (!ok)
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "out and modify arguments must be writable storage locations of the parameter type");
    }
    core_list_free(named);
    if (!ok && !ir_diags_failed(context->diagnostics))
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not mark a call argument as an lvalue");
    return ok;
}

static bool
n00bb_mark_constructor_lvalues(ir_lower_context_t *context,
                               parse_visit_t *visit, bool entering,
                               void *state)
{
    (void)state;
    if (!entering || !visit->node) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = tu
        ? link_use_site_for_node(tu, visit->node) : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    th_type *signature = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    signature = signature ? th_resolve(signature) : nullptr;
    if (!symbol || !signature || th_kind_of(signature) != TH_FUNCTION)
        return true;

    parse_tree_t *formal_signature = child_nt(visit->grammar,
        symbol->decl_node, "func-type-signature");
    parse_tree_t *formals = child_nt(visit->grammar, formal_signature,
                                     "param-decl");
    parse_tree_t *args = child_nt(visit->grammar, visit->node, "call-args");
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, args, "positional-args"), &positional);
    size_t fixed_count = th_func_nparams(signature);
    const th_param *rest = th_func_rest(signature);
    bool ok = true;
    for (size_t i = 0; ok && i < positional.len; i++) {
        size_t index = site->init_call ? i : i + 1;
        if (index >= fixed_count) {
            if (rest && rest->mode == TH_INOUT) {
                ok = n00bb_mark_reference_target(context, visit->grammar,
                    visit->node, positional.data[i]);
                if (!ok)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, positional.data[i]),
                        "modify variadic arguments must be writable storage locations of the element type");
            }
            continue;
        }
        th_mode mode = TH_IN;
        if (site->init_call && index == 0) mode = TH_INOUT;
        else if (function_parameter_info(visit->grammar, symbol, formals,
                index, nullptr, nullptr, nullptr, nullptr, &mode)
            && !th_mode_produces_output(mode)) continue;
        if (th_mode_produces_output(mode)) {
            ok = n00bb_mark_addressable_argument(context, visit->grammar,
                visit->node, positional.data[i], mode);
            if (!ok)
                lower_error(context, IR_DIAG_TYPE_ARGS,
                    lower_span_for_tree(context, positional.data[i]),
                    "out and modify arguments must be writable storage locations of the parameter type");
        }
    }
    core_list_free(positional);

    core_list_t(n00bb_tree_ptr_t) keywords =
        core_list_new(n00bb_tree_ptr_t);
    collect_keyword_nodes(visit->grammar,
        child_nt(visit->grammar, args, "keyword-args"), &keywords);
    for (size_t i = 0; ok && i < keywords.len; i++) {
        size_t index = 0;
        th_mode mode = TH_IN;
        core_string_t name = call_keyword_name(keywords.data[i]);
        if (!function_parameter_named(visit->grammar, symbol, formals,
                                      name, &index)
            || !function_parameter_info(visit->grammar, symbol, formals,
                index, nullptr, nullptr, nullptr, nullptr, &mode)
            || !th_mode_produces_output(mode)) continue;
        parse_tree_t *argument = child_nt(visit->grammar, keywords.data[i],
                                           "expression");
        ok = argument && n00bb_mark_addressable_argument(context,
            visit->grammar, visit->node, argument, mode);
        if (!ok)
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument ? argument
                                                      : keywords.data[i]),
                "out and modify arguments must be writable storage locations of the parameter type");
    }
    core_list_free(keywords);
    if (!ok && !ir_diags_failed(context->diagnostics))
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not mark a constructor argument as an lvalue");
    return ok;
}

static ir_value_t *n00bb_promote_option(ir_lower_context_t *context,
                                        parse_tree_t *node, ir_value_t *value,
                                        th_type *item, ir_span_t span);

static ir_lower_result_t
n00bb_lower_module_member(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *const *operands, size_t operand_count,
                          th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    if (operand_count || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "module member lowering expects a typed reference");
    parse_node_resolution_t resolution = parse_tree_resolution(visit->node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && resolution.target.symbol
        && resolution.target.symbol->kind == PARSE_SYM_ENUM_CONST) {
        parse_sym_entry_t *member = resolution.target.symbol;
        n00bb_lower_context_t *lower = context->user;
        if (lower && n00bb_flagset_instance_access(visit->grammar,
                lower->unit, visit->node, member)) {
            parse_tree_t *base_node = core_tree_child(visit->node, 0);
            ir_value_t *base = ir_lower_node_value(context, base_node);
            parse_sym_entry_t *base_symbol = attached_symbol(
                visit->grammar, lower->unit, base_node);
            if (!base && base_symbol)
                base = ir_lower_bound_value(context, base_symbol);
            n00bb_flagset_value_t *value = member->has_comptime_value
                ? member->comptime_value : nullptr;
            ir_instr_t *mask = value
                ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONST, span)
                : nullptr;
            ir_instr_t *test = mask
                ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONTAINS,
                                            span) : nullptr;
            if (!base || !value || !mask || !test
                || !ir_lower_set_temp_dest(context, mask, "__n00bb_",
                    member->type)
                || !ir_instr_operand_name(mask, member->name.data)
                || !ir_instr_operand_uint(mask, value->mask)
                || !ir_lower_set_temp_dest(context, test, "__n00bb_",
                    result_type)
                || !ir_instr_operand_value(test, base)
                || !ir_instr_operand_value(test, mask->dest))
                return lower_error(context, IR_DIAG_INTERNAL, span,
                                   "could not emit a flagset member test");
            return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
        }
        if (n00bb_flagset_member(member)) {
            n00bb_flagset_value_t *value = member->has_comptime_value
                ? member->comptime_value : nullptr;
            if (!value || !member->type)
                return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "flagset constant has no matching resolved mask and type");
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_BITSET_CONST, span);
            if (!constant || !ir_lower_set_temp_dest(context, constant,
                "__n00bb_", result_type)
                || !ir_instr_operand_name(constant, member->name.data)
                || !ir_instr_operand_uint(constant, value->mask))
                return lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit the flagset constant");
            return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
        }
        n00bb_enum_value_t *value = member->has_comptime_value
            ? member->comptime_value : nullptr;
        if (!value || !member->type
            || !ir_type_eq(context->module, result_type, member->type))
            return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                "enum constant has no matching resolved value and type");
        ir_instr_t *constant = ir_lower_emit_instruction(context,
            IR_OP_ENUM_CONST, span);
        if (!constant || !ir_lower_set_temp_dest(context, constant,
            "__n00bb_", result_type)
            || !ir_instr_operand_name(constant, member->name.data)
            || !ir_instr_operand_int_lit(constant,
                (ir_lit_t){.mag = value->magnitude, .neg = value->negative}))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the enum constant");
        return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
    }
    if (resolution.kind != PARSE_NODE_RESOLUTION_SYMBOL
        || !resolution.target.symbol
        || resolution.target.symbol->kind != PARSE_SYM_VARIABLE)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (core_tree_num_children(visit->node) != 3)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (n00bb_variant_arm_symbol(visit->grammar, resolution.target.symbol)
        && n00bb_assignment_target(visit->grammar, visit->node, nullptr))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (n00bb_reference_target(visit->grammar, visit->node))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (n00bb_option_type_element(resolution.target.symbol->type, nullptr)
        && th_kind_of(th_resolve(result_type)) == TH_FUNCTION)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *member = resolution.target.symbol;
    if (member->is_field) {
        if (n00bb_simple_assignment_field_target(visit->grammar,
                                                 visit->node))
            return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
        parse_tree_t *base_node = core_tree_num_children(visit->node)
            ? core_tree_child(visit->node, 0) : nullptr;
        ir_value_t *base = base_node
            ? ir_lower_node_value(context, base_node) : nullptr;
        parse_sym_entry_t *base_symbol = base_node && lower
            ? attached_symbol(visit->grammar, lower->unit, base_node) : nullptr;
        if (!base && base_symbol)
            base = ir_lower_bound_value(context, base_symbol);
        if (!base || !n00bb_check_phase_read(context, visit, true, nullptr))
            return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
        th_type *option_item = nullptr;
        if (n00bb_option_type_element(base->type, &option_item)) {
            base = n00bb_promote_option(context, base_node, base, option_item,
                                        span);
            if (!base) return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
        }
        if (th_kind_of(th_resolve(base->type)) == TH_INDIRECTION
            || n00bb_reference_definition(ir_module_types(context->module),
                                          base->type)) {
            ir_value_t *address = n00bb_record_field_address(context,
                visit->grammar, lower->unit, visit->node, span);
            ir_instr_t *load = address
                ? ir_lower_emit_instruction(context, IR_OP_LOAD, span)
                : nullptr;
            if (!load || !ir_lower_set_temp_dest(context, load,
                "__n00bb_", result_type)
                || !ir_instr_operand_value(load, address)
                || !n00bb_set_reference_access(lower->ir_context,
                                               load->dest, result_type))
                return lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not load a referenced record field");
            return (ir_lower_result_t){IR_LOWER_VALUE, load->dest};
        }
        ir_instr_t *extract = ir_lower_emit_instruction(context, IR_OP_EXTRACT,
                                                        span);
        if (!extract || !ir_lower_set_temp_dest(context, extract,
            "__n00bb_", result_type)
            || !ir_instr_operand_value(extract, base)
            || !ir_instr_operand_name(extract, member->name.data)
            || !n00bb_set_reference_access(lower->ir_context,
                                           extract->dest, result_type))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                               "could not emit a record field read");
        return (ir_lower_result_t){IR_LOWER_VALUE, extract->dest};
    }
    if (!n00bb_check_phase_read(context, visit, true, nullptr))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};

    ir_value_t *binding = ir_lower_bound_value(context,
                                               resolution.target.symbol);
    if (!binding || !ir_type_eq(context->module, binding->type, result_type)) {
        th_ctx *types = ir_ctx_types(lower->ir_context);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "resolved module variable has no matching canonical IR binding (node: %s, binding: %s)",
            n00bb_print_type(types, result_type),
            binding ? n00bb_print_type(types, binding->type) : "<missing>");
        ir_lower_result_t failed = lower_error(context, IR_DIAG_VERIFY_TYPED,
            span, message.data);
        core_buffer_release(&message);
        return failed;
    }

    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!copy || !ir_lower_set_temp_dest(context, copy, "__n00bb_",
                                         result_type)
        || !ir_instr_operand_value(copy, binding))
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit a module variable read");
    return (ir_lower_result_t){IR_LOWER_VALUE, copy->dest};
}

static bool
n00bb_builtin_parameter_named(parse_sym_entry_t *function,
                              core_string_t name, size_t *index)
{
    if (core_string_eq(function->name, CORE_STRING_STATIC("ref_read"))
        && core_string_eq(name, CORE_STRING_STATIC("reference"))) {
        *index = 0;
        return true;
    }
    if (!core_string_eq(function->name,
                        CORE_STRING_STATIC("ref_write"))) return false;
    if (core_string_eq(name, CORE_STRING_STATIC("reference"))) {
        *index = 0;
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("value"))) {
        *index = 1;
        return true;
    }
    return false;
}

static bool
n00bb_attach_call_return_alias(ir_lower_context_t *context,
                               n00bb_lower_context_t *lower,
                               n00bb_link_analysis_t *analysis,
                               parse_sym_entry_t *function,
                               ir_value_t *const *arguments,
                               size_t argument_count,
                               bool escapes,
                               ir_value_t *result)
{
    if (!context || !lower || !analysis || !function || !result
        || !result->type)
        return false;
    th_type *callable = function->callable_contract
        ? function->callable_contract : function->type;
    th_type *signature = callable ? th_resolve(callable) : nullptr;
    th_type *source_result = signature
        && th_kind_of(signature) == TH_FUNCTION
            ? th_func_ret(signature) : nullptr;
    if (!source_result) return false;
    if (!type_may_contain_reference_alias(ir_module_types(context->module),
                                          result->type)) return true;
    n00bb_function_lifetime_effects_t effects = {0};
    if (!function_lifetime_effects_for_symbol(analysis, function, &effects))
        return true;
    if (effects.return_kind == N00BB_RETURN_ALIAS_FRESH_EXTERNAL)
        return ir_value_set_external_writable_storage(lower->ir_context,
                                                      result);
    if (effects.return_kind != N00BB_RETURN_ALIAS_PARAMETER
        && effects.return_kind != N00BB_RETURN_ALIAS_GLOBAL) return true;

    ir_value_t *target = nullptr;
    if (effects.return_kind == N00BB_RETURN_ALIAS_PARAMETER) {
        if (effects.return_parameter >= argument_count) return false;
        size_t index = effects.return_parameter;
        parse_tree_t *signature_node = function->decl_node
            ? child_nt(lower->grammar, function->decl_node,
                       "func-type-signature") : nullptr;
        parse_tree_t *formals = signature_node
            ? child_nt(lower->grammar, signature_node, "param-decl")
            : nullptr;
        for (size_t depth = 0; depth < argument_count; depth++) {
            target = arguments[index];
            if (target) break;
            parse_sym_entry_t *formal = nullptr;
            if (!function_parameter_info(lower->grammar, function, formals,
                    index, &formal, nullptr, nullptr, nullptr, nullptr)
                || !formal || !formal->is_optional || !formal->decl_node)
                return false;
            parse_tree_t *expression = child_nt(lower->grammar,
                formal->decl_node, "expression");
            parse_sym_entry_t *source = simple_expression_symbol(expression);
            if (!source) return false;
            if (source->kind == PARSE_SYM_VARIABLE) {
                target = ir_lower_bound_value(context, source);
                if (!target) {
                    ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                    source);
                    if (global)
                        target = ir_global_address(global->owner, global);
                }
                break;
            }
            bool found = false;
            for (size_t next = 0; next < argument_count; next++) {
                parse_sym_entry_t *candidate = nullptr;
                if (function_parameter_info(lower->grammar, function,
                        formals, next, &candidate, nullptr, nullptr, nullptr,
                        nullptr) && candidate == source) {
                    index = next;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    else {
        parse_sym_entry_t *symbol = global_symbol_from_qualified_name(
            lower->grammar, analysis, effects.return_global,
            effects.return_global_size);
        target = symbol ? ir_lower_bound_value(context, symbol) : nullptr;
        if (!target && symbol) {
            ir_global_t *global = link_ir_global_for_symbol(analysis, symbol);
            if (global)
                target = ir_global_address(global->owner, global);
        }
    }
    if (!target) return false;
    ir_alias_target_t alias = {.value = target, .escapes = escapes};
    return ir_value_set_alias_targets(lower->ir_context, result, &alias, 1,
                                      true);
}

static bool
n00bb_tree_returns_value(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = core_tree_node_value(at).parent)
        if (nt_is(grammar, at, "return-stmt")) return true;
    return false;
}

static bool
n00bb_attach_index_load_shape(ir_lower_context_t *context,
                              ir_value_t *receiver, th_type *result_type,
                              ir_value_t *result)
{
    if (!context || !receiver || !result_type || !result) return false;
    th_type *resolved = th_resolve(result_type);
    if (th_kind_of(resolved) != TH_SEQUENCE) return true;
    ir_shape_t *source = ir_value_shape(receiver);
    if (!source) return true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->ir_context) return false;
    ir_shape_t *suffix = ir_shape_suffix(lower->ir_context, source, 1);
    if (!suffix) return false;
    uint32_t result_rank = 0;
    th_type *element = resolved;
    while (th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        result_rank++;
    }
    return result_rank == ir_shape_rank(suffix)
        && ir_type_eq(context->module, element,
                      ir_shape_element_type(suffix))
        && ir_value_set_shape(result, suffix);
}

static bool
n00bb_index_receiver_may_escape(parse_sym_entry_t *symbol)
{
    if (!symbol) return true;
    if (symbol->kind == PARSE_SYM_PARAM
        || symbol->lifetime.escapes
        || (symbol->scope && core_string_eq(symbol->scope->scope_tag,
            CORE_STRING_STATIC("module")))) return true;
    for (size_t i = 0; i < symbol->alias_count; i++)
        if (symbol->aliases[i].escapes) return true;
    return false;
}

static th_type *trait_method_semantic_shape(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *symbol,
    th_type *source);

static ir_value_t *
n00bb_select_trait_method(ir_lower_context_t *context,
                          parse_grammar_t *grammar,
                          core_translation_unit_t *owner,
                          parse_sym_entry_t *requirement,
                          th_type *signature, ir_value_t *receiver,
                          ir_value_t *contract, ir_span_t span)
{
    if (!context || !requirement || !requirement->scope
        || !receiver || !contract)
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection is missing an input"), nullptr);
    bool builtin = parse_sym_binding(requirement,
        CORE_STRING_STATIC("n00bb.builtin-trait-method")) != nullptr;
    if (!owner && !builtin)
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection has no defining unit"), nullptr);
    th_type *method_type = builtin ? signature
        : trait_method_semantic_shape(grammar, owner, requirement, signature);
    if (!method_type)
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection has no semantic signature"), nullptr);
    core_buffer_t trait_name = {0};
    if (builtin)
        core_buffer_printf(&trait_name, "%.*s",
            (int)requirement->scope->name.u8_bytes,
            requirement->scope->name.data);
    else
        core_buffer_printf(&trait_name, "%.*s.%.*s",
            (int)owner->module.identifier.u8_bytes,
            owner->module.identifier.data,
            (int)requirement->scope->name.u8_bytes,
            requirement->scope->name.data);
    char *method_name = core_strndup(requirement->name.data,
                                     requirement->name.u8_bytes);
    ir_instr_t *selection = ir_lower_emit_instruction(context,
        IR_OP_TRAIT_METHOD, span);
    bool selected = selection && method_name;
    if (!selected)
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not create an instruction"),
            nullptr);
    if (selected && !ir_lower_set_temp_dest(context, selection, "__n00bb_",
                                             method_type))
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not define a result"), nullptr);
    if (selected && !ir_instr_operand_name(selection, trait_name.data))
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not name its trait"), nullptr);
    if (selected && !ir_instr_operand_name(selection, method_name))
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not name its method"), nullptr);
    if (selected && !ir_instr_operand_value(selection, receiver))
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not bind its receiver"), nullptr);
    if (selected && !ir_instr_operand_value(selection, contract))
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "trait method selection could not bind its contract"), nullptr);
    core_buffer_release(&trait_name);
    core_free(method_name);
    if (!selected) return nullptr;
    return selection->dest;
}

static ir_func_t *
n00bb_record_field_method(ir_lower_context_t *context,
                          const th_env_method_t *selected,
                          ir_module_t **owner_module)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    core_link_declaration_t *declaration = state && state->link
        ? linked_method_declaration(state->link, selected) : nullptr;
    parse_sym_entry_t *symbol = initializer_declaration_symbol(declaration);
    core_translation_unit_t *owner = symbol && analysis
        ? unit_for_symbol(analysis->grammar, &analysis->units, symbol)
        : nullptr;
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    if (owner_module) *owner_module = owner_state
        ? unit_module(owner_state) : nullptr;
    ir_func_t *function = symbol && analysis
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    return function;
}

static ir_value_t *
n00bb_record_field_call(ir_lower_context_t *context, ir_block_t *block,
                        const th_env_method_t *selected,
                        ir_value_t *left, ir_value_t *right,
                        th_type *result_type, const char *name)
{
    ir_module_t *owner = nullptr;
    ir_func_t *function = n00bb_record_field_method(context, selected,
                                                     &owner);
    ir_value_t *callee = owner && function
        ? ir_func_address(owner, function) : nullptr;
    th_type *signature = callee && selected->signature
        ? th_unify(ir_module_types(context->module), function->type,
                   selected->signature) : nullptr;
    ir_instr_t *call = signature ? ir_instr_add(block, IR_OP_CALL) : nullptr;
    bool ok = call && ir_instr_set_dest(call, name, result_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, left)
        && (!right || ir_instr_operand_value(call, right))
        && ir_instr_set_call_signature(call, signature);
    return ok ? call->dest : nullptr;
}

static bool
n00bb_emit_panic(ir_lower_context_t *context, ir_value_t *message,
                 ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    for (size_t i = 0; analysis && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        if (!core_string_eq(unit->module.identifier,
                            CORE_STRING_STATIC("builtin.panic"))) continue;
        n00bb_tu_state_t *owner = tu_state(unit);
        parse_scope_t *scope = module_scope(owner);
        parse_sym_entry_t *symbol = scope
            ? parse_symtab_lookup_in(scope, CORE_STRING_STATIC("panic"))
            : nullptr;
        ir_func_t *function = symbol
            ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
        ir_module_t *module = unit_module(owner);
        ir_value_t *callee = function && module
            ? ir_func_address(module, function) : nullptr;
        ir_instr_t *call = callee
            ? ir_instr_add(context->block, IR_OP_CALL) : nullptr;
        if (!call || !ir_instr_operand_value(call, callee)
            || !ir_instr_operand_value(call, message)
            || !ir_instr_set_call_signature(call, function->type))
            return false;
        call->span = span;
        return true;
    }
    return false;
}

static ir_func_t *n00bb_record_hash_function(ir_lower_context_t *context,
                                             th_type *receiver);

static ir_func_t *
n00bb_record_equal_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    th_type *shape = receiver && context
        ? ir_type_definition(context->module, receiver) : nullptr;
    if (!state || !types || !shape || th_kind_of(th_resolve(shape)) != TH_RECORD)
        return nullptr;
    ir_func_t *function = context->function;
    if (!function || function->n_blocks) return nullptr;
    th_type *boolean = th_name(types, "bool");
    ir_value_t *left = ir_param_add(function, "left", receiver);
    ir_value_t *right = ir_param_add(function, "right", receiver);
    ir_block_t *current = ir_block_add(function, "entry");
    ir_block_t *mismatch = ir_block_add(function, "mismatch");
    ir_block_t *match = ir_block_add(function, "match");
    if (!left || !right || !current || !mismatch || !match) return nullptr;
    for (size_t i = 0; i < th_record_len(shape); i++) {
        const th_member *field = th_record_member(shape, i);
        th_env_method_t selected = {0};
        if (!field || th_env_method_resolve(state->types, field->type,
                "Equal", "__equal", &selected) != TH_ENV_METHOD_FOUND
            || (selected.error_type
                && !th_is_nil(selected.error_type)))
            return nullptr;
        char left_name[64], right_name[64], test_name[64], next_name[64];
        snprintf(left_name, sizeof left_name, "field_%zu_left", i);
        snprintf(right_name, sizeof right_name, "field_%zu_right", i);
        snprintf(test_name, sizeof test_name, "field_%zu_equal", i);
        snprintf(next_name, sizeof next_name, "field_%zu_next", i);
        ir_instr_t *left_field = ir_instr_add(current, IR_OP_EXTRACT);
        ir_instr_t *right_field = ir_instr_add(current, IR_OP_EXTRACT);
        bool builtin = selected.module
            && !strcmp(selected.module, "builtins");
        ir_instr_t *equal = builtin
            ? ir_instr_add(current, IR_OP_EQ) : nullptr;
        ir_block_t *next = ir_block_add(function, next_name);
        if (!left_field || !right_field || !next
            || !ir_instr_set_dest(left_field, left_name, field->type)
            || !ir_instr_operand_value(left_field, left)
            || !ir_instr_operand_name(left_field, field->name)
            || !ir_instr_set_dest(right_field, right_name, field->type)
            || !ir_instr_operand_value(right_field, right)
            || !ir_instr_operand_name(right_field, field->name))
            return nullptr;
        ir_value_t *test = nullptr;
        if (builtin) {
            if (equal && ir_instr_set_dest(equal, test_name, boolean)
                && ir_instr_operand_value(equal, left_field->dest)
                && ir_instr_operand_value(equal, right_field->dest)
                && (ir_machine_of(context->module, field->type).class
                        != IR_CLASS_FLOAT
                    || ir_instr_add_mod(equal, "ordered")))
                test = equal->dest;
        }
        else test = n00bb_record_field_call(context, current, &selected,
            left_field->dest, right_field->dest, boolean, test_name);
        ir_instr_t *branch = test
            ? ir_instr_add(current, IR_OP_BRANCH) : nullptr;
        if (!branch || !ir_instr_operand_value(branch, test)
            || !ir_instr_operand_block(branch, next)
            || !ir_instr_operand_block(branch, mismatch)) return nullptr;
        current = next;
    }
    ir_instr_t *success = ir_instr_add(current, IR_OP_JUMP);
    ir_instr_t *false_value = ir_instr_add(mismatch, IR_OP_CONST);
    ir_instr_t *false_return = ir_instr_add(mismatch, IR_OP_RETURN);
    ir_instr_t *true_value = ir_instr_add(match, IR_OP_CONST);
    ir_instr_t *true_return = ir_instr_add(match, IR_OP_RETURN);
    if (!success || !ir_instr_operand_block(success, match)) return nullptr;
    if (!false_value || !ir_instr_set_dest(false_value, "false", boolean)
        || !ir_instr_operand_int(false_value, 0)) return nullptr;
    if (!false_return || !ir_instr_operand_value(false_return,
                                                false_value->dest)) return nullptr;
    if (!true_value || !ir_instr_set_dest(true_value, "true", boolean)
        || !ir_instr_operand_int(true_value, 1)) return nullptr;
    if (!true_return || !ir_instr_operand_value(true_return,
                                               true_value->dest)) return nullptr;
    return function;
}

static ir_func_t *
n00bb_record_hash_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    th_type *shape = receiver && context
        ? ir_type_definition(context->module, receiver) : nullptr;
    if (!state || !types || !shape || th_kind_of(th_resolve(shape)) != TH_RECORD)
        return nullptr;
    ir_func_t *function = context->function;
    if (!function || function->n_blocks) return nullptr;
    th_type *hash_type = th_name(types, "u128");
    ir_value_t *self = ir_param_add(function, "self", receiver);
    ir_block_t *block = ir_block_add(function, "entry");
    ir_instr_t *zero = block ? ir_instr_add(block, IR_OP_CONST) : nullptr;
    if (!self || !zero || !ir_instr_set_dest(zero, "hash_zero", hash_type)
        || !ir_instr_operand_int(zero, 0)) return nullptr;
    ir_value_t *accumulator = zero->dest;
    th_type *count_type = th_name(types, "u8");
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_t *last = ir_instr_add(block, IR_OP_CONST);
    if (!one || !last || !ir_instr_set_dest(one, "rotate_one", count_type)
        || !ir_instr_operand_int(one, 1)
        || !ir_instr_set_dest(last, "rotate_last", count_type)
        || !ir_instr_operand_int(last, 127)) return nullptr;
    for (size_t i = 0; i < th_record_len(shape); i++) {
        const th_member *field = th_record_member(shape, i);
        th_env_method_t selected = {0};
        if (!field || th_env_method_resolve(state->types, field->type,
                "builtin.hash.Hash", "hash", &selected)
                != TH_ENV_METHOD_FOUND
            || (selected.error_type
                && !th_is_nil(selected.error_type)))
            return nullptr;
        char field_name[64], hash_name[64], left_name[64], right_name[64];
        char rotated_name[64], mixed_name[64];
        snprintf(field_name, sizeof field_name, "field_%zu", i);
        snprintf(hash_name, sizeof hash_name, "field_%zu_hash", i);
        snprintf(left_name, sizeof left_name, "hash_%zu_left", i);
        snprintf(right_name, sizeof right_name, "hash_%zu_right", i);
        snprintf(rotated_name, sizeof rotated_name, "hash_%zu_rotated", i);
        snprintf(mixed_name, sizeof mixed_name, "hash_%zu_mixed", i);
        ir_instr_t *extract = ir_instr_add(block, IR_OP_EXTRACT);
        if (!extract || !ir_instr_set_dest(extract, field_name, field->type)
            || !ir_instr_operand_value(extract, self)
            || !ir_instr_operand_name(extract, field->name)) return nullptr;
        ir_value_t *field_hash = n00bb_record_field_call(context, block,
            &selected,
            extract->dest, nullptr, hash_type, hash_name);
        if (!field_hash) return nullptr;
        ir_instr_t *left = ir_instr_add(block, IR_OP_SHL);
        ir_instr_t *right = ir_instr_add(block, IR_OP_SHR);
        ir_instr_t *rotated = ir_instr_add(block, IR_OP_XOR);
        ir_instr_t *mixed = ir_instr_add(block, IR_OP_XOR);
        if (!left || !right || !rotated || !mixed
            || !ir_instr_set_dest(left, left_name, hash_type)
            || !ir_instr_operand_value(left, accumulator)
            || !ir_instr_operand_value(left, one->dest)
            || !ir_instr_set_dest(right, right_name, hash_type)
            || !ir_instr_operand_value(right, accumulator)
            || !ir_instr_operand_value(right, last->dest)
            || !ir_instr_set_dest(rotated, rotated_name, hash_type)
            || !ir_instr_operand_value(rotated, left->dest)
            || !ir_instr_operand_value(rotated, right->dest)
            || !ir_instr_set_dest(mixed, mixed_name, hash_type)
            || !ir_instr_operand_value(mixed, rotated->dest)
            || !ir_instr_operand_value(mixed, field_hash)) return nullptr;
        accumulator = mixed->dest;
    }
    ir_instr_t *ret = ir_instr_add(block, IR_OP_RETURN);
    return ret && ir_instr_operand_value(ret, accumulator)
        ? function : nullptr;
}

static ir_func_t *
n00bb_scalar_hash_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!lower || !lower->ir_context || !types || !function
        || function->n_blocks) return nullptr;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    th_type *hash_type = th_name(types, "u128");
    bool flagset = n00bb_flagset_type(lower->unit, state, receiver, nullptr);
    parse_sym_entry_t *enum_symbol = flagset ? nullptr
        : n00bb_enum_type_symbol(state->link_analysis, receiver);
    const n00bb_enum_range_t *range = enum_symbol
        ? parse_sym_binding(enum_symbol,
            CORE_STRING_STATIC("n00bb.enum-range")) : nullptr;
    if (!flagset && !range) return nullptr;
    th_type *integer_type = th_name(types,
        range && range->is_signed ? "i128" : "u128");
    th_env_method_t selected = {0};
    if (th_env_method_resolve(state->types, integer_type,
            "builtin.hash.Hash", "hash", &selected) != TH_ENV_METHOD_FOUND)
        return nullptr;
    ir_value_t *self = ir_param_add(function, "self", receiver);
    ir_block_t *block = ir_block_add(function, "entry");
    ir_instr_t *integer = block ? ir_instr_add(block,
        flagset ? IR_OP_BITSET_VALUE : IR_OP_ENUM_VALUE) : nullptr;
    if (!self || !integer
        || !ir_instr_set_dest(integer, "value", integer_type)
        || !ir_instr_operand_value(integer, self)
        || (flagset && !ir_instr_add_mod(integer, "trap"))) return nullptr;
    ir_value_t *hashed = n00bb_record_field_call(context, block, &selected,
        integer->dest, nullptr, hash_type, "hash");
    ir_instr_t *ret = hashed ? ir_instr_add(block, IR_OP_RETURN) : nullptr;
    return ret && ir_instr_operand_value(ret, hashed) ? function : nullptr;
}

static bool
n00bb_generate_value_methods(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                th_type *receiver = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.generated-value-method"));
                if (!receiver) continue;
                bool record_hash = symbol->name.data
                    && !strncmp(symbol->name.data,
                        "__n00bb_record_hash_", 20);
                bool scalar_hash = symbol->name.data
                    && !strncmp(symbol->name.data,
                        "__n00bb_scalar_hash_", 20);
                ir_func_t *function = link_ir_function_for_symbol(analysis, symbol);
                ir_module_t *module = unit_module(state);
                n00bb_lower_context_t lower = {
                    .unit = unit,
                    .ir_context = analysis->ir_context,
                };
                ir_lower_context_t context = {
                    .module = module,
                    .function = function,
                    .user = &lower,
                };
                bool generated = function && module
                    && (scalar_hash
                        ? n00bb_scalar_hash_function(&context, receiver)
                        : record_hash
                        ? n00bb_record_hash_function(&context, receiver)
                        : n00bb_record_equal_function(&context, receiver));
                if (!generated) {
                    core_buffer_t message = {0};
                    core_buffer_printf(&message,
                        "could not lower generated value method '%.*s'",
                        (int)symbol->name.u8_bytes, symbol->name.data);
                    core_diagnostics_report(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL,
                        CORE_STRING_STATIC("N00BB-L0062"),
                        core_string_borrow(message.data,
                                           message.byte_len));
                    core_buffer_release(&message);
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
    }
    return true;
}

static ir_func_t *
n00bb_closed_builtin_operator(ir_lower_context_t *context,
                              parse_sym_entry_t *implementation,
                              parse_sym_entry_t *requirement,
                              th_type *receiver_type)
{
    (void)receiver_type;
    if (!parse_sym_binding(implementation,
            CORE_STRING_STATIC("n00bb.builtin-operator"))) return nullptr;
    const char *method = requirement->name.data;
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(requirement->name);
    ir_op_t op;
    if (operator && !operator->shift) op = operator->op;
    else if (!strcmp(method, "__equal")) op = IR_OP_EQ;
    else if (!strcmp(method, "__cmp")) op = IR_OP_LT;
    else return nullptr;

    th_type *signature = th_resolve(implementation->type);
    if (th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != 2 || th_func_rest(signature))
        return nullptr;
    const th_param *left_type = th_func_param(signature, 0);
    const th_param *right_type = th_func_param(signature, 1);
    th_type *return_type = th_func_ret(signature);
    if (!left_type || !right_type || !return_type) return nullptr;

    core_buffer_t name = {0};
    core_buffer_printf(&name, "__dispatch_%.*s",
        (int)implementation->name.u8_bytes, implementation->name.data);
    ir_func_t *existing = name.data
        ? ir_func_find(context->module, name.data) : nullptr;
    ir_func_t *function = existing ? existing : name.data
        ? ir_func_add(context->module, name.data, signature,
                      IR_LINK_INTERNAL) : nullptr;
    core_buffer_release(&name);
    if (!function || existing) return function;
    ir_value_t *left = ir_param_add(function, "left", left_type->type);
    ir_value_t *right = ir_param_add(function, "right", right_type->type);
    ir_block_t *entry = ir_block_add(function, "entry");
    if (!strcmp(method, "__cmp")) {
        static const struct {
            ir_op_t op;
            const char *name;
        } outcomes[] = {
            {IR_OP_LT, "less"},
            {IR_OP_GT, "greater"},
            {IR_OP_EQ, "equal"},
        };
        th_type *bool_type = th_name(ir_module_types(context->module),
                                     "bool");
        ir_block_t *test_block = entry;
        if (!left || !right || !entry || !bool_type) return nullptr;
        for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
            char block_name[32], next_name[32], value_name[32];
            snprintf(block_name, sizeof(block_name), "matched_%s",
                     outcomes[i].name);
            snprintf(next_name, sizeof(next_name), "next_%s",
                     outcomes[i].name);
            snprintf(value_name, sizeof(value_name), "test_%s",
                     outcomes[i].name);
            ir_block_t *matched = ir_block_add(function, block_name);
            ir_block_t *next = ir_block_add(function, next_name);
            ir_instr_t *test = ir_instr_add(test_block, outcomes[i].op);
            ir_instr_t *branch = ir_instr_add(test_block, IR_OP_BRANCH);
            ir_instr_t *constant = ir_instr_add(matched, IR_OP_ENUM_CONST);
            ir_instr_t *ret = ir_instr_add(matched, IR_OP_RETURN);
            if (!matched || !next || !test || !branch || !constant || !ret
                || !ir_instr_set_dest(test, value_name, bool_type)
                || !ir_instr_operand_value(test, left)
                || !ir_instr_operand_value(test, right)
                || !ir_instr_operand_value(branch, test->dest)
                || !ir_instr_operand_block(branch, matched)
                || !ir_instr_operand_block(branch, next)
                || !ir_instr_set_dest(constant, outcomes[i].name,
                                      return_type)
                || !ir_instr_operand_name(constant, outcomes[i].name)
                || !ir_instr_operand_int_lit(constant,
                    (ir_lit_t){.mag = i == 0 ? 0 : i == 1 ? 2 : 1})
                || !ir_instr_operand_value(ret, constant->dest))
                return nullptr;
            if (ir_machine_of(context->module, left_type->type).class
                    == IR_CLASS_FLOAT
                && !ir_instr_add_mod(test, "ordered")) return nullptr;
            test_block = next;
        }
        ir_instr_t *unordered = ir_instr_add(test_block, IR_OP_ENUM_CONST);
        ir_instr_t *ret = ir_instr_add(test_block, IR_OP_RETURN);
        return unordered && ret
            && ir_instr_set_dest(unordered, "unordered", return_type)
            && ir_instr_operand_name(unordered, "unordered")
            && ir_instr_operand_int_lit(unordered, (ir_lit_t){.mag = 3})
            && ir_instr_operand_value(ret, unordered->dest)
            ? function : nullptr;
    }
    ir_instr_t *value = entry ? ir_instr_add(entry, op) : nullptr;
    if (!left || !right || !value
        || !ir_instr_set_dest(value, "result", return_type)
        || !ir_instr_operand_value(value, left)
        || !ir_instr_operand_value(value, right)) return nullptr;
    ir_class_t result_class = ir_machine_of(context->module,
                                           return_type).class;
    if ((result_class == IR_CLASS_SINT || result_class == IR_CLASS_UINT)
        && ir_op_accepts_mod(op, "trap")
        && !ir_instr_add_mod(value, "trap")) return nullptr;
    if (op == IR_OP_EQ
        && ir_machine_of(context->module, left_type->type).class
            == IR_CLASS_FLOAT
        && !ir_instr_add_mod(value, "ordered")) return nullptr;
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    return ret && ir_instr_operand_value(ret, value->dest)
        ? function : nullptr;
}

typedef struct {
    ir_instr_t *call;
    ir_instr_t *result;
    th_type *signature;
} n00bb_closed_trait_call_t;

static n00bb_closed_trait_call_t
n00bb_emit_closed_trait_arm(ir_lower_context_t *context,
                            n00bb_lower_context_t *lower,
                            n00bb_trait_dispatch_entry_t *entry,
                            parse_grammar_t *grammar,
                            parse_sym_entry_t *requirement,
                            parse_tree_t *formal_params,
                            size_t parameter_count,
                            th_type *signature, th_type *result_type,
                            core_list_t(n00bb_ir_value_ptr_t) *values,
                            ir_value_t *receiver, ir_span_t span)
{
    n00bb_closed_trait_call_t result = {0};
    n00bb_link_analysis_t *analysis = tu_state(lower->unit)->link_analysis;
    core_translation_unit_t *owner = unit_for_symbol(grammar, lower->units,
                                                      entry->implementation);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    ir_func_t *function = analysis
        ? link_ir_function_for_symbol(analysis, entry->implementation)
        : nullptr;
    ir_module_t *module = owner_state
        ? unit_module(owner_state) : nullptr;
    if (!function) {
        function = n00bb_closed_builtin_operator(context,
            entry->implementation, requirement, entry->receiver_type);
        module = context->module;
    }
    ir_value_t *callee = module && function
        ? ir_func_address(module, function) : nullptr;
    th_type *call_signature = function
        ? th_unify(ir_module_types(context->module), function->type, signature)
        : nullptr;
    th_type *call_result = call_signature
        ? th_func_ret(call_signature) : nullptr;
    th_type *call_failure = call_signature
        ? th_func_error(call_signature) : nullptr;
    if (call_failure && !th_is_nil(call_failure))
        call_result = ir_call_outcome_type(context->module, call_signature);
    ir_instr_t *call = callee && call_signature && call_result
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool emitted = call
        && ir_lower_set_temp_dest(context, call, "__n00bb_", call_result)
        && ir_instr_operand_value(call, callee);
    for (size_t p = 0; emitted && p < values->len; p++) {
        ir_value_t *argument = p == 0 ? receiver : values->data[p];
        if (argument) {
            emitted = ir_instr_operand_value(call, argument);
            continue;
        }
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool known = p < parameter_count
            && function_parameter_info(grammar, requirement,
                formal_params, p, nullptr, nullptr, nullptr, &flags, &mode);
        emitted = known && (flags & 1u) && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(call)
            : known && mode == TH_OUT
                ? ir_instr_operand_out(call) : false;
    }
    emitted = emitted && ir_instr_set_call_signature(call, call_signature);
    if (!emitted) return result;
    ir_instr_t *last = call;
    if (!ir_type_eq(context->module, call_result, result_type)) {
        ir_value_t *boxed = n00bb_emit_wrapping(context, IR_OP_BOX,
            call->dest, result_type, span);
        if (!boxed) return result;
        last = boxed->def;
    }
    result.call = call;
    result.result = last;
    result.signature = call_signature;
    return result;
}

static ir_instr_t *
n00bb_emit_closed_trait_call(ir_lower_context_t *context,
                             n00bb_lower_context_t *lower,
                             n00bb_trait_dispatch_plan_t *plan,
                             parse_grammar_t *grammar,
                             parse_sym_entry_t *requirement,
                             parse_tree_t *formal_params,
                             size_t parameter_count,
                             th_type *signature,
                             th_type *result_type,
                             core_list_t(n00bb_ir_value_ptr_t) *values,
                             core_list_t(n00bb_copyout_t) *copyouts,
                             ir_span_t span)
{
    if (!context || !lower || !plan || !plan->count || !requirement
        || !signature || !result_type || !values || !values->len
        || !values->data[0]) return nullptr;
    if (plan->direct) {
        n00bb_closed_trait_call_t emitted = n00bb_emit_closed_trait_arm(
            context, lower, &plan->entries[0], grammar, requirement,
            formal_params, parameter_count, signature, result_type,
            values, values->data[0], span);
        return emitted.result;
    }
    char local_name[48];
    snprintf(local_name, sizeof(local_name), "__n00bb_dispatch_%llu",
             (unsigned long long)context->next_value_id++);
    bool void_result = th_is_nil(result_type);
    ir_local_t *result = void_result ? nullptr
        : ir_local_add(context->function, local_name, result_type);
    ir_block_t *join = n00bb_branch_block(context, lower,
                                           "trait_dispatch_join");
    bool has_fallback = plan->entries[plan->count - 1].generic_fallback;
    ir_block_t *unmatched = has_fallback ? nullptr
        : n00bb_branch_block(context, lower, "trait_dispatch_unmatched");
    th_type *bool_type = th_name(ir_module_types(context->module), "bool");
    if ((!void_result && !result) || !join
        || (!has_fallback && !unmatched) || !bool_type)
        return nullptr;

    ir_block_t *test_block = context->block;
    for (size_t i = 0; i < plan->count; i++) {
        n00bb_trait_dispatch_entry_t *entry = &plan->entries[i];
        ir_block_t *arm = n00bb_branch_block(context, lower,
                                              "trait_dispatch_arm");
        ir_block_t *next = entry->generic_fallback ? nullptr
            : i + 1 < plan->count
            ? n00bb_branch_block(context, lower, "trait_dispatch_test")
            : unmatched;
        ir_instr_t *test = entry->generic_fallback ? nullptr
            : ir_instr_add(test_block, IR_OP_TYPE_TEST);
        ir_instr_t *branch = ir_instr_add(test_block,
            entry->generic_fallback ? IR_OP_JUMP : IR_OP_BRANCH);
        bool emitted = arm && branch;
        if (emitted && entry->generic_fallback)
            emitted = ir_instr_operand_block(branch, arm);
        else if (emitted) {
            emitted = next && test
                && ir_lower_set_temp_dest(context, test, "__n00bb_",
                                           bool_type)
                && ir_instr_operand_value(test, values->data[0])
                && ir_instr_operand_type(test, entry->receiver_type)
                && ir_instr_operand_value(branch, test->dest)
                && ir_instr_operand_block(branch, arm)
                && ir_instr_operand_block(branch, next);
        }
        if (!emitted) return nullptr;
        if (test) test->span = span;
        branch->span = span;

        ir_instr_t *narrow = entry->generic_fallback ? nullptr
            : ir_instr_add(arm, IR_OP_TYPE_NARROW);
        if (narrow) {
            emitted = ir_lower_set_temp_dest(context, narrow, "__n00bb_",
                                              entry->receiver_type)
                && ir_instr_operand_value(narrow, values->data[0])
                && ir_instr_operand_type(narrow, entry->receiver_type);
        }
        context->block = arm;
        n00bb_closed_trait_call_t arm_call = emitted
            ? n00bb_emit_closed_trait_arm(context, lower, entry, grammar,
                requirement, formal_params, parameter_count, signature,
                result_type, values,
                narrow ? narrow->dest : values->data[0], span)
            : (n00bb_closed_trait_call_t){0};
        ir_instr_t *call = arm_call.call;
        th_type *call_signature = arm_call.signature;
        emitted = arm_call.result != nullptr;
        ir_value_t *arm_result = emitted ? arm_call.result->dest : nullptr;
        ir_instr_t *store = emitted && !void_result
            ? ir_instr_add(arm, IR_OP_COPY) : nullptr;
        emitted = emitted && (void_result
            || (store && ir_instr_set_dest_local(store, result)
                && ir_instr_operand_value(store, arm_result)));
        for (size_t p = 0; emitted && copyouts && p < copyouts->len; p++) {
            n00bb_copyout_t *copyout = &copyouts->data[p];
            const th_param *formal = n00bb_output_formal(call_signature,
                                                         copyout->output_index);
            th_type *target_type = n00bb_copyout_type(copyout);
            ir_instr_t *projection = formal && target_type
                ? ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT, span)
                : nullptr;
            emitted = projection
                && ir_lower_set_temp_dest(context, projection, "__n00bb_",
                                          formal->type)
                && ir_instr_operand_value(projection, call->dest)
                && ir_instr_operand_uint(projection, copyout->output_index);
            ir_value_t *output = projection ? projection->dest : nullptr;
            if (emitted && !ir_type_eq(context->module, formal->type,
                                        target_type)) {
                output = n00bb_emit_wrapping(context, IR_OP_BOX, output,
                                             target_type, span);
                emitted = output != nullptr;
            }
            if (emitted) {
                emitted = n00bb_emit_copyout_write(context, copyout,
                                                    output, span);
            }
        }
        ir_instr_t *jump = emitted
            ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
        emitted = jump && ir_instr_operand_block(jump, join);
        if (!emitted) return nullptr;
        if (narrow) narrow->span = span;
        call->span = span;
        if (store) store->span = span;
        jump->span = span;
        test_block = next;
    }
    if (unmatched) {
        ir_instr_t *unreachable = ir_instr_add(unmatched, IR_OP_UNREACHABLE);
        if (!unreachable) return nullptr;
        unreachable->span = span;
    }
    context->block = join;
    ir_instr_t *read = ir_instr_add(join,
        void_result ? IR_OP_NIL : IR_OP_COPY);
    if (!read || !ir_lower_set_temp_dest(context, read, "__n00bb_",
                                         result_type)
        || (!void_result && !ir_instr_operand_local(read, result)))
        return nullptr;
    read->span = span;
    return read;
}

static ir_value_t *
n00bb_emit_iteration_trait_call(ir_lower_context_t *context,
                                n00bb_loop_frame_t *frame,
                                size_t site_index, ir_value_t *receiver,
                                th_mode receiver_mode,
                                th_type *return_type,
                                ir_value_t **copyout_value,
                                parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    n00bb_link_use_site_t *site = state
        && site_index < state->link_use_sites.len
            ? &state->link_use_sites.data[site_index] : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    ir_span_t span = lower_span_for_tree(context, source);
    if (!lower || !state || !analysis || !frame || !site || !symbol
        || !receiver || !return_type)
        return (lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            "iteration trait call has incomplete semantic bindings"), nullptr);

    symbol = linkable_trait_requirement(analysis, lower->unit, site, symbol);
    core_translation_unit_t *owner = symbol
        ? unit_for_symbol(lower->grammar, lower->units, symbol) : nullptr;
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    ir_func_t *function = owner_state
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_module_t *module = owner_state
        ? unit_module(owner_state) : nullptr;
    ir_value_t *callee = module && function
        ? ir_func_address(module, function) : nullptr;
    th_ctx *types = ir_module_types(context->module);
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    bool trait_requirement = symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("trait"));
    th_type *callable = contract ? th_type_clone(types, contract) : nullptr;
    if (callable && th_kind_of(th_resolve(callable)) == TH_BINDER)
        callable = th_type_clone(types, th_binder_body(th_resolve(callable)));
    callable = callable ? th_resolve(callable) : nullptr;
    th_type *failure = callable && th_kind_of(callable) == TH_FUNCTION
        ? th_func_error(callable) : nullptr;
    if (trait_requirement && failure
        && th_kind_of(th_resolve(failure)) == TH_VAR
        && !lower->tries.len)
        failure = th_func_error(lower->function_symbol
            ? lower->function_symbol->type : context->function->type);
    th_param parameter = {.mode = receiver_mode, .type = receiver->type};
    th_type *shape = failure
        ? th_func_with_error(types, return_type, failure, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    th_type *signature = shape && callable
        ? th_unify(types, callable, shape) : nullptr;
    if (signature) signature = th_unify_in_place(types, signature, shape);
    if (!signature || th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != 1 || th_func_rest(signature))
        return (lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "iteration trait method does not match its required signature"),
                nullptr);

    failure = th_func_error(signature);
    if (trait_requirement && !callee) {
        core_buffer_t requirement_name = {0};
        core_buffer_printf(&requirement_name, "__trait_req_%s_%.*s",
            site->resolved_trait_name ? site->resolved_trait_name
                                      : site->dispatch_trait.data,
            (int)site->dispatch_method.u8_bytes,
            site->dispatch_method.data);
        ir_func_t *requirement = requirement_name.data
            ? ir_func_find(context->module, requirement_name.data) : nullptr;
        if (!requirement && requirement_name.data) {
            th_type *requirement_type = th_type_clone(types, contract);
            requirement = requirement_type
                ? ir_func_add(context->module, requirement_name.data,
                              requirement_type, IR_LINK_EXTERN) : nullptr;
            if (requirement && !th_is_closed(types, contract)
                && !ir_func_set_callable_contract(requirement, contract))
                requirement = nullptr;
        }
        core_buffer_release(&requirement_name);
        callee = requirement
            ? ir_func_address(context->module, requirement) : nullptr;
    }
    if (trait_requirement) {
        ir_instr_t *selection = callee
            ? ir_lower_emit_instruction(context, IR_OP_TRAIT_METHOD, span)
            : nullptr;
        bool selected = selection
            && ir_lower_set_temp_dest(context, selection,
                "__n00bb_iteration_method_", signature)
            && ir_instr_operand_name(selection,
                site->resolved_trait_name ? site->resolved_trait_name
                                          : site->dispatch_trait.data)
            && ir_instr_operand_name(selection,
                                     site->dispatch_method.data)
            && ir_instr_operand_value(selection, receiver)
            && ir_instr_operand_value(selection, callee);
        callee = selected ? selection->dest : nullptr;
    }
    if (!callee)
        return (lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "iteration trait method has no IR binding"), nullptr);

    bool can_fail = failure && !th_is_nil(failure);
    th_type *call_type = can_fail
        ? ir_call_outcome_type(context->module, signature) : return_type;
    if (receiver_mode == TH_CONSUME) {
        receiver = n00bb_emit_consume_value(context, receiver, nullptr,
                                             span);
        if (!receiver) return nullptr;
    }
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    bool emitted = call && call_type
        && ir_lower_set_temp_dest(context, call,
            "__n00bb_iteration_call_", call_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, receiver)
        && ir_instr_set_call_signature(call, signature);
    if (!emitted)
        return (lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the iteration trait call"), nullptr);

    ir_value_t *result = call->dest;
    if (can_fail) {
        parse_visit_t visit = {
            .grammar = lower->grammar,
            .node = source,
        };
        if (!n00bb_unwrap_call_outcome(context, &visit, result, span,
                                       &result, false)) return nullptr;
    }
    if (copyout_value) {
        ir_instr_t *output = ir_lower_emit_instruction(context,
            IR_OP_CALL_OUTPUT, span);
        if (!output || !ir_lower_set_temp_dest(context, output,
            "__n00bb_iteration_cursor_", receiver->type)
            || !ir_instr_operand_value(output, result)
            || !ir_instr_operand_uint(output, 0)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not retain the modified iteration cursor");
            return nullptr;
        }
        *copyout_value = output->dest;
    }
    return result;
}

static bool
n00bb_emit_iteration_cleanup(ir_lower_context_t *context,
                             n00bb_loop_frame_t *frame,
                             parse_tree_t *source)
{
    if (!frame || !frame->cleanup_active || !frame->iteration_plan
        || !frame->iteration_cursor_value) return true;
    th_type *nil_type = th_name(ir_module_types(context->module), "nil");
    return nil_type
        && n00bb_emit_iteration_trait_call(context, frame,
            frame->iteration_plan->cleanup_site,
            frame->iteration_cursor_value, TH_CONSUME, nil_type,
            nullptr, source) != nullptr;
}

static bool
n00bb_aliases_are_sound_for_consume(parse_sym_entry_t *symbol)
{
    if (!symbol) return true;
    if (!symbol->alias_analysis_complete
        && (symbol->alias_count || symbol->aliased_by_count
            || symbol->lifetime.escapes)) return false;
    for (size_t i = 0; i < symbol->alias_count; i++)
        if (symbol->aliases[i].escapes) return false;
    for (size_t i = 0; i < symbol->aliased_by_count; i++)
        if (symbol->aliased_by[i].escapes) return false;
    return true;
}

static ir_value_t *
n00bb_emit_consume_value(ir_lower_context_t *context, ir_value_t *value,
                         ir_value_t *binding, ir_span_t span)
{
    if (!context || !value) return nullptr;
    ir_instr_t *consume = ir_lower_emit_instruction(context, IR_OP_CONSUME,
                                                    span);
    if (!consume || !ir_lower_set_temp_dest(context, consume,
        "__n00bb_consumed_", value->type)
        || !ir_instr_operand_value(consume, value)
        || (binding && binding != value
            && !ir_instr_operand_value(consume, binding))) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the semantic ownership transfer");
        return nullptr;
    }
    return consume->dest;
}

static ir_value_t *
n00bb_emit_consume(ir_lower_context_t *context, parse_grammar_t *grammar,
                   core_translation_unit_t *unit, parse_tree_t *argument,
                   ir_value_t *value)
{
    if (!context || !argument || !value) return nullptr;
    parse_tree_t *index = n00bb_index_expression_node(grammar, argument);
    parse_tree_t *member = index ? nullptr
        : member_access_node(grammar, argument);
    if (index || member) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, argument),
            "consume requires a whole binding or a temporary value, not a field or indexed subobject");
        return nullptr;
    }

    parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
    parse_sym_entry_t *symbol = lvalue
        ? attached_symbol(grammar, unit, lvalue) : nullptr;
    ir_value_t *binding = nullptr;
    if (symbol && (symbol->kind == PARSE_SYM_VARIABLE
                   || symbol->kind == PARSE_SYM_PARAM)) {
        if (!n00bb_aliases_are_sound_for_consume(symbol)) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "consume cannot prove that all aliases to this binding are tracked and non-escaping");
            return nullptr;
        }
        binding = ir_lower_bound_address(context, symbol);
        if (!binding) {
            ir_local_t *local = ir_lower_bound_local(context, symbol);
            binding = local ? ir_local_value(context->function, local)
                            : ir_lower_bound_value(context, symbol);
        }
        if (!binding) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, argument),
                "consumed binding has no lowered semantic identity");
            return nullptr;
        }
    }

    return n00bb_emit_consume_value(context, value, binding,
                                    lower_span_for_tree(context, argument));
}

static ir_value_t *
n00bb_typed_integer_value(ir_lower_context_t *context,
                          parse_tree_t *node, ir_value_t *value,
                          th_type *target)
{
    if (!value || !target || ir_type_eq(context->module, value->type, target))
        return value;
    ir_instr_t *constant = value->def;
    if (!constant || constant->op != IR_OP_CONST
        || constant->n_operands != 1
        || constant->operands[0].kind != IR_OPD_CONST
        || constant->operands[0].lit.is_float) {
        n00bb_lower_context_t *lower = context->user;
        n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
        int64_t minimum = 0, maximum = 0;
        ir_machine_t source = ir_machine_of(context->module, value->type);
        ir_machine_t destination = ir_machine_of(context->module, target);
        bool widening = source.width && destination.width
            && ((source.class == destination.class
                    && source.width <= destination.width)
                || (source.class == IR_CLASS_UINT
                    && destination.class == IR_CLASS_SINT
                    && source.width < destination.width)
                || n00bb_practical_unsigned_to_signed(lower
                        ? lower->grammar : nullptr, state, node,
                    value->type, target));
        bool interval_fits = state
            && n00bb_current_integer_range(state, node, &minimum, &maximum)
            && n00bb_integer_interval_fits_type(target, minimum, maximum);
        if ((!widening && !interval_fits)
            || (source.class != IR_CLASS_SINT
                && source.class != IR_CLASS_UINT)
            || (destination.class != IR_CLASS_SINT
                && destination.class != IR_CLASS_UINT)) return value;
        ir_span_t span = lower_span_for_tree(context, node);
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST,
                                                     span);
        if (!cast || !ir_lower_set_temp_dest(context, cast, "__n00bb_",
                                             target)
            || !ir_instr_operand_value(cast, value)
            || ((source.width > destination.width
                 || source.class != destination.class)
                && !ir_instr_add_mod(cast,
                    widening ? "trap" : "wrap")))
            return nullptr;
        return cast->dest;
    }
    ir_machine_t machine = ir_machine_of(context->module, target);
    ir_machine_t source = ir_machine_of(context->module, value->type);
    ir_lit_t literal = constant->operands[0].lit.i;
    if ((source.class != IR_CLASS_SINT && source.class != IR_CLASS_UINT)
        || (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 128)
        return value;
    if (machine.class == IR_CLASS_UINT && literal.neg) return value;
    if (machine.width < 128) {
        ir_uint_t limit = (ir_uint_t)1 << (machine.width
            - (machine.class == IR_CLASS_SINT));
        if (literal.mag > limit
            || (literal.mag == limit
                && (machine.class == IR_CLASS_UINT || !literal.neg)))
            return value;
    }
    else if (machine.class == IR_CLASS_SINT) {
        ir_uint_t sign = (ir_uint_t)1 << 127;
        if (literal.mag > sign || (literal.mag == sign && !literal.neg))
            return value;
    }
    ir_span_t span = lower_span_for_tree(context, node);
    ir_instr_t *typed = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    if (!typed || !ir_lower_set_temp_dest(context, typed, "__n00bb_",
                                          target)
        || !ir_instr_operand_int_lit(typed, literal)) return nullptr;
    return typed->dest;
}

static ir_value_t *
n00bb_typed_integer_literal(ir_lower_context_t *context,
                            parse_tree_t *node, th_type *target)
{
    ir_value_t *value = ir_lower_node_value(context, node);
    ir_value_t *typed = n00bb_typed_integer_value(context, node, value,
                                                  target);
    if (!typed || typed == value) return typed;
    return ir_lower_set_node_value(context, node, typed) ? typed : nullptr;
}

static ir_value_t *
n00bb_emit_wrapping(ir_lower_context_t *context, ir_op_t op,
                    ir_value_t *payload, th_type *target, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                "__n00bb_", target)
        || (payload && !ir_instr_operand_value(instruction, payload)))
        return nullptr;
    return instruction->dest;
}

static bool
n00bb_union_admits(ir_module_t *module, th_type *target, th_type *value)
{
    if (th_builtin_kind(ir_module_types(module), target) == TH_BUILTIN_ANY)
        return true;
    th_comb comb;
    if (!th_sum_comb(th_resolve(target), &comb) || comb != TH_UNION)
        return false;
    for (size_t i = 0; i < th_sum_len(target); i++)
        if (ir_type_eq(module, value, th_sum_arm(target, i))) return true;
    return false;
}

static bool n00bb_option_path_proven(n00bb_lower_context_t *lower,
                                     parse_grammar_t *grammar,
                                     parse_tree_t *receiver);

static bool
n00bb_option_value_proven(n00bb_lower_context_t *lower,
                          parse_grammar_t *grammar, parse_tree_t *value)
{
    if (!lower || !grammar || !value) return false;
    parse_sym_entry_t *symbol = call_receiver_symbol(grammar, lower->unit,
                                                     value);
    return parse_sym_binding(symbol,
               CORE_STRING_STATIC("n00bb.option-set-element"))
        || n00bb_option_path_proven(lower, grammar, value);
}

static ir_value_t *
n00bb_promote_option(ir_lower_context_t *context, parse_tree_t *node,
                     ir_value_t *value, th_type *item, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!n00bb_option_value_proven(lower, lower ? lower->grammar : nullptr,
                                   node)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "an option's value requires it to be proven non-nil by the "
            "active branch");
        return nullptr;
    }
    ir_value_t *promoted = n00bb_emit_wrapping(context, IR_OP_OPTION_GET,
                                               value, item, span);
    return promoted && n00bb_set_reference_access(lower->ir_context,
                                                  promoted, item)
        ? promoted : nullptr;
}

static ir_value_t *
n00bb_promoted_operand(ir_lower_context_t *context, parse_tree_t *node,
                       ir_value_t *value, bool comparison, ir_span_t span)
{
    th_type *item = nullptr;
    if (comparison || !value || !n00bb_option_type_element(value->type, &item))
        return value;
    return n00bb_promote_option(context, node, value, item, span);
}

static ir_value_t *
n00bb_materialize(ir_lower_context_t *context, parse_tree_t *node,
                  ir_value_t *value, th_type *target, ir_span_t span)
{
    if (!value || !target || ir_type_eq(context->module, value->type, target))
        return value;
    th_ctx *types = ir_module_types(context->module);
    th_type *referent = n00bb_dereference_target(types, value->type, target);
    if (referent) {
        ir_value_t *loaded = ir_lower_value_as(context, value, referent, span);
        return loaded ? n00bb_materialize(context, node, loaded, target, span)
                      : nullptr;
    }
    th_type *option_item = nullptr;
    if (n00bb_option_type_element(value->type, &option_item)
        && !n00bb_option_type_element(target, nullptr)
        && !n00bb_union_admits(context->module, target, value->type)) {
        ir_value_t *promoted = n00bb_promote_option(context, node, value,
                                                    option_item, span);
        return promoted ? n00bb_materialize(context, node, promoted, target,
                                            span)
                        : nullptr;
    }
    th_type *atomic_element = n00bb_atomic_element_type(target);
    if (atomic_element && !n00bb_atomic_element_type(value->type)) {
        ir_value_t *inner = n00bb_materialize(context, node, value,
                                              atomic_element, span);
        if (!inner || !ir_type_eq(context->module, inner->type,
                                  atomic_element)) return inner;
        return n00bb_emit_atomic_wrap(context, inner, target, span);
    }
    th_type *option_element = nullptr;
    if (n00bb_option_type_element(target, &option_element)) {
        if (th_is_nil(value->type))
            return n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr,
                                       target, span);
        ir_value_t *inner = n00bb_materialize(context, node, value,
                                              option_element, span);
        if (!inner || !ir_type_eq(context->module, inner->type,
                                  option_element)) return inner;
        return n00bb_emit_wrapping(context, IR_OP_OPTION_SOME, inner, target,
                                   span);
    }
    ir_value_t *integer = n00bb_typed_integer_value(context, node, value,
                                                    target);
    if (integer != value) return integer;
    if (n00bb_union_admits(context->module, target, value->type))
        return n00bb_emit_wrapping(context, IR_OP_BOX, value, target, span);
    return value;
}

static ir_value_t *
n00bb_materialize_location(ir_lower_context_t *context, ir_value_t *reference,
                           th_type *target, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    while (reference && reference->type
           && th_kind_of(th_resolve(reference->type)) == TH_INDIRECTION) {
        th_type *referent = th_ind_referent(th_resolve(reference->type));
        if (th_unify_probe(types, referent, target)) return reference;
        if (!n00bb_dereferences_to(types, referent, target)) return nullptr;
        reference = ir_lower_value_as(context, reference, referent, span);
    }
    return nullptr;
}

typedef struct {
    parse_sym_entry_t *symbol;
    ir_local_t *local;
    ir_value_t *global;
    ir_value_t *address;
    parse_tree_t *index_target;
    parse_tree_t *lvalue;
    th_type *type;
    bool through_reference;
    bool writable;
} n00bb_out_location_t;

static n00bb_out_location_t
n00bb_out_argument_location(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *argument,
                            th_type *parameter_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_out_location_t location = {0};
    location.index_target = n00bb_index_expression_node(grammar, argument);
    parse_tree_t *member_target = location.index_target ? nullptr
        : member_access_node(grammar, argument);
    parse_sym_entry_t *member_symbol = member_target
        ? attached_symbol(grammar, lower->unit, argument) : nullptr;
    parse_tree_t *field_target = member_symbol && member_symbol->is_field
        ? member_target : nullptr;
    bool module_target = member_target && member_symbol
        && !member_symbol->is_field;
    location.lvalue = !location.index_target && !member_target
        ? n00bb_simple_lvalue(argument) : nullptr;
    location.symbol = location.index_target
        ? n00bb_assignment_root_symbol(grammar, lower->unit,
                                       location.index_target)
        : location.lvalue
        ? attached_symbol(grammar, lower->unit, location.lvalue)
        : field_target ? n00bb_assignment_root_symbol(grammar, lower->unit,
                                                      field_target)
        : module_target ? member_symbol : nullptr;
    location.local = location.lvalue
        ? ir_lower_bound_local(context, location.symbol) : nullptr;
    location.global = (location.lvalue || module_target) && !location.local
        ? ir_lower_bound_value(context, location.symbol) : nullptr;
    location.address = member_target
        ? n00bb_addressable_expression_address(context, grammar, lower->unit,
            argument, lower_span_for_tree(context, argument)) : nullptr;
    th_type *storage = location.local ? location.local->type
        : location.global && location.global->kind == IR_VAL_GLOBAL
            ? location.global->type : nullptr;
    if (location.lvalue && storage && parameter_type
        && n00bb_dereferences_to(ir_module_types(context->module), storage,
                                 parameter_type)) {
        location.address = n00bb_materialize_location(context,
            location.local ? n00bb_emit_local_read(context, location.local,
                                                   span)
                           : location.global, parameter_type, span);
        location.through_reference = location.address != nullptr;
        location.local = nullptr;
        location.global = nullptr;
    }
    location.type = location.index_target
        ? core_tree_node_value(location.index_target).type
        : location.address ? th_ind_referent(location.address->type)
        : location.local ? location.local->type
        : location.global && location.global->kind == IR_VAL_GLOBAL
            ? location.global->type : nullptr;
    location.writable = location.through_reference || (location.symbol
        && n00bb_root_storage_writable(grammar,
            ir_module_types(context->module), argument, location.symbol,
            n00bb_compile_phase()));
    if (field_target)
        location.writable = location.writable
            && member_symbol->mutability == PARSE_SYM_MUTABLE;
    return location;
}

static ir_value_t *
n00bb_out_location_read(ir_lower_context_t *context, parse_tree_t *argument,
                        const n00bb_out_location_t *location, ir_span_t span)
{
    return location->index_target ? ir_lower_node_value(context, argument)
        : location->address ? n00bb_emit_indirect_read(context,
            location->address, location->type, span)
        : location->local ? n00bb_emit_local_read(context, location->local,
                                                  span)
        : location->global;
}

static ir_value_t *
n00bb_materialize_node(ir_lower_context_t *context, parse_tree_t *node,
                       th_type *target, ir_span_t span)
{
    return n00bb_materialize(context, node, ir_lower_node_value(context, node),
                             target, span);
}

static bool n00bb_nominal_value_as(ir_lower_context_t *context,
                                   parse_visit_t *visit,
                                   th_type *target_type,
                                   parse_tree_t *source);
static bool n00bb_option_path_proven(n00bb_lower_context_t *lower,
                                     parse_grammar_t *grammar,
                                     parse_tree_t *receiver);

static ir_lower_result_t
n00bb_lower_call(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    if (operand_count || !result_type)
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                           "call lowering needs a result type");
    ir_lower_result_t context_result = {IR_LOWER_FAILED, nullptr};
    int context_call = n00bb_context_call(context, visit, result_type, span,
                                          &context_result);
    if (context_call < 0)
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    if (context_call > 0)
        return context_result;
    if (nt_is(visit->grammar, visit->node, "context-call"))
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
            "context call is not inside an active with scope or did not resolve to a context declaration");
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller_state = lower
        ? tu_state(lower->unit) : nullptr;
    parse_tree_t *call_node = visit->node;
    core_string_t index_trait = {0}, index_method = {0};
    bool index_call = index_dispatch_for_node(visit->grammar, call_node,
                                               &index_trait, &index_method);
    if (index_call && n00bb_reference_target(visit->grammar, call_node))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (index_call
        && (core_string_eq(index_trait, CORE_STRING_STATIC("Index_Write"))
            || core_string_eq(index_trait,
                              CORE_STRING_STATIC("Index_Slice_Write"))))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    n00bb_link_use_site_t *index_site = index_call && caller_state
        ? link_use_site_for_node(caller_state, call_node) : nullptr;
    parse_cf_label_t *call_label = lower && caller_state
        ? parse_cf_label_lookup(caller_state->annotations, call_node) : nullptr;
    parse_tree_t *callee = core_tree_num_children(call_node)
        ? core_tree_child(call_node, 0) : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver_node = index_call
        ? (core_tree_num_children(call_node)
            ? core_tree_child(call_node, 0) : nullptr)
        : member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    bool member_call = member
        && !call_receiver_is_module(visit->grammar, lower->unit,
                                    receiver_node);
    n00bb_link_use_site_t *dispatch_site = index_call ? index_site
        : member && caller_state
            ? link_use_site_for_node(caller_state, member) : nullptr;
    n00bb_link_use_site_t *call_site = caller_state
        ? link_use_site_for_node(caller_state, call_node) : nullptr;
    parse_sym_entry_t *symbol = index_call
        ? index_site ? index_site->symbol : nullptr
        : call_site && call_site->init_call ? call_site->symbol
        : dispatch_site && dispatch_site->symbol
            && dispatch_site->symbol->kind == PARSE_SYM_FUNCTION
            ? dispatch_site->symbol
        : callee && lower
        ? attached_symbol(visit->grammar, lower->unit, callee) : nullptr;
    bool callback_call = member_call
        && n00bb_builtin_callback_function(symbol);
    th_type *callback_type = callback_call && receiver_node && caller_state
        ? n00bb_dereferenced_type(caller_state->types,
                                  core_tree_node_value(receiver_node).type)
        : nullptr;
    n00bb_callback_contract_t *nominal_callback = callback_type
        ? n00bb_callback_contract_for_type(caller_state, callback_type)
        : nullptr;
    if (nominal_callback && !n00bb_register_callback_representation(
            context->module, callback_type))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "callback call has no registered value representation");
    bool trait_call = index_call || (member_call && !callback_call);
    parse_sym_entry_t *call_annotation_symbol =
        core_tree_node_value(call_node).sym;
    if (!index_call && !(call_site && call_site->init_call)
        && symbol && call_annotation_symbol
        && call_annotation_symbol->kind == PARSE_SYM_FUNCTION
        && call_annotation_symbol != symbol
        && !(call_annotation_symbol->scope == symbol->scope
             && core_string_eq(call_annotation_symbol->name, symbol->name)))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "call annotation target differs from the bound callee symbol");
    if (symbol && !trait_call) symbol = canonical_symbol(symbol);
    if (symbol && symbol->kind != PARSE_SYM_FUNCTION) symbol = nullptr;
    if (!symbol && trait_call)
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "trait-dispatched calls are not lowered yet");
    if (!symbol) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics,
            IR_DIAG_UNDEFINED_SYMBOL, span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics,
                        "call target did not resolve to a declared function");
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    if (index_call && n00bb_builtin_array_read_function(symbol)) {
        parse_tree_t *contents = core_tree_num_children(call_node) > 2
            ? core_tree_child(call_node, 2) : nullptr;
        parse_tree_t *subscript = child_nt(visit->grammar, contents,
                                           "expression");
        th_type *receiver_type = receiver_node
            ? n00bb_dereferenced_type(ir_module_types(context->module),
                core_tree_node_value(receiver_node).type) : nullptr;
        th_type *element = receiver_type
            && th_kind_of(receiver_type) == TH_SEQUENCE
            ? th_seq_elem(receiver_type) : nullptr;
        ir_value_t *address = subscript && element
            && th_is_closed(ir_module_types(context->module), element)
            ? n00bb_addressable_expression_address(context,
                visit->grammar, lower->unit, call_node, span) : nullptr;
        if (address) {
            ir_value_t *loaded = th_unify_in_place(
                ir_module_types(context->module), element, result_type)
                ? n00bb_emit_indirect_read(context, address, element, span)
                : nullptr;
            if (!loaded) return lower_error(context, IR_DIAG_TYPE_MISMATCH,
                span, "builtin array read could not resolve its indexed element");
            ir_shape_t *shape = ir_value_shape(address);
            if (shape && !ir_value_set_shape(loaded, shape))
                return lower_error(context, IR_DIAG_INTERNAL, span,
                    "builtin array read could not retain element shape");
            return (ir_lower_result_t){IR_LOWER_VALUE, loaded};
        }
        ir_value_t *receiver = receiver_node
            ? ir_lower_node_value(context, receiver_node) : nullptr;
        if (!receiver && receiver_node) {
            ir_value_t *receiver_address =
                n00bb_addressable_expression_address(context,
                    visit->grammar, lower->unit, receiver_node, span);
            receiver = receiver_address && receiver_type
                ? n00bb_emit_indirect_read(context, receiver_address,
                    receiver_type, span) : nullptr;
        }
        receiver = n00bb_materialize(context, receiver_node, receiver,
                                     receiver_type, span);
        ir_value_t *index = subscript
            ? ir_lower_node_value(context, subscript) : nullptr;
        if (!receiver || !index || !element
            || !th_unify_in_place(ir_module_types(context->module),
                                  element, result_type))
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "builtin array read could not resolve its indexed element");
        ir_instr_t *read = ir_lower_emit_instruction(context,
            IR_OP_SEQUENCE_GET, span);
        if (!read || !ir_lower_set_temp_dest(context, read,
            "__n00bb_array_element_", result_type)
            || !ir_instr_operand_value(read, receiver)
            || !ir_instr_operand_value(read, index))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit Array value indexing");
        if (!n00bb_attach_index_load_shape(context, receiver, result_type,
                                           read->dest))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "builtin array read could not retain element shape");
        return (ir_lower_result_t){IR_LOWER_VALUE, read->dest};
    }
    bool native_builtin = n00bb_builtin_native_function(symbol);
    bool reference_builtin = n00bb_builtin_reference_function(symbol);
    bool byte_offset_builtin = n00bb_builtin_byte_offset_function(symbol);
    bool byte_diff_builtin = n00bb_builtin_byte_diff_function(symbol);
    const n00bb_unsafe_arithmetic_t *unsafe_arithmetic =
        n00bb_builtin_unsafe_arithmetic(symbol);
    bool bit_count_builtin = n00bb_builtin_bit_count_function(symbol);
    bool gc_memory_builtin = n00bb_builtin_gc_memory_function(symbol);
    bool volatile_load_builtin = n00bb_builtin_volatile_load_function(symbol);
    bool signal_fence_builtin = n00bb_builtin_signal_fence_function(symbol);
    bool prefetch_builtin = n00bb_builtin_prefetch_function(symbol);
    bool atomic_rmw_builtin = n00bb_builtin_atomic_rmw_function(symbol);
    bool atomic_cas_builtin = n00bb_builtin_atomic_cas_function(symbol);
    bool atomic_access_builtin = n00bb_builtin_atomic_access_function(symbol);
    bool option_builtin = n00bb_builtin_option_function(symbol);
    n00bb_link_analysis_t *analysis = caller_state
        ? caller_state->link_analysis : nullptr;
    if (trait_call && !reference_builtin && !option_builtin) {
        symbol = linkable_trait_requirement(analysis, lower->unit,
                                            dispatch_site, symbol);
        if (!symbol)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "linkable trait call has no accessible method requirement");
    }
    bool semantic_trait_call = trait_call && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("trait"));
    bool builtin_trait_call = semantic_trait_call && parse_sym_binding(
        symbol, CORE_STRING_STATIC("n00bb.builtin-trait-method"));
    if (option_builtin
        && core_string_eq(symbol->name, CORE_STRING_STATIC("__option_get"))
        && !n00bb_option_value_proven(lower, visit->grammar, receiver_node))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "an option's value requires it to be proven non-nil by the "
            "active branch");
    bool no_function_body = native_builtin
        || reference_builtin || byte_offset_builtin || byte_diff_builtin
        || unsafe_arithmetic
        || bit_count_builtin
        || gc_memory_builtin
        || volatile_load_builtin
        || signal_fence_builtin
        || prefetch_builtin
        || atomic_rmw_builtin
        || atomic_cas_builtin
        || atomic_access_builtin
        || option_builtin
        || callback_call || builtin_trait_call;
    core_translation_unit_t *callee_unit = !no_function_body && lower
        ? unit_for_symbol(visit->grammar, lower->units, symbol) : nullptr;
    if (!no_function_body && !callee_unit)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target is outside the lowered import closure");
    n00bb_tu_state_t *callee_state = callee_unit
        ? tu_state(callee_unit) : nullptr;
    ir_func_t *callee_function = callee_state && symbol
        ? link_ir_function_for_symbol(
            tu_state(lower->unit)->link_analysis, symbol)
        : nullptr;
    if (!callee_function && trait_call && symbol
        && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-operator"))) {
        th_type *receiver_type = receiver_node
            ? core_tree_node_value(receiver_node).type : nullptr;
        callee_function = n00bb_closed_builtin_operator(context, symbol,
            symbol, receiver_type);
        if (callee_function) {
            callee_unit = lower->unit;
            callee_state = caller_state;
        }
    }
    if (!no_function_body && !callee_function)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target has no canonical IR binding in the link unit");
    ir_value_t *callee_value = callback_call && receiver_node
        ? n00bb_promoted_operand(context, receiver_node,
              ir_lower_node_value(context, receiver_node), false, span)
        : callee_function
            ? ir_func_address(callee_function->owner, callee_function)
                        : nullptr;
    if (callback_call && !callee_value)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "callback receiver has no lowered reference value");
    if (!no_function_body && !callee_value)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                           "call target has no lowered function declaration");
    th_type *callable = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    if (callback_call && callee_value) {
        if (nominal_callback && nominal_callback->call)
            callable = n00bb_specialize_callback_role(
                ir_module_types(context->module), nominal_callback,
                callee_value->type, nominal_callback->call->type);
        else {
            th_type *reference_type = th_resolve(callee_value->type);
            callable = reference_type
                && th_kind_of(reference_type) == TH_INDIRECTION
                ? th_ind_referent(reference_type) : nullptr;
        }
    }
    th_type *signature = th_type_clone(ir_module_types(context->module),
                                       callable);
    if (!signature || th_kind_of(signature) != TH_FUNCTION)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "call target does not have a function type");
    th_type *signature_return = th_func_ret(signature);
    th_type *call_constraint = signature_return
        ? function_with_return_type(ir_module_types(context->module),
                                    signature, result_type) : nullptr;
    signature = call_constraint
        ? th_unify_in_place(ir_module_types(context->module), signature,
                            call_constraint) : nullptr;
    signature_return = signature ? th_func_ret(signature) : nullptr;
    if (!signature || !signature_return)
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                           "call result does not match the resolved function return type");
    if (builtin_trait_call) {
        core_buffer_t requirement_name = {0};
        core_buffer_printf(&requirement_name, "__trait_req_%s_%.*s",
            dispatch_site && dispatch_site->resolved_trait_name
                ? dispatch_site->resolved_trait_name
                : symbol->scope->name.data,
            (int)symbol->name.u8_bytes, symbol->name.data);
        ir_func_t *requirement = requirement_name.data
            ? ir_func_find(context->module, requirement_name.data) : nullptr;
        if (!requirement && requirement_name.data) {
            th_type *requirement_type = th_type_clone(
                ir_module_types(context->module), callable);
            requirement = requirement_type
                ? ir_func_add(context->module, requirement_name.data,
                              requirement_type, IR_LINK_EXTERN) : nullptr;
            if (requirement && !th_is_closed(ir_module_types(context->module),
                    callable) && !ir_func_set_callable_contract(requirement,
                                                               callable))
                requirement = nullptr;
        }
        core_buffer_release(&requirement_name);
        callee_value = requirement
            ? ir_func_address(context->module, requirement) : nullptr;
        if (!callee_value)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not retain builtin trait method requirement");
    }
    if (native_builtin) {
        const char *ir_name = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.native"));
        ir_func_t *function = ir_func_find(context->module, ir_name);
        if (!function) {
            th_type *declaration = th_type_clone(
                ir_module_types(context->module), callable);
            function = declaration ? ir_func_add(context->module, ir_name,
                                                  declaration, IR_LINK_EXTERN)
                                   : nullptr;
        }
        if (function && !ir_annot_set(lower->ir_context,
                &function->annots, (ir_annot_t){
                    .key = "n00bb.abi.native",
                    .kind = IR_ANNOT_STR,
                    .s = ir_name,
                })) return lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not annotate a native function");
        if (function && parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.native.collection-free"))
            && !ir_annot_set(lower->ir_context, &function->annots,
                (ir_annot_t){
                    .key = "n00bb.native.collection-free",
                    .kind = IR_ANNOT_BOOL,
                    .b = true,
                })) return lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not annotate a collection-free native");
        callee_value = function
            ? ir_func_address(context->module, function) : nullptr;
        if (!callee_value)
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not declare a native function");
    }
    core_tree_node_value(call_node).type = signature_return;
    parse_tree_t *args = call_label
        && call_label->kind == PARSE_CF_CALL ? call_label->then_body
        : call_arguments_node(visit->grammar, call_node);
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    core_list_t(n00bb_copyout_t) copyouts =
        core_list_new(n00bb_copyout_t);
    size_t parameter_count = th_func_nparams(signature);
    parse_tree_t *formal_signature = child_nt(visit->grammar,
        symbol->decl_node, "func-type-signature");
    if (!formal_signature)
        formal_signature = child_nt(visit->grammar, symbol->decl_node,
                                    "trait-func-type-signature");
    parse_tree_t *formal_params = child_nt(visit->grammar,
        formal_signature, "param-decl");
    if (!formal_params)
        formal_params = child_nt(visit->grammar, formal_signature,
                                 "trait-param-decl");
    parse_tree_t **argument_by_parameter = core_alloc_array(
        parse_tree_t *, parameter_count);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided = core_alloc_array(bool, parameter_count);
    bool mapped = true;
    if (trait_call && receiver_node) {
        if (!parameter_count) mapped = false;
        else {
            argument_by_parameter[0] = receiver_node;
            provided[0] = true;
            core_list_push(actuals, ((n00bb_call_actual_t){
                receiver_node, 0}));
        }
    }
    parse_tree_t *positional = child_nt(visit->grammar, args,
                                        "positional-args");
    core_list_t(n00bb_tree_ptr_t) positional_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (index_call) {
        parse_tree_t *contents = core_tree_num_children(call_node) > 2
            ? core_tree_child(call_node, 2) : nullptr;
        if (nt_is(visit->grammar, contents, "index-contents")) {
            parse_tree_t *index = child_nt(visit->grammar, contents,
                                           "expression");
            if (index) core_list_push(positional_nodes, index);
        }
        else if (nt_is(visit->grammar, contents, "slice-contents")) {
            for (size_t i = 0; i < core_tree_num_children(contents); i++) {
                parse_tree_t *child = core_tree_child(contents, i);
                if (nt_is(visit->grammar, child, "expression"))
                    core_list_push(positional_nodes, child);
            }
        }
        if (core_string_eq(index_trait, CORE_STRING_STATIC("Index_Write"))
            || core_string_eq(index_trait,
                              CORE_STRING_STATIC("Index_Slice_Write"))) {
            parse_tree_t *parent = core_tree_node_value(call_node).parent;
            while (parent && !nt_is(visit->grammar, parent, "assign-stmt"))
                parent = core_tree_node_value(parent).parent;
            const parse_rule_t *rule = parent
                ? parse_tree_rule(visit->grammar, parent) : nullptr;
            parse_annotation_t *assignment = rule
                ? parse_rule_find_annot(visit->grammar, rule,
                                        PARSE_ANNOT_ASSIGNS) : nullptr;
            parse_tree_t *value = assignment
                && parse_child_ref_is_set(assignment->value_ref)
                    ? parse_tree_resolve_child_ref(visit->grammar, parent,
                                                   assignment->value_ref)
                    : nullptr;
            if (value) core_list_push(positional_nodes, value);
        }
    }
    else if (positional)
        collect_positional_nodes(visit->grammar, positional,
                                 &positional_nodes);
    size_t positional_offset = trait_call && receiver_node ? 1 : 0;
    for (size_t i = 0; mapped && i < positional_nodes.len; i++) {
        size_t index = positional_offset + i;
        if (index >= parameter_count) {
            if (!th_func_rest(signature) || trait_call) {
                mapped = false;
                break;
            }
            core_list_push(actuals, ((n00bb_call_actual_t){
                positional_nodes.data[i], SIZE_MAX}));
            continue;
        }
        if (provided[index]) {
            mapped = false;
            break;
        }
        argument_by_parameter[index] = positional_nodes.data[i];
        provided[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){
            positional_nodes.data[i], index}));
    }
    core_list_free(positional_nodes);
    parse_tree_t *keyword_args = child_nt(visit->grammar, args,
                                          "keyword-args");
    core_list_t(n00bb_tree_ptr_t) keyword_nodes =
        core_list_new(n00bb_tree_ptr_t);
    if (keyword_args)
        collect_keyword_nodes(visit->grammar, keyword_args, &keyword_nodes);
    for (size_t i = 0; mapped && i < keyword_nodes.len; i++) {
        core_string_t name = call_keyword_name(keyword_nodes.data[i]);
        size_t index = 0;
        bool has_formal = reference_builtin
            ? n00bb_builtin_parameter_named(symbol, name, &index)
            : function_parameter_named(visit->grammar, symbol,
                formal_params, name, &index);
        parse_tree_t *argument = child_nt(visit->grammar,
            keyword_nodes.data[i], "expression");
        if (!has_formal || !argument || index >= parameter_count) {
            lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                        "named argument does not match a parameter");
            mapped = false;
            break;
        }
        if (provided[index]) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, keyword_nodes.data[i]),
                "a parameter is supplied more than once");
            mapped = false;
            break;
        }
        argument_by_parameter[index] = argument;
        provided[index] = true;
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    core_list_free(keyword_nodes);
    size_t missing_required = 0;
    for (size_t i = 0; i < parameter_count; i++) {
        uint8_t flags = 0;
        if ((!reference_builtin
                && !function_parameter_info(visit->grammar, symbol,
                    formal_params, i, nullptr, nullptr, nullptr, &flags,
                    nullptr))
            || !(flags & 1u)) {
            if (!provided[i]) missing_required++;
        }
    }
    if (!mapped || missing_required) {
        if (!mapped && !ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                        "call argument does not match a parameter");
        if (mapped)
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "call omits a required parameter");
        core_list_free(actuals);
        core_list_free(values);
        core_list_free(rest_values);
        core_list_free(copyouts);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    th_type **argument_types = actuals.len
        ? core_alloc_array(th_type *, actuals.len) : nullptr;
    if (actuals.len && !argument_types) {
        core_list_free(actuals);
        core_list_free(values);
        core_list_free(rest_values);
        core_list_free(copyouts);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not allocate call argument type context");
    }
    for (size_t i = 0; i < actuals.len; i++) {
        parse_tree_t *argument = actuals.data[i].node;
        size_t index = actuals.data[i].parameter_index;
        const th_param *formal = index == SIZE_MAX
            ? th_func_rest(signature) : th_func_param(signature, index);
        th_type *actual = core_tree_is_leaf(argument) ? nullptr
            : core_tree_node_value(argument).type;
        argument_types[i] = formal
            && th_mode_is_input_only(formal->mode) && actual
            && !th_is_closed(ir_module_types(context->module), actual)
            && !core_tree_node_value(argument).integer_literal_origin
            && !n00bb_node_record_literal_origin(argument)
                ? actual : nullptr;
    }
    bool collected = true;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        if (index == SIZE_MAX) {
            const th_param *rest = th_func_rest(signature);
            parse_tree_t *argument = actuals.data[i].node;
            ir_value_t *value = nullptr;
            if (rest && rest->mode == TH_INOUT) {
                n00bb_lower_context_t *lower = context->user;
                parse_tree_t *root = n00bb_addressable_root_lvalue(
                    visit->grammar, argument);
                parse_sym_entry_t *root_symbol = root && lower
                    ? attached_symbol(visit->grammar, lower->unit, root)
                    : nullptr;
                parse_tree_t *member = member_access_node(visit->grammar,
                                                           argument);
                parse_sym_entry_t *resolved = member && lower
                    ? attached_symbol(visit->grammar, lower->unit, argument)
                    : nullptr;
                parse_sym_phase_t phase = n00bb_compile_phase();
                bool writable = root_symbol
                    && parse_sym_can_write(root_symbol, phase);
                if (resolved && resolved->is_field)
                    writable = writable
                        && resolved->mutability == PARSE_SYM_MUTABLE;
                value = writable && lower
                    ? n00bb_addressable_expression_address(context,
                        visit->grammar, lower->unit, argument,
                        lower_span_for_tree(context, argument)) : nullptr;
                th_type *referent = value
                    ? th_ind_referent(value->type) : nullptr;
                if (!referent || !th_unify_in_place(
                        ir_module_types(context->module), rest->type,
                        argument_types[i] ? argument_types[i] : referent)
                    || !th_unify_probe(ir_module_types(context->module),
                                       rest->type, referent)) {
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, argument),
                        "modify variadic arguments must be writable storage locations of the element type");
                    collected = false;
                    break;
                }
            }
            else value = ir_lower_node_value(context, argument);
            th_type *argument_type = argument_types[i]
                ? argument_types[i] : value ? value->type : nullptr;
            if (!rest || !value || !th_unify_in_place(
                    ir_module_types(context->module),
                    rest->mode == TH_INOUT
                        ? th_ind(ir_module_types(context->module), 0,
                                 rest->type)
                        : rest->type,
                    rest->mode == TH_INOUT ? value->type : argument_type)) {
                if (rest && value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, argument),
                        "variadic argument does not match its element type");
                collected = false;
                break;
            }
            if (rest->mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower ? lower->unit : nullptr, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            core_list_push(rest_values, value);
            continue;
        }
        const th_param *parameter = th_func_param(signature, index);
        parse_tree_t *argument = actuals.data[i].node;
        th_mode mode = parameter ? parameter->mode : TH_IN;
        function_parameter_info(visit->grammar, symbol, formal_params,
            index, nullptr, nullptr, nullptr, nullptr, &mode);
        if (!parameter || !argument) {
            collected = false;
            break;
        }
        parse_sym_entry_t *formal = nullptr;
        function_parameter_info(visit->grammar, symbol, formal_params,
            index, &formal, nullptr, nullptr, nullptr, nullptr);
        ir_shape_t *required_shape = formal
            ? parse_sym_binding(formal,
                CORE_STRING_STATIC("ir.array-shape"))
            : function_parameter_shape(symbol, index);
        if (th_mode_is_input_only(mode)) {
            if (!n00bb_nominal_value_as(context, visit, parameter->type,
                                        argument)) {
                collected = false;
                break;
            }
            ir_value_t *value = n00bb_materialize_node(context, argument,
                parameter->type, lower_span_for_tree(context, argument));
            th_type *argument_type = value
                && ir_type_eq(context->module, value->type, parameter->type)
                ? value->type
                : argument_types[i]
                    ? argument_types[i] : value ? value->type : nullptr;
            if (!value || !th_unify_in_place(
                    ir_module_types(context->module), parameter->type,
                    argument_type)
                || !th_unify_probe(ir_module_types(context->module),
                                   parameter->type, value->type)) {
                if (value)
                    lower_error(context, IR_DIAG_TYPE_ARGS,
                        lower_span_for_tree(context, argument),
                        "argument type does not match its parameter");
                collected = false;
                break;
            }
            if (mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower ? lower->unit : nullptr, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            value_by_parameter[index] = value;
            continue;
        }
        if (mode != TH_OUT && mode != TH_INOUT) {
            collected = false;
            break;
        }
        n00bb_out_location_t location = n00bb_out_argument_location(context,
            visit->grammar, argument, parameter->type, span);
        parse_sym_entry_t *arg_symbol = location.symbol;
        ir_local_t *target = location.local;
        ir_value_t *global = location.global;
        ir_value_t *address = location.address;
        parse_tree_t *index_target = location.index_target;
        parse_tree_t *lvalue = location.lvalue;
        th_type *target_type = location.type;
        bool writable = location.writable;
        bool type_matches = target_type && parameter->type
            && th_unify_in_place(ir_module_types(context->module),
                                 parameter->type,
                                 argument_types[i]
                                     && !location.through_reference
                                     ? argument_types[i] : target_type)
            && th_unify_probe(ir_module_types(context->module),
                              parameter->type, target_type);
        if (!target_type || !writable
            || !type_matches) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "out and modify arguments must be writable storage locations of the parameter type");
            collected = false;
            break;
        }
        bool aliases = false;
        for (size_t previous = 0; arg_symbol && lvalue
                && previous < copyouts.len; previous++)
            aliases |= copyouts.data[previous].symbol == arg_symbol
                && copyouts.data[previous].local == target
                && copyouts.data[previous].global == global;
        if (aliases) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, argument),
                "the same variable cannot be passed to multiple out or modify parameters");
            collected = false;
            break;
        }
        ir_value_t *initial = nullptr;
        if (mode == TH_INOUT) {
            initial = n00bb_out_location_read(context, argument, &location,
                                              span);
            if (!initial || !n00bb_emit_sequence_shape_check(context,
                    initial, required_shape,
                    lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
        }
        value_by_parameter[index] = initial;
        size_t output_index = 0;
        for (size_t previous = 0; previous < index; previous++) {
            const th_param *prior = th_func_param(signature, previous);
            if (prior && th_mode_produces_output(prior->mode)) output_index++;
        }
        core_list_push(copyouts,
            ((n00bb_copyout_t){.symbol = arg_symbol,
                               .local = target,
                               .global = global,
                               .address = address,
                               .grammar = visit->grammar,
                               .site_node = visit->node,
                               .index_target = index_target,
                               .source_node = argument,
                               .output_index = output_index,
                               .required_shape = required_shape}));
    }
    for (size_t i = 0; collected && i < parameter_count; i++) {
        parse_sym_entry_t *formal = nullptr;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool has_formal = builtin_trait_call || reference_builtin || byte_offset_builtin
            || byte_diff_builtin || unsafe_arithmetic || bit_count_builtin
            || gc_memory_builtin
            || volatile_load_builtin
            || prefetch_builtin
            || atomic_rmw_builtin || atomic_cas_builtin
            || atomic_access_builtin
            || native_builtin
            || function_parameter_info(visit->grammar, symbol,
                formal_params, i, &formal, nullptr, nullptr, &flags, &mode);
        const th_param *parameter = th_func_param(signature, i);
        if (!has_formal || !parameter) {
            collected = false;
            break;
        }
        if (flags & 1u) {
            if (!th_mode_is_input_only(parameter->mode)) {
                collected = false;
                break;
            }
            if (!provided[i]) {
                core_list_push(values, nullptr);
                continue;
            }
            ir_value_t *value = value_by_parameter[i];
            if (!value) {
                collected = false;
                break;
            }
            ir_shape_t *required_shape = formal
                ? parse_sym_binding(formal,
                    CORE_STRING_STATIC("ir.array-shape"))
                : function_parameter_shape(symbol, i);
            parse_tree_t *shape_source = argument_by_parameter[i]
                ? argument_by_parameter[i]
                : formal ? child_nt(visit->grammar, formal->decl_node,
                                    "expression") : nullptr;
            if (th_mode_is_input_only(parameter->mode)
                && ((!n00bb_attach_expression_shape(context,
                        visit->grammar, shape_source, value)
                        && shape_source)
                    || !n00bb_emit_sequence_shape_check(context, value,
                    required_shape, formal && formal->decl_node
                        ? lower_span_for_tree(context, formal->decl_node)
                        : span))) {
                collected = false;
                break;
            }
            core_list_push(values, value);
        }
        else if (mode == TH_OUT) {
            core_list_push(values, nullptr);
        }
        else if (value_by_parameter[i]) {
            ir_shape_t *required_shape = formal
                ? parse_sym_binding(formal,
                    CORE_STRING_STATIC("ir.array-shape"))
                : function_parameter_shape(symbol, i);
            if (th_mode_is_input_only(mode)
                && (!n00bb_attach_expression_shape(context,
                        visit->grammar, argument_by_parameter[i],
                        value_by_parameter[i])
                    || !n00bb_emit_sequence_shape_check(context,
                        value_by_parameter[i], required_shape,
                        argument_by_parameter[i]
                            ? lower_span_for_tree(context,
                                argument_by_parameter[i]) : span))) {
                collected = false;
                break;
            }
            core_list_push(values, value_by_parameter[i]);
        }
        else {
            collected = false;
        }
    }
    for (size_t i = 0; collected && i < rest_values.len; i++)
        core_list_push(values, rest_values.data[i]);
    core_free(argument_types);
    core_list_free(actuals);
    core_list_free(rest_values);
    core_free(argument_by_parameter);
    core_free(value_by_parameter);
    core_free(provided);
    if (!collected) {
        core_list_free(values);
        core_list_free(copyouts);
        if (!ir_diags_failed(context->diagnostics))
            lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                "call arguments could not be lowered for their parameter modes");
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    if (native_builtin && core_string_eq(symbol->name,
            CORE_STRING_STATIC("__array_length"))) {
        bool valid = values.len == 1 && values.data[0]
            && !copyouts.len;
        ir_instr_t *length = valid
            ? ir_lower_emit_instruction(context, IR_OP_SEQUENCE_LENGTH, span)
            : nullptr;
        bool emitted = length
            && ir_lower_set_temp_dest(context, length, "__n00bb_",
                                       result_type)
            && ir_instr_operand_value(length, values.data[0]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, length->dest}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH, span,
                "Array length requires one sequence value");
    }
    th_ctx *types = ir_module_types(context->module);
    th_type *fact_failure = nullptr;
    bool fact_failure_conflict = false;
    if (builtin_trait_call && !lower->tries.len) {
        th_type *current_failure = th_func_error(signature);
        if (current_failure
            && th_kind_of(th_resolve(current_failure)) == TH_VAR) {
            th_type *enclosing_failure = th_func_error(
                lower->function_symbol ? lower->function_symbol->type
                                       : context->function->type);
            th_type *bound_failure = enclosing_failure
                ? th_unify_in_place(types, current_failure,
                    th_type_clone(types, enclosing_failure)) : nullptr;
            signature = bound_failure
                ? function_with_error_channel(types, signature,
                                              bound_failure) : nullptr;
            if (!signature) {
                core_list_free(values);
                core_list_free(copyouts);
                return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "builtin trait failure does not match the enclosing function");
            }
        }
    }
    for (size_t i = 0; analysis
         && i < analysis->trait_specializations.len; i++) {
        n00bb_trait_specialization_t *fact =
            analysis->trait_specializations.data[i];
        if (!fact || fact->instantiation_site != call_node
            || fact->generic_function != symbol)
            continue;
        if (!fact->failure_type) continue;
        fact_failure = fact_failure
            ? th_unify(types, fact_failure, fact->failure_type)
            : th_type_clone(types, fact->failure_type);
        if (!fact_failure) {
            fact_failure_conflict = true;
            break;
        }
    }
    if (fact_failure_conflict) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "generic trait specialization has no consistent failure channel");
    }
    if (fact_failure) {
        th_type *current_failure = th_func_error(signature);
        th_type *specialized_failure = current_failure
            ? th_unify(types, current_failure, fact_failure)
            : fact_failure;
        signature = specialized_failure
            ? function_with_error_channel(types, signature,
                                          specialized_failure)
            : nullptr;
        if (!signature) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "call failure type does not match its generic trait specialization");
        }
    }
    th_type *cached_failure = parse_tree_error_type(call_node);
    if (cached_failure) {
        th_type *failure_copy = th_type_clone(types, cached_failure);
        th_type *specialized_failure = failure_copy
            ? th_unify_in_place(types, th_func_error(signature),
                                failure_copy) : nullptr;
        signature = specialized_failure
            ? function_with_error_channel(types, signature,
                                          specialized_failure)
            : nullptr;
        if (!signature) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "call failure type does not match its cached expression channel");
        }
    }
    th_type *error_type = th_func_error(signature);
    bool has_error = error_type && !th_is_nil(error_type);
    bool propagated = n00bb_call_is_error_propagated(visit->grammar, call_node);
    if (has_error != propagated) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            has_error
                ? "a call with an error channel must be used with postfix ! and its return type must match"
                : "postfix ! requires a failable expression; this call is infallible");
    }
    th_type *call_result_type = has_error
        ? ir_call_outcome_type(context->module, signature)
        : signature_return;
    ir_instr_t *instruction = nullptr;
    if (semantic_trait_call) {
        if (dispatch_site && dispatch_site->closed_dispatch) {
            instruction = n00bb_emit_closed_trait_call(context, lower,
                dispatch_site->closed_dispatch, visit->grammar, symbol,
                formal_params, parameter_count, signature, call_result_type,
                &values, &copyouts, span);
        }
        else {
            ir_value_t *receiver_value = values.len ? values.data[0] : nullptr;
            if (!receiver_value || !callee_value) {
                core_list_free(values);
                core_list_free(copyouts);
                return lower_error(context, IR_DIAG_INTERNAL, span,
                    !receiver_value ? "trait receiver has no lowered value"
                                    : "trait requirement has no lowered address");
            }
            callee_value = n00bb_select_trait_method(context,
                visit->grammar, callee_unit, symbol, signature,
                receiver_value, callee_value, span);
        }
        if ((dispatch_site && dispatch_site->closed_dispatch && !instruction)
            || ((!dispatch_site || !dispatch_site->closed_dispatch)
                && !callee_value)) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not select the semantic trait method requirement");
        }
    }
    if (option_builtin) {
        ir_op_t option_op = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_is_nil"))
            ? IR_OP_OPTION_IS_NIL
            : core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_get"))
                ? IR_OP_OPTION_GET : IR_OP_OPTION_GET_OR_ELSE;
        size_t expected = option_op == IR_OP_OPTION_GET_OR_ELSE ? 2 : 1;
        bool valid = values.len == expected && !copyouts.len;
        ir_instr_t *instruction = valid
            ? ir_lower_emit_instruction(context, option_op, span) : nullptr;
        bool emitted = instruction
            && ir_lower_set_temp_dest(context, instruction, "__n00bb_",
                                      call_result_type);
        for (size_t i = 0; emitted && i < values.len; i++)
            emitted = ir_instr_operand_value(instruction, values.data[i]);
        if (emitted && option_op != IR_OP_OPTION_IS_NIL)
            emitted = n00bb_set_reference_access(lower->ir_context,
                instruction->dest, call_result_type);
        core_list_free(values);
        core_list_free(copyouts);
        if (!emitted)
            return lower_error(context, valid ? IR_DIAG_INTERNAL
                                              : IR_DIAG_TYPE_ARGS,
                span, "option builtin arguments do not match its operation");
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    if (reference_builtin) {
        size_t expected = core_string_eq(symbol->name,
                            CORE_STRING_STATIC("ref_read")) ? 1 : 2;
        ir_value_t *address = values.len ? values.data[0] : nullptr;
        th_type *referent = address
            ? th_ind_referent(address->type) : nullptr;
        bool valid = values.len == expected && referent
            && !copyouts.len;
        if (valid && expected == 1)
            valid = ir_type_eq(context->module, result_type, referent);
        if (valid && expected == 2)
            valid = th_unify_in_place(ir_module_types(context->module),
                referent, values.data[1]->type) != nullptr;
        if (!valid) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "reference builtin arguments do not match its reference target");
        }
        if (expected == 1) {
            ir_value_t *loaded = n00bb_emit_indirect_read(context, address,
                                                           referent, span);
            core_list_free(values);
            core_list_free(copyouts);
            return loaded
                ? (ir_lower_result_t){IR_LOWER_VALUE, loaded}
                : lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not emit a read through the reference");
        }
        bool written = n00bb_emit_indirect_write(context, address,
                                                  values.data[1], span);
        core_list_free(values);
        core_list_free(copyouts);
        return written
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit a write through the reference");
    }
    if (byte_offset_builtin) {
        th_ctx *types = ir_module_types(context->module);
        th_type *byte = th_name(types, "byte");
        th_type *indexable = byte ? th_ind(types, TH_INDEXABLE, byte)
                                  : nullptr;
        bool valid = values.len == 2 && !copyouts.len && indexable
            && th_ind_referent(values.data[0]->type)
            && ir_type_eq(context->module, call_result_type,
                          values.data[0]->type);
        if (!valid) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "byte offset requires a byte reference and an integer offset");
        }
        ir_instr_t *cast = ir_lower_emit_instruction(context,
            IR_OP_BITCAST, span);
        bool emitted = cast && ir_lower_set_temp_dest(context, cast,
            "__n00bb_", indexable)
            && ir_instr_operand_value(cast, values.data[0]);
        ir_instr_t *offset = emitted ? ir_lower_emit_instruction(context,
            IR_OP_OFFSET, span) : nullptr;
        emitted = offset && ir_lower_set_temp_dest(context, offset,
            "__n00bb_", indexable)
            && ir_instr_operand_value(offset, cast->dest)
            && ir_instr_operand_value(offset, values.data[1]);
        ir_instr_t *result = emitted ? ir_lower_emit_instruction(context,
            IR_OP_BITCAST, span) : nullptr;
        emitted = result && ir_lower_set_temp_dest(context, result,
            "__n00bb_", call_result_type)
            && ir_instr_operand_value(result, offset->dest);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, result->dest}
            : lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit byte reference offset");
    }
    if (byte_diff_builtin) {
        bool valid = values.len == 2 && !copyouts.len
            && th_ind_referent(values.data[0]->type)
            && ir_type_eq(context->module, values.data[0]->type,
                          values.data[1]->type);
        ir_instr_t *difference = valid
            ? ir_lower_emit_instruction(context, IR_OP_DIFF, span) : nullptr;
        bool emitted = difference
            && ir_lower_set_temp_dest(context, difference, "__n00bb_",
                                      call_result_type)
            && ir_instr_operand_value(difference, values.data[0])
            && ir_instr_operand_value(difference, values.data[1]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, difference->dest}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "byte difference requires two byte references");
    }
    if (unsafe_arithmetic && unsafe_arithmetic->unary) {
        bool valid = values.len == 1 && !copyouts.len
            && is_integer_type(call_result_type)
            && ir_type_eq(context->module, values.data[0]->type,
                          call_result_type);
        ir_value_t *result = valid
            ? n00bb_emit_wrapping(context, unsafe_arithmetic->op,
                                  values.data[0], call_result_type, span)
            : nullptr;
        core_list_free(values);
        core_list_free(copyouts);
        return result
            ? (ir_lower_result_t){IR_LOWER_VALUE, result}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "unsafe_not requires one integer operand");
    }
    if (unsafe_arithmetic) {
        bool valid = values.len == 2 && !copyouts.len
            && is_integer_type(call_result_type)
            && ir_type_eq(context->module, values.data[0]->type,
                          call_result_type)
            && (unsafe_arithmetic->shift
                || ir_type_eq(context->module, values.data[1]->type,
                              call_result_type));
        ir_value_t *right = valid && unsafe_arithmetic->shift
            ? n00bb_lower_shift_count(context, nullptr, values.data[1],
                                      call_result_type, span)
            : valid ? values.data[1] : nullptr;
        ir_value_t *result = right
            ? n00bb_emit_binary_ir_with(context, unsafe_arithmetic->op, true,
                call_result_type, values.data[0], right, span)
            : nullptr;
        core_list_free(values);
        core_list_free(copyouts);
        return result
            ? (ir_lower_result_t){IR_LOWER_VALUE, result}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "unsafe arithmetic requires integer operands of one type");
    }
    if (bit_count_builtin) {
        th_type *uint_type = th_name(ir_module_types(context->module), "uint");
        th_type *u32_type = th_name(ir_module_types(context->module), "u32");
        bool valid = values.len == 1 && !copyouts.len && uint_type
            && u32_type && ir_type_eq(context->module, call_result_type,
                                      u32_type)
            && ir_type_eq(context->module, values.data[0]->type, uint_type);
        ir_op_t op = core_string_eq(symbol->name,
            CORE_STRING_STATIC("__count_leading_zeros"))
                ? IR_OP_CLZ : IR_OP_CTZ;
        ir_instr_t *count = valid
            ? ir_lower_emit_instruction(context, op, span) : nullptr;
        bool emitted = count
            && ir_lower_set_temp_dest(context, count, "__n00bb_",
                                      call_result_type)
            && ir_instr_operand_value(count, values.data[0]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, count->dest}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "bit count requires one uint operand");
    }
    if (gc_memory_builtin) {
        bool valid = values.len == 3 && !copyouts.len
            && th_ind_referent(values.data[0]->type);
        bool copy = core_string_eq(symbol->name,
            CORE_STRING_STATIC("__gc_memcpy"));
        ir_instr_t *memory = valid
            ? ir_lower_emit_instruction(context,
                copy ? IR_OP_MEMCPY : IR_OP_MEMSET, span) : nullptr;
        bool emitted = memory
            && ir_instr_operand_value(memory, values.data[0])
            && ir_instr_operand_value(memory, values.data[1])
            && ir_instr_operand_value(memory, values.data[2]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, copy
                    ? "memory copy requires destination, source, and length"
                    : "memory fill requires an address, byte, and length");
    }
    if (volatile_load_builtin) {
        bool valid = values.len == 1 && !copyouts.len
            && th_ind_referent(values.data[0]->type)
            && ir_type_eq(context->module, call_result_type,
                          th_ind_referent(values.data[0]->type));
        ir_instr_t *load = valid
            ? ir_lower_emit_instruction(context, IR_OP_LOAD, span) : nullptr;
        bool emitted = load && ir_lower_set_temp_dest(context, load,
            "__n00bb_", call_result_type)
            && ir_instr_operand_value(load, values.data[0])
            && ir_instr_add_mod(load, "volatile")
            && n00bb_set_reference_access(lower->ir_context,
                load->dest, call_result_type);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, load->dest}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "volatile load requires a reference");
    }
    if (signal_fence_builtin) {
        bool valid = !values.len && !copyouts.len;
        ir_instr_t *fence = valid
            ? ir_lower_emit_instruction(context, IR_OP_SIGNAL_FENCE, span)
            : nullptr;
        core_list_free(values);
        core_list_free(copyouts);
        return fence
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "signal fence takes no arguments");
    }
    if (prefetch_builtin) {
        bool valid = values.len == 1 && !copyouts.len
            && th_ind_referent(values.data[0]->type);
        ir_instr_t *prefetch = valid
            ? ir_lower_emit_instruction(context, IR_OP_PREFETCH, span)
            : nullptr;
        bool emitted = prefetch
            && ir_instr_operand_value(prefetch, values.data[0]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_error(context, valid ? IR_DIAG_INTERNAL
                                         : IR_DIAG_TYPE_MISMATCH,
                span, "prefetch requires one reference");
    }
    if (atomic_rmw_builtin) {
        th_type *storage = values.len == 2
            ? th_ind_referent(values.data[0]->type) : nullptr;
        th_type *element = storage
            ? n00bb_atomic_element_type(storage) : nullptr;
        bool valid = element && !copyouts.len
            && ir_type_eq(context->module, element, values.data[1]->type)
            && ir_type_eq(context->module, element, call_result_type);
        if (!valid) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "atomic operation needs a matching reference and value");
        }
        ir_instr_t *rmw = ir_lower_emit_instruction(context,
            IR_OP_ATOMIC_RMW, span);
        bool emitted = rmw && ir_lower_set_temp_dest(context, rmw,
            "__n00bb_", element)
            && ir_instr_operand_value(rmw, values.data[0])
            && ir_instr_operand_name(rmw,
                core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_exchange"))
                    || core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_exchange_acquire"))
                    || core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_exchange_relaxed"))
                    ? "xchg" : core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_fetch_add"))
                        || core_string_eq(symbol->name,
                            CORE_STRING_STATIC("__atomic_fetch_add_relaxed"))
                            ? "add" : core_string_eq(symbol->name,
                                CORE_STRING_STATIC("__atomic_fetch_sub"))
                                    ? "sub" : core_string_eq(symbol->name,
                                        CORE_STRING_STATIC("__atomic_fetch_and"))
                                            ? "and" : "or")
            && ir_instr_operand_value(rmw, values.data[1])
            && (!core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_or_release"))
                || ir_instr_add_mod(rmw, "release"))
            && ((!core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_or_relaxed"))
                && !core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_exchange_relaxed"))
                && !core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_add_relaxed")))
                || ir_instr_add_mod(rmw, "relaxed"));
        if (emitted && core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_exchange_acquire")))
            emitted = ir_instr_add_mod(rmw, "acquire");
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, rmw->dest}
            : lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit atomic operation");
    }
    if (atomic_cas_builtin) {
        th_type *storage = values.len == 3
            ? th_ind_referent(values.data[0]->type) : nullptr;
        th_type *element = storage
            ? n00bb_atomic_element_type(storage) : nullptr;
        bool valid = element && !copyouts.len
            && ir_type_eq(context->module, element, values.data[1]->type)
            && ir_type_eq(context->module, element, values.data[2]->type)
            && ir_type_eq(context->module, call_result_type,
                          th_name(ir_module_types(context->module), "bool"));
        if (!valid) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "atomic compare-exchange needs a matching atomic reference and values");
        }
        ir_instr_t *cas = ir_lower_emit_instruction(context,
            IR_OP_ATOMIC_CAS, span);
        bool emitted = cas && ir_lower_set_temp_dest(context, cas,
            "__n00bb_", call_result_type)
            && ir_instr_operand_value(cas, values.data[0])
            && ir_instr_operand_value(cas, values.data[1])
            && ir_instr_operand_value(cas, values.data[2]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, cas->dest}
            : lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit atomic compare-exchange");
    }
    if (atomic_access_builtin) {
        bool loading = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_relaxed"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_acquire"));
        bool relaxed = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_relaxed"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_store_relaxed"));
        bool ordered = relaxed
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_acquire"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_store_release"));
        size_t expected = loading ? 1 : 2;
        th_type *storage = values.len == expected
            ? th_ind_referent(values.data[0]->type) : nullptr;
        th_type *element = storage
            ? n00bb_atomic_element_type(storage) : nullptr;
        bool valid = element && !copyouts.len
            && (loading
                ? ir_type_eq(context->module, element, call_result_type)
                : ir_type_eq(context->module, element, values.data[1]->type));
        ir_instr_t *access = valid ? ir_lower_emit_instruction(context,
            loading ? IR_OP_LOAD : IR_OP_STORE, span) : nullptr;
        bool emitted = access
            && (!loading || ir_lower_set_temp_dest(context, access,
                                                   "__n00bb_", element))
            && ir_instr_operand_value(access, values.data[0])
            && (loading || ir_instr_operand_value(access, values.data[1]))
            && ir_instr_add_mod(access, "atomic")
            && (!ordered || ir_instr_add_mod(access,
                relaxed ? "relaxed" : loading ? "acquire" : "release"))
            && (!loading || n00bb_set_reference_access(lower->ir_context,
                access->dest, element));
        core_list_free(values);
        core_list_free(copyouts);
        if (!emitted)
            return lower_error(context, valid ? IR_DIAG_INTERNAL
                                              : IR_DIAG_TYPE_MISMATCH, span,
                "atomic access needs a matching atomic reference and value");
        return loading
            ? (ir_lower_result_t){IR_LOWER_VALUE, access->dest}
            : (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    }
    if (!instruction)
        instruction = ir_lower_emit_instruction(context,
            nominal_callback ? IR_OP_CALLBACK_INVOKE : IR_OP_CALL, span);
    bool emitted = dispatch_site && dispatch_site->closed_dispatch
        ? instruction != nullptr
        : instruction && ir_lower_set_temp_dest(context, instruction,
                                                 "__n00bb_", call_result_type)
            && ir_instr_operand_value(instruction, callee_value);
    for (size_t i = 0; emitted
         && !(dispatch_site && dispatch_site->closed_dispatch)
         && i < values.len; i++) {
        if (values.data[i]) {
            emitted = ir_instr_operand_value(instruction, values.data[i]);
            continue;
        }
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool known = i < parameter_count
            && function_parameter_info(visit->grammar, symbol,
                formal_params, i, nullptr, nullptr, nullptr, &flags, &mode);
        emitted = known && (flags & 1u) && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(instruction)
            : known && mode == TH_OUT
                ? ir_instr_operand_out(instruction) : false;
    }
    if (emitted && !(dispatch_site && dispatch_site->closed_dispatch)) {
        core_list_t(th_param) call_params = core_list_new(th_param);
        bool signature_ok = true;
        for (size_t i = 0; signature_ok && i < parameter_count; i++) {
            const th_param *parameter = th_func_param(signature, i);
            th_mode mode = parameter ? parameter->mode : TH_IN;
            if (!builtin_trait_call && !reference_builtin && !native_builtin
                && !function_parameter_info(visit->grammar, symbol,
                    formal_params, i, nullptr, nullptr, nullptr, nullptr,
                    &mode)) {
                signature_ok = false;
                break;
            }
            if (!parameter || !th_mode_is_valid(mode)) {
                signature_ok = false;
                break;
            }
            core_list_push(call_params, ((th_param){
                .mode = mode, .type = parameter->type,
            }));
        }
        const th_param *source_rest = th_func_rest(signature);
        th_param lowered_rest = {0};
        const th_param *rest = source_rest
            && lowered_rest_type(ir_module_types(context->module),
                source_rest, source_rest->mode, &lowered_rest)
            ? &lowered_rest : nullptr;
        if (source_rest && !rest) signature_ok = false;
        core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
        for (size_t i = 0; signature_ok
             && i < th_func_nkw(signature); i++) {
            const th_kwparam *keyword = th_func_kw(signature, i);
            if (!keyword) signature_ok = false;
            else core_list_push(keywords, *keyword);
        }
        th_type *call_signature = signature_ok
            ? th_func_with_error(ir_module_types(context->module),
                th_func_ret(signature), th_func_error(signature),
                call_params.data, call_params.len, rest,
                keywords.data, keywords.len) : nullptr;
        emitted = call_signature
            && ir_instr_set_call_signature(instruction, call_signature);
        if (emitted && nominal_callback)
            emitted = ir_instr_set_callback_invoke_handler(instruction,
                n00bb_specialize_callback_role(
                    ir_module_types(context->module), nominal_callback,
                    callee_value->type, nominal_callback->handler->type));
        core_list_free(call_params);
        core_list_free(keywords);
    }
    if (!emitted) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the function call");
    }
    if (dispatch_site && dispatch_site->closed_dispatch
        && !dispatch_site->closed_dispatch->direct) {
        core_list_free(copyouts);
        copyouts = core_list_new(n00bb_copyout_t);
    }
    instruction->span = span;
    ir_shape_t *return_shape = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.return-shape"));
    if (!has_error && !n00bb_emit_sequence_shape_check(context,
            instruction->dest, return_shape, span)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit the function return shape check");
    }
    if (!has_error && return_shape
        && !ir_value_set_shape(instruction->dest, return_shape)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not attach the checked function return shape");
    }
    if (!n00bb_set_reference_access(lower->ir_context, instruction->dest,
                                    call_result_type)
        || !n00bb_attach_call_return_alias(context, lower, caller_state
            ? caller_state->link_analysis : nullptr, symbol, values.data,
            values.len, n00bb_tree_returns_value(visit->grammar,
                visit->node), instruction->dest)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not attach the resolved return alias");
    }
    if (index_call
        && core_string_eq(index_trait, CORE_STRING_STATIC("Index_Read"))
        && values.len) {
        if (!n00bb_attach_expression_shape(context, visit->grammar,
                receiver_node, values.data[0])
            || !n00bb_attach_index_load_shape(context, values.data[0],
                signature_return, instruction->dest)
            || (ir_value_shape(instruction->dest)
                && !ir_value_add_alias_target(lower->ir_context,
                    instruction->dest, (ir_alias_target_t){
                        .value = values.data[0],
                    }))) {
            core_list_free(values);
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "indexed sequence result could not retain receiver shape and alias provenance");
        }
    }
    core_list_free(values);
    n00bb_order_copyouts(&copyouts);
    if (copyouts.len) {
        if (has_error) {
            core_list_push(lower->pending_call_copyouts,
                ((n00bb_pending_copyouts_t){instruction->dest, copyouts}));
        }
        else {
            bool copied = true;
            for (size_t i = 0; copied && i < copyouts.len; i++)
                copied = n00bb_emit_call_copyout(context, &copyouts.data[i],
                                                  instruction->dest, span);
            core_list_free(copyouts);
            if (!copied)
                return lower_error(context, IR_DIAG_INTERNAL, span,
                                   "could not copy out call parameters");
        }
    }
    else core_list_free(copyouts);
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static void
n00bb_constructor_assignment_target(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    th_type *result_type,
                                    parse_tree_t **assignment,
                                    ir_value_t **address)
{
    parse_tree_t *child = visit->node;
    for (parse_tree_t *parent = core_tree_node_value(child).parent;
         parent && !core_tree_is_leaf(parent);
         child = parent, parent = core_tree_node_value(parent).parent) {
        if (nt_is(visit->grammar, parent, "sym-decl")) {
            if (child_nt(visit->grammar, parent, "expression") != child)
                break;
            parse_sym_entry_t *symbol = core_tree_node_value(parent).sym;
            ir_value_t *storage = symbol
                ? ir_lower_bound_address(context, symbol) : nullptr;
            th_type *referent = storage
                ? th_ind_referent(storage->type) : nullptr;
            if (referent && ir_type_eq(context->module, referent,
                                       result_type)) {
                *assignment = parent;
                *address = storage;
            }
            break;
        }
        if (core_tree_num_children(parent) != 1) break;
        if (!nt_is(visit->grammar, parent, "primary")
            && !nt_is(visit->grammar, parent, "postfix-expr")
            && !nt_is(visit->grammar, parent, "unary-expr")
            && !nt_is(visit->grammar, parent, "mul-expr")
            && !nt_is(visit->grammar, parent, "add-expr")
            && !nt_is(visit->grammar, parent, "shift-expr")
            && !nt_is(visit->grammar, parent, "bitand-expr")
            && !nt_is(visit->grammar, parent, "bitxor-expr")
            && !nt_is(visit->grammar, parent, "bitor-expr")
            && !nt_is(visit->grammar, parent, "compare-expr")
            && !nt_is(visit->grammar, parent, "equality-expr")
            && !nt_is(visit->grammar, parent, "and-expr")
            && !nt_is(visit->grammar, parent, "or-expr")
            && !nt_is(visit->grammar, parent, "expression")) break;
    }
}

static ir_lower_result_t
n00bb_lower_constructor_new(ir_lower_context_t *context, parse_visit_t *visit,
                            th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *caller_state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = caller_state
        ? caller_state->link_analysis : nullptr;
    n00bb_link_use_site_t *site = caller_state
        ? link_use_site_for_node(caller_state, visit->node) : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    if (!lower || !caller_state || !analysis || !symbol
        || symbol->kind != PARSE_SYM_FUNCTION)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "non-array construction has no resolved initializer");

    core_translation_unit_t *callee_unit = unit_for_symbol(visit->grammar,
        lower->units, symbol);
    n00bb_tu_state_t *callee_state = callee_unit
        ? tu_state(callee_unit) : nullptr;
    ir_func_t *callee_function = callee_state
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_module_t *callee_module = callee_state
        ? unit_module(callee_state) : nullptr;
    ir_value_t *callee = callee_module && callee_function
        ? ir_func_address(callee_module, callee_function) : nullptr;
    if (!callee || !callee_function)
        return lower_error(context, IR_DIAG_UNDEFINED_SYMBOL, span,
            "constructor has no semantic IR binding in this phase");

    th_ctx *types = ir_module_types(context->module);
    th_type *source_type = constructor_signature_instance(types, symbol);
    if (!source_type || th_kind_of(source_type) != TH_FUNCTION
        || !th_func_nparams(source_type)
        || !th_is_nil(th_func_error(source_type))
        || !th_is_nil(th_func_ret(source_type)))
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "resolved __init does not have an infallible, no-value signature");
    const th_param *receiver = th_func_param(source_type, 0);
    if (!receiver || receiver->mode != TH_INOUT
        || !th_unify_in_place(types, receiver->type, result_type))
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "constructor receiver does not match the constructed type");

    size_t parameter_count = th_func_nparams(source_type);
    parse_tree_t *function_signature = child_nt(visit->grammar,
        symbol->decl_node, "func-type-signature");
    parse_tree_t *formal_params = child_nt(visit->grammar,
        function_signature, "param-decl");
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_formal = nullptr;
    collect_ir_parameters(visit->grammar, formal_params, &formals,
                          &rest_formal);
    if (symbol->decl_node && formals.len != parameter_count) {
        core_list_free(formals);
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "constructor parameter symbols do not match its signature");
    }

    parse_tree_t **argument_by_parameter = core_alloc_array(
        parse_tree_t *, parameter_count);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided = core_alloc_array(bool, parameter_count);
    bool mapped = argument_by_parameter && value_by_parameter && provided;
    if (mapped) {
        memset(argument_by_parameter, 0,
               parameter_count * sizeof(*argument_by_parameter));
        memset(value_by_parameter, 0,
               parameter_count * sizeof(*value_by_parameter));
        memset(provided, 0, parameter_count * sizeof(*provided));
        provided[0] = true;
    }
    parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                       "call-args");
    core_list_t(n00bb_tree_ptr_t) positional =
        core_list_new(n00bb_tree_ptr_t);
    core_list_t(n00bb_tree_ptr_t) keywords =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, call_args, "positional-args"),
        &positional);
    collect_keyword_nodes(visit->grammar,
        child_nt(visit->grammar, call_args, "keyword-args"), &keywords);

    core_list_t(n00bb_tree_ptr_t) rest_arguments =
        core_list_new(n00bb_tree_ptr_t);
    for (size_t i = 0;
         mapped && i < positional.len; i++) {
        size_t index = i + 1;
        if (index >= parameter_count) {
            if (!th_func_rest(source_type)) mapped = false;
            else core_list_push(rest_arguments, positional.data[i]);
            continue;
        }
        if (provided[index]) mapped = false;
        else {
            argument_by_parameter[index] = positional.data[i];
            provided[index] = true;
        }
    }
    for (size_t i = 0; mapped && i < keywords.len; i++) {
        core_string_t name = call_keyword_name(keywords.data[i]);
        parse_tree_t *argument = child_nt(visit->grammar, keywords.data[i],
                                          "expression");
        size_t index = 0;
        bool found = function_parameter_named(visit->grammar, symbol,
            formal_params, name, &index);
        if (!found || index == 0 || index >= parameter_count
            || provided[index] || !argument) mapped = false;
        else {
            argument_by_parameter[index] = argument;
            provided[index] = true;
        }
    }
    core_list_free(positional);
    core_list_free(keywords);
    if (!mapped) {
        core_list_free(rest_arguments);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        core_list_free(formals);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
            "constructor arguments do not match its parameters");
    }

    core_list_t(n00bb_copyout_t) copyouts =
        core_list_new(n00bb_copyout_t);
    bool collected = true;
    for (size_t i = 1; collected && i < parameter_count; i++) {
        const th_param *parameter = th_func_param(source_type, i);
        parse_sym_entry_t *formal = i < formals.len ? formals.data[i]
                                                    : nullptr;
        th_mode mode = TH_IN;
        uint8_t flags = 0;
        if (!parameter
            || !function_parameter_info(visit->grammar, symbol,
                formal_params, i, nullptr, nullptr, nullptr, &flags, &mode)) {
            collected = false;
            break;
        }
        parse_tree_t *argument = argument_by_parameter[i];
        if (!argument && !(flags & 1u)) {
            collected = false;
            break;
        }
        if (!argument) continue;
        ir_shape_t *required_shape = formal ? parse_sym_binding(formal,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        if (th_mode_is_input_only(mode)) {
            ir_value_t *value = n00bb_materialize_node(context, argument,
                parameter->type, lower_span_for_tree(context, argument));
            if (!value || !th_unify_in_place(types, parameter->type,
                                             value->type)
                || !n00bb_attach_expression_shape(context, visit->grammar,
                                                    argument, value)
                || !n00bb_emit_sequence_shape_check(context, value,
                    required_shape, lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
            if (mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower->unit, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            value_by_parameter[i] = value;
            continue;
        }
        if (mode != TH_OUT && mode != TH_INOUT) {
            collected = false;
            break;
        }
        n00bb_out_location_t location = n00bb_out_argument_location(context,
            visit->grammar, argument, parameter->type, span);
        parse_sym_entry_t *arg_symbol = location.symbol;
        ir_local_t *target_local = location.local;
        ir_value_t *target_value = location.global;
        ir_value_t *address = location.address;
        parse_tree_t *index_target = location.index_target;
        parse_tree_t *lvalue = location.lvalue;
        th_type *target_type = location.type;
        bool writable = location.writable;
        if (!target_type || !writable
            || !th_unify_in_place(types, target_type, parameter->type)) {
            collected = false;
            break;
        }
        bool aliases = false;
        for (size_t j = 0; lvalue && j < copyouts.len; j++)
            aliases |= copyouts.data[j].symbol == arg_symbol;
        if (aliases) {
            collected = false;
            break;
        }
        ir_value_t *initial = nullptr;
        if (mode == TH_INOUT) {
            initial = n00bb_out_location_read(context, argument, &location,
                                              span);
            if (!initial || !n00bb_emit_sequence_shape_check(context,
                    initial, required_shape,
                    lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
        }
        value_by_parameter[i] = initial;
        size_t output_index = 1;
        for (size_t previous = 1; previous < i; previous++) {
            th_mode previous_mode = TH_IN;
            if (!function_parameter_info(visit->grammar, symbol,
                    formal_params, previous, nullptr, nullptr, nullptr,
                    nullptr, &previous_mode)) {
                collected = false;
                break;
            }
            if (th_mode_produces_output(previous_mode)) output_index++;
        }
        if (!collected) break;
        core_list_push(copyouts,
            ((n00bb_copyout_t){.symbol = arg_symbol,
                .local = target_local, .global = target_value,
                .address = address, .grammar = visit->grammar,
                .site_node = visit->node, .index_target = index_target,
                .source_node = argument,
                .output_index = output_index,
                .required_shape = required_shape}));
    }
    for (size_t i = 1; collected && i < parameter_count; i++) {
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        const th_param *parameter = th_func_param(source_type, i);
        if (!function_parameter_info(visit->grammar, symbol, formal_params,
                i, nullptr, nullptr, nullptr, &flags, &mode) || !parameter) {
            collected = false;
            break;
        }
        if (flags & 1u) {
            if (!th_mode_is_input_only(mode)) {
                collected = false;
                break;
            }
            if (!provided[i]) {
                value_by_parameter[i] = nullptr;
                continue;
            }
        }
        if (mode == TH_OUT) value_by_parameter[i] = nullptr;
        else if (!value_by_parameter[i]) collected = false;
    }

    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    ir_value_t *allocation_address = nullptr;
    parse_tree_t *direct_assignment = nullptr;
    if (collected)
        n00bb_constructor_assignment_target(context, visit, result_type,
                                            &direct_assignment,
                                            &allocation_address);
    if (collected && !allocation_address) {
        th_type *address_type = n00bb_indirect_type(types, result_type);
        char allocation_name[48];
        snprintf(allocation_name, sizeof(allocation_name),
            "__n00bb_construct_storage_%llu",
            (unsigned long long)context->next_value_id++);
        ir_instr_t *allocation = address_type
            ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
        collected = allocation
            && ir_instr_set_dest(allocation, allocation_name, address_type)
            && ir_instr_operand_type(allocation, result_type);
        if (collected) allocation_address = allocation->dest;
    }
    n00bb_order_copyouts(&copyouts);
    for (size_t i = 1; collected && i < parameter_count; i++)
        core_list_push(values, value_by_parameter[i]);
    if (collected && th_func_rest(source_type)) {
        const th_param *rest = th_func_rest(source_type);
        for (size_t i = 0; collected && i < rest_arguments.len; i++) {
            parse_tree_t *argument = rest_arguments.data[i];
            ir_value_t *value = nullptr;
            if (rest->mode == TH_INOUT) {
                parse_tree_t *root = n00bb_addressable_root_lvalue(
                    visit->grammar, argument);
                parse_sym_entry_t *root_symbol = root
                    ? attached_symbol(visit->grammar, lower->unit, root)
                    : nullptr;
                parse_tree_t *member = member_access_node(visit->grammar,
                                                           argument);
                parse_sym_entry_t *resolved = member
                    ? attached_symbol(visit->grammar, lower->unit, argument)
                    : nullptr;
                parse_sym_phase_t phase = n00bb_compile_phase();
                bool writable = root_symbol
                    && parse_sym_can_write(root_symbol, phase);
                if (resolved && resolved->is_field)
                    writable = writable
                        && resolved->mutability == PARSE_SYM_MUTABLE;
                value = writable
                    ? n00bb_addressable_expression_address(context,
                        visit->grammar, lower->unit, argument,
                        lower_span_for_tree(context, argument)) : nullptr;
                collected = value && th_ind_referent(value->type)
                    && th_unify_in_place(types, rest->type,
                                         th_ind_referent(value->type));
            }
            else {
                value = ir_lower_node_value(context, argument);
                collected = value && th_unify_in_place(types, rest->type,
                                                        value->type);
            }
            if (collected && rest->mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower->unit, argument, value);
                collected = value != nullptr;
            }
            if (collected) core_list_push(values, value);
        }
    }
    core_list_free(rest_arguments);

    char call_name[48];
    snprintf(call_name, sizeof(call_name), "__n00bb_init_%llu",
             (unsigned long long)context->next_value_id++);
    th_type *nil_type = th_name(types, "nil");
    ir_instr_t *call = collected && nil_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    collected = call && ir_instr_set_dest(call, call_name, nil_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_fresh(call, allocation_address);
    for (size_t i = 0; collected && i < values.len; i++) {
        if (values.data[i]) {
            collected = ir_instr_operand_value(call, values.data[i]);
            continue;
        }
        size_t parameter_index = i + 1;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool known = parameter_index < parameter_count
            && function_parameter_info(visit->grammar, symbol,
                formal_params, parameter_index, nullptr, nullptr, nullptr,
                &flags, &mode);
        collected = known && (flags & 1u) && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(call)
            : known && mode == TH_OUT ? ir_instr_operand_out(call) : false;
    }
    core_list_t(th_param) call_params = core_list_new(th_param);
    core_list_t(th_kwparam) call_keywords = core_list_new(th_kwparam);
    if (collected) {
        const th_param *self = th_func_param(source_type, 0);
        collected = self != nullptr;
        if (collected) core_list_push(call_params, ((th_param){
            .mode = TH_INOUT, .type = self->type,
        }));
    }
    for (size_t i = 1; collected && i < parameter_count; i++) {
        const th_param *parameter = th_func_param(source_type, i);
        th_mode mode = TH_IN;
        collected = parameter && function_parameter_info(visit->grammar,
            symbol, formal_params, i, nullptr, nullptr, nullptr, nullptr,
            &mode);
        if (collected) core_list_push(call_params, ((th_param){
            .mode = mode, .type = parameter->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source_type);
    th_param lowered_rest = {0};
    const th_param *rest = source_rest
        && lowered_rest_type(types, source_rest, source_rest->mode,
                             &lowered_rest)
        ? &lowered_rest : nullptr;
    if (source_rest && !rest) collected = false;
    for (size_t i = 0; collected && i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) collected = false;
        else core_list_push(call_keywords, *keyword);
    }
    th_type *call_signature = collected
        ? th_func_with_error(types, nil_type, nil_type, call_params.data,
            call_params.len, rest, call_keywords.data, call_keywords.len)
        : nullptr;
    collected = collected && call_signature
        && ir_instr_set_call_signature(call, call_signature);
    core_list_free(call_params);
    core_list_free(call_keywords);
    core_list_free(values);
    core_free(argument_by_parameter);
    core_free(value_by_parameter);
    core_free(provided);
    core_list_free(formals);
    if (!collected) {
        core_list_free(copyouts);
        return lower_error(context, IR_DIAG_TYPE_ARGS, span,
            "could not lower arguments or invocation for __init");
    }
    for (size_t i = 0; i < copyouts.len; i++)
        if (!n00bb_emit_call_copyout(context, &copyouts.data[i], call->dest,
                                     span)) {
            core_list_free(copyouts);
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not copy out constructor arguments");
    }
    core_list_free(copyouts);
    if (direct_assignment
        && !ir_lower_finish_assignment(context, direct_assignment,
                                       visit->node))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not complete direct constructor initialization");
    ir_value_t *constructed = n00bb_emit_indirect_read(context,
        allocation_address, result_type, span);
    return constructed ? (ir_lower_result_t){IR_LOWER_VALUE, constructed}
        : lower_error(context, IR_DIAG_INTERNAL, span,
            "could not read the initialized object value");
}

static ir_value_t *
n00bb_emit_outcome_payload(ir_lower_context_t *context, ir_value_t *outcome,
                          const char *arm, th_type *type, ir_span_t span)
{
    ir_instr_t *payload = ir_lower_emit_instruction(context,
        IR_OP_VARIANT_PAYLOAD, span);
    if (!payload || !ir_lower_set_temp_dest(context, payload,
                                            "__n00bb_outcome_", type)
        || !ir_instr_operand_value(payload, outcome)
        || !ir_instr_operand_name(payload, arm))
        return nullptr;
    n00bb_lower_context_t *lower = context->user;
    if (!strcmp(arm, "ok")
        && (!lower || !ir_value_copy_alias_targets(lower->ir_context,
                                        payload->dest,
                                        outcome))) return nullptr;
    if (!strcmp(arm, "ok") && ir_value_shape(outcome)
        && !ir_value_set_shape(payload->dest, ir_value_shape(outcome)))
        return nullptr;
    return payload->dest;
}

static bool
n00bb_operator_error(ir_lower_context_t *context, ir_diagcode_t code,
                     ir_span_t span, const char *message)
{
    lower_error(context, code, span, message);
    return false;
}

static bool
names_symbol_directly(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (parse_tree_is_direct_symbol_reference(node)) return true;
        if (core_tree_num_children(node) != 1) return false;
        node = core_tree_child(node, 0);
    }
    return false;
}

static bool
n00bb_unwrap_call_outcome(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *outcome, ir_span_t span,
                          ir_value_t **success, bool trap_unhandled)
{
    if (success) *success = nullptr;
    if (!outcome && ir_diags_failed(context->diagnostics)) return false;
    bool variant = false;
    th_type *definition = outcome
        ? ir_type_definition(context->module, outcome->type) : nullptr;
    if (!outcome || !th_named_info(outcome->type, nullptr, nullptr, &variant,
                                   nullptr) || !variant
        || !definition || th_kind_of(definition) != TH_RECORD)
        return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS, span,
            "failure propagation requires a complete call outcome");
    th_type *ok_type = th_record_lookup(definition, "ok");
    th_type *error_type = th_record_lookup(definition, "error");
    if (!ok_type || !error_type)
        return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS, span,
            "failure propagation requires a complete call outcome");

    uint64_t block_id = context->next_value_id++;
    char ok_block_name[48], error_block_name[48];
    snprintf(ok_block_name, sizeof(ok_block_name),
             "__n00bb_unwrap_ok_%llu", (unsigned long long)block_id);
    snprintf(error_block_name, sizeof(error_block_name),
             "__n00bb_unwrap_error_%llu", (unsigned long long)block_id);
    ir_block_t *ok_block = ir_block_add(context->function, ok_block_name);
    ir_block_t *error_block = ir_block_add(context->function,
                                           error_block_name);
    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    ir_instr_t *tag = ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG,
                                                span);
    ir_instr_t *literal = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
    bool emitted = ok_block && error_block && uint_type && bool_type
        && tag && ir_lower_set_temp_dest(context, tag, "__n00bb_tag_",
                                          uint_type)
        && ir_instr_operand_value(tag, outcome)
        && literal && ir_lower_set_temp_dest(context, literal,
                                              "__n00bb_tag_lit_", uint_type)
        && ir_instr_operand_uint(literal, th_arm_tag("ok"))
        && test && ir_lower_set_temp_dest(context, test, "__n00bb_is_ok_",
                                          bool_type)
        && ir_instr_operand_value(test, tag->dest)
        && ir_instr_operand_value(test, literal->dest);
    ir_instr_t *branch = emitted
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    emitted = emitted && branch
        && ir_instr_operand_value(branch, test->dest)
        && ir_instr_operand_block(branch, ok_block)
        && ir_instr_operand_block(branch, error_block);
    if (!emitted)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not branch on the call outcome");
    branch->span = span;

    context->block = error_block;
    ir_value_t *error = n00bb_emit_outcome_payload(
        context, outcome, "error", error_type, span);
    if (!error || !n00bb_emit_with_failure(context, visit->node))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not unwind active context managers");
    int routed = n00bb_route_try_error(context, visit->node, error, span);
    if (routed < 0)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not route the call error");
    if (!routed) {
        n00bb_lower_context_t *lower = context->user;
        bool literal_producer = lower && lower->has_active_literal;
        if (trap_unhandled || literal_producer) {
            if (literal_producer
                && th_builtin_kind_of(th_resolve(error_type))
                    == TH_BUILTIN_STRING
                && !n00bb_emit_panic(context, error, span))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower an unhandled literal failure");
            ir_instr_t *trap = ir_instr_add(context->block, IR_OP_TRAP);
            if (!trap)
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower an unhandled call failure");
            trap->span = span;
            context->reachable = false;
        }
        else if (!n00bb_failure_channel_accepts(context, error_type)) {
            if (n00bb_reject_runtime_initializer_failure(context, span))
                return false;
            return n00bb_operator_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "an uncaught call error must match the enclosing function's failure channel");
        }
        else {
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, error))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not propagate the call error");
            fail->span = span;
            context->reachable = false;
        }
    }

    context->block = ok_block;
    context->reachable = true;
    if (trap_unhandled
        && ir_machine_of(context->module, ok_type).class == IR_CLASS_VOID)
        return true;
    ir_value_t *value = n00bb_emit_outcome_payload(
        context, outcome, "ok", ok_type, span);
    if (!value)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not extract the successful call value");
    if (success) *success = value;
    return true;
}

static bool
n00bb_lower_operator_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering,
                                void *state)
{
    (void)state;
    bool is_ref = core_string_eq(visit->annot->op_kind,
                                 CORE_STRING_STATIC("ref"));
    if (is_ref) {
        if (entering) {
            parse_tree_t *operand = nullptr;
            for (size_t i = 0; i < core_tree_num_children(visit->node); i++) {
                parse_tree_t *child = core_tree_child(visit->node, i);
                if (!core_tree_is_leaf(child)) {
                    operand = child;
                    break;
                }
            }
            n00bb_lower_context_t *lower = context->user;
            parse_sym_entry_t *target = operand && lower
                ? attached_symbol(visit->grammar, lower->unit, operand)
                : nullptr;
            bool function_reference = target
                && target->kind == PARSE_SYM_FUNCTION
                && names_symbol_directly(operand);
            if (!operand || (function_reference
                    ? !ir_lower_skip_value(context, visit->node, operand)
                    : !n00bb_mark_reference_target(context,
                        visit->grammar, visit->node, operand)))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not mark a reference target as an lvalue");
            return true;
        }

        parse_tree_t *operand = nullptr;
        for (size_t i = 0; i < core_tree_num_children(visit->node); i++) {
            parse_tree_t *child = core_tree_child(visit->node, i);
            if (!core_tree_is_leaf(child)) {
                operand = child;
                break;
            }
        }
        n00bb_lower_context_t *lower = context->user;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        parse_sym_entry_t *target = operand && lower
            ? attached_symbol(visit->grammar, lower->unit, operand) : nullptr;
        bool function_reference = target
            && target->kind == PARSE_SYM_FUNCTION
            && names_symbol_directly(operand);
        ir_value_t *storage = nullptr;
        if (function_reference) {
            parse_sym_entry_t *function_target = canonical_symbol(target);
            n00bb_link_analysis_t *analysis = tu_state(lower->unit)
                ? tu_state(lower->unit)->link_analysis : nullptr;
            core_translation_unit_t *owner = unit_for_symbol(
                visit->grammar, lower->units, function_target);
            n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
            ir_func_t *function = analysis
                ? link_ir_function_for_symbol(analysis, function_target)
                : nullptr;
            ir_module_t *module = owner_state
                ? unit_module(owner_state)
                : nullptr;
            storage = module && function
                ? ir_func_reference(module, function) : nullptr;
        }
        else if (operand && lower)
            storage = n00bb_addressable_expression_address(context,
                visit->grammar, lower->unit, operand, span);
        if (storage && storage->def && storage->def->op == IR_OP_INDEX
            && storage->def->n_operands
            && storage->def->operands[0].kind == IR_OPD_VALUE) {
            ir_shape_t *shape = ir_value_shape(
                storage->def->operands[0].value);
            if (!shape) {
                parse_sym_entry_t *root = n00bb_assignment_root_symbol(
                    visit->grammar, lower->unit, operand);
                shape = root ? parse_sym_binding(root,
                    CORE_STRING_STATIC("ir.array-shape")) : nullptr;
            }
            ir_shape_storage_t layout = {0};
            if (shape && ir_shape_storage(shape, &layout)
                && layout.classification_known
                && layout.flat_prefix_rank > 1)
                return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS,
                    span, "ref cannot address a derived rectangular Array suffix");
            if (!ir_annot_set(lower->ir_context, &storage->def->annots,
                    (ir_annot_t){
                        .key = "n00bb.array.direct_ref",
                        .kind = IR_ANNOT_BOOL,
                        .b = true,
                    }))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL,
                    span, "could not mark direct Array element reference");
        }
        if (!storage)
            return n00bb_operator_error(context, IR_DIAG_TYPE_CLASS,
                span, "ref requires an addressable expression");
        if (!ir_lower_set_node_value(context, visit->node, storage)) {
            n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                 "could not retain the reference value");
            return false;
        }
        return true;
    }
    if (!core_string_eq(visit->annot->op_kind,
                        CORE_STRING_STATIC("unwrap_result"))) {
        if (!entering) return true;
        lower_error(context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(context, visit->node),
                    "this operator is not lowered yet");
        return false;
    }
    if (entering) return true;

    ir_span_t span = lower_span_for_tree(context, visit->node);
    parse_tree_t *operand_node = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *outcome = operand_node
        ? ir_lower_node_value(context, operand_node) : nullptr;
    ir_value_t *value = nullptr;
    if (!n00bb_unwrap_call_outcome(context, visit, outcome, span, &value,
                                   false))
        return false;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *callee = operand_node
        && core_tree_num_children(operand_node)
            ? core_tree_child(operand_node, 0) : nullptr;
    parse_sym_entry_t *callee_symbol = lower && callee
        ? attached_symbol(visit->grammar, lower->unit, callee) : nullptr;
    ir_shape_t *return_shape = callee_symbol
        ? parse_sym_binding(callee_symbol,
            CORE_STRING_STATIC("ir.return-shape")) : nullptr;
    if (value && return_shape
        && (!n00bb_emit_sequence_shape_check(context, value, return_shape,
                                              span)
            || !ir_value_set_shape(value, return_shape)))
        value = nullptr;
    for (size_t i = 0; value && lower
            && i < lower->pending_call_copyouts.len; i++) {
        n00bb_pending_copyouts_t *pending =
            &lower->pending_call_copyouts.data[i];
        if (pending->outcome != outcome) continue;
        for (size_t c = 0; value && c < pending->items.len; c++)
            value = n00bb_emit_call_copyout(context,
                &pending->items.data[c], value, span)
                ? value : nullptr;
        core_list_free(pending->items);
        pending->items = core_list_new(n00bb_copyout_t);
        pending->outcome = nullptr;
        break;
    }
    if (!value || !ir_lower_set_node_value(context, visit->node, value))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not extract the successful call value");
    return true;
}

static bool
n00bb_attach_symbol_array_shape(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_sym_entry_t *symbol, th_type *type,
                                ir_value_t *value, ir_local_t *local)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    bool complete = false;
    ir_shape_t *shape = lower ? array_shape_from_declaration(grammar,
        lower->unit, lower->ir_context, symbol, type, &complete) : nullptr;
    if (!complete || (shape && value && !ir_value_set_shape(value, shape))
        || (shape && local && !ir_local_set_shape(local, shape))) {
        lower_error(context, IR_DIAG_INTERNAL,
            symbol ? lower_span_for_tree(context, symbol->decl_node)
                   : IR_SPAN_UNKNOWN,
            "could not attach the declaration's array shape");
        return false;
    }
    return true;
}

static bool
n00bb_bind_runtime_array_shape(ir_lower_context_t *context,
                               ir_shape_t *shape)
{
    if (!shape) return true;
    ir_shape_storage_t storage;
    if (!ir_shape_storage(shape, &storage)) return false;
    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    ir_value_t *one = nullptr;
    bool has_runtime = false;
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_RUNTIME)
            continue;
        has_runtime = true;
        if (extent.value) continue;
        if (!one) {
            ir_instr_t *constant = uint_type
                ? n00bb_emit_uint_constant(context, uint_type, 1,
                    IR_SPAN_UNKNOWN, "__n00bb_array_one") : nullptr;
            one = constant ? constant->dest : nullptr;
            if (!one) return false;
        }
        parse_tree_t *expression = (parse_tree_t *)extent.expression;
        extent.value = expression
            ? n00bb_lower_array_dimension_value(context, expression,
                uint_type, one,
                lower_span_for_tree(context, expression), true) : nullptr;
        if (!extent.value || !ir_shape_set_extent(shape, i, extent))
            return false;
    }
    if (!has_runtime) return true;
    ir_shape_measure_t count;
    if (!ir_shape_element_count(shape, &count)) return false;
    if (count.kind != IR_SHAPE_MEASURE_UNKNOWN) return true;
    if (!one) {
        ir_instr_t *constant = uint_type
            ? n00bb_emit_uint_constant(context, uint_type, 1,
                IR_SPAN_UNKNOWN, "__n00bb_array_one") : nullptr;
        one = constant ? constant->dest : nullptr;
        if (!one) return false;
    }
    ir_value_t *total = one;
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        ir_value_t *value = extent.value;
        if (extent.kind == IR_SHAPE_EXTENT_STATIC) {
            ir_instr_t *constant = n00bb_emit_uint_constant(context,
                uint_type, extent.static_value, IR_SPAN_UNKNOWN,
                "__n00bb_array_extent");
            value = constant ? constant->dest : nullptr;
        }
        if (!value) return false;
        ir_instr_t *product = n00bb_emit_checked_product(context, total,
            value, uint_type, IR_SPAN_UNKNOWN);
        if (!product) return false;
        total = product->dest;
    }
    return ir_shape_set_element_count(shape, (ir_shape_measure_t){
        .kind = IR_SHAPE_MEASURE_RUNTIME,
        .value = total,
    });
}

static bool
n00bb_lower_signature_array_shape(ir_lowerer_t *lowerer,
                                  ir_lower_context_t *context,
                                  parse_grammar_t *grammar,
                                  ir_shape_t *shape)
{
    th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
    ir_value_t *one = nullptr;
    for (uint32_t d = 0; shape && d < ir_shape_rank(shape); d++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, d, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_RUNTIME || extent.value)
            continue;
        parse_tree_t *expression = (parse_tree_t *)extent.expression;
        if (!expression) return false;
        ir_lower_result_t bound = ir_lower_tree(lowerer, context, grammar,
                                               expression);
        if (bound.status != IR_LOWER_VALUE || !bound.value)
            return false;
        if (!one) {
            ir_instr_t *constant = uint_type
                ? n00bb_emit_uint_constant(context, uint_type, 1,
                    IR_SPAN_UNKNOWN, "__n00bb_array_one") : nullptr;
            one = constant ? constant->dest : nullptr;
            if (!one) return false;
        }
        extent.value = n00bb_coerce_array_dimension_value(context,
            expression, bound.value, uint_type, one,
            lower_span_for_tree(context, expression), true);
        if (!extent.value || !ir_shape_set_extent(shape, d, extent))
            return false;
    }
    return n00bb_bind_runtime_array_shape(context, shape);
}

static bool
n00bb_lower_declare_local(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    if (!nt_is(visit->grammar, visit->node, "sym-decl"))
        return true;
    parse_sym_entry_t *symbol = visit->pn ? visit->pn->sym : nullptr;
    if (!entering) {
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            "expression");
        ir_local_t *local = symbol
            ? ir_lower_bound_local(context, symbol) : nullptr;
        ir_shape_t *shape = local ? ir_local_shape(local) : nullptr;
        if (!n00bb_bind_runtime_array_shape(context, shape)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not bind runtime array dimensions");
            return false;
        }
        if (!expression || !n00bb_shape_has_constraints(shape)) return true;
        ir_value_t *value = ir_lower_node_value(context, expression);
        if (!value || !n00bb_attach_expression_shape(context, visit->grammar,
                expression, value)
            || !n00bb_emit_sequence_shape_check(context, value, shape,
                lower_span_for_tree(context, visit->node))) return false;
        return true;
    }
    if (symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("module")))
        return true;
    if (!symbol || !symbol->type) {
        lower_error(context, IR_DIAG_VERIFY_TYPED,
            lower_span_for_tree(context, visit->node),
            "local declaration has no resolved type");
        return false;
    }
    char name[48];
    snprintf(name, sizeof(name), "__n00bb_local_%llu",
             (unsigned long long)context->next_value_id++);
    th_type *type = th_apply_subst(ir_module_types(context->module),
                                   symbol->type);
    th_comb combination;
    if (type && th_sum_comb(th_resolve(type), &combination)
        && combination == TH_UNION) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH,
            lower_span_for_tree(context, visit->node),
            "an anonymous union cannot be stored as a variable; use a concrete type or a variant");
        return false;
    }
    ir_local_t *local = type
        ? ir_local_add(context->function, name, type) : nullptr;
    if (!local || !n00bb_set_reference_access(
            ((n00bb_lower_context_t *)context->user)->ir_context,
            ir_local_value(context->function, local), type)
        || !n00bb_bind_local(context, symbol, local,
                                     lower_span_for_tree(context,
                                                         visit->node))) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not bind local declaration");
        return false;
    }
    if (!n00bb_attach_symbol_array_shape(context, visit->grammar,
                                          symbol, type, nullptr, local))
        return false;
    return true;
}

static bool
n00bb_has_index_store_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    if (index_dispatch_for_node(grammar, node, &trait, &method)
        && (core_string_eq(trait, CORE_STRING_STATIC("Index_Write"))
            || core_string_eq(trait,
                              CORE_STRING_STATIC("Index_Slice_Write"))
            || index_is_compound_target(grammar, node)))
        return true;
    for (size_t i = 0; node && !core_tree_is_leaf(node)
            && i < core_tree_num_children(node); i++)
        if (n00bb_has_index_store_target(grammar,
                                         core_tree_child(node, i)))
            return true;
    return false;
}

static ir_value_t *
n00bb_lower_slice_bound(ir_lower_context_t *context, parse_tree_t *node,
                        th_type *bound_type, ir_span_t span)
{
    if (!bound_type) return nullptr;
    if (!node)
        return n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr,
                                   bound_type, span);
    ir_value_t *value = n00bb_materialize_node(context, node, bound_type,
                                               span);
    return value && ir_type_eq(context->module, value->type, bound_type)
        ? value : nullptr;
}

static bool
n00bb_lower_index_store_at(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           parse_tree_t *site_node,
                           parse_tree_t *target, parse_tree_t *source,
                           ir_value_t *source_value)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    parse_tree_t *index_node = n00bb_index_expression_node(grammar, target);
    bool slice = nt_is(grammar, index_node
        && core_tree_num_children(index_node) > 2
            ? core_tree_child(index_node, 2) : nullptr, "slice-contents");
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_dispatch(caller, index_node,
            slice ? CORE_STRING_STATIC("Index_Slice_Write")
                  : CORE_STRING_STATIC("Index_Write"))
        : nullptr;
    parse_tree_t *contents = index_node
        && core_tree_num_children(index_node) > 2
            ? core_tree_child(index_node, 2) : nullptr;
    parse_tree_t *receiver_node = index_node
        && core_tree_num_children(index_node)
            ? core_tree_child(index_node, 0) : nullptr;
    parse_tree_t *index_arg = child_nt(grammar, contents,
                                       "expression");
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    if (!slice && n00bb_builtin_array_write_function(symbol)) {
        ir_value_t *index_value = index_arg
            ? ir_lower_node_value(context, index_arg) : nullptr;
        th_type *receiver_type = receiver_node
            ? n00bb_dereferenced_type(ir_module_types(context->module),
                core_tree_node_value(receiver_node).type) : nullptr;
        th_type *element = receiver_type
            && th_kind_of(receiver_type) == TH_SEQUENCE
            ? th_seq_elem(receiver_type) : nullptr;
        ir_span_t span = lower_span_for_tree(context, index_node);
        parse_visit_t assignment = {.grammar = grammar, .node = site_node};
        if (!source_value && source && element
            && !n00bb_nominal_value_as(context, &assignment, element,
                                       source)) return false;
        ir_value_t *value = source_value
            ? n00bb_materialize(context, source, source_value, element, span)
            : source ? n00bb_materialize_node(context, source, element, span)
                     : nullptr;
        if (!lower || !index_value || !value || !element
            || !is_integer_type(index_value->type)) {
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "array index or stored value has an incompatible type");
            return false;
        }
        if (!th_unify_in_place(ir_module_types(context->module),
                               element, value->type)) {
            lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "array index or stored value has an incompatible type");
            return false;
        }
        if (!th_is_closed(ir_module_types(context->module), element)) {
            ir_value_t *base = n00bb_addressable_expression_address(
                context, grammar, lower->unit, receiver_node, span);
            ir_instr_t *write = base
                ? ir_lower_emit_instruction(context, IR_OP_SEQUENCE_SET,
                                            span) : nullptr;
            if (!write || !ir_instr_operand_value(write, base)
                || !ir_instr_operand_value(write, index_value)
                || !ir_instr_operand_value(write, value)) {
                lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower generic array indexed assignment");
                return false;
            }
            return true;
        }
        ir_value_t *address = n00bb_addressable_expression_address(
            context, grammar, lower->unit, index_node, span);
        ir_shape_t *shape = address ? ir_value_shape(address) : nullptr;
        if (shape && th_kind_of(th_resolve(value->type)) == TH_SEQUENCE
            && (!n00bb_emit_sequence_shape_check(context, value, shape,
                    lower_span_for_tree(context,
                        source ? source : site_node))
                || !ir_value_add_alias_target(lower->ir_context, address,
                    (ir_alias_target_t){.value = value,
                        .escapes = n00bb_index_receiver_may_escape(
                            attached_symbol(grammar, lower->unit,
                                            receiver_node))}))) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "could not enforce indexed element shape or alias provenance");
            return false;
        }
        if (!address || !n00bb_emit_indirect_write(context, address,
                                                    value, span)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                "could not lower builtin array indexed assignment");
            return false;
        }
        return true;
    }
    size_t argument_count = slice ? 5 : 3;
    ir_value_t *arguments[5] = {
        receiver_node ? ir_lower_node_value(context, receiver_node) : nullptr,
        nullptr, nullptr, nullptr,
        source_value ? source_value
                     : source ? ir_lower_node_value(context, source) : nullptr,
    };
    if (!slice) {
        arguments[1] = index_arg
            ? ir_lower_node_value(context, index_arg) : nullptr;
        arguments[2] = arguments[4];
    }
    else {
        parse_tree_t *bounds[2] = {0};
        size_t position = 0;
        bool inclusive = false, separator_seen = false;
        for (size_t i = 0; contents && i < core_tree_num_children(contents);
             i++) {
            parse_tree_t *part = core_tree_child(contents, i);
            if (nt_is(grammar, part, "range-sep")
                || parse_tree_leaf_is(part, CORE_STRING_STATIC(":"))) {
                position = 1;
                separator_seen = true;
                inclusive = core_string_eq(first_token_text(part),
                                           CORE_STRING_STATIC("to"));
            }
            else if (nt_is(grammar, part, "expression"))
                bounds[position] = part;
        }
        th_type *integer = n00bb_ir_builtin_type(context->module, "int");
        th_type *bound_type = integer
            ? th_option(ir_module_types(context->module), integer) : nullptr;
        ir_span_t span = lower_span_for_tree(context, contents);
        arguments[1] = separator_seen
            ? n00bb_lower_slice_bound(context, bounds[0], bound_type, span)
            : nullptr;
        arguments[2] = separator_seen
            ? n00bb_lower_slice_bound(context, bounds[1], bound_type, span)
            : nullptr;
        th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
        ir_instr_t *flag = separator_seen && bool_type
            ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
        if (flag && ir_lower_set_temp_dest(context, flag,
            "__n00bb_slice_inclusive_", bool_type)
            && ir_instr_operand_int(flag, inclusive ? 1 : 0)) {
            arguments[3] = flag->dest;
        }
    }
    if (!lower || !caller || !site || !symbol
        || symbol->kind != PARSE_SYM_FUNCTION
        || !arguments[0]) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, site_node),
            "indexed assignment has no resolved call operands");
        return false;
    }
    for (size_t i = 1; i < argument_count; i++)
        if (!arguments[i]) {
        lower_error(context, IR_DIAG_VERIFY_PARSED,
            lower_span_for_tree(context, site_node),
            "indexed assignment has no resolved call operands");
        return false;
        }
    if (!n00bb_attach_expression_shape(context, grammar,
            receiver_node, arguments[0])
        || (source && !n00bb_attach_expression_shape(context, grammar,
            source, arguments[argument_count - 1]))) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, site_node),
            "could not restore indexed value shape metadata");
        return false;
    }
    ir_shape_t *receiver_shape = ir_value_shape(arguments[0]);
    th_type *stored_type = th_resolve(arguments[argument_count - 1]->type);
    if (!slice && receiver_shape && ir_shape_rank(receiver_shape) > 1
        && stored_type && th_kind_of(stored_type) == TH_SEQUENCE) {
        ir_shape_t *element_shape = ir_shape_suffix(lower->ir_context,
                                                    receiver_shape, 1);
        parse_sym_entry_t *receiver_symbol = attached_symbol(
            grammar, lower->unit, receiver_node);
        if (!element_shape
            || !n00bb_emit_sequence_shape_check(context, arguments[2],
                element_shape, lower_span_for_tree(context,
                    source ? source : site_node))) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, site_node),
                "could not enforce the indexed element shape");
            return false;
        }
        if (!ir_value_add_alias_target(lower->ir_context, arguments[0],
                (ir_alias_target_t){
                    .value = arguments[2],
                    .escapes = n00bb_index_receiver_may_escape(
                        receiver_symbol),
                })) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, site_node),
                "could not record the stored element's alias provenance");
            return false;
        }
    }
    symbol = linkable_trait_requirement(caller->link_analysis, lower->unit,
                                        site, symbol);
    if (!symbol) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, site_node),
            "linkable indexed write has no resolved trait requirement");
        return false;
    }
    bool semantic_trait_call = symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("trait"));

    core_translation_unit_t *callee_unit = unit_for_symbol(
        grammar, lower->units, symbol);
    n00bb_tu_state_t *callee_state = callee_unit
        ? tu_state(callee_unit) : nullptr;
    ir_func_t *callee_function = callee_state
        ? link_ir_function_for_symbol(caller->link_analysis, symbol)
        : nullptr;
    ir_module_t *callee_module = callee_state
        ? unit_module(callee_state) : nullptr;
    ir_value_t *callee = callee_module && callee_function
        ? ir_func_address(callee_module, callee_function) : nullptr;
    th_type *callable = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    th_type *signature = th_type_clone(ir_module_types(context->module),
                                       callable);
    bool abstract_requirement = !callee
        && n00bb_open_dispatch(caller->link_analysis)
        && site->resolved_trait_name && site->dispatch_trait.data
        && !strcmp(site->resolved_trait_name, site->dispatch_trait.data);
    if ((!callee && !abstract_requirement) || !signature
        || th_kind_of(signature) != TH_FUNCTION
        || th_func_nparams(signature) != argument_count
        || th_func_rest(signature)) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, site_node),
            "indexed write method parameter count does not match the assignment");
        return false;
    }
    parse_tree_t *receiver_index = n00bb_index_expression_node(grammar,
                                                                 receiver_node);
    parse_tree_t *receiver_member = receiver_index ? nullptr
        : member_access_node(grammar, receiver_node);
    parse_tree_t *receiver_lvalue = !receiver_index && !receiver_member
        ? n00bb_simple_lvalue(receiver_node) : nullptr;
    parse_sym_entry_t *receiver_field = receiver_member
        ? attached_symbol(grammar, lower->unit, receiver_member) : nullptr;
    parse_sym_entry_t *receiver_symbol = receiver_index
        ? n00bb_assignment_root_symbol(grammar, lower->unit, receiver_index)
        : receiver_field && receiver_field->is_field
            ? n00bb_assignment_root_symbol(grammar, lower->unit,
                                           receiver_member)
        : receiver_field ? receiver_field
        : receiver_lvalue
            ? attached_symbol(grammar, lower->unit, receiver_lvalue)
            : nullptr;
    ir_local_t *receiver_local = receiver_lvalue
        ? ir_lower_bound_local(context, receiver_symbol) : nullptr;
    ir_value_t *receiver_global = !receiver_local
        && (receiver_lvalue || (receiver_field && !receiver_field->is_field))
        ? ir_lower_bound_value(context, receiver_symbol) : nullptr;
    ir_value_t *receiver_address = receiver_member && receiver_field
        && receiver_field->is_field
        ? n00bb_addressable_expression_address(context, grammar,
            lower->unit, receiver_node,
            lower_span_for_tree(context, receiver_node)) : nullptr;
    parse_sym_phase_t phase = n00bb_compile_phase();
    if (!receiver_symbol || !parse_sym_can_write(receiver_symbol, phase)
        || (receiver_field && receiver_field->is_field
            && receiver_field->mutability != PARSE_SYM_MUTABLE)
        || (!receiver_local && !receiver_global && !receiver_address
            && !receiver_index)) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, receiver_node),
            "indexed assignment requires a writable receiver storage location");
        return false;
    }
    n00bb_copyout_t receiver_copyout = {
        .symbol = receiver_symbol,
        .local = receiver_local,
        .global = receiver_global,
        .address = receiver_address,
        .grammar = grammar,
        .site_node = site_node,
        .index_target = receiver_index,
        .source_node = receiver_node,
        .required_shape = ir_value_shape(arguments[0]),
        .output_index = 0,
    };
    th_type *failure = th_func_error(signature);
    if (abstract_requirement && failure
        && th_kind_of(th_resolve(failure)) == TH_VAR && !lower->tries.len) {
        th_type *enclosing = th_func_error(context->function->type);
        if (!th_unify_in_place(ir_module_types(context->module), failure,
                enclosing ? enclosing
                          : th_name(ir_module_types(context->module), "nil"))) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, site_node),
                "an uncaught indexed write failure must match the enclosing function's failure channel");
            return false;
        }
        failure = th_resolve(failure);
    }
    bool has_failure = failure
        && !th_is_nil(failure);
    for (size_t i = 0; i < argument_count; i++) {
        const th_param *parameter = th_func_param(signature, i);
        th_mode mode = parameter ? parameter->mode : TH_IN;
        parse_tree_t *formal_signature = child_nt(grammar,
            symbol->decl_node, "func-type-signature");
        if (!formal_signature)
            formal_signature = child_nt(grammar, symbol->decl_node,
                                        "trait-func-type-signature");
        parse_tree_t *formal_params = child_nt(grammar,
            formal_signature, "param-decl");
        if (!formal_params)
            formal_params = child_nt(grammar, formal_signature,
                                     "trait-param-decl");
        function_parameter_info(grammar, symbol, formal_params, i,
                                nullptr, nullptr, nullptr, nullptr, &mode);
        if (i > 0 && parameter && th_mode_is_input_only(mode)) {
            bool literal_source = !slice && i == argument_count - 1
                && source && !source_value;
            arguments[i] = n00bb_materialize(context,
                literal_source ? source : nullptr, arguments[i],
                parameter->type, lower_span_for_tree(context, site_node));
            if (!arguments[i]) return false;
        }
        if (!parameter
            || (i == 0 ? mode != TH_INOUT : !th_mode_is_input_only(mode))
            || !th_unify_in_place(ir_module_types(context->module),
                                  parameter->type, arguments[i]->type)) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, i == 0 ? receiver_node
                    : !slice && i == 1 ? index_arg
                    : slice && i < 3 ? contents : source),
                "indexed store argument does not match its resolved parameter");
            return false;
        }
    }

    if (abstract_requirement) {
        callee = n00bb_abstract_trait_requirement(context, site, callable,
            signature, arguments[0], lower_span_for_tree(context, index_node));
        if (!callee) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, site_node),
                "could not select the indexed write trait requirement");
            return false;
        }
    }
    else if (semantic_trait_call) {
        ir_value_t *selection = n00bb_select_trait_method(context,
            grammar, callee_unit, symbol, signature, arguments[0],
            callee, lower_span_for_tree(context, index_node));
        if (!selection) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, site_node),
                "could not select the indexed write trait requirement");
            return false;
        }
        callee = selection;
    }
    ir_span_t call_span = lower_span_for_tree(context, index_node);
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL,
                                                 call_span);
    th_type *return_type = th_func_ret(signature);
    th_type *call_result_type = has_failure
        ? ir_call_outcome_type(context->module, signature) : return_type;
    bool emitted = call && return_type
        && call_result_type
        && ir_lower_set_temp_dest(context, call, "__n00bb_index_store_",
                                  call_result_type)
        && ir_instr_operand_value(call, callee);
    for (size_t i = 0; emitted && i < argument_count; i++)
        emitted = ir_instr_operand_value(call, arguments[i]);
    if (emitted && (abstract_requirement
                    || ir_func_callable_contract(callee_function))) {
        th_param params[5];
        for (size_t i = 0; i < argument_count; i++)
            params[i] = (th_param){.mode = i == 0 ? TH_INOUT : TH_IN,
                                   .type = arguments[i]->type};
        th_type *call_signature = th_func_with_error(
            ir_module_types(context->module), return_type, failure,
            params, argument_count, nullptr, nullptr, 0);
        emitted = call_signature
            && ir_instr_set_call_signature(call, call_signature);
    }
    if (!emitted || !n00bb_attach_call_return_alias(context, lower,
            caller->link_analysis, symbol, arguments, argument_count,
            n00bb_tree_returns_value(grammar, site_node),
            call->dest)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, site_node),
            "could not emit the resolved indexed write call");
        return false;
    }
    if (has_failure) {
        uint64_t branch_id = context->next_value_id++;
        char ok_name[48], error_name[48], tag_name[48];
        char literal_name[48], test_name[48];
        snprintf(ok_name, sizeof(ok_name), "__n00bb_index_store_ok_%llu",
                 (unsigned long long)branch_id);
        snprintf(error_name, sizeof(error_name),
                 "__n00bb_index_store_error_%llu",
                 (unsigned long long)branch_id);
        snprintf(tag_name, sizeof(tag_name), "__n00bb_index_store_tag_%llu",
                 (unsigned long long)branch_id);
        snprintf(literal_name, sizeof(literal_name),
                 "__n00bb_index_store_ok_tag_%llu",
                 (unsigned long long)branch_id);
        snprintf(test_name, sizeof(test_name),
                 "__n00bb_index_store_is_ok_%llu",
                 (unsigned long long)branch_id);
        ir_block_t *ok_block = ir_block_add(context->function, ok_name);
        ir_block_t *error_block = ir_block_add(context->function,
                                                error_name);
        th_type *uint_type = n00bb_ir_builtin_type(context->module, "uint");
        th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
        ir_span_t span = lower_span_for_tree(context, index_node);
        ir_instr_t *tag = ir_lower_emit_instruction(context,
            IR_OP_VARIANT_TAG, span);
        ir_instr_t *literal = ir_lower_emit_instruction(context,
            IR_OP_CONST, span);
        ir_instr_t *test = ir_lower_emit_instruction(context, IR_OP_EQ, span);
        bool branched = ok_block && error_block && uint_type && bool_type
            && tag && ir_instr_set_dest(tag, tag_name, uint_type)
            && ir_instr_operand_value(tag, call->dest)
            && literal && ir_instr_set_dest(literal, literal_name, uint_type)
            && ir_instr_operand_uint(literal, th_arm_tag("ok"))
            && test && ir_instr_set_dest(test, test_name, bool_type)
            && ir_instr_operand_value(test, tag->dest)
            && ir_instr_operand_value(test, literal->dest);
        ir_instr_t *branch = branched
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        branched = branched && branch
            && ir_instr_operand_value(branch, test->dest)
            && ir_instr_operand_block(branch, ok_block)
            && ir_instr_operand_block(branch, error_block);
        if (!branched) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, site_node),
                "could not branch on the indexed write outcome");
            return false;
        }
        branch->span = span;

        context->block = error_block;
        context->reachable = true;
        ir_value_t *error = n00bb_emit_outcome_payload(context, call->dest,
            "error", failure, span);
        if (!error || !n00bb_emit_with_failure(context, site_node)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not unwind contexts for indexed write failure");
            return false;
        }
        int routed = n00bb_route_try_error(context, site_node, error, span);
        if (routed < 0) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not route the indexed write failure");
            return false;
        }
        if (!routed) {
            if (!n00bb_failure_channel_accepts(context, failure)) {
                if (!n00bb_reject_runtime_initializer_failure(context,
                                                               span))
                    lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                        "an uncaught indexed write failure must match the enclosing function's failure channel");
                return false;
            }
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, error)) {
                lower_error(context, IR_DIAG_INTERNAL, span,
                            "could not propagate the indexed write failure");
                return false;
            }
            fail->span = span;
            context->reachable = false;
        }
        context->block = ok_block;
        context->reachable = true;
    }
    ir_span_t span = lower_span_for_tree(context, index_node);
    ir_value_t *successful = has_failure
        ? n00bb_emit_outcome_payload(context, call->dest, "ok",
                                      return_type, span)
        : call->dest;
    if (!successful || !n00bb_emit_call_copyout(context,
            &receiver_copyout, successful, span)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not copy out the indexed write receiver");
        return false;
    }
    return true;
}

static bool
n00bb_shape_has_constraints(ir_shape_t *shape)
{
    for (uint32_t i = 0; shape && i < ir_shape_rank(shape); i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_UNRESOLVED) return true;
    }
    return false;
}

static bool
n00bb_lower_shaped_assignment(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_tree_t *target,
                              parse_tree_t *source)
{
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue
        ? attached_symbol(visit->grammar,
            ((n00bb_lower_context_t *)context->user)->unit, lvalue) : nullptr;
    ir_local_t *local = symbol ? ir_lower_bound_local(context, symbol) : nullptr;
    ir_value_t *binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    ir_value_t *address = symbol
        ? ir_lower_bound_address(context, symbol) : nullptr;
    ir_shape_t *required = local ? ir_local_shape(local)
        : binding ? ir_value_shape(binding) : nullptr;
    if (!required && symbol)
        required = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (!n00bb_shape_has_constraints(required)) return false;
    ir_value_t *value = source ? ir_lower_node_value(context, source) : nullptr;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    if (!value || !n00bb_attach_expression_shape(context, visit->grammar,
            source, value))
        return false;
    if (local) return n00bb_emit_local_write(context, local, value, span);
    if (!n00bb_emit_sequence_shape_check(context, value, required, span))
        return false;
    if (address)
        return n00bb_emit_indirect_write(context, address, value, span);
    if (binding && binding->kind == IR_VAL_GLOBAL
        && ir_type_eq(context->module, binding->type, value->type)) {
        ir_instr_t *store = ir_lower_emit_instruction(context,
            IR_OP_GLOBAL_STORE, span);
        if (!store || !ir_instr_set_binding_initialization(store, true)
            || !ir_instr_operand_value(store, binding)
            || !ir_instr_operand_value(store, value)) return false;
        return true;
    }
    lower_error(context, IR_DIAG_TYPE_ARGS, span,
                "shaped assignment target has no writable IR binding");
    return false;
}

static bool
n00bb_materialized_nil_source(parse_grammar_t *grammar, parse_tree_t *source,
                              th_type *target)
{
    return source && target && !th_is_nil(target)
        && n00bb_is_nil_literal(grammar, source);
}

static ir_value_t *
n00bb_materialize_source(ir_lower_context_t *context, parse_visit_t *visit,
                         parse_tree_t *source, th_type *target)
{
    ir_span_t span = lower_span_for_tree(context, source);
    ir_value_t *value = nullptr;
    if (n00bb_materialized_nil_source(visit->grammar, source, target))
        value = n00bb_emit_wrapping(context, IR_OP_NIL, nullptr,
            th_nil(ir_module_types(context->module)), span);
    else value = ir_lower_node_value(context, source);
    value = n00bb_materialize(context, source, value, target, span);
    return value && ir_lower_set_node_value(context, source, value)
        ? value : nullptr;
}

static bool
n00bb_materialize_assignment_source(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *source, th_type *target,
                                    bool entering)
{
    if (!source || !target) return true;
    if (entering)
        return !n00bb_materialized_nil_source(visit->grammar, source, target)
            || ir_lower_skip_value(context, visit->node, source);
    if (!n00bb_materialize_source(context, visit, source, target)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not materialize the assigned value");
        return false;
    }
    return true;
}


static bool
n00bb_nominal_value_as(ir_lower_context_t *context, parse_visit_t *visit,
                       th_type *target_type, parse_tree_t *source)
{
    n00bb_lower_context_t *lower = context->user;
    ir_value_t *value = source ? ir_lower_node_value(context, source) : nullptr;
    if (!value || !target_type || !value->def
        || value->def->op != IR_OP_AGGREGATE
        || th_kind_of(th_resolve(value->type)) != TH_RECORD)
        return true;
    th_type *option_element = nullptr;
    if (n00bb_option_type_element(target_type, &option_element))
        target_type = option_element;
    th_ctx *types = ir_module_types(context->module);
    th_type *definition = th_type_definition(types, target_type);
    if (!definition || th_kind_of(th_resolve(definition)) != TH_RECORD
        || !ir_type_eq(context->module, definition, value->type))
        return true;

    core_translation_unit_t *owner = nullptr;
    if (!lower || !record_scope_for_type(lower->grammar, lower->units,
                                        target_type, &owner)
        || !owner) return false;
    n00bb_tu_state_t *owner_state = tu_state(owner);
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(owner_state);
    bool has_constructor = false;
    for (size_t i = 0; !has_constructor && i < scopes.len; i++) {
        for (parse_sym_entry_t *candidate = scopes.data[i]->first_in_scope;
             candidate; candidate = candidate->next_in_scope) {
            if (candidate->kind != PARSE_SYM_FUNCTION
                || !core_string_eq(candidate->name,
                                   CORE_STRING_STATIC("__init"))) continue;
            th_type *signature = constructor_signature_instance(types,
                                                                 candidate);
            const th_param *receiver = signature
                && th_kind_of(signature) == TH_FUNCTION
                ? th_func_param(signature, 0) : nullptr;
            if (receiver && th_env_type_pattern_subsumes(receiver->type,
                                                         target_type)) {
                has_constructor = true;
                break;
            }
        }
    }
    core_list_free(scopes);
    if (has_constructor)
        return lower_error(context, IR_DIAG_TYPE_MISMATCH,
            lower_span_for_tree(context, visit->node),
            "named record has a constructor; use an explicit cast or new")
            .status != IR_LOWER_FAILED;

    value->type = target_type;
    return ir_lower_set_node_value(context, source, value);
}

static parse_sym_entry_t *
n00bb_assignment_symbol(ir_lower_context_t *context, parse_visit_t *visit,
                        parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    if (nt_is(visit->grammar, visit->node, "sym-decl"))
        return core_tree_node_value(visit->node).sym;
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    return lvalue && lower
        ? attached_symbol(visit->grammar, lower->unit, lvalue) : nullptr;
}

static bool
n00bb_atomic_assignment_value(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_tree_t *target,
                              parse_tree_t *source, bool entering,
                              bool *handled)
{
    *handled = false;
    parse_sym_entry_t *symbol = n00bb_assignment_symbol(context, visit,
                                                         target);
    if (!symbol || !n00bb_atomic_element_type(symbol->type)) return true;
    *handled = true;
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_skip_value(context, visit->node, target)
            && n00bb_materialize_assignment_source(context, visit, source,
                                                   symbol->type, true);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *value = source
        ? n00bb_materialize_source(context, visit, source, symbol->type)
        : nullptr;
    ir_value_t *address = n00bb_atomic_binding_address(context, symbol, span);
    if (!value || !address
        || !ir_type_eq(context->module, symbol->type, value->type)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "atomic assignment requires a value of the same underlying type and writable storage");
        return false;
    }
    return n00bb_emit_indirect_write(context, address, value, span);
}

static parse_sym_entry_t *
n00bb_assignment_root_symbol(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *target)
{
    parse_tree_t *lvalue = n00bb_addressable_root_lvalue(grammar, target);
    return lvalue ? attached_symbol(grammar, unit, lvalue) : nullptr;
}

static ir_value_t *
n00bb_reference_view(ir_lower_context_t *context, ir_value_t *value,
                     ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_type *definition = value && lower
        ? n00bb_reference_definition(ir_module_types(context->module),
                                     value->type) : nullptr;
    if (!definition) return value;
    ir_instr_t *conversion = ir_lower_emit_instruction(context,
        IR_OP_NOMINAL_CAST, span);
    if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                               "__n00bb_", definition)
        || !ir_instr_operand_value(conversion, value)
        || !ir_value_copy_alias_targets(lower->ir_context, conversion->dest,
                                        value)
        || !n00bb_set_reference_access(lower->ir_context, conversion->dest,
                                       definition)) return nullptr;
    return conversion->dest;
}

static ir_value_t *
n00bb_record_field_address(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_tree_t *node, ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!context || !lower || !unit || !node) return nullptr;
    parse_tree_t *member = member_access_node(grammar, node);
    if (member) {
        parse_tree_t *base_node = core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
        parse_sym_entry_t *field = attached_symbol(grammar, unit, member);
        ir_value_t *base = n00bb_reference_view(context,
            ir_lower_node_value(context, base_node), span);
        parse_sym_entry_t *base_symbol = attached_symbol(grammar, unit,
                                                         base_node);
        ir_value_t *binding = base_symbol
            ? ir_lower_bound_value(context, base_symbol) : nullptr;
        if (binding && binding->type
            && th_kind_of(th_resolve(binding->type)) == TH_INDIRECTION)
            base = binding;
        if (!base || th_kind_of(th_resolve(base->type)) != TH_INDIRECTION)
            base = n00bb_addressable_expression_address(
                context, grammar, unit, base_node, span);
        while (base && th_kind_of(th_resolve(base->type)) == TH_INDIRECTION) {
            th_type *referent = th_resolve(th_ind_referent(base->type));
            if (!referent || th_kind_of(referent) != TH_INDIRECTION) break;
            ir_instr_t *load = ir_lower_emit_instruction(context, IR_OP_LOAD,
                                                          span);
            if (!load || !ir_lower_set_temp_dest(context, load,
                "__n00bb_deref_", referent)
                || !ir_instr_operand_value(load, base)
                || !n00bb_set_reference_access(lower->ir_context,
                    load->dest, referent)) return nullptr;
            base = load->dest;
        }
        if (!field || !field->is_field || !field->type || !base
            || n00bb_option_type_element(th_ind_referent(th_resolve(base->type)),
                                         nullptr))
            return nullptr;
        th_type *member_type = core_tree_node_value(member).type;
        th_type *pointer_type = th_ind(ir_module_types(context->module), 0,
                                       member_type ? member_type : field->type);
        ir_instr_t *address = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_FIELD, span) : nullptr;
        if (!address || !ir_lower_set_temp_dest(context, address,
            "__n00bb_field_address_", pointer_type)
            || !ir_instr_operand_value(address, base)
            || !ir_instr_operand_name(address, field->name.data))
            return nullptr;
        ir_shape_t *field_shape = parse_sym_binding(field,
            CORE_STRING_STATIC("ir.array-shape"));
        if (field_shape && !ir_value_set_shape(address->dest, field_shape))
            return nullptr;
        return address->dest;
    }

    parse_sym_entry_t *symbol = attached_symbol(grammar, unit, node);
    ir_value_t *address = symbol
        ? ir_lower_bound_address(context, symbol) : nullptr;
    ir_local_t *local = symbol
        ? ir_lower_bound_local(context, symbol) : nullptr;
    if (!address && local) address = ir_lower_local_address(context, local);
    if (address) return address;
    if (local) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, local->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            "__n00bb_local_address_", pointer_type)
            && ir_instr_operand_local(take, local) ? take->dest : nullptr;
    }
    ir_value_t *binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    if (binding && binding->kind == IR_VAL_GLOBAL && binding->global) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, binding->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            "__n00bb_global_address_", pointer_type)
            && ir_instr_operand_value(take, binding) ? take->dest : nullptr;
    }
    if (binding && th_kind_of(th_resolve(binding->type)) == TH_INDIRECTION)
        return binding;
    if (binding && symbol && symbol->type
        && (binding->kind == IR_VAL_PARAM
            || binding->kind == IR_VAL_GLOBAL)) {
        th_type *pointer_type = th_ind(
            ir_module_types(context->module), 0, symbol->type);
        ir_instr_t *take = pointer_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            "__n00bb_value_address_", pointer_type)
            && ir_instr_operand_value(take, binding) ? take->dest : nullptr;
    }
    return nullptr;
}

static ir_value_t *
n00bb_addressable_expression_address(ir_lower_context_t *context,
                                     parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node, ir_span_t span)
{
    if (!context || !grammar || !unit || !node) return nullptr;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *member = member_access_node(grammar, node);
    parse_sym_entry_t *resolved = attached_symbol(grammar, unit, node);
    if (member && resolved && !resolved->is_field) {
        ir_value_t *binding = ir_lower_bound_value(context, resolved);
        ir_value_t *address = ir_lower_bound_address(context, resolved);
        ir_local_t *local = ir_lower_bound_local(context, resolved);
        if (address) return address;
        if (!local && (!binding
            || (binding->kind != IR_VAL_PARAM
                && binding->kind != IR_VAL_GLOBAL))) return nullptr;
        th_type *type = local ? local->type
            : binding ? binding->type : resolved->type;
        th_type *address_type = type
            ? th_ind(ir_module_types(context->module), 0, type) : nullptr;
        ir_instr_t *take = address_type
            ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
            : nullptr;
        return take && ir_lower_set_temp_dest(context, take,
            "__n00bb_member_address_", address_type)
            && (local ? ir_instr_operand_local(take, local)
                      : binding && ir_instr_operand_value(take, binding))
            ? take->dest : nullptr;
    }
    if (member)
        return n00bb_record_field_address(context, grammar, unit, member,
                                           span);

    parse_tree_t *index = n00bb_index_expression_node(grammar, node);
    if (index) {
        parse_tree_t *receiver = core_tree_num_children(index)
            ? core_tree_child(index, 0) : nullptr;
        parse_tree_t *contents = core_tree_num_children(index) > 2
            ? core_tree_child(index, 2) : nullptr;
        parse_tree_t *subscript = child_nt(grammar, contents, "expression");
        ir_value_t *base = n00bb_addressable_expression_address(
            context, grammar, unit, receiver, span);
        ir_value_t *offset = subscript
            ? ir_lower_node_value(context, subscript) : nullptr;
        th_type *container = base
            ? th_ind_referent(th_resolve(base->type)) : nullptr;
        container = container ? th_resolve(container) : nullptr;
        uint8_t address_attrs = 0;
        if (!container || (th_kind_of(container) != TH_SEQUENCE
                && (!th_ind_attrs(base->type, &address_attrs)
                    || !(address_attrs & TH_INDEXABLE))))
            return nullptr;
        th_type *element = container && th_kind_of(container) == TH_SEQUENCE
            ? th_seq_elem(container) : core_tree_node_value(index).type;
        th_type *address_type = element
            ? th_ind(ir_module_types(context->module), 0, element) : nullptr;
        ir_instr_t *address = base && offset && address_type
            ? ir_lower_emit_instruction(context, IR_OP_INDEX, span) : nullptr;
        if (!address || !ir_lower_set_temp_dest(context, address,
            "__n00bb_index_address_", address_type)
            || !ir_instr_operand_value(address, base)
            || !ir_instr_operand_value(address, offset)) return nullptr;
        if (lower && element
            && th_kind_of(th_resolve(element)) == TH_SEQUENCE) {
            ir_shape_t *base_shape = ir_value_shape(base);
            ir_shape_t *suffix = base_shape
                ? ir_shape_suffix(lower->ir_context, base_shape, 1) : nullptr;
            if (base_shape && (!suffix
                    || !ir_value_set_shape(address->dest, suffix)))
                return nullptr;
        }
        return address->dest;
    }
    return n00bb_record_field_address(context, grammar, unit, node, span);
}

static bool
n00bb_lower_flagset_field_assignment(ir_lower_context_t *context,
                                     parse_visit_t *visit,
                                     parse_tree_t *target,
                                     parse_tree_t *source,
                                     bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *flag = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (!lower || !n00bb_flagset_member(flag)
        || !n00bb_flagset_instance_access(visit->grammar, lower->unit,
                                           member, flag)) return true;
    parse_tree_t *receiver = core_tree_child(member, 0);
    parse_sym_entry_t *root = n00bb_assignment_root_symbol(
        visit->grammar, lower->unit, receiver);
    parse_sym_phase_t phase = n00bb_compile_phase();
    if (entering) {
        if (!root || !parse_sym_can_write(root, phase)) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, visit->node),
                "flagset member assignment requires a writable flagset value");
            return false;
        }
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_skip_value(context, visit->node, target);
    }

    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *assigned = source
        ? ir_lower_node_value(context, source) : nullptr;
    ir_value_t *address = n00bb_addressable_expression_address(
        context, visit->grammar, lower->unit, receiver, span);
    th_type *flagset_type = address ? th_ind_referent(address->type) : nullptr;
    ir_value_t *current = address && flagset_type
        ? n00bb_emit_indirect_read(context, address, flagset_type, span)
        : nullptr;
    n00bb_flagset_value_t *value = flag->has_comptime_value
        ? flag->comptime_value : nullptr;
    ir_instr_t *mask = current && value
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONST, span)
        : nullptr;
    ir_instr_t *updated = mask
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_SET, span)
        : nullptr;
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    if (!assigned || !bool_type
        || !ir_type_eq(context->module, assigned->type, bool_type)
        || !mask || !updated
        || !ir_lower_set_temp_dest(context, mask, "__n00bb_", flagset_type)
        || !ir_instr_operand_name(mask, flag->name.data)
        || !ir_instr_operand_uint(mask, value->mask)
        || !ir_lower_set_temp_dest(context, updated, "__n00bb_",
                                   flagset_type)
        || !ir_instr_operand_value(updated, current)
        || !ir_instr_operand_value(updated, mask->dest)
        || !ir_instr_operand_value(updated, assigned)
        || !n00bb_emit_indirect_write(context, address, updated->dest, span)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                    "could not update this flagset member");
        return false;
    }
    return true;
}

static bool
n00bb_lower_record_field_assignment(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *target,
                                    parse_tree_t *source,
                                    bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *field = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (!field || !field->is_field) return true;
    bool is_nil = n00bb_materialized_nil_source(visit->grammar, source,
                                                field->type);
    if (entering) {
        parse_tree_t *receiver = core_tree_child(member, 0);
        th_type *receiver_type = th_resolve(
            core_tree_node_value(receiver).type);
        bool through_reference = receiver_type
            && (th_kind_of(receiver_type) == TH_INDIRECTION
                || n00bb_reference_definition(ir_module_types(context->module),
                                              receiver_type));
        parse_sym_entry_t *root = n00bb_assignment_root_symbol(
            visit->grammar, lower->unit, target);
        parse_sym_phase_t phase = n00bb_compile_phase();
        if (field->mutability != PARSE_SYM_MUTABLE
            || (!through_reference
                && (!root || (!parse_sym_can_write(root, phase)
                    && (!root->type
                        || !th_ind_referent(th_resolve(root->type))))))) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, visit->node),
                "record field assignment requires a mutable field on a writable value");
            return false;
        }
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_suppress_emit(context, member)
            && (!is_nil || ir_lower_skip_value(context, visit->node, source));
    }

    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *address = n00bb_record_field_address(context, visit->grammar,
        lower->unit, target, span);
    th_type *resolved_field_type = address
        ? th_ind_referent(address->type) : field->type;
    ir_value_t *value = source
        ? n00bb_materialize_source(context, visit, source,
                                   resolved_field_type)
        : nullptr;
    if (!value || !address) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "record field assignment has no value or address");
        return false;
    }
    if (!th_unify_in_place(ir_module_types(context->module),
                           th_ind_referent(address->type), value->type)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "record field assignment value does not match its resolved type "
            "(field %s, value %s)",
            th_print(ir_module_types(context->module),
                     th_ind_referent(address->type)),
            th_print(ir_module_types(context->module), value->type));
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            message.data);
        core_buffer_release(&message);
        return false;
    }
    if (!n00bb_emit_sequence_shape_check(context, value,
            parse_sym_binding(field, CORE_STRING_STATIC("ir.array-shape")),
            span)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "record field assignment does not match its array shape");
        return false;
    }
    if (!n00bb_emit_indirect_write(context, address, value, span)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "could not emit the record field store");
        return false;
    }
    return true;
}

static bool
n00bb_variant_payload_compatible(ir_lower_context_t *context,
                                 th_type *declared, th_type *actual)
{
    ir_module_t *module = context ? context->module : nullptr;
    th_ctx *types = module ? ir_module_types(module) : nullptr;
    if (!types || !declared || !actual) return false;
    if (ir_type_eq(module, declared, actual)) return true;
    th_type *shape = ir_type_definition(module, th_resolve(declared));
    return shape && th_kind_of(th_resolve(shape)) == TH_RECORD
        && th_kind_of(th_resolve(actual)) == TH_RECORD
        && th_hash(types, shape) == th_hash(types, actual);
}

static bool
n00bb_lower_variant_arm_assignment(ir_lower_context_t *context,
                                   parse_visit_t *visit,
                                   parse_tree_t *target,
                                   parse_tree_t *source, bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *arm = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (!lower || !n00bb_variant_arm_symbol(visit->grammar, arm)) return true;
    parse_tree_t *receiver_node = core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    parse_sym_entry_t *receiver = attached_symbol(visit->grammar, lower->unit,
                                                   receiver_node);
    if (!receiver || !receiver->type || !arm->type) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, visit->node),
            "variant arm assignment requires a resolved variant value and payload type");
        return false;
    }
    if (entering) {
        parse_sym_phase_t phase = n00bb_compile_phase();
        if (!parse_sym_can_write(receiver, phase)) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, visit->node),
                "variant arm assignment requires a writable variant value");
            return false;
        }
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_skip_value(context, visit->node, target);
    }

    ir_span_t span = lower_span_for_tree(context, visit->node);
    th_ctx *types = ir_module_types(context->module);
    th_type *variant_type = th_apply_subst(types, receiver->type);
    bool is_variant = false;
    if (!variant_type
        || !th_named_info(th_resolve(variant_type), nullptr, nullptr,
                          &is_variant, nullptr)
        || !is_variant) {
        lower_error(context, IR_DIAG_TYPE_CLASS, span,
            "variant arm assignment target does not have a variant type");
        return false;
    }
    ir_value_t *payload = source ? ir_lower_node_value(context, source) : nullptr;
    th_type *arm_type = n00bb_variant_arm_payload_type(types, variant_type,
                                                        arm);
    if (!payload || !arm_type
        || !n00bb_variant_payload_compatible(context, arm_type,
                                                       payload->type)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "variant arm assignment requires payload type %s, but got %s",
            arm_type ? n00bb_print_type(types, arm_type) : "unresolved",
            payload ? n00bb_print_type(types, payload->type) : "no value");
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span, message.data);
        core_buffer_release(&message);
        return false;
    }

    ir_instr_t *make = ir_lower_emit_instruction(context, IR_OP_VARIANT_MAKE,
                                                 span);
    if (!make || !ir_lower_set_temp_dest(context, make,
        "__n00bb_variant_", variant_type)
        || !ir_instr_operand_name(make, arm->name.data)
        || !ir_instr_operand_value(make, payload)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not construct the selected variant arm");
        return false;
    }
    if (!ir_value_copy_alias_targets(lower->ir_context, make->dest, payload)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not preserve variant payload alias provenance");
        return false;
    }

    ir_value_t *address = n00bb_record_field_address(context, visit->grammar,
        lower->unit, receiver_node, span);
    if (!address
        || !n00bb_emit_indirect_write(context, address, make->dest, span)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "could not store the selected variant arm");
        return false;
    }
    return true;
}

static bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (nt_is(visit->grammar, visit->node, "sym-decl")) {
        parse_tree_t *target = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->name_ref);
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *declared = core_tree_node_value(visit->node).sym;
        th_type *expected = declared ? declared->type : nullptr;
        if (!entering && source
            && !n00bb_nominal_value_as(context, visit, expected, source))
            return false;
        bool atomic_value = false;
        if (!n00bb_atomic_assignment_value(context, visit, target, source,
                                            entering, &atomic_value))
            return false;
        if (atomic_value) return true;
        if (!n00bb_materialize_assignment_source(context, visit, source,
                                                 expected, entering))
            return false;
        parse_tree_t *lvalue = n00bb_simple_lvalue(target);
        n00bb_lower_context_t *lower = context->user;
        parse_sym_entry_t *symbol = lvalue
            ? attached_symbol(visit->grammar,
                lower ? lower->unit : nullptr, lvalue) : nullptr;
        ir_shape_t *shape = symbol ? parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        if (!n00bb_shape_has_constraints(shape)) return true;
        if (entering)
            return ir_lower_handle_assignment(context, visit->node);
        if (n00bb_lower_shaped_assignment(context, visit, target, source))
            return true;
        return !ir_diags_failed(context->diagnostics);
    }
    bool compound = nt_is(visit->grammar, visit->node,
                          "binop-assign-stmt");
    if (!compound && !nt_is(visit->grammar, visit->node, "assign-stmt"))
        return true;
    parse_tree_t *target = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, visit->annot->name_ref);
    parse_tree_t *source = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, visit->annot->value_ref);
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *field_access = member_access_node(visit->grammar, target);
    parse_sym_entry_t *field = field_access && lower
        ? attached_symbol(visit->grammar, lower->unit, field_access) : nullptr;
    if (!compound && field && n00bb_flagset_member(field))
        return n00bb_lower_flagset_field_assignment(context, visit, target,
                                                     source, entering);
    if (!compound && field
        && n00bb_variant_arm_symbol(visit->grammar, field))
        return n00bb_lower_variant_arm_assignment(context, visit, target,
                                                   source, entering);
    if (!compound && field && field->is_field)
        return n00bb_lower_record_field_assignment(context, visit, target,
                                                    source, entering);
    if (entering && n00bb_atomic_member_lvalue_root(visit->grammar,
            lower ? lower->unit : nullptr, target)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH,
            lower_span_for_tree(context, visit->node),
            "atomic values cannot be updated through a field; assign the whole value");
        return false;
    }
    if (!compound) {
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        bool atomic_value = false;
        if (!n00bb_atomic_assignment_value(context, visit, target, source,
                                            entering, &atomic_value))
            return false;
        if (atomic_value) return true;
    }
    if (!entering) {
        if (!compound && n00bb_has_index_store_target(visit->grammar, target)) {
            parse_tree_t *source = parse_tree_resolve_child_ref(
                visit->grammar, visit->node, visit->annot->value_ref);
            return n00bb_lower_index_store_at(context, visit->grammar,
                visit->node, target, source, nullptr);
        }
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *assigned = n00bb_assignment_symbol(context, visit,
                                                               target);
        th_type *target_type = assigned ? assigned->type
            : target && !core_tree_is_leaf(target)
                ? core_tree_node_value(target).type : nullptr;
        if (!compound
            && (!n00bb_nominal_value_as(context, visit, target_type, source)
                || !n00bb_materialize_assignment_source(context, visit,
                       source, target_type, false)))
            return false;
        if (n00bb_lower_shaped_assignment(context, visit, target, source))
            return true;
        if (ir_diags_failed(context->diagnostics)) return false;
        return true;
    }
    if (!compound && n00bb_has_index_store_target(visit->grammar, target)) {
        parse_tree_t *index = n00bb_index_expression_node(visit->grammar,
                                                           target);
        n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
        n00bb_link_use_site_t *site = caller
            ? link_use_site_for_dispatch(caller, index,
                nt_is(visit->grammar, index
                    && core_tree_num_children(index) > 2
                        ? core_tree_child(index, 2) : nullptr,
                      "slice-contents")
                    ? CORE_STRING_STATIC("Index_Slice_Write")
                    : CORE_STRING_STATIC("Index_Write"))
            : nullptr;
        parse_tree_t *receiver = index && core_tree_num_children(index)
            ? core_tree_child(index, 0) : nullptr;
        parse_tree_t *member = receiver
            ? member_access_node(visit->grammar, receiver) : nullptr;
        bool address_only = site
            && n00bb_builtin_array_write_function(site->symbol);
        if ((!address_only || !member
                || ir_lower_suppress_emit(context, member))
            && ir_lower_handle_assignment(context, visit->node)) return true;
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not claim indexed assignment lowering");
        return false;
    }
    if (!compound) {
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *assigned = n00bb_assignment_symbol(context, visit,
                                                               target);
        if (!n00bb_materialize_assignment_source(context, visit, source,
                assigned ? assigned->type : nullptr, true)) return false;
    }
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue
        ? core_tree_node_value(lvalue).sym : nullptr;
    if (symbol && symbol->kind == PARSE_SYM_VARIABLE
        && scope_is_within_function(symbol->scope)
        && !ir_lower_bound_value(context, symbol)
        && !ir_lower_bound_local(context, symbol)
        && !ir_lower_bound_address(context, symbol)) {
        if (!symbol->type) {
            lower_error(context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(context, visit->node),
                "inferred assignment has no resolved type");
            return false;
        }
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_local_%llu",
                 (unsigned long long)context->next_value_id++);
        th_type *type = th_apply_subst(ir_module_types(context->module),
                                       symbol->type);
        ir_local_t *local = type
            ? ir_local_add(context->function, name, type) : nullptr;
        if (!local || !n00bb_bind_local(context, symbol, local,
                                         lower_span_for_tree(context,
                                                             visit->node))) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not bind inferred assignment destination");
            return false;
        }
        if (!n00bb_attach_symbol_array_shape(context, visit->grammar,
                symbol, type, nullptr, local)) return false;
    }
    parse_sym_phase_t phase = n00bb_compile_phase();
    bool field_compound = compound && field && field->is_field;
    parse_sym_entry_t *write_owner = field_compound
        ? n00bb_assignment_root_symbol(visit->grammar,
              lower ? lower->unit : nullptr, target)
        : symbol;
    bool writable = !write_owner || parse_sym_can_write(write_owner, phase);
    if (write_owner && !compound
        && write_owner->mutability == PARSE_SYM_IMMUTABLE
        && parse_sym_can_initialize(write_owner, phase)
        && (scope_is_within_function(write_owner->scope)
            || ir_func_is_initializer(context->function))) {
        // A single assignment site can execute again when its loop repeats.
        for (size_t i = 0; lower && i < lower->loops.len; i++) {
            n00bb_loop_frame_t *loop = &lower->loops.data[i];
            if (!loop->active || !loop->node
                || n00bb_binding_in_loop_scope(write_owner, loop->node))
                continue;
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, visit->node),
                "a let binding declared outside a loop cannot be initialized inside it");
            return false;
        }
        writable = true;
    }
    if (field_compound && !writable && write_owner->type
        && th_ind_referent(th_resolve(write_owner->type)))
        writable = true;
    if (field_compound && field->mutability != PARSE_SYM_MUTABLE)
        writable = false;
    bool indexed_compound = compound
        && n00bb_has_index_store_target(visit->grammar, target);
    if (write_owner && !writable && !indexed_compound) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, visit->node),
            "cannot assign to this binding");
        return false;
    }
    if (compound && entering
        && !ir_lower_evaluate_assignment_target(context, visit->node)) {
        lower_error(context, IR_DIAG_INTERNAL,
            lower_span_for_tree(context, visit->node),
            "could not evaluate compound assignment target");
        return false;
    }
    ir_local_t *target_local = symbol
        ? ir_lower_bound_local(context, symbol) : nullptr;
    ir_value_t *target_binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    ir_shape_t *target_shape = target_local
        ? ir_local_shape(target_local)
        : target_binding ? ir_value_shape(target_binding) : nullptr;
    if (!target_shape && symbol)
        target_shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (!compound && n00bb_shape_has_constraints(target_shape)) {
        if (!ir_lower_handle_assignment(context, visit->node)
            || !ir_lower_skip_value(context, visit->node, target)) {
            lower_error(context, IR_DIAG_INTERNAL,
                lower_span_for_tree(context, visit->node),
                "could not claim shaped assignment lowering");
            return false;
        }
    }
    return true;
}

static bool
n00bb_simple_assignment_field_target(parse_grammar_t *grammar,
                                     parse_tree_t *node)
{
    for (parse_tree_t *parent = core_tree_node_value(node).parent;
         parent; parent = core_tree_node_value(parent).parent) {
        if (nt_is(grammar, parent, "binop-assign-stmt")) return false;
        if (!nt_is(grammar, parent, "assign-stmt")) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        parse_annotation_t *assign = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
            : nullptr;
        parse_tree_t *target = assign
            ? parse_tree_resolve_child_ref(grammar, parent,
                                           assign->name_ref) : nullptr;
        return member_access_node(grammar, target) == node;
    }
    return false;
}

static bool
n00bb_reference_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = core_tree_node_value(at).parent) {
        if (!nt_is(grammar, at, "unary-expr")) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, at);
        parse_annotation_t *operator = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_OPERATOR)
            : nullptr;
        if (!operator || !core_string_eq(operator->op_kind,
                CORE_STRING_STATIC("ref"))) continue;
        for (size_t i = 0; i < core_tree_num_children(at); i++) {
            parse_tree_t *operand = core_tree_child(at, i);
            if (!node_is_within(node, operand)) continue;
            for (parse_tree_t *path = node; path && path != operand;
                 path = core_tree_node_value(path).parent) {
                parse_tree_t *parent = core_tree_node_value(path).parent;
                core_string_t trait = {0}, method = {0};
                if (parent && index_dispatch_for_node(grammar, parent,
                                                       &trait, &method)) {
                    parse_tree_t *receiver = core_tree_num_children(parent)
                        ? core_tree_child(parent, 0) : nullptr;
                    if (!node_is_within(node, receiver)) return false;
                }
            }
            return true;
        }
    }
    return false;
}

static bool
n00bb_check_phase_read(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (!entering || !visit->node) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *use_site = tu
        ? link_use_site_for_node(tu, visit->node) : nullptr;
    if (use_site && use_site->variant_case_candidate) return true;
    parse_sym_entry_t *symbol = lower
        ? attached_symbol(visit->grammar, lower->unit, visit->node) : nullptr;
    if (!symbol && visit->pn) symbol = visit->pn->sym;
    if (!symbol) return true;
    if ((symbol->kind != PARSE_SYM_VARIABLE
         && symbol->kind != PARSE_SYM_PARAM)
        || symbol->is_field) return true;
    if (n00bb_reference_target(visit->grammar, visit->node)) return true;
    if (lower && initializer_node_is_out_argument(visit->grammar,
            lower->unit, visit->node)) return true;
    th_type *type = symbol->type ? th_resolve(symbol->type) : nullptr;
    if (type && th_kind_of(type) == TH_FUNCTION) return true;

    bool compound_target = false;
    if (n00bb_assignment_target(visit->grammar, visit->node,
                               &compound_target) && !compound_target)
        return true;

    parse_sym_phase_t phase = n00bb_compile_phase();
    if (parse_sym_can_read(symbol, phase)) return true;

    lower_error(context, IR_DIAG_TYPE_ARGS,
        lower_span_for_tree(context, visit->node),
        "this binding is not available here");
    return false;
}

static parse_sym_entry_t *
n00bb_compound_target_symbol(ir_lower_context_t *context, parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *unit = lower ? tu_state(lower->unit) : nullptr;
    n00bb_return_source_t *source = unit
        ? n00bb_node_return_source(target) : nullptr;
    if (source && source->kind == N00BB_RETURN_SOURCE_SYMBOL)
        return source->symbol;
    parse_node_resolution_t resolution = parse_tree_resolution(target);
    return resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol
        : target ? core_tree_node_value(target).sym : nullptr;
}

static ir_lower_result_t
n00bb_lower_compound_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit,
                                ir_value_t *const *operands,
                                size_t operand_count,
                                th_type *result_type, ir_span_t span,
                                void *state)
{
    (void)result_type;
    (void)state;
    if (operand_count != 2 || !operands || !operands[0] || !operands[1])
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "compound assignment needs its captured target and right operand");
    parse_annotation_t *assigns = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_ASSIGNS) : nullptr;
    if (!assigns && visit->nt)
        assigns = parse_nt_find_direct_annot(visit->nt,
                                             PARSE_ANNOT_ASSIGNS);
    parse_tree_t *target = assigns
        ? parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                       assigns->name_ref) : nullptr;
    parse_sym_entry_t *symbol = n00bb_compound_target_symbol(context, target);
    n00bb_lower_context_t *lower = context->user;
    bool field_target = member_access_node(visit->grammar, target) != nullptr;
    bool index_target = n00bb_has_index_store_target(visit->grammar, target);
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    bool global = binding && binding->kind == IR_VAL_GLOBAL;
    th_type *target_type = !core_tree_is_leaf(target)
        ? core_tree_node_value(target).type
        : local ? local->type
        : binding ? binding->type : symbol ? symbol->type : nullptr;
    if (!local && !address && !global && !field_target && !index_target)
        return lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                           "compound assignment target has no writable storage");
    if (!target_type)
        return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                           "compound assignment target has no resolved type");

    const n00bb_binary_operator_t *operator = n00bb_binary_operator_assigned(
        first_token_text(core_tree_num_children(visit->node) > 1
            ? core_tree_child(visit->node, 1) : nullptr));
    if (!operator)
        return lower_error(context, IR_DIAG_TYPE_CLASS, span,
                           "this compound assignment operator is not lowered");

    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    ir_value_t *updated = nullptr;
    if (site && site->in_place_dispatch) {
        ir_lower_result_t result = n00bb_lower_in_place_method(context, visit,
            site, operands, target_type, span);
        if (result.status != IR_LOWER_VALUE) return result;
        updated = result.value;
    }
    else if (site && site->operator_dispatch
        && !primitive_operator_site(caller, site)) {
        ir_lower_result_t result = n00bb_lower_operator_method(context, visit,
            operands, operand_count, target_type, span);
        if (result.status != IR_LOWER_VALUE) return result;
        updated = result.value;
    }
    else {
        if (!operator->shift && !ir_type_eq(context->module, target_type,
                                            operands[1]->type))
            return lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
                "compound assignment operands do not match its operator");
        ir_value_t *right = operator->shift
            ? n00bb_lower_shift_count(context, core_tree_child(visit->node, 2),
                operands[1], target_type, span) : operands[1];
        if (!right) return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
        updated = n00bb_emit_binary_ir(context, operator->op, target_type,
            operands[0], right, span);
    }
    bool stored = false;
    if (updated && local)
        stored = n00bb_emit_local_write(context, local, updated, span);
    else if (updated && global) {
        ir_instr_t *store = ir_lower_emit_instruction(context,
            IR_OP_GLOBAL_STORE, span);
        stored = store && ir_instr_operand_value(store, binding)
            && ir_instr_operand_value(store, updated);
    }
    else if (updated && index_target)
        stored = n00bb_lower_index_store_at(context, visit->grammar,
            visit->node, target, core_tree_child(visit->node, 2), updated);
    else if (updated && field_target) {
        ir_value_t *field_address = n00bb_record_field_address(context,
            visit->grammar, lower ? lower->unit : nullptr, target, span);
        stored = field_address
            && n00bb_emit_indirect_write(context, field_address, updated,
                                         span);
    }
    else if (updated)
        stored = n00bb_emit_indirect_write(context, address, updated, span);
    if (!stored)
        return lower_error(context, IR_DIAG_INTERNAL, span,
                           "could not emit compound assignment store");
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

static bool
n00bb_capture_compound_target(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    parse_tree_t *parent = core_tree_node_value(visit->node).parent;
    if (entering || !context->reachable || !parent
        || !nt_is(visit->grammar, parent, "binop-assign-stmt")
        || core_tree_child(parent, 0) != visit->node) return true;
    parse_sym_entry_t *symbol = n00bb_compound_target_symbol(context, visit->node);
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *value = ir_lower_node_value(context, visit->node);
    if (!value && core_tree_num_children(visit->node) == 1)
        value = ir_lower_node_value(context,
                                    core_tree_child(visit->node, 0));
    if (value && value->kind != IR_VAL_GLOBAL) return true;
    if (value && value->kind == IR_VAL_GLOBAL) binding = value;
    value = nullptr;
    value = local ? n00bb_emit_local_read(context, local, span)
        : address && symbol
            ? n00bb_emit_indirect_read(context, address, symbol->type, span)
            : nullptr;
    if (!value && binding && binding->kind == IR_VAL_GLOBAL) {
        ir_instr_t *snapshot = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                          span);
        if (snapshot && ir_lower_set_temp_dest(context, snapshot,
            "__n00bb_", binding->type)
            && ir_instr_operand_value(snapshot, binding)) {
            value = snapshot->dest;
        }
    }
    if (value && ir_lower_set_node_value(context, visit->node, value))
        return true;
    lower_error(context, IR_DIAG_VERIFY_PARSED, span,
                "compound assignment target has no writable storage");
    return false;
}

static const parse_sym_entry_t *
function_symbol_at(parse_grammar_t *grammar, parse_tree_t *node)
{
    return node && nt_is(grammar, node, "func-def")
        ? core_tree_node_value(node).sym : nullptr;
}

static const char *
context_hook_suffix(parse_grammar_t *grammar, parse_tree_t *hook)
{
    if (nt_is(grammar, hook, "context-enter")) return "enter";
    if (nt_is(grammar, hook, "context-exit")) return "exit";
    if (nt_is(grammar, hook, "context-error")) return "error";
    if (nt_is(grammar, hook, "context-ensure")) return "ensure";
    return nullptr;
}

static const char *
context_hook_binding_role(parse_grammar_t *grammar, parse_tree_t *hook)
{
    const char *suffix = context_hook_suffix(grammar, hook);
    if (!suffix) return nullptr;
    if (strcmp(suffix, "enter") == 0) return "n00bb.ir.context.enter";
    if (strcmp(suffix, "exit") == 0) return "n00bb.ir.context.exit";
    if (strcmp(suffix, "error") == 0) return "n00bb.ir.context.error";
    if (strcmp(suffix, "ensure") == 0) return "n00bb.ir.context.ensure";
    return nullptr;
}

static bool
context_hook_ir_name(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_tree_t *hook, char *buffer, size_t size)
{
    const char *suffix = context_hook_suffix(grammar, hook);
    return suffix && context_hook_name(symbol, suffix, buffer, size);
}

static bool
context_default_role(size_t index, char *buffer, size_t size)
{
    if (!buffer || !size) return false;
    int written = snprintf(buffer, size, "n00bb.ir.context.default.%zu",
                           index);
    return written > 0 && (size_t)written < size;
}

static bool
context_interface_parameter(parse_sym_entry_t *symbol, size_t target,
                            const char **name, size_t *name_size,
                            uint8_t *flags, uint8_t *mode)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (!data || (data[0] & 0x70u)) return false;
    bool extended = (data[0] & 0x80u) != 0;
    uint32_t count = read_u32_le(data + 1);
    if (target >= count) return false;
    size_t offset = 5;
    for (size_t i = 0; i < count; i++) {
        if (offset > SIZE_MAX - 4) return false;
        uint16_t current_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t current_flags = data[offset + 2];
        uint8_t current_mode = data[offset + 3];
        offset += 4;
        if (!current_size || current_size > SIZE_MAX - offset) return false;
        if (i == target) {
            if (name) *name = (const char *)data + offset;
            if (name_size) *name_size = current_size;
            if (flags) *flags = current_flags;
            if (mode) *mode = current_mode;
            return true;
        }
        offset += current_size;
        if (extended && (current_flags & 4u)) {
            if (offset > SIZE_MAX - 2) return false;
            uint16_t dependencies = (uint16_t)data[offset]
                | (uint16_t)data[offset + 1] << 8;
            offset += 2;
            if (dependencies > (SIZE_MAX - offset) / 2) return false;
            offset += (size_t)dependencies * 2;
        }
    }
    return false;
}

static bool
context_interface_dependencies(parse_sym_entry_t *symbol, size_t target,
                                size_t *indices, size_t capacity,
                                size_t *count)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (count) *count = 0;
    if (!data || !(data[0] & 0x80u) || (data[0] & 0x70u))
        return false;
    uint32_t parameter_count = read_u32_le(data + 1);
    if (target >= parameter_count) return false;
    size_t offset = 5;
    for (size_t i = 0; i < parameter_count; i++) {
        if (offset > SIZE_MAX - 4) return false;
        uint16_t name_size = (uint16_t)data[offset]
            | (uint16_t)data[offset + 1] << 8;
        uint8_t flags = data[offset + 2];
        offset += 4;
        if (!name_size || name_size > SIZE_MAX - offset) return false;
        offset += name_size;
        size_t dependency_count = 0;
        if (flags & 4u) {
            if (offset > SIZE_MAX - 2) return false;
            dependency_count = (uint16_t)data[offset]
                | (uint16_t)data[offset + 1] << 8;
            offset += 2;
            if (dependency_count > (SIZE_MAX - offset) / 2) return false;
            if (i == target && dependency_count > capacity) return false;
            for (size_t d = 0; d < dependency_count; d++) {
                size_t dependency = (uint16_t)data[offset]
                    | (uint16_t)data[offset + 1] << 8;
                if (i == target && indices) indices[d] = dependency;
                offset += 2;
            }
        }
        if (i == target) {
            if (count) *count = dependency_count;
            return true;
        }
    }
    return false;
}

static size_t
context_interface_parameter_count(parse_sym_entry_t *symbol)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    return data ? read_u32_le(data + 1) : 0;
}

static bool
context_interface_parameter_named(parse_sym_entry_t *symbol,
                                  core_string_t target, size_t *index)
{
    const uint8_t *data = symbol ? symbol->user_data : nullptr;
    if (!data) return false;
    uint32_t count = read_u32_le(data + 1);
    for (size_t i = 0; i < count; i++) {
        const char *name = nullptr;
        size_t name_size = 0;
        if (!context_interface_parameter(symbol, i, &name, &name_size,
                                         nullptr, nullptr)) return false;
        if (name_size == target.u8_bytes
            && !memcmp(name, target.data, name_size)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

static th_type *
context_hook_ir_type(th_ctx *types, th_type *source_type)
{
    if (!types || !source_type || th_kind_of(source_type) != TH_FUNCTION)
        return nullptr;
    core_list_t(th_param) parameters = core_list_new(th_param);
    for (size_t i = 0; i < th_func_nparams(source_type); i++) {
        const th_param *parameter = th_func_param(source_type, i);
        if (!parameter) {
            core_list_free(parameters);
            return nullptr;
        }
        core_list_push(parameters, *parameter);
    }
    const th_param *rest = th_func_rest(source_type);
    if (rest) {
        th_type *array = th_array(types, rest->type);
        if (!array) {
            core_list_free(parameters);
            return nullptr;
        }
        core_list_push(parameters, ((th_param){.mode = TH_IN, .type = array}));
    }
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    for (size_t i = 0; i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) {
            core_list_free(parameters);
            core_list_free(keywords);
            return nullptr;
        }
        core_list_push(keywords, *keyword);
    }
    th_type *type = th_func_with_error(types, th_func_ret(source_type),
        th_func_error(source_type), parameters.data, parameters.len, nullptr,
        keywords.data, keywords.len);
    core_list_free(parameters);
    core_list_free(keywords);
    return type;
}

static bool
add_context_hook_functions(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           n00bb_link_analysis_t *analysis,
                           core_link_unit_t *link,
                           uint32_t producer_stage)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        state, CORE_STRING_STATIC(""));
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (!context_symbol(grammar, symbol)) continue;
            if (state->interface_module) {
                core_link_declaration_t *context_declaration =
                    link_declaration_for_symbol(analysis, symbol);
                static const struct {
                    const char *suffix;
                    const char *role;
                } interface_hooks[] = {
                    {"enter", "n00bb.ir.context.enter"},
                    {"exit", "n00bb.ir.context.exit"},
                    {"error", "n00bb.ir.context.error"},
                    {"ensure", "n00bb.ir.context.ensure"},
                };
                for (size_t h = 0; ok && h <
                     sizeof(interface_hooks) / sizeof(interface_hooks[0]); h++) {
                    char name[256];
                    if (!context_hook_name(symbol, interface_hooks[h].suffix,
                                           name, sizeof(name))) {
                        ok = false;
                        break;
                    }
                    parse_sym_entry_t *hook_symbol = parse_symtab_lookup_scoped(
                        state->annotations->symtab, symbol->scope,
                        CORE_STRING_STATIC(""),
                        core_string_borrow(name, strlen(name)));
                    if (!hook_symbol) continue;
                    core_link_declaration_t *hook_declaration =
                        link_declaration_for_symbol(analysis, hook_symbol);
                    ir_func_t *function = hook_declaration
                        ? core_link_declaration_binding(hook_declaration,
                            CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
                    ok = context_declaration && function
                        && core_link_declaration_set_binding(link,
                            context_declaration,
                            core_string_from_cstr(interface_hooks[h].role),
                            producer_stage, function);
                }
                for (size_t p = 0; ok && p <
                     context_interface_parameter_count(symbol); p++) {
                    uint8_t flags = 0;
                    if (!context_interface_parameter(symbol, p, nullptr,
                                                     nullptr, &flags, nullptr)) {
                        ok = false;
                        break;
                    }
                    if (!(flags & 4u)) continue;
                    char name[256], role[64];
                    if (!context_default_name(symbol, p, name,
                                              sizeof(name))
                        || !context_default_role(p, role, sizeof(role))) {
                        ok = false;
                        break;
                    }
                    parse_sym_entry_t *helper_symbol =
                        parse_symtab_lookup_scoped(state->annotations->symtab,
                            symbol->scope, CORE_STRING_STATIC(""),
                            core_string_borrow(name, strlen(name)));
                    core_link_declaration_t *helper_declaration =
                        link_declaration_for_symbol(analysis, helper_symbol);
                    ir_func_t *function = helper_declaration
                        ? core_link_declaration_binding(helper_declaration,
                            CORE_STRING_STATIC("n00bb.ir.function")) : nullptr;
                    ok = helper_symbol && context_declaration && function
                        && core_link_declaration_set_binding(link,
                            context_declaration,
                            core_string_from_cstr(role), producer_stage,
                            function);
                }
                continue;
            }
            static const char *hooks[] = {
                "context-enter", "context-exit", "context-error",
                "context-ensure",
            };
            for (size_t i = 0; ok && i < sizeof(hooks) / sizeof(hooks[0]);
                 i++) {
                const char *hook_role = n00bb_context_hook_role(hooks[i]);
                parse_tree_t *hook = hook_role ? parse_sym_binding(symbol,
                    core_string_borrow(hook_role, strlen(hook_role))) : nullptr;
                if (!hook) continue;
                char name[256];
                th_type *type = context_hook_ir_type(state->types,
                    core_tree_node_value(hook).type);
                ir_func_t *function = context_hook_ir_name(
                        grammar, symbol, hook, name, sizeof(name))
                    && type && !ir_func_find(state->ir_module, name)
                    ? ir_func_add(state->ir_module, name, type,
                                  IR_LINK_INTERNAL) : nullptr;
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                const char *role = context_hook_binding_role(grammar, hook);
                ok = function && declaration && role
                    && core_link_declaration_set_binding(link, declaration,
                        core_string_from_cstr(role), producer_stage,
                        function);
            }
            core_list_t(parse_sym_entry_ptr_t) parameters =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            collect_ir_parameters(grammar,
                child_nt(grammar, symbol->decl_node, "param-decl"),
                &parameters, &rest);
            if (rest) core_list_push(parameters, rest);
            for (size_t i = 0; ok && i < parameters.len; i++) {
                parse_sym_entry_t *parameter = parameters.data[i];
                if (!parameter || !parameter->is_optional) continue;
                parse_tree_t *default_expr = child_nt(grammar,
                    parameter->decl_node, "expression");
                core_list_t(parse_sym_entry_ptr_t) dependencies =
                    core_list_new(parse_sym_entry_ptr_t);
                context_default_dependencies(default_expr,
                    &parameters, &dependencies);
                th_type *type = context_default_ir_type(state->types,
                    symbol->type, &dependencies, parameter->type);
                char name[256], role[64];
                bool named = context_default_name(symbol, i, name,
                                                   sizeof(name))
                    && context_default_role(i, role, sizeof(role));
                ir_func_t *function = named && type
                    && !ir_func_find(state->ir_module, name)
                    ? ir_func_add(state->ir_module, name, type,
                                  IR_LINK_INTERNAL) : nullptr;
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                ok = function && declaration
                    && core_link_declaration_set_binding(link, declaration,
                        core_string_from_cstr(role), producer_stage,
                        function);
                core_list_free(dependencies);
            }
            core_list_free(parameters);
            (void)rest;
        }
    }
    core_list_free(scopes);
    return ok;
}


static bool
n00bb_failure_channel_accepts(ir_lower_context_t *context,
                              th_type *failure)
{
    th_type *caller_error = context && context->function
        ? th_func_error(context->function->type) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    if (!types || !caller_error || !failure
        || th_is_nil(caller_error)) return false;
    if (th_kind_of(th_resolve(caller_error)) == TH_VAR
        || th_kind_of(th_resolve(failure)) == TH_VAR)
        return th_unify_in_place(types, caller_error, failure);
    return ir_type_eq(context->module, caller_error, failure);
}

static bool
n00bb_reject_runtime_initializer_failure(ir_lower_context_t *context,
                                         ir_span_t span)
{
    ir_func_t *function = context ? context->function : nullptr;
    if (!function || !ir_func_is_initializer(function)
        || ir_func_execution_phase(function) != IR_PHASE_RUNTIME) return false;
    lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
        "a runtime module initializer has no failure channel; this operation may fail");
    return true;
}

static bool
register_nominal_types(parse_grammar_t *grammar,
                       core_translation_unit_t *unit, ir_module_t *module,
                       bool imported)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab)
        return false;
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                || !!parse_sym_binding_origin(symbol) != imported) continue;
            if (core_string_eq(symbol->adt_kind,
                               CORE_STRING_STATIC("callback"))) {
                th_ctx *types = ir_module_types(module);
                th_type *fields[] = {
                    th_name(types, "u32"), th_name(types, "u32"),
                    th_ind(types, 0, th_name(types, "byte")),
                };
                th_type *representation = fields[2]
                    ? th_tuple(types, fields, 3) : nullptr;
                if (!representation
                    || (!th_type_definition(types, symbol->type)
                        && !th_type_define(types, symbol->type,
                                           representation))
                    || !ir_type_define(module, symbol->type, representation))
                    return false;
                continue;
            }
            if (core_string_eq(symbol->adt_kind,
                               CORE_STRING_STATIC("enum"))) {
                const n00bb_enum_range_t *range = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.enum-range"));
                if (!range && symbol->binding_origin)
                    range = parse_sym_binding(symbol->binding_origin,
                        CORE_STRING_STATIC("n00bb.enum-range"));
                uint64_t width = range
                    ? range->magnitude_bits + range->is_signed : 0;
                if (range && !width) width = 1;
                th_type *representation = range && width <= 128
                    ? th_bitint(ir_module_types(module), width,
                                range->is_signed) : nullptr;
                if (!representation || !ir_type_define(module, symbol->type,
                                                        representation))
                    return false;
                continue;
            }
            if (symbol->member_scope
                && core_string_eq(symbol->member_scope->scope_tag,
                                  CORE_STRING_STATIC("flagset"))) {
                const n00bb_flagset_info_t *info = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.flagset-info"));
                if (!info && symbol->binding_origin)
                    info = parse_sym_binding(symbol->binding_origin,
                        CORE_STRING_STATIC("n00bb.flagset-info"));
                th_type *representation = info && info->atomic_count
                    && info->atomic_count <= 128
                    ? th_bitint(ir_module_types(module), info->atomic_count,
                                false) : nullptr;
                if (!representation || !ir_type_define(module, symbol->type,
                                                        representation))
                    return false;
                continue;
            }
            bool nominal = symbol->decl_node
                ? nt_is(grammar, symbol->decl_node, "alias-stmt")
                    && !alias_declaration_is_transparent(symbol->decl_node)
                : core_string_eq(symbol->adt_kind, CORE_STRING_STATIC("nominal"));
            if (!nominal) continue;
            th_type *representation = th_type_definition(state->types, symbol->type);
            if (!representation || !ir_type_define(module, symbol->type,
                                                    representation)) return false;
        }
    }
    return true;
}

static bool
register_variant_types(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->annotations
        || !state->annotations->symtab) return false;
    parse_symtab_t *symtab = state->annotations->symtab;
    bool ok = true;
    for (parse_scope_t *scope = symtab->all_scopes; ok && scope;
         scope = scope->all_next) {
        bool expected_namespace = scope->ns &&
            core_string_eq(scope->ns->ns_name,
                state->interface_module
                    ? CORE_STRING_STATIC("type")
                    : CORE_STRING_STATIC(""));
        if (!expected_namespace
            || !core_string_eq(scope->scope_tag,
                            CORE_STRING_STATIC("variant"))) continue;
        core_list_t(th_member) members = core_list_new(th_member);
        core_list_t(parse_sym_entry_ptr_t) ordered =
            variant_arms_in_source_order(grammar, scope);
        for (size_t i = 0; ok && i < ordered.len; i++) {
            parse_sym_entry_t *arm = ordered.data[i];
            th_type *payload = arm->decl_node
                ? declared_type_from_node(grammar, unit, arm->decl_node)
                : arm->type;
            if (!payload) { ok = false; break; }
            core_list_push(members, ((th_member){
                .name = arm->name.data,
                .type = payload,
            }));
        }
        core_buffer_t qualified = {0};
        append_module_type_name(unit, scope->name, &qualified);
        parse_sym_entry_t *symbol = scope->node
            ? core_tree_node_value(scope->node).sym : nullptr;
        th_type *variant = ok
            ? variant_template_type(state->types, qualified.data, symbol)
            : nullptr;
        th_type *definition = ok
            ? th_record(state->types, members.data, members.len) : nullptr;
        ok = variant && definition
            && ir_type_define(destination, variant, definition);
        core_buffer_release(&qualified);
        core_list_free(members);
        core_list_free(ordered);
    }
    return ok;
}

static bool
register_record_types(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      ir_module_t *destination, ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->types || !state->annotations
        || !state->annotations->symtab) return false;
    if (state->interface_module) {
        core_list_t(parse_scope_ptr_t) types = link_declaration_scopes(
            state, CORE_STRING_STATIC("type"));
        bool valid = true;
        for (size_t s = 0; valid && s < types.len; s++) {
            for (parse_sym_entry_t *symbol = types.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                parse_scope_t *members = symbol->member_scope;
                if (symbol->kind != PARSE_SYM_TYPEDEF || !members ||
                    !core_string_eq(members->scope_tag,
                                    CORE_STRING_STATIC("type"))) continue;
                n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.export-origin"));
                th_type *template = origin
                    ? record_template_type(state->types,
                        origin->declaration.data, symbol) : nullptr;
                th_type *record = th_type_definition(state->types,
                                                     template);
                valid = record && th_kind_of(record) == TH_RECORD &&
                        ir_type_define(destination, template, record);
                for (parse_sym_entry_t *field = members->first_in_scope;
                     valid && field; field = field->next_in_scope) {
                    if (!field->is_field ||
                        !(field->has_bit_offset || field->has_bit_width))
                        continue;
                    valid = ir_layout_pin_member(destination, template,
                        field->name.data, field->bit_offset, field->bit_width);
                }
                if (!valid) break;
            }
        }
        core_list_free(types);
        if (!valid) return false;
        return true;
    }
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        if (!scope_in_value_namespace(scope)
            || (!core_string_eq(scope->scope_tag, CORE_STRING_STATIC("type"))
                && !core_string_eq(scope->scope_tag,
                                   CORE_STRING_STATIC("record")))
            || !scope->node
            || !field_record_declaration(grammar, scope->node))
            continue;
        core_list_t(th_member) members = core_list_new(th_member);
        core_list_t(parse_sym_entry_ptr_t) fields =
            core_list_new(parse_sym_entry_ptr_t);
        record_fields_in_source_order(scope, &fields);
        bool valid = true;
        for (size_t i = 0; i < fields.len; i++) {
            parse_sym_entry_t *field = fields.data[i];
            th_type *field_type = declared_type_from_node(grammar, unit,
                                                           field->decl_node);
            if (!field_type) { valid = false; break; }
            core_list_push(members, ((th_member){
                .name = field->name.data,
                .type = field_type,
            }));
        }
        parse_sym_entry_t *symbol = core_tree_node_value(scope->node).sym;
        core_buffer_t qualified = {0};
        append_module_type_name(unit, scope->name, &qualified);
        th_type *named = valid && symbol
            ? record_template_type(state->types, qualified.data, symbol)
            : nullptr;
        th_type *record = valid
            ? th_record(state->types, members.data, members.len) : nullptr;
        valid = named && record
            && th_type_define(state->types, named, record)
            && ir_type_define(destination, named, record);
        for (parse_sym_entry_t *field = scope->first_in_scope;
             valid && field; field = field->next_in_scope) {
            if (!field->is_field
                || !(field->has_bit_offset || field->has_bit_width)) continue;
            valid = ir_layout_pin_member(destination, named,
                field->name.data, field->bit_offset, field->bit_width);
        }
        core_list_free(fields);
        core_list_free(members);
        core_buffer_release(&qualified);
        if (!valid) {
            ir_diags_t *diagnostics = ir_diags_new_attached(
                ir_context, nullptr,
                core_translation_unit_diagnostics(unit), unit);
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_TYPE_MISMATCH, ir_module_span(state->ir_module),
                "this record declaration");
            ir_diag_message(diagnostic, diagnostics,
                "could not register the record representation in Lantern IR");
            return false;
        }
    }
    return true;
}

static bool
register_imported_record_layouts(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 ir_module_t *destination)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !destination || !state->types || !state->annotations
        || !state->annotations->symtab) return false;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        state, CORE_STRING_STATIC("type"));
    for (size_t s = 0; s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            parse_scope_t *members = symbol->member_scope;
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type ||
                !members || !core_string_eq(members->scope_tag,
                    CORE_STRING_STATIC("type"))) continue;
            core_buffer_t qualified = {0};
            n00bb_export_origin_t *origin = state->interface_module
                ? parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.export-origin")) : nullptr;
            if (!state->interface_module)
                append_module_type_name(unit, symbol->name, &qualified);
            th_type *template = (origin || !state->interface_module)
                ? record_template_type(state->types,
                    origin ? origin->declaration.data : qualified.data,
                    symbol) : nullptr;
            th_type *shape = template
                ? th_type_definition(state->types, template) : nullptr;
            core_buffer_release(&qualified);
            if (!shape) continue;
            th_type *existing = ir_type_definition(destination, template);
            bool valid = existing
                ? th_hash(state->types, existing) ==
                    th_hash(state->types, shape)
                : ir_type_define(destination, template, shape);
            for (parse_sym_entry_t *field = members->first_in_scope;
                 valid && field; field = field->next_in_scope) {
                if (!(field->has_bit_offset || field->has_bit_width)) continue;
                valid = ir_layout_pin_member(destination, template,
                    field->name.data, field->bit_offset, field->bit_width);
            }
            if (valid)
                valid = ir_layout_ensure(destination, template,
                    ir_module_target(destination)) != nullptr;
            if (!valid) {
                core_list_free(scopes);
                return false;
            }
        }
    }
    core_list_free(scopes);
    (void)grammar;
    return true;
}

static th_type *
static_global_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                   parse_sym_entry_t *symbol)
{
    th_type *inferred = th_resolve(symbol ? symbol->type : nullptr);
    th_type *declared = symbol
        ? declared_type_from_node(grammar, unit, symbol->decl_node) : nullptr;
    if (!declared) return inferred;
    declared = th_resolve(declared);
    if (inferred && th_kind_of(inferred) == TH_NAMED
        && th_kind_of(declared) == TH_NAMED
        && th_named_nargs(inferred) > 0 && th_named_nargs(declared) == 0)
        return inferred;
    return declared;
}

static void
zero_fixed_shape_release_items(ir_global_init_value_t *value)
{
    if (value->kind != IR_GLOBAL_INIT_SEQUENCE
        && value->kind != IR_GLOBAL_INIT_RECORD) return;
    ir_global_init_value_t *items =
        (ir_global_init_value_t *)value->aggregate.items;
    for (size_t i = 0; i < value->aggregate.count; i++)
        zero_fixed_shape_release_items(&items[i]);
    core_free(items);
}

static void
zero_fixed_shape_release(ir_global_init_value_t *value)
{
    if (!value) return;
    zero_fixed_shape_release_items(value);
    core_free(value);
}

static ir_global_init_value_t *
zero_fixed_shape_initializer(ir_module_t *module, th_type *type,
                             const ir_shape_t *shape, uint32_t dimension)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(type)) : nullptr;
    ir_shape_extent_t extent;
    if (!shape || !element || !ir_shape_extent(shape, dimension, &extent)
        || extent.kind != IR_SHAPE_EXTENT_STATIC
        || extent.static_value > SIZE_MAX / sizeof(ir_global_init_value_t))
        return nullptr;
    size_t count = (size_t)extent.static_value;
    ir_global_init_value_t *value = core_alloc_array(ir_global_init_value_t, 1);
    ir_global_init_value_t *items = core_alloc_array(ir_global_init_value_t,
                                                     count ? count : 1);
    memset(items, 0, (count ? count : 1) * sizeof(*items));
    *value = (ir_global_init_value_t){
        .kind = IR_GLOBAL_INIT_SEQUENCE,
        .aggregate = {.items = items, .count = count},
    };
    bool nested = th_kind_of(element) == TH_SEQUENCE;
    ir_class_t category = ir_machine_of(module, element).class;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        if (nested) {
            ir_global_init_value_t *inner = zero_fixed_shape_initializer(
                module, element, shape, dimension + 1);
            valid = inner != nullptr;
            if (valid) {
                items[i] = *inner;
                core_free(inner);
            }
        }
        else if (category == IR_CLASS_SINT || category == IR_CLASS_UINT)
            items[i].kind = IR_GLOBAL_INIT_INT;
        else if (category == IR_CLASS_BOOL)
            items[i].kind = IR_GLOBAL_INIT_BOOL;
        else if (category == IR_CLASS_FLOAT)
            items[i].kind = IR_GLOBAL_INIT_FLOAT;
        else valid = false;
    }
    if (!valid) {
        zero_fixed_shape_release(value);
        return nullptr;
    }
    return value;
}

static bool
zero_member_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, const ir_shape_t *shape,
                        size_t depth, bool *shaped,
                        ir_global_init_value_t *out);

static ir_global_init_value_t *
zero_record_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, size_t depth, bool *shaped)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *record = depth < 16 ? static_record_shape(state, type) : nullptr;
    parse_sym_entry_t *owner = record ? type_symbol_in_loaded_imports(unit,
        state->types, type, nullptr) : nullptr;
    if (!record || th_kind_of(record) != TH_RECORD || !owner
        || !owner->member_scope) return nullptr;
    size_t count = th_record_len(record);
    ir_global_init_value_t *value = core_alloc_array(ir_global_init_value_t, 1);
    ir_global_init_value_t *items = core_alloc_array(ir_global_init_value_t,
                                                     count ? count : 1);
    memset(items, 0, (count ? count : 1) * sizeof(*items));
    *value = (ir_global_init_value_t){
        .kind = IR_GLOBAL_INIT_RECORD,
        .aggregate = {.items = items, .count = count},
    };
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        const th_member *member = th_record_member(record, i);
        parse_sym_entry_t *field = nullptr;
        for (parse_sym_entry_t *candidate = owner->member_scope->first_in_scope;
             candidate && member && !field; candidate = candidate->next_in_scope)
            if (candidate->is_field && core_string_eq(candidate->name,
                    core_string_from_cstr(member->name))) field = candidate;
        valid = member && zero_member_initializer(unit, module, member->type,
            field ? parse_sym_binding(field,
                CORE_STRING_STATIC("ir.array-shape")) : nullptr,
            depth + 1, shaped, &items[i]);
        if (valid) items[i].member = member->name;
    }
    if (!valid) {
        zero_fixed_shape_release(value);
        return nullptr;
    }
    return value;
}

static bool
zero_member_initializer(core_translation_unit_t *unit, ir_module_t *module,
                        th_type *type, const ir_shape_t *shape,
                        size_t depth, bool *shaped,
                        ir_global_init_value_t *out)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;
    if (!resolved) return false;
    if (th_kind_of(resolved) == TH_SEQUENCE) {
        ir_global_init_value_t *array = zero_fixed_shape_initializer(module,
            resolved, shape, 0);
        if (!array) return false;
        *out = *array;
        core_free(array);
        *shaped = true;
        return true;
    }
    if (n00bb_option_type_element(resolved, nullptr)) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_NIL};
        return true;
    }
    ir_class_t category = ir_machine_of(module, resolved).class;
    if (category == IR_CLASS_SINT || category == IR_CLASS_UINT) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_INT};
        return true;
    }
    if (category == IR_CLASS_BOOL) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_BOOL};
        return true;
    }
    if (category == IR_CLASS_FLOAT) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_FLOAT};
        return true;
    }
    if (th_builtin_kind(ir_module_types(module), resolved)
            == TH_BUILTIN_STRING) {
        *out = (ir_global_init_value_t){.kind = IR_GLOBAL_INIT_STRING};
        return true;
    }
    ir_global_init_value_t *record = zero_record_initializer(unit, module,
        resolved, depth, shaped);
    if (!record) return false;
    *out = *record;
    core_free(record);
    return true;
}

static bool
initialize_static_global(parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         ir_module_t *module,
                         parse_sym_entry_t *symbol, ir_global_t *global,
                         ir_diags_t *diagnostics, int32_t file_id)
{
    parse_tree_t *declaration = symbol->decl_node;
    parse_tree_t *initializer = nt_is(grammar, declaration, "sym-decl")
        ? child_nt(grammar, declaration, "expression") : nullptr;
    if (!initializer && (!symbol->has_comptime_value
                         || !symbol->comptime_value)) {
        if (symbol->mutability == PARSE_SYM_CONST) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(&(ir_lower_context_t){.file_id = file_id},
                                    declaration),
                "this module global");
            ir_diag_message(diagnostic, diagnostics,
                "const globals require a compile-time initializer");
            return false;
        }
        ir_global_init_value_t *zero = zero_fixed_shape_initializer(module,
            global->type, global->shape, 0);
        bool shaped = false;
        if (!zero) zero = zero_record_initializer(unit, module, global->type,
                                                  0, &shaped);
        if (zero && zero->kind == IR_GLOBAL_INIT_RECORD && !shaped) {
            zero_fixed_shape_release(zero);
            zero = nullptr;
        }
        bool zeroed = !zero
            || ir_global_set_static_initializer(module, global, zero);
        zero_fixed_shape_release(zero);
        return zeroed;
    }

    n00bb_tu_state_t *state = tu_state(unit);
    th_type *static_type = static_global_type(grammar, unit, symbol);
    n00bb_static_value_t derived_value;
    const n00bb_static_value_t *static_value = nullptr;
    if (state && initializer
        && semantic_static_value_from_expression(grammar, initializer,
            static_type, state, &derived_value, 0)
        && static_value_matches_type(&derived_value, static_type, state, 0))
        static_value = &derived_value;
    if (!static_value && symbol->has_comptime_value && symbol->comptime_value)
        static_value = symbol->comptime_value;

    ir_span_t span = initializer
        ? lower_span_for_tree(
            &(ir_lower_context_t){.file_id = file_id}, initializer)
        : (ir_span_t){0};
    ir_global_init_value_t value = {0};
    bool initialized = static_value && n00bb_static_value_to_ir_initializer(
        static_value, static_type, state, &value, 0)
        && ir_global_set_static_initializer(module, global, &value);

    if (!initialized) {
        if (symbol->mutability != PARSE_SYM_CONST) return true;
        if (!th_is_nil(static_type)) return true;
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_TYPE_LITERAL, span, "this global initializer");
        ir_diag_message(diagnostic, diagnostics,
            "nil cannot be stored as an ordinary value");
        return false;
    }
    return true;
}

static bool
n00bb_unit_defines_global(n00bb_link_analysis_t *analysis,
                          core_translation_unit_t *unit,
                          parse_sym_entry_t *symbol)
{
    core_link_declaration_t *declaration =
        link_declaration_for_symbol(analysis, symbol);
    return !declaration || core_string_eq(
        core_link_declaration_module(declaration), unit->module.identifier);
}

static bool
bake_const_global(core_translation_unit_t *source, ir_module_t *destination,
                  parse_sym_entry_t *symbol, ir_global_t *global,
                  n00bb_link_analysis_t *analysis)
{
    const char *identifier = n00bb_baked_root_identifier(analysis,
        source->module.identifier, symbol->name);
    if (identifier)
        return ir_global_set_phase_result_initializer(destination, global,
                                                      identifier);
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "const '%.*s' has no value from the build-time run",
        (int)symbol->name.u8_bytes, symbol->name.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(source),
        source, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0026"),
        core_string_borrow(message.data, message.byte_len),
        link_source_location(symbol->decl_node));
    core_buffer_release(&message);
    return false;
}

static bool
register_module_globals(parse_grammar_t *grammar,
                        core_translation_unit_t *source,
                        core_translation_unit_t *destination_unit,
                        ir_module_t *destination,
                        ir_ctx_t *ir_context,
                        n00bb_link_analysis_t *analysis,
                        core_link_unit_t *link, uint32_t producer_stage)
{
    n00bb_tu_state_t *source_state = tu_state(source);
    if (!source_state || !source_state->annotations || !destination)
        return false;
    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(source), source);
    if (!diagnostics) return false;
    int32_t source_file = source_state->ir_module
        ? ir_file_count(source_state->ir_module) - 1 : -1;
    core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
        source_state, CORE_STRING_STATIC(""));
    bool ok = true;
    for (size_t s = 0; ok && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             ok && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE
                || context_symbol(grammar, symbol)
                || (!symbol_binding_node(symbol)
                    && !source_state->interface_module)
                || !symbol->type
                || !n00bb_unit_defines_global(analysis, source, symbol))
                continue;
            if (source != destination_unit) {
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                bool referenced = false;
                for (size_t r = 0; declaration && !referenced
                     && r < core_link_unit_reference_count(link); r++) {
                    core_link_reference_t *reference =
                        core_link_unit_reference_at_index(link, r);
                    referenced = core_link_reference_referrer(reference)
                            == destination_unit
                        && core_link_reference_target(reference)
                            == declaration;
                }
                if (!referenced) continue;
            }
            core_buffer_t name = {0};
            append_module_symbol_name(source, symbol, &name);
            th_type *global_type = static_global_type(grammar, source,
                                                       symbol);
            ir_global_t *global = global_type
                ? ir_global_add(destination, name.data, global_type) : nullptr;
            ir_shape_t *array_shape = parse_sym_binding(symbol,
                CORE_STRING_STATIC("ir.array-shape"));
            bool local_owner = destination == source_state->ir_module;
            bool canonical_owner = local_owner;
            bool local_definition = local_owner
                && !source_state->interface_module;
            if (global) {
                bool shape_bound = !array_shape
                    || ir_global_set_shape(global, array_shape);
                global->is_extern = !local_definition;
                global->is_static = local_definition
                    && !symbol_is_public_declaration(symbol);
                bool writable = parse_sym_can_write(symbol,
                                                    n00bb_compile_phase());
                global->mutability = writable ? IR_MUT_MUTABLE
                    : symbol->mutability == PARSE_SYM_CONST ? IR_MUT_CONST
                    : IR_MUT_IMMUTABLE;
                th_type *referent = th_resolve(global->type);
                ok = shape_bound
                    && (!referent || th_kind_of(referent) != TH_INDIRECTION
                        || ir_value_set_indirect_access(ir_context,
                            ir_global_address(destination, global),
                            IR_INDIRECT_ACCESS_READ_WRITE))
                    && ir_global_set_phase_access(ir_context,
                    global, IR_PHASE_ACCESS_RUNTIME,
                    writable ? IR_PHASE_ACCESS_RUNTIME : IR_PHASE_ACCESS_NONE,
                    IR_PHASE_ACCESS_RUNTIME);
                ir_storage_duration_t duration =
                    IR_STORAGE_DURATION_UNSPECIFIED;
                switch (symbol->storage_duration) {
                case PARSE_SYM_STORAGE_COMPTIME:
                    duration = IR_STORAGE_DURATION_COMPTIME;
                    break;
                case PARSE_SYM_STORAGE_AUTOMATIC:
                    duration = IR_STORAGE_DURATION_AUTOMATIC;
                    break;
                case PARSE_SYM_STORAGE_MODULE:
                    duration = IR_STORAGE_DURATION_MODULE;
                    break;
                case PARSE_SYM_STORAGE_THREAD:
                    duration = IR_STORAGE_DURATION_THREAD;
                    break;
                case PARSE_SYM_STORAGE_DYNAMIC:
                    duration = IR_STORAGE_DURATION_DYNAMIC;
                    break;
                case PARSE_SYM_STORAGE_UNSPECIFIED:
                    break;
                }
                if (ok)
                    ok = ir_global_set_storage_duration(
                        ir_context, global, duration)
                        && ir_global_set_storage_class(ir_context, global,
                            symbol->requested_storage_class.data,
                            symbol->storage_class_required,
                            symbol->selected_storage_class.data);
                if (symbol->is_thread_local)
                    ir_annot_set(ir_context, &global->annots,
                        (ir_annot_t){.key = "n00b.thread_local",
                                     .kind = IR_ANNOT_BOOL, .b = true});
                if (local_owner) {
                    if (symbol->decl_node)
                        global->span = lower_span_for_tree(
                            &(ir_lower_context_t){.file_id = source_file},
                            symbol->decl_node);
                    if (ok && local_definition)
                        ok = initialize_static_global(grammar, source,
                            destination, symbol, global, diagnostics,
                            source_file);
                    bool dynamic_const = ok && local_definition
                        && symbol->mutability == PARSE_SYM_CONST
                        && !global->has_static_initializer;
                    if (dynamic_const
                        && n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME)
                        analysis->build_time_capture = true;
                    else if (dynamic_const)
                        ok = bake_const_global(source, destination, symbol,
                            global, analysis);
                    core_link_declaration_t *declaration = canonical_owner
                        ? link_declaration_for_symbol(analysis, symbol)
                        : nullptr;
                    bool already_bound = declaration
                        && core_link_declaration_binding(declaration,
                            CORE_STRING_STATIC("n00bb.ir.global"));
                    if (ok && canonical_owner && (!declaration
                        || ((!source_state->interface_module
                                || !already_bound)
                            && !core_link_declaration_set_binding(link,
                                declaration,
                                CORE_STRING_STATIC("n00bb.ir.global"),
                                producer_stage, global)))) {
                        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                            IR_DIAG_VERIFY_PARSED,
                            lower_span_for_tree(
                                &(ir_lower_context_t){.file_id = source_file},
                                symbol->decl_node),
                            "this module global");
                        ir_diag_message(diagnostic, diagnostics,
                            "could not bind its IR global to the canonical declaration");
                        ok = false;
                    }
                }
            }
            else {
                ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                    IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(
                        &(ir_lower_context_t){.file_id = source_file},
                        symbol->decl_node),
                    "this module global");
                ir_diag_message(diagnostic, diagnostics,
                    "could not create its IR global declaration");
                ok = false;
            }
            core_buffer_release(&name);
        }
    }
    core_list_free(scopes);
    return ok && !ir_diags_failed(diagnostics);
}

static bool
bind_module_global_identities(ir_ctx_t *ir_context,
                              n00bb_link_analysis_t *analysis)
{
    if (!ir_context || !analysis) return false;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *source = analysis->units.data[i];
        n00bb_tu_state_t *source_state = tu_state(source);
        if (!source_state || !source_state->annotations) return false;
        core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
            source_state, CORE_STRING_STATIC(""));
        bool ok = true;
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    || !n00bb_unit_defines_global(analysis, source, symbol)
                    || !is_module_global_symbol(analysis->grammar, symbol))
                    continue;
                ir_global_t *canonical = link_ir_global_for_symbol(analysis,
                                                                    symbol);
                if (!canonical) continue;
                core_buffer_t name = {0};
                append_module_symbol_name(source, symbol, &name);
                for (size_t j = 0; ok && j < analysis->units.len; j++) {
                    ir_module_t *module = unit_module(
                        tu_state(analysis->units.data[j]));
                    ir_global_t *view = module
                        ? ir_global_find(module, name.data) : nullptr;
                    if (view)
                        ok = ir_global_set_identity(ir_context, view,
                                                    canonical);
                }
                core_buffer_release(&name);
            }
        }
        core_list_free(scopes);
        if (!ok) return false;
    }
    return true;
}

static th_type *
lowered_rest_type(th_ctx *types, const th_param *source, th_mode mode,
                  th_param *lowered)
{
    if (!types || !source || !lowered || !th_mode_is_valid(mode))
        return nullptr;
    th_type *type = mode == TH_INOUT
        ? th_ind(types, 0, source->type) : source->type;
    if (!type) return nullptr;
    *lowered = (th_param){
        .mode = mode == TH_CONSUME ? TH_CONSUME : TH_IN,
        .type = type,
    };
    return type;
}

static th_type *
function_semantic_shape(parse_grammar_t *grammar, core_translation_unit_t *unit,
                        parse_sym_entry_t *symbol, th_type *source_type)
{
    n00bb_tu_state_t *state = tu_state(unit);
    source_type = source_type ? th_resolve(source_type) : nullptr;
    if (!state || !symbol || !source_type
        || th_kind_of(source_type) != TH_FUNCTION) return nullptr;
    if (parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-value-method")))
        return source_type;
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       "func-type-signature");
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_symbol = nullptr;
    collect_ir_parameters(grammar, signature, &formals, &rest_symbol);
    if (formals.len != th_func_nparams(source_type)
        || ((rest_symbol != nullptr) != (th_func_rest(source_type) != nullptr))) {
        core_list_free(formals);
        return nullptr;
    }

    core_list_t(th_param) parameters = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    bool ok = true;
    for (size_t i = 0; ok && i < formals.len; i++) {
        const th_param *source = th_func_param(source_type, i);
        parse_sym_entry_t *formal = formals.data[i];
        th_mode mode = formal && formal->has_mode ? formal->mode : TH_IN;
        if (!source || !formal || !th_mode_is_valid(mode)) {
            ok = false;
            break;
        }
        core_list_push(parameters, ((th_param){
            .mode = mode, .type = source->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source_type);
    th_param lowered_rest = {0};
    const th_param *rest = nullptr;
    if (source_rest) {
        th_mode mode = rest_symbol && rest_symbol->has_mode
            ? rest_symbol->mode : TH_IN;
        rest = lowered_rest_type(state->types, source_rest, mode,
                                 &lowered_rest) ? &lowered_rest : nullptr;
        if (!rest) ok = false;
    }
    for (size_t i = 0; ok && i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) ok = false;
        else core_list_push(keywords, *keyword);
    }
    th_type *semantic_type = ok
        ? th_func_with_error(state->types, th_func_ret(source_type),
            th_func_error(source_type), parameters.data, parameters.len,
            rest, keywords.data, keywords.len) : nullptr;
    core_list_free(formals);
    core_list_free(parameters);
    core_list_free(keywords);
    return semantic_type;
}

static th_type *
function_semantic_type(parse_grammar_t *grammar, core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !symbol || !symbol->type) return nullptr;
    th_type *body_type = parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.body-function-signature"));
    th_type *contract = symbol->callable_contract
        ? symbol->callable_contract : symbol->type;
    th_type *source_type = th_type_clone(state->types, contract);
    if (body_type && source_type) {
        if (!th_unify_in_place(state->types, body_type, source_type))
            return nullptr;
        source_type = body_type;
    }
    return function_semantic_shape(grammar, unit, symbol, source_type);
}

static th_type *
function_semantic_contract(parse_grammar_t *grammar,
                           core_translation_unit_t *unit,
                           parse_sym_entry_t *symbol)
{
    n00bb_tu_state_t *state = tu_state(unit);
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : nullptr;
    if (!state || !contract) return nullptr;
    return function_semantic_shape(grammar, unit, symbol, contract);
}

static th_type *
trait_method_semantic_shape(parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       parse_sym_entry_t *symbol, th_type *source)
{
    n00bb_tu_state_t *state = tu_state(unit);
    source = th_resolve(source);
    parse_tree_t *signature = symbol && symbol->decl_node
        ? child_nt(grammar, symbol->decl_node, "trait-func-type-signature")
        : nullptr;
    parse_tree_t *parameters_node = signature
        ? child_nt(grammar, signature, "trait-param-decl") : nullptr;
    if (!state || !source || th_kind_of(source) != TH_FUNCTION
        || !parameters_node) return nullptr;
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_symbol = nullptr;
    collect_ir_parameters(grammar, parameters_node, &formals, &rest_symbol);
    if (formals.len != th_func_nparams(source)
        || ((rest_symbol != nullptr) != (th_func_rest(source) != nullptr))) {
        core_list_free(formals);
        return nullptr;
    }
    core_list_t(th_param) parameters = core_list_new(th_param);
    core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
    bool ok = true;
    for (size_t i = 0; ok && i < formals.len; i++) {
        const th_param *input = th_func_param(source, i);
        parse_sym_entry_t *formal = formals.data[i];
        th_mode mode = formal && formal->has_mode ? formal->mode : TH_IN;
        if (!input || !formal || !th_mode_is_valid(mode)) ok = false;
        else core_list_push(parameters, ((th_param){
            .mode = mode, .type = input->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source);
    th_param lowered_rest = {0};
    const th_param *rest = nullptr;
    if (source_rest) {
        th_mode mode = rest_symbol && rest_symbol->has_mode
            ? rest_symbol->mode : TH_IN;
        rest = lowered_rest_type(state->types, source_rest, mode,
                                 &lowered_rest) ? &lowered_rest : nullptr;
        if (!rest) ok = false;
    }
    for (size_t i = 0; ok && i < th_func_nkw(source); i++) {
        const th_kwparam *keyword = th_func_kw(source, i);
        if (!keyword) ok = false;
        else core_list_push(keywords, *keyword);
    }
    th_type *semantic = ok
        ? th_func_with_error(state->types, th_func_ret(source),
            th_func_error(source), parameters.data, parameters.len, rest,
            keywords.data, keywords.len) : nullptr;
    core_list_free(formals);
    core_list_free(parameters);
    core_list_free(keywords);
    return semantic;
}

static th_type *
trait_method_semantic_type(parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *symbol,
                      th_type **callable_contract)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (callable_contract) *callable_contract = nullptr;
    if (!state || !symbol) return nullptr;
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    th_type *source = contract ? th_resolve(contract) : nullptr;
    th_type *semantic = source
        ? trait_method_semantic_shape(grammar, unit, symbol, source) : nullptr;
    if (semantic && callable_contract) *callable_contract = semantic;
    return semantic;
}

static th_type *
interface_function_semantic_type(th_ctx *types, parse_sym_entry_t *symbol)
{
    th_type *contract = symbol && symbol->callable_contract
        ? symbol->callable_contract : symbol ? symbol->type : nullptr;
    th_type *signature = contract ? th_resolve(contract) : nullptr;
    if (!types || !signature || th_kind_of(signature) != TH_FUNCTION)
        return nullptr;
    size_t count = function_interface_parameter_count(symbol);
    size_t expected = th_func_nparams(signature)
        + (th_func_rest(signature) != nullptr);
    if (count != expected) return nullptr;

    core_list_t(th_param) parameters = core_list_new(th_param);
    bool ok = true;
    for (size_t i = 0; ok && i < th_func_nparams(signature); i++) {
        const th_param *source = th_func_param(signature, i);
        uint8_t flags = 0, mode = 0;
        if (!source || !function_interface_parameter(symbol, i, nullptr,
                nullptr, &flags, &mode) || (flags & ~1u)
            || !th_mode_is_valid((th_mode)mode)
            || ((flags & 1u) && !th_mode_is_input_only((th_mode)mode))) {
            ok = false;
            break;
        }
        core_list_push(parameters, ((th_param){
            .mode = mode, .type = source->type,
        }));
    }
    const th_param *source_rest = th_func_rest(signature);
    th_param rest_value = {0};
    const th_param *rest = nullptr;
    if (ok && source_rest) {
        uint8_t flags = 0, mode = 0;
        if (!function_interface_parameter(symbol,
                th_func_nparams(signature), nullptr, nullptr, &flags, &mode)
            || flags != 2u || !th_mode_is_valid((th_mode)mode)) ok = false;
        else {
            rest = lowered_rest_type(types, source_rest, (th_mode)mode,
                                     &rest_value) ? &rest_value : nullptr;
            if (!rest) ok = false;
        }
    }
    th_type *semantic = ok
        ? th_func_with_error(types, th_func_ret(signature),
            th_func_error(signature), parameters.data, parameters.len,
            rest, nullptr, 0) : nullptr;
    core_list_free(parameters);
    return semantic;
}

static bool
ir_initialization_error(core_translation_unit_t *unit, const char *code,
                        const char *message)
{
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_FATAL, core_string_from_cstr(code),
        core_string_from_cstr(message));
    return false;
}

static bool
register_optional_parameter_metadata(parse_grammar_t *grammar,
                                     parse_sym_entry_t *symbol,
                                     ir_func_t *function)
{
    if (!symbol || !function) return false;
    parse_tree_t *signature = child_nt(grammar, symbol->decl_node,
                                       "func-type-signature");
    if (!signature)
        signature = child_nt(grammar, symbol->decl_node,
                             "trait-func-type-signature");
    parse_tree_t *formals = child_nt(grammar, signature, "param-decl");
    if (!formals)
        formals = child_nt(grammar, signature, "trait-param-decl");
    for (size_t i = 0; i < th_func_nparams(function->type); i++) {
        uint8_t flags = 0;
        if (!function_parameter_info(grammar, symbol, formals, i,
                nullptr, nullptr, nullptr, &flags, nullptr)) return false;
        if ((flags & 1u)
            && !ir_func_set_parameter_optional(function, (uint32_t)i, true))
            return false;
    }
    return true;
}

bool
initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                      core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis,
                      uint32_t producer_stage)
{
    core_list_t(core_translation_unit_ptr_t) *units = &analysis->units;
    core_target_desc_t target;
    if (!host_target_profile(&target)) {
        if (units->len)
            ir_initialization_error(units->data[0], "N00BB-L0024",
                "the host is not an ABI v0.1 target");
        return false;
    }
    analysis->target = target;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_tree_t *tree = n00bb_tu_parsed_tree(state);
        if (!tree || !state->semantic_annotation_complete)
            return ir_initialization_error(unit, "N00BB-L0024",
                "translation unit has no annotated tree for Lantern IR");
        state->ir_module = ir_module_new(ir_context,
            unit->module.identifier.data, n00bb_type_profile(), target.triple);
        if (!state->ir_module)
            return ir_initialization_error(unit, "N00BB-L0024",
                "could not allocate the Lantern IR module");
        core_string_t path = core_source_file_path(&unit->source);
        int32_t file_id = ir_file_add(state->ir_module, path.data);
        ir_module_set_span(state->ir_module, ir_span_make(file_id, 0,
            (uint32_t)core_source_file_size(&unit->source), 1, 1, 1, 1));
        ir_module_set_tree(state->ir_module, grammar, tree);
    }

    for (size_t i = 0; i < units->len; i++)
        if (!register_nominal_types(grammar, units->data[i],
                tu_state(units->data[i])->ir_module, false))
            return ir_initialization_error(units->data[i], "N00BB-L0026",
                "could not retain nominal representations in Lantern IR");

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        if (!register_nominal_types(grammar, unit, module, true))
            return ir_initialization_error(unit, "N00BB-L0026",
                "could not retain nominal representations in Lantern IR");
        if (!register_variant_types(grammar, unit, module))
            return ir_initialization_error(unit, "N00BB-L0025",
                "could not register variant definitions in Lantern IR");
        if (!register_record_types(grammar, unit, module, ir_context))
            return ir_initialization_error(unit, "N00BB-L0026",
                "could not register record definitions in Lantern IR");
    }

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        for (size_t j = 0; j < units->len; j++) {
            if (units->data[i] == units->data[j]
                || !n00bb_unit_reaches_import(unit, units->data[j])) continue;
            if (!register_imported_record_layouts(grammar, units->data[j],
                                                   tu_state(unit)->ir_module))
                return ir_initialization_error(unit, "N00BB-L0027",
                    "could not register imported record layout in Lantern IR");
        }
    }

    for (size_t i = 0; i < units->len; i++)
        for (size_t j = 0; j < units->len; j++)
            if (!register_module_globals(grammar, units->data[j],
                    units->data[i], tu_state(units->data[i])->ir_module,
                    ir_context, analysis, link, producer_stage))
                return ir_initialization_error(units->data[i],
                    "N00BB-L0028",
                    "could not register module globals in Lantern IR");
    if (!bind_module_global_identities(ir_context, analysis))
        return ir_initialization_error(units->len ? units->data[0] : nullptr,
            "N00BB-L0028",
            "could not bind module-local global views to their canonical identities");

    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = link_declaration_scopes(
            state, CORE_STRING_STATIC(""));
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type)
                    continue;
                bool definition = function_symbol_at(grammar,
                        symbol->decl_node)
                    || parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.generated-value-method"));
                parse_tree_t *prelude = definition
                    ? child_nt(grammar, symbol->decl_node, "func-prelude")
                    : nullptr;
                bool always_inline = prelude
                    && child_nt(grammar, prelude, "func-inline");
                ir_linkage_t linkage = !definition ? IR_LINK_EXTERN
                    : symbol_visibility(symbol) == TH_ENV_PRIVATE
                        && !core_string_eq(symbol->name,
                                           CORE_STRING_STATIC("__finalize"))
                        ? IR_LINK_INTERNAL : IR_LINK_EXTERNAL;
                th_type *semantic_type = state->interface_module
                    ? symbol->user_data
                        ? interface_function_semantic_type(state->types,
                                                           symbol)
                        : interface_context_hook_symbol(symbol)
                            ? symbol->type
                            : nullptr
                    : function_semantic_type(grammar, unit, symbol);
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                core_string_t declaration_key = declaration
                    ? core_link_declaration_key(declaration)
                    : (core_string_t){0};
                core_string_t overload_group = declaration
                    ? core_link_declaration_overload_group(declaration)
                    : (core_string_t){0};
                bool overloaded = declaration && overload_group.data
                    && core_link_unit_overload_member_count(link,
                        unit->module.identifier, overload_group) > 1;
                const char *semantic_name = symbol->name.data;
                if (state->interface_module) {
                    n00bb_export_origin_t *origin = parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.export-origin"));
                    if (origin) semantic_name = origin->declaration.data;
                }
                if (!state->interface_module && overloaded
                    && declaration_key.data) {
                    const char *separator = memchr(declaration_key.data, ':',
                                                   declaration_key.u8_bytes);
                    if (separator) semantic_name = separator + 1;
                }
                ir_func_t *function = declaration
                    ? core_link_declaration_binding(declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"))
                    : nullptr;
                bool new_function = function == nullptr;
                if (new_function && semantic_type)
                    function = ir_func_add(state->ir_module, semantic_name,
                                           semantic_type, linkage);
                if (always_inline && function
                    && !ir_func_set_always_inline(ir_context, function, true)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit, "N00BB-L0029",
                        "could not attach inline semantics to a function");
                }
                if (new_function && function
                    && !register_optional_parameter_metadata(
                        grammar, symbol, function)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit, "N00BB-L0029",
                        "could not attach optional parameter semantics to a function");
                }
                bool polymorphic_contract = symbol->callable_contract
                    && !th_is_closed(state->types,
                                     symbol->callable_contract);
                th_type *ir_callable_contract =
                    polymorphic_contract && !state->interface_module
                        ? function_semantic_contract(grammar, unit, symbol)
                        : symbol->callable_contract;
                if (new_function && function && polymorphic_contract
                    && (!ir_callable_contract
                        || !ir_func_set_callable_contract(
                            function, ir_callable_contract))) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit, "N00BB-L0029",
                        "could not attach a callable contract to a function declaration");
                }
                if (!function || !declaration
                    || !core_link_declaration_set_binding(link, declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"),
                        producer_stage, function)) {
                    core_list_free(scopes);
                    return ir_initialization_error(unit, "N00BB-L0029",
                        "could not bind function declarations in Lantern IR");
                }
            }
        }
        core_list_free(scopes);
    }
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return ir_initialization_error(unit, "N00BB-L0029",
            "translation unit has no symbol table for trait requirements");
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)
                || !core_string_eq(scope->scope_tag,
                                CORE_STRING_STATIC("trait"))) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION) continue;
                core_buffer_t name = {0};
                core_buffer_printf(&name, "__trait_req_%.*s_%.*s",
                    (int)scope->name.u8_bytes, scope->name.data,
                    (int)symbol->name.u8_bytes, symbol->name.data);
                th_type *contract = nullptr;
                th_type *semantic_type = trait_method_semantic_type(grammar, unit,
                                                           symbol, &contract);
                ir_func_t *function = semantic_type
                    ? ir_func_add(state->ir_module, name.data, semantic_type,
                                  IR_LINK_EXTERN) : nullptr;
                uint64_t contract_arity = 0;
                bool polymorphic_contract = contract
                    && th_binder_arity(contract, &contract_arity)
                    && contract_arity > 0;
                if (function && polymorphic_contract
                    && !ir_func_set_callable_contract(function, contract))
                    function = nullptr;
                if (function && !register_optional_parameter_metadata(
                        grammar, symbol, function)) function = nullptr;
                core_link_declaration_t *declaration =
                    link_declaration_for_symbol(analysis, symbol);
                if (!function || !declaration
                    || !core_link_declaration_set_binding(link, declaration,
                        CORE_STRING_STATIC("n00bb.ir.function"),
                        producer_stage, function)) {
                    core_buffer_release(&name);
                    return ir_initialization_error(unit, "N00BB-L0029",
                        "could not register an abstract trait method in Lantern IR");
                }
                core_buffer_release(&name);
            }
        }
    }
    for (size_t i = 0; i < units->len; i++)
        if (!add_context_hook_functions(grammar, units->data[i], analysis,
                                         link, producer_stage))
            return ir_initialization_error(units->data[i], "N00BB-L0030",
                "could not register context hooks in Lantern IR");
    return true;
}

bool
validate_module_declarations(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             ir_ctx_t *ir_context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->ir_module) return false;
    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return false;
    int32_t file_id = ir_file_count(state->ir_module) - 1;
    ir_lower_context_t span_context = {.file_id = file_id};
    bool valid = true;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
    for (size_t s = 0; valid && s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            parse_tree_t *binding = symbol_binding_node(symbol);
            if (symbol->kind == PARSE_SYM_FUNCTION
                || symbol->kind == PARSE_SYM_MODULE
                || context_symbol(grammar, symbol)
                || (symbol->decl_node
                    && nt_is(grammar, symbol->decl_node, "trait-decl"))
                || (symbol->decl_node
                    && (field_record_declaration(grammar, symbol->decl_node)
                        || nt_is(grammar, symbol->decl_node, "variant-decl")
                        || nt_is(grammar, symbol->decl_node, "enum-stmt")))
                || (symbol->decl_node
                    && nested_module_declaration_owner(grammar,
                                                        symbol->decl_node))
                || !binding)
                continue;
            if (symbol->kind == PARSE_SYM_VARIABLE
                && symbol->type
                && (nt_is(grammar, binding, "sym-decl")
                    || nt_is(grammar, binding, "assign-stmt"))) {
                bool registered = link_ir_global_for_symbol(
                    state->link_analysis, symbol) != nullptr;
                if (registered) continue;
            }
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_PARSED,
                lower_span_for_tree(&span_context, binding),
                "this module-level binding");
            ir_diag_message(diagnostic, diagnostics,
                "module-level variable bindings are not lowered yet");
            valid = false;
            break;
        }
    }
    core_list_free(scopes);
    return valid && !ir_diags_failed(diagnostics);
}

static bool
n00bb_emit_default_param(ir_lowerer_t *lowerer,
                        ir_lower_context_t *context,
                        n00bb_default_param_t *parameter)
{
    if (!lowerer || !context || !parameter) return false;
    if (parameter->emitted) return true;
    if (parameter->lowering) {
        lower_error(context, IR_DIAG_TYPE_ARGS,
            lower_span_for_tree(context, parameter->symbol->decl_node),
            "default parameter dependencies are cyclic");
        return false;
    }
    parameter->lowering = true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower) return false;
    for (size_t i = 0; i < lower->default_params.len; i++) {
        n00bb_default_param_t *dependency = &lower->default_params.data[i];
        if (n00bb_default_references(parameter->expression,
                dependency->symbol)
            && !n00bb_emit_default_param(lowerer, context, dependency))
            return false;
    }
    ir_span_t span = lower_span_for_tree(context, parameter->symbol->decl_node);
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    uint64_t id = context->next_value_id++;
    char provided_name[48], missing_name[48], join_name[48];
    char present_name[48];
    snprintf(provided_name, sizeof(provided_name),
             "__n00bb_default_provided_%llu", (unsigned long long)id);
    snprintf(missing_name, sizeof(missing_name),
             "__n00bb_default_missing_%llu", (unsigned long long)id);
    snprintf(join_name, sizeof(join_name), "__n00bb_default_join_%llu",
             (unsigned long long)id);
    snprintf(present_name, sizeof(present_name),
             "__n00bb_default_present_%llu", (unsigned long long)id);
    ir_block_t *provided = ir_block_add(context->function, provided_name);
    ir_block_t *missing = ir_block_add(context->function, missing_name);
    ir_block_t *join = ir_block_add(context->function, join_name);
    ir_instr_t *present_test = provided && missing && join && bool_type
        ? ir_lower_emit_instruction(context, IR_OP_PARAM_PRESENT, span)
        : nullptr;
    bool present_built = present_test
        && ir_instr_set_dest(present_test, present_name, bool_type)
        && ir_instr_operand_value(present_test, parameter->argument);
    ir_instr_t *branch = present_built
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!present_built || !branch
        || !ir_instr_operand_value(branch, present_test->dest)
        || !ir_instr_operand_block(branch, provided)
        || !ir_instr_operand_block(branch, missing)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not branch on default argument presence");
        return false;
    }
    branch->span = span;

    context->block = provided;
    context->reachable = true;
    ir_value_t *given = parameter->argument;
    if (!given || !n00bb_emit_local_write(context, parameter->local,
                                           given, span)
        || !n00bb_emit_jump(context, join, span)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
                    "could not bind a supplied default argument");
        return false;
    }
    context->block = missing;
    context->reachable = true;
    n00bb_default_param_t *active_default = lower
        ? lower->active_default_param : nullptr;
    if (lower) lower->active_default_param = parameter;
    ir_lower_result_t value = ir_lower_tree(lowerer, context,
        lower ? lower->grammar : nullptr, parameter->expression);
    if (lower) lower->active_default_param = active_default;
    if (value.status == IR_LOWER_FAILED) return false;
    if (value.status == IR_LOWER_VALUE && value.value) {
        ir_value_t *resolved = n00bb_materialize(context,
            parameter->expression, value.value, parameter->local->type, span);
        if (!resolved) return false;
        if (!ir_type_eq(context->module, parameter->local->type,
                        resolved->type)) {
            lower_error(context, IR_DIAG_TYPE_MISMATCH,
                lower_span_for_tree(context, parameter->expression),
                "default parameter value cannot be converted to its declared type");
            return false;
        }
        if (!n00bb_emit_local_write(context, parameter->local, resolved,
                                    span)) {
            lower_error(context, IR_DIAG_INTERNAL, span,
                        "could not store a default parameter value");
            return false;
        }
        if (!n00bb_emit_jump(context, join, span)) return false;
    }
    else if (value.status != IR_LOWER_TERMINATED) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
                    "a default parameter expression did not produce a value");
        return false;
    }
    context->block = join;
    context->reachable = true;
    parameter->lowering = false;
    parameter->emitted = true;
    return true;
}

static bool
n00bb_initialize_default_params(ir_lowerer_t *lowerer,
                                ir_lower_context_t *context,
                                core_list_t(n00bb_default_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++)
        if (!params->data[i].expression
            || !n00bb_emit_default_param(lowerer, context,
                                         &params->data[i])) return false;
    return true;
}

static bool
n00bb_initialize_default_reference(ir_lower_context_t *context,
                                   parse_visit_t *visit, bool entering,
                                   void *state)
{
    (void)state;
    if (!entering || !visit->node) return true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->active_default_param) return true;
    parse_sym_entry_t *symbol = attached_symbol(visit->grammar, lower->unit,
                                                visit->node);
    if (!symbol && visit->pn) symbol = visit->pn->sym;
    for (size_t i = 0; symbol && i < lower->default_params.len; i++) {
        n00bb_default_param_t *parameter =
            &lower->default_params.data[i];
        if (parameter->symbol != symbol) continue;
        if (!parameter->emitted) {
            lower_error(context, IR_DIAG_TYPE_ARGS,
                lower_span_for_tree(context, symbol->decl_node),
                "default parameter dependency was not initialized");
            return false;
        }
        return true;
    }
    return true;
}

static bool
bind_missing_external(ir_lower_context_t *context,
                      parse_sym_entry_t *symbol)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    if (!state || !symbol || !symbol->type) return false;
    if (symbol->kind == PARSE_SYM_FUNCTION) {
        parse_sym_entry_t *function = canonical_symbol(symbol);
        ir_func_t *target = link_ir_function_for_symbol(
            state->link_analysis, function);
        core_translation_unit_t *target_unit = target
            ? unit_for_symbol(lower->grammar, lower->units, function)
            : nullptr;
        ir_module_t *target_module = unit_module(
            target_unit ? tu_state(target_unit) : nullptr);
        ir_value_t *address = target && target_module
            ? ir_func_address(target_module, target) : nullptr;
        return address && ir_lower_bind_value(context, symbol, address);
    }
    if (symbol->kind != PARSE_SYM_VARIABLE
        || context_symbol(lower->grammar, symbol)) return false;
    core_translation_unit_t *owner = unit_for_symbol(lower->grammar,
                                                    lower->units, symbol);
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    parse_tree_t *binding = symbol_binding_node(symbol);
    if (!owner_state || (!owner_state->interface_module && (!binding
        || (!nt_is(lower->grammar, binding, "sym-decl")
            && !nt_is(lower->grammar, binding, "assign-stmt")))))
        return false;
    ir_global_t *global = link_ir_global_for_symbol(
        state->link_analysis, symbol);
    ir_global_t *phase_global = global
        ? ir_global_find(context->module, global->name) : nullptr;
    ir_value_t *value = phase_global
        ? ir_global_address(context->module, phase_global) : nullptr;
    return value && ir_lower_bind_value(context, symbol, value);
}

static bool
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
              void *expression_result_data)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    th_type *function_type = function ? th_resolve(function->type) : nullptr;
    if (!state || !module || !body || !function || !function_type)
        return false;

    ir_diags_t *diagnostics = ir_diags_new_attached(
        ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
    if (!diagnostics) return false;
    size_t collected_rest = rest_parameter && !th_func_rest(function_type);
    if (parameters->len + collected_rest !=
        th_func_nparams(function_type)) {
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "these parameters");
        ir_diag_message(diagnostic, diagnostics,
                        "parameter declarations do not match the callable type");
        return false;
    }

    ir_block_t *entry = ir_block_add(function, "entry");
    if (!entry) {
        return false;
    }
    int32_t file_id = ir_file_count(module) - 1;
    n00bb_lower_context_t lower_data = {
        .grammar = grammar,
        .lowerer = lowerer,
        .ir_context = ir_context,
        .units = units,
        .unit = unit,
        .function_body = body,
        .function_symbol = symbol,
        .loops = core_list_new(n00bb_loop_frame_t),
        .value_blocks = core_list_new(n00bb_value_block_frame_t),
        .tries = core_list_new(n00bb_try_frame_t),
        .withs = core_list_new(n00bb_with_frame_t),
        .parameters_to_copy_out = core_list_new(n00bb_copyout_t),
        .pending_call_copyouts = core_list_new(n00bb_pending_copyouts_t),
        .default_params = core_list_new(n00bb_default_param_t),
        .branches = core_list_new(n00bb_branch_frame_t),
        .switches = core_list_new(n00bb_switch_frame_t),
        .typecases = core_list_new(n00bb_typecase_frame_t),
    };
    ir_lower_context_t context = {
        .module = module,
        .function = function,
        .block = entry,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .user = &lower_data,
        .bind_missing = bind_missing_external,
        .reachable = true,
    };
    bool ok = false;
    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        const th_param *type_param = th_func_param(function_type, i);
        th_mode mode = parameter && parameter->has_mode
            ? parameter->mode : TH_IN;
        if (!parameter || !type_param || !parameter->name.data
            || (parameter->is_optional && !th_mode_is_input_only(mode))
            || !th_mode_is_valid(mode)) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN, "this parameter");
            ir_diag_message(diagnostic, diagnostics,
                "the parameter has an invalid passing mode, or a copy-out "
                "parameter has a default");
            goto cleanup;
        }
        th_type *signature_parameter_type = type_param->type;
        th_type *parameter_type = parameter->type
            ? th_unify_in_place(state->types, parameter->type,
                                signature_parameter_type)
            : signature_parameter_type;
        if (!parameter_type) {
            lower_error(&context, IR_DIAG_TYPE_MISMATCH, IR_SPAN_UNKNOWN,
                "parameter symbol type does not match the callable signature");
            goto cleanup;
        }
        parameter->type = parameter_type;
        if (parameter->decl_node)
            core_tree_node_value(parameter->decl_node).type = parameter_type;
        if (th_mode_is_input_only(mode)) {
            if (ir_func_parameter_is_optional(function, (uint32_t)i)) {
                char local_name[48];
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_default_local_%llu",
                         (unsigned long long)context.next_value_id++);
                ir_value_t *argument = ir_param_add(function,
                    parameter->name.data, parameter_type);
                bool optional_set = argument
                    && ir_param_set_optional(argument, true);
                ir_local_t *local = ir_local_add(function, local_name,
                                                 parameter_type);
                ir_value_t *local_value = local
                    ? ir_local_value(function, local) : nullptr;
                ir_span_t parameter_span = lower_span_for_tree(
                    &context, parameter->decl_node);
                if (!argument || !optional_set || !local
                    || !n00bb_set_reference_access(ir_context, local_value,
                                                   parameter_type)
                    || !n00bb_bind_local(&context, parameter, local,
                                          parameter_span)
                    || !ir_value_set_phase_access(ir_context, local_value,
                        IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_BOTH,
                        IR_PHASE_ACCESS_BOTH)) {
                    lower_error(&context, IR_DIAG_VERIFY_TYPED,
                        lower_span_for_tree(&context, parameter->decl_node),
                        "defaulted parameter has an invalid optional IR binding");
                    goto cleanup;
                }
                if (!n00bb_attach_symbol_array_shape(&context, grammar,
                        parameter, parameter_type, argument, local)) {
                    goto cleanup;
                }
                core_list_push(lower_data.default_params,
                    ((n00bb_default_param_t){
                        .symbol = parameter,
                        .expression = child_nt(grammar, parameter->decl_node,
                                               "expression"),
                        .local = local,
                        .argument = argument,
                    }));
                continue;
            }
            ir_value_t *value = ir_param_add(function, parameter->name.data,
                                             parameter_type);
            if (!value || !n00bb_set_reference_access(ir_context, value,
                                                       parameter_type)) {
                goto cleanup;
            }
            if (!n00bb_attach_symbol_array_shape(&context, grammar,
                    parameter, parameter_type, value, nullptr)) {
                goto cleanup;
            }
            if (parameter->storage_duration == PARSE_SYM_STORAGE_DYNAMIC
                || (parameter->selected_storage_class.data
                    && parameter->selected_storage_class.u8_bytes)) {
                char local_name[48];
                snprintf(local_name, sizeof(local_name),
                         "__n00bb_param_local_%llu",
                         (unsigned long long)context.next_value_id++);
                ir_local_t *local = ir_local_add(function, local_name,
                                                 parameter_type);
                ir_value_t *local_value = local
                    ? ir_local_value(function, local) : nullptr;
                ir_span_t parameter_span = lower_span_for_tree(
                    &context, parameter->decl_node);
                if (!local
                    || !n00bb_set_reference_access(ir_context, local_value,
                                                   parameter_type)
                    || (ir_value_shape(value)
                        && !ir_local_set_shape(local,
                                               ir_value_shape(value)))
                    || !n00bb_bind_local(&context, parameter, local,
                                                 parameter_span)
                    || !n00bb_emit_local_write(&context, local, value,
                                               parameter_span)) {
                    lower_error(&context, IR_DIAG_INTERNAL, parameter_span,
                        "could not place the parameter in selected storage");
                    goto cleanup;
                }
            }
            else if (!ir_lower_bind_value(&context, parameter, value)) {
                goto cleanup;
            }
            continue;
        }

        bool mode_matches = parameter->has_mode
            && (mode == TH_OUT || mode == TH_INOUT);
        ir_value_t *input = mode_matches && mode == TH_INOUT
            ? ir_param_add(function, parameter->name.data, parameter_type)
            : nullptr;
        char local_name[48];
        snprintf(local_name, sizeof(local_name), "__n00bb_param_%llu",
                 (unsigned long long)context.next_value_id++);
        ir_local_t *local = mode_matches
            ? ir_local_add(function, local_name, parameter_type) : nullptr;
        ir_value_t *local_value = local ? ir_local_value(function, local)
                                        : nullptr;
        ir_span_t parameter_span = lower_span_for_tree(
            &context, parameter->decl_node);
        if (!mode_matches || (mode == TH_INOUT && !input) || !local
            || (input && !n00bb_set_reference_access(ir_context, input,
                                                     parameter_type))
            || !n00bb_set_reference_access(ir_context, local_value,
                                           parameter_type)
            || !n00bb_attach_symbol_array_shape(&context, grammar,
                parameter, parameter_type, input, local)
            || !n00bb_bind_local(&context, parameter, local,
                                  parameter_span)) {
            lower_error(&context, IR_DIAG_VERIFY_TYPED, IR_SPAN_UNKNOWN,
                "out and modify parameters need matching passing metadata and a typed local copy");
            goto cleanup;
        }
        if (mode == TH_INOUT) {
            if (!n00bb_emit_local_write(&context, local, input,
                                        parameter_span)) {
                lower_error(&context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(&context, parameter->decl_node),
                    "could not copy a modify parameter into its local storage");
                goto cleanup;
            }
        }
        size_t output_index = 0;
        for (size_t previous = 0; previous < i; previous++) {
            parse_sym_entry_t *prior = parameters->data[previous];
            if (prior && prior->has_mode
                && th_mode_produces_output(prior->mode))
                output_index++;
        }
        core_list_push(lower_data.parameters_to_copy_out,
            ((n00bb_copyout_t){.symbol = parameter,
                               .local = local,
                               .output_index = output_index,
                               .required_shape = parse_sym_binding(parameter,
                                   CORE_STRING_STATIC("ir.array-shape"))}));
    }

    if (rest_parameter) {
        const th_param *rest_contract = th_func_rest(function->type);
        th_type *rest_array_type = th_resolve(rest_parameter->type);
        ir_value_t *rest = nullptr;
        ir_value_t *rest_binding = nullptr;
        if (rest_contract) {
            rest = ir_restparam_add(function, rest_parameter->name.data,
                                    rest_contract->type);
            char collected_name[48];
            snprintf(collected_name, sizeof(collected_name),
                     "__n00bb_rest_collect_%llu",
                     (unsigned long long)context.next_value_id++);
            ir_instr_t *collect = rest && rest_array_type
                && th_kind_of(rest_array_type) == TH_SEQUENCE
                    ? ir_instr_add(context.block, IR_OP_REST_COLLECT) : nullptr;
            if (collect && ir_instr_set_dest(collect, collected_name,
                    rest_parameter->type)
                && ir_instr_operand_value(collect, rest)) {
                collect->span = lower_span_for_tree(&context,
                    rest_parameter->decl_node);
                rest_binding = collect->dest;
            }
        }
        else {
            rest = ir_param_add(function, rest_parameter->name.data,
                                rest_parameter->type);
            rest_binding = rest;
        }
        bool collected_rest = rest && rest_binding
            && ir_lower_bind_value(&context, rest_parameter, rest_binding)
            && n00bb_attach_symbol_array_shape(&context, grammar,
                rest_parameter, rest_parameter->type, rest_binding, nullptr);
        if (!collected_rest) {
            lower_error(&context, IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(&context, rest_parameter->decl_node),
                "variadic parameter does not have its collected array type");
            goto cleanup;
        }
    }

    if (!n00bb_initialize_default_params(lowerer, &context,
                                          &lower_data.default_params)) {
        goto cleanup;
    }
    for (size_t i = 0; i < lower_data.default_params.len; i++) {
        n00bb_default_param_t *parameter =
            &lower_data.default_params.data[i];
        char local_name[48];
        snprintf(local_name, sizeof(local_name),
                 "__n00bb_default_bound_%llu",
                 (unsigned long long)context.next_value_id++);
        ir_local_t *bound = ir_local_add(function, local_name,
                                         parameter->local->type);
        ir_value_t *bound_value = bound
            ? ir_local_value(function, bound) : nullptr;
        ir_span_t span = lower_span_for_tree(&context,
                                             parameter->symbol->decl_node);
        ir_value_t *resolved = n00bb_emit_local_read(&context,
                                                     parameter->local, span);
        if (!bound || !bound_value
            || !n00bb_set_reference_access(ir_context, bound_value,
                                           parameter->local->type)
            || !ir_lower_rebind_local(&context, parameter->symbol, bound)
            || !n00bb_attach_symbol_array_shape(&context, grammar,
                parameter->symbol, parameter->local->type, nullptr, bound)
            || !resolved
            || !n00bb_emit_local_write(&context, bound, resolved, span)) {
            lower_error(&context, IR_DIAG_INTERNAL, span,
                "could not bind the initialized default parameter value");
            goto cleanup;
        }
    }

    for (size_t i = 0; i < parameters->len; i++) {
        parse_sym_entry_t *parameter = parameters->data[i];
        ir_shape_t *shape = parse_sym_binding(parameter,
            CORE_STRING_STATIC("ir.array-shape"));
        bool runtime_bound = false;
        for (uint32_t d = 0; shape && d < ir_shape_rank(shape); d++) {
            ir_shape_extent_t extent;
            if (!ir_shape_extent(shape, d, &extent)) goto cleanup;
            runtime_bound |= extent.kind == IR_SHAPE_EXTENT_RUNTIME;
        }
        if (!runtime_bound) continue;
        ir_span_t span = lower_span_for_tree(&context,
                                             parameter->decl_node);
        ir_shape_t *bound_shape = ir_shape_suffix(ir_context, shape, 0);
        if (!bound_shape || !n00bb_lower_signature_array_shape(lowerer,
                &context, grammar, bound_shape)) {
            lower_error(&context, IR_DIAG_INTERNAL, span,
                "could not lower a parameter's runtime array dimensions");
            goto cleanup;
        }
        th_mode mode = parameter->has_mode ? parameter->mode : TH_IN;
        ir_local_t *local = ir_lower_bound_local(&context, parameter);
        ir_value_t *parameter_value = ir_lower_bound_value(&context,
                                                           parameter);
        if ((local && !ir_local_set_shape(local, bound_shape))
            || (parameter_value
                && !ir_value_set_shape(parameter_value, bound_shape)))
            goto cleanup;
        for (size_t c = 0; c < lower_data.parameters_to_copy_out.len; c++)
            if (lower_data.parameters_to_copy_out.data[c].symbol == parameter)
                lower_data.parameters_to_copy_out.data[c].required_shape =
                    bound_shape;
        if (mode == TH_OUT) continue;
        ir_value_t *value = local
            ? n00bb_emit_local_read(&context, local, span)
            : parameter_value;
        if (!value || !n00bb_emit_sequence_shape_check_impl(&context,
                value, bound_shape, span, true)) goto cleanup;
    }
    ir_shape_t *return_shape = symbol ? parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.return-shape")) : nullptr;
    if (return_shape) {
        lower_data.bound_return_shape = ir_shape_suffix(ir_context,
                                                        return_shape, 0);
    }
    if (return_shape && (!lower_data.bound_return_shape
        || !n00bb_lower_signature_array_shape(lowerer, &context,
            grammar, lower_data.bound_return_shape))) {
        lower_error(&context, IR_DIAG_INTERNAL,
            lower_span_for_tree(&context, symbol->decl_node),
            "could not lower the return's runtime array dimensions");
        goto cleanup;
    }

    ir_lower_result_t lowered = {IR_LOWER_NO_VALUE, nullptr};
    if (roots) {
        for (size_t i = 0; i < roots->len; i++) {
            if (!context.reachable) {
                lower_error(&context, IR_DIAG_VERIFY_PARSED,
                    lower_span_for_tree(&context, roots->data[i]),
                    "module initializer control flow cannot terminate before later top-level statements");
                lowered.status = IR_LOWER_FAILED;
                break;
            }
            lower_data.function_body = nt_is(grammar, roots->data[i], "body")
                ? roots->data[i] : body;
            lowered = ir_lower_tree(lowerer, &context, grammar,
                                    roots->data[i]);
            if (lowered.status == IR_LOWER_FAILED) break;
        }
    }
    else lowered = ir_lower_tree(lowerer, &context, grammar, body);
    if (lowered.status == IR_LOWER_FAILED || ir_diags_failed(diagnostics))
        goto cleanup;
    if (expression_body) {
        if (lowered.status != IR_LOWER_VALUE || !lowered.value) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED,
                lower_span_for_tree(&context, body),
                "this context default expression");
            ir_diag_message(diagnostic, diagnostics,
                "the default expression does not produce a value on every successful path");
            goto cleanup;
        }
        if (expression_result_hook
            && !expression_result_hook(&context, &lowered.value,
                                       expression_result_data))
            goto cleanup;
        if (context.reachable) {
            if (!n00bb_emit_success_return(&context, lowered.value,
                    &lower_data.parameters_to_copy_out,
                    lower_span_for_tree(&context, body))) goto cleanup;
        }
    }
    else if (context.reachable) {
        if (!th_is_nil(th_func_ret(function_type))) {
            ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                IR_DIAG_VERIFY_TYPED, lower_span_for_tree(&context, body),
                "this function body");
            char message[256];
            snprintf(message, sizeof(message),
                     "function '%.*s' can reach its end without returning a value",
                     symbol ? (int)symbol->name.u8_bytes : 0,
                     symbol ? symbol->name.data : "");
            ir_diag_message(diagnostic, diagnostics, message);
            goto cleanup;
        }
        if (!n00bb_emit_with_success(&context, body)) goto cleanup;
        if (!n00bb_emit_success_return(&context, nullptr,
                &lower_data.parameters_to_copy_out, ir_module_span(module)))
            goto cleanup;
    }
    ok = !ir_diags_failed(diagnostics);

cleanup:
    for (size_t i = 0; i < lower_data.branches.len; i++)
        core_list_free(lower_data.branches.data[i].arms);
    core_list_free(lower_data.branches);
    core_list_free(lower_data.loops);
    for (size_t i = 0; i < lower_data.switches.len; i++) {
        for (size_t c = 0; c < lower_data.switches.data[i].cases.len; c++)
            core_list_free(lower_data.switches.data[i].cases.data[c].patterns);
        core_list_free(lower_data.switches.data[i].cases);
    }
    core_list_free(lower_data.switches);
    for (size_t i = 0; i < lower_data.typecases.len; i++)
        core_list_free(lower_data.typecases.data[i].arms);
    core_list_free(lower_data.typecases);
    core_list_free(lower_data.value_blocks);
    core_list_free(lower_data.tries);
    for (size_t w = 0; w < lower_data.withs.len; w++) {
        for (size_t i = 0; i < lower_data.withs.data[w].items.len; i++)
            core_list_free(lower_data.withs.data[w].items.data[i].captures);
        core_list_free(lower_data.withs.data[w].items);
    }
    core_list_free(lower_data.withs);
    for (size_t i = 0; i < lower_data.pending_call_copyouts.len; i++)
        core_list_free(lower_data.pending_call_copyouts.data[i].items);
    core_list_free(lower_data.pending_call_copyouts);
    core_list_free(lower_data.default_params);
    for (size_t i = 0; i < lower_data.tuple_parts.len; i++) {
        core_list_free(lower_data.tuple_parts.data[i].expressions);
        core_list_free(lower_data.tuple_parts.data[i].values);
    }
    core_list_free(lower_data.tuple_parts);
    core_list_free(lower_data.parameters_to_copy_out);
    ir_lower_context_release(&context);
    return ok;
}

static bool
lower_function_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                    core_translation_unit_t *unit,
                    parse_sym_entry_t *symbol,
                    ir_ctx_t *ir_context,
                    core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    parse_tree_t *declaration = symbol ? symbol->decl_node : nullptr;
    parse_tree_t *body = child_nt(grammar, declaration, "body");
    ir_func_t *function = state && module && symbol
        ? link_ir_function_for_symbol(state->link_analysis, symbol)
        : nullptr;
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    parse_tree_t *signature = child_nt(grammar, declaration,
                                       "func-type-signature");
    collect_ir_parameters(grammar, signature, &parameters, &rest);
    bool ok = lower_ir_body(lowerer, grammar, unit, symbol, body,
        function, &parameters, nullptr, rest, false, ir_context, units,
        nullptr, nullptr);
    core_list_free(parameters);
    return ok;
}

static parse_tree_t *
n00bb_region_child(parse_tree_t *node, parse_tree_t *region)
{
    while (node && core_tree_node_value(node).parent != region)
        node = core_tree_node_value(node).parent;
    return node;
}

static bool
n00bb_region_includes(parse_tree_t *node, parse_tree_t *region,
                      parse_tree_t *after)
{
    if (!node_is_within(node, region) || node == region) return false;
    if (!after) return true;
    parse_tree_t *child = n00bb_region_child(node, region);
    parse_tree_t *guard = n00bb_region_child(after, region);
    for (size_t i = 0; i < core_tree_num_children(region); i++) {
        parse_tree_t *at = core_tree_child(region, i);
        if (at == guard) return child != guard;
        if (at == child) return false;
    }
    return false;
}

static bool
n00bb_option_path_stable(n00bb_lower_context_t *lower,
                         parse_grammar_t *grammar, parse_tree_t *receiver,
                         const n00bb_option_path_t *path)
{
    n00bb_tu_state_t *state = tu_state(lower->unit);
    for (parse_tree_t *access = member_access_node(grammar, receiver);
         access; access = member_access_node(grammar,
                              core_tree_child(access, 0))) {
        th_type *base = th_resolve(core_tree_node_value(
            core_tree_child(access, 0)).type);
        th_type *definition = n00bb_reference_definition(state->types, base);
        if (!base || th_kind_of(definition ? definition : base)
                         == TH_INDIRECTION)
            return false;
    }
    parse_sym_entry_t *root = path->root;
    bool writable_param = root->kind == PARSE_SYM_PARAM && root->has_mode
        && th_mode_produces_output(root->mode);
    if ((root->kind == PARSE_SYM_PARAM && !writable_param)
        || (root->kind == PARSE_SYM_VARIABLE
            && root->mutability == PARSE_SYM_IMMUTABLE)) return true;
    if (root->kind == PARSE_SYM_VARIABLE
        && root->storage_duration != PARSE_SYM_STORAGE_AUTOMATIC) return false;
    for (size_t i = 0; i < root->use_count; i++) {
        parse_tree_t *use = root->use_nodes[i];
        if (n00bb_region_includes(use, path->region, path->after)
            && n00bb_use_writes_root(lower->unit, grammar, use, path->region))
            return false;
    }
    return true;
}

static bool
n00bb_option_path_proven(n00bb_lower_context_t *lower,
                         parse_grammar_t *grammar, parse_tree_t *receiver)
{
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    parse_sym_entry_t *root = nullptr;
    core_buffer_t name = {0};
    if (!state || !receiver
        || !n00bb_member_path(grammar, lower->unit, receiver, &root, &name)) {
        core_buffer_release(&name);
        return false;
    }
    core_string_t spelling = core_string_borrow(name.data, name.byte_len);
    bool proven = false;
    for (size_t i = 0; !proven && i < state->option_paths.len; i++) {
        const n00bb_option_path_t *path = &state->option_paths.data[i];
        proven = path->root == root && core_string_eq(path->path, spelling)
            && n00bb_region_includes(receiver, path->region, path->after)
            && n00bb_option_path_stable(lower, grammar, receiver, path);
    }
    core_buffer_release(&name);
    return proven;
}

static bool
n00bb_assert_has_known_side_effect(n00bb_lower_context_t *lower,
                                   parse_tree_t *node)
{
    n00bb_tu_state_t *state = tu_state(lower->unit);
    core_link_unit_t *link = state ? state->link : nullptr;
    if (!link) return false;
    size_t cursor = 0;
    core_link_dependency_view_t call;
    th_type *signature = nullptr;
    while (n00bb_next_call_within(lower->unit, node, &cursor, &call,
                                  &signature))
        if (n00bb_signature_writes_arguments(signature)
            || n00bb_callee_writes_globals(link, call.to)) return true;
    return false;
}

static bool
n00bb_lower_assert_statement(ir_lower_context_t *context,
                             parse_visit_t *visit, bool entering,
                             void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *node = visit->node;
    ir_span_t span = lower_span_for_tree(context, node);
    bool debug_only = core_string_eq(first_token_text(node),
                                     CORE_STRING_STATIC("assert"));
    if (entering || !context->reachable) return true;
    if (debug_only && n00bb_assert_has_known_side_effect(lower, node)) {
        lower_error(context, IR_DIAG_VERIFY_TYPED, span,
            "assert expression has a side effect");
        return false;
    }

    parse_tree_t *expression = child_nt(visit->grammar, node, "expression");
    ir_value_t *condition = expression
        ? ir_lower_node_value(context, expression) : nullptr;
    th_type *bool_type = n00bb_ir_builtin_type(context->module, "bool");
    if (!condition || !bool_type
        || !ir_type_eq(context->module, condition->type, bool_type)) {
        lower_error(context, IR_DIAG_TYPE_MISMATCH, span,
            "assert and require conditions must be bool");
        return false;
    }
    ir_instr_t *constant = condition->def;
    if (constant && constant->op == IR_OP_CONST
        && constant->n_operands == 1
        && constant->operands[0].kind == IR_OPD_CONST
        && !constant->operands[0].lit.is_float
        && !constant->operands[0].lit.i.mag) {
        ir_instr_t *trap = ir_instr_add(context->block, IR_OP_TRAP);
        if (!trap) return false;
        trap->span = span;
        context->reachable = false;
        return true;
    }
    ir_block_t *join = n00bb_branch_block(context, lower, "require_join");
    ir_block_t *failure = n00bb_branch_block(context, lower,
                                              "condition_failed");
    ir_instr_t *branch = ir_instr_add(context->block, IR_OP_BRANCH);
    if (!join || !failure || !branch
        || !ir_instr_operand_value(branch, condition)
        || !ir_instr_operand_block(branch, join)
        || !ir_instr_operand_block(branch, failure)) {
        lower_error(context, IR_DIAG_INTERNAL, span,
            "could not lower assertion condition");
        return false;
    }
    branch->span = span;
    ir_instr_t *trap = ir_instr_add(failure, IR_OP_TRAP);
    if (!trap) return false;
    trap->span = span;
    context->block = join;
    context->reachable = true;
    return true;
}

static bool
lower_context_hook(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                   core_translation_unit_t *unit,
                   parse_sym_entry_t *context_symbol,
                   parse_tree_t *hook, ir_ctx_t *ir_context,
                   core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    const char *role = context_hook_binding_role(grammar, hook);
    core_link_declaration_t *declaration = state
        ? link_declaration_for_symbol(state->link_analysis, context_symbol)
        : nullptr;
    ir_func_t *function = declaration && role
        ? core_link_declaration_binding(declaration,
                                        core_string_from_cstr(role))
        : nullptr;
    parse_tree_t *body = child_nt(grammar, hook, "body");
    th_type *type = core_tree_node_value(hook).type;
    if (!function || !body || !type) return false;

    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_tree_t *context_params = child_nt(grammar, context_symbol->decl_node,
                                             "param-decl");
    parse_tree_t *hook_params = child_nt(grammar, hook,
                                          "context-hook-params");
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, context_params, &parameters, &rest);
    collect_ir_parameters(grammar, hook_params, &parameters, &rest);
    parse_sym_entry_t function_symbol = {
        .name = context_symbol->name,
        .kind = PARSE_SYM_FUNCTION,
        .type = type,
        .decl_node = hook,
    };
    bool ok = lower_ir_body(lowerer, grammar, unit,
        &function_symbol, body, function, &parameters, nullptr, rest,
        false, ir_context, units, nullptr, nullptr);
    core_list_free(parameters);
    return ok;
}

static bool
lower_context_default(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                      core_translation_unit_t *unit,
                      parse_sym_entry_t *context_symbol, size_t parameter_index,
                      ir_ctx_t *ir_context,
                      core_list_t(core_translation_unit_ptr_t) *units)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_link_declaration_t *declaration = state
        ? link_declaration_for_symbol(state->link_analysis, context_symbol)
        : nullptr;
    char role[64], name[256];
    if (!state || !declaration
        || !context_default_role(parameter_index, role, sizeof(role))
        || !context_default_name(context_symbol, parameter_index,
                                 name, sizeof(name))) return false;
    ir_func_t *function = core_link_declaration_binding(declaration,
                                                        core_string_from_cstr(role));
    parse_tree_t *param_decl = child_nt(grammar, context_symbol->decl_node,
                                         "param-decl");
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest = nullptr;
    collect_ir_parameters(grammar, param_decl, &parameters, &rest);
    if (rest) core_list_push(parameters, rest);
    parse_sym_entry_t *target = parameter_index < parameters.len
        ? parameters.data[parameter_index] : nullptr;
    parse_tree_t *expression = target
        ? child_nt(grammar, target->decl_node, "expression") : nullptr;
    core_list_t(parse_sym_entry_ptr_t) dependencies =
        core_list_new(parse_sym_entry_ptr_t);
    context_default_dependencies(expression, &parameters,
                                 &dependencies);
    if (!function || !target || !expression) {
        core_list_free(dependencies);
        core_list_free(parameters);
        return false;
    }
    parse_sym_entry_t function_symbol = {
        .name = core_string_borrow(name, strlen(name)),
        .kind = PARSE_SYM_FUNCTION,
        .type = function->type,
        .decl_node = target->decl_node,
    };
    bool ok = lower_ir_body(lowerer, grammar, unit, &function_symbol,
        expression, function, &dependencies, nullptr, nullptr,
        true, ir_context, units, nullptr, nullptr);
    core_list_free(dependencies);
    core_list_free(parameters);
    return ok;
}


static void
collect_dynamic_sym_declarations(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state, parse_tree_t *node,
                                 core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (nt_is(grammar, node, "sym-decl")) {
        if (!child_nt(grammar, node, "expression") || !state
            || !state->ir_module) return;
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        ir_global_t *global = symbol
            ? link_ir_global_for_symbol(state->link_analysis, symbol)
            : nullptr;
        if (global && !global->has_static_initializer)
            core_list_push(*roots, node);
        return;
    }
    if (!nt_is(grammar, node, "sym-decl-list")) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (nt_is(grammar, child, "sym-decl")
            || nt_is(grammar, child, "sym-decl-list"))
            collect_dynamic_sym_declarations(grammar, state, child, roots);
    }
}

static void
collect_dynamic_global_initializers(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *node,
                                    core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (!nt_is(grammar, node, "variable-decl")) return;
    parse_tree_t *list = child_nt(grammar, node, "sym-decl-list");
    collect_dynamic_sym_declarations(grammar, tu_state(unit), list, roots);
}

static bool n00bb_root_without_start(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit);

static bool
script_build_time(parse_grammar_t *grammar, core_translation_unit_t *unit)
{
    return n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME
        && n00bb_root_without_start(grammar, unit);
}

static bool
n00bb_root_without_start(parse_grammar_t *grammar,
                         core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->link
        || unit != core_link_unit_root(state->link, 0)) return false;
    for (size_t i = 0; i < state->module_top_level_statements.len; i++) {
        parse_tree_t *statement = state->module_top_level_statements.data[i];
        parse_tree_t *name = nt_is(grammar, statement, "func-def")
            && core_tree_num_children(statement) > 1
            ? core_tree_child(statement, 1) : nullptr;
        if (name && core_tree_is_leaf(name)
            && core_string_eq(parse_tree_leaf_text(name),
                              CORE_STRING_STATIC(N00BB_START_NAME)))
            return false;
    }
    return true;
}

static bool
collect_one_module_initializer_root(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *statement, bool consts_only,
                                    core_list_t(n00bb_tree_ptr_t) *roots)
{
    if (!statement || module_initializer_declaration(grammar, statement))
        return true;
    if (nt_is(grammar, statement, "variable-decl")) {
        parse_tree_t *prefix = child_nt(grammar, statement,
                                        "declaration-prefix");
        if (child_nt(grammar, prefix, "storage-class")) return true;
        bool is_const = core_string_eq(first_token_text(child_nt(grammar,
            prefix, "binding-kind")), CORE_STRING_STATIC("const"));
        if ((is_const && n00bb_compile_phase() != PARSE_SYM_PHASE_COMPTIME)
            || (!is_const && consts_only)) return true;
        collect_dynamic_global_initializers(grammar, unit, statement, roots);
        return true;
    }
    if (!consts_only) core_list_push(*roots, statement);
    return true;
}

static bool
collect_module_initializer_roots(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 bool consts_only,
                                 core_list_t(n00bb_tree_ptr_t) *roots)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return false;
    for (size_t i = 0; i < state->module_top_level_statements.len; i++)
        if (!collect_one_module_initializer_root(grammar, unit,
                state->module_top_level_statements.data[i], consts_only,
                roots))
            return false;
    return true;
}

static ir_func_t *
add_module_initializer(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                       const char *name, bool initializer, uint64_t order)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = unit_module(state);
    if (!module) return nullptr;
    if (ir_func_find(module, name)) {
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_VERIFY_PARSED, ir_module_span(module),
            "this generated initializer");
        ir_diag_message(diagnostic, diagnostics,
            "a module declaration uses a reserved initializer name");
        return nullptr;
    }
    th_type *nil_type = n00bb_ir_builtin_type(module, "nil");
    th_type *type = nil_type
        ? th_func_with_error(state->types, nil_type, nil_type,
                             nullptr, 0, nullptr, nullptr, 0)
        : nullptr;
    ir_func_t *function = type
        ? ir_func_add(module, name, type, state->interface_module
            ? IR_LINK_EXTERN : IR_LINK_INTERNAL) : nullptr;
    bool annotated = function
        && (!initializer
            || ir_func_set_initializer(ir_context, function, true))
        && ir_annot_set(ir_context, &function->annots,
            (ir_annot_t){.key = "n00bb.initializer_order",
                         .kind = IR_ANNOT_INT,
                         .i = {.mag = order}});
    return annotated ? function : nullptr;
}

static parse_sym_entry_t *
root_global_read(parse_grammar_t *grammar, core_translation_unit_t *unit,
                 parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    parse_sym_entry_t *symbol = resolution.kind
        == PARSE_NODE_RESOLUTION_SYMBOL ? resolution.target.symbol : nullptr;
    if (symbol && symbol->kind == PARSE_SYM_VARIABLE
        && symbol->mutability != PARSE_SYM_CONST
        && symbol->storage_duration == PARSE_SYM_STORAGE_MODULE
        && symbol->scope == module_scope(tu_state(unit))) return symbol;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_sym_entry_t *found = root_global_read(grammar, unit,
            core_tree_child(node, i));
        if (found) return found;
    }
    return nullptr;
}

static bool
validate_script_root(parse_grammar_t *grammar, core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!n00bb_root_without_start(grammar, unit)) return true;
    if (state->build_time_use.has_location) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0028"),
            CORE_STRING_STATIC(
                "comptime() and comptime_start() need 'func start() -> nil' in the root module"),
            state->build_time_use);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < state->module_top_level_statements.len; i++) {
        parse_tree_t *statement = state->module_top_level_statements.data[i];
        parse_tree_t *prefix = nt_is(grammar, statement, "variable-decl")
            ? child_nt(grammar, statement, "declaration-prefix") : nullptr;
        if (!prefix || !core_string_eq(first_token_text(child_nt(grammar,
                prefix, "binding-kind")), CORE_STRING_STATIC("const")))
            continue;
        parse_sym_entry_t *read = root_global_read(grammar, unit,
                                                   statement);
        if (!read) continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "const initializer reads '%.*s', which is set only at run time in a root module without 'func start() -> nil'",
            (int)read->name.u8_bytes, read->name.data);
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0029"),
            core_string_borrow(message.data, message.byte_len),
            link_source_location(statement));
        core_buffer_release(&message);
        ok = false;
    }
    return ok;
}

static bool
lower_initializer_body(ir_lowerer_t *lowerer, parse_grammar_t *grammar,
                       core_translation_unit_t *unit,
                       core_list_t(core_translation_unit_ptr_t) *units,
                       ir_ctx_t *ir_context, ir_func_t *function,
                       const char *name, bool consts_only)
{
    parse_tree_t *tree = n00bb_tu_parsed_tree(tu_state(unit));
    core_list_t(n00bb_tree_ptr_t) roots = core_list_new(n00bb_tree_ptr_t);
    bool ok = tree && collect_module_initializer_roots(grammar, unit,
                                                       consts_only, &roots);
    parse_sym_entry_t symbol = {
        .name = core_string_from_cstr(name),
        .kind = PARSE_SYM_FUNCTION,
        .type = function->type,
    };
    core_list_t(parse_sym_entry_ptr_t) parameters =
        core_list_new(parse_sym_entry_ptr_t);
    ok = ok && lower_ir_body(lowerer, grammar, unit, &symbol,
        tree, function, &parameters, &roots, nullptr,
        false, ir_context, units, nullptr, nullptr);
    core_list_free(parameters);
    core_list_free(roots);
    return ok;
}

static bool
lower_module_initializer(ir_lowerer_t *lowerer,
                         parse_grammar_t *grammar,
                         core_translation_unit_t *unit,
                         core_list_t(core_translation_unit_ptr_t) *units,
                         ir_ctx_t *ir_context, uint64_t order)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!n00bb_tu_parsed_tree(state)) return false;
    ir_func_t *function = add_module_initializer(ir_context, unit,
        N00BB_MODULE_INIT_NAME, true, order);
    if (!function) return false;
    if (state->interface_module) return true;
    if (!validate_script_root(grammar, unit)) return false;
    bool script = script_build_time(grammar, unit);
    if (!lower_initializer_body(lowerer, grammar, unit, units, ir_context,
            function, N00BB_MODULE_INIT_NAME, script)) return false;
    if (!script) return true;
    ir_func_t *literals = add_module_initializer(ir_context, unit,
        N00BB_SCRIPT_BODY_NAME, false, order);
    return literals && lower_initializer_body(lowerer, grammar, unit, units,
        ir_context, literals, N00BB_SCRIPT_BODY_NAME, false);
}

static bool
literal_constant_reads(n00bb_tu_state_t *state, ir_func_t *function,
                       core_list_t(n00bb_ir_func_ptr_t) *visited,
                       core_list_t(size_t) *reads)
{
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == function) return true;
    core_list_push(*visited, function);
    for (size_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (size_t n = 0; block && n < block->n_instrs; n++) {
            ir_instr_t *instruction = block->instrs[n];
            for (size_t o = 0; instruction && o < instruction->n_operands;
                 o++) {
                if (instruction->operands[o].kind != IR_OPD_VALUE) continue;
                ir_value_t *value = instruction->operands[o].value;
                if (!value) continue;
                if (value->kind == IR_VAL_FUNC && value->owner
                    && !literal_constant_reads(state, value->owner, visited,
                                               reads)) return false;
                if (value->kind != IR_VAL_GLOBAL) continue;
                for (size_t c = 0; c < state->literal_constants.len; c++)
                    if (state->literal_constants.data[c].global
                        && state->literal_constants.data[c].global
                            == value->global)
                        core_list_push(*reads, c);
            }
        }
    }
    return true;
}

static bool
literal_constant_order(core_translation_unit_t *unit, size_t index,
                       uint8_t *marks, core_list_t(size_t) *order)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (marks[index] == 2) return true;
    n00bb_literal_constant_t *constant = &state->literal_constants.data[index];
    if (marks[index] == 1) {
        core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0031"),
            CORE_STRING_STATIC(
                "a literal handler depends on its own literal's result"));
        return false;
    }
    marks[index] = 1;
    core_list_t(n00bb_ir_func_ptr_t) visited =
        core_list_new(n00bb_ir_func_ptr_t);
    core_list_t(size_t) reads = core_list_new(size_t);
    bool ok = literal_constant_reads(state, constant->producer, &visited,
                                     &reads);
    for (size_t i = 0; ok && i < reads.len; i++)
        ok = literal_constant_order(unit, reads.data[i], marks, order);
    core_list_free(visited);
    core_list_free(reads);
    if (!ok) return false;
    marks[index] = 2;
    core_list_push(*order, index);
    return true;
}

static bool
lower_literal_initializer(ir_ctx_t *ir_context, core_translation_unit_t *unit,
                          uint64_t order)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state->literal_constants.len
        || n00bb_compile_phase() != PARSE_SYM_PHASE_COMPTIME) return true;
    uint8_t *marks = core_alloc_array(uint8_t, state->literal_constants.len);
    core_list_t(size_t) sequence = core_list_new(size_t);
    bool ordered = marks != nullptr;
    for (size_t i = 0; ordered && i < state->literal_constants.len; i++)
        ordered = literal_constant_order(unit, i, marks, &sequence);
    core_free(marks);
    if (!ordered) {
        core_list_free(sequence);
        return false;
    }
    ir_func_t *function = add_module_initializer(ir_context, unit,
        N00BB_LITERAL_INIT_NAME, false, order);
    ir_block_t *block = function ? ir_block_add(function, "entry") : nullptr;
    bool ok = block != nullptr;
    for (size_t s = 0; ok && s < sequence.len; s++) {
        size_t i = sequence.data[s];
        n00bb_literal_constant_t *constant = &state->literal_constants.data[i];
        ir_instr_t *call = ir_instr_add(block, IR_OP_CALL);
        th_type *result = th_func_ret(constant->producer->type);
        char name[48];
        snprintf(name, sizeof(name), "__n00bb_literal_%zu", i);
        ok = call
            && ir_instr_operand_value(call,
                ir_func_address(state->ir_module, constant->producer))
            && ir_instr_set_call_signature(call, constant->producer->type)
            && (!constant->global || ir_instr_set_dest(call, name, result));
        if (!ok || !constant->global) continue;
        ir_instr_t *store = ir_instr_add(block, IR_OP_GLOBAL_STORE);
        ok = store
            && ir_instr_operand_value(store,
                ir_global_address(state->ir_module, constant->global))
            && ir_instr_operand_value(store, call->dest);
    }
    core_list_free(sequence);
    ir_instr_t *ret = ok ? ir_instr_add(block, IR_OP_RETURN) : nullptr;
    return ret != nullptr;
}

typedef struct {
    ir_func_t *function;
    uint32_t phase;
    uint32_t id;
} n00bb_exec_graph_function_t;
core_list_decl(n00bb_exec_graph_function_t);

static bool
exec_graph_function_id(ir_exec_graph_t *graph, uint32_t phase,
    ir_func_t *function,
    core_list_t(n00bb_exec_graph_function_t) *registered,
    uint32_t *id_out)
{
    if (!graph || !function || !registered || !id_out) return false;
    for (size_t i = 0; i < registered->len; i++) {
        n00bb_exec_graph_function_t *entry = &registered->data[i];
        if (entry->function != function) continue;
        if (entry->phase != phase) return false;
        *id_out = entry->id;
        return true;
    }
    uint32_t id;
    bool external_initializer = !function->n_blocks
        && function->linkage == IR_LINK_EXTERN
        && ir_func_is_initializer(function)
        && th_is_nil(th_func_error(function->type));
    ir_exec_graph_outcome_t success = IR_EXEC_GRAPH_OUTCOME_SUCCESS;
    bool added = external_initializer
        ? ir_exec_graph_add_external_function(graph, phase, function,
            &success, 1, &id)
        : ir_exec_graph_add_function(graph, phase, function,
            nullptr, nullptr, &id);
    if (!added) return false;
    core_list_push(*registered,
        ((n00bb_exec_graph_function_t){function, phase, id}));
    *id_out = id;
    return true;
}

static bool
collect_exec_function_success_exits(
    ir_exec_graph_t *graph, uint32_t function_id, uint32_t successor_entry,
    core_list_t(size_t) *exit_nodes, size_t *success_count)
{
    ir_exec_graph_function_view_t function;
    if (!ir_exec_graph_function_at(graph, function_id, &function))
        return false;
    for (size_t i = 0; i < function.exit_count; i++) {
        uint32_t node;
        ir_exec_graph_outcome_t outcome;
        if (!ir_exec_graph_function_exit_at(graph, function_id, i,
                &node, &outcome)) return false;
        if (outcome != IR_EXEC_GRAPH_OUTCOME_SUCCESS) continue;
        if (success_count) (*success_count)++;
        if (successor_entry != UINT32_MAX
            && !ir_exec_graph_add_edge(graph, node, successor_entry,
                IR_EXEC_GRAPH_EDGE_SEQUENCE)) return false;
        if (exit_nodes) core_list_push(*exit_nodes, (size_t)node);
    }
    return true;
}

static bool
build_initializer_graph(
    ir_exec_graph_t *graph, uint32_t phase_id, uint32_t phase_entry,
    core_list_t(n00bb_ir_func_ptr_t) *initializers,
    core_list_t(n00bb_exec_graph_function_t) *registered_functions)
{
    uint32_t previous_function = UINT32_MAX;
    for (size_t i = 0; i < initializers->len; i++) {
        uint32_t function_id;
        ir_exec_graph_function_view_t region;
        if (!exec_graph_function_id(graph, phase_id, initializers->data[i],
                registered_functions, &function_id)
            || !ir_exec_graph_function_at(graph, function_id, &region))
            return false;
        bool linked = previous_function == UINT32_MAX
            ? ir_exec_graph_add_edge(graph, phase_entry, region.entry_node,
                                     IR_EXEC_GRAPH_EDGE_SEQUENCE)
            : collect_exec_function_success_exits(graph, previous_function,
                region.entry_node, nullptr, nullptr);
        if (!linked) return false;
        previous_function = function_id;
    }
    if (previous_function != UINT32_MAX) return true;
    uint32_t empty_success;
    return ir_exec_graph_add_phase_exit(graph, phase_id,
            "empty-initialization", IR_EXEC_GRAPH_OUTCOME_SUCCESS,
            &empty_success)
        && ir_exec_graph_add_edge(graph, phase_entry, empty_success,
                                  IR_EXEC_GRAPH_EDGE_SEQUENCE);
}

enum {
    N00BB_GLOBAL_STATE_UNWRITTEN = 1u << 0,
    N00BB_GLOBAL_STATE_WRITTEN = 1u << 1,
};

static uint8_t
effect_global_apply_effect(uint8_t incoming, uint8_t effects)
{
    uint8_t result = 0;
    if (incoming & N00BB_GLOBAL_STATE_WRITTEN)
        result |= effects ? N00BB_GLOBAL_STATE_WRITTEN : 0;
    if (incoming & N00BB_GLOBAL_STATE_UNWRITTEN)
        result |= effects;
    return result;
}

static bool
effect_function_list_add(core_list_t(n00bb_ir_func_ptr_t) *functions,
                           ir_func_t *function)
{
    if (!functions || !function) return false;
    for (size_t i = 0; i < functions->len; i++)
        if (functions->data[i] == function) return true;
    core_list_push(*functions, function);
    return true;
}

static n00bb_function_effect_summary_t *
effect_summary_for(core_list_t(n00bb_function_effect_summary_t) *summaries,
                     ir_func_t *function)
{
    for (size_t i = 0; summaries && i < summaries->len; i++)
        if (summaries->data[i].function == function)
            return &summaries->data[i];
    return nullptr;
}

static bool
effect_summary_add(
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_func_t *function)
{
    if (!summaries || !function) return false;
    if (effect_summary_for(summaries, function)) return true;
    n00bb_function_effect_summary_t summary = {.function = function};
    if (function->n_params) {
        summary.parameter_success_states = calloc(function->n_params,
                                                   sizeof(uint8_t));
        summary.parameter_failure_states = calloc(function->n_params,
                                                   sizeof(uint8_t));
        if (!summary.parameter_success_states
            || !summary.parameter_failure_states) {
            free(summary.parameter_success_states);
            free(summary.parameter_failure_states);
            return false;
        }
    }
    core_list_push(*summaries, summary);
    return true;
}

static bool
effect_function_is_fallible(ir_func_t *function)
{
    if (!function || !function->owner || !function->type) return false;
    th_type *failure = th_func_error(function->type);
    return failure && !th_is_nil(failure);
}

static uint8_t
effect_function_outcomes(
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_func_t *function, size_t target_index, bool failure)
{
    if (!function) return N00BB_GLOBAL_STATE_UNWRITTEN;
    bool fallible = effect_function_is_fallible(function);
    if (failure && !fallible) return 0;
    if (!function->n_blocks)
        return N00BB_GLOBAL_STATE_UNWRITTEN;
    n00bb_function_effect_summary_t *summary =
        effect_summary_for(summaries, function);
    if (!summary) return N00BB_GLOBAL_STATE_UNWRITTEN;
    if (target_index >= summary->target_count) return 0;
    return failure ? summary->failure_states[target_index]
                   : summary->success_states[target_index];
}

static uint8_t
effect_parameter_outcomes(
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_func_t *function, uint32_t parameter, bool failure)
{
    n00bb_function_effect_summary_t *summary =
        effect_summary_for(summaries, function);
    if (!summary || parameter >= function->n_params) return 0;
    uint8_t *states = failure ? summary->parameter_failure_states
                              : summary->parameter_success_states;
    return states ? states[parameter] : 0;
}

static bool
effect_function_summaries_expand_calls(
    core_list_t(n00bb_function_effect_summary_t) *summaries)
{
    if (!summaries) return false;
    for (size_t i = 0; i < summaries->len; i++) {
        ir_func_t *function = summaries->data[i].function;
        ir_doms_t *doms = function->n_blocks ? ir_doms_of(function) : nullptr;
        for (size_t b = 0; doms && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            if (!ir_block_reachable(doms, block)) continue;
            for (size_t n = 0; n < block->n_instrs; n++) {
                ir_instr_t *instruction = block->instrs[n];
                if (!instruction || instruction->op != IR_OP_CALL
                    || !instruction->n_operands
                    || instruction->operands[0].kind != IR_OPD_VALUE)
                    continue;
                ir_value_t *callee = instruction->operands[0].value;
                if (callee && callee->kind == IR_VAL_FUNC && callee->owner
                    && !effect_summary_add(summaries, callee->owner))
                    return false;
            }
        }
    }
    return true;
}

static bool
collect_phase_reachable_function(ir_func_t *function, void *raw)
{
    core_list_t(n00bb_ir_func_ptr_t) *functions = raw;
    core_list_push(*functions, function);
    return true;
}

ir_func_t *
n00bb_root_entry(const n00bb_link_analysis_t *analysis, const char *name)
{
    n00bb_tu_state_t *any = analysis && analysis->units.len
        ? tu_state(analysis->units.data[0]) : nullptr;
    core_translation_unit_t *root = any && any->link
        ? core_link_unit_root(any->link, 0) : nullptr;
    ir_module_t *module = root ? unit_module(tu_state(root)) : nullptr;
    ir_func_t *function = module ? ir_func_find(module, name) : nullptr;
    return function && function->n_blocks ? function : nullptr;
}

ir_func_t *
n00bb_baked_function(const n00bb_link_analysis_t *analysis,
                     const char *module, const char *name)
{
    for (size_t i = 0; analysis && module && name
         && i < analysis->units.len; i++) {
        ir_module_t *candidate = unit_module(tu_state(analysis->units.data[i]));
        if (!candidate || strcmp(ir_module_name(candidate), module)) continue;
        ir_func_t *function = ir_func_find(candidate, name);
        return function && function->n_blocks ? function : nullptr;
    }
    return nullptr;
}

bool
build_reachable_functions(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_diagnostics_t *diagnostics)
{
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    const char *initializers[] = {
        N00BB_LITERAL_INIT_NAME, N00BB_MODULE_INIT_NAME,
    };
    for (size_t i = 0; i < initializer_order->len; i++) {
        ir_module_t *module = unit_module(
            tu_state(initializer_order->data[i]));
        for (size_t k = 0; module && k < 2; k++) {
            ir_func_t *initializer = ir_func_find(module, initializers[k]);
            if (initializer) core_list_push(roots, initializer);
        }
        ir_func_t *capture = module
            && n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME
            && !strcmp(ir_module_name(module), N00BB_CAPTURE_MODULE)
            ? ir_func_find(module, N00BB_CAPTURE_FUNCTION) : nullptr;
        if (capture) core_list_push(roots, capture);
    }
    const char *entries[] = {N00BB_START_NAME, N00BB_COMPTIME_START_NAME};
    for (size_t i = 0; i < 2; i++) {
        ir_func_t *entry = n00bb_root_entry(analysis, entries[i]);
        if (entry) core_list_push(roots, entry);
    }
    for (size_t i = 0; i < initializer_order->len; i++) {
        n00bb_tu_state_t *state = tu_state(initializer_order->data[i]);
        ir_module_t *module = unit_module(state);
        const char *module_name = module ? ir_module_name(module) : nullptr;
        if (!module_name) continue;
        for (size_t j = 0; j < ir_func_count(module); j++) {
            ir_func_t *function = ir_func_at(module, j);
            if (!function || !function->n_blocks) continue;
            if (!strcmp(function->name, "__finalize")
                || !strncmp(function->name, "__finalize#", 11))
                core_list_push(roots, function);
        }
        size_t prefix = strlen(module_name);
        size_t cursor = 0;
        const n00bb_runtime_native_root_t *native;
        while ((native = n00bb_native_root_in_module(module_name, prefix,
                                                     &cursor))) {
            ir_func_t *function = ir_func_find(module,
                n00bb_native_root_function(native, prefix));
            if (function && function->n_blocks)
                core_list_push(roots, function);
        }
    }
    for (size_t r = 0; r < analysis->native_root_count; r++) {
        bool found = false;
        for (size_t i = 0; i < initializer_order->len && !found; i++) {
            n00bb_tu_state_t *state = tu_state(initializer_order->data[i]);
            ir_module_t *module = unit_module(state);
            const char *module_name = module
                ? ir_module_name(module) : nullptr;
            if (!module_name) continue;
            core_string_t owner = core_string_from_cstr(module_name);
            for (size_t j = 0; j < ir_func_count(module); j++) {
                ir_func_t *function = ir_func_at(module, j);
                if (!function || !function->n_blocks
                    || !native_root_matches(analysis->native_roots[r],
                        owner, core_string_from_cstr(function->name)))
                    continue;
                core_list_push(roots, function);
                found = true;
                break;
            }
        }
        if (!found) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "native root '%.*s' has no lowered function body",
                (int)analysis->native_roots[r].u8_bytes,
                analysis->native_roots[r].data);
            core_diagnostics_report(diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0019"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            core_list_free(roots);
            return false;
        }
    }
    for (size_t r = 0; r < n00bb_native_root_registered_count(); r++) {
        const char *name = n00bb_native_root_registered_at(r)->qualified_name;
        bool found = false;
        for (size_t i = 0; i < initializer_order->len && !found; i++) {
            ir_module_t *module = unit_module(
                tu_state(initializer_order->data[i]));
            const char *module_name = module
                ? ir_module_name(module) : nullptr;
            size_t prefix = module_name ? strlen(module_name) : 0;
            ir_func_t *function = prefix && !strncmp(name, module_name, prefix)
                && name[prefix] == '.'
                ? ir_func_find(module, name + prefix + 1) : nullptr;
            found = function && function->n_blocks;
        }
        if (!found) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "provided function '%s' has no lowered function body", name);
            core_diagnostics_report(diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0019"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            core_list_free(roots);
            return false;
        }
    }
    for (size_t s = 0; s < analysis->static_graph_slots.len; s++) {
        const n00bb_static_graph_slot_t *slot =
            &analysis->static_graph_slots.data[s];
        if (slot->table != N00B_ABI_TABLE_FUNCTION) continue;
        ir_func_t *function = n00bb_baked_function(analysis, slot->module,
                                                   slot->name);
        if (!function) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "baked function '%s.%s' has no lowered function body",
                slot->module, slot->name);
            core_diagnostics_report(diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0019"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            core_list_free(roots);
            return false;
        }
        core_list_push(roots, function);
    }
    core_list_t(n00bb_ir_func_ptr_t) *reachable = &analysis->reachable_functions;
    reachable->len = 0;
    bool ok = ir_walk_reachable_functions(roots.data, roots.len,
        collect_phase_reachable_function, reachable);
    core_list_free(roots);
    return ok;
}

static ir_instr_t *
effect_branch_outcome_call(ir_instr_t *terminator,
                             bool *true_is_success)
{
    if (!terminator || terminator->op != IR_OP_BRANCH
        || terminator->n_operands < 3
        || terminator->operands[0].kind != IR_OPD_VALUE) return nullptr;
    ir_value_t *condition = terminator->operands[0].value;
    ir_instr_t *comparison = condition ? condition->def : nullptr;
    if (!comparison || (comparison->op != IR_OP_EQ
            && comparison->op != IR_OP_NE)
        || comparison->n_operands != 2) return nullptr;
    for (size_t tag_index = 0; tag_index < 2; tag_index++) {
        ir_operand_t *tag_operand = &comparison->operands[tag_index];
        ir_operand_t *literal_operand = &comparison->operands[1 - tag_index];
        if (tag_operand->kind != IR_OPD_VALUE
            || literal_operand->kind != IR_OPD_VALUE) continue;
        ir_instr_t *tag = tag_operand->value
            ? tag_operand->value->def : nullptr;
        ir_instr_t *literal = literal_operand->value
            ? literal_operand->value->def : nullptr;
        if (!tag || tag->op != IR_OP_VARIANT_TAG || tag->n_operands != 1
            || tag->operands[0].kind != IR_OPD_VALUE
            || !tag->operands[0].value || !tag->operands[0].value->def
            || tag->operands[0].value->def->op != IR_OP_CALL
            || !literal || literal->op != IR_OP_CONST
            || literal->n_operands != 1
            || literal->operands[0].kind != IR_OPD_CONST
            || literal->operands[0].lit.i.neg
            || literal->operands[0].lit.i.mag != th_arm_tag("ok"))
            continue;
        if (true_is_success) *true_is_success = comparison->op == IR_OP_EQ;
        return tag->operands[0].value->def;
    }
    return nullptr;
}

static bool
effect_target_matches(ir_value_t *target, ir_value_t *candidate)
{
    if (!target || !candidate) return false;
    if (target->kind == IR_VAL_GLOBAL)
        return candidate->kind == IR_VAL_GLOBAL && target->global
            && candidate->global
            && ir_global_identity(candidate->global)
                == ir_global_identity(target->global);
    return target == candidate;
}

typedef struct {
    ir_value_t *target;
    bool saw_target;
    bool all_targets_match;
} effect_store_target_t;

static bool
effect_store_target_visit(void *context, ir_value_t *candidate)
{
    effect_store_target_t *match = context;
    match->saw_target = true;
    match->all_targets_match &= effect_target_matches(match->target,
                                                         candidate);
    return true;
}

static bool
effect_store_targets(ir_instr_t *instruction, ir_value_t *target)
{
    if (!instruction || !target
        || (instruction->op != IR_OP_GLOBAL_STORE
            && instruction->op != IR_OP_STORE)
        || instruction->n_operands < 1
        || instruction->operands[0].kind != IR_OPD_VALUE) return false;
    effect_store_target_t match = {
        .target = target,
        .all_targets_match = true,
    };
    bool complete = ir_value_visit_store_origins(
        instruction->operands[0].value,
        effect_store_target_visit, &match);
    return complete && match.saw_target && match.all_targets_match;
}

static bool
effect_call_argument_targets(ir_instr_t *call, uint32_t parameter,
                               ir_value_t *target)
{
    if (!call || !target || call->op != IR_OP_CALL
        || parameter + 1 >= call->n_operands
        || call->operands[parameter + 1].kind != IR_OPD_VALUE) return false;
    effect_store_target_t match = {
        .target = target,
        .all_targets_match = true,
    };
    bool complete = ir_value_visit_store_origins(
        call->operands[parameter + 1].value,
        effect_store_target_visit, &match);
    return complete && match.saw_target && match.all_targets_match;
}

static uint8_t
effect_effect_sequence(uint8_t first, uint8_t second)
{
    uint8_t result = 0;
    if (first & N00BB_GLOBAL_STATE_WRITTEN)
        result |= N00BB_GLOBAL_STATE_WRITTEN;
    if (first & N00BB_GLOBAL_STATE_UNWRITTEN) result |= second;
    return result;
}

static uint8_t
effect_call_effects(
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_instr_t *call, ir_value_t *target, size_t target_index, bool failure)
{
    ir_func_t *callee = call && call->n_operands
        && call->operands[0].kind == IR_OPD_VALUE
        && call->operands[0].value
        && call->operands[0].value->kind == IR_VAL_FUNC
        ? call->operands[0].value->owner : nullptr;
    uint8_t effects = effect_function_outcomes(summaries, callee,
                                                 target_index, failure);
    if (!callee || (failure && !effect_function_is_fallible(callee)))
        return effects;
    for (uint32_t p = 0; p < callee->n_params; p++) {
        uint8_t parameter_effects = effect_parameter_outcomes(
            summaries, callee, p, failure);
        if (parameter_effects
            && effect_call_argument_targets(call, p, target))
            effects = effect_effect_sequence(effects, parameter_effects);
    }
    return effects;
}

static bool
thread_read_site_at(
    core_list_t(n00bb_thread_read_site_t) *read_sites,
    ir_func_t *function, ir_instr_t *instruction)
{
    for (size_t i = 0; read_sites && i < read_sites->len; i++)
        if (read_sites->data[i].function == function
            && read_sites->data[i].instruction == instruction)
            return true;
    return false;
}

static bool
effect_analyze_targets_function(
    ir_func_t *function, ir_value_t *const *targets, size_t target_count,
    const size_t *summary_indices,
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    core_list_t(n00bb_thread_read_site_t) *thread_read_sites,
    const uint8_t *initial_states, bool *reads_before_initialization,
    ir_instr_t **first_uninitialized_read,
    uint8_t *success_states, uint8_t *failure_states,
    ir_instr_t *stop_before, uint8_t *states_before)
{
    if (!function || !targets || !target_count || !initial_states
        || !success_states || !failure_states
        || (function->n_blocks
            && target_count > SIZE_MAX / function->n_blocks))
        return false;
    if (reads_before_initialization)
        memset(reads_before_initialization, 0,
               target_count * sizeof(*reads_before_initialization));
    if (first_uninitialized_read)
        memset(first_uninitialized_read, 0,
               target_count * sizeof(*first_uninitialized_read));
    memset(success_states, 0, target_count);
    memset(failure_states, 0, target_count);
    if (states_before) memset(states_before, 0, target_count);
    if (!function->n_blocks) {
        memcpy(success_states, initial_states, target_count);
        if (effect_function_is_fallible(function))
            memcpy(failure_states, initial_states, target_count);
        return true;
    }
    ir_block_t *entry = ir_block_entry(function);
    ir_doms_t *doms = ir_doms_of(function);
    uint8_t *incoming = calloc(function->n_blocks * target_count, 1);
    uint8_t *state = calloc(target_count, 1);
    uint8_t *before_outcome = calloc(target_count, 1);
    bool *stored_after_outcome = calloc(target_count,
                                        sizeof(*stored_after_outcome));
    if (!entry || !doms || !incoming || !state || !before_outcome
        || !stored_after_outcome
        || entry->index >= function->n_blocks) {
        free(incoming);
        free(state);
        free(before_outcome);
        free(stored_after_outcome);
        return false;
    }
    memcpy(incoming + entry->index * target_count, initial_states,
           target_count);
    bool changed;
    do {
        changed = false;
        for (size_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            if (!block || block->index >= function->n_blocks
                || !ir_block_reachable(doms, block)) continue;
            memcpy(state, incoming + block->index * target_count,
                   target_count);
            bool active = false;
            for (size_t t = 0; t < target_count; t++) active |= state[t] != 0;
            if (!active) continue;
            ir_instr_t *terminator = ir_block_terminator(block);
            bool true_is_success = false;
            ir_instr_t *outcome_call =
                effect_branch_outcome_call(terminator, &true_is_success);
            bool call_after_outcome = false;
            bool after_outcome_call = false;
            for (size_t n = 0; n < block->n_instrs; n++) {
                ir_instr_t *instruction = block->instrs[n];
                if (instruction == outcome_call) {
                    after_outcome_call = true;
                    continue;
                }
                if (after_outcome_call && instruction
                    && instruction->op == IR_OP_CALL)
                    call_after_outcome = true;
            }
            if (call_after_outcome) outcome_call = nullptr;

            bool found_outcome_call = false;
            memset(stored_after_outcome, 0,
                   target_count * sizeof(*stored_after_outcome));
            after_outcome_call = false;
            for (size_t n = 0; n < block->n_instrs; n++) {
                ir_instr_t *instruction = block->instrs[n];
                if (!instruction) continue;
                if (instruction == stop_before && states_before)
                    for (size_t t = 0; t < target_count; t++)
                        states_before[t] |= state[t];
                if (thread_read_site_at(thread_read_sites, function,
                                        instruction)) {
                    for (size_t t = 0; t < target_count; t++) {
                        if (!(state[t] & N00BB_GLOBAL_STATE_UNWRITTEN))
                            continue;
                        if (reads_before_initialization)
                            reads_before_initialization[t] = true;
                        if (first_uninitialized_read
                            && !first_uninitialized_read[t])
                            first_uninitialized_read[t] = instruction;
                    }
                }
                if (instruction == terminator) continue;
                if (instruction == outcome_call) {
                    memcpy(before_outcome, state, target_count);
                    found_outcome_call = true;
                    after_outcome_call = true;
                    continue;
                }
                bool store = false;
                for (size_t t = 0; t < target_count; t++) {
                    if (!state[t]
                        || !effect_store_targets(instruction, targets[t]))
                        continue;
                    state[t] = N00BB_GLOBAL_STATE_WRITTEN;
                    stored_after_outcome[t] |= after_outcome_call;
                    store = true;
                }
                if (store) continue;
                if (instruction->op != IR_OP_CALL || after_outcome_call)
                    continue;
                ir_func_t *callee = instruction->n_operands
                    && instruction->operands[0].kind == IR_OPD_VALUE
                    && instruction->operands[0].value
                    && instruction->operands[0].value->kind == IR_VAL_FUNC
                    ? instruction->operands[0].value->owner : nullptr;
                if (effect_function_is_fallible(callee)) continue;
                for (size_t t = 0; t < target_count; t++) {
                    uint8_t effects = effect_call_effects(
                        summaries, instruction, targets[t],
                        summary_indices ? summary_indices[t] : t, false);
                    state[t] = effect_global_apply_effect(state[t], effects);
                }
            }

            if (terminator && terminator->op == IR_OP_RETURN) {
                for (size_t t = 0; t < target_count; t++)
                    success_states[t] |= state[t];
                continue;
            }
            if (terminator && terminator->op == IR_OP_FAIL) {
                for (size_t t = 0; t < target_count; t++)
                    failure_states[t] |= state[t];
                continue;
            }
            if (terminator && terminator->op == IR_OP_BRANCH
                && found_outcome_call) {
                ir_block_t *true_block = terminator->operands[1].block;
                for (size_t s = 0; s < ir_succ_count(terminator); s++) {
                    ir_block_t *successor = ir_succ_at(terminator,
                                                       (uint32_t)s);
                    if (!successor || successor->index >= function->n_blocks)
                        continue;
                    bool call_success = (successor == true_block)
                        == true_is_success;
                    uint8_t *next = incoming
                        + successor->index * target_count;
                    for (size_t t = 0; t < target_count; t++) {
                        uint8_t effects = effect_call_effects(summaries,
                            outcome_call, targets[t],
                            summary_indices ? summary_indices[t] : t,
                            !call_success);
                        uint8_t edge = effect_global_apply_effect(
                            before_outcome[t], effects);
                        if (stored_after_outcome[t] && edge)
                            edge = N00BB_GLOBAL_STATE_WRITTEN;
                        uint8_t merged = next[t] | edge;
                        changed |= merged != next[t];
                        next[t] = merged;
                    }
                }
                continue;
            }
            for (size_t s = 0; terminator
                 && s < ir_succ_count(terminator); s++) {
                ir_block_t *successor = ir_succ_at(terminator, (uint32_t)s);
                if (!successor || successor->index >= function->n_blocks)
                    continue;
                uint8_t *next = incoming + successor->index * target_count;
                for (size_t t = 0; t < target_count; t++) {
                    uint8_t merged = next[t] | state[t];
                    changed |= merged != next[t];
                    next[t] = merged;
                }
            }
        }
    } while (changed);
    free(incoming);
    free(state);
    free(before_outcome);
    free(stored_after_outcome);
    return true;
}

static bool
effect_analyze_target_function(
    ir_func_t *function, ir_value_t *target, size_t target_index,
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    core_list_t(n00bb_thread_read_site_t) *thread_read_sites,
    uint8_t initial_state, bool *reads_before_initialization,
    ir_instr_t **first_uninitialized_read,
    uint8_t *success_states, uint8_t *failure_states,
    ir_instr_t *stop_before, uint8_t *states_before)
{
    ir_value_t *targets[] = {target};
    size_t indices[] = {target_index};
    return effect_analyze_targets_function(function, targets, 1,
        indices, summaries, thread_read_sites, &initial_state,
        reads_before_initialization, first_uninitialized_read,
        success_states, failure_states, stop_before, states_before);
}
static bool
effect_function_summaries_build(
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_value_t *const *targets, size_t target_count)
{
    if (!summaries || !targets || !target_count) return false;
    uint8_t *initial = malloc(target_count);
    uint8_t *success = malloc(target_count);
    uint8_t *failure = malloc(target_count);
    if (!initial || !success || !failure) {
        free(initial);
        free(success);
        free(failure);
        return false;
    }
    memset(initial, N00BB_GLOBAL_STATE_UNWRITTEN, target_count);
    bool ok = true;
    for (size_t i = 0; i < summaries->len; i++) {
        n00bb_function_effect_summary_t *summary = &summaries->data[i];
        free(summary->success_states);
        free(summary->failure_states);
        summary->success_states = calloc(target_count, 1);
        summary->failure_states = calloc(target_count, 1);
        summary->target_count = target_count;
        if (!summary->success_states || !summary->failure_states) {
            ok = false;
            break;
        }
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; ok && i < summaries->len; i++) {
            n00bb_function_effect_summary_t *summary = &summaries->data[i];
            ok = effect_analyze_targets_function(summary->function,
                targets, target_count, nullptr, summaries, nullptr, initial,
                nullptr, nullptr, success, failure, nullptr, nullptr);
            for (size_t t = 0; ok && t < target_count; t++) {
                uint8_t next_success = summary->success_states[t]
                    | success[t];
                uint8_t next_failure = summary->failure_states[t]
                    | failure[t];
                changed |= next_success != summary->success_states[t]
                    || next_failure != summary->failure_states[t];
                summary->success_states[t] = next_success;
                summary->failure_states[t] = next_failure;
            }
        }
    } while (ok && changed);
    free(initial);
    free(success);
    free(failure);
    return ok;
}

static bool
effect_parameter_summaries_build(
    core_list_t(n00bb_function_effect_summary_t) *summaries)
{
    if (!summaries) return false;
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < summaries->len; i++) {
            n00bb_function_effect_summary_t *summary = &summaries->data[i];
            for (uint32_t p = 0; p < summary->function->n_params; p++) {
                uint8_t success, failure;
                if (!effect_analyze_target_function(summary->function,
                        summary->function->params[p], 0, summaries, nullptr,
                        N00BB_GLOBAL_STATE_UNWRITTEN, nullptr, nullptr,
                        &success, &failure, nullptr, nullptr)) return false;
                uint8_t next_success = summary->parameter_success_states[p]
                    | success;
                uint8_t next_failure = summary->parameter_failure_states[p]
                    | failure;
                changed |= next_success
                        != summary->parameter_success_states[p]
                    || next_failure
                        != summary->parameter_failure_states[p];
                summary->parameter_success_states[p] = next_success;
                summary->parameter_failure_states[p] = next_failure;
            }
        }
    } while (changed);
    return true;
}

static bool
effect_global_final_state(
    ir_func_t *const *roots, size_t root_count,
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_global_t *source, size_t target_index,
    bool *phase_succeeds, bool *definitely_initialized,
    bool *has_successful_write)
{
    if ((!roots && root_count) || !summaries || !source || !phase_succeeds
        || !definitely_initialized || !has_successful_write) return false;
    uint8_t state = source->has_static_initializer
        ? N00BB_GLOBAL_STATE_WRITTEN
        : N00BB_GLOBAL_STATE_UNWRITTEN;
    uint8_t write_state = N00BB_GLOBAL_STATE_UNWRITTEN;
    *phase_succeeds = true;
    for (size_t i = 0; i < root_count; i++) {
        uint8_t effects = effect_function_outcomes(
            summaries, roots[i], target_index, false);
        if (!effects) {
            *phase_succeeds = false;
            state = 0;
            break;
        }
        state = effect_global_apply_effect(state, effects);
        write_state = effect_global_apply_effect(write_state, effects);
    }
    *definitely_initialized = state != 0
        && !(state & N00BB_GLOBAL_STATE_UNWRITTEN);
    *has_successful_write =
        (write_state & N00BB_GLOBAL_STATE_WRITTEN) != 0;
    return true;
}

static bool
build_n00bb_execution_graph(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *order)
{
    ir_exec_graph_t *graph = ir_exec_graph_new(analysis->ir_context);
    uint32_t phase, entry;
    if (!graph || !ir_exec_graph_add_phase(graph, "initialization", &phase,
                                           &entry)) return false;
    const char *names[] = {N00BB_LITERAL_INIT_NAME, N00BB_MODULE_INIT_NAME};
    core_list_t(n00bb_ir_func_ptr_t) initializers =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < order->len; i++) {
        ir_module_t *module = unit_module(tu_state(order->data[i]));
        for (size_t k = 0; module && k < 2; k++) {
            ir_func_t *initializer = ir_func_find(module, names[k]);
            if (initializer) core_list_push(initializers, initializer);
        }
    }
    core_list_t(n00bb_exec_graph_function_t) registered_functions =
        core_list_new(n00bb_exec_graph_function_t);
    bool ok = build_initializer_graph(graph, phase, entry, &initializers,
                                      &registered_functions)
        && ir_exec_graph_validate(graph);
    core_list_free(initializers);
    core_list_free(registered_functions);
    if (ok) analysis->execution_graph = graph;
    return ok;
}

static bool
lower_ordered_module_initializers(
    ir_lowerer_t *lowerer, parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    ir_ctx_t *ir_context,
    core_list_t(core_translation_unit_ptr_t) *order)
{
    for (size_t i = 0; i < order->len; i++)
        if (!lower_module_initializer(lowerer, grammar, order->data[i],
                units, ir_context, i)
            || !lower_literal_initializer(ir_context, order->data[i], i))
            return false;
    return true;
}

static void
dump_ir_instr(FILE *out, const ir_instr_t *instr)
{
    fprintf(out, "@%s:%s#%u (%s)", instr->block->owner->name,
            instr->block->name, instr->index,
            ir_op_spelling(instr->op));
}

static void
dump_ir_analysis(ir_module_t *module, bool control_flow, bool full_pdg)
{
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (control_flow) {
            ir_pdg_t *pdg = ir_pdg_of(function);
            fprintf(stdout, "control-flow @%s\n", function->name);
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                ir_instr_t *term = ir_block_terminator(block);
                bool has_successor = false;
                for (uint32_t s = 0; term && s < term->n_operands; s++) {
                    ir_block_t *target = ir_succ_of(&term->operands[s]);
                    if (!target) continue;
                    fprintf(stdout, "  %s -> %s [%s]\n", block->name,
                            target->name,
                            ir_edge_kind_name(ir_edge_kind(pdg, block, s)));
                    has_successor = true;
                }
                if (!has_successor)
                    fprintf(stdout, "  %s -> exit\n", block->name);
            }
        }
        if (full_pdg) {
            ir_pdg_t *pdg = ir_pdg_of(function);
            fprintf(stdout, "pdg @%s (%s)\n", function->name,
                    ir_pdg_is_exact(pdg) ? "exact" : "may");
            for (uint32_t e = 0; e < ir_pdg_edge_count(pdg); e++) {
                const ir_dep_edge_t *edge = ir_pdg_edge_at(pdg, e);
                fputs(edge->kind == IR_DEP_CONTROL ? "  control " : "  data ",
                      stdout);
                dump_ir_instr(stdout, edge->from);
                fputs(edge->certainty == IR_DEP_EXACT ? " => " : " ~> ",
                      stdout);
                dump_ir_instr(stdout, edge->to);
                if (edge->via) fprintf(stdout, " via $%s", edge->via->name);
                fputc('\n', stdout);
            }
        }
    }
}

static void
dump_layout_for_unit(parse_grammar_t *grammar, core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab ||
        !state->ir_module) return;
    const char *target = ir_module_target(state->ir_module);
    core_list_t(parse_scope_ptr_t) scopes = module_scopes_in_namespace(
        state, CORE_STRING_STATIC("type"));
    for (size_t s = 0; s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type) continue;
            ir_layout_compute(state->ir_module, symbol->type, nullptr);
            ir_descriptor_t *descriptor = ir_layout_of(state->ir_module,
                symbol->type, target);
            if (!descriptor) continue;
            printf("layout %s target=%s size=", n00bb_print_type(state->types,
                symbol->type), target ? target : "<none>");
            if (ir_layout_is_settled(&descriptor->size))
                printf("%llu", (unsigned long long)descriptor->size.value);
            else fputs("<unresolved>", stdout);
            fputs(" align=", stdout);
            if (ir_layout_is_settled(&descriptor->align))
                printf("%llu", (unsigned long long)descriptor->align.value);
            else fputs("<unresolved>", stdout);
            putchar('\n');
            for (uint32_t m = 0; m < descriptor->n_members; m++) {
                const ir_member_layout_t *member = &descriptor->members[m];
                printf("  member %s bit-offset=", member->name);
                if (ir_layout_is_settled(&member->bit_offset))
                    printf("%llu", (unsigned long long)
                        member->bit_offset.value);
                else fputs("<unresolved>", stdout);
                fputs(" bit-width=", stdout);
                if (ir_layout_is_settled(&member->bit_width))
                    printf("%llu", (unsigned long long)
                        member->bit_width.value);
                else fputs("<unresolved>", stdout);
                putchar('\n');
            }
        }
    }
    core_list_free(scopes);
    (void)grammar;
}







static bool
atomic_aggregate_layout_pending(ir_module_t *module, th_type *atomic_type,
                                const core_target_desc_t *target)
{
    const th_arg *argument = th_named_arg(atomic_type, 0);
    th_type *child = argument && argument->kind == TH_ARG_TYPE
        ? th_resolve(argument->type) : nullptr;
    if (!child || ir_layout_final(module, child, target->triple))
        return false;
    if (th_kind_of(child) == TH_RECORD) return true;
    th_type *definition = th_kind_of(child) == TH_NAMED
        ? ir_type_definition(module, child) : nullptr;
    return definition && th_kind_of(definition) == TH_RECORD;
}

static bool
prepare_ir_type_layouts(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *owner,
                        ir_module_t *module, th_type *type,
                        const core_target_desc_t *target,
                        core_list_t(th_type_ptr_t) *visited)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return true;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == type) return true;
    core_list_push(*visited, type);

    bool ok = true;
    switch (th_kind_of(type)) {
    case TH_NAMED: {
        const char *name = nullptr;
        size_t argument_count = 0;
        bool is_variant = false;
        th_named_info(type, &name, nullptr, &is_variant, &argument_count);
        int expected = !is_variant
            ? builtin_generic_type_arity(name) : -1;
        if (expected >= 0 && argument_count != (size_t)expected) {
            validate_builtin_generic_type_arity(grammar, owner, unit, type);
            ok = false;
            break;
        }
        if (th_builtin_kind_of(type) == TH_BUILTIN_ATOMIC) {
            const th_arg *argument = th_named_arg(type, 0);
            if (argument_count != 1 || !argument
                || argument->kind != TH_ARG_TYPE
                || !ir_type_define(module, type, argument->type)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0017"),
                    CORE_STRING_STATIC(
                        "could not register the atomic type layout"),
                    link_source_location(owner));
                ok = false;
                break;
            }
        }
        for (size_t i = 0; ok && i < argument_count; i++) {
            const th_arg *argument = th_named_arg(type, i);
            if (argument && argument->kind == TH_ARG_TYPE)
                ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                                              argument->type, target, visited);
        }
        if (ok && th_builtin_kind_of(type) == TH_BUILTIN_ATOMIC
            && !atomic_aggregate_layout_pending(module, type, target)
            && !n00bb_atomic_native_support(module, type, target)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0018"),
                CORE_STRING_STATIC(
                    "atomic type is not supported by the selected target"),
                link_source_location(owner));
            ok = false;
        }
        break;
    }
    case TH_INDIRECTION:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_ind_referent(type), target, visited);
        break;
    case TH_SEQUENCE:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_seq_elem(type), target, visited);
        break;
    case TH_RECORD:
        for (size_t i = 0; ok && i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            ok = member && prepare_ir_type_layouts(grammar, unit, owner,
                module, member->type, target, visited);
        }
        break;
    case TH_FUNCTION:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_func_ret(type), target, visited)
            && prepare_ir_type_layouts(grammar, unit, owner, module,
                th_func_error(type), target, visited);
        for (size_t i = 0; ok && i < th_func_nparams(type); i++) {
            const th_param *parameter = th_func_param(type, i);
            ok = parameter && prepare_ir_type_layouts(grammar, unit, owner,
                module, parameter->type, target, visited);
        }
        if (ok && th_func_rest(type))
            ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                th_func_rest(type)->type, target, visited);
        for (size_t i = 0; ok && i < th_func_nkw(type); i++) {
            const th_kwparam *parameter = th_func_kw(type, i);
            ok = parameter && prepare_ir_type_layouts(grammar, unit, owner,
                module, parameter->type, target, visited);
        }
        break;
    case TH_SUM:
        for (size_t i = 0; ok && i < th_sum_len(type); i++)
            ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                th_sum_arm(type, i), target, visited);
        break;
    case TH_BINDER:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_binder_body(type), target, visited);
        break;
    default:
        break;
    }
    return ok;
}

static bool
prepare_ir_type_layouts_for_symbol(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_sym_entry_t *symbol,
                                   ir_module_t *module,
                                   const core_target_desc_t *target)
{
    core_list_t(th_type_ptr_t) visited = core_list_new(th_type_ptr_t);
    bool ok = prepare_ir_type_layouts(grammar, unit, symbol->decl_node,
        module, symbol->type, target, &visited)
        && prepare_ir_type_layouts(grammar, unit, symbol->decl_node, module,
            symbol->callable_contract, target, &visited);
    core_list_free(visited);
    return ok;
}

bool
bind_link_declarations_to_ir(parse_grammar_t *grammar,
                             core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             uint32_t producer_stage)
{
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        if (core_link_declaration_is_external_requirement(declaration))
            continue;
        if (core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.builtin-symbol"))) continue;
        n00bb_link_payload_t *payload = declaration
            ? core_link_declaration_payload(declaration) : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        core_translation_unit_t *owner = declaration
            ? core_link_declaration_owner(declaration) : nullptr;
        if (!owner && symbol)
            owner = unit_for_symbol(grammar, &analysis->units, symbol);
        n00bb_tu_state_t *state = tu_state(owner);
        if (!symbol || !state || !state->ir_module) {
            core_string_t module = declaration
                ? core_link_declaration_module(declaration)
                : (core_string_t){0};
            core_string_t key = declaration
                ? core_link_declaration_key(declaration)
                : (core_string_t){0};
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "LinkUnit declaration '%.*s:%.*s' has no symbol or IR module owner",
                (int)module.u8_bytes, module.data,
                (int)key.u8_bytes, key.data);
            core_diagnostics_report(core_link_unit_diagnostics(link), owner,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0035"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            return false;
        }
        if (!core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("n00bb.semantic.symbol"),
                producer_stage, symbol)) return false;
        if (!prepare_ir_type_layouts_for_symbol(grammar, owner, symbol,
                                                 state->ir_module,
                                                 &analysis->target))
            return false;
        bool module_scope = symbol->scope
            && core_string_eq(symbol->scope->scope_tag,
                              CORE_STRING_STATIC("module"));
        if (is_named_type_declaration(grammar, symbol)
            || context_symbol(grammar, symbol)) {
            th_type *type = link_semantic_type_for_symbol(
                grammar, owner, symbol);
            if (!type) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                    CORE_STRING_STATIC(
                        "link-unit declaration has no finalized semantic type"));
                return false;
            }
            if (!core_link_declaration_set_binding(link, declaration,
                    CORE_STRING_STATIC("n00bb.semantic.type"),
                    producer_stage, type)) {
                core_diagnostics_report(core_translation_unit_diagnostics(owner),
                    owner, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0036"),
                    CORE_STRING_STATIC("could not bind a declaration's semantic type"));
                return false;
            }
        }
        else if (module_scope && symbol->kind == PARSE_SYM_FUNCTION) {
            ir_func_t *function = core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.ir.function"));
            if (!function) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                    CORE_STRING_STATIC(
                        "link-unit function has no direct canonical IR binding"));
                return false;
            }
        }
        else if (module_scope && symbol->kind == PARSE_SYM_VARIABLE) {
            if (!symbol->type) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                    CORE_STRING_STATIC(
                        "link-unit global has no finalized source type"));
                return false;
            }
            ir_global_t *global = core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.ir.global"));
            if (!global && !state->interface_module) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                    CORE_STRING_STATIC(
                        "link-unit global has no direct canonical IR binding"));
                return false;
            }
        }
        else if (symbol->kind == PARSE_SYM_TYPEDEF
                 || symbol->kind == PARSE_SYM_TAG) {
            if (!symbol->type) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0006"),
                    CORE_STRING_STATIC(
                        "link-unit type has no finalized semantic binding"));
                return false;
            }
            if (!core_link_declaration_set_binding(link, declaration,
                    CORE_STRING_STATIC("n00bb.semantic.type"),
                    producer_stage, symbol->type)) {
                core_diagnostics_report(core_translation_unit_diagnostics(owner),
                    owner, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0036"),
                    CORE_STRING_STATIC("could not bind a declaration's semantic type"));
                return false;
            }
        }
        if (!core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.type"))) {
            th_type *semantic_type = symbol->type
                ? symbol->type : symbol->callable_contract;
            if (semantic_type
                && !core_link_declaration_set_binding(link, declaration,
                    CORE_STRING_STATIC("n00bb.semantic.type"),
                    producer_stage, semantic_type)) {
                core_diagnostics_report(core_translation_unit_diagnostics(owner),
                    owner, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0036"),
                    CORE_STRING_STATIC("could not bind a declaration's semantic type"));
                return false;
            }
        }
    }
    return true;
}









static ir_instr_t *
runtime_global_read_at(ir_func_t *function, ir_value_t *target,
                      core_diag_location_t location)
{
    ir_instr_t *best = nullptr;
    uint64_t best_width = UINT64_MAX;
    for (size_t b = 0; function && target && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (size_t i = 0; block && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction) continue;
            uint32_t first_operand = instruction->op == IR_OP_GLOBAL_STORE
                    || instruction->op == IR_OP_STORE
                ? 1 : 0;
            bool uses_target = false;
            for (size_t operand = first_operand;
                 operand < instruction->n_operands; operand++) {
                if (instruction->operands[operand].kind == IR_OPD_VALUE
                    && effect_target_matches(target,
                        instruction->operands[operand].value)) {
                    uses_target = true;
                    break;
                }
            }
            if (!uses_target) continue;

            ir_span_t span = instruction->span;
            bool contains = span.start_line <= location.line
                && (span.end_line > location.line
                    || (span.end_line == location.line
                        && span.start_col <= location.column
                        && span.end_col >= location.column));
            if (!contains) continue;
            uint64_t width = (uint64_t)(span.end_line - span.start_line)
                    * UINT64_C(1000000)
                + (span.end_col >= span.start_col
                    ? span.end_col - span.start_col : UINT32_MAX);
            if (width < best_width) {
                best = instruction;
                best_width = width;
            }
        }
    }
    return best;
}

static ir_instr_t *
runtime_call_at(ir_func_t *function, core_diag_location_t location)
{
    ir_instr_t *best = nullptr;
    uint64_t best_width = UINT64_MAX;
    for (size_t b = 0; function && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (size_t i = 0; block && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction || (instruction->op != IR_OP_CALL
                && instruction->op != IR_OP_CALLBACK_CREATE
                && instruction->op != IR_OP_CALLBACK_INVOKE)) continue;
            ir_span_t span = instruction->span;
            bool contains = span.start_line <= location.line
                && (span.end_line > location.line
                    || (span.end_line == location.line
                        && span.start_col <= location.column
                        && span.end_col >= location.column));
            if (!contains) continue;
            uint64_t width = (uint64_t)(span.end_line - span.start_line)
                    * UINT64_C(1000000)
                + (span.end_col >= span.start_col
                    ? span.end_col - span.start_col : UINT32_MAX);
            if (width < best_width) {
                best = instruction;
                best_width = width;
            }
        }
    }
    return best;
}

static bool
thread_read_site_add(
    core_list_t(n00bb_thread_read_site_t) *read_sites,
    ir_func_t *function, ir_instr_t *instruction,
    core_translation_unit_t *unit, core_diag_location_t location)
{
    if (!read_sites || !function || !instruction || !unit) return false;
    for (size_t i = 0; i < read_sites->len; i++)
        if (read_sites->data[i].function == function
            && read_sites->data[i].instruction == instruction)
            return true;
    core_list_push(*read_sites, ((n00bb_thread_read_site_t){
        .function = function,
        .instruction = instruction,
        .unit = unit,
        .location = location,
    }));
    return true;
}

static bool
thread_global_read_dependency(uint32_t kind)
{
    return kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ
        || kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
        || kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY
        || kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY;
}

static bool
thread_read_sites_build(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *thread_global,
    core_list_t(n00bb_thread_read_site_t) *read_sites)
{
    core_link_declaration_t *global_declaration =
        link_declaration_for_symbol(analysis, thread_global);
    ir_global_t *global = link_ir_global_for_symbol(analysis, thread_global);
    ir_value_t *global_value = global
        ? ir_global_address(global->owner, ir_global_identity(global))
        : nullptr;
    if (!link || !analysis || !global_declaration || !global_value
        || !read_sites) return false;

    for (size_t i = 0; i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) return false;
        bool unknown = edge.kind
            == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN;
        if (!unknown && (!thread_global_read_dependency(edge.kind)
                         || edge.to != global_declaration)) continue;
        if (!unknown && edge.state != CORE_LINK_DEPENDENCY_RESOLVED)
            continue;
        parse_sym_entry_t *function_symbol =
            initializer_declaration_symbol(edge.from);
        if (!function_symbol || function_symbol->kind != PARSE_SYM_FUNCTION)
            continue;
        ir_func_t *function = link_ir_function_for_symbol(
            analysis, function_symbol);
        if (!function || !function->n_blocks) continue;

        bool summary = edge.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY
            || edge.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY
            || unknown;
        ir_instr_t *instruction = summary
            ? runtime_call_at(function, edge.source_location)
            : runtime_global_read_at(function, global_value,
                                     edge.source_location);
        if (!instruction) return false;
        core_translation_unit_t *unit = edge.source_unit
            ? edge.source_unit : core_link_declaration_owner(edge.from);
        if (!thread_read_site_add(read_sites, function, instruction, unit,
                                  edge.source_location)) return false;
    }
    return true;
}

static bool
thread_runtime_entry_function(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && symbol->decl_node && nt_is(grammar, symbol->decl_node, "func-def")
        && (symbol->phases & PARSE_SYM_PHASE_RUNTIME)
        && symbol_visibility(symbol) != TH_ENV_PRIVATE;
}

static void
runtime_init_analysis_release(n00bb_runtime_init_analysis_t *runtime)
{
    for (size_t i = 0; i < runtime->summaries.len; i++) {
        n00bb_function_effect_summary_t *summary =
            &runtime->summaries.data[i];
        free(summary->success_states);
        free(summary->failure_states);
        free(summary->parameter_success_states);
        free(summary->parameter_failure_states);
    }
    core_list_free(runtime->summaries);
    core_list_free(runtime->initializer_roots);
    core_list_free(runtime->entry_functions);
    core_list_free(runtime->targets);
    core_dict_free(&runtime->target_indices);
}

static bool
runtime_init_target_add(n00bb_runtime_init_analysis_t *runtime,
                        ir_global_t *global)
{
    ir_global_t *identity = global ? ir_global_identity(global) : nullptr;
    if (!identity) return false;
    if (core_dict_contains(&runtime->target_indices, identity)) return true;
    ir_value_t *target = ir_global_address(global->owner, identity);
    if (!target) return false;
    core_list_push(runtime->targets, target);
    return core_dict_add(&runtime->target_indices, identity,
                         (void *)(uintptr_t)runtime->targets.len);
}

static bool
runtime_init_analysis_build(
    core_link_unit_t *link, parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *runtime_initializer_order,
    n00bb_runtime_init_analysis_t *runtime)
{
    if (!link || !grammar || !analysis || !runtime_initializer_order
        || !runtime)
        return false;
    runtime->initializer_roots = core_list_new(n00bb_ir_func_ptr_t);
    runtime->entry_functions = core_list_new(n00bb_ir_func_ptr_t);
    runtime->targets = core_list_new(n00bb_ir_value_ptr_t);
    runtime->summaries = core_list_new(n00bb_function_effect_summary_t);
    core_dict_init(&runtime->target_indices, nullptr, nullptr);
    for (size_t i = 0; i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) return false;
        if (edge.kind != N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER
            || edge.state != CORE_LINK_DEPENDENCY_RESOLVED || !edge.to)
            continue;
        parse_sym_entry_t *symbol = initializer_declaration_symbol(edge.to);
        if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
            || !is_module_global_symbol(grammar, symbol)) continue;
        ir_global_t *global = link_ir_global_for_symbol(analysis, symbol);
        if (global && !runtime_init_target_add(runtime, global)) return false;
    }

    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind == PARSE_SYM_VARIABLE
                    && symbol->is_thread_local
                    && is_module_global_symbol(grammar, symbol)) {
                    ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                    symbol);
                    if (!runtime_init_target_add(runtime, global))
                        return false;
                }
                if (!thread_runtime_entry_function(grammar, symbol)) continue;
                ir_func_t *function = link_ir_function_for_symbol(analysis,
                                                                  symbol);
                if (!function || !function->n_blocks) continue;
                if (!effect_function_list_add(&runtime->entry_functions,
                                               function)
                    || !effect_summary_add(&runtime->summaries, function))
                    return false;
            }
        }
    }

    if (!runtime->targets.len) return true;
    for (size_t i = 0; i < runtime_initializer_order->len; i++) {
        n00bb_tu_state_t *state = tu_state(runtime_initializer_order->data[i]);
        ir_func_t *initializer = state && state->ir_module
            ? ir_func_find(state->ir_module, N00BB_MODULE_INIT_NAME) : nullptr;
        if (!initializer
            || !effect_function_list_add(&runtime->initializer_roots,
                                           initializer)
            || !effect_summary_add(&runtime->summaries, initializer))
            return false;
    }
    return effect_function_summaries_expand_calls(&runtime->summaries)
        && effect_parameter_summaries_build(&runtime->summaries)
        && effect_function_summaries_build(&runtime->summaries,
            runtime->targets.data, runtime->targets.len);
}
static bool
thread_runtime_entry_state(
    ir_func_t *const *initializer_roots, size_t initializer_count,
    core_list_t(n00bb_function_effect_summary_t) *summaries,
    ir_global_t *global, size_t target_index, bool has_static_default,
    bool *phase_succeeds, uint8_t *state_out)
{
    bool definitely_initialized, has_successful_write;
    if (!effect_global_final_state(initializer_roots, initializer_count,
            summaries, global, target_index,
            phase_succeeds, &definitely_initialized,
            &has_successful_write)) return false;
    if (!*phase_succeeds) {
        *state_out = 0;
    }
    else if (has_static_default || definitely_initialized) {
        *state_out = N00BB_GLOBAL_STATE_WRITTEN;
    }
    else if (has_successful_write) {
        *state_out = N00BB_GLOBAL_STATE_UNWRITTEN
            | N00BB_GLOBAL_STATE_WRITTEN;
    }
    else {
        *state_out = N00BB_GLOBAL_STATE_UNWRITTEN;
    }
    return true;
}

static size_t
initializer_order_index(core_list_t(core_translation_unit_ptr_t) *order,
                        core_translation_unit_t *unit)
{
    for (size_t i = 0; order && i < order->len; i++)
        if (order->data[i] == unit) return i;
    return SIZE_MAX;
}

static bool
initializer_write_precedes_read(
    n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_link_dependency_view_t *read)
{
    parse_sym_entry_t *target = read && read->to
        ? initializer_declaration_symbol(read->to) : nullptr;
    if (!analysis || !runtime || !target) return false;
    core_translation_unit_t *target_unit =
        core_link_declaration_owner(read->to);
    core_translation_unit_t *source_unit = read->from
        ? core_link_declaration_owner(read->from) : read->source_unit;
    if (!target_unit || !source_unit) return false;
    bool different_units = target_unit != source_unit;
    if (different_units) {
        size_t target_order = initializer_order_index(initializer_order,
                                                      target_unit);
        size_t source_order = initializer_order_index(initializer_order,
                                                      source_unit);
        if (target_order == SIZE_MAX || source_order == SIZE_MAX
            || target_order >= source_order) return false;
    }
    ir_global_t *global = link_ir_global_for_symbol(analysis, target);
    uintptr_t encoded = global ? (uintptr_t)core_dict_get(
        &runtime->target_indices, ir_global_identity(global), nullptr) : 0;
    n00bb_tu_state_t *target_state = tu_state(target_unit);
    ir_func_t *initializer = target_state && target_state->ir_module
        ? ir_func_find(target_state->ir_module, N00BB_MODULE_INIT_NAME)
        : nullptr;
    ir_value_t *target_value = global
        ? ir_global_address(global->owner, ir_global_identity(global))
        : nullptr;
    if (!initializer || !encoded || !target_value) return false;
    size_t target_index = encoded - 1;
    if (different_units) {
        bool succeeds, initialized, has_write;
        return effect_global_final_state(&initializer, 1,
            &runtime->summaries, global, target_index, &succeeds,
            &initialized, &has_write)
            && succeeds && initialized && has_write;
    }
    n00bb_tu_state_t *source_state = tu_state(source_unit);
    ir_func_t *source_initializer = source_state && source_state->ir_module
        ? ir_func_find(source_state->ir_module, N00BB_MODULE_INIT_NAME)
        : nullptr;
    ir_instr_t *read_instruction = runtime_global_read_at(
        source_initializer, target_value, read->source_location);
    if (!read_instruction)
        read_instruction = runtime_call_at(source_initializer,
                                           read->source_location);
    uint8_t success, failure, state_before_read;
    return read_instruction
        && effect_analyze_target_function(source_initializer,
            target_value, target_index, &runtime->summaries, nullptr,
            N00BB_GLOBAL_STATE_UNWRITTEN, nullptr, nullptr,
            &success, &failure, read_instruction, &state_before_read)
        && state_before_read == N00BB_GLOBAL_STATE_WRITTEN;
}

static bool
validate_uninitialized_global_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    core_list_t(core_translation_unit_ptr_t) *initializer_order)
{
    if (!link || !analysis) return false;
    for (size_t i = 0; i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) return false;
        if (edge.kind != N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER
            || edge.state != CORE_LINK_DEPENDENCY_RESOLVED || !edge.to)
            continue;
        parse_sym_entry_t *target = initializer_declaration_symbol(edge.to);
        if (!target || target->kind != PARSE_SYM_VARIABLE
            || !is_module_global_symbol(analysis->grammar, target)
            || global_has_explicit_initializer(analysis->grammar, target)
            || (edge.source_site && edge.source_unit
                && initializer_node_is_reference_store_target(
                    analysis->grammar, edge.source_unit,
                    (parse_tree_t *)edge.source_site, target))
            || initializer_write_precedes_read(analysis, runtime,
                initializer_order, &edge))
            continue;
        core_translation_unit_t *source_unit = edge.from
            ? core_link_declaration_owner(edge.from) : edge.source_unit;
        if (!source_unit) return false;
        core_translation_unit_t *target_unit =
            core_link_declaration_owner(edge.to);
        bool same_cycle = initializer_units_share_cycle(
            link, source_unit, target_unit);
        bool write_candidate = has_initializer_write_candidate(
            link, analysis, target);
        core_string_t message = target->is_thread_local
            ? same_cycle
                ? CORE_STRING_STATIC(
                    "initialization in an import cycle reads a thread-local global before a guaranteed initialization")
                : CORE_STRING_STATIC(
                    "initializer reads a thread-local global before a guaranteed initialization")
            : same_cycle
                ? write_candidate
                    ? CORE_STRING_STATIC("initialization in an import cycle reads a global before a guaranteed initialization")
                    : CORE_STRING_STATIC("initialization in an import cycle reads a global without an explicit initializer")
                : write_candidate
                    ? CORE_STRING_STATIC("initializer reads a global before a guaranteed initialization")
                    : CORE_STRING_STATIC("initializer reads a global without an explicit initializer");
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(source_unit), source_unit,
            CORE_DIAG_ERROR,
            same_cycle ? CORE_STRING_STATIC("N00BB-L0041")
                       : CORE_STRING_STATIC("N00BB-L0044"),
            message, edge.source_location);
    }
    return true;
}

static bool
validate_thread_function_reads(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    n00bb_runtime_init_analysis_t *runtime,
    bool comptime_phase_succeeds)
{
    if (!link || !analysis || !runtime) return false;
    if (!comptime_phase_succeeds) return true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *thread_global = scope->first_in_scope;
                 thread_global; thread_global = thread_global->next_in_scope) {
                if (!is_module_global_symbol(analysis->grammar,
                        thread_global) || !thread_global->is_thread_local)
                    continue;
                ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                thread_global);
                if (!global) return false;
                uintptr_t encoded = (uintptr_t)core_dict_get(
                    &runtime->target_indices, ir_global_identity(global),
                    nullptr);
                if (!encoded) return false;
                size_t target_index = encoded - 1;
                ir_value_t *target = runtime->targets.data[target_index];
                core_list_t(n00bb_thread_read_site_t) read_sites =
                    core_list_new(n00bb_thread_read_site_t);
                bool ok = thread_read_sites_build(link, analysis,
                                                  thread_global, &read_sites);
                uint8_t entry_state = 0;
                bool runtime_succeeds = false;
                ok = ok && thread_runtime_entry_state(
                    runtime->initializer_roots.data,
                    runtime->initializer_roots.len, &runtime->summaries,
                    global, target_index,
                    global->has_static_initializer
                        || (thread_global->has_comptime_value
                            && thread_global->comptime_value),
                    &runtime_succeeds, &entry_state);
                bool diagnosed = false;
                for (size_t f = 0; ok && runtime_succeeds
                     && f < runtime->entry_functions.len; f++) {
                    ir_func_t *function = runtime->entry_functions.data[f];
                    uint8_t success, failure;
                    bool reads_uninitialized;
                    ir_instr_t *bad_instruction = nullptr;
                    ok = target && effect_analyze_target_function(function,
                        target, target_index, &runtime->summaries,
                        &read_sites, entry_state,
                        &reads_uninitialized, &bad_instruction,
                        &success, &failure, nullptr, nullptr);
                    if (!ok || !reads_uninitialized) continue;
                    for (size_t r = 0; r < read_sites.len; r++) {
                        n00bb_thread_read_site_t *read = &read_sites.data[r];
                        if (read->function != function
                            || read->instruction != bad_instruction) continue;
                        core_buffer_t message = {0};
                        core_buffer_printf(&message,
                            "runtime function may read thread-local global '%.*s' before a guaranteed initialization",
                            (int)thread_global->name.u8_bytes,
                            thread_global->name.data);
                        core_diagnostics_report_at(
                            core_translation_unit_diagnostics(read->unit),
                            read->unit, CORE_DIAG_ERROR,
                            CORE_STRING_STATIC("N00BB-L0044"),
                            core_string_borrow(message.data, message.byte_len),
                            read->location);
                        core_buffer_release(&message);
                        diagnosed = true;
                        break;
                    }
                    if (!diagnosed) ok = false;
                }
                core_list_free(read_sites);
                if (!ok) return false;
            }
        }
    }
    return true;
}







enum {
    N00BB_BINDING_AVAILABLE = 1u << 0,
    N00BB_BINDING_MOVED = 1u << 1,
    N00BB_OLD_ALIAS_STALE = 1u << 2,
};

#define N00BB_BINDING_STATE_MASK \
    (N00BB_BINDING_AVAILABLE | N00BB_BINDING_MOVED)

static ir_value_t *
n00bb_consume_identity(ir_instr_t *instruction)
{
    if (!instruction || instruction->op != IR_OP_CONSUME
        || !instruction->n_operands) return nullptr;
    return ir_operand_value(&instruction->operands[
        instruction->n_operands > 1 ? 1 : 0]);
}

static bool
n00bb_instruction_initializes_binding(ir_instr_t *instruction,
                                      ir_value_t *binding)
{
    if (!instruction || !binding) return false;
    if (binding->kind == IR_VAL_LOCAL && binding->local
        && instruction->dest_kind == IR_DEST_LOCAL
        && instruction->dest_local == binding->local)
        return true;
    if ((instruction->op == IR_OP_STORE
            || instruction->op == IR_OP_GLOBAL_STORE)
        && instruction->n_operands
        && ir_operand_value(&instruction->operands[0]) == binding)
        return true;
    return false;
}

static bool
n00bb_instruction_reads_binding(ir_instr_t *instruction,
                                ir_value_t *binding)
{
    if (!instruction || !binding) return false;
    bool address_binding = binding->kind == IR_VAL_INSTR
        && th_ind_referent(binding->type);
    if (address_binding)
        return instruction->op == IR_OP_LOAD && instruction->n_operands
            && ir_operand_value(&instruction->operands[0]) == binding;
    for (uint32_t i = 0; i < instruction->n_operands; i++) {
        if (ir_operand_value(&instruction->operands[i]) != binding) continue;
        if (instruction->op == IR_OP_CONSUME
            && i + 1 == instruction->n_operands) continue;
        if ((instruction->op == IR_OP_STORE
                || instruction->op == IR_OP_GLOBAL_STORE)
            && i == 0) continue;
        if (instruction->op == IR_OP_ADDRESS_OF) continue;
        return true;
    }
    return false;
}

static ir_value_t *
n00bb_symbol_binding_in_function(parse_sym_entry_t *symbol,
                                 ir_func_t *function)
{
    if (!symbol || !function) return nullptr;
    for (size_t i = 0; i < symbol->function_binding_count; i++) {
        const parse_sym_function_binding_t *binding =
            &symbol->function_bindings[i];
        if (binding->phase == PARSE_SYM_PHASE_RUNTIME
            && binding->function == function) return binding->binding;
    }
    return nullptr;
}

enum {
    N00BB_ALIAS_READ_STORAGE = 1u << 0,
    N00BB_ALIAS_READ_OLD_VALUE = 1u << 1,
};

static bool
n00bb_alias_targets_whole_binding(ir_value_t *alias_binding,
                                  ir_value_t *binding,
                                  ir_func_t *function)
{
    if (!alias_binding || !binding || !function
        || alias_binding->kind != IR_VAL_LOCAL || !alias_binding->local)
        return false;
    bool saw_assignment = false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction || instruction->dest_kind != IR_DEST_LOCAL
                || instruction->dest_local != alias_binding->local
                || !instruction->n_operands) continue;
            saw_assignment = true;
            ir_value_t *source = ir_operand_value(&instruction->operands[0]);
            ir_instr_t *definition = source && source->kind == IR_VAL_INSTR
                ? source->def : nullptr;
            if (!definition || definition->op != IR_OP_ADDRESS_OF
                || !definition->n_operands
                || ir_operand_value(&definition->operands[0]) != binding)
                return false;
        }
    }
    return saw_assignment;
}

static uint8_t
n00bb_instruction_binding_alias_reads(ir_instr_t *instruction,
                                      parse_sym_entry_t *symbol,
                                      ir_value_t *binding,
                                      ir_func_t *function)
{
    if (!instruction || !symbol || !binding || !function) return 0;
    uint8_t reads = 0;
    for (size_t i = 0; i < symbol->aliased_by_count; i++) {
        parse_sym_entry_t *alias = symbol->aliased_by[i].other;
        ir_value_t *alias_binding = n00bb_symbol_binding_in_function(
            alias, function);
        if (alias_binding
            && n00bb_instruction_reads_binding(instruction, alias_binding)) {
            reads |= n00bb_alias_targets_whole_binding(
                alias_binding, binding, function)
                ? N00BB_ALIAS_READ_STORAGE : N00BB_ALIAS_READ_OLD_VALUE;
        }
    }
    return reads;
}

static uint8_t
n00bb_binding_transfer(ir_block_t *block, uint8_t state,
                       ir_value_t *binding, parse_sym_entry_t *symbol,
                       ir_instr_t **bad,
                       bool *bad_consume)
{
    if (bad) *bad = nullptr;
    if (bad_consume) *bad_consume = false;
    for (uint32_t i = 0; block && i < block->n_instrs; i++) {
        ir_instr_t *instruction = block->instrs[i];
        bool consumes = n00bb_consume_identity(instruction) == binding;
        uint8_t alias_reads = n00bb_instruction_binding_alias_reads(
            instruction, symbol, binding, block->owner);
        bool unavailable = (state & N00BB_BINDING_STATE_MASK)
            != N00BB_BINDING_AVAILABLE;
        if ((n00bb_instruction_reads_binding(instruction, binding)
             || (alias_reads & N00BB_ALIAS_READ_STORAGE))
            && unavailable && bad && !*bad)
            *bad = instruction;
        if ((alias_reads & N00BB_ALIAS_READ_OLD_VALUE)
            && (state & N00BB_OLD_ALIAS_STALE) && bad && !*bad)
            *bad = instruction;
        if (consumes) {
            if (unavailable && bad && !*bad) {
                *bad = instruction;
                if (bad_consume) *bad_consume = true;
            }
            state = N00BB_BINDING_MOVED | N00BB_OLD_ALIAS_STALE;
        }
        if (n00bb_instruction_initializes_binding(instruction, binding))
            state = (state & N00BB_OLD_ALIAS_STALE)
                | N00BB_BINDING_AVAILABLE;
    }
    return state;
}

static bool
n00bb_validate_consumed_binding(parse_sym_entry_t *symbol,
                                ir_func_t *function, ir_value_t *binding,
                                ir_diags_t *diagnostics,
                                const char *binding_name)
{
    if (!function || !binding || !diagnostics || !function->n_blocks)
        return false;
    bool has_consume = false;
    for (uint32_t b = 0; b < function->n_blocks && !has_consume; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++)
            has_consume |= n00bb_consume_identity(
                function->blocks[b]->instrs[i]) == binding;
    if (!has_consume) return true;

    ir_block_t *entry = ir_block_entry(function);
    ir_doms_t *doms = ir_doms_of(function);
    uint8_t *incoming = calloc(function->n_blocks, sizeof(*incoming));
    if (!entry || !doms || !incoming || entry->index >= function->n_blocks) {
        free(incoming);
        return false;
    }
    incoming[entry->index] = binding->kind == IR_VAL_LOCAL
        ? N00BB_BINDING_MOVED : N00BB_BINDING_AVAILABLE;
    bool changed;
    do {
        changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            if (!block || !ir_block_reachable(doms, block)
                || !incoming[block->index]) continue;
            uint8_t outgoing = n00bb_binding_transfer(
                block, incoming[block->index], binding, symbol, nullptr,
                nullptr);
            ir_instr_t *terminator = ir_block_terminator(block);
            for (uint32_t s = 0; terminator
                 && s < ir_succ_count(terminator); s++) {
                ir_block_t *successor = ir_succ_at(terminator, s);
                if (!successor || successor->index >= function->n_blocks)
                    continue;
                uint8_t next = incoming[successor->index] | outgoing;
                changed |= next != incoming[successor->index];
                incoming[successor->index] = next;
            }
        }
    } while (changed);

    bool ok = true;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (!block || !ir_block_reachable(doms, block)
            || !incoming[block->index]) continue;
        ir_instr_t *bad = nullptr;
        bool bad_consume = false;
        (void)n00bb_binding_transfer(block, incoming[block->index], binding,
                                     symbol, &bad, &bad_consume);
        if (!bad) continue;
        ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
            IR_DIAG_TYPE_ARGS, bad->span,
            bad_consume ? "this consume" : "this use");
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            bad_consume
                ? "binding '%s' is consumed more than once or is not available on every incoming path"
                : "binding '%s' is used after consume or is not available on every incoming path",
            binding_name ? binding_name : binding->name);
        ir_diag_message(diagnostic, diagnostics, message.data);
        core_buffer_release(&message);
        ok = false;
    }
    free(incoming);
    return ok;
}

static bool
n00bb_validate_consume_states(n00bb_link_analysis_t *analysis,
                              core_list_t(core_translation_unit_ptr_t) *units)
{
    bool ok = analysis && units;
    for (size_t u = 0; ok && u < units->len; u++) {
        core_translation_unit_t *unit = units->data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        ir_diags_t *diagnostics = ir_diags_new_attached(
            analysis->ir_context, nullptr,
            core_translation_unit_diagnostics(unit), unit);
        if (!symtab || !diagnostics) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            if (!scope_in_value_namespace(scope)) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_VARIABLE
                    && symbol->kind != PARSE_SYM_PARAM) continue;
                for (size_t i = 0; i < symbol->function_binding_count; i++) {
                    const parse_sym_function_binding_t *binding =
                        &symbol->function_bindings[i];
                    if (!n00bb_validate_consumed_binding(
                            symbol, binding->function, binding->binding,
                            diagnostics,
                            symbol->name.data)) ok = false;
                }
            }
        }
        if (ir_diags_failed(diagnostics)) ok = false;
    }
    return ok;
}

bool
lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                     n00bb_link_analysis_t *analysis,
                     ir_ctx_t *ir_context,
                     core_diagnostics_t *diagnostics,
                     bool dump_ir,
                     bool dump_control_flow, bool dump_pdg,
                     bool dump_layout)
{
    double profile_start = n00bb_profile_seconds();
    if (analysis) analysis->ir_context = ir_context;
    core_link_contribution_view_t semantic_analysis;
    core_link_contribution_view_t bootstrap_analysis;
    core_list_t(core_translation_unit_ptr_t) units =
        core_list_new(core_translation_unit_ptr_t);
    bool ok = link && core_link_unit_graph_is_finalized(link)
        && analysis
        && core_link_unit_require_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.semantic-ready"), 1,
            &semantic_analysis)
        && core_link_unit_require_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.bootstrap-pipeline-ran"), 1,
            &bootstrap_analysis)
        && core_link_unit_run_stage(link, N00BB_LINK_STAGE_IR_BINDINGS)
        && core_link_unit_collect(link);
    for (size_t i = 0; ok
         && i < core_link_unit_translation_unit_count(link); i++)
        core_list_push(units,
            core_link_unit_translation_unit(link, i));
    ok = ok && units.len;
    for (size_t i = 0; ok && i < units.len; i++) {
        n00bb_tu_state_t *state = tu_state(units.data[i]);
        if (!state) ok = false;
        else {
            state->link = link;
            state->link_analysis = analysis;
        }
    }
    ir_lowerer_t *lowerer = ir_lowerer_new(ir_context);
    ok = ok && lowerer
        && ir_lowerer_register(lowerer, "binary", n00bb_lower_binary,
                               nullptr)
        && ir_lowerer_register(lowerer, "nil_literal",
                               n00bb_lower_nil_literal, nullptr)
        && ir_lowerer_register(lowerer, "cast", n00bb_lower_cast, nullptr)
        && ir_lowerer_register(lowerer, "unsafe_cast", n00bb_lower_cast,
                               nullptr)
        && ir_lowerer_register(lowerer, "typeof", n00bb_lower_typeof,
                               nullptr)
        && ir_lowerer_register(lowerer, "bool_not", n00bb_lower_bool_not,
                               nullptr)
        && ir_lowerer_register(lowerer, "complement",
                               n00bb_lower_complement, nullptr)
        && ir_lowerer_register(lowerer, "negate", n00bb_lower_negate,
                               nullptr)
        && ir_lowerer_register(lowerer, "string_literal",
                               n00bb_lower_string_literal, nullptr)
        && ir_lowerer_register(lowerer, "char_literal",
                               n00bb_lower_char_literal, nullptr)
        && ir_lowerer_register(lowerer, "sequence_literal",
                               n00bb_lower_sequence_literal, nullptr)
        && ir_lowerer_register(lowerer, "brace_literal",
                               n00bb_lower_brace_literal, nullptr)
        && ir_lowerer_register(lowerer, "array_new",
                               n00bb_lower_array_new, nullptr)
        && ir_lowerer_register(lowerer, "callback_create",
                               n00bb_lower_callback_create, nullptr)
        && ir_lowerer_register(lowerer, "compound_assign",
                               n00bb_lower_compound_assignment, nullptr)
        && ir_lowerer_register(lowerer, "module_member",
                               n00bb_lower_module_member, nullptr)
        && ir_lowerer_register(lowerer, "return", n00bb_lower_return,
                               nullptr)
        && ir_lowerer_register(lowerer, "call", n00bb_lower_call, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_BRANCH,
                                     n00bb_lower_branch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_LOOP,
                                     n00bb_lower_loop_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SWITCH,
                                     n00bb_lower_switch_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_JUMP,
                                     n00bb_lower_jump_annotation, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_INFER,
                                     n00bb_lower_variant_payload, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_CALL,
                                     n00bb_mark_call_lvalues, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_EMIT,
                                     n00bb_mark_builtin_index_receiver,
                                     nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_EMIT,
                                     n00bb_skip_i128_min_magnitude, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_DECLARES,
                                     n00bb_lower_declare_local, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_ASSIGNS,
                                     n00bb_check_assignment, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_VARREF,
                                     n00bb_check_phase_read, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_VARREF,
                                     n00bb_initialize_default_reference,
                                     nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_VARREF,
                                     n00bb_lower_atomic_reference, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_branch_body, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_typecase_scope, nullptr)
        && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                     n00bb_lower_try_scope, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "simple-lit", n00bb_lower_static_litmod, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        "list-lit", n00bb_lower_static_litmod, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        "dict-or-set-lit", n00bb_lower_static_litmod, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        "embed-lit", n00bb_lower_static_litmod, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
        "tuple-or-paren", n00bb_lower_static_litmod, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "new-expr", n00bb_mark_constructor_lvalues, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "assert-stmt", n00bb_lower_assert_statement, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "expression", n00bb_capture_compound_target, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "case-expr-item", n00bb_prune_enum_case, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "and-rhs", n00bb_lower_branch_body, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            "or-rhs", n00bb_lower_branch_body, nullptr);
    ok = ok && ir_lowerer_register_nt_visit(lowerer,
        "expr-comma-list", n00bb_lower_tuple_parts, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            "named-field-list", n00bb_lower_named_field_parts, nullptr)
        && ir_lowerer_register_nt_visit(lowerer,
            "tuple-or-paren", n00bb_lower_tuple_literal, nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_SCOPE_OPEN,
                                         n00bb_lower_with_scope, nullptr);
    ok = ok && ir_lowerer_register_visit(lowerer, PARSE_ANNOT_OPERATOR,
                                         n00bb_lower_operator_annotation,
                                         nullptr);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-register %.3f\n",
            n00bb_profile_seconds() - profile_start);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type
                    || !function_symbol_at(grammar, symbol->decl_node))
                    continue;
                ok = lower_function_body(lowerer, grammar, unit, symbol,
                                         ir_context, &units);
            }
        }
        core_list_free(scopes);
    }
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-functions %.3f\n",
            n00bb_profile_seconds() - profile_start);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; ok && s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 ok && symbol; symbol = symbol->next_in_scope) {
                if (!context_symbol(grammar, symbol)) continue;
                static const char *hooks[] = {
                    "context-enter", "context-exit", "context-error",
                    "context-ensure",
                };
                for (size_t h = 0; ok
                    && h < sizeof(hooks) / sizeof(hooks[0]); h++) {
                    const char *role = n00bb_context_hook_role(hooks[h]);
                    parse_tree_t *hook = symbol->decl_node && role
                        ? parse_sym_binding(symbol,
                            core_string_borrow(role, strlen(role))) : nullptr;
                    if (hook)
                        ok = lower_context_hook(lowerer, grammar, unit,
                            symbol, hook, ir_context, &units);
                }
                core_list_t(parse_sym_entry_ptr_t) parameters =
                    core_list_new(parse_sym_entry_ptr_t);
                parse_sym_entry_t *rest = nullptr;
                collect_ir_parameters(grammar,
                    child_nt(grammar, symbol->decl_node, "param-decl"),
                    &parameters, &rest);
                for (size_t p = 0; ok && p < parameters.len; p++)
                    if (parameters.data[p]->is_optional)
                        ok = lower_context_default(lowerer, grammar, unit,
                            symbol, p, ir_context, &units);
                core_list_free(parameters);
                (void)rest;
            }
        }
        core_list_free(scopes);
    }
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-contexts %.3f\n",
            n00bb_profile_seconds() - profile_start);
    core_list_t(core_translation_unit_ptr_t) init_order =
        core_list_new(core_translation_unit_ptr_t);
    ok = ok && n00bb_collect_initializer_order(link, &init_order);
    core_link_contribution_view_t initializer_dependencies;
    ok = ok && core_link_unit_require_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.initializer-dependencies"), 1,
        &initializer_dependencies);
    ok = ok && n00bb_generate_value_methods(analysis)
        && lower_ordered_module_initializers(lowerer, grammar, &units,
            ir_context, &init_order)
        && publish_declared_aggregate_layouts(analysis)
        && publish_interned_variant_layouts(analysis)
        && build_reachable_functions(analysis, &init_order, diagnostics);

    if (ok) ok = diagnose_unused_link_symbols(grammar, analysis);
    n00bb_runtime_init_analysis_t runtime_init = {0};
    bool analyze_runtime_init = ok
        && n00bb_compile_phase() == PARSE_SYM_PHASE_RUNTIME;
    if (analyze_runtime_init)
        ok = runtime_init_analysis_build(link, grammar, analysis,
            &init_order, &runtime_init);
    if (ok && analyze_runtime_init)
        ok = validate_uninitialized_global_reads(link, analysis,
            &runtime_init, &init_order);
    if (ok && analyze_runtime_init)
        ok = validate_thread_function_reads(link, analysis, &runtime_init,
                                            true);
    if (analyze_runtime_init) runtime_init_analysis_release(&runtime_init);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-initializers %.3f\n",
            n00bb_profile_seconds() - profile_start);

    if (ok && dump_ir) {
        for (size_t i = 0; i < units.len; i++) {
            char *text = ir_module_print(tu_state(units.data[i])->ir_module);
            if (!text) { ok = false; break; }
            fputs(text, stdout);
            core_free(text);
        }
    }

    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr,
            core_translation_unit_diagnostics(unit), unit);
        ir_props_t properties = ir_verify(module, diagnostics);
        ir_props_t verified = IR_PROP_PARSED | IR_PROP_TYPED
            | IR_PROP_VERIFIED;
        ok = !ir_diags_failed(diagnostics)
            && (properties & verified) == verified;
    }
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-verify %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok) {
        ir_policy_t *policy = ir_policy_new(ir_context);
        ok = policy
            && ir_policy_set(policy, "IR-W0027", IR_POLICY_DENY)
            && ir_policy_set(policy, "IR-W0030", IR_POLICY_ALLOW)
            && ir_policy_set(policy, "IR-W0031", IR_POLICY_ALLOW);
        for (size_t i = 0; ok && i < units.len; i++) {
            core_translation_unit_t *unit = units.data[i];
            ir_diags_t *diagnostics = ir_diags_new_attached(
                ir_context, policy,
                core_translation_unit_diagnostics(unit), unit);
            ok = ir_analyze(tu_state(unit)->ir_module, diagnostics);
        }
    }
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-analyze %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok)
        ok = n00bb_validate_consume_states(analysis, &units);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-consume %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok)
        ok = core_link_unit_run_stage(link,
                                     N00BB_LINK_STAGE_CFG_LIVENESS);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-cfg-liveness %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok)
        ok = core_link_unit_run_stage(link,
                N00BB_LINK_STAGE_CLEANUP_OBLIGATIONS);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-cleanup %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok)
        ok = core_link_unit_run_stage(link,
                N00BB_LINK_STAGE_STORAGE_LIFETIMES);
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-lifetime %.3f\n",
            n00bb_profile_seconds() - profile_start);
    static const ir_pass_t *const lowering_passes[] = {
        &ir_pass_promote,
        &ir_pass_structure,
        &ir_pass_layout,
    };
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr,
            core_translation_unit_diagnostics(unit), unit);
        for (size_t p = 0; ok
             && p < sizeof(lowering_passes) / sizeof(lowering_passes[0]);
             p++) {
            ir_props_t unmet = 0;
            ir_pass_result_t result = ir_pass_run(lowering_passes[p],
                module, nullptr, &unmet, diagnostics);
            if (result != IR_PASS_OK) {
                if (result == IR_PASS_UNMET) {
                    ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                        IR_DIAG_VERIFY_TYPED, ir_module_span(module),
                        "this lowered module");
                    ir_diag_message(diagnostic, diagnostics,
                        "Lantern pass prerequisites are not satisfied");
                }
                ok = false;
            }
        }
    }
    if (getenv("N00BB_PROFILE_COMPILE"))
        fprintf(stderr, "PROFILE lower-passes %.3f\n",
            n00bb_profile_seconds() - profile_start);
    if (ok && dump_layout)
        for (size_t i = 0; i < units.len; i++)
            dump_layout_for_unit(grammar, units.data[i]);
    for (size_t i = 0; ok && i < units.len; i++) {
        core_translation_unit_t *unit = units.data[i];
        ir_module_t *module = tu_state(unit)->ir_module;
        ir_diags_t *diagnostics = ir_diags_new_attached(
            ir_context, nullptr, core_translation_unit_diagnostics(unit), unit);
        for (size_t f = 0; ok && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (function->n_blocks && !ir_pdg_of(function)) {
                ir_diag_t *diagnostic = ir_diag_begin(diagnostics,
                    IR_DIAG_INTERNAL, ir_module_span(module),
                    "this lowered function");
                ir_diag_message(diagnostic, diagnostics,
                    "Lantern could not construct its dependence graph");
                ok = false;
            }
        }
    }

    if (ok && !build_n00bb_execution_graph(analysis, &init_order)) {
        core_diagnostics_report(
            core_translation_unit_diagnostics(units.data[0]), units.data[0],
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0048"),
            CORE_STRING_STATIC(
                "could not construct the execution graph from finalized initializer control flow"));
        ok = false;
    }
    core_list_free(init_order);

    for (size_t i = 0; i < units.len; i++) {
        if (core_diagnostics_failed(
                core_translation_unit_diagnostics(units.data[i])))
            ok = false;
    }
    if (link && core_link_unit_has_fatal_diagnostics(link)) ok = false;

    if (ok) {
        for (size_t i = 0; i < units.len; i++) {
            if (dump_control_flow || dump_pdg)
                dump_ir_analysis(tu_state(units.data[i])->ir_module,
                                 dump_control_flow, dump_pdg);
        }
        if (dump_control_flow) {
            char *text = ir_exec_graph_print(analysis->execution_graph);
            if (!text) {
                core_diagnostics_report(
                    core_translation_unit_diagnostics(units.data[0]),
                    units.data[0], CORE_DIAG_FATAL,
                    CORE_STRING_STATIC("N00BB-L0048"),
                    CORE_STRING_STATIC(
                        "the finalized execution graph failed validation while dumping"));
                ok = false;
            }
            else {
                fputs(text, stdout);
                core_free(text);
                for (size_t f = 0; f < analysis->reachable_functions.len;
                     f++) {
                    ir_func_t *function = analysis->reachable_functions.data[f];
                    printf("reachable @%s.%s\n",
                        ir_module_name(function->owner), function->name);
                }
            }
        }
    }
    core_list_free(units);
    return ok;
}

static bool
n00bb_register_callback_representation(ir_module_t *module, th_type *type)
{
    if (!module || !type) return false;
    if (ir_type_definition(module, type)) return true;
    const char *name = nullptr;
    size_t name_length = 0;
    size_t arguments = 0;
    if (!th_named_info(th_resolve(type), &name, &name_length, nullptr,
                       &arguments) || !arguments) return false;
    char *base_name = core_strndup(name, name_length);
    th_type *base = base_name
        ? th_name(ir_module_types(module), base_name) : nullptr;
    core_free(base_name);
    th_type *definition = base ? ir_type_definition(module, base) : nullptr;
    return definition && ir_type_define(module, type, definition);
}




static bool
n00bb_static_value_to_ir_initializer(const n00bb_static_value_t *value,
                                    th_type *type,
                                    n00bb_tu_state_t *state,
                                    ir_global_init_value_t *out,
                                    size_t depth)
{
    if (!value || !type || !state || !out) return false;
    type = th_resolve(type);
    if (!type) return false;
    if (n00bb_atomic_element_type(type))
        return n00bb_static_value_to_ir_initializer(value,
            n00bb_atomic_element_type(type), state, out, depth + 1);
    *out = (ir_global_init_value_t){0};
    switch (value->kind) {
    case N00BB_STATIC_VALUE_BOOL:
        out->kind = IR_GLOBAL_INIT_BOOL;
        out->boolean = value->as.boolean;
        return true;
    case N00BB_STATIC_VALUE_INT:
        out->kind = IR_GLOBAL_INIT_INT;
        out->integer.neg = value->as.integer < 0;
        out->integer.mag = value->as.integer < 0
            ? (ir_uint_t)(-(value->as.integer + 1)) + 1
            : (ir_uint_t)value->as.integer;
        return true;
    case N00BB_STATIC_VALUE_FLOAT:
        out->kind = IR_GLOBAL_INIT_FLOAT;
        out->floating = value->as.real;
        return true;
    case N00BB_STATIC_VALUE_STRING:
        out->kind = IR_GLOBAL_INIT_STRING;
        out->string.bytes = value->as.string.data;
        out->string.length = value->as.string.u8_bytes;
        return true;
    case N00BB_STATIC_VALUE_NIL:
        if (!th_is_nil(type)
            && !n00bb_option_type_element(type, nullptr)) return false;
        out->kind = IR_GLOBAL_INIT_NIL;
        return true;
    case N00BB_STATIC_VALUE_VARIANT: {
        bool variant = false;
        th_type *shape = static_record_shape(state, type);
        if (!th_named_info(type, nullptr, nullptr, &variant, nullptr)
            || !variant || !shape || th_kind_of(shape) != TH_RECORD
            || !value->as.variant.payload) return false;
        for (size_t i = 0; i < th_record_len(shape); i++) {
            const th_member *arm = th_record_member(shape, i);
            if (!arm || !core_string_eq(value->as.variant.arm,
                                        core_string_from_cstr(arm->name)))
                continue;
            ir_global_init_value_t *payload = core_arena_alloc(
                &state->semantic_values, sizeof(*payload));
            if (!payload || !n00bb_static_value_to_ir_initializer(
                    value->as.variant.payload, arm->type, state, payload,
                    depth + 1)) return false;
            payload->member = arm->name;
            out->kind = IR_GLOBAL_INIT_VARIANT;
            out->aggregate.items = payload;
            out->aggregate.count = 1;
            return true;
        }
        return false;
    }
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD:
        break;
    }

    size_t count = value->as.aggregate.count;
    if (count && !value->as.aggregate.items) return false;
    ir_global_init_value_t *items = count
        ? core_arena_alloc(&state->semantic_values, count * sizeof(*items))
        : nullptr;
    if (count && !items) return false;
    if (count) memset(items, 0, count * sizeof(*items));
    out->aggregate.items = items;
    out->aggregate.count = count;

    if (value->kind == N00BB_STATIC_VALUE_SEQUENCE) {
        th_type *element = static_sequence_element_type(type);
        if (!element) return false;
        out->kind = IR_GLOBAL_INIT_SEQUENCE;
        for (size_t i = 0; i < count; i++)
            if (!n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], element, state,
                    &items[i], depth + 1)) return false;
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_TUPLE) {
        if (!th_builtin_applied(type, TH_BUILTIN_TUPLE, count)) return false;
        out->kind = IR_GLOBAL_INIT_TUPLE;
        for (size_t i = 0; i < count; i++) {
            th_type *element = static_tuple_element_type(type, i, nullptr);
            if (!element || !n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], element, state,
                    &items[i], depth + 1)) return false;
        }
        return true;
    }
    if (value->kind == N00BB_STATIC_VALUE_RECORD) {
        th_type *shape = static_record_shape(state, type);
        if (!shape || th_kind_of(shape) != TH_RECORD
            || count != th_record_len(shape)) return false;
        out->kind = IR_GLOBAL_INIT_RECORD;
        for (size_t i = 0; i < count; i++) {
            const th_member *member = th_record_member(shape, i);
            if (!member || !n00bb_static_value_to_ir_initializer(
                    &value->as.aggregate.items[i], member->type, state,
                    &items[i], depth + 1)) return false;
            items[i].member = member->name;
        }
        return true;
    }
    return false;
}
