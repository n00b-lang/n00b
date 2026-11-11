#ifndef N00BB_COMPILER_LOWER_EMIT_H
#define N00BB_COMPILER_LOWER_EMIT_H

#include "compiler_link.h"
#include "lower/walk.h"

#define N00BB_OPEN_CAST_KEY CORE_STRING_STATIC("n00bb.open-cast")
#define N00BB_RECORD_LITERAL_KEY CORE_STRING_STATIC("n00bb.record-literal")
#define N00BB_SWITCH_COPY_KEY CORE_STRING_STATIC("n00bb.switch.copy")
#define N00BB_WRITE_BACK_KEY CORE_STRING_STATIC("n00bb.write-back")
#define N00BB_DECLARATION_KEY CORE_STRING_STATIC("n00bb.declaration")
#define N00BB_REFERENCE_ESCAPES_KEY CORE_STRING_STATIC("n00bb.reference-escapes")
#define N00BB_CALLBACK_SETUP_KEY CORE_STRING_STATIC("n00bb.callback-setup")
#define N00BB_TUPLE_INDEX_KEY CORE_STRING_STATIC("n00bb.tuple-index")
#define N00BB_COMPOUND_KEY CORE_STRING_STATIC("n00bb.compound")

typedef struct n00bb_lower_context n00bb_lower_context_t;

ir_value_t *n00bb_emit_hook_call(ir_lower_context_t *context,
                                 const th_env_method_t *method,
                                 ir_value_t *argument, ir_span_t span);
core_string_t describe_parameter_target(th_mode mode, parse_sym_entry_t *callee,
                                        core_string_t parameter);

n00bb_lower_context_t *n00bb_solved_lower_new(n00bb_link_analysis_t *analysis,
                                              parse_grammar_t *grammar,
                                              core_translation_unit_t *unit);
ir_diags_t *attach_ir_diags(ir_ctx_t *ir_context, ir_policy_t *policy,
                            core_translation_unit_t *unit,
                            const char *policy_name);
bool n00bb_finalize_temporaries(n00bb_link_analysis_t *analysis,
                                parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                ir_func_t *function);
parse_sym_entry_t *n00bb_comparison_member(n00bb_link_analysis_t *analysis,
                                           th_type *type, core_string_t name);

typedef struct {
    th_type *type;
    parse_sym_entry_t *members[4];
} n00bb_comparison_t;

parse_sym_entry_t *const *n00bb_comparison_members(
    n00bb_link_analysis_t *analysis, n00bb_comparison_t *cache,
    th_type *type);
ir_value_t *n00bb_reference_hook_address(ir_lower_context_t *context,
                                         ir_value_t *reference, bool writable,
                                         ir_span_t span);
ir_value_t *n00bb_reference_placeholder(ir_lower_context_t *context,
                                        ir_value_t *reference,
                                        th_type *address_type, bool writable,
                                        ir_span_t span);
ir_value_t *n00bb_emit_enum_member(ir_lower_context_t *context,
                                   parse_sym_entry_t *member, ir_span_t span);
ir_value_t *n00bb_emit_binary_ir(ir_lower_context_t *context, ir_op_t op,
                                 th_type *result_type, ir_value_t *left,
                                 ir_value_t *right, ir_span_t span);
bool n00bb_place_named_arguments(ir_instr_t *in, ir_func_t *function);
ir_value_t *n00bb_lower_shift_count(ir_lower_context_t *context,
                                    parse_tree_t *node, ir_value_t *value,
                                    th_type *receiver, ir_span_t span);
ir_value_t *n00bb_emit_atomic_wrap(ir_lower_context_t *context,
                                   ir_value_t *value, th_type *atomic_type,
                                   ir_span_t span);
ir_value_t *n00bb_union_filler(ir_lower_context_t *context, th_type *type,
                               ir_span_t span);
ir_value_t *n00bb_emit_flagset_operator(ir_lower_context_t *context,
                                        core_string_t method, th_type *result,
                                        ir_value_t *left, ir_value_t *right,
                                        ir_span_t span);
ir_lower_result_t n00bb_emit_untyped_const(ir_lower_context_t *context,
                                           __int128 value, ir_span_t span);
ir_value_t *n00bb_emit_three_way(ir_lower_context_t *context, th_type *type,
                                 parse_sym_entry_t *const members[4],
                                 ir_value_t *left, ir_value_t *right,
                                 ir_span_t span);
bool n00bb_primitive_operator(ir_class_t class, core_string_t method);
ir_value_t *n00bb_emit_primitive(ir_lower_context_t *context,
                                 core_string_t method, th_type *result,
                                 ir_value_t *left, ir_value_t *right,
                                 ir_span_t span);
ir_lower_result_t n00bb_emit_cast(ir_lower_context_t *context,
                                  parse_grammar_t *grammar,
                                  parse_tree_t *operand_node,
                                  ir_value_t *const *operands,
                                  th_type *target, bool unsafe_cast,
                                  ir_span_t span);
bool n00bb_emit_finalize_value(ir_lower_context_t *context,
                               ir_value_t *address, th_type *type,
                               ir_span_t span, size_t depth);
ir_value_t *n00bb_emit_indirect_read(ir_lower_context_t *context,
                                     ir_value_t *address, th_type *type,
                                     ir_span_t span);
bool n00bb_emit_jump(ir_lower_context_t *context, ir_block_t *target,
                     ir_span_t span);
ir_lower_result_t n00bb_emit_char_array(ir_lower_context_t *context,
                                        const uint8_t *bytes, size_t length,
                                        th_type *result_type, ir_span_t span);
ir_value_t *n00bb_emit_fresh_object(ir_lower_context_t *context,
                                    ir_value_t *storage,
                                    th_type *object_type, ir_span_t span);
ir_value_t *n00bb_object_from_fields(ir_lower_context_t *context,
                                     ir_value_t *fields,
                                     th_type *object_type, ir_span_t span);
ir_lower_result_t n00bb_emit_intrinsic(ir_lower_context_t *context,
                                       parse_sym_entry_t *symbol,
                                       ir_value_t *const *operands,
                                       size_t count, bool copyouts,
                                       th_type *call_result_type,
                                       ir_span_t span);
[[gnu::format(printf, 3, 4)]] core_string_t
n00bb_format_name(char *buffer, size_t size, const char *format, ...);
th_type *n00bb_ir_builtin_type(ir_module_t *module, core_string_t name);
ir_value_t *n00bb_materialize(ir_lower_context_t *context, parse_tree_t *node,
                              ir_value_t *value, th_type *target,
                              ir_span_t span);
ir_value_t *n00bb_reference_view(ir_lower_context_t *context,
                                 ir_value_t *value, ir_span_t span);
bool n00bb_set_reference_access(ir_ctx_t *context, ir_value_t *value,
                                th_type *type);
ir_value_t *n00bb_widen_integer_operand(ir_lower_context_t *context,
                                        ir_value_t *value,
                                        th_type *result_type, ir_span_t span);
bool n00bb_machine_promotes(ir_machine_t source, ir_machine_t destination);

#endif
