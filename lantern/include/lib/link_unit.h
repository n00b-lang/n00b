#pragma once

#include "diag/diagnostics.h"
#include "lib/module.h"

typedef struct core_link_unit_t core_link_unit_t;
typedef struct core_link_declaration_t core_link_declaration_t;
typedef struct core_link_reference_t core_link_reference_t;
typedef struct core_link_dependency_t core_link_dependency_t;

typedef enum {
    CORE_LINK_CONTRIBUTION_PENDING,
    CORE_LINK_CONTRIBUTION_COMPLETE,
    CORE_LINK_CONTRIBUTION_UNAVAILABLE,
    CORE_LINK_CONTRIBUTION_FAILED,
} core_link_contribution_state_t;

typedef enum {
    CORE_LINK_STAGE_PENDING,
    CORE_LINK_STAGE_RUNNING,
    CORE_LINK_STAGE_COMPLETE,
    CORE_LINK_STAGE_UNAVAILABLE,
    CORE_LINK_STAGE_FAILED,
} core_link_stage_state_t;

typedef enum {
    CORE_LINK_STAGE_RESULT_COMPLETE,
    CORE_LINK_STAGE_RESULT_UNAVAILABLE,
    CORE_LINK_STAGE_RESULT_FAILED,
} core_link_stage_result_t;

typedef core_link_stage_result_t (*core_link_stage_fn)(
    core_link_unit_t *link, uint32_t stage_kind, void *context);

typedef enum {
    CORE_LINK_PAYLOAD_BORROWED,
    CORE_LINK_PAYLOAD_OWNED,
} core_link_payload_ownership_t;

typedef void (*core_link_payload_free_fn)(void *payload);

typedef bool (*core_link_contract_compatible_fn)(
    const void *existing, const void *candidate, void *context);

typedef struct {
    uint32_t kind;
    uint32_t visibility;
    core_string_t overload_group;
    const void *source_site;
    core_diag_location_t source_location;
    core_string_t provenance;
    uint32_t provenance_version;
    const void *contract;
    core_link_contract_compatible_fn compatible;
    void *compatibility_context;
} core_link_declaration_metadata_t;

typedef enum {
    CORE_LINK_ORIGIN_SOURCE,
    CORE_LINK_ORIGIN_LIBRARY,
    CORE_LINK_ORIGIN_EXTERNAL_REQUIREMENT,
} core_link_origin_kind_t;

typedef struct {
    core_translation_unit_t *unit;
    const void *source_site;
    core_diag_location_t location;
    core_string_t provenance;
    uint32_t provenance_version;
    core_link_origin_kind_t origin;
    bool is_definition;
} core_link_declaration_site_view_t;

typedef struct {
    core_string_t module;
    core_string_t format;
    uint32_t version;
    const void *payload;
    bool unit_wide;
} core_link_artifact_view_t;

typedef struct {
    core_string_t module;
    core_string_t domain;
    uint32_t version;
    uint32_t producer_stage;
    core_link_contribution_state_t state;
    core_string_t provenance;
    uint32_t provenance_version;
    const void *payload;
    bool unit_wide;
} core_link_contribution_view_t;

typedef enum {
    CORE_LINK_DEPENDENCY_RESOLVED,
    CORE_LINK_DEPENDENCY_DEFERRED,
    CORE_LINK_DEPENDENCY_EXTERNAL,
    CORE_LINK_DEPENDENCY_UNAVAILABLE,
    CORE_LINK_DEPENDENCY_FAILED,
} core_link_dependency_state_t;

typedef struct {
    uint32_t kind;
    uint32_t producer_stage;
    core_link_dependency_state_t state;
    core_link_declaration_t *from;
    core_link_declaration_t *to;
    core_translation_unit_t *source_unit;
    const void *source_site;
    core_diag_location_t source_location;
    core_string_t provenance;
    uint32_t provenance_version;
} core_link_dependency_view_t;

core_link_unit_t *core_link_unit_new(void);
void core_link_unit_free(core_link_unit_t *link);
core_diagnostics_t *core_link_unit_diagnostics(core_link_unit_t *link);
bool core_link_unit_has_fatal_diagnostics(core_link_unit_t *link);

typedef enum {
    CORE_LINK_PROBLEM_SAME_MODULE,
    CORE_LINK_PROBLEM_DUPLICATE_DEFINITION,
    CORE_LINK_PROBLEM_CONFLICTING_DECLARATION,
    CORE_LINK_PROBLEM_UNRESOLVED_REFERENCE,
    CORE_LINK_PROBLEM_DUPLICATE_ARTIFACT,
    CORE_LINK_PROBLEM_ARTIFACT_VERSION,
    CORE_LINK_PROBLEM_INTERNAL,
    CORE_LINK_PROBLEM_COUNT,
} core_link_problem_t;

void core_link_set_problem_code(core_link_problem_t problem,
                                core_string_t code);
core_string_t core_link_problem_code(core_link_problem_t problem);
bool core_link_report_problem(core_diagnostics_t *diagnostics,
                              const core_translation_unit_t *unit,
                              core_diag_location_t location,
                              core_link_problem_t problem,
                              core_string_t message);
bool core_link_report_internal(core_diagnostics_t *diagnostics,
                               const core_translation_unit_t *unit,
                               core_diag_location_t location,
                               core_string_t stage, core_string_t message);

bool core_link_unit_add_root(core_link_unit_t *link,
                             core_translation_unit_t *root);
size_t core_link_unit_root_count(const core_link_unit_t *link);
core_translation_unit_t *core_link_unit_root(const core_link_unit_t *link,
                                             size_t index);

bool core_link_unit_collect(core_link_unit_t *link);
bool core_link_unit_finalize_graph(core_link_unit_t *link);
bool core_link_unit_graph_is_finalized(const core_link_unit_t *link);
size_t core_link_unit_translation_unit_count(core_link_unit_t *link);
core_translation_unit_t *core_link_unit_translation_unit(
    core_link_unit_t *link, size_t index);
bool core_link_unit_visit_sccs(core_link_unit_t *link,
                               core_translation_unit_scc_fn callback,
                               void *context);
size_t core_link_unit_import_edge_count(core_link_unit_t *link);
bool core_link_unit_import_edge(core_link_unit_t *link, size_t index,
                                core_translation_unit_t **importer,
                                core_translation_unit_t **imported);
size_t core_link_unit_scc_count(core_link_unit_t *link);
size_t core_link_unit_scc_member_count(core_link_unit_t *link,
                                       size_t scc_index);
core_translation_unit_t *core_link_unit_scc_member(
    core_link_unit_t *link, size_t scc_index, size_t member_index);
bool core_link_unit_scc_of(core_link_unit_t *link,
                           core_translation_unit_t *unit,
                           size_t *scc_index);

bool core_link_unit_register_stage(
    core_link_unit_t *link, uint32_t stage_kind,
    const uint32_t *prerequisites, size_t prerequisite_count,
    core_string_t provenance, bool requires_finalized_graph,
    core_link_stage_fn callback, void *context);
bool core_link_unit_run_stage(core_link_unit_t *link, uint32_t stage_kind);
bool core_link_unit_invalidate_stage(core_link_unit_t *link,
                                     uint32_t stage_kind);
core_link_stage_state_t core_link_unit_stage_state(
    core_link_unit_t *link, uint32_t stage_kind);
size_t core_link_unit_stage_count(const core_link_unit_t *link);
uint32_t core_link_unit_stage_kind(const core_link_unit_t *link,
                                   size_t index);
core_string_t core_link_unit_stage_provenance(
    const core_link_unit_t *link, uint32_t stage_kind);

bool core_link_unit_define(core_link_unit_t *link,
                           core_translation_unit_t *owner,
                           core_string_t declaration_key, void *payload,
                           core_link_payload_ownership_t ownership,
                           core_link_payload_free_fn free_fn,
                           core_link_declaration_t **out);
bool core_link_unit_register_declaration(
    core_link_unit_t *link, core_translation_unit_t *owner,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata, bool is_definition,
    void *payload, core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn, core_link_declaration_t **out);
bool core_link_unit_define_external(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn, core_link_declaration_t **out);
bool core_link_unit_define_external_typed(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn, core_link_declaration_t **out);
bool core_link_unit_redeclare_external_typed(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata,
    core_link_declaration_t **out);
core_link_declaration_t *core_link_unit_find_declaration(
    const core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key);
bool core_link_unit_require_external(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata,
    core_link_declaration_t **out);
size_t core_link_unit_declaration_count(const core_link_unit_t *link);
core_link_declaration_t *core_link_unit_declaration_at(
    const core_link_unit_t *link, size_t index);
size_t core_link_unit_overload_member_count(
    core_link_unit_t *link, core_string_t module, core_string_t overload_group);
core_link_declaration_t *core_link_unit_overload_member(
    core_link_unit_t *link, core_string_t module, core_string_t overload_group,
    size_t index);
bool core_link_unit_reference(core_link_unit_t *link,
                              core_translation_unit_t *referrer,
                              core_string_t target_module,
                              core_string_t declaration_key,
                              core_link_reference_t **out);
bool core_link_unit_reference_at(core_link_unit_t *link,
                                 core_translation_unit_t *referrer,
                                 const void *use_site,
                                 core_string_t target_module,
                                 core_string_t declaration_key,
                                 core_link_reference_t **out);
bool core_link_unit_reference_at_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, core_string_t target_module,
    core_string_t declaration_key, uint32_t expected_kind,
    core_link_reference_t **out);
bool core_link_unit_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope,
    core_string_t target_module, core_string_t declaration_key,
    uint32_t expected_kind, core_link_reference_t **out);
bool core_link_unit_defer_reference(
    core_link_unit_t *link, core_link_reference_t *reference,
    core_string_t reason, uint32_t prerequisite_stage);
bool core_link_unit_defer_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope, uint32_t expected_kind,
    core_string_t reason, uint32_t prerequisite_stage,
    core_link_reference_t **out);
bool core_link_unit_add_deferred_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope, uint32_t expected_kind,
    core_string_t reason, uint32_t prerequisite_stage,
    core_link_reference_t **out);
bool core_link_unit_resolve_reference(
    core_link_unit_t *link, core_link_reference_t *reference,
    core_string_t target_module, core_string_t declaration_key);
size_t core_link_unit_reference_count(const core_link_unit_t *link);
core_link_reference_t *core_link_unit_reference_at_index(
    const core_link_unit_t *link, size_t index);
bool core_link_unit_validate(core_link_unit_t *link);

core_translation_unit_t *core_link_declaration_owner(
    const core_link_declaration_t *declaration);
core_string_t core_link_declaration_module(
    const core_link_declaration_t *declaration);
core_string_t core_link_declaration_key(
    const core_link_declaration_t *declaration);
const void *core_link_declaration_contract(
    const core_link_declaration_t *declaration);
void *core_link_declaration_payload(
    const core_link_declaration_t *declaration);
bool core_link_declaration_set_binding(
    core_link_unit_t *link, core_link_declaration_t *declaration,
    core_string_t role, uint32_t producer_stage, void *handle);
void *core_link_declaration_binding(
    const core_link_declaration_t *declaration, core_string_t role);
bool core_link_declaration_is_external(
    const core_link_declaration_t *declaration);
uint32_t core_link_declaration_kind(
    const core_link_declaration_t *declaration);
uint32_t core_link_declaration_visibility(
    const core_link_declaration_t *declaration);
core_string_t core_link_declaration_overload_group(
    const core_link_declaration_t *declaration);
bool core_link_declaration_is_defined(
    const core_link_declaration_t *declaration);
bool core_link_declaration_is_external_requirement(
    const core_link_declaration_t *declaration);
size_t core_link_declaration_site_count(
    const core_link_declaration_t *declaration);
bool core_link_declaration_site(const core_link_declaration_t *declaration,
                                size_t index,
                                core_link_declaration_site_view_t *out);
core_link_declaration_t *core_link_reference_target(
    const core_link_reference_t *reference);
core_translation_unit_t *core_link_reference_referrer(
    const core_link_reference_t *reference);
const void *core_link_reference_use_site(
    const core_link_reference_t *reference);
const void *core_link_reference_source_scope(
    const core_link_reference_t *reference);
core_string_t core_link_reference_module(
    const core_link_reference_t *reference);
core_string_t core_link_reference_key(
    const core_link_reference_t *reference);
bool core_link_reference_expected_kind(
    const core_link_reference_t *reference, uint32_t *out);
bool core_link_reference_set_source_info(
    core_link_unit_t *link, core_link_reference_t *reference,
    core_diag_location_t location, core_string_t provenance);
bool core_link_reference_source_info(
    const core_link_reference_t *reference,
    core_diag_location_t *location, core_string_t *provenance);
bool core_link_reference_is_deferred(
    const core_link_reference_t *reference);
core_string_t core_link_reference_deferral_reason(
    const core_link_reference_t *reference);
bool core_link_reference_deferral_prerequisite(
    const core_link_reference_t *reference, uint32_t *out);

bool core_link_unit_add_artifact(
    core_link_unit_t *link, core_string_t module, core_string_t format,
    uint32_t version, uint32_t producer_stage, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_add_unit_artifact(
    core_link_unit_t *link, core_string_t format, uint32_t version,
    uint32_t producer_stage, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_require_artifact(core_link_unit_t *link,
                                    core_string_t module,
                                    core_string_t format,
                                    uint32_t required_version,
                                    core_link_artifact_view_t *out);

bool core_link_unit_set_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t version, uint32_t producer_stage,
    core_link_contribution_state_t state, core_string_t provenance,
    uint32_t provenance_version, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_contribution(core_link_unit_t *link,
                                 core_string_t module, core_string_t domain,
                                 core_link_contribution_view_t *out);
bool core_link_unit_require_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t required_version, core_link_contribution_view_t *out);
size_t core_link_unit_contribution_count(core_link_unit_t *link);
bool core_link_unit_contribution_at(core_link_unit_t *link,
                                    size_t index,
                                    core_link_contribution_view_t *out);

bool core_link_unit_add_dependency(
    core_link_unit_t *link, uint32_t kind, uint32_t producer_stage,
    core_link_dependency_state_t state,
    core_link_declaration_t *from, core_link_declaration_t *to,
    core_translation_unit_t *source_unit, const void *source_site,
    core_diag_location_t source_location, core_string_t provenance,
    uint32_t provenance_version, core_link_dependency_t **out);
size_t core_link_unit_dependency_count(core_link_unit_t *link);
bool core_link_unit_dependency_at(core_link_unit_t *link, size_t index,
                                  core_link_dependency_view_t *out);
