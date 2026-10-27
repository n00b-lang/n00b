#pragma once

#include <string.h>
#include <limits.h>

#include "lib/alloc.h"
#include "lib/arena.h"
#include "lib/dict.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "diag/diagnostics.h"
#include "compiler_visibility.h"
#include "parse/annot_walk.h"
#include "parse/tree_util.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "ir/libir.h"
#include "ir/interval.h"
#include "ir/summary.h"
#include "th/environment.h"
#include "builtin_types.h"
#include "compiler_module_literal.h"
#include "compiler_system.h"
#include "signature_typevars.h"
#include "preprocessor.h"
#include "diagnostics.h"

#define N00BB_PARAMETER_FACTS_KEY \
    CORE_STRING_STATIC("n00bb.parameter-facts")

core_list_decl(th_param);
core_list_decl(th_trait_requirement);
typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);
typedef struct {
    uint32_t parameter_count;
    ir_shape_t **parameters;
} n00bb_callable_shapes_t;
typedef struct {
    th_trait_requirement *data;
    size_t len;
} n00bb_declared_trait_requirements_t;
typedef parse_tree_t *n00bb_tree_ptr_t;
core_list_decl(n00bb_tree_ptr_t);
typedef struct {
    size_t index;
    core_string_t name;
    bool positional;
} n00bb_loop_binding_projection_t;
void collect_for_bindings(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(parse_tree_ptr_t) *bindings);
bool measure_static_shape_prefix(ir_shape_t *shape,
                                 uint32_t flat_prefix_rank);
th_type *record_template_type(th_ctx *types, core_string_t name,
                              parse_sym_entry_t *symbol);
th_type *variant_template_type(th_ctx *types, core_string_t name,
                               parse_sym_entry_t *symbol);
th_type *n00bb_variant_arm_payload_type(th_ctx *types, th_type *variant,
                                        parse_sym_entry_t *arm);
bool cache_record_layout_request(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *declaration);
bool cache_record_field_extents(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *declaration);
bool register_record_semantic_shape(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *declaration);
bool n00bb_assignment_target(parse_grammar_t *grammar, parse_tree_t *node,
                             bool *compound);
th_type *resolved_iteration_item_type(parse_grammar_t *grammar,
                                      core_translation_unit_t *unit,
                                      parse_tree_t *loop,
                                      th_type **cursor_type_out);
bool cache_iteration_plan(parse_grammar_t *grammar,
                          core_translation_unit_t *unit, parse_tree_t *loop,
                          th_type *source_type, th_type *cursor_type,
                          th_type *item_type);
bool resolve_for_binding_types(parse_grammar_t *grammar,
                               core_translation_unit_t *unit);
void collect_keyword_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                           core_list_t(n00bb_tree_ptr_t) *arguments);
core_string_t call_keyword_name(parse_tree_t *argument);
bool index_is_copyout_argument(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *index_node);
parse_sym_entry_t *formal_symbol_named(parse_grammar_t *grammar,
                                       parse_tree_t *formals,
                                       core_string_t name, size_t *index);
bool n00bb_builtin_callback_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_option_function(parse_sym_entry_t *symbol);
bool function_parameter_info(parse_grammar_t *grammar,
                             parse_sym_entry_t *symbol,
                             parse_tree_t *formals, size_t index,
                             parse_sym_entry_t **formal_out,
                             core_string_t *name,
                             uint8_t *flags, th_mode *mode);
bool function_parameter_named(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol,
                              parse_tree_t *formals, core_string_t name,
                              size_t *index);
parse_tree_t *callable_formal_parameters(parse_grammar_t *grammar,
                                         parse_sym_entry_t *symbol);
typedef enum {
    N00BB_LITERAL_STRING_SHAPE,
    N00BB_LITERAL_SEQUENCE_SHAPE,
    N00BB_LITERAL_BRACE_SHAPE,
} n00bb_literal_shape_t;

typedef enum {
    N00BB_LITERAL_ARRAY,
    N00BB_LITERAL_LIST,
    N00BB_LITERAL_DICT,
    N00BB_LITERAL_SET,
    N00BB_LITERAL_BINARY,
    N00BB_LITERAL_U32,
} n00bb_literal_handler_t;

typedef struct {
    core_string_t prefix;
    n00bb_literal_shape_t shape;
    n00bb_literal_handler_t handler;
    core_string_t canonical_name;
} n00bb_literal_registration_t;
const n00bb_literal_registration_t *n00bb_literal_handler(
    core_string_t prefix, n00bb_literal_shape_t shape);
bool n00bb_literal_prefix_registered(core_string_t prefix);
bool register_user_litmod(void *context, core_translation_unit_t *unit,
                          parse_tree_t *declaration,
                          core_string_t modifier, core_string_t kind);
bool register_user_litmod_alias(void *context, core_translation_unit_t *unit,
                                parse_tree_t *statement, core_string_t alias,
                                core_string_t target);
bool validate_litmod_aliases(core_translation_unit_t *unit);
th_type *n00bb_atomic_element_type(th_type *type);
bool n00bb_option_type_element(th_type *type, th_type **element);

typedef enum {
    N00BB_DESTINATION_EXPECTED,
    N00BB_DESTINATION_VARIABLE,
    N00BB_DESTINATION_FIELD,
    N00BB_DESTINATION_RETURN,
    N00BB_DESTINATION_ELEMENT,
    N00BB_DESTINATION_ARGUMENT,
} n00bb_destination_kind_t;

typedef struct {
    n00bb_destination_kind_t kind;
    core_string_t name;
    core_string_t owner;
    size_t position;
    parse_tree_t *declared_at;
    core_translation_unit_t *declared_in;
} n00bb_value_destination_t;

bool n00bb_value_needs_dereference(th_ctx *types, th_type *value,
                                   th_type *wanted);
core_string_t n00bb_value_spelling(core_translation_unit_t *unit,
                                   parse_tree_t *value);
void n00bb_report_value_type_mismatch(core_translation_unit_t *unit,
    parse_tree_t *value, th_type *actual, th_type *expected,
    const n00bb_value_destination_t *destination);
th_type *n00bb_operand_value_type(th_type *operand, bool comparison);
enum { N00BB_MAX_SIZED_BITS = sizeof(ir_uint_t) * CHAR_BIT };
typedef struct {
    core_string_t spelling;
    core_string_t assign_spelling;
    core_string_t trait;
    core_string_t method;
    core_string_t in_place_trait;
    core_string_t in_place;
    ir_op_t op;
    bool unsigned_only;
    bool shift;
} n00bb_binary_operator_t;
typedef struct {
    core_string_t name;
    ir_op_t op;
    bool shift;
    bool unary;
} n00bb_unsafe_arithmetic_t;
extern const n00bb_binary_operator_t n00bb_binary_operators[];
extern const size_t n00bb_binary_operator_count;
const n00bb_binary_operator_t *n00bb_binary_operator_spelled(
    core_string_t spelling);
const n00bb_binary_operator_t *n00bb_binary_operator_assigned(
    core_string_t spelling);
const n00bb_binary_operator_t *n00bb_binary_operator_method(
    core_string_t method);
bool n00bb_trap_integer_arithmetic(ir_instr_t *instruction, ir_class_t class);
bool binary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                       parse_tree_t *node,
                                       core_string_t *trait,
                                       core_string_t *method);
bool unary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                      parse_tree_t *node,
                                      core_string_t *trait,
                                      core_string_t *method);
parse_tree_t *member_access_node(parse_grammar_t *grammar, parse_tree_t *node);
parse_tree_t *referent_access_node(parse_grammar_t *grammar,
                                   parse_tree_t *node);
bool index_dispatch_for_node(parse_grammar_t *grammar, parse_tree_t *node,
                             core_string_t *trait, core_string_t *method);
parse_tree_t *n00bb_index_stored_value(parse_grammar_t *grammar,
                                       parse_tree_t *node);
bool n00bb_index_reference_storage(parse_grammar_t *grammar,
                                   parse_tree_t *node);
parse_tree_t *n00bb_index_expression_node(parse_grammar_t *grammar,
                                          parse_tree_t *node);
parse_tree_t *n00bb_addressable_root_lvalue(parse_grammar_t *grammar,
                                            parse_tree_t *target);
parse_sym_entry_t *direct_ref_target(parse_grammar_t *grammar,
                                     parse_tree_t *node);
core_string_t canonical_builtin_type_name(core_string_t name);
bool dotted_symbol_path_node(parse_grammar_t *grammar, parse_tree_t *node);
bool simple_member_chain_name(parse_grammar_t *grammar,
                              parse_tree_t *node, core_string_t *name);
int builtin_generic_type_arity(core_string_t name);
bool builtin_type_name(core_string_t name);
bool is_named_type_symbol(parse_grammar_t *grammar,
                          parse_sym_entry_t *symbol);
bool is_trait_type_symbol(parse_grammar_t *grammar,
                          parse_sym_entry_t *symbol);
th_type *trait_type_reference(parse_grammar_t *grammar,
                              core_translation_unit_t *use_unit,
                              core_translation_unit_t *declaration_unit,
                              parse_sym_entry_t *symbol, th_type *existing);
void resolve_local_type_reference(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *node, parse_tree_t *parent);
th_type *compose_type_syntax_node(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *node);
void propagate_resolved_type_reference(parse_grammar_t *grammar,
                                       core_translation_unit_t *unit,
                                       parse_tree_t *node, th_type *type);
void refresh_cached_type_path(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *node);
bool is_type_reference_node_at(parse_grammar_t *grammar, parse_tree_t *node,
                               parse_tree_t *parent);
bool is_type_reference_node(parse_grammar_t *grammar, parse_tree_t *node);
bool resolve_type_path_root_past_fields(parse_grammar_t *grammar,
                                        core_translation_unit_t *unit,
                                        parse_tree_t *node,
                                        parse_tree_t *parent);
bool inside_array_dimension(parse_grammar_t *grammar, parse_tree_t *node);
core_string_t bare_type_reference_name(parse_grammar_t *grammar,
                                       parse_tree_t *node);
bool bind_array_shape_alias_source(parse_grammar_t *grammar,
                                   parse_tree_t *type_reference,
                                   parse_sym_entry_t *alias);
th_type *unify_declared_symbol_type(th_ctx *types, parse_sym_entry_t *symbol,
                                    th_type *declared);
void bind_declared_type_from_node(parse_grammar_t *grammar, th_ctx *types,
                                  parse_tree_t *node);
parse_sym_entry_t *attached_symbol(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node);
bool n00bb_is_nil_literal(parse_grammar_t *grammar, parse_tree_t *node);

typedef struct n00bb_link_analysis_t n00bb_link_analysis_t;
typedef enum {
    N00BB_STATIC_VALUE_BOOL,
    N00BB_STATIC_VALUE_INT,
    N00BB_STATIC_VALUE_FLOAT,
    N00BB_STATIC_VALUE_STRING,
    N00BB_STATIC_VALUE_SEQUENCE,
    N00BB_STATIC_VALUE_TUPLE,
    N00BB_STATIC_VALUE_RECORD,
    N00BB_STATIC_VALUE_NIL,
    N00BB_STATIC_VALUE_VARIANT,
    N00BB_STATIC_VALUE_ADDRESS,
} n00bb_static_value_kind_t;

typedef struct n00bb_static_value {
    n00bb_static_value_kind_t kind;
    union {
        bool boolean;
        __int128 integer;
        double real;
        core_string_t string;
        struct {
            struct n00bb_static_value *items;
            size_t count;
        } aggregate;
        struct {
            core_string_t arm;
            struct n00bb_static_value *payload;
        } variant;
        parse_sym_entry_t *address;
    } as;
} n00bb_static_value_t;
bool n00bb_integer_type_promotes_to(th_type *source, th_type *target);
th_type *n00bb_common_integer_type(th_ctx *types, th_type *left,
                                   th_type *right);
uint64_t n00bb_shift_receiver_width(th_type *type);
bool n00bb_bitwise_type(th_type *type);
bool n00bb_unsigned_shift_count_type(th_type *type);
th_type *n00bb_shift_count_type(th_ctx *types, th_type *receiver);
bool n00bb_integer_range_fits_type(th_type *type, ir_interval_t range);
bool n00bb_parse_switch_integer(core_string_t text, ir_lit_t *literal);
parse_tree_t *n00bb_simple_lvalue(parse_tree_t *node);
parse_sym_entry_t *simple_expression_symbol(parse_tree_t *node);
ir_summary_t *n00bb_native_axiom(ir_summaries_t *summaries,
                                 const ir_func_t *function);
bool n00bb_native_calls_n00b(const ir_func_t *function);
parse_sym_entry_t *n00bb_atomic_member_lvalue_root(
    parse_grammar_t *grammar, core_translation_unit_t *unit,
    parse_tree_t *target);
bool n00bb_reference_target(parse_grammar_t *grammar, parse_tree_t *node);
parse_sym_entry_t *n00bb_atomic_load_symbol(parse_grammar_t *grammar,
                                            core_translation_unit_t *unit,
                                            parse_tree_t *node);
void n00bb_apply_atomic_load(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node);
parse_sym_entry_t *n00bb_member_chain_root(parse_grammar_t *grammar,
                                           core_translation_unit_t *unit,
                                           parse_tree_t *node);
bool n00bb_member_path(parse_grammar_t *grammar, core_translation_unit_t *unit,
                       parse_tree_t *node, parse_sym_entry_t **root_out,
                       core_buffer_t *path);
bool n00bb_integer_range_wrapper(parse_grammar_t *grammar, parse_tree_t *node);
bool n00bb_range_simple_integer_literal(parse_grammar_t *grammar,
                                        parse_tree_t *node, int64_t *value);
typedef struct {
    parse_sym_entry_t *call;
    parse_sym_entry_t *handler;
    parse_sym_entry_t *setup;
    th_type *state_type;
    bool stateful;
} n00bb_callback_contract_t;
typedef enum {
    N00BB_RETURN_SOURCE_NONE,
    N00BB_RETURN_SOURCE_SYMBOL,
    N00BB_RETURN_SOURCE_CALL,
    N00BB_RETURN_SOURCE_SLICE,
    N00BB_RETURN_SOURCE_PROJECTION,
    N00BB_RETURN_SOURCE_PROJECTION_REFERENCE,
} n00bb_return_source_kind_t;
typedef struct {
    n00bb_return_source_kind_t kind;
    parse_tree_t *node;
    parse_sym_entry_t *symbol;
} n00bb_return_source_t;
typedef enum {
    N00BB_ALIAS_SYMBOL,
    N00BB_ALIAS_REFERENCE,
    N00BB_ALIAS_CALL,
} n00bb_alias_edge_kind_t;
typedef struct {
    n00bb_alias_edge_kind_t kind;
    parse_tree_t *node;
    parse_sym_entry_t *symbol;
    uint64_t derived_at;
} n00bb_alias_edge_t;
typedef struct {
    n00bb_alias_edge_t *edges;
    size_t count;
} n00bb_alias_edges_t;
typedef struct {
    th_type **types;
    size_t count;
} n00bb_typecase_arm_types_t;
typedef struct n00bb_ordered_nodes_t n00bb_ordered_nodes_t;
typedef struct n00bb_iteration_plan_t n00bb_iteration_plan_t;
typedef struct n00bb_litmod_site_t n00bb_litmod_site_t;
typedef struct {
    n00bb_return_source_t *return_source;
    n00bb_alias_edges_t *alias_edges;
    n00bb_typecase_arm_types_t *typecase_arm_types;
    n00bb_ordered_nodes_t *ordered_nodes;
    n00bb_iteration_plan_t *iteration_plan;
    n00bb_litmod_site_t *literal_site;
    parse_tree_t *record_literal_origin;
    ir_interval_t integer_range;
    bool integer_range_known;
    size_t first_link_use_site;
    size_t last_link_use_site;
    bool static_literal;
    bool selected_when_body;
    bool unsigned_literal;
    bool shape_literal_pending;
    bool shape_literal_resolved;
    bool overload_conversion_warning_reported;
    bool tuple_index_known;
    bool tuple_index_reported;
    uint32_t tuple_index;
} n00bb_node_facts_t;
typedef core_translation_unit_t *core_translation_unit_ptr_t;
core_list_decl(core_translation_unit_ptr_t);
core_list_decl(th_arg);
typedef parse_scope_t *parse_scope_ptr_t;
core_list_decl(parse_scope_ptr_t);

typedef struct {
    parse_sym_entry_t *symbol;
    ir_interval_t range;
} n00bb_symbol_range_t;
core_list_decl(n00bb_symbol_range_t);

typedef struct {
    core_list_t(n00bb_symbol_range_t) entries;
    core_dict_t index;
} n00bb_range_table_t;

typedef struct {
    parse_tree_t *node;
    n00bb_range_table_t incoming;
    n00bb_range_table_t merged;
    bool has_branch;
    bool has_else;
    bool branch_active;
} n00bb_range_branch_frame_t;
core_list_decl(n00bb_range_branch_frame_t);

typedef struct {
    core_string_t modifier;
    core_string_t kind;
    parse_tree_t *declaration;
    parse_sym_entry_t *function;
    core_diag_location_t location;
    core_string_t module;
} n00bb_user_litmod_t;
core_list_decl(n00bb_user_litmod_t);

typedef struct {
    core_string_t alias;
    core_string_t target;
    core_diag_location_t location;
} n00bb_litmod_alias_t;
core_list_decl(n00bb_litmod_alias_t);
typedef n00bb_user_litmod_t *n00bb_user_litmod_ptr_t;
core_list_decl(n00bb_user_litmod_ptr_t);
struct n00bb_litmod_site_t {
    n00bb_user_litmod_t *registration;
    th_type *payload_type;
    th_type *signature;
    bool deferred;
};

typedef struct {
    parse_tree_t *node;
    th_type *generic_union_type;
    bool variadic;
    bool has_type_node;
} n00bb_function_parameter_ref_t;

typedef struct {
    n00bb_function_parameter_ref_t *params;
    size_t param_count;
    size_t param_capacity;
    bool has_inferred_parameter;
} n00bb_function_parameter_facts_t;

typedef struct {
    th_type *success;
    th_type *failure;
    bool conflict;
    bool reported;
    parse_tree_t **return_expressions;
    size_t return_expression_count;
    size_t return_expression_capacity;
    size_t returned_parameter;
} n00bb_function_return_facts_t;

typedef struct {
    th_type *receiver_type;
    parse_sym_entry_t *implementation;
    bool generic_fallback;
} n00bb_trait_dispatch_entry_t;

typedef struct {
    n00bb_trait_dispatch_entry_t *entries;
    size_t count;
    bool direct;
} n00bb_trait_dispatch_plan_t;

typedef struct {
    core_string_t trait;
    core_string_t method;
    uint64_t fingerprint;
} n00bb_dispatch_record_t;

typedef struct {
    parse_tree_t *site;
    th_type *value;
    th_type *target;
} n00bb_deferred_conversion_t;
core_list_decl(n00bb_deferred_conversion_t);
core_list_decl(n00bb_dispatch_record_t);

typedef struct {
    parse_tree_t *site;
    th_type *type;
    ir_int_t lo;
    ir_int_t hi;
    bool wide;
} n00bb_integer_seed_t;
core_list_decl(n00bb_integer_seed_t);

typedef enum : uint8_t {
    N00BB_INTEGER_TERM_NONE,
    N00BB_INTEGER_TERM_TYPE,
    N00BB_INTEGER_TERM_SYMBOL,
    N00BB_INTEGER_TERM_NODE,
} n00bb_integer_term_kind_t;

typedef struct {
    n00bb_integer_term_kind_t kind;
    th_type *type;
    void *key;
} n00bb_integer_term_t;

typedef struct {
    parse_sym_entry_t *symbol;
    n00bb_integer_term_t bound;
    ir_op_t op;
} n00bb_integer_guard_t;
core_list_decl(n00bb_integer_guard_t);

typedef struct {
    parse_tree_t *site;
    n00bb_integer_term_t result;
    n00bb_integer_term_t left;
    n00bb_integer_term_t right;
    parse_sym_entry_t *left_symbol;
    parse_sym_entry_t *right_symbol;
    uint32_t guard_start;
    uint32_t guard_count;
    parse_tree_t *loop;
    ir_op_t op;
} n00bb_integer_edge_t;
core_list_decl(n00bb_integer_edge_t);

struct n00bb_iteration_plan_t {
    size_t iter_site;
    size_t next_site;
    size_t cleanup_site;
    th_type *cursor_type;
    th_type *item_type;
};

typedef struct {
    parse_tree_t *node;
    parse_tree_t *receiver_node;
    parse_scope_t *scope;
    parse_sym_entry_t *symbol;
    core_link_reference_t *reference;
    size_t next_for_node;
    core_string_t member_name;
    core_string_t *trait_path_parts;
    size_t trait_path_part_count;
    parse_sym_entry_t *function_owner;
    parse_sym_entry_t *initializer_owner;
    core_string_t dispatch_trait;
    core_string_t dispatch_method;
    core_string_t resolved_trait_name;
    core_string_t origin_module;
    th_type *dispatch_receiver_type;
    n00bb_trait_dispatch_plan_t *closed_dispatch;
    bool dotted_path;
    bool trait_reference;
    bool variant_case_candidate;
    bool constructor_call;
    bool unresolved;
    bool init_call;
    bool callback_call;
    bool callback_setup;
    bool operator_dispatch;
    bool in_place_dispatch;
    bool intrinsic_operator;
    bool semantic_dispatch;
    bool secondary_dispatch;
    bool optional_dispatch;
    bool in_module_initializer;
} n00bb_link_use_site_t;
core_list_decl(n00bb_link_use_site_t);

typedef struct {
    parse_token_stream_t *tokens;
    parse_pwz_parser_t *parser;
} n00bb_lex_result_t;

struct n00bb_ordered_nodes_t {
    parse_tree_t **nodes;
    size_t count;
};

typedef struct {
    core_string_t source_name;
    core_string_t export_name;
    core_string_t grantee;
    parse_tree_t *node;
    parse_sym_entry_t *symbol;
    core_translation_unit_t *source_unit;
    bool module_export;
    bool litmod_alias;
    bool renamed;
} n00bb_package_export_t;
core_list_decl(n00bb_package_export_t);

typedef struct n00bb_package_declaration_t {
    core_string_t name;
    parse_tree_t *node;
    parse_scope_t *scope;
    core_list_t(n00bb_package_export_t) exports;
} n00bb_package_declaration_t;

typedef struct {
    th_env_type_shape_kind_t kind;
    th_type *shape;
} n00bb_interface_type_shape_t;
core_list_decl(n00bb_package_declaration_t);

typedef struct {
    core_string_t name;
    core_translation_unit_t *unit;
    bool renamed;
} n00bb_import_alias_t;
core_list_decl(n00bb_import_alias_t);

typedef struct {
    parse_tree_t *node;
    core_translation_unit_t *provider;
    core_string_t local;
    core_string_t remote;
    bool all;
} n00bb_brought_name_t;
core_list_decl(n00bb_brought_name_t);


typedef struct {
    ir_func_t *producer;
    ir_global_t *global;
} n00bb_literal_constant_t;
core_list_decl(n00bb_literal_constant_t);

typedef struct {
    parse_tree_t *literal;
    ir_global_t *global;
} n00bb_literal_backed_const_t;
core_list_decl(n00bb_literal_backed_const_t);

typedef struct {
    core_translation_unit_t *provider;
    parse_scope_t *values;
    parse_scope_t *types;
    const n00bb_brought_name_t *brought;
} n00bb_import_source_t;
core_list_decl(n00bb_import_source_t);

typedef struct {
    core_translation_unit_t *unit;
    n00bb_lex_result_t *lex;
    parse_annot_result_t *annotations;
    core_list_t(parse_tree_ptr_t) module_top_level_statements;
    core_list_t(n00bb_literal_constant_t) literal_constants;
    core_list_t(parse_tree_ptr_t) array_type_nodes;
    core_list_t(n00bb_user_litmod_t) literal_modifiers;
    core_list_t(n00bb_litmod_alias_t) litmod_aliases;
    core_list_t(n00bb_package_declaration_t) package_declarations;
    core_list_t(n00bb_import_alias_t) import_aliases;
    core_dict_t import_alias_index;
    core_list_t(core_translation_unit_ptr_t) path_imports;
    core_list_t(core_translation_unit_ptr_t) restricted_imports;
    core_list_t(n00bb_brought_name_t) brought_names;
    core_list_t(n00bb_link_use_site_t) link_use_sites;
    core_list_t(n00bb_dispatch_record_t) dispatch_records;
    uint32_t syntax_spelling_lines[PP_SPELL_COUNT];
    bool needs_build_time_capture;
    bool needs_literal_capture;
    core_list_t(n00bb_literal_backed_const_t) literal_backed_consts;
    core_list_t(n00bb_import_source_t) import_sources;
    bool import_sources_ready;
    core_list_t(n00bb_deferred_conversion_t) deferred_conversions;
    core_list_t(n00bb_integer_seed_t) integer_seeds;
    core_list_t(n00bb_integer_edge_t) integer_edges;
    core_list_t(n00bb_integer_guard_t) integer_guards;
    core_dict_t integer_edge_sites;
    core_dict_t integer_range_nodes;
    core_dict_t integer_tracked_symbols;
    core_dict_t failed_calls;
    core_list_t(parse_tree_ptr_t) integer_shift_sites;
    core_dict_t integer_shift_seen;
    core_list_t(parse_tree_ptr_t) literal_site_nodes;
    core_list_t(parse_tree_ptr_t) shape_literal_nodes;
    core_dict_t flagset_values;
    core_dict_t shared_state_types;
    core_dict_t declarations_typed_ahead;
    core_link_unit_t *link;
    core_diag_location_t build_time_use;
    core_dict_t build_time_queries;
    n00bb_link_analysis_t *link_analysis;
    th_ctx *types;
    ir_module_t *ir_module;
    core_arena_t semantic_values;
    parse_tree_t *integer_range_root;
    core_dict_t integer_range_references;
    n00bb_range_table_t integer_symbol_ranges;
    core_dict_t integer_range_invalidated_symbols;
    core_dict_t integer_solved_symbols;
    core_dict_t integer_boundary_sites;
    core_dict_t integer_narrowing_sites;
    core_dict_t integer_reference_taken;
    parse_grammar_t *grammar;
    core_list_t(n00bb_range_branch_frame_t) integer_range_branches;
    bool integer_range_use_symbol_facts;
    bool integer_range_roles_ready;
    bool inference_finalized;
    core_buffer_t declaration_sites;
    bool interface_module;
    bool import_failure_reported;
    bool package_interface;
    bool package_manifest_restored;
    bool semantic_annotation_started;
    bool semantic_annotation_complete;
    bool semantic_errors;
    bool initializer_excluded;
    bool semantic_annotation_failed;
    bool type_symbols_indexed;
    core_dict_t type_symbol_index;
    parse_tree_t *flagset_scope;
} n00bb_tu_state_t;
static inline parse_tree_t *
n00bb_tu_parsed_tree(const n00bb_tu_state_t *state)
{
    return state && state->lex && state->lex->parser
        ? parse_pwz_get_tree(state->lex->parser) : nullptr;
}
bool reference_is_simple_assignment_target(parse_grammar_t *grammar,
                                           core_translation_unit_t *unit,
                                           parse_tree_t *node,
                                           parse_sym_entry_t *symbol);
bool resolve_local_enum_constant(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state,
                                 n00bb_link_use_site_t *site);
bool identifier_is_variant_case_expression(parse_grammar_t *grammar,
                                           core_translation_unit_t *unit,
                                           parse_tree_t *node);
parse_tree_t *direct_ref_expression(parse_grammar_t *grammar,
                                    parse_tree_t *node);
bool bind_resolved_reference_alias(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *use_site);
parse_cf_label_t *call_label_for_member(parse_grammar_t *grammar,
                                        n00bb_tu_state_t *state,
                                        parse_tree_t *member);
parse_scope_t *n00bb_variant_scope_for_type(core_translation_unit_t *unit,
                                            n00bb_tu_state_t *state,
                                            th_type *type);
bool n00bb_variant_arm_symbol(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol);
core_list_t(parse_sym_entry_ptr_t) variant_arms_in_source_order(
    parse_grammar_t *grammar, parse_scope_t *scope);
parse_sym_entry_t *variant_payload_arm(parse_grammar_t *grammar,
                                       core_translation_unit_t *unit,
                                       parse_tree_t *node);
parse_sym_entry_t *variant_assignment_arm(parse_grammar_t *grammar,
                                          core_translation_unit_t *unit,
                                          parse_tree_t *node);
bool prepare_variant_payload_type(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *node);
bool n00bb_unify_node_type(th_ctx *types, parse_tree_t *node, th_type *type);
bool n00bb_receiver_names_type(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state, parse_tree_t *receiver,
                               parse_sym_entry_t *type_symbol);
bool check_local_record_field_reference(parse_grammar_t *grammar,
                                        core_translation_unit_t *unit,
                                        parse_tree_t *node);
bool infer_callback_expression_type(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    parse_tree_t *node,
                                    n00bb_tu_state_t *state);
bool infer_tuple_literal_type(parse_grammar_t *grammar, parse_tree_t *node,
                              n00bb_tu_state_t *state);
bool infer_sequence_modifier_type(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state);
bool infer_brace_modifier_type(parse_grammar_t *grammar, parse_tree_t *node,
                               n00bb_tu_state_t *state);
bool infer_string_modifier_type(parse_grammar_t *grammar, parse_tree_t *node,
                                n00bb_tu_state_t *state);
bool infer_user_literal_type(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *node,
                             n00bb_user_litmod_t *chosen);
bool n00bb_defer_shape_literal(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *node);
bool n00bb_type_shape_literal(parse_grammar_t *grammar,
                              core_translation_unit_t *unit,
                              parse_tree_t *node);
bool n00bb_resolve_shape_literal(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node);
void n00bb_poison_node(n00bb_tu_state_t *state, parse_tree_t *node);
bool n00bb_resolve_shape_literals(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit);
bool resolve_forward_literal_sites(core_translation_unit_t *unit,
                                   n00bb_tu_state_t *state);
bool report_shadowed_litmods(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state);
bool n00bb_declares_litmod(n00bb_tu_state_t *state,
                           parse_sym_entry_t *symbol);
bool n00bb_declares_litmod_alias(n00bb_tu_state_t *state, core_string_t name);
parse_sym_entry_t *n00bb_litmod_handler(
    const n00bb_user_litmod_t *registration);
bool collect_ordered_children(parse_tree_t *node,
    core_list_t(parse_tree_ptr_t) *children);
core_string_t named_field_identifier(parse_grammar_t *grammar,
                                     parse_tree_t *field);
bool n00bb_switch_case_integer(parse_grammar_t *grammar,
                               parse_tree_t *node, ir_lit_t *value);
th_type *static_sequence_element_type(th_type *type);
th_type *static_tuple_element_type(th_type *type, size_t index,
                                   size_t *count);
bool static_value_matches_type(const n00bb_static_value_t *value,
                               th_type *type, n00bb_tu_state_t *state,
                               size_t depth);
bool semantic_static_value_from_expression(parse_grammar_t *grammar,
                                           parse_tree_t *expression,
                                           th_type *type,
                                           n00bb_tu_state_t *state,
                                           n00bb_static_value_t *out,
                                           size_t depth);
bool semantic_static_literal_value(parse_grammar_t *grammar,
                                   parse_tree_t *declaration,
                                   n00bb_tu_state_t *state,
                                   n00bb_static_value_t *out);
th_type *static_record_shape(n00bb_tu_state_t *state, th_type *type);
th_type *n00bb_field_tuple(th_ctx *types, th_member *members, size_t count);
bool n00bb_type_has_constructor(core_translation_unit_t *unit, th_type *type);
bool infer_anonymous_record(parse_grammar_t *grammar, parse_tree_t *node,
                            n00bb_tu_state_t *state);
bool contextualize_record_literal(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state,
                                  parse_tree_t *expression,
                                  th_type *expected);
th_type *record_literal_expected_type(parse_grammar_t *grammar,
                                      th_ctx *types, parse_tree_t *literal);
bool contextualize_record_fields_before_inference(
    parse_grammar_t *grammar, core_translation_unit_t *unit,
    n00bb_tu_state_t *state, parse_tree_t *literal);
void remember_link_use_site(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            n00bb_tu_state_t *state, parse_tree_t *node);
parse_tree_t *call_arguments_node(parse_grammar_t *grammar,
                                  parse_tree_t *call);
void collect_positional_nodes(parse_grammar_t *grammar,
                              parse_tree_t *node,
                              core_list_t(n00bb_tree_ptr_t) *arguments);
core_string_t *cache_trait_name_parts(parse_tree_t *node, size_t *count);
parse_tree_t *n00bb_init_target(parse_grammar_t *grammar,
                                parse_tree_t *node);
bool primitive_operator_site(n00bb_tu_state_t *state,
                             n00bb_link_use_site_t *site);
const ir_scalar_t *n00bb_scalar_of(th_type *type);
bool n00bb_scalar_class_is(th_type *type, ir_class_t class);
bool n00bb_scalar_role_is(th_type *type, ir_role_t role);
bool is_float_type(th_type *type);
bool n00bb_flagset_type(core_translation_unit_t *unit,
                        n00bb_tu_state_t *state, th_type *type,
                        parse_sym_entry_t **symbol_out);
bool n00bb_flagset_operator_site(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state,
                                  parse_tree_t *node);
bool refine_flagset_operator_result(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool refine_intrinsic_binary_result(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool record_generic_operator_failure(parse_grammar_t *grammar,
                                     n00bb_tu_state_t *state,
                                     parse_tree_t *node);
bool refine_intrinsic_unary_result(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   n00bb_tu_state_t *state,
                                   parse_tree_t *node);
bool index_is_compound_target(parse_grammar_t *grammar,
                              parse_tree_t *node);
th_type *n00bb_callback_function_type(parse_grammar_t *grammar,
                                      core_translation_unit_t *unit,
                                      parse_tree_t *member);
parse_tree_t *operator_receiver_node(n00bb_link_use_site_t *site);
bool module_initializer_declaration(parse_grammar_t *grammar,
                                    parse_tree_t *node);
bool is_module_initializer_use(parse_grammar_t *grammar,
                               parse_tree_t *node,
                               parse_sym_entry_t *initializer_owner);
bool initializer_node_is_reference_operand(parse_grammar_t *grammar,
    parse_tree_t *node);
bool initializer_node_is_reference_store_target(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node,
    parse_sym_entry_t *symbol);
bool initializer_node_is_out_argument(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
parse_sym_entry_t *module_variable_initializer_owner(
    parse_grammar_t *grammar, parse_symtab_t *symtab,
    parse_tree_t *node);
void validate_builtin_generic_type_arity(parse_grammar_t *grammar,
                                         parse_tree_t *node,
                                         core_translation_unit_t *unit,
                                         th_type *type);
bool n00bb_flagset_member(parse_sym_entry_t *symbol);
bool n00bb_flagset_instance_access(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node,
                                   parse_sym_entry_t *member);
n00bb_user_litmod_t *user_litmod(n00bb_tu_state_t *state,
                                 core_string_t modifier,
                                 core_string_t kind, bool *ambiguous);
core_list_t(n00bb_user_litmod_ptr_t)
n00bb_visible_litmods(n00bb_tu_state_t *state, core_string_t kind);
core_string_t n00bb_literal_shape_trait(core_string_t kind);
core_string_t literal_modifier(parse_grammar_t *grammar,
                               parse_tree_t *node);
bool dict_only_empty_literal(parse_grammar_t *grammar,
                             parse_tree_t *node);
bool literal_modifier_terminal(parse_grammar_t *grammar,
                               parse_tree_t *leaf,
                               parse_tree_t *parent);
bool cache_static_literal_node(parse_grammar_t *grammar,
                               n00bb_tu_state_t *state,
                               parse_tree_t *node);
bool cache_ordered_literal_children(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool cache_ordered_array_dimensions(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool cache_typecase_arm_types(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state,
                              parse_tree_t *node);
void prepare_typecase_arm(parse_grammar_t *grammar,
                          core_translation_unit_t *unit,
                          parse_tree_t *case_block);
bool apply_typecase_narrowing(parse_grammar_t *grammar,
                              parse_tree_t *node);
parse_sym_entry_t *lookup_named_type_reference(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_symtab_t *symtab, parse_scope_t *scope,
    core_string_t name);
bool n00bb_practical_unsigned_to_signed(parse_grammar_t *grammar,
                                        n00bb_tu_state_t *state,
                                        parse_tree_t *source_node,
                                        th_type *source, th_type *target);
void record_function_exit_fact(parse_grammar_t *grammar,
                               core_translation_unit_t *unit,
                               parse_tree_t *node,
                               n00bb_tu_state_t *state);
bool n00bb_constrain_integer_fit(n00bb_tu_state_t *state, th_type *type,
                                 th_type *source, bool range_known,
                                 ir_interval_t range);
bool n00bb_cache_contextual_integer(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
typedef struct {
    int64_t value;
    bool is_unsigned;
} n00bb_untyped_integer_t;

bool n00bb_untyped_integer_const(parse_grammar_t *grammar,
                                 parse_sym_entry_t *symbol,
                                 int64_t *minimum, int64_t *maximum);
bool n00bb_untyped_integer_const_unsigned(parse_grammar_t *grammar,
                                          parse_sym_entry_t *symbol);
bool n00bb_integer_constant_reference(parse_grammar_t *grammar,
                                      parse_tree_t *node, int64_t *value);
bool n00bb_folded_integer_expression(parse_grammar_t *grammar,
                                     parse_tree_t *node, int64_t *value);
bool n00bb_unsigned_literal_origin(parse_tree_t *origin);
th_type *n00bb_integer_literal_context(parse_grammar_t *grammar,
                                       n00bb_tu_state_t *state,
                                       parse_tree_t *literal);

bool cache_return_source_fact(parse_grammar_t *grammar, parse_tree_t *node,
                              n00bb_tu_state_t *state);
bool cache_alias_edges(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_tu_state_t *state);

bool n00bb_current_integer_range(parse_tree_t *node, ir_interval_t *range);
bool n00bb_set_integer_range(n00bb_tu_state_t *state, parse_tree_t *node,
                             ir_interval_t range);
void n00bb_settle_symbol_range(n00bb_tu_state_t *state, parse_tree_t *node,
                               ir_interval_t range);
void n00bb_integer_boundary(n00bb_tu_state_t *state, parse_tree_t *node,
                            th_type *target);
void n00bb_report_integer_misfit(n00bb_tu_state_t *state, parse_tree_t *node,
                                 bool constant, ir_interval_t range,
                                 th_type *target);
void n00bb_report_integer_narrowing(n00bb_tu_state_t *state,
                                    parse_tree_t *node, ir_interval_t range,
                                    th_type *target);
bool n00bb_integer_narrowing_reported(n00bb_tu_state_t *state, uint32_t line,
                                      uint32_t column);
bool n00bb_integer_point(parse_tree_t *node, int64_t *value);
bool n00bb_type_interval(th_type *type, ir_interval_t *range);
ir_op_t n00bb_comparison_op(core_string_t op);
bool n00bb_function_node(parse_grammar_t *grammar, parse_tree_t *node);
parse_tree_t *n00bb_enclosing_function_node(parse_grammar_t *grammar,
                                            parse_tree_t *node);
#define N00BB_INTEGER_WIDTHS 5
th_type *n00bb_integer_type_of_width(th_ctx *types, uint64_t width,
                                     bool is_signed);
parse_sym_entry_t *n00bb_range_variable_of(parse_tree_t *node);
bool n00bb_integer_representation(th_type *type, uint64_t *width,
                                  bool *is_signed);
th_type *n00bb_integer_literal_variable(n00bb_tu_state_t *state,
                                        parse_tree_t *site, ir_lit_t literal);
void n00bb_register_integer_class(n00bb_tu_state_t *state);
bool n00bb_open_integer(th_type *type);
th_type *n00bb_integer_variable(n00bb_tu_state_t *state, uint8_t flags);
n00bb_integer_term_t n00bb_integer_term_of(n00bb_tu_state_t *state,
                                           parse_tree_t *node);
bool n00bb_integer_flow(n00bb_tu_state_t *state, parse_tree_t *site,
                        ir_op_t op, n00bb_integer_term_t result,
                        n00bb_integer_term_t left,
                        n00bb_integer_term_t right);
void n00bb_integer_growth(n00bb_tu_state_t *state, parse_tree_t *site,
                          ir_op_t op, parse_tree_t *target,
                          parse_tree_t *value);
void n00bb_integer_copy(n00bb_tu_state_t *state, parse_tree_t *site,
                        n00bb_integer_term_t target, parse_tree_t *value);
void n00bb_integer_assign(n00bb_tu_state_t *state, parse_tree_t *site,
                          n00bb_integer_term_t written, parse_tree_t *target,
                          parse_tree_t *value);
void n00bb_integer_node_result(n00bb_tu_state_t *state, parse_tree_t *site,
                               ir_op_t op, parse_tree_t *left,
                               parse_tree_t *right);
th_type *n00bb_integer_result_variable(n00bb_tu_state_t *state,
                                       parse_tree_t *site, ir_op_t op,
                                       parse_tree_t *left,
                                       parse_tree_t *right);
bool n00bb_require_unsigned_integer(n00bb_tu_state_t *state, th_type *type);
bool n00bb_ground_integers(core_translation_unit_t *const *units,
                           size_t unit_count, bool interface_only,
                           bool *grounded);
bool n00bb_range_proven_above_int64(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool n00bb_constant_misfits_type(n00bb_tu_state_t *state, parse_tree_t *node,
                                 th_type *expected);
bool n00bb_integer_literal_joins_type(n00bb_tu_state_t *state,
                                      parse_tree_t *node, th_type *target);
bool n00bb_literal_adopts_open_type(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node, th_type *type);
static constexpr char n00bb_string_to_buffer_text[] =
    "a string is immutable and does not convert to Buffer; use "
    "buffer_from_string(s)";
void n00bb_report_string_not_buffer(core_translation_unit_t *unit,
                                    parse_tree_t *value);
bool n00bb_report_option_operand(core_translation_unit_t *unit, th_ctx *types,
                                 core_string_t operator_text,
                                 parse_tree_t *operand, th_type *type);
void n00bb_report_operator_operands(core_translation_unit_t *unit,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node,
                                    core_string_t operator_text,
                                    parse_tree_t *left, th_type *left_type,
                                    parse_tree_t *right, th_type *right_type);
void n00bb_report_shift_count_range(core_translation_unit_t *unit,
                                    parse_tree_t *at, ir_interval_t count,
                                    uint64_t width, core_string_t type_text);
void n00bb_report_integer_constant_range(core_translation_unit_t *unit,
                                         parse_tree_t *at,
                                         core_string_t value,
                                         core_string_t target);
static constexpr char n00bb_buffer_type_name[] =
    "builtin.containers.buffer.Buffer";
bool n00bb_value_coerces(th_ctx *types, th_type *from, th_type *to);
bool n00bb_string_where_buffer_expected(th_ctx *types, th_type *expected,
                                        th_type *actual);
parse_tree_t *n00bb_string_where_buffer_site(parse_grammar_t *grammar,
                                             th_ctx *types,
                                             parse_tree_t *node,
                                             th_type *expected);
void n00bb_register_value_coercions(n00bb_tu_state_t *state);
th_type *n00bb_value_coercion_target(th_ctx *types, th_type *from);
bool n00bb_blanket_receiver(th_type *contract);
th_type *n00bb_type_materialize(parse_grammar_t *grammar,
                                n00bb_tu_state_t *state, parse_tree_t *node,
                                th_type *actual, th_type *expected,
                                bool *converted);
bool n00bb_contextual_float_literal(parse_tree_t *node, th_type *target);
bool n00bb_float_literal_adopts(parse_tree_t *node, th_type *operand);
void cache_type_syntax_node(parse_grammar_t *grammar, parse_tree_t *node);
bool append_type_argument_pack(th_type *pack,
                               core_list_t(th_arg) *arguments);
th_type *type_argument_pack(parse_grammar_t *grammar, th_ctx *types,
                            parse_tree_t *node);
th_type *function_parameter_pack(parse_grammar_t *grammar, th_ctx *types,
                                 parse_tree_t *node);
th_type *n00bb_array_index_type(th_ctx *types, core_arena_t *arena);
bool register_native_declarations(parse_annot_result_t *annotations,
                                  th_ctx *types,
                                  n00bb_native_declarations_t natives);
bool register_builtin_symbols(parse_annot_result_t *annotations,
                              n00bb_tu_state_t *state, th_ctx *types);
bool n00bb_register_operator_function(th_env *environment, th_ctx *types,
                                      core_string_t trait, core_string_t method,
                                      core_string_t owner, th_type *self,
                                      th_type *other, th_type *result,
                                      core_string_t module, core_string_t package,
                                      th_env_visibility_t visibility);
bool register_builtin_traits(th_env *environment, th_ctx *types,
                             core_arena_t *arena);
bool record_function_parameter_fact(parse_grammar_t *grammar,
                                    parse_tree_t *node,
                                    n00bb_tu_state_t *state);
void append_module_type_name(core_translation_unit_t *unit,
                             core_string_t name, core_buffer_t *buffer);
bool finalize_variant_symbol_type(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state);
bool finalize_enum_symbol_type(parse_grammar_t *grammar, parse_tree_t *node,
                               core_translation_unit_t *unit,
                               n00bb_tu_state_t *state);
parse_tree_t *flagset_declaration_ancestor(parse_grammar_t *grammar,
                                           parse_tree_t *node);
bool finalize_flagset_symbol_type(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state);
core_string_t n00bb_node_source_text(core_translation_unit_t *unit,
                                     parse_tree_t *node,
                                     core_string_t fallback);
bool cache_flagset_semantics(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *node);
bool n00bb_flagset_first_problem(parse_sym_entry_t *owner);
void n00bb_range_enter_node(parse_grammar_t *grammar,
                            n00bb_tu_state_t *state, parse_tree_t *node,
                            parse_tree_t *parent);
void n00bb_range_after_children(parse_grammar_t *grammar,
                                n00bb_tu_state_t *state, parse_tree_t *node);
void n00bb_range_after_annotation(parse_grammar_t *grammar,
                                  n00bb_tu_state_t *state, parse_tree_t *node,
                                  parse_tree_t *parent);
void n00bb_range_state_release(n00bb_tu_state_t *state);

n00bb_tu_state_t *n00bb_tu_state_new(core_translation_unit_t *unit,
                                    n00bb_lex_result_t *lex, th_ctx *types);
parse_scope_t *module_scope(n00bb_tu_state_t *state);
parse_scope_t *type_module_scope(n00bb_tu_state_t *state);
core_list_t(parse_scope_ptr_t) module_scopes_in_namespace(
    n00bb_tu_state_t *state, core_string_t namespace_name);
core_list_t(parse_scope_ptr_t) module_scopes(n00bb_tu_state_t *state);
core_translation_unit_t *find_direct_import(core_translation_unit_t *unit,
                                            core_string_t module);
n00bb_package_declaration_t *find_package_declaration(
    core_translation_unit_t *unit, core_string_t name);
core_diag_location_t link_source_location(parse_tree_t *node);

static inline void
n00bb_tu_internal_error(core_translation_unit_t *unit, parse_tree_t *node,
                        const char *stage, const char *what)
{
    n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
        node ? link_source_location(node) : (core_diag_location_t){0},
        stage, "%s", what);
}

static inline void
n00bb_tu_internal_error_unless_reported(core_translation_unit_t *unit,
                                        parse_tree_t *node,
                                        const char *stage, const char *what)
{
    if (core_diagnostics_failed(core_translation_unit_diagnostics(unit)))
        return;
    n00bb_tu_internal_error(unit, node, stage, what);
}

parse_sym_entry_t *canonical_symbol(parse_sym_entry_t *symbol);
parse_sym_entry_t *n00bb_scope_named_next(parse_scope_t *scope,
    core_string_t name, parse_sym_entry_t *previous);
bool n00bb_same_declaration_listed(
    const core_list_t(parse_sym_entry_ptr_t) *list, parse_sym_entry_t *symbol);
parse_sym_entry_t *type_symbol_in_loaded_imports(
    core_translation_unit_t *root, th_ctx *types, th_type *wanted,
    core_translation_unit_t **owner_unit_out);
void n00bb_index_type_symbols(n00bb_tu_state_t *state);
n00bb_callback_contract_t *n00bb_callback_contract_for_type(
    n00bb_tu_state_t *state, th_type *type);
th_type *n00bb_specialize_callback_role(th_ctx *types,
    n00bb_callback_contract_t *contract, th_type *receiver, th_type *role);
th_type *n00bb_callback_method_signature(
    th_ctx *types, th_type *receiver, core_string_t name, void *context);
bool prepare_callback_call_symbols(core_translation_unit_t *unit);
bool n00bb_bind_callback_call(n00bb_tu_state_t *state,
                              n00bb_link_use_site_t *site);
bool n00bb_builtin_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_native_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_init_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_array_read_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_array_write_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_byte_offset_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_byte_diff_function(parse_sym_entry_t *symbol);
const n00bb_unsafe_arithmetic_t *n00bb_builtin_unsafe_arithmetic(
    parse_sym_entry_t *symbol);
bool n00bb_builtin_bit_count_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_gc_memory_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_volatile_load_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_signal_fence_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_prefetch_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_rmw_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_cas_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_access_function(parse_sym_entry_t *symbol);
bool n00bb_is_trait_dispatch_reference(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
parse_sym_entry_t *simple_expression_bound_symbol(parse_tree_t *node);
bool n00bb_default_references(parse_tree_t *node,
    parse_sym_entry_t *symbol);
void context_default_dependencies(parse_tree_t *expr,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    core_list_t(parse_sym_entry_ptr_t) *out);
th_type *context_default_ir_type(th_ctx *types, th_type *context_type,
    core_list_t(parse_sym_entry_ptr_t) *dependencies,
    th_type *result_type);
bool n00bb_call_is_error_propagated(parse_grammar_t *grammar,
    parse_tree_t *node);
parse_tree_t *n00bb_try_handler_clause(parse_grammar_t *grammar,
    parse_tree_t *node);
parse_sym_entry_t *n00bb_try_clause_symbol(parse_grammar_t *grammar,
    parse_tree_t *clause);
bool bind_array_shape_type_source(parse_grammar_t *grammar,
                                  parse_tree_t *type_reference,
                                  parse_sym_entry_t *symbol);
bool context_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol);
bool is_module_global_symbol(parse_grammar_t *grammar,
                             parse_sym_entry_t *symbol);
bool type_may_contain_reference_alias(th_ctx *types, th_type *type);
bool n00bb_type_reaches_shared_state(th_ctx *types, th_type *type);
bool n00bb_unit_type_reaches_shared_state(n00bb_tu_state_t *state,
                                          th_type *type);
parse_sym_entry_t *n00bb_view_origin(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node,
                                     parse_sym_entry_t **in_parameter);
parse_sym_entry_t *n00bb_frozen_origin(parse_grammar_t *grammar,
                                       core_translation_unit_t *unit,
                                       parse_tree_t *node);
bool finalize_alias_free_local_summaries(parse_grammar_t *grammar,
    core_translation_unit_t *unit);
parse_tree_t *aggregate_literal_root(parse_grammar_t *grammar,
    parse_tree_t *expression);
bool record_aggregate_aliases(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node,
    parse_sym_entry_t *holder);
void record_reference_facts(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
typedef bool (*n00bb_semantic_unit_finish_t)(void *context,
    core_translation_unit_t *unit);
bool semantic_annotate_closure(parse_grammar_t *grammar, th_ctx *types,
    core_translation_unit_t *unit, n00bb_semantic_unit_finish_t finish,
    void *context);
bool is_reference_symbol(parse_sym_entry_t *symbol);
bool is_integer_type(th_type *type);
bool n00bb_static_integer_expression(parse_grammar_t *grammar,
                                     parse_tree_t *expression, th_type *type,
                                     uint64_t *value);
bool validate_cached_array_dimensions(parse_grammar_t *grammar,
    core_translation_unit_t *unit, n00bb_tu_state_t *state);
bool select_storage_classes(core_list_t(core_translation_unit_ptr_t) *units);
bool node_is_within(parse_tree_t *node, parse_tree_t *ancestor);
void collect_ir_parameters(parse_grammar_t *grammar, parse_tree_t *node,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    parse_sym_entry_t **rest);
bool context_hook_name(parse_sym_entry_t *symbol, core_string_t suffix,
    core_buffer_t *out);
bool context_default_name(parse_sym_entry_t *symbol, size_t index,
    core_buffer_t *out);
bool callback_protocol_name(core_string_t type_name, core_string_t marker,
    core_buffer_t *out);
n00bb_node_facts_t *n00bb_node_facts(parse_tree_t *node);
n00bb_ordered_nodes_t *n00bb_node_ordered_nodes(parse_tree_t *node);
n00bb_litmod_site_t *n00bb_node_literal_site(parse_tree_t *node);
parse_tree_t *n00bb_node_record_literal_origin(parse_tree_t *node);
bool n00bb_node_tuple_index(parse_tree_t *node, uint32_t *index);
th_type *n00bb_tuple_element(th_ctx *types, parse_grammar_t *grammar,
                             parse_tree_t *index, th_type *tuple, bool write,
                             void *context);
parse_sym_entry_t *bound_function_symbol(parse_tree_t *node);
th_type *n00bb_reference_definition(th_ctx *types, th_type *type);
th_type *n00bb_dereferenced_type(th_ctx *types, th_type *type);
bool n00bb_object_type(th_ctx *types, th_type *type);
n00bb_node_facts_t *n00bb_ensure_node_facts(n00bb_tu_state_t *state,
    parse_tree_t *node);
n00bb_return_source_t *n00bb_node_return_source(parse_tree_t *node);
n00bb_alias_edges_t *n00bb_node_alias_edges(parse_tree_t *node);
core_string_t n00bb_context_hook_role(core_string_t name);
bool alias_declaration_is_transparent(parse_grammar_t *grammar,
                                      parse_tree_t *declaration);
th_type *declared_type_from_node(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
bool record_fields_in_source_order(parse_scope_t *scope,
    core_list_t(parse_sym_entry_ptr_t) *fields);
bool exported_type_shape(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *symbol,
    th_env_type_shape_kind_t *shape_kind, th_type **shape);
parse_sym_entry_t *enclosing_function_symbol(parse_grammar_t *grammar,
                                             parse_tree_t *node);
parse_sym_entry_t *link_effect_owner_symbol(parse_grammar_t *grammar,
                                             parse_tree_t *node);
bool n00bb_symbol_visible_to(core_translation_unit_t *consumer,
                             core_translation_unit_t *owner,
                             parse_sym_entry_t *symbol);
bool n00bb_member_visible_to(core_translation_unit_t *consumer,
                             core_translation_unit_t *owner,
                             parse_sym_entry_t *symbol);
bool symbol_is_exportable(parse_sym_entry_t *symbol);
bool symbol_exported_by_default(parse_sym_entry_t *symbol);
th_env_visibility_t symbol_visibility(parse_sym_entry_t *symbol);
parse_tree_t *symbol_binding_node(parse_sym_entry_t *symbol);
bool symbol_has_definition(parse_sym_entry_t *symbol, parse_tree_t *node);
bool scope_in_value_namespace(parse_scope_t *scope);
bool is_named_type_declaration(parse_grammar_t *grammar,
                               parse_sym_entry_t *symbol);
bool field_record_declaration(parse_grammar_t *grammar, parse_tree_t *node);
bool n00bb_field_scope_tag(core_string_t tag);
typedef struct {
    th_type *base;
    th_type *named;
    th_type *shape;
    th_type *record;
    th_type *representation;
} n00bb_field_declaration_types_t;
bool n00bb_field_declaration_types(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_scope_t *scope,
                                   n00bb_field_declaration_types_t *out);
bool n00bb_object_type_declared(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_sym_entry_t *value);
#define n00bb_object_shape_suffix ".__fields"
#define n00bb_view_blocked_binding CORE_STRING_STATIC("n00bb.view-blocked")
void n00bb_set_build_time(bool build_time);
void n00bb_set_literal_stage(bool literal_stage);
bool n00bb_literal_stage(void);
size_t n00bb_initializer_kinds(void);
parse_sym_phase_t n00bb_compile_phase(void);
bool scope_is_within_function(parse_scope_t *scope);
bool scope_is_within(parse_scope_t *scope, parse_scope_t *ancestor);
bool scope_has_lexical_lifetime(parse_scope_t *scope);
parse_scope_t *lexical_function_scope(parse_scope_t *scope);
parse_tree_t *simple_identifier_primary(parse_grammar_t *grammar,
                                        parse_tree_t *node);
typedef struct {
    parse_tree_t *literal;
    size_t literal_count;
    bool valid;
    bool negative;
} n00bb_static_literal_probe_t;
n00bb_static_literal_probe_t find_static_literal(
    parse_grammar_t *grammar, parse_tree_t *node);
bool simple_identifier_expression(parse_grammar_t *grammar,
                                  parse_tree_t *node, core_string_t *name);
void bind_function_body_signature(core_translation_unit_t *unit,
    parse_tree_t *signature, n00bb_tu_state_t *state);
bool resolve_function_parameter_facts(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, n00bb_function_parameter_facts_t *facts,
    core_list_t(th_param) *params, th_param *rest, bool *has_rest);
core_string_t n00bb_print_type(th_ctx *types, th_type *type);
core_string_t n00bb_print_type_qualified(th_ctx *types, th_type *type);
core_string_t n00bb_print_type_for_user(th_ctx *types, th_type *type);
core_string_t n00bb_print_type_brief(th_ctx *types, th_type *type);
th_type *function_with_error_channel(th_ctx *types, th_type *function,
    th_type *error_type);
parse_sym_entry_t *formal_symbol_at(parse_grammar_t *grammar,
    parse_tree_t *node, size_t target, size_t *index);
void record_function_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *func,
    n00bb_tu_state_t *state, bool check_body);
bool collect_array_dimension_nodes(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_tree_t *node,
    core_list_t(n00bb_tree_ptr_t) *dimensions);
ir_shape_extent_t array_dimension_extent(parse_grammar_t *grammar,
    parse_tree_t *expression);
ir_shape_t *array_shape_from_declaration(parse_grammar_t *grammar,
    core_translation_unit_t *unit, ir_ctx_t *ir_context,
    parse_sym_entry_t *symbol, th_type *semantic_type, bool *complete);
bool populate_unit_symbol_shapes(parse_grammar_t *grammar,
    ir_ctx_t *ir_context, core_translation_unit_t *unit);
void signature_typevars_for_symbol(parse_sym_entry_t *symbol,
    core_list_t(n00bb_typevar_binding_t) *out);
th_type *type_variable_for_signature(th_ctx *types,
    core_list_t(n00bb_typevar_binding_t) *variables,
    core_string_t name, th_type *annotated, parse_tree_t *node);
th_type *signature_typevar_for_node(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
th_type *n00bb_resolve_type_node(th_ctx *types, parse_grammar_t *grammar,
    parse_tree_t *node, void *context);
bool apply_signature_constraints(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *where, th_ctx *types,
    core_list_t(n00bb_typevar_binding_t) *variables,
    core_list_t(th_trait_requirement) *declared);
void record_cached_declared_function_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *signature,
    n00bb_tu_state_t *state);
th_type *explicit_trait_method_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *method, th_ctx *types,
    th_type **failure);
void bind_declared_parameter(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node,
    n00bb_tu_state_t *state);
n00bb_link_use_site_t *link_use_site_for_node(
    n00bb_tu_state_t *state, parse_tree_t *node);
bool append_link_use_site(n00bb_tu_state_t *state,
    n00bb_link_use_site_t site);
n00bb_link_use_site_t *link_use_site_for_dispatch(
    n00bb_tu_state_t *state, parse_tree_t *node, core_string_t trait);
void record_callback_role_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *role,
    n00bb_tu_state_t *state);
void record_context_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *context,
    n00bb_tu_state_t *state);
void record_context_hook_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *hook,
    n00bb_tu_state_t *state);
void finalize_callback_contract(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *declaration,
    n00bb_tu_state_t *state);
th_type *callback_setup_dispatch_contract(th_ctx *types,
    th_type *setup_type);
void initial_symbol_node(parse_visit_t *visit, bool entering, void *context);
void declared_symbol_node(parse_visit_t *visit, bool entering, void *context);
bool prepare_initial_symbols(core_translation_unit_t *unit, th_ctx *types);
parse_tree_t *n00bb_simple_switch_literal(parse_grammar_t *grammar,
                                          parse_tree_t *node,
                                          bool *negative, bool *saw_sign);
bool refine_integer_literal_type(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node,
                                 n00bb_tu_state_t *state);
bool infer_new_expression_type(parse_grammar_t *grammar,
                               parse_tree_t *node, n00bb_tu_state_t *state);
parse_tree_t *top_level_statement_child(parse_grammar_t *grammar,
                                        parse_tree_t *wrapper);
bool cache_enum_item_value(parse_grammar_t *grammar,
                           n00bb_tu_state_t *state, parse_tree_t *node);
bool validate_package_body_statement(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node, parse_tree_t *parent);
void cache_wrapped_symbol_resolution(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *node);
bool bind_builtin_reference(parse_grammar_t *grammar,
                            n00bb_tu_state_t *state, parse_tree_t *node);
th_type *expected_expression_child_type(parse_grammar_t *grammar,
                                        parse_tree_t *parent,
                                        parse_tree_t *node,
                                        th_type *expected);
core_string_t store_semantic_string(n00bb_tu_state_t *state,
                                    core_string_t text);
bool cache_package_export_syntax(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node);
bool cache_package_declaration_syntax(parse_grammar_t *grammar,
                                      core_translation_unit_t *unit,
                                      parse_tree_t *node);
bool package_tree_may_declare(parse_grammar_t *grammar, parse_tree_t *tree,
                              core_string_t name);
bool package_tree_syntax_index(parse_grammar_t *grammar, parse_tree_t *tree,
                               core_buffer_t *names);
bool n00bb_tu_remember_selected_when_body(void *opaque,
    core_translation_unit_t *unit, parse_tree_t *body);
bool n00bb_tu_remember_build_time_use(void *opaque,
    core_translation_unit_t *unit, parse_tree_t *node);
bool n00bb_tu_build_time_query_at(core_translation_unit_t *unit,
    uint32_t line, uint32_t column);
void finalize_module_inference(core_translation_unit_t *unit);
void propagate_tree_type_to_wrappers(parse_tree_t *node, th_ctx *types,
                                     th_type *previous, th_type *type);
void propagate_symbol_type(parse_sym_entry_t *symbol, th_ctx *types,
                           th_type *previous, th_type *type);
bool validate_type_value_namespaces(core_translation_unit_t *unit);
bool validate_user_litmod_signatures(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit);
th_type *n00bb_literal_site_type(th_ctx *types);
bool n00bb_litmod_takes_site(th_ctx *types, th_type *signature);

static inline n00bb_tu_state_t *
tu_state(core_translation_unit_t *unit)
{
    return unit ? core_translation_unit_user_data(unit) : nullptr;
}

static inline bool
nt_is(parse_grammar_t *grammar, parse_tree_t *node, core_string_t name)
{
    return node && !core_tree_is_leaf(node)
        && core_string_eq(parse_tree_nt_name(grammar, node), name);
}

static inline parse_tree_t *
child_nt(parse_grammar_t *grammar, parse_tree_t *node, core_string_t name)
{
    return parse_tree_find_flat_child_by_nt_name(grammar, node, name);
}

static inline core_string_t
first_token_text(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    core_option_t(core_string_t) value = token
        ? parse_token_value(token) : core_option_none(core_string_t);
    return core_option_is_set(value) ? core_option_get(value)
                                     : (core_string_t){0};
}

static inline bool
n00bb_nil_spelling(core_string_t spelling)
{
    return core_string_eq(spelling, CORE_STRING_STATIC("nil"))
        || core_string_eq(spelling, CORE_STRING_STATIC("null"))
        || core_string_eq(spelling, CORE_STRING_STATIC("None"));
}

static inline core_string_t
n00bb_canonical_logical_spelling(core_string_t spelling)
{
    if (core_string_eq(spelling, CORE_STRING_STATIC("&&")))
        return CORE_STRING_STATIC("and");
    if (core_string_eq(spelling, CORE_STRING_STATIC("||")))
        return CORE_STRING_STATIC("or");
    if (core_string_eq(spelling, CORE_STRING_STATIC("not")))
        return CORE_STRING_STATIC("!");
    return spelling;
}

static inline th_type *
record_lookup_text(th_ctx *types, const th_type *record, core_string_t name)
{
    if (!record || !name.data) return nullptr;
    return types ? th_record_lookup_in(types, record, name)
                 : th_record_lookup(record, name);
}

static inline th_type *
var_named_text(th_ctx *types, core_string_t name)
{
    return th_var_named(types, name);
}
