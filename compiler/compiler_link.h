#pragma once

#include "compiler_effects.h"
#include "compiler_marks.h"
#include "compiler_tu.h"
#include "driver.h"
#include "abi_record_layout.h"
#include "llvm_backend.h"
#include "specialize.h"
#include "ir/archive.h"
#include "ir/pdg.h"
#include "lib/dict.h"

#define N00BB_LIBRARY_IR_KEY "n00b.library-ir"
#define N00BB_LIBRARY_ABI_KEY "n00b.library-abi"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "lib/target.h"
#include "parse/annot_walk.h"
#include "parse/grammar.h"

typedef struct {
    const ir_func_t *function;
    core_string_t    field;
    th_type         *owner;
} n00bb_bookkeeping_write_t;
core_list_decl(n00bb_bookkeeping_write_t);
#include "th/environment.h"

typedef struct {
    core_string_t module;
    core_string_t declaration;
} n00bb_export_origin_t;

bool append_u32_le(core_buffer_t *buffer, uint32_t value);
uint32_t read_u32_le(const uint8_t *data);
bool function_interface_parameter(parse_sym_entry_t *symbol, size_t target,
                                  core_string_t *name,
                                  uint8_t *flags, uint8_t *mode);
size_t function_interface_parameter_count(parse_sym_entry_t *symbol);
bool function_interface_parameter_named(parse_sym_entry_t *symbol,
                                        core_string_t target, size_t *index);
bool context_interface_parameter(parse_sym_entry_t *symbol, size_t target,
                                 core_string_t *name,
                                 uint8_t *flags, uint8_t *mode);
size_t context_interface_parameter_count(parse_sym_entry_t *symbol);
bool context_interface_parameter_named(parse_sym_entry_t *symbol,
                                       core_string_t target, size_t *index);
bool call_receiver_is_module(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *receiver);
bool n00bb_pending_type_reference(n00bb_tu_state_t *state,
                                  parse_tree_t *node);
bool resolve_module_member_site(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node);
bool resolve_imported_module_symbols(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units);
bool module_type_universe_is_shared(
    core_list_t(core_translation_unit_ptr_t) *units);
bool native_root_matches(core_string_t qualified, core_string_t module,
    core_string_t name);
bool n00bb_unit_reaches_import(core_translation_unit_t *unit,
                               core_translation_unit_t *target);
bool global_has_explicit_initializer(parse_grammar_t *grammar,
    parse_sym_entry_t *symbol);
bool register_unit_environment(parse_grammar_t *grammar,
    th_env *environment, core_translation_unit_t *unit);
th_env *build_import_environment(parse_grammar_t *grammar,
    core_link_unit_t *link, n00bb_link_analysis_t *analysis);
typedef enum {
    N00BB_CALL_ARGUMENT,
    N00BB_CALL_RECEIVER,
    N00BB_CALL_RESULT,
    N00BB_CALL_SIGNATURE,
} n00bb_call_role_t;
typedef struct {
    parse_tree_t *argument;
    th_type *expected;
    bool keyword;
    bool writes;
    th_mode mode;
    size_t parameter;
    core_string_t parameter_name;
    bool rest;
} n00bb_call_binding_t;
core_list_decl(n00bb_call_binding_t);
bool call_units_failed(core_translation_unit_t *const *units,
    size_t unit_count);
bool publish_link_call_dependencies(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t *state_out);
parse_tree_t *call_argument_for_parameter(parse_grammar_t *grammar,
    parse_tree_t *call, parse_sym_entry_t *function, size_t target,
    bool trait_dispatch);
core_link_declaration_t *linked_method_declaration(core_link_unit_t *link,
    const th_env_method_t *method);
typedef enum {
    N00BB_REQUIREMENT_DOES_NOT_FIT,
    N00BB_REQUIREMENT_FITS_WITH_DEFAULTS,
    N00BB_REQUIREMENT_FITS_EXACTLY,
} n00bb_requirement_fit_t;
parse_sym_entry_t *trait_method_requirement_symbol(
    parse_grammar_t *grammar, n00bb_link_analysis_t *analysis,
    core_translation_unit_t *caller, core_string_t qualified_trait,
    core_string_t method_name);
bool n00bb_field_shared_outside(parse_sym_entry_t *field);
bool n00bb_field_use_is_write(parse_grammar_t *grammar,
    parse_sym_entry_t *field, parse_tree_t *use, bool *also_reads);
th_type *constructor_signature_instance(th_ctx *types,
    parse_sym_entry_t *symbol);
th_type *link_semantic_type_for_symbol(parse_grammar_t *grammar,
                                       core_translation_unit_t *owner,
                                       parse_sym_entry_t *symbol);
bool finalize_named_declaration_types(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units);
core_translation_unit_t *unit_for_ir_module(n00bb_link_analysis_t *analysis,
                                            ir_module_t *module);
core_translation_unit_t *n00bb_unit_named(n00bb_link_analysis_t *analysis,
                                          core_string_t name);

typedef struct {
    parse_tree_t *site;
    parse_tree_t *value;
    core_diag_location_t location;
    core_string_t provenance;
} n00bb_initializer_fact_t;
core_list_decl(n00bb_initializer_fact_t);

typedef struct {
    parse_sym_entry_t *symbol;
    th_type *semantic_type;
    n00bb_static_value_t *static_value;
    parse_sym_storage_duration_t storage_duration;
    core_string_t requested_storage_class;
    core_string_t selected_storage_class;
    bool storage_class_required;
    bool is_thread_local;
    bool facts_complete;
    bool array_shape_complete;
    core_list_t(n00bb_initializer_fact_t) initializers;
} n00bb_link_payload_t;

typedef struct {
    parse_tree_t *instantiation_site;
    parse_tree_t *trait_site;
    parse_sym_entry_t *generic_function;
    th_type *receiver_type;
    th_type *failure_type;
} n00bb_trait_specialization_t;
parse_sym_entry_t *unit_trait_symbol(parse_grammar_t *grammar,
    core_translation_unit_t *unit, core_string_t name,
    core_translation_unit_t *consumer);
bool n00bb_resolve_trait_reference(parse_grammar_t *grammar,
    core_translation_unit_t *unit, n00bb_link_use_site_t *site,
    core_translation_unit_t **owner_out, parse_sym_entry_t **symbol_out,
    core_buffer_t *path_out);
core_translation_unit_t *unit_for_symbol(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    parse_sym_entry_t *symbol);
core_list_t(parse_scope_ptr_t) link_declaration_scopes(
    n00bb_tu_state_t *state, core_string_t namespace_name);
bool bind_link_references_to_nodes(parse_grammar_t *grammar,
                                   core_link_unit_t *link);
bool register_package_module_metadata(n00bb_link_analysis_t *analysis);
bool register_library_ir(n00bb_link_analysis_t *analysis, core_string_t owner);
bool interface_has_library_ir(const th_env *environment);
bool library_unit(core_translation_unit_t *unit);
bool toolchain_package(core_string_t package);
th_env *library_interface_environment(n00bb_link_analysis_t *analysis);
const uint8_t *library_abi_object(const n00bb_link_analysis_t *analysis,
                                  core_string_t module, size_t *size);
bool load_library_ir(n00bb_link_analysis_t *analysis,
                     ir_module_lookup_fn lookup);
ir_module_t *library_ir_module(const n00bb_link_analysis_t *analysis,
                               core_string_t name);
ir_module_t *interface_body_module(const n00bb_link_analysis_t *analysis,
                                   core_string_t name);
bool register_package_interfaces(n00bb_link_analysis_t *analysis,
    th_env *environment);
ir_func_t *link_ir_function_for_symbol(n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *symbol);
ir_global_t *link_ir_global_for_symbol(n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *symbol);
core_link_declaration_t *link_declaration_for_symbol(
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *symbol);
core_link_stage_result_t n00bb_link_stage_symbols(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
parse_tree_t *nested_module_declaration_owner(parse_grammar_t *grammar,
    parse_tree_t *declaration);
bool append_environment_symbol_name(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *symbol,
    core_buffer_t *qualified);
bool n00bb_replace_function_typevars(th_ctx *types, th_env *environment,
    parse_sym_entry_t *symbol, th_type *contract,
    core_string_t qualified_name);
bool append_function_contract_identity(core_translation_unit_t *unit,
    parse_sym_entry_t *symbol, core_buffer_t *key);
typedef enum {
    N00BB_OVERLOAD_SELECTED,
    N00BB_OVERLOAD_PENDING,
    N00BB_OVERLOAD_NO_MATCH,
    N00BB_OVERLOAD_AMBIGUOUS,
    N00BB_OVERLOAD_CONFLICT,
} n00bb_overload_status_t;
typedef struct {
    n00bb_overload_status_t status;
    parse_sym_entry_t *symbol;
    bool passed_over_conversion;
    core_list_t(parse_sym_entry_ptr_t) matches;
} n00bb_overload_choice_t;
typedef struct {
    bool unqualified;
    core_string_t origin_module;
    core_string_t name;
} n00bb_call_lookup_t;
#define N00BB_MODULE_VIEW_NAMESPACE "module-view"
#define N00BB_MODULE_VIEW_KEY "n00bb.package-module-view"
typedef struct {
    parse_scope_t *package_scope;
    core_string_t module;
} n00bb_module_view_t;
bool check_local_names_against_imports(n00bb_link_analysis_t *analysis);
void n00bb_check_clobbered_import(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
core_string_t n00bb_describe_candidates(parse_grammar_t *grammar,
    n00bb_tu_state_t *state,
    const core_list_t(parse_sym_entry_ptr_t) *symbols);
bool n00bb_function_has_overloads(n00bb_tu_state_t *state,
    parse_sym_entry_t *symbol, n00bb_call_lookup_t lookup);
const core_list_t(n00bb_import_source_t) *unqualified_import_sources(
    core_translation_unit_t *unit);
typedef n00bb_trait_specialization_t *n00bb_trait_specialization_ptr_t;
core_list_decl(n00bb_trait_specialization_ptr_t);

enum {
    N00BB_LINK_STAGE_INPUTS = 1,
    N00BB_LINK_STAGE_SYMBOLS = 2,
    N00BB_LINK_STAGE_SEMANTICS = 3,
    N00BB_LINK_STAGE_CONTRIBUTIONS = 4,
    N00BB_LINK_STAGE_IR_BINDINGS = 5,
};

enum {
    N00BB_LINK_DEPENDENCY_CALL = 1,
    N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH = 2,
    N00BB_LINK_DEPENDENCY_CONTEXT_CALL = 11,
    N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION = 16,
};

typedef struct {
    uint64_t typehash;
    n00bb_record_policy_t policy;
    parse_sym_entry_t *symbol;
    core_translation_unit_t *unit;
} n00bb_record_request_site_t;
core_list_decl(n00bb_record_request_site_t);

typedef struct n00bb_interface_summaries_t n00bb_interface_summaries_t;

typedef struct {
    core_translation_unit_t *unit;
    parse_tree_t *statement;
    parse_tree_t *expression;
} n00bb_effect_check_t;
core_list_decl(n00bb_effect_check_t);

typedef struct {
    core_translation_unit_t *unit;
    ir_span_t span;
    core_string_t trait;
    core_string_t qualified;
    core_string_t method;
    n00bb_bound_t rule;
    core_string_t expression;
} n00bb_trait_bound_t;
core_list_decl(n00bb_trait_bound_t);
core_list_decl(n00bb_user_effect_t);

typedef struct {
    ir_func_t *function;
    ir_span_t span;
    core_translation_unit_t *unit;
} n00bb_assert_site_t;
core_list_decl(n00bb_assert_site_t);


typedef struct {
    core_string_t module;
    ir_span_t span;
} n00bb_failed_region_t;
core_list_decl(n00bb_failed_region_t);

typedef struct {
    parse_sym_entry_t *symbol;
    core_translation_unit_t *unit;
} n00bb_nominal_hit_t;
core_list_decl(n00bb_nominal_hit_t);

typedef struct n00bb_link_analysis_t {
    parse_grammar_t *grammar;
    ir_ctx_t *ir_context;
    core_target_desc_t target;
    core_list_t(core_translation_unit_ptr_t) units;
    core_dict_t unit_of_module;
    bool unit_of_module_ready;
    bool effect_checks_failed;
    bool lowered_with_errors;
    bool defer_program_analysis;
    bool members_bound;
    core_list_t(n00bb_ir_func_ptr_t) reachable_functions;
    core_list_t(n00bb_ir_func_ptr_t) emitted_functions;
    core_list_t(n00bb_ir_func_ptr_t) callback_handlers;
    core_list_t(th_type_ptr_t) callback_handler_types;
    core_list_t(n00bb_ir_func_ptr_t) referenced_functions;
    core_dict_t trait_targets;
    bool trait_targets_ready;
    ir_summaries_t *entry_owner;
    ir_summary_t *entry_summaries[3];
    bool entry_ready[3];
    core_dict_t global_symbols;
    bool global_symbols_ready;
    core_dict_t escaped_globals;
    ir_summaries_t *escaped_owner;
    core_list_t(n00bb_specialization_t) specializations;
    core_list_t(n00bb_effect_check_t) effect_checks;
    core_list_t(n00bb_trait_bound_t) trait_bounds;
    core_dict_t bounded_targets;
    bool bounded_targets_ready;
    core_list_t(n00bb_user_effect_t) user_effects;
    core_list_t(n00bb_mark_question_t) mark_questions;
    core_dict_t effect_functions;
    bool effect_functions_ready;
    core_list_t(core_string_t) effect_natives;
    core_list_t(n00bb_specialization_request_t) specialization_requests;
    n00bb_boxed_hooks_t boxed_hooks;
    core_list_t(n00bb_bookkeeping_write_t) bookkeeping_writes;
    core_dict_t declarations_by_symbol;
    core_list_t(n00bb_record_request_site_t) record_requests;
    core_dict_t record_requests_by_type[N00BB_RECORD_C + 1];
    bool record_requests_indexed;
    core_dict_t enum_symbols_by_type;
    bool enum_symbols_indexed;
    core_dict_t nominal_symbols;
    core_list_t(n00bb_nominal_hit_t) nominal_hits;
    bool nominal_symbols_ready;
    ir_summary_hooks_t summary_hooks;
    ir_summaries_t *summaries;
    ir_summaries_t *escapes;
    ir_group_pdg_t *program;
    struct n00bb_rule_facts_t *rule_facts;
    core_dict_t dead_code_sites;
    bool dead_code_sites_ready;
    core_list_t(n00bb_failed_region_t) failed_regions;
    core_list_t(n00bb_failed_region_t) dead_regions;
    core_list_t(n00bb_assert_site_t) assert_sites;
    n00bb_interface_summaries_t *interface_summaries;
    core_list_t(core_translation_unit_ptr_t) initializer_order;
    bool dump_ranges;
    core_string_t effects_of;
    bool effects_json;
    bool effects_explain;
    core_string_t effects_query;
    core_string_t effects_in;
    core_string_t effects_not_in;
    bool effects_why;
    th_env *environment;
    th_ctx *link_types;
    th_env *export_environment;
    th_env *interface_environment;
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    ir_module_t **library_ir;
    size_t library_ir_count;
    const core_string_t *native_roots;
    size_t native_root_count;
    bool may_link_further;
    bool closed_world;
    bool keep_summaries;
    n00bb_resolver_t *resolver;
    struct n00bb_cache_t *cache;
} n00bb_link_analysis_t;

void n00bb_trait_targets_free(n00bb_link_analysis_t *analysis);

static inline bool
n00bb_open_dispatch(const n00bb_link_analysis_t *analysis)
{
    return analysis && analysis->may_link_further;
}

parse_sym_entry_t *n00bb_enum_type_symbol(n00bb_link_analysis_t *analysis,
                                          th_type *type);

parse_sym_entry_t *initializer_declaration_symbol(
    core_link_declaration_t *declaration);
bool dynamic_initializer_declaration(parse_grammar_t *grammar,
    core_link_declaration_t *declaration);
bool populate_link_declaration_facts(parse_grammar_t *grammar,
    ir_ctx_t *ir_context, core_translation_unit_t *unit,
    n00bb_link_payload_t *payload);
bool register_builtin_value_defaults(th_env *defaults, const th_env *base,
                                     core_link_unit_t *link,
                                     parse_grammar_t *grammar,
                                     n00bb_link_analysis_t *analysis);
bool register_dispatch_adapters(const th_env *environment,
                                core_link_unit_t *link,
                                parse_grammar_t *grammar,
                                n00bb_link_analysis_t *analysis);
parse_sym_entry_t *n00bb_nominal_symbol(n00bb_link_analysis_t *analysis,
                                        th_type *type,
                                        core_translation_unit_t **unit_out);
parse_scope_t *record_scope_for_type(n00bb_link_analysis_t *analysis,
                                     th_type *type,
                                     core_translation_unit_t **owner_out);
bool resolve_type_member_references(core_link_unit_t *link,
                                    n00bb_link_analysis_t *analysis);
bool bind_field_member_uses(n00bb_link_analysis_t *analysis);
bool diagnose_reserved_declarations(core_translation_unit_t *unit,
                                    const th_env *environment);

n00bb_link_analysis_t *n00bb_link_analysis_new(
    parse_grammar_t *grammar, ir_ctx_t *ir_context,
    const th_env *const *linked_interfaces, size_t linked_interface_count,
    const core_string_t *native_roots, size_t native_root_count,
    bool may_link_further, n00bb_resolver_t *resolver);
void n00bb_link_analysis_free(n00bb_link_analysis_t *analysis);
typedef struct n00bb_diagnostic_filter_t n00bb_diagnostic_filter_t;
n00bb_diagnostic_filter_t *n00bb_diagnostic_filter_take(
    n00bb_link_analysis_t *analysis);
void n00bb_diagnostic_filter_free(n00bb_diagnostic_filter_t *filter);
bool n00bb_diagnostic_visible(void *filter,
                              const core_diagnostic_t *diagnostic);
core_link_stage_result_t n00bb_link_stage_inputs(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_semantics(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_contributions(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_ir_bindings(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
bool validate_package_interfaces(n00bb_link_analysis_t *analysis);
bool populate_package_interfaces(n00bb_link_analysis_t *analysis);
bool reject_ambiguous_external_names(n00bb_link_analysis_t *analysis);
parse_sym_entry_t *n00bb_package_module_view(parse_symtab_t *symtab,
                                             parse_scope_t *scope,
                                             core_string_t name);
parse_sym_entry_t *granted_export_symbol(core_translation_unit_t *provider,
                                         core_string_t name,
                                         core_string_t grantee);
bool bind_imported_module_scopes(
    core_list_t(core_translation_unit_ptr_t) *units);
bool initialize_unit_package_interfaces(core_translation_unit_t *unit);
bool populate_unit_package_interfaces(core_translation_unit_t *unit);
bool prepopulate_component_package_interfaces(
    core_translation_unit_t *const *members, size_t count);
bool refresh_component_export_bindings(
    core_translation_unit_t *const *members, size_t count);
typedef enum {
    N00BB_NAMES_VALUES = 1u << 0,
    N00BB_NAMES_TYPES = 1u << 1,
} n00bb_name_kinds_t;
core_diag_location_t n00bb_node_span(parse_tree_t *node);
core_diag_location_t n00bb_symbol_location(parse_sym_entry_t *symbol);
core_diag_location_t n00bb_symbol_declaration_location(
    parse_sym_entry_t *symbol);
void n00bb_record_declaration_sites(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit);
bool n00bb_declaration_site_key(parse_grammar_t *grammar,
                                parse_sym_entry_t *symbol,
                                parse_sym_entry_t *owner, core_buffer_t *out);
void n00bb_diag_declared_here(core_diagnostics_t *diagnostics,
    parse_sym_entry_t *symbol, n00bb_text_t text);
bool n00bb_suggest_visible_name(core_diagnostics_t *diagnostics,
    core_translation_unit_t *unit, parse_scope_t *scope, core_string_t name,
    n00bb_name_kinds_t kinds);
bool n00bb_suggest_scope_member(core_diagnostics_t *diagnostics,
    core_translation_unit_t *consumer, core_translation_unit_t *provider,
    parse_scope_t *scope, core_string_t name);
bool n00bb_explain_hidden_name(core_diagnostics_t *diagnostics,
    core_translation_unit_t *unit, parse_scope_t *scope, core_string_t name,
    bool type_reference);
th_env *n00bb_compose_environments(const th_env *const *inputs, size_t count,
    th_ctx *types, core_translation_unit_t *unit);
