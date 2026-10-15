#pragma once

#include <string.h>
#include <limits.h>

#include "lib/alloc.h"
#include "lib/arena.h"
#include "lib/dict.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "lib/diagnostic.h"
#include "logic/clpfd_store.h"
#include "parse/annot_walk.h"
#include "parse/tree_util.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "ir/libir.h"
#include "th/environment.h"
#include "signature_typevars.h"

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
    const char *name;
    bool positional;
} n00bb_loop_binding_projection_t;
void collect_for_bindings(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(parse_tree_ptr_t) *bindings);
th_type *record_template_type(th_ctx *types, const char *name,
                              parse_sym_entry_t *symbol);
th_type *variant_template_type(th_ctx *types, const char *name,
                               parse_sym_entry_t *symbol);
th_type *n00bb_variant_arm_payload_type(th_ctx *types, th_type *variant,
                                        parse_sym_entry_t *arm);
bool cache_record_layout_request(parse_grammar_t *grammar,
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
                             const char **name, size_t *name_size,
                             uint8_t *flags, th_mode *mode);
bool function_parameter_named(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol,
                              parse_tree_t *formals, core_string_t name,
                              size_t *index);
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
    N00BB_LITERAL_RICH,
    N00BB_LITERAL_U32,
} n00bb_literal_handler_t;

typedef struct {
    const char *prefix;
    n00bb_literal_shape_t shape;
    n00bb_literal_handler_t handler;
    const char *canonical_name;
} n00bb_literal_registration_t;
const n00bb_literal_registration_t *n00bb_literal_handler(
    core_string_t prefix, n00bb_literal_shape_t shape);
bool n00bb_literal_prefix_registered(core_string_t prefix);
bool register_user_litmod(void *context, core_translation_unit_t *unit,
                          parse_tree_t *declaration,
                          core_string_t modifier, core_string_t kind);
th_type *n00bb_atomic_element_type(th_type *type);
bool n00bb_option_type_element(th_type *type, th_type **element);
enum { N00BB_MAX_SIZED_BITS = sizeof(ir_uint_t) * CHAR_BIT };
bool binary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                       parse_tree_t *node,
                                       core_string_t *trait,
                                       core_string_t *method);
bool unary_operator_dispatch_for_node(parse_grammar_t *grammar,
                                      parse_tree_t *node,
                                      core_string_t *trait,
                                      core_string_t *method);
parse_tree_t *member_access_node(parse_grammar_t *grammar, parse_tree_t *node);
bool index_dispatch_for_node(parse_grammar_t *grammar, parse_tree_t *node,
                             core_string_t *trait, core_string_t *method);
bool n00bb_index_reference_storage(parse_grammar_t *grammar,
                                   parse_tree_t *node);
parse_tree_t *n00bb_index_expression_node(parse_grammar_t *grammar,
                                          parse_tree_t *node);
parse_tree_t *n00bb_addressable_root_lvalue(parse_grammar_t *grammar,
                                            parse_tree_t *target);
parse_sym_entry_t *direct_ref_target(parse_grammar_t *grammar,
                                     parse_tree_t *node);
const char *canonical_builtin_type_name(const char *name);
bool dotted_symbol_path_node(parse_grammar_t *grammar, parse_tree_t *node);
bool simple_member_chain_name(parse_grammar_t *grammar,
                              parse_tree_t *node, core_string_t *name);
int builtin_generic_type_arity(const char *name);
bool builtin_type_name(const char *name);
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
bool inside_array_dimension(parse_grammar_t *grammar, parse_tree_t *node);
core_string_t bare_type_reference_name(parse_grammar_t *grammar,
                                       parse_tree_t *node);
bool bind_array_shape_alias_source(parse_grammar_t *grammar,
                                   parse_tree_t *type_reference,
                                   parse_sym_entry_t *alias);
void bind_declared_type_from_node(parse_grammar_t *grammar,
                                  parse_tree_t *node);
parse_sym_entry_t *attached_symbol(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node);
bool n00bb_is_nil_literal(parse_grammar_t *grammar, parse_tree_t *node);
bool n00bb_bind_option_condition_shadows(parse_grammar_t *grammar,
                                         core_translation_unit_t *unit,
                                         parse_symtab_t *symtab,
                                         parse_scope_t *scope,
                                         parse_tree_t *condition, bool truth);
bool n00bb_bind_option_branch_shadow(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *body, parse_tree_t *branch);
parse_tree_t *n00bb_exiting_guard_condition(parse_grammar_t *grammar,
                                           parse_tree_t *statement);
bool n00bb_apply_option_branch_shadow(parse_grammar_t *grammar,
                                      core_translation_unit_t *unit,
                                      parse_tree_t *node);

typedef struct n00bb_link_analysis_t n00bb_link_analysis_t;
typedef enum {
    N00BB_STATIC_VALUE_BOOL,
    N00BB_STATIC_VALUE_INT,
    N00BB_STATIC_VALUE_FLOAT,
    N00BB_STATIC_VALUE_STRING,
    N00BB_STATIC_VALUE_SEQUENCE,
    N00BB_STATIC_VALUE_TUPLE,
    N00BB_STATIC_VALUE_RECORD,
    N00BB_STATIC_VALUE_SET,
    N00BB_STATIC_VALUE_DICTIONARY,
    N00BB_STATIC_VALUE_NIL,
    N00BB_STATIC_VALUE_VARIANT,
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
    } as;
} n00bb_static_value_t;
const ir_profile_t *n00bb_type_profile(void);
bool n00bb_integer_type_promotes_to(th_type *source, th_type *target);
bool n00bb_nil_type(th_ctx *types, th_type *type);
uint64_t n00bb_shift_receiver_width(th_type *type);
bool n00bb_unsigned_shift_type(th_type *type);
bool n00bb_unsigned_shift_count_type(th_type *type);
th_type *n00bb_shift_count_type(th_ctx *types, th_type *receiver);
bool n00bb_integer_interval_type(th_type *type, int64_t *minimum,
                                 int64_t *maximum);
bool n00bb_integer_interval_fits_type(th_type *type, int64_t minimum,
                                      int64_t maximum);
bool n00bb_parse_switch_integer(core_string_t text, ir_lit_t *literal);
parse_tree_t *n00bb_simple_lvalue(parse_tree_t *node);
parse_sym_entry_t *simple_expression_symbol(parse_tree_t *node);
bool n00bb_builtin_reference_function(parse_sym_entry_t *symbol);
parse_sym_entry_t *n00bb_atomic_member_lvalue_root(
    parse_grammar_t *grammar, core_translation_unit_t *unit,
    parse_tree_t *target);
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
    n00bb_alias_edges_t *escape_edges;
    n00bb_typecase_arm_types_t *typecase_arm_types;
    n00bb_ordered_nodes_t *ordered_nodes;
    n00bb_iteration_plan_t *iteration_plan;
    n00bb_litmod_site_t *literal_site;
    parse_tree_t *record_literal_origin;
    size_t first_link_use_site;
    size_t last_link_use_site;
    bool static_literal;
    bool selected_when_body;
    bool proven_above_int64;
} n00bb_node_facts_t;
typedef core_translation_unit_t *core_translation_unit_ptr_t;
core_list_decl(core_translation_unit_ptr_t);
core_list_decl(th_arg);
typedef parse_scope_t *parse_scope_ptr_t;
core_list_decl(parse_scope_ptr_t);

typedef struct {
    parse_sym_entry_t *symbol;
    int64_t minimum;
    int64_t maximum;
    bool proven_above_int64;
} n00bb_symbol_range_t;

typedef struct {
    parse_tree_t *node;
    core_dict_t incoming;
    core_dict_t merged;
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
} n00bb_user_litmod_t;
core_list_decl(n00bb_user_litmod_t);
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
    parse_tree_t **return_expressions;
    size_t return_expression_count;
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
    const char *resolved_trait_name;
    th_type *dispatch_receiver_type;
    n00bb_trait_dispatch_plan_t *closed_dispatch;
    bool dotted_path;
    bool trait_reference;
    bool variant_case_candidate;
    bool constructor_call;
    bool init_call;
    bool callback_call;
    bool callback_setup;
    bool operator_dispatch;
    bool intrinsic_operator;
    bool semantic_dispatch;
    bool secondary_dispatch;
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
    parse_tree_t *node;
    parse_sym_entry_t *symbol;
    core_translation_unit_t *source_unit;
    bool module_export;
    bool renamed;
    bool subpackage;
} n00bb_package_export_t;
core_list_decl(n00bb_package_export_t);

typedef struct n00bb_package_declaration_t {
    core_string_t name;
    parse_tree_t *node;
    parse_scope_t *scope;
    core_list_t(n00bb_package_export_t) exports;
} n00bb_package_declaration_t;
core_list_decl(n00bb_package_declaration_t);

typedef struct {
    core_string_t name;
    core_translation_unit_t *unit;
} n00bb_import_alias_t;
core_list_decl(n00bb_import_alias_t);

typedef struct {
    core_translation_unit_t *unit;
    n00bb_lex_result_t *lex;
    parse_annot_result_t *annotations;
    core_list_t(parse_tree_ptr_t) module_top_level_statements;
    core_list_t(parse_tree_ptr_t) array_type_nodes;
    core_list_t(n00bb_user_litmod_t) literal_modifiers;
    core_list_t(n00bb_package_declaration_t) package_declarations;
    core_list_t(n00bb_import_alias_t) import_aliases;
    core_dict_t import_alias_index;
    core_list_t(n00bb_link_use_site_t) link_use_sites;
    core_list_t(parse_tree_ptr_t) literal_site_nodes;
    core_dict_t flagset_values;
    core_link_unit_t *link;
    n00bb_link_analysis_t *link_analysis;
    th_ctx *types;
    ir_module_t *ir_module;
    ir_module_t *comptime_ir_module;
    core_arena_t semantic_values;
    core_csp_store_t *integer_range_store;
    parse_tree_t *integer_range_root;
    core_dict_t integer_range_variables;
    core_dict_t integer_symbol_ranges;
    core_dict_t integer_range_invalidated_symbols;
    core_list_t(n00bb_range_branch_frame_t) integer_range_branches;
    bool integer_range_use_symbol_facts;
    bool inference_finalized;
    bool interface_module;
    bool semantic_annotation_started;
    bool semantic_annotation_complete;
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
bool infer_user_literal_type(parse_grammar_t *grammar, parse_tree_t *node,
                             n00bb_tu_state_t *state);
bool resolve_forward_literal_sites(core_translation_unit_t *unit,
                                   n00bb_tu_state_t *state);
bool collect_ordered_children(parse_tree_t *node,
    core_list_t(parse_tree_ptr_t) *children);
core_string_t named_field_identifier(parse_grammar_t *grammar,
                                     parse_tree_t *field);
bool n00bb_switch_case_integer(parse_grammar_t *grammar,
                               parse_tree_t *node, ir_lit_t *value);
th_type *static_named_arguments(th_type *type, const char *expected_name,
                                size_t expected_count);
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
bool infer_anonymous_record(parse_grammar_t *grammar, parse_tree_t *node,
                            n00bb_tu_state_t *state);
bool contextualize_record_literal(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state,
                                  parse_tree_t *expression,
                                  th_type *expected);
th_type *record_literal_expected_type(parse_grammar_t *grammar,
                                      parse_tree_t *literal);
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
bool named_type_is(th_type *type, const char *wanted);
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
                                 core_string_t kind);
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
                                 int64_t minimum, int64_t maximum);
bool n00bb_cache_contextual_integer(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
th_type *n00bb_integer_literal_context(parse_grammar_t *grammar,
                                       n00bb_tu_state_t *state,
                                       parse_tree_t *literal);

bool cache_return_source_fact(parse_grammar_t *grammar, parse_tree_t *node,
                              n00bb_tu_state_t *state);
bool cache_alias_edges(parse_grammar_t *grammar, parse_tree_t *node,
                       n00bb_tu_state_t *state);
bool cache_escape_edges(parse_grammar_t *grammar, parse_tree_t *node,
                        n00bb_tu_state_t *state);

bool n00bb_current_integer_range(n00bb_tu_state_t *state, parse_tree_t *node,
                                 int64_t *minimum, int64_t *maximum);
bool n00bb_integer_representation(th_type *type, uint64_t *width,
                                  bool *is_signed);
bool n00bb_range_proven_above_int64(parse_grammar_t *grammar,
                                    n00bb_tu_state_t *state,
                                    parse_tree_t *node);
bool n00bb_contextual_integer_literal(n00bb_tu_state_t *state,
                                      parse_tree_t *node, th_type *target);
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
bool register_builtin_symbols(parse_annot_result_t *annotations,
                              n00bb_tu_state_t *state, th_ctx *types);
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
bool cache_flagset_semantics(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *node);
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
parse_sym_entry_t *canonical_symbol(parse_sym_entry_t *symbol);
parse_sym_entry_t *type_symbol_in_loaded_imports(
    core_translation_unit_t *root, th_ctx *types, th_type *wanted,
    core_translation_unit_t **owner_unit_out);
n00bb_callback_contract_t *n00bb_callback_contract_for_type(
    n00bb_tu_state_t *state, th_type *type);
th_type *n00bb_specialize_callback_role(th_ctx *types,
    n00bb_callback_contract_t *contract, th_type *receiver, th_type *role);
th_type *n00bb_callback_method_signature(
    th_ctx *types, th_type *receiver, const char *name, void *context);
bool prepare_callback_call_symbols(core_translation_unit_t *unit);
bool n00bb_builtin_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_fd_write_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_repr_default_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_native_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_init_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_string_operator(parse_sym_entry_t *symbol);
bool n00bb_builtin_array_read_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_array_write_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_byte_offset_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_byte_diff_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_wrapping_mul_function(parse_sym_entry_t *symbol);
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
bool bind_array_shape_type_source(parse_tree_t *type_reference,
                                  parse_sym_entry_t *symbol);
bool context_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol);
bool is_module_global_symbol(parse_grammar_t *grammar,
                             parse_sym_entry_t *symbol);
bool type_may_contain_reference_alias(th_ctx *types, th_type *type);
void mark_alias_symbol_may_escape(th_ctx *types,
    parse_sym_entry_t *symbol);
void mark_referent_value_may_escape(th_ctx *types,
    parse_sym_entry_t *symbol);
bool finalize_alias_free_local_summaries(parse_grammar_t *grammar,
    core_translation_unit_t *unit);
parse_sym_entry_t *cached_argument_value_source(
    n00bb_tu_state_t *state, parse_tree_t *argument);
void mark_expression_aliases_may_escape(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
parse_tree_t *aggregate_literal_root(parse_grammar_t *grammar,
    parse_tree_t *expression);
bool record_aggregate_aliases(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node,
    parse_sym_entry_t *holder);
void record_reference_facts(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
bool semantic_annotate_tree(parse_grammar_t *grammar, th_ctx *types,
    core_translation_unit_t *unit, parse_tree_t *root);
typedef bool (*n00bb_semantic_unit_finish_t)(void *context,
    core_translation_unit_t *unit);
bool semantic_annotate_closure(parse_grammar_t *grammar, th_ctx *types,
    core_translation_unit_t *unit, n00bb_semantic_unit_finish_t finish,
    void *context);
bool is_reference_symbol(parse_sym_entry_t *symbol);
bool is_integer_type(th_type *type);
bool validate_cached_array_dimensions(parse_grammar_t *grammar,
    core_translation_unit_t *unit, n00bb_tu_state_t *state);
bool select_storage_classes(core_list_t(core_translation_unit_ptr_t) *units);
bool node_is_within(parse_tree_t *node, parse_tree_t *ancestor);
void collect_ir_parameters(parse_grammar_t *grammar, parse_tree_t *node,
    core_list_t(parse_sym_entry_ptr_t) *parameters,
    parse_sym_entry_t **rest);
bool context_hook_name(parse_sym_entry_t *symbol, const char *suffix,
    char *buffer, size_t size);
bool context_default_name(parse_sym_entry_t *symbol, size_t index,
    char *buffer, size_t size);
bool callback_protocol_name(const char *type_name, const char *marker,
    core_buffer_t *out);
n00bb_node_facts_t *n00bb_node_facts(parse_tree_t *node);
n00bb_ordered_nodes_t *n00bb_node_ordered_nodes(parse_tree_t *node);
n00bb_litmod_site_t *n00bb_node_literal_site(parse_tree_t *node);
parse_tree_t *n00bb_node_record_literal_origin(parse_tree_t *node);
parse_sym_entry_t *bound_function_symbol(parse_tree_t *node);
n00bb_node_facts_t *n00bb_ensure_node_facts(n00bb_tu_state_t *state,
    parse_tree_t *node);
n00bb_return_source_t *n00bb_node_return_source(parse_tree_t *node);
n00bb_alias_edges_t *n00bb_node_alias_edges(parse_tree_t *node);
n00bb_alias_edges_t *n00bb_node_escape_edges(parse_tree_t *node);
const char *n00bb_context_hook_role(const char *name);
bool alias_declaration_is_transparent(parse_tree_t *declaration);
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
bool symbol_is_public_declaration(parse_sym_entry_t *symbol);
th_env_visibility_t symbol_visibility(parse_sym_entry_t *symbol);
parse_tree_t *symbol_binding_node(parse_sym_entry_t *symbol);
bool symbol_has_definition(parse_sym_entry_t *symbol, parse_tree_t *node);
bool scope_in_value_namespace(parse_scope_t *scope);
bool is_named_type_declaration(parse_grammar_t *grammar,
                               parse_sym_entry_t *symbol);
bool field_record_declaration(parse_grammar_t *grammar, parse_tree_t *node);
parse_sym_phase_t lexical_execution_phase(parse_grammar_t *grammar,
                                          parse_tree_t *node);
bool declared_in_comptime(parse_grammar_t *grammar, parse_tree_t *node);
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
const char *source_builtin_type_name(const char *name);
const char *n00bb_print_type(th_ctx *types, th_type *type);
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
uint32_t initializer_phase_for_site(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *site,
    parse_sym_entry_t *initializer_owner);
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
void finalize_module_inference(core_translation_unit_t *unit);
void propagate_tree_type_to_wrappers(parse_tree_t *node, th_ctx *types,
                                     th_type *previous, th_type *type);
void propagate_symbol_type(parse_sym_entry_t *symbol, th_ctx *types,
                           th_type *previous, th_type *type);
bool validate_type_value_namespaces(core_translation_unit_t *unit);
bool validate_user_litmod_signatures(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit);

static inline n00bb_tu_state_t *
tu_state(core_translation_unit_t *unit)
{
    return unit ? core_translation_unit_user_data(unit) : nullptr;
}

static inline bool
nt_is(parse_grammar_t *grammar, parse_tree_t *node, const char *name)
{
    return node && !core_tree_is_leaf(node)
        && core_string_eq(parse_tree_nt_name(grammar, node),
                          core_string_borrow(name, strlen(name)));
}

static inline parse_tree_t *
child_nt(parse_grammar_t *grammar, parse_tree_t *node, const char *name)
{
    return parse_tree_find_flat_child_by_nt_name(grammar, node,
        core_string_borrow(name, strlen(name)));
}

static inline core_string_t
first_token_text(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    return token && core_option_is_set(token->value)
        ? core_option_get(token->value) : (core_string_t){0};
}
