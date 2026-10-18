#pragma once

#include "compiler_tu.h"
#include "driver.h"
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
                                  const char **name, size_t *name_size,
                                  uint8_t *flags, uint8_t *mode);
size_t function_interface_parameter_count(parse_sym_entry_t *symbol);
bool function_interface_parameter_named(parse_sym_entry_t *symbol,
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
void report_call_type_error(core_translation_unit_t *unit,
    parse_tree_t *call, th_ctx *types, th_type *actual, th_type *expected);
bool unify_call_node_type(core_translation_unit_t *unit,
    n00bb_tu_state_t *state, parse_tree_t *diagnostic_node,
    parse_tree_t *typed_node, th_type *actual, th_type *expected);
bool dotted_receiver_prefix_node(parse_grammar_t *grammar,
    parse_tree_t *node);
bool call_units_failed(core_translation_unit_t *const *units,
    size_t unit_count);
bool unify_calls_until_stable(parse_grammar_t *grammar,
    core_link_unit_t *link, core_translation_unit_t *const *units,
    size_t unit_count, bool resolve_ordinary_calls,
    bool resolve_member_calls, bool deferred_trait_requirements_only);
bool generalize_link_unit(parse_grammar_t *grammar, core_link_unit_t *link,
    core_list_t(core_translation_unit_ptr_t) *units);
bool publish_link_call_dependencies(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t *state_out);
parse_sym_entry_t *link_dependency_callee_symbol(
    n00bb_link_analysis_t *analysis, core_link_dependency_view_t *call);
bool link_dependency_reference_builtin(core_link_dependency_view_t *call,
    core_string_t *name);
bool publish_function_effect_summaries(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t direct_state,
    core_link_contribution_state_t call_state,
    core_link_contribution_state_t *state_out);
bool initializer_units_share_cycle(core_link_unit_t *link,
    core_translation_unit_t *left, core_translation_unit_t *right);
bool publish_link_initializer_uses(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t *state_out);
bool has_initializer_write_candidate(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *symbol);
bool n00bb_module_initializer_writes(n00bb_link_analysis_t *analysis,
                                     parse_sym_entry_t *symbol);
bool n00bb_function_writes_global(core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis,
                                  parse_sym_entry_t *symbol);
bool n00bb_next_call_within(core_translation_unit_t *unit, parse_tree_t *node,
                            size_t *cursor, core_link_dependency_view_t *call,
                            th_type **signature);
bool n00bb_signature_writes_arguments(th_type *signature);
bool n00bb_callee_writes_globals(core_link_unit_t *link,
                                 core_link_declaration_t *callee);
bool n00bb_use_writes_root(core_translation_unit_t *unit,
                           parse_grammar_t *grammar, parse_tree_t *use,
                           parse_tree_t *region);
bool validate_initializer_cycles(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis,
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
    core_translation_unit_t *unit, const char *trait_name,
    const th_env_method_t *method);
bool has_trait_constraint(th_type *receiver, const char *trait_name);
bool has_deferred_method_constraint(th_type *receiver,
    core_string_t method_name);
bool n00bb_method_requirement_matches(th_type *receiver,
                                      core_string_t method_name,
                                      th_type *signature);
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
    core_translation_unit_t *unit, core_string_t name, bool imported);
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
bool publish_function_global_effects(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis, uint32_t producer_stage,
    core_link_contribution_state_t *state_out);
bool register_package_context_effect_metadata(n00bb_link_analysis_t *analysis);
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
parse_sym_entry_t *n00bb_select_call_overload(parse_grammar_t *grammar,
    n00bb_tu_state_t *state, parse_cf_label_t *call,
    parse_sym_entry_t *symbol, bool *pending);
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
    char *type_name;
    bool immutable;
    bool array_backing;
    bool foreign;
} n00bb_captured_graph_node_t;
core_list_decl(n00bb_captured_graph_node_t);
core_list_decl(n00bb_static_graph_edge_t);

typedef struct {
    char *identifier;
    size_t root_index;
} n00bb_baked_root_t;
core_list_decl(n00bb_baked_root_t);

typedef struct {
    size_t node;
    size_t offset;
    uint32_t table;
    uint32_t kind;
    uint64_t typehash;
    char *module;
    char *name;
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
    N00BB_LINK_DEPENDENCY_INITIALIZER_USE = 3,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ = 4,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE = 5,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE = 6,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN = 7,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY = 8,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY = 9,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY = 10,
    N00BB_LINK_DEPENDENCY_CONTEXT_CALL = 11,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS = 12,
    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY = 13,
    N00BB_LINK_DEPENDENCY_INITIALIZER_ORDER = 15,
    N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION = 16,
};

typedef struct n00bb_link_analysis_t {
    parse_grammar_t *grammar;
    ir_ctx_t *ir_context;
    core_target_desc_t target;
    core_list_t(core_translation_unit_ptr_t) units;
    core_list_t(n00bb_trait_specialization_ptr_t) trait_specializations;
    core_list_t(n00bb_ir_func_ptr_t) reachable_functions;
    core_list_t(n00bb_ir_func_ptr_t) emitted_functions;
    core_list_t(n00bb_specialization_t) specializations;
    core_list_t(n00bb_specialization_request_t) specialization_requests;
    core_list_t(n00bb_captured_graph_node_t) static_graph_nodes;
    core_list_t(n00bb_static_graph_edge_t) static_graph_edges;
    core_list_t(n00bb_baked_root_t) baked_roots;
    core_list_t(n00bb_static_graph_slot_t) static_graph_slots;
    char *baked_failure_root;
    char *baked_failure_path;
    char *baked_failure_message;
    bool baked_failure_reported;
    bool build_time_capture;
    core_dict_t declarations_by_symbol;
    ir_exec_graph_t *execution_graph;
    th_env *environment;
    th_env *export_environment;
    th_env *interface_environment;
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    const core_string_t *native_roots;
    size_t native_root_count;
    bool may_link_further;
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
typedef struct {
    core_translation_unit_t **units;
    size_t count;
} n00bb_initializer_order_t;
n00bb_initializer_order_t *build_initializer_order(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    core_link_contribution_state_t *state_out);
#define N00BB_INITIALIZER_ORDER_DOMAIN "n00bb.module-initializer-order"
bool n00bb_collect_initializer_order(core_link_unit_t *link,
    core_list_t(core_translation_unit_ptr_t) *order);
void free_initializer_order(void *opaque);
parse_sym_entry_t *initializer_declaration_symbol(
    core_link_declaration_t *declaration);
void append_initializer_declaration_name(core_buffer_t *out,
    core_link_declaration_t *declaration);
bool dynamic_initializer_declaration(parse_grammar_t *grammar,
    core_link_declaration_t *declaration);
parse_tree_t *link_initializer_value(parse_grammar_t *grammar,
    parse_tree_t *definition);
bool populate_link_declaration_facts(parse_grammar_t *grammar,
    ir_ctx_t *ir_context, core_translation_unit_t *unit,
    n00bb_link_payload_t *payload);
uint32_t effect_summary_kind(uint32_t kind);
bool restore_context_effect_dependencies(core_link_unit_t *link,
    uint32_t producer_stage);
bool register_context_effect_metadata(n00bb_link_analysis_t *analysis,
    core_link_unit_t *link);
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

n00bb_link_analysis_t *n00bb_link_analysis_new(
    parse_grammar_t *grammar, ir_ctx_t *ir_context,
    const th_env *const *linked_interfaces, size_t linked_interface_count,
    const core_string_t *native_roots, size_t native_root_count,
    bool may_link_further, n00bb_resolver_t *resolver);
void n00bb_link_analysis_free(n00bb_link_analysis_t *analysis);
core_link_stage_result_t n00bb_link_stage_inputs(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_semantics(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_contributions(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
core_link_stage_result_t n00bb_link_stage_ir_bindings(
    core_link_unit_t *link, uint32_t stage_kind, void *opaque);
bool validate_package_interfaces(n00bb_link_analysis_t *analysis);
bool connect_package_subpackages(n00bb_link_analysis_t *analysis);
bool populate_package_interfaces(n00bb_link_analysis_t *analysis);
parse_sym_entry_t *granted_export_symbol(core_translation_unit_t *provider,
                                         core_string_t name,
                                         core_string_t grantee);
bool bind_imported_module_scopes(
    core_list_t(core_translation_unit_ptr_t) *units);
void package_diagnostic(core_translation_unit_t *unit, parse_tree_t *node,
                        const char *code, const char *message);
bool initialize_unit_package_interfaces(core_translation_unit_t *unit);
bool populate_unit_package_interfaces(core_translation_unit_t *unit);