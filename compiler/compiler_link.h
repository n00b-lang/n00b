#pragma once

#include "compiler_tu.h"
#include "driver.h"
#include "abi_record_layout.h"
#include "abi_static_graph.h"
#include "llvm_backend.h"
#include "specialize.h"
#include "lib/dict.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "lib/target.h"
#include "parse/annot_walk.h"
#include "parse/grammar.h"
#include "ir/exec_graph.h"
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
core_translation_unit_t *unit_for_module_site(
    core_translation_unit_t *unit, n00bb_link_use_site_t *site,
    bool allow_suffix);
parse_scope_t *package_scope_for_module_site(
    core_translation_unit_t *unit, n00bb_link_use_site_t *site,
    core_translation_unit_t *imported);
parse_sym_entry_t *call_receiver_symbol(parse_grammar_t *grammar,
                                        core_translation_unit_t *unit,
                                        parse_tree_t *receiver);
bool call_receiver_is_module(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *receiver);
bool field_access_is_visible(core_translation_unit_t *use_unit,
                             core_translation_unit_t *owner_unit,
                             parse_sym_entry_t *owner_type,
                             parse_sym_entry_t *field);
bool module_member_is_call(parse_grammar_t *grammar, parse_tree_t *node);
bool site_is_resolved_module_prefix(parse_grammar_t *grammar,
                                    core_translation_unit_t *unit,
                                    n00bb_tu_state_t *state,
                                    n00bb_link_use_site_t *site);
bool n00bb_pending_type_reference(n00bb_tu_state_t *state,
                                  parse_tree_t *node);
bool resolve_module_member_site(parse_grammar_t *grammar,
                                core_translation_unit_t *unit,
                                parse_tree_t *node);
bool resolve_imported_module_symbols(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units);
void mark_resolved_flagset_operator_sites(parse_grammar_t *grammar,
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
void report_call_type_error(core_translation_unit_t *unit,
    parse_tree_t *call, th_ctx *types, th_type *actual, th_type *expected,
    n00bb_call_role_t role);
typedef struct {
    parse_tree_t *argument;
    th_type *expected;
    bool keyword;
    bool writes;
    th_mode mode;
} n00bb_call_binding_t;
core_list_decl(n00bb_call_binding_t);
bool n00bb_bind_call_arguments(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *callee,
    th_type *signature, parse_tree_t *call_args, size_t offset,
    parse_tree_t *site, core_list_t(n00bb_call_binding_t) *bindings);
bool unify_call_node_type(core_translation_unit_t *unit,
    n00bb_tu_state_t *state, parse_tree_t *diagnostic_node,
    parse_tree_t *typed_node, th_type *actual, th_type *expected,
    n00bb_call_role_t role);
bool n00bb_check_deferred_conversions(core_translation_unit_t *unit);
void n00bb_report_conversion_error(core_translation_unit_t *unit,
    parse_tree_t *site, th_ctx *types, th_type *value, th_type *target);
bool dotted_receiver_prefix_node(parse_grammar_t *grammar,
    parse_tree_t *node);
bool call_units_failed(core_translation_unit_t *const *units,
    size_t unit_count);
bool unify_calls_until_stable(parse_grammar_t *grammar,
    core_link_unit_t *link, core_translation_unit_t *const *units,
    size_t unit_count, bool resolve_ordinary_calls,
    bool resolve_member_calls, bool deferred_trait_requirements_only,
    bool settle_overloads);
bool generalize_link_unit(parse_grammar_t *grammar, core_link_unit_t *link,
    core_list_t(core_translation_unit_ptr_t) *units);
bool publish_link_call_dependencies(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t *state_out);
parse_tree_t *call_argument_for_parameter(parse_grammar_t *grammar,
    parse_tree_t *call, parse_sym_entry_t *function, size_t target,
    bool trait_dispatch);
parse_sym_entry_t *resolve_linked_method(core_link_unit_t *link,
    n00bb_link_use_site_t *site, core_translation_unit_t *unit,
    const th_env_method_t *method);
core_link_declaration_t *linked_method_declaration(core_link_unit_t *link,
    const th_env_method_t *method);
core_link_declaration_t *register_builtin_method_declaration(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    core_translation_unit_t *unit, core_string_t trait_name,
    const th_env_method_t *method);
bool has_trait_constraint(th_type *receiver, core_string_t trait_name);
bool n00bb_dispatch_fingerprint(th_ctx *types, const th_env *environment,
                                core_string_t trait_name,
                                core_string_t method_name, uint64_t *out);
bool has_deferred_method_constraint(th_type *receiver,
    core_string_t method_name);
typedef enum {
    N00BB_REQUIREMENT_DOES_NOT_FIT,
    N00BB_REQUIREMENT_FITS_WITH_DEFAULTS,
    N00BB_REQUIREMENT_FITS_EXACTLY,
} n00bb_requirement_fit_t;
n00bb_requirement_fit_t n00bb_method_requirement_fit(
    parse_grammar_t *grammar, th_ctx *types, th_type *receiver,
    core_string_t method_name, th_type *signature,
    parse_sym_entry_t *requirement);
bool n00bb_bind_method_keywords(parse_grammar_t *grammar, th_ctx *types,
                                th_type *receiver, core_string_t method_name,
                                parse_sym_entry_t *requirement);
parse_sym_entry_t *trait_method_requirement_symbol(
    parse_grammar_t *grammar, n00bb_link_analysis_t *analysis,
    core_translation_unit_t *caller, core_string_t qualified_trait,
    core_string_t method_name);
parse_sym_entry_t *linkable_trait_requirement(
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    n00bb_link_use_site_t *site, parse_sym_entry_t *symbol);
bool diagnose_unused_link_symbols(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis);
bool resolve_inferred_trait_references(parse_grammar_t *grammar,
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    const th_env *environment);
bool resolve_generic_trait_specializations(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage);
th_type *constructor_signature_instance(th_ctx *types,
    parse_sym_entry_t *symbol);
th_type *link_semantic_type_for_symbol(parse_grammar_t *grammar,
                                       core_translation_unit_t *owner,
                                       parse_sym_entry_t *symbol);
bool finalize_named_declaration_types(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units);

typedef struct {
    parse_tree_t *site;
    parse_tree_t *value;
    th_type *type;
    core_diag_location_t location;
    core_string_t provenance;
} n00bb_initializer_fact_t;
core_list_decl(n00bb_initializer_fact_t);

typedef enum {
    N00BB_CLEANUP_RELEASE_NONE,
    N00BB_CLEANUP_RELEASE_RUNTIME_MANAGED,
    N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE,
} n00bb_cleanup_release_policy_t;

typedef struct {
    parse_sym_entry_t *symbol;
    ir_func_t *function;
    ir_value_t *binding;
    parse_sym_phase_t phase;
    parse_sym_storage_duration_t storage_duration;
    n00bb_cleanup_release_policy_t release_policy;
    size_t cfg_edges;
    size_t cleanup_cfg_edges;
    size_t safe_endpoints;
    size_t point_endpoints;
    size_t edge_endpoints;
    size_t exit_edges;
    size_t safe_exit_edges;
    size_t live_exit_edges;
    size_t unknown_exit_edges;
    bool range_complete;
} n00bb_cleanup_fact_t;
core_list_decl(n00bb_cleanup_fact_t);

typedef struct {
    parse_sym_entry_t *symbol;
    th_type *semantic_type;
    n00bb_static_value_t *static_value;
    parse_sym_phase_t phases;
    parse_sym_phase_t mutable_phases;
    parse_sym_phase_t initializer_phases;
    parse_sym_storage_duration_t storage_duration;
    parse_sym_lifetime_t lifetime;
    core_string_t requested_storage_class;
    core_string_t selected_storage_class;
    bool storage_class_required;
    bool is_thread_local;
    bool alias_analysis_complete;
    bool facts_complete;
    bool phase_value_complete;
    bool initializer_facts_complete;
    bool array_shape_complete;
    bool storage_lifetime_complete;
    bool cfg_liveness_complete;
    bool cleanup_obligations_complete;
    size_t cleanup_range_count;
    size_t cleanup_complete_ranges;
    size_t cleanup_cfg_edges;
    size_t cleanup_endpoint_count;
    size_t cleanup_exit_edges;
    size_t cleanup_safe_exit_edges;
    size_t cleanup_live_exit_edges;
    size_t cleanup_unknown_exit_edges;
    size_t cleanup_no_release_ranges;
    size_t cleanup_runtime_managed_ranges;
    size_t cleanup_unavailable_ranges;
    size_t cfg_liveness_symbols;
    size_t cfg_liveness_ranges;
    size_t cfg_liveness_complete_ranges;
    size_t cfg_liveness_start_points;
    size_t cfg_liveness_end_points;
    size_t cfg_liveness_end_edges;
    size_t cfg_liveness_missing_starts;
    size_t cfg_liveness_edges;
    size_t cfg_liveness_direct_live_edges;
    size_t cfg_liveness_direct_dead_edges;
    size_t cfg_liveness_direct_unknown_edges;
    size_t cfg_liveness_alias_live_edges;
    size_t cfg_liveness_alias_dead_edges;
    size_t cfg_liveness_alias_unknown_edges;
    size_t cfg_liveness_range_ends;
    size_t cfg_liveness_missing_bindings;
    size_t cfg_liveness_incomplete_blocks;
    size_t cfg_liveness_owner_mismatches;
    size_t cfg_liveness_empty_functions;
    core_list_t(n00bb_initializer_fact_t) initializers;
    core_list_t(n00bb_cleanup_fact_t) cleanup_facts;
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
bool linkable_module_symbol(parse_sym_entry_t *symbol);
core_translation_unit_t *unit_for_symbol(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    parse_sym_entry_t *symbol);
core_list_t(parse_scope_ptr_t) link_declaration_scopes(
    n00bb_tu_state_t *state, core_string_t namespace_name);
void free_n00bb_link_payload(void *payload);
bool n00bb_link_contract_compatible(const void *existing,
    const void *candidate, void *opaque);
bool register_link_declaration(core_link_unit_t *link,
    core_translation_unit_t *unit, parse_grammar_t *grammar,
    core_dict_t *declarations_by_symbol, parse_sym_entry_t *symbol);
bool register_link_declarations(core_link_unit_t *link,
    parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units,
    core_dict_t *declarations_by_symbol);
bool register_link_use_sites(core_link_unit_t *link,
    parse_grammar_t *grammar, core_list_t(core_translation_unit_ptr_t) *units);
bool bind_link_references_to_nodes(parse_grammar_t *grammar,
                                   core_link_unit_t *link);
bool register_package_module_metadata(n00bb_link_analysis_t *analysis);
bool register_package_interfaces(n00bb_link_analysis_t *analysis,
    th_env *environment);
void bind_return_call_sources(n00bb_link_analysis_t *analysis);
ir_func_t *link_ir_function_for_symbol(n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *symbol);
ir_global_t *link_ir_global_for_symbol(n00bb_link_analysis_t *analysis,
    parse_sym_entry_t *symbol);
core_link_declaration_t *link_declaration_for_symbol(
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *symbol);
bool link_declaration_has_only_interface_definition(
    core_link_declaration_t *declaration);
parse_sym_entry_t *lifetime_function_owner(parse_grammar_t *grammar,
                                            parse_sym_entry_t *symbol);
core_link_stage_result_t n00bb_link_stage_cfg_liveness(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_cleanup_obligations(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_storage_lifetimes(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_symbols(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
parse_tree_t *nested_module_declaration_owner(parse_grammar_t *grammar,
    parse_tree_t *declaration);
bool append_link_symbol_key(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *symbol,
    core_buffer_t *key);
bool append_environment_symbol_name(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_sym_entry_t *symbol,
    core_buffer_t *qualified);
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
n00bb_overload_choice_t n00bb_select_call_overload(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_cf_label_t *call,
    parse_sym_entry_t *symbol, n00bb_call_lookup_t lookup);
void n00bb_overload_choice_release(n00bb_overload_choice_t *choice);
bool check_local_names_against_imports(n00bb_link_analysis_t *analysis);
bool check_import_brings(n00bb_link_analysis_t *analysis);
parse_sym_entry_t *n00bb_unqualified_import_named(
    core_translation_unit_t *unit, core_string_t name, bool function);
void n00bb_check_clobbered_import(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
bool n00bb_report_name_conflict(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node, core_string_t name,
    parse_scope_t *scope, bool type_reference);
bool n00bb_resolve_overload_reference(parse_grammar_t *grammar,
    core_link_unit_t *link, n00bb_tu_state_t *state, parse_tree_t *callee,
    parse_sym_entry_t *selected);
void n00bb_describe_candidate(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_sym_entry_t *symbol, core_buffer_t *out);
void n00bb_describe_candidates(parse_grammar_t *grammar,
    n00bb_tu_state_t *state,
    const core_list_t(parse_sym_entry_ptr_t) *symbols, core_buffer_t *out);
bool n00bb_function_has_overloads(n00bb_tu_state_t *state,
    parse_sym_entry_t *symbol, n00bb_call_lookup_t lookup);
parse_sym_entry_t *n00bb_module_view_member(parse_symtab_t *symtab,
    const n00bb_module_view_t *view, core_string_t name, bool type_member);
const core_list_t(n00bb_import_source_t) *unqualified_import_sources(
    core_translation_unit_t *unit);
core_list_t(parse_sym_entry_ptr_t) unqualified_import_candidates(
    core_translation_unit_t *unit, core_string_t name, bool type_reference);
void n00bb_defer_overloaded_callee(parse_grammar_t *grammar,
    core_translation_unit_t *unit, parse_tree_t *node);
core_string_t link_symbol_module(core_translation_unit_t *unit,
                                 parse_sym_entry_t *symbol);
bool n00bb_bind_site_reference(core_translation_unit_t *unit,
    n00bb_link_use_site_t *site,
    core_link_reference_t *reference);
typedef n00bb_trait_specialization_t *n00bb_trait_specialization_ptr_t;
core_list_decl(n00bb_trait_specialization_ptr_t);

typedef struct {
    n00bb_static_graph_node_t node;
    uint64_t address;
    uint64_t typehash;
    core_string_t type_name;
    bool immutable;
    bool array_backing;
    bool foreign;
} n00bb_captured_graph_node_t;
core_list_decl(n00bb_captured_graph_node_t);
core_list_decl(n00bb_static_graph_edge_t);

typedef struct {
    core_string_t identifier;
    size_t root_index;
} n00bb_baked_root_t;
core_list_decl(n00bb_baked_root_t);

typedef struct {
    size_t node;
    size_t offset;
    uint32_t table;
    uint32_t kind;
    uint64_t typehash;
    core_string_t module;
    core_string_t name;
} n00bb_static_graph_slot_t;
core_list_decl(n00bb_static_graph_slot_t);

enum {
    N00BB_LINK_STAGE_INPUTS = 1,
    N00BB_LINK_STAGE_SYMBOLS = 2,
    N00BB_LINK_STAGE_SEMANTICS = 3,
    N00BB_LINK_STAGE_CONTRIBUTIONS = 4,
    N00BB_LINK_STAGE_IR_BINDINGS = 5,
    N00BB_LINK_STAGE_CFG_LIVENESS = 6,
    N00BB_LINK_STAGE_CLEANUP_OBLIGATIONS = 7,
    N00BB_LINK_STAGE_STORAGE_LIFETIMES = 8,
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
    ir_func_t *function;
    ir_span_t span;
    core_translation_unit_t *unit;
} n00bb_assert_site_t;
core_list_decl(n00bb_assert_site_t);

typedef struct {
    ir_func_t *function;
    ir_instr_t *trap;
    core_translation_unit_t *unit;
    core_string_t name;
} n00bb_missing_return_t;
core_list_decl(n00bb_missing_return_t);

typedef struct n00bb_link_analysis_t {
    parse_grammar_t *grammar;
    ir_ctx_t *ir_context;
    core_target_desc_t target;
    core_list_t(core_translation_unit_ptr_t) units;
    core_dict_t unit_of_module;
    bool unit_of_module_ready;
    core_list_t(n00bb_trait_specialization_ptr_t) trait_specializations;
    core_list_t(n00bb_ir_func_ptr_t) reachable_functions;
    core_list_t(n00bb_ir_func_ptr_t) emitted_functions;
    core_list_t(n00bb_ir_func_ptr_t) callback_handlers;
    core_list_t(th_type_ptr_t) callback_handler_types;
    ir_summaries_t *entry_owner;
    ir_summary_t *entry_summaries[3];
    bool entry_ready[3];
    core_dict_t global_symbols;
    bool global_symbols_ready;
    core_dict_t escaped_globals;
    ir_summaries_t *escaped_owner;
    core_list_t(n00bb_specialization_t) specializations;
    core_list_t(n00bb_specialization_request_t) specialization_requests;
    core_list_t(n00bb_captured_graph_node_t) static_graph_nodes;
    core_list_t(n00bb_static_graph_edge_t) static_graph_edges;
    core_list_t(n00bb_baked_root_t) baked_roots;
    core_dict_t baked_root_index;
    core_list_t(n00bb_static_graph_slot_t) static_graph_slots;
    core_string_t baked_failure_root;
    core_string_t baked_failure_path;
    core_string_t baked_failure_message;
    bool baked_failure_reported;
    bool build_time_capture;
    bool build_time_needed;
    bool literal_capture;
    bool literal_needed;
    core_dict_t declarations_by_symbol;
    core_list_t(n00bb_record_request_site_t) record_requests;
    core_dict_t record_requests_by_type[N00BB_RECORD_C + 1];
    bool record_requests_indexed;
    core_dict_t enum_symbols_by_type;
    bool enum_symbols_indexed;
    ir_exec_graph_t *execution_graph;
    ir_summary_hooks_t summary_hooks;
    ir_summaries_t *summaries;
    ir_summaries_t *escapes;
    struct n00bb_rule_facts_t *rule_facts;
    core_dict_t dead_code_sites;
    bool dead_code_sites_ready;
    core_list_t(n00bb_assert_site_t) assert_sites;
    core_list_t(n00bb_missing_return_t) missing_returns;
    n00bb_interface_summaries_t *interface_summaries;
    core_list_t(core_translation_unit_ptr_t) initializer_order;
    bool dump_ranges;
    th_env *environment;
    th_env *export_environment;
    th_env *interface_environment;
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    const core_string_t *native_roots;
    size_t native_root_count;
    bool may_link_further;
    bool closed_world;
    bool returned_parameters_computed;
    n00bb_resolver_t *resolver;
    struct n00bb_cache_t *cache;
} n00bb_link_analysis_t;

static inline bool
n00bb_open_dispatch(const n00bb_link_analysis_t *analysis)
{
    return analysis && analysis->may_link_further;
}

parse_sym_entry_t *n00bb_enum_type_symbol(n00bb_link_analysis_t *analysis,
                                          th_type *type);

const core_list_t(core_translation_unit_ptr_t) *
n00bb_link_analysis_units(const n00bb_link_analysis_t *analysis);
parse_sym_entry_t *initializer_declaration_symbol(
    core_link_declaration_t *declaration);
bool dynamic_initializer_declaration(parse_grammar_t *grammar,
    core_link_declaration_t *declaration);
parse_tree_t *link_initializer_value(parse_grammar_t *grammar,
    parse_tree_t *definition);
bool populate_link_declaration_facts(parse_grammar_t *grammar,
    ir_ctx_t *ir_context, core_translation_unit_t *unit,
    n00bb_link_payload_t *payload);
bool register_builtin_value_defaults(th_env *defaults, const th_env *base,
                                     core_link_unit_t *link,
                                     parse_grammar_t *grammar,
                                     n00bb_link_analysis_t *analysis);
bool resolve_variant_case_references(core_link_unit_t *link,
                                     n00bb_link_analysis_t *analysis);
parse_scope_t *record_scope_for_type(parse_grammar_t *grammar,
    core_list_t(core_translation_unit_ptr_t) *units, th_type *type,
    core_translation_unit_t **owner_out);
bool resolve_inferred_field_references(core_link_unit_t *link,
                                       n00bb_link_analysis_t *analysis);
bool diagnose_known_missing_record_fields(n00bb_link_analysis_t *analysis);
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
parse_sym_entry_t *n00bb_package_module_view(parse_symtab_t *symtab,
                                             parse_scope_t *scope,
                                             core_string_t name);
parse_sym_entry_t *granted_export_symbol(core_translation_unit_t *provider,
                                         core_string_t name,
                                         core_string_t grantee);
bool bind_imported_module_scopes(
    core_list_t(core_translation_unit_ptr_t) *units);
void package_diagnostic(core_translation_unit_t *unit, parse_tree_t *node,
                        core_string_t code, core_string_t message);
bool initialize_unit_package_interfaces(core_translation_unit_t *unit);
bool populate_unit_package_interfaces(core_translation_unit_t *unit);
bool prepopulate_component_package_interfaces(
    core_translation_unit_t *const *members, size_t count);
bool refresh_component_export_bindings(
    core_translation_unit_t *const *members, size_t count);
