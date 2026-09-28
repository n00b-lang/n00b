#pragma once

#include "lib/diagnostic.h"
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

/* `kind`, `visibility`, and `overload_group` are language-defined identities.
 * The compatibility callback compares contracts without exposing their
 * structure to Lantern. Contract, callback context, and source-site pointers
 * are borrowed for the lifetime of the LinkUnit; provenance strings are
 * copied. */
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

/* Roots and import edges are borrowed. TUs must outlive the link unit. */
bool core_link_unit_add_root(core_link_unit_t *link,
                             core_translation_unit_t *root);
size_t core_link_unit_root_count(const core_link_unit_t *link);
core_translation_unit_t *core_link_unit_root(const core_link_unit_t *link,
                                             size_t index);

/* Collects each reachable TU once and rejects duplicate module identities. */
bool core_link_unit_collect(core_link_unit_t *link);
/* Call after all imports are discovered and before parallel analysis. This
 * finalizes imports on every reachable TU; roots and imports then stay fixed. */
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

/* Stage kinds are opaque to Lantern. Register the complete stage set before
 * running any stage. A stage callback runs only after all prerequisites
 * complete. The stage driver is synchronous; callbacks may dispatch parallel
 * per-TU work through a work queue. Unavailable/failed outcomes block
 * dependents; callbacks report language diagnostics through TU/LinkUnit APIs. */
bool core_link_unit_register_stage(
    core_link_unit_t *link, uint32_t stage_kind,
    const uint32_t *prerequisites, size_t prerequisite_count,
    core_string_t provenance, bool requires_finalized_graph,
    core_link_stage_fn callback, void *context);
bool core_link_unit_run_stage(core_link_unit_t *link, uint32_t stage_kind);
/* Invalidates this stage and all transitive dependents. */
bool core_link_unit_invalidate_stage(core_link_unit_t *link,
                                     uint32_t stage_kind);
core_link_stage_state_t core_link_unit_stage_state(
    const core_link_unit_t *link, uint32_t stage_kind);
size_t core_link_unit_stage_count(const core_link_unit_t *link);
uint32_t core_link_unit_stage_kind(const core_link_unit_t *link,
                                   size_t index);
core_string_t core_link_unit_stage_provenance(
    const core_link_unit_t *link, uint32_t stage_kind);

/* Keys are opaque, language-defined identities local to a module. An owned
 * payload transfers to the link unit on successful registration; a borrowed
 * payload must outlive it. */
bool core_link_unit_define(core_link_unit_t *link,
                           core_translation_unit_t *owner,
                           core_string_t declaration_key, void *payload,
                           core_link_payload_ownership_t ownership,
                           core_link_payload_free_fn free_fn,
                           core_link_declaration_t **out);
/* Multiple compatible declarations may share one canonical handle. At most
 * one may be a definition; a compatible definition completes a declaration.
 * Payload ownership transfers only on success. */
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
/* Records an unresolved external symbol requirement. It is valid at link-unit
 * validation, but remains distinguishable from a supplied definition. */
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
/* A non-null use_site preserves source-reference identity when one TU has
 * multiple references to the same declaration. The pointer is borrowed. */
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
/* source_scope is an opaque, borrowed frontend scope handle retained with the
 * reference so later resolution can use its original lexical context. The
 * caller keeps it alive until the link unit and its references are released. */
bool core_link_unit_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope,
    core_string_t target_module, core_string_t declaration_key,
    uint32_t expected_kind, core_link_reference_t **out);
bool core_link_unit_defer_reference(
    core_link_unit_t *link, core_link_reference_t *reference,
    core_string_t reason, uint32_t prerequisite_stage);
/* Register a reference before its target is known, retaining the original
 * use site and scope until the named prerequisite stage resolves it. */
bool core_link_unit_defer_reference_at_scoped_kind(
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
/* References are owned by the LinkUnit and keyed by their borrowed source
 * node. A source node may carry multiple references; these queries enumerate
 * them without requiring a frontend-side handle cache. */
size_t core_link_unit_reference_count_at(
    const core_link_unit_t *link, const core_translation_unit_t *referrer,
    const void *use_site);
core_link_reference_t *core_link_unit_reference_at_use_site(
    const core_link_unit_t *link, const core_translation_unit_t *referrer,
    const void *use_site, size_t index);
bool core_link_unit_validate(core_link_unit_t *link);

core_translation_unit_t *core_link_declaration_owner(
    const core_link_declaration_t *declaration);
core_string_t core_link_declaration_module(
    const core_link_declaration_t *declaration);
core_string_t core_link_declaration_key(
    const core_link_declaration_t *declaration);
void *core_link_declaration_payload(
    const core_link_declaration_t *declaration);
/* Bind a live, borrowed subsystem handle to a canonical declaration under an
 * opaque role. A producer stage owns the binding when nonzero; invalidating
 * that stage clears it. These handles are not persistence payloads. */
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

/* Artifact payload ownership is explicit. Format identifiers and versions
 * let front ends reject incompatible persisted semantic contributions. */
bool core_link_unit_add_artifact(
    core_link_unit_t *link, core_string_t module, core_string_t format,
    uint32_t version, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_add_unit_artifact(
    core_link_unit_t *link, core_string_t format, uint32_t version,
    void *payload, core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_require_artifact(core_link_unit_t *link,
                                    core_string_t module,
                                    core_string_t format,
                                    uint32_t required_version,
                                    core_link_artifact_view_t *out);
size_t core_link_unit_artifact_count(const core_link_unit_t *link);
bool core_link_unit_artifact(const core_link_unit_t *link, size_t index,
                             core_link_artifact_view_t *out);

/* Contributions are analysis facts, not necessarily serialized artifacts.
 * A missing record is different from a recorded pending/unavailable result.
 * Stage-produced values are reset to pending when their producer or a
 * prerequisite is invalidated. A complete contribution may have a null
 * payload when completion itself is the fact. A null/empty module denotes a
 * unit-wide contribution. */
bool core_link_unit_set_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t version, uint32_t producer_stage,
    core_link_contribution_state_t state, core_string_t provenance,
    uint32_t provenance_version, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn);
bool core_link_unit_contribution(const core_link_unit_t *link,
                                 core_string_t module, core_string_t domain,
                                 core_link_contribution_view_t *out);
bool core_link_unit_require_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t required_version, core_link_contribution_view_t *out);
size_t core_link_unit_contribution_count(const core_link_unit_t *link);
bool core_link_unit_contribution_at(const core_link_unit_t *link,
                                    size_t index,
                                    core_link_contribution_view_t *out);

/* Directed semantic dependency facts between canonical declarations. `from`
 * may be null for a unit/TU-level dependency. `to` may be null only when the
 * state is not resolved. Kinds are language-defined. Source identity and
 * provenance are retained for diagnostics and incremental analysis.
 * Stage-produced facts are removed if their producer or a prerequisite is
 * invalidated, unavailable, or fails. */
bool core_link_unit_add_dependency(
    core_link_unit_t *link, uint32_t kind, uint32_t producer_stage,
    core_link_dependency_state_t state,
    core_link_declaration_t *from, core_link_declaration_t *to,
    core_translation_unit_t *source_unit, const void *source_site,
    core_diag_location_t source_location, core_string_t provenance,
    uint32_t provenance_version, core_link_dependency_t **out);
size_t core_link_unit_dependency_count(const core_link_unit_t *link);
bool core_link_unit_dependency_at(const core_link_unit_t *link, size_t index,
                                  core_link_dependency_view_t *out);
