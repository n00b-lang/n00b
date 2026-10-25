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
#define N00BB_RESULT_KEY CORE_STRING_STATIC("n00bb.result")
#define N00BB_CASE_METHOD CORE_STRING_STATIC("__is")
#define N00BB_REF_METHOD CORE_STRING_STATIC("__ref")
ir_instr_t *n00bb_declared_result_default(ir_instr_t *write);
core_diag_location_t n00bb_name_location(core_translation_unit_t *unit,
                                         ir_span_t span, core_string_t name);
#define N00BB_CASE_KEY CORE_STRING_STATIC("n00bb.case")
#define N00BB_CASE_INDEX_KEY CORE_STRING_STATIC("n00bb.case.index")
#define N00BB_CASE_PATTERNS_KEY CORE_STRING_STATIC("n00bb.case.patterns")
#define N00BB_SWITCH_KEY CORE_STRING_STATIC("n00bb.switch")
#define N00BB_SWITCH_VARIABLE_KEY CORE_STRING_STATIC("n00bb.switch.variable")
#define N00BB_SWITCH_EXPRESSION_KEY CORE_STRING_STATIC("n00bb.switch.expression")
#define N00BB_SWITCH_SUBJECT_KEY CORE_STRING_STATIC("n00bb.switch.subject")
#define N00BB_SWITCH_MISSING_KEY CORE_STRING_STATIC("n00bb.switch.missing")
#define N00BB_CASE_KIND_KEY CORE_STRING_STATIC("n00bb.case.kind")
#define N00BB_CONDITION_KEY CORE_STRING_STATIC("n00bb.condition")
#define N00BB_FAILED_KEY CORE_STRING_STATIC("n00bb.failed")
#define N00BB_UNTYPED_CONST_KEY CORE_STRING_STATIC("n00bb.untyped-const")
#define N00BB_PATTERN_ARITY_KEY CORE_STRING_STATIC("n00bb.pattern-arity")
#define N00BB_LOOP_OUTER_NAME_KEY CORE_STRING_STATIC("n00bb.loop-outer-name")
#define N00BB_FIELD_LITERAL_SET_KEY CORE_STRING_STATIC("n00bb.field-literal-set")
#define N00BB_STATEMENT_KEY CORE_STRING_STATIC("n00bb.statement")
#define N00BB_COMPARISON_KEY CORE_STRING_STATIC("n00bb.comparison")
#define N00BB_POISONED_KEY CORE_STRING_STATIC("n00bb.poisoned")
#define N00BB_REFINED_UNION_KEY CORE_STRING_STATIC("n00bb.refined-union")
#define N00BB_TRY_ERROR_ANNOTATION "n00bb.try-error"

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
typedef ir_instr_t *n00bb_ir_instr_ptr_t;
core_list_decl(n00bb_ir_instr_ptr_t);
core_list_decl(n00bb_tree_ptr_t);
typedef struct {
    size_t index;
    core_string_t name;
    bool positional;
    size_t arity;
} n00bb_projection_step_t;
core_list_decl(n00bb_projection_step_t);
void collect_for_bindings(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(parse_tree_ptr_t) *bindings);
parse_tree_t *n00bb_pattern_owner(parse_grammar_t *grammar,
                                  parse_tree_t *node);
bool n00bb_pattern_path(parse_grammar_t *grammar, parse_tree_t *target,
                        core_list_t(n00bb_projection_step_t) *path);
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
bool n00bb_type_finalized(core_translation_unit_t *unit, th_type *type);
bool n00bb_type_may_finalize(core_translation_unit_t *unit, th_type *type);
bool n00bb_reference_hook(th_ctx *types, th_type *receiver,
                          core_string_t name, th_env_method_t *method,
                          th_type **result);
bool n00bb_reference_method(th_ctx *types, th_type *receiver, bool writable,
                            th_env_method_t *method, th_type **referent);
bool n00bb_type_finalizer(core_translation_unit_t *unit, th_type *type,
                          th_env_method_t *method);
typedef bool (*n00bb_has_finalizer_fn)(void *context, th_type *type);
th_type *n00bb_finalize_record_shape(th_ctx *types, th_type *type, bool open,
                                     bool named_only);
bool n00bb_finalize_reaches(th_ctx *types, th_type *type, bool open,
                            bool named_only,
                            n00bb_has_finalizer_fn has_finalizer,
                            void *context);
void collect_keyword_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                           core_list_t(n00bb_tree_ptr_t) *arguments);
core_string_t call_keyword_name(parse_tree_t *argument);
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
parse_tree_t *member_access_node(parse_grammar_t *grammar, parse_tree_t *node);
parse_tree_t *referent_access_node(parse_grammar_t *grammar,
                                   parse_tree_t *node);
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
core_string_t canonical_builtin_type_name(core_string_t name);
bool dotted_symbol_path_node(parse_grammar_t *grammar, parse_tree_t *node);
int builtin_generic_type_arity(core_string_t name);
bool builtin_type_name(core_string_t name);
bool is_named_type_symbol(parse_grammar_t *grammar,
                          parse_sym_entry_t *symbol);
bool is_trait_type_symbol(parse_grammar_t *grammar,
                          parse_sym_entry_t *symbol);
th_type *trait_type_reference(parse_grammar_t *grammar,
                              core_translation_unit_t *use_unit,
                              core_translation_unit_t *declaration_unit,
                              parse_sym_entry_t *symbol,
                              parse_tree_t *reference);
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
bool bind_array_shape_alias_source(parse_grammar_t *grammar,
                                   parse_tree_t *type_reference,
                                   parse_sym_entry_t *alias);
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
bool n00bb_bitwise_type(th_type *type);
th_type *n00bb_shift_count_type(th_ctx *types, th_type *receiver);
bool n00bb_parse_switch_integer(core_string_t text, ir_lit_t *literal);
parse_tree_t *n00bb_simple_lvalue(parse_tree_t *node);
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
typedef struct {
    parse_sym_entry_t *call;
    parse_sym_entry_t *handler;
    parse_sym_entry_t *setup;
    th_type *state_type;
    bool stateful;
} n00bb_callback_contract_t;
typedef struct {
    th_type **types;
    size_t count;
} n00bb_typecase_arm_types_t;
typedef struct n00bb_ordered_nodes_t n00bb_ordered_nodes_t;
typedef struct n00bb_iteration_plan_t n00bb_iteration_plan_t;
typedef struct n00bb_litmod_site_t n00bb_litmod_site_t;
typedef struct {
    n00bb_typecase_arm_types_t *typecase_arm_types;
    n00bb_ordered_nodes_t *ordered_nodes;
    n00bb_iteration_plan_t *iteration_plan;
    n00bb_litmod_site_t *literal_site;
    size_t first_link_use_site;
    size_t last_link_use_site;
    bool static_literal;
    bool selected_when_body;
} n00bb_node_facts_t;
typedef core_translation_unit_t *core_translation_unit_ptr_t;
core_list_decl(core_translation_unit_ptr_t);
core_list_decl(th_arg);
typedef parse_scope_t *parse_scope_ptr_t;
core_list_decl(parse_scope_ptr_t);

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
};
typedef struct {
    parse_tree_t *node;
    ir_global_t *global;
} n00bb_hoisted_literal_t;
core_list_decl(n00bb_hoisted_literal_t);

typedef struct {
    parse_tree_t *node;
    bool variadic;
    bool has_type_node;
} n00bb_function_parameter_ref_t;

typedef struct {
    n00bb_function_parameter_ref_t *params;
    size_t param_count;
    size_t param_capacity;
    bool has_inferred_parameter;
} n00bb_function_parameter_facts_t;

struct n00bb_iteration_plan_t {
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
    core_string_t origin_module;
    bool dotted_path;
    bool trait_reference;
    bool variant_case_candidate;
    bool constructor_call;
    bool unresolved;
    bool init_call;
    bool callback_call;
    bool callback_setup;
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
    // The module a module literal generated: its public declarations are
    // members of the importing module.
    bool injected;
    // For a module literal: its modifier, the byte span of its text in the
    // importing module's source and where that text starts, so a cached
    // build can generate it again.
    core_string_t literal_modifier;
    size_t literal_start;
    size_t literal_end;
    uint32_t literal_line;
    uint32_t literal_column;
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
    core_translation_unit_t *provider;
    parse_scope_t *values;
    parse_scope_t *types;
    const n00bb_brought_name_t *brought;
} n00bb_import_source_t;
core_list_decl(n00bb_import_source_t);

typedef struct {
    parse_tree_t *target;
    parse_sym_entry_t *root;
    ir_instr_t *store;
    core_string_t field;
} n00bb_write_site_t;
core_list_decl(n00bb_write_site_t);

typedef struct {
    core_translation_unit_t *unit;
    n00bb_lex_result_t *lex;
    parse_annot_result_t *annotations;
    core_list_t(parse_tree_ptr_t) module_top_level_statements;
    core_list_t(parse_tree_ptr_t) mark_annotations;
    core_list_t(parse_tree_ptr_t) assignment_patterns;
    core_list_t(parse_tree_ptr_t) declaration_patterns;
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
    uint32_t syntax_spelling_lines[PP_SPELL_COUNT];
    core_list_t(n00bb_import_source_t) import_sources;
    bool import_sources_ready;
    core_dict_t constructor_types;
    core_dict_t integer_tracked_symbols;
    core_dict_t dispatch_calls;
    core_list_t(parse_tree_ptr_t) integer_shift_sites;
    core_dict_t integer_shift_seen;
    core_list_t(parse_tree_ptr_t) literal_site_nodes;
    core_list_t(n00bb_hoisted_literal_t) hoisted_literals;
    core_dict_t *deferred_consts;
    core_list_t(parse_tree_ptr_t) shape_literal_nodes;
    core_dict_t flagset_values;
    core_dict_t shared_state_types;
    core_dict_t declarations_typed_ahead;
    core_link_unit_t *link;
    n00bb_link_analysis_t *link_analysis;
    th_ctx *types;
    ir_module_t *ir_module;
    core_arena_t semantic_values;
    parse_grammar_t *grammar;
    bool inference_finalized;
    core_buffer_t declaration_sites;
    core_buffer_t unused_facts;
    bool interface_module;
    bool import_failure_reported;
    bool package_interface;
    bool package_manifest_restored;
    bool semantic_annotation_started;
    bool semantic_annotation_complete;
    bool initializer_excluded;
    bool semantic_annotation_failed;
    bool type_symbols_indexed;
    core_dict_t type_symbol_index;
    parse_tree_t *flagset_scope;
    core_dict_t finalized_types;
    core_list_t(n00bb_ir_instr_ptr_t) rewrite_sites;
    core_list_t(n00bb_write_site_t) write_sites;
    core_list_t(n00bb_ir_instr_ptr_t) result_defaults;
    core_dict_t intrinsics;
    core_dict_t open_storage;
} n00bb_tu_state_t;
bool n00bb_untyped_integer_const(parse_grammar_t *grammar,
                                 const parse_sym_entry_t *symbol,
                                 __int128 *value);
bool n00bb_trivial_static_value(parse_grammar_t *grammar,
                                n00bb_tu_state_t *state,
                                parse_tree_t *expression,
                                n00bb_static_value_t *out);
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
parse_scope_t *n00bb_variant_scope_for_type(core_translation_unit_t *unit,
                                            n00bb_tu_state_t *state,
                                            th_type *type);
bool n00bb_variant_arm_symbol(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol);
core_list_t(parse_sym_entry_ptr_t) variant_arms_in_source_order(
    parse_grammar_t *grammar, parse_scope_t *scope);
bool report_shadowed_litmods(core_translation_unit_t *unit,
                             n00bb_tu_state_t *state);
bool n00bb_declares_litmod(n00bb_tu_state_t *state,
                           parse_sym_entry_t *symbol);
bool n00bb_declares_litmod_alias(n00bb_tu_state_t *state, core_string_t name);
bool collect_ordered_children(parse_tree_t *node,
    core_list_t(parse_tree_ptr_t) *children);
core_string_t named_field_identifier(parse_grammar_t *grammar,
                                     parse_tree_t *field);
bool n00bb_switch_case_integer(parse_grammar_t *grammar,
                               parse_tree_t *node, ir_lit_t *value);
bool n00bb_switch_case_is_char(parse_grammar_t *grammar, parse_tree_t *node);
th_type *static_sequence_element_type(th_type *type);
bool static_value_matches_type(const n00bb_static_value_t *value,
                               th_type *type, n00bb_tu_state_t *state,
                               size_t depth);
th_type *static_record_shape(n00bb_tu_state_t *state, th_type *type);
th_type *n00bb_record_shape(n00bb_tu_state_t *state, th_type *type);
bool n00bb_type_has_constructor(core_translation_unit_t *unit, th_type *type);
void remember_link_use_site(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            n00bb_tu_state_t *state, parse_tree_t *node);
parse_tree_t *call_arguments_node(parse_grammar_t *grammar,
                                  parse_tree_t *call);
void collect_positional_nodes(parse_grammar_t *grammar,
                              parse_tree_t *node,
                              core_list_t(n00bb_tree_ptr_t) *arguments);
parse_tree_t *n00bb_call_positional_argument(parse_grammar_t *grammar,
                                             parse_tree_t *call, size_t index,
                                             size_t arity);
bool n00bb_call_literal_argument(parse_grammar_t *grammar,
                                 n00bb_tu_state_t *state, parse_tree_t *call,
                                 size_t index, size_t arity,
                                 core_string_t *out);
bool n00bb_scalar_role_is(th_type *type, ir_role_t role);
bool n00bb_flagset_type(core_translation_unit_t *unit,
                        n00bb_tu_state_t *state, th_type *type,
                        parse_sym_entry_t **symbol_out);
bool index_is_compound_target(parse_grammar_t *grammar,
                              parse_tree_t *node);
bool module_initializer_declaration(parse_grammar_t *grammar,
                                    parse_tree_t *node);
bool is_module_initializer_use(parse_grammar_t *grammar,
                               parse_tree_t *node,
                               parse_sym_entry_t *initializer_owner);
bool initializer_node_is_reference_store_target(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node,
    parse_sym_entry_t *symbol);
void validate_builtin_generic_type_arity(parse_grammar_t *grammar,
                                         parse_tree_t *node,
                                         core_translation_unit_t *unit,
                                         th_type *type);
bool n00bb_flagset_member(parse_sym_entry_t *symbol);
bool n00bb_flagset_instance_access(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *node,
                                   parse_sym_entry_t *member);
core_string_t n00bb_literal_shape_trait(core_string_t kind);
core_string_t literal_modifier(parse_grammar_t *grammar,
                               parse_tree_t *node);
bool dict_only_empty_literal(parse_grammar_t *grammar,
                             parse_tree_t *node);
bool literal_modifier_terminal(parse_grammar_t *grammar,
                               parse_tree_t *leaf,
                               parse_tree_t *parent);
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
parse_sym_entry_t *lookup_named_type_reference(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_symtab_t *symtab, parse_scope_t *scope,
    core_string_t name);
typedef struct {
    int64_t value;
    bool is_unsigned;
} n00bb_untyped_integer_t;


bool n00bb_type_interval(th_type *type, ir_interval_t *range);
#define N00BB_INTEGER_WIDTHS 5
bool n00bb_integer_representation(th_type *type, uint64_t *width,
                                  bool *is_signed);
static constexpr char n00bb_string_to_buffer_text[] =
    "a string is immutable and does not convert to Buffer; use "
    "buffer_from_string(s)";
static constexpr char n00bb_buffer_type_name[] =
    "builtin.containers.buffer.Buffer";
bool n00bb_value_coerces(th_ctx *types, th_type *from, th_type *to);
bool n00bb_reference_views(th_type *from, th_type *to);
bool n00bb_string_where_buffer_expected(th_ctx *types, th_type *expected,
                                        th_type *actual);
parse_tree_t *n00bb_string_where_buffer_site(parse_grammar_t *grammar,
                                             th_ctx *types,
                                             parse_tree_t *node,
                                             th_type *expected);
void cache_type_syntax_node(parse_grammar_t *grammar, parse_tree_t *node);
// Declares a cached module's module literals' names into it, once its
// interface is restored; its generated modules' interfaces come first.
bool n00bb_inject_interface_literals(core_translation_unit_t *unit);
bool register_native_declarations(parse_annot_result_t *annotations,
                                  th_ctx *types,
                                  n00bb_native_declarations_t natives);
// Notes what the analyses know about natives: pure, or their declared
// effects.
void n00bb_note_native_axioms(n00bb_native_declarations_t natives);
bool register_builtin_symbols(parse_annot_result_t *annotations,
                              n00bb_tu_state_t *state, th_ctx *types);
bool n00bb_register_operator_function(th_env *environment, th_ctx *types,
                                      core_string_t trait, core_string_t method,
                                      core_string_t owner, th_type *self,
                                      th_type *other, th_type *result,
                                      core_string_t module, core_string_t package,
                                      th_env_visibility_t visibility);
bool register_builtin_traits(th_env *environment, th_ctx *types);
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
bool finalize_flagset_symbol_type(parse_grammar_t *grammar,
                                  parse_tree_t *node,
                                  core_translation_unit_t *unit,
                                  n00bb_tu_state_t *state);
bool n00bb_node_bytes(parse_tree_t *node, size_t *start, size_t *end);
core_string_t n00bb_node_source_text(const core_translation_unit_t *unit,
                                     parse_tree_t *node,
                                     core_string_t fallback);
bool cache_flagset_semantics(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *node);
bool n00bb_flagset_first_problem(parse_sym_entry_t *owner);

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

static inline bool
n00bb_unit_failed(core_translation_unit_t *unit)
{
    return unit
        && core_diagnostics_failed(core_translation_unit_diagnostics(unit));
}

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
bool n00bb_callback_rejected(n00bb_tu_state_t *state, th_type *type);
th_type *n00bb_specialize_callback_role(th_ctx *types,
    n00bb_callback_contract_t *contract, th_type *receiver, th_type *role);
th_type *n00bb_callback_method_signature(
    th_ctx *types, th_type *receiver, core_string_t name, void *context);
bool prepare_callback_call_symbols(core_translation_unit_t *unit);
bool n00bb_builtin_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_native_function(parse_sym_entry_t *symbol);
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
bool n00bb_builtin_comptime_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_prefetch_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_rmw_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_cas_function(parse_sym_entry_t *symbol);
bool n00bb_builtin_atomic_access_function(parse_sym_entry_t *symbol);
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
bool context_hook_name(parse_sym_entry_t *symbol, core_string_t suffix,
    core_buffer_t *out);
bool context_default_name(parse_sym_entry_t *symbol, size_t index,
    core_buffer_t *out);
bool callback_protocol_name(core_string_t type_name, core_string_t marker,
    core_buffer_t *out);
n00bb_node_facts_t *n00bb_node_facts(parse_tree_t *node);
n00bb_ordered_nodes_t *n00bb_node_ordered_nodes(parse_tree_t *node);
n00bb_litmod_site_t *n00bb_node_literal_site(parse_tree_t *node);
n00bb_litmod_site_t *n00bb_explicit_literal_site(parse_grammar_t *grammar,
                                                 core_translation_unit_t *unit,
                                                 parse_tree_t *node);
parse_sym_entry_t *bound_function_symbol(parse_tree_t *node);
th_type *n00bb_reference_definition(th_ctx *types, th_type *type);
th_type *n00bb_dereferenced_type(th_ctx *types, th_type *type);
bool n00bb_object_type(th_ctx *types, th_type *type);
th_type *n00bb_shape_object(th_ctx *types, th_type *type);
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
const n00bb_field_declaration_types_t *
n00bb_record_types(parse_sym_entry_t *symbol);
#define n00bb_object_shape_suffix ".__fields"
#define n00bb_view_blocked_binding CORE_STRING_STATIC("n00bb.view-blocked")
bool scope_is_within_function(parse_scope_t *scope);
bool scope_is_within(parse_scope_t *scope, parse_scope_t *ancestor);
bool scope_has_lexical_lifetime(parse_scope_t *scope);
parse_tree_t *simple_identifier_primary(parse_grammar_t *grammar,
                                        parse_tree_t *node);
bool simple_identifier_expression(parse_grammar_t *grammar,
                                  parse_tree_t *node, core_string_t *name);
core_string_t n00bb_print_type(th_ctx *types, th_type *type);
core_string_t n00bb_print_type_qualified(th_ctx *types, th_type *type);
core_string_t n00bb_print_type_for_user(th_ctx *types, th_type *type);
core_string_t n00bb_hide_reserved_segments(core_string_t printed);
th_type *function_with_error_channel(th_ctx *types, th_type *function,
    th_type *error_type);
parse_sym_entry_t *formal_symbol_at(parse_grammar_t *grammar,
    parse_tree_t *node, size_t target, size_t *index);
parse_sym_entry_t *formal_rest_symbol(parse_grammar_t *grammar,
                                      parse_tree_t *node);
void record_function_signature(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *func,
    n00bb_tu_state_t *state);
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
th_type *signature_typevar_for_node(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
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
void link_use_site_bind(n00bb_link_use_site_t *site,
    parse_sym_entry_t *symbol);
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
bool n00bb_define_callback_type(th_ctx *types, th_type *callback);
th_type *n00bb_symbol_signature(parse_sym_entry_t *symbol);
th_type *n00bb_symbol_contract(th_ctx *types, parse_sym_entry_t *symbol);
th_type *callback_setup_dispatch_contract(th_ctx *types,
    th_type *setup_type);
void initial_symbol_node(parse_visit_t *visit, bool entering, void *context);
void declared_symbol_node(parse_visit_t *visit, bool entering, void *context);
bool prepare_initial_symbols(core_translation_unit_t *unit, th_ctx *types);
parse_tree_t *n00bb_simple_switch_literal(parse_grammar_t *grammar,
                                          parse_tree_t *node,
                                          bool *negative, bool *saw_sign);
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

typedef struct {
    core_string_t action;
    core_string_t literal;
    uint32_t      operands[2];
    size_t        operand_count;
} n00bb_emit_t;

bool n00bb_node_emit(parse_grammar_t *grammar, parse_tree_t *node,
                     n00bb_emit_t *emit);

static inline bool
n00bb_name_place(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("place"))
        && core_tree_num_children(node) == 1;
}

static inline bool
n00bb_postfix_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("postfix-expr"))
        || nt_is(grammar, node, CORE_STRING_STATIC("place"));
}

static inline bool
n00bb_primary_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("primary"))
        || n00bb_name_place(grammar, node);
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
