#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "lib/alloc.h"
#include "lib/link_unit.h"
#include "harness.h"

static bool
init_unit(core_translation_unit_t *unit, const char *module_name)
{
    core_module_spec_t module = {0};
    core_source_file_t source = {0};
    bool ok = core_module_spec_init(&module,
        core_string_from_cstr(module_name))
        && core_source_file_init(&source,
            core_string_from_cstr(module_name), "", 0)
        && core_translation_unit_init(unit, &module, &source);
    core_module_spec_release(&module);
    core_source_file_release(&source);
    return ok;
}

typedef struct {
    size_t components;
    size_t cycle_members;
    size_t shared_visits;
    bool cycle_first;
} scc_trace_t;

static bool
trace_link_scc(void *context, core_translation_unit_t *const *members,
               size_t count)
{
    scc_trace_t *trace = context;
    if (!trace->components)
        trace->cycle_first = count == 2;
    if (count == 2) trace->cycle_members += count;
    for (size_t i = 0; i < count; i++)
        if (core_string_eq(members[i]->module.identifier,
                           CORE_STRING_STATIC("pkg.shared")))
            trace->shared_visits++;
    trace->components++;
    return true;
}

static int released_payloads;

static void
release_payload(void *payload)
{
    released_payloads++;
    core_free(payload);
}

static void
test_link_unit_closure_and_linkage(void)
{
    core_translation_unit_t root_a = {0};
    core_translation_unit_t root_b = {0};
    core_translation_unit_t shared = {0};
    core_translation_unit_t cycle_a = {0};
    core_translation_unit_t cycle_b = {0};
    init_unit(&root_a, "pkg.root_a");
    init_unit(&root_b, "pkg.root_b");
    init_unit(&shared, "pkg.shared");
    init_unit(&cycle_a, "pkg.cycle_a");
    init_unit(&cycle_b, "pkg.cycle_b");
    core_translation_unit_add_import(&root_a, &shared);
    core_translation_unit_add_import(&root_b, &shared);
    core_translation_unit_add_import(&shared, &cycle_a);
    core_translation_unit_add_import(&cycle_a, &cycle_b);
    core_translation_unit_add_import(&cycle_b, &cycle_a);

    core_link_unit_t *link = core_link_unit_new();
    check("multiple roots are registered once",
          core_link_unit_add_root(link, &root_a)
              && core_link_unit_add_root(link, &root_b)
              && core_link_unit_add_root(link, &root_a)
              && core_link_unit_root_count(link) == 2);
    check("the link unit collects a deduplicated transitive closure",
          core_link_unit_collect(link)
              && core_link_unit_translation_unit_count(link) == 5);

    scc_trace_t trace = {0};
    check("multi-root SCC traversal visits cycles once, dependency-first",
          core_link_unit_visit_sccs(link, trace_link_scc, &trace)
              && trace.components == 4 && trace.cycle_members == 2
              && trace.cycle_first && trace.shared_visits == 1);

    int shared_type = 17;
    core_link_declaration_t *definition = nullptr;
    check("a TU-owned definition has one stable declaration handle",
          core_link_unit_define(link, &shared,
              CORE_STRING_STATIC("type.Widget"), &shared_type,
              CORE_LINK_PAYLOAD_BORROWED, nullptr, &definition)
              && definition
              && core_link_declaration_owner(definition) == &shared
              && core_link_declaration_payload(definition) == &shared_type);

    core_link_reference_t *from_a = nullptr;
    core_link_reference_t *from_b = nullptr;
    core_link_reference_t *same_from_a = nullptr;
    check("cross-module references target the defining declaration",
          core_link_unit_reference(link, &root_a,
              CORE_STRING_STATIC("pkg.shared"),
              CORE_STRING_STATIC("type.Widget"), &from_a)
              && core_link_unit_reference(link, &root_b,
                  CORE_STRING_STATIC("pkg.shared"),
                  CORE_STRING_STATIC("type.Widget"), &from_b)
              && core_link_unit_reference(link, &root_a,
                  CORE_STRING_STATIC("pkg.shared"),
                  CORE_STRING_STATIC("type.Widget"), &same_from_a)
              && same_from_a == from_a);

    int call_site_one = 1;
    int call_site_two = 2;
    int scoped_call_site = 4;
    int lexical_scope = 3;
    core_link_reference_t *site_one = nullptr;
    core_link_reference_t *site_two = nullptr;
    core_link_reference_t *scoped_site = nullptr;
    check("same-target source references retain distinct use-site identity",
          core_link_unit_reference_at(link, &root_a, &call_site_one,
              CORE_STRING_STATIC("pkg.shared"),
              CORE_STRING_STATIC("type.Widget"), &site_one)
              && core_link_unit_reference_at(link, &root_a, &call_site_two,
                  CORE_STRING_STATIC("pkg.shared"),
                  CORE_STRING_STATIC("type.Widget"), &site_two)
              && site_one != site_two
              && core_link_reference_referrer(site_one) == &root_a
              && core_link_reference_use_site(site_one) == &call_site_one
              && core_link_reference_use_site(site_two) == &call_site_two
              && core_link_reference_target(site_one) == nullptr);
    check("source nodes enumerate their LinkUnit-owned references",
          core_link_unit_reference_count_at(link, &root_a, &call_site_one) == 1
              && core_link_unit_reference_at_use_site(
                  link, &root_a, &call_site_one, 0) == site_one
              && core_link_unit_reference_at_use_site(
                  link, &root_a, &call_site_one, 1) == nullptr
              && core_link_unit_reference_count_at(
                  link, &root_a, &call_site_two) == 1);
    check("frontend lexical scope is retained opaquely on a reference",
          core_link_unit_reference_at_scoped_kind(link, &root_a,
              &scoped_call_site, &lexical_scope,
              CORE_STRING_STATIC("pkg.shared"),
              CORE_STRING_STATIC("type.Widget"), 0, &scoped_site)
              && scoped_site
              && core_link_reference_use_site(scoped_site)
                  == &scoped_call_site
              && core_link_reference_source_scope(scoped_site)
                  == &lexical_scope);

    int library_trait = 23;
    core_link_declaration_t *external = nullptr;
    check("external library definitions remain explicitly external",
          core_link_unit_define_external(link,
              CORE_STRING_STATIC("pkg.library"),
              CORE_STRING_STATIC("trait.Format"), &library_trait,
              CORE_LINK_PAYLOAD_BORROWED, nullptr, &external)
              && external && core_link_declaration_is_external(external)
              && !core_link_declaration_owner(external));
    core_link_reference_t *to_library = nullptr;
    check("references resolve against linked library contributions",
          core_link_unit_reference(link, &root_a,
              CORE_STRING_STATIC("pkg.library"),
              CORE_STRING_STATIC("trait.Format"), &to_library));

    int semantic_environment = 31;
    check("versioned opaque semantic artifacts attach to the link unit",
          core_link_unit_add_artifact(link,
              CORE_STRING_STATIC("pkg.library"),
              CORE_STRING_STATIC("lantern.type-environment"), 4,
              &semantic_environment, CORE_LINK_PAYLOAD_BORROWED, nullptr));
    int composed_environment = 37;
    check("a composed environment attaches once at link-unit scope",
          core_link_unit_add_unit_artifact(link,
              CORE_STRING_STATIC("n00bb.frozen-type-environment"), 1,
              &composed_environment, CORE_LINK_PAYLOAD_BORROWED, nullptr));
    core_link_artifact_view_t artifact = {0};
    check("artifact format and version are queryable",
          core_link_unit_artifact_count(link) == 2
              && core_link_unit_artifact(link, 0, &artifact)
              && artifact.version == 4
              && artifact.payload == &semantic_environment
              && core_string_eq(artifact.module,
                                CORE_STRING_STATIC("pkg.library"))
              && !artifact.unit_wide);
    check("unit-wide artifacts are distinct from per-module artifacts",
          core_link_unit_artifact(link, 1, &artifact)
              && artifact.unit_wide && !artifact.module.data
              && artifact.payload == &composed_environment);
    check("consumers can require an exact artifact version",
          core_link_unit_require_artifact(link,
              CORE_STRING_STATIC("pkg.library"),
              CORE_STRING_STATIC("lantern.type-environment"), 4,
              &artifact));

    check("link-unit validation resolves all references",
          core_link_unit_validate(link)
              && core_link_reference_target(from_a) == definition
              && core_link_reference_target(from_b) == definition
              && core_link_reference_target(to_library) == external
              && !core_link_unit_has_fatal_diagnostics(link));
    check("validated use-site references are enumerable by stable handle",
          core_link_unit_reference_count(link) == 6
              && core_link_unit_reference_at_index(link, 2) == site_one
              && core_link_unit_reference_at_index(link, 3) == site_two
              && core_link_unit_reference_at_index(link, 4) == scoped_site
              && core_link_unit_reference_at_index(link, 6) == nullptr
              && core_link_reference_target(site_one) == definition
              && core_link_reference_target(site_two) == definition);
    check("incompatible artifact versions become link diagnostics",
          !core_link_unit_require_artifact(link,
              CORE_STRING_STATIC("pkg.library"),
              CORE_STRING_STATIC("lantern.type-environment"), 3,
              nullptr)
              && core_link_unit_has_fatal_diagnostics(link));

    core_link_unit_free(link);
    core_translation_unit_release(&root_a);
    core_translation_unit_release(&root_b);
    core_translation_unit_release(&shared);
    core_translation_unit_release(&cycle_a);
    core_translation_unit_release(&cycle_b);
}

static void
test_link_unit_conflicts_and_payload_ownership(void)
{
    core_translation_unit_t owner = {0};
    core_translation_unit_t duplicate = {0};
    init_unit(&owner, "pkg.conflict");
    init_unit(&duplicate, "pkg.conflict");
    core_link_unit_t *conflict = core_link_unit_new();
    core_link_unit_add_root(conflict, &owner);
    check("distinct roots with one canonical module id are rejected",
          !core_link_unit_add_root(conflict, &duplicate)
              && core_link_unit_has_fatal_diagnostics(conflict)
              && !core_link_unit_visit_sccs(conflict, trace_link_scc, nullptr));
    core_link_unit_free(conflict);

    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.owner");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_collect(link);
    int first = 1;
    int second = 2;
    check("a definition validates before later link conflicts",
          core_link_unit_define(link, &root, CORE_STRING_STATIC("value"),
              &first, CORE_LINK_PAYLOAD_BORROWED, nullptr, nullptr)
              && core_link_unit_validate(link));
    core_link_declaration_t *duplicate_definition = nullptr;
    check("duplicate declaration identities are diagnosed",
          !core_link_unit_define(link, &root,
              CORE_STRING_STATIC("value"), &second,
              CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &duplicate_definition)
              && !duplicate_definition
              && core_link_unit_has_fatal_diagnostics(link)
              && !core_link_unit_validate(link));
    core_link_unit_free(link);

    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_collect(link);
    core_link_reference_t *missing = nullptr;
    core_link_unit_reference(link, &root, CORE_STRING_STATIC("pkg.absent"),
                             CORE_STRING_STATIC("value"), &missing);
    check("unresolved references become link-unit diagnostics",
          !core_link_unit_validate(link)
              && core_diagnostics_has_fatal(
                  core_link_unit_diagnostics(link))
              && !core_link_reference_target(missing));
    core_link_unit_free(link);

    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_collect(link);
    int *owned = core_alloc(int);
    *owned = 42;
    int before = released_payloads;
    check("owned declaration payload registration succeeds",
          core_link_unit_define(link, &root,
              CORE_STRING_STATIC("owned"), owned,
              CORE_LINK_PAYLOAD_OWNED, release_payload, nullptr));
    core_link_unit_free(link);
    check("link-unit release destroys owned payload exactly once",
          released_payloads == before + 1);
    core_translation_unit_release(&root);
    core_translation_unit_release(&owner);
    core_translation_unit_release(&duplicate);
}

static void
test_link_unit_graph_refresh(void)
{
    core_translation_unit_t root = {0};
    core_translation_unit_t dependency = {0};
    core_translation_unit_t late_dependency = {0};
    core_translation_unit_t after_finalize = {0};
    init_unit(&root, "pkg.refresh_root");
    init_unit(&dependency, "pkg.refresh_dependency");
    init_unit(&late_dependency, "pkg.refresh_late");
    init_unit(&after_finalize, "pkg.refresh_after_finalize");
    core_translation_unit_add_import(&root, &dependency);

    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    check("initial graph generation retains closure edges and SCCs",
          core_link_unit_collect(link)
              && core_link_unit_translation_unit_count(link) == 2
              && core_link_unit_import_edge_count(link) == 1
              && core_link_unit_scc_count(link) == 2);

    core_translation_unit_add_import(&dependency, &late_dependency);
    core_translation_unit_t *importer = nullptr;
    core_translation_unit_t *imported = nullptr;
    check("an import mutation refreshes every LinkUnit graph query",
          core_link_unit_translation_unit_count(link) == 3
              && core_link_unit_import_edge_count(link) == 2
              && core_link_unit_scc_count(link) == 3
              && core_link_unit_import_edge(link, 1, &importer, &imported)
              && importer == &dependency && imported == &late_dependency);

    check("finalization seals every reachable TU import list",
          core_link_unit_finalize_graph(link)
              && core_link_unit_graph_is_finalized(link)
              && core_translation_unit_imports_finalized(&root)
              && core_translation_unit_imports_finalized(&dependency)
              && core_translation_unit_imports_finalized(&late_dependency));
    check("post-finalization imports are rejected without changing the graph",
          !core_translation_unit_add_import(&late_dependency,
                                            &after_finalize)
              && core_link_unit_translation_unit_count(link) == 3
              && core_link_unit_import_edge_count(link) == 2
              && core_translation_unit_has_fatal_diagnostics(
                     &late_dependency));

    core_link_unit_free(link);
    core_translation_unit_release(&root);
    core_translation_unit_release(&dependency);
    core_translation_unit_release(&late_dependency);
    core_translation_unit_release(&after_finalize);
}

static bool
same_int_contract(const void *existing, const void *candidate, void *context)
{
    (void)context;
    return existing && candidate
        && *(const int *)existing == *(const int *)candidate;
}

static void
test_link_unit_typed_declarations(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.typed_declarations");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);

    int signature = 91;
    int compatible_signature = 91;
    int prototype_payload = 1;
    int definition_payload = 2;
    core_link_declaration_metadata_t metadata = {
        .kind = 17,
        .visibility = 3,
        .source_site = &prototype_payload,
        .source_location = {
            .line = 3, .column = 2, .end_line = 3, .end_column = 11,
            .has_location = true,
        },
        .provenance = CORE_STRING_STATIC("n00bb.source-declaration"),
        .provenance_version = 1,
        .contract = &signature,
        .compatible = same_int_contract,
    };
    core_link_declaration_t *prototype = nullptr;
    core_link_declaration_t *definition = nullptr;
    check("typed declaration metadata and compatible completion share identity",
          core_link_unit_register_declaration(link, &root,
              CORE_STRING_STATIC("function.calculate"), &metadata, false,
              &prototype_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &prototype)
              && !core_link_declaration_is_defined(prototype)
              && core_link_declaration_kind(prototype) == 17
              && core_link_declaration_visibility(prototype) == 3);

    metadata.contract = &compatible_signature;
    int call_site = 0;
    core_link_reference_t *typed_reference = nullptr;
    check("references retain their expected declaration kind",
          core_link_unit_reference_at_kind(link, &root, &call_site,
              CORE_STRING_STATIC("pkg.typed_declarations"),
              CORE_STRING_STATIC("function.calculate"), 17,
              &typed_reference)
              && core_link_reference_expected_kind(typed_reference,
                                                    nullptr));
    core_diag_location_t use_location = {
        .line = 22, .column = 5, .end_line = 22, .end_column = 14,
        .has_location = true,
    };
    check("reference provenance is attached to its original use site",
          core_link_reference_set_source_info(link, typed_reference,
              use_location, CORE_STRING_STATIC("n00bb.call-site")));
    metadata.source_site = &definition_payload;
    metadata.source_location.line = 9;
    metadata.source_location.end_line = 9;
    check("a compatible definition completes the canonical declaration",
          core_link_unit_register_declaration(link, &root,
              CORE_STRING_STATIC("function.calculate"), &metadata, true,
              &definition_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &definition)
              && definition == prototype
              && core_link_declaration_is_defined(definition)
              && core_link_declaration_payload(definition)
                     == &definition_payload
              && core_link_declaration_site_count(definition) == 2
              && core_link_unit_validate(link)
              && core_link_reference_target(typed_reference) == definition);
    core_link_declaration_site_view_t definition_site = {0};
    core_diag_location_t attached_use_location = {0};
    core_string_t use_provenance = {0};
    check("declaration origins and source locations remain queryable",
          core_link_declaration_site(definition, 1, &definition_site)
              && definition_site.is_definition
              && definition_site.unit == &root
              && definition_site.source_site == &definition_payload
              && definition_site.location.line == 9
              && definition_site.provenance_version == 1
              && core_link_reference_source_info(typed_reference,
                  &attached_use_location, &use_provenance)
              && attached_use_location.line == 22
              && core_string_eq(use_provenance,
                                CORE_STRING_STATIC("n00bb.call-site")));
    int overload_a_payload = 3;
    int overload_b_payload = 4;
    metadata.overload_group = CORE_STRING_STATIC("function.calculate");
    check("opaque overload groups enumerate distinct canonical declarations",
          core_link_unit_register_declaration(link, &root,
              CORE_STRING_STATIC("function.calculate.i32"), &metadata, true,
              &overload_a_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              nullptr)
              && core_link_unit_register_declaration(link, &root,
                  CORE_STRING_STATIC("function.calculate.f64"), &metadata,
                  true, &overload_b_payload, CORE_LINK_PAYLOAD_BORROWED,
                  nullptr, nullptr)
              && core_link_unit_overload_member_count(link,
                  CORE_STRING_STATIC("pkg.typed_declarations"),
                  CORE_STRING_STATIC("function.calculate")) == 2
              && core_link_unit_overload_member(link,
                  CORE_STRING_STATIC("pkg.typed_declarations"),
                  CORE_STRING_STATIC("function.calculate"), 1)
                     != nullptr
              && core_link_unit_declaration_count(link) == 3
              && core_link_unit_declaration_at(link, 0) == definition);
    core_link_unit_free(link);
    core_translation_unit_release(&root);

    init_unit(&root, "pkg.external_requirement");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    int external_payload = 4;
    core_link_declaration_metadata_t external_metadata = {
        .kind = 29,
        .visibility = 1,
        .contract = &signature,
        .compatible = same_int_contract,
    };
    core_link_declaration_t *external_requirement = nullptr;
    core_link_reference_t *external_use = nullptr;
    check("permitted external requirements remain explicit and valid",
          core_link_unit_require_external(link,
              CORE_STRING_STATIC("runtime.native"),
              CORE_STRING_STATIC("function.write"), &external_metadata,
              &external_requirement)
              && core_link_unit_reference_at_kind(link, &root, &call_site,
                  CORE_STRING_STATIC("runtime.native"),
                  CORE_STRING_STATIC("function.write"), 29, &external_use)
              && core_link_unit_validate(link)
              && !core_link_declaration_is_defined(external_requirement)
              && core_link_declaration_is_external_requirement(
                     external_requirement)
              && core_link_reference_target(external_use)
                     == external_requirement);
    core_link_declaration_t *external_definition = nullptr;
    check("a linked external definition fulfills the same canonical handle",
          core_link_unit_define_external_typed(link,
              CORE_STRING_STATIC("runtime.native"),
              CORE_STRING_STATIC("function.write"), &external_metadata,
              &external_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &external_definition)
              && external_definition == external_requirement
              && core_link_declaration_is_defined(external_definition)
              && core_link_declaration_is_external(external_definition)
              && !core_link_declaration_is_external_requirement(
                     external_definition)
              && core_link_unit_validate(link)
              && core_link_reference_target(external_use)
                     == external_definition);
    core_link_unit_free(link);
    core_translation_unit_release(&root);

    init_unit(&root, "pkg.incomplete_declaration");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    check("validation rejects declarations with no definition or external requirement",
          core_link_unit_register_declaration(link, &root,
              CORE_STRING_STATIC("function.missing"), &metadata, false,
              &prototype_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &prototype)
              && !core_link_unit_validate(link)
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);

    init_unit(&root, "pkg.wrong_kind_reference");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_declaration_t *target = nullptr;
    check("declarations with opaque language kinds can be registered",
          core_link_unit_register_declaration(link, &root,
              CORE_STRING_STATIC("value.answer"), &metadata, true,
              &definition_payload, CORE_LINK_PAYLOAD_BORROWED, nullptr,
              &target));
    check("validation rejects a reference with the wrong expected kind",
          core_link_unit_reference_at_kind(link, &root, &call_site,
              CORE_STRING_STATIC("pkg.wrong_kind_reference"),
              CORE_STRING_STATIC("value.answer"), 18,
              &typed_reference)
              && core_link_reference_set_source_info(link, typed_reference,
                  use_location, CORE_STRING_STATIC("n00bb.call-site"))
              && !core_link_unit_validate(link)
              && core_link_reference_target(typed_reference) == target
              && core_diagnostics_at(core_link_unit_diagnostics(link), 0)
              && core_diagnostics_at(core_link_unit_diagnostics(link), 0)
                     ->location.line == 22
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

typedef struct {
    core_link_stage_result_t result;
    size_t calls;
} stage_probe_t;

static core_link_stage_result_t
run_probe_stage(core_link_unit_t *link, uint32_t kind, void *context)
{
    (void)link;
    (void)kind;
    stage_probe_t *probe = context;
    probe->calls++;
    return probe->result;
}

typedef struct {
    core_translation_unit_t *importer;
    core_translation_unit_t *imported;
} graph_mutation_probe_t;

static core_link_stage_result_t
mutate_graph_stage(core_link_unit_t *link, uint32_t kind, void *context)
{
    (void)link;
    (void)kind;
    graph_mutation_probe_t *probe = context;
    core_translation_unit_add_import(probe->importer, probe->imported);
    return CORE_LINK_STAGE_RESULT_COMPLETE;
}

typedef struct {
    core_link_reference_t *reference;
    core_string_t module;
    core_string_t key;
} deferred_reference_probe_t;

typedef struct {
    core_link_declaration_t *declaration;
    core_string_t role;
    void *handle;
    core_link_stage_result_t result;
} declaration_binding_probe_t;

static core_link_stage_result_t
declaration_binding_stage(core_link_unit_t *link, uint32_t kind,
                          void *context)
{
    declaration_binding_probe_t *probe = context;
    if (!core_link_declaration_set_binding(link, probe->declaration,
            probe->role, kind, probe->handle))
        return CORE_LINK_STAGE_RESULT_FAILED;
    return probe->result;
}

static void
test_link_unit_declaration_bindings(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.binding_root");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);
    core_link_declaration_t *declaration = nullptr;
    core_link_unit_define(link, &root, CORE_STRING_STATIC("function.main"),
        nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr, &declaration);
    static int ir_handle = 29;
    declaration_binding_probe_t probe = {
        .declaration = declaration,
        .role = CORE_STRING_STATIC("test.ir.function"),
        .handle = &ir_handle,
        .result = CORE_LINK_STAGE_RESULT_COMPLETE,
    };
    check("a running producer stage binds a borrowed handle to its declaration",
          core_link_unit_register_stage(link, 92, nullptr, 0,
              CORE_STRING_STATIC("test.ir-bindings"), true,
              declaration_binding_stage, &probe)
              && core_link_unit_run_stage(link, 92)
              && core_link_declaration_binding(declaration, probe.role)
                  == &ir_handle);
    check("invalidating a producer stage clears its declaration bindings",
          core_link_unit_invalidate_stage(link, 92)
              && !core_link_declaration_binding(declaration, probe.role));
    probe.result = CORE_LINK_STAGE_RESULT_FAILED;
    check("failed binding stages do not leave partial handles",
          !core_link_unit_run_stage(link, 92)
              && !core_link_declaration_binding(declaration, probe.role));
    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

static core_link_stage_result_t
resolve_deferred_reference_stage(core_link_unit_t *link, uint32_t kind,
                                 void *context)
{
    (void)kind;
    deferred_reference_probe_t *probe = context;
    return core_link_unit_resolve_reference(link, probe->reference,
        probe->module, probe->key)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}

typedef struct {
    core_link_contribution_state_t state;
    core_link_stage_result_t result;
    int value;
    int observed;
    bool writes;
} contribution_stage_probe_t;

static core_link_stage_result_t
contribution_stage(core_link_unit_t *link, uint32_t kind, void *context)
{
    contribution_stage_probe_t *probe = context;
    if (probe->writes) {
        int *payload = nullptr;
        if (probe->state == CORE_LINK_CONTRIBUTION_COMPLETE) {
            payload = core_alloc(int);
            *payload = probe->value;
        }
        bool ok = core_link_unit_set_contribution(link,
            CORE_STRING_STATIC("pkg.contribution"),
            CORE_STRING_STATIC("type.inference"), 1, kind, probe->state,
            CORE_STRING_STATIC("test.type-inference"), 3, payload,
            payload ? CORE_LINK_PAYLOAD_OWNED : CORE_LINK_PAYLOAD_BORROWED,
            payload ? release_payload : nullptr);
        if (!ok) core_free(payload);
        return ok ? probe->result : CORE_LINK_STAGE_RESULT_FAILED;
    }

    core_link_contribution_view_t view;
    if (!core_link_unit_require_contribution(link,
            CORE_STRING_STATIC("pkg.contribution"),
            CORE_STRING_STATIC("type.inference"), 1, &view))
        return CORE_LINK_STAGE_RESULT_FAILED;
    probe->observed = view.payload ? *(const int *)view.payload : -1;
    return probe->result;
}

static void
test_link_unit_stage_driver(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.stage_driver");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);

    stage_probe_t prerequisite = {
        .result = CORE_LINK_STAGE_RESULT_UNAVAILABLE,
    };
    stage_probe_t dependent = {
        .result = CORE_LINK_STAGE_RESULT_COMPLETE,
    };
    uint32_t depends_on[] = {10};
    check("stage definitions record prerequisite and provenance",
          core_link_unit_register_stage(link, 10, nullptr, 0,
              CORE_STRING_STATIC("frontend.symbol-index"), true,
              run_probe_stage, &prerequisite)
              && core_link_unit_register_stage(link, 20, depends_on, 1,
                  CORE_STRING_STATIC("frontend.type-analysis"), true,
                  run_probe_stage, &dependent)
              && core_link_unit_stage_count(link) == 2
              && core_link_unit_stage_kind(link, 1) == 20
              && core_string_eq(core_link_unit_stage_provenance(link, 10),
                                CORE_STRING_STATIC("frontend.symbol-index")));
    check("unavailable prerequisites block dependents without failing them",
          !core_link_unit_run_stage(link, 20)
              && prerequisite.calls == 1 && dependent.calls == 0
              && core_link_unit_stage_state(link, 10)
                     == CORE_LINK_STAGE_UNAVAILABLE
              && core_link_unit_stage_state(link, 20)
                     == CORE_LINK_STAGE_UNAVAILABLE);
    prerequisite.result = CORE_LINK_STAGE_RESULT_COMPLETE;
    check("invalidation resets the stage and transitive dependents",
          core_link_unit_invalidate_stage(link, 10)
              && core_link_unit_stage_state(link, 10)
                     == CORE_LINK_STAGE_PENDING
              && core_link_unit_stage_state(link, 20)
                     == CORE_LINK_STAGE_PENDING
              && core_link_unit_run_stage(link, 20)
              && prerequisite.calls == 2 && dependent.calls == 1
              && core_link_unit_stage_state(link, 20)
                     == CORE_LINK_STAGE_COMPLETE);
    check("stage registration closes once execution begins",
          !core_link_unit_register_stage(link, 30, nullptr, 0,
              CORE_STRING_STATIC("late"), false, run_probe_stage,
              &dependent));

    core_link_unit_free(link);
    core_translation_unit_release(&root);

    init_unit(&root, "pkg.cyclic_stages");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    uint32_t prereq_a[] = {52};
    uint32_t prereq_b[] = {51};
    check("analysis-stage cycles are diagnosed instead of recursing forever",
          core_link_unit_register_stage(link, 51, prereq_a, 1,
              CORE_STRING_STATIC("test.stage-a"), false,
              run_probe_stage, &dependent)
              && core_link_unit_register_stage(link, 52, prereq_b, 1,
                  CORE_STRING_STATIC("test.stage-b"), false,
                  run_probe_stage, &dependent)
              && !core_link_unit_run_stage(link, 51)
              && core_link_unit_stage_state(link, 51)
                     == CORE_LINK_STAGE_FAILED
              && core_link_unit_stage_state(link, 52)
                     == CORE_LINK_STAGE_FAILED
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);

    core_translation_unit_t late = {0};
    init_unit(&root, "pkg.graph_mutation_during_stage");
    init_unit(&late, "pkg.graph_mutation_during_stage_late");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    graph_mutation_probe_t mutation = {
        .importer = &root,
        .imported = &late,
    };
    check("an analysis stage cannot publish against a changed graph",
          core_link_unit_register_stage(link, 35, nullptr, 0,
              CORE_STRING_STATIC("test.mutating-stage"), false,
              mutate_graph_stage, &mutation)
              && !core_link_unit_run_stage(link, 35)
              && core_link_unit_stage_state(link, 35)
                     == CORE_LINK_STAGE_FAILED
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);
    core_translation_unit_release(&late);

    init_unit(&root, "pkg.unfinalized_stage");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    stage_probe_t guarded = {.result = CORE_LINK_STAGE_RESULT_COMPLETE};
    check("stages may require a finalized graph",
          core_link_unit_register_stage(link, 40, nullptr, 0,
              CORE_STRING_STATIC("link.analysis"), true,
              run_probe_stage, &guarded)
              && !core_link_unit_run_stage(link, 40)
              && guarded.calls == 0
              && core_link_unit_stage_state(link, 40)
                     == CORE_LINK_STAGE_FAILED
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

static void
test_link_unit_deferred_references(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.deferred_reference");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);
    int signature = 2;
    int implementation = 3;
    core_link_declaration_metadata_t metadata = {
        .kind = 12,
        .contract = &signature,
        .compatible = same_int_contract,
    };
    core_link_declaration_t *target = nullptr;
    core_link_unit_define_external_typed(link,
        CORE_STRING_STATIC("pkg.actual"), CORE_STRING_STATIC("function.go"),
        &metadata, &implementation, CORE_LINK_PAYLOAD_BORROWED, nullptr,
        &target);
    int use_site = 0;
    int source_scope = 0;
    core_link_reference_t *reference = nullptr;
    deferred_reference_probe_t resolver = {
        .reference = nullptr,
        .module = CORE_STRING_STATIC("pkg.actual"),
        .key = CORE_STRING_STATIC("function.go"),
    };
    check("deferred references retain reason and prerequisite state",
          core_link_unit_defer_reference_at_scoped_kind(link, &root,
              &use_site, &source_scope, 12,
              CORE_STRING_STATIC("member resolution awaits import closure"),
              80, &reference)
              && (resolver.reference = reference)
              && core_link_reference_is_deferred(reference)
              && core_link_reference_use_site(reference) == &use_site
              && core_link_reference_source_scope(reference) == &source_scope
              && core_link_reference_expected_kind(reference, nullptr)
              && !core_link_reference_module(reference).data
              && !core_link_reference_key(reference).data
              && core_string_eq(core_link_reference_deferral_reason(reference),
                  CORE_STRING_STATIC("member resolution awaits import closure"))
              && core_link_reference_deferral_prerequisite(reference,
                                                            nullptr)
              && core_link_unit_register_stage(link, 80, nullptr, 0,
                  CORE_STRING_STATIC("frontend.deferred-name-resolution"),
                  true, resolve_deferred_reference_stage, &resolver)
              && core_link_unit_validate(link)
              && core_link_reference_target(reference) == nullptr);
    check("a prerequisite resolves the same use-site reference canonically",
          core_link_unit_run_stage(link, 80)
              && !core_link_reference_is_deferred(reference)
              && core_link_unit_validate(link)
              && core_link_reference_target(reference) == target);
    core_link_unit_free(link);
    core_translation_unit_release(&root);

    init_unit(&root, "pkg.stale_deferred_reference");
    link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);
    core_link_unit_reference_at(link, &root, nullptr,
        CORE_STRING_STATIC("pkg.late"), CORE_STRING_STATIC("member"),
        &reference);
    core_link_unit_defer_reference(link, reference,
        CORE_STRING_STATIC("cycle resolution"), 81);
    stage_probe_t completed = {.result = CORE_LINK_STAGE_RESULT_COMPLETE};
    core_link_unit_register_stage(link, 81, nullptr, 0,
        CORE_STRING_STATIC("frontend.resolve-cycle"), true,
        run_probe_stage, &completed);
    check("an unfulfilled deferral fails after its prerequisite completes",
          core_link_unit_validate(link)
              && core_link_unit_run_stage(link, 81)
              && !core_link_unit_validate(link)
              && core_link_unit_has_fatal_diagnostics(link));
    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

static void
test_link_unit_contributions(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.contribution_root");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);
    core_link_contribution_view_t view;
    check("a completed empty fact is distinct from a missing contribution",
          core_link_unit_set_contribution(link,
              CORE_STRING_STATIC("pkg.contribution"),
              CORE_STRING_STATIC("alias.summary"), 1, 0,
              CORE_LINK_CONTRIBUTION_COMPLETE,
              CORE_STRING_STATIC("test.empty-summary"), 1, nullptr,
              CORE_LINK_PAYLOAD_BORROWED, nullptr)
              && core_link_unit_require_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("alias.summary"), 1, &view)
              && view.state == CORE_LINK_CONTRIBUTION_COMPLETE
              && view.payload == nullptr
              && !core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.missing"),
                  CORE_STRING_STATIC("alias.summary"), &view));
    contribution_stage_probe_t producer = {
        .state = CORE_LINK_CONTRIBUTION_COMPLETE,
        .result = CORE_LINK_STAGE_RESULT_COMPLETE,
        .value = 17,
        .writes = true,
    };
    contribution_stage_probe_t consumer = {
        .result = CORE_LINK_STAGE_RESULT_COMPLETE,
    };
    contribution_stage_probe_t unavailable = {
        .state = CORE_LINK_CONTRIBUTION_PENDING,
        .result = CORE_LINK_STAGE_RESULT_UNAVAILABLE,
        .writes = true,
    };
    contribution_stage_probe_t failed = {
        .state = CORE_LINK_CONTRIBUTION_COMPLETE,
        .result = CORE_LINK_STAGE_RESULT_FAILED,
        .value = 99,
        .writes = true,
    };
    stage_probe_t blocked = {.result = CORE_LINK_STAGE_RESULT_COMPLETE};
    uint32_t producer_prerequisite[] = {10};
    uint32_t unavailable_prerequisite[] = {30};
    check("analysis contributions record explicit stage outcomes",
          core_link_unit_register_stage(link, 10, nullptr, 0,
              CORE_STRING_STATIC("type-inference"), true,
              contribution_stage, &producer)
          && core_link_unit_register_stage(link, 20, producer_prerequisite,
              1, CORE_STRING_STATIC("dispatch-consumer"), true,
              contribution_stage, &consumer)
          && core_link_unit_register_stage(link, 30, nullptr, 0,
              CORE_STRING_STATIC("effects-unavailable"), true,
              contribution_stage, &unavailable)
          && core_link_unit_register_stage(link, 40,
              unavailable_prerequisite, 1,
              CORE_STRING_STATIC("dependent-effects"), true,
              run_probe_stage, &blocked)
          && core_link_unit_register_stage(link, 50, nullptr, 0,
              CORE_STRING_STATIC("failed-contribution"), true,
              contribution_stage, &failed));

    check("a consumer can require a complete producer contribution",
          core_link_unit_run_stage(link, 20)
              && consumer.observed == 17
              && core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("type.inference"), &view)
              && view.state == CORE_LINK_CONTRIBUTION_COMPLETE
              && view.producer_stage == 10 && view.payload
              && core_string_eq(view.provenance,
                  CORE_STRING_STATIC("test.type-inference")));

    int released_before_invalidation = released_payloads;
    check("stage invalidation resets its dependent contribution to pending",
          core_link_unit_invalidate_stage(link, 10)
              && core_link_unit_stage_state(link, 20)
                     == CORE_LINK_STAGE_PENDING
              && core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("type.inference"), &view)
              && view.state == CORE_LINK_CONTRIBUTION_PENDING
              && view.payload == nullptr
              && released_payloads == released_before_invalidation + 1);
    check("invalidated analysis can recompute the contribution",
          core_link_unit_run_stage(link, 20)
              && consumer.observed == 17
              && core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("type.inference"), &view)
              && view.state == CORE_LINK_CONTRIBUTION_COMPLETE);

    check("unavailable analyses remain distinct from empty complete facts",
          !core_link_unit_run_stage(link, 40)
              && blocked.calls == 0
              && core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("type.inference"), &view)
              && view.state == CORE_LINK_CONTRIBUTION_UNAVAILABLE
              && view.producer_stage == 30);
    check("failed stages cannot publish stale contribution payloads",
          !core_link_unit_run_stage(link, 50)
              && core_link_unit_contribution(link,
                  CORE_STRING_STATIC("pkg.contribution"),
                  CORE_STRING_STATIC("type.inference"), &view)
              && view.state == CORE_LINK_CONTRIBUTION_FAILED
              && view.payload == nullptr);

    const core_link_contribution_state_t rejected_states[] = {
        CORE_LINK_CONTRIBUTION_PENDING,
        CORE_LINK_CONTRIBUTION_UNAVAILABLE,
        CORE_LINK_CONTRIBUTION_FAILED,
    };
    const char *rejected_domains[] = {
        "consumer.pending", "consumer.unavailable", "consumer.failed",
    };
    bool rejected_noncomplete_states = true;
    for (size_t i = 0; i < sizeof(rejected_states)
            / sizeof(rejected_states[0]); i++) {
        rejected_noncomplete_states &= core_link_unit_set_contribution(link,
            CORE_STRING_STATIC("pkg.contribution"),
            core_string_from_cstr(rejected_domains[i]), 1, 0,
            rejected_states[i], CORE_STRING_STATIC("test.consumer-state"),
            1, nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr);
        view = (core_link_contribution_view_t){
            .payload = (void *)1,
            .version = UINT32_MAX,
        };
        rejected_noncomplete_states &= !core_link_unit_require_contribution(
            link, CORE_STRING_STATIC("pkg.contribution"),
            core_string_from_cstr(rejected_domains[i]), 1, &view)
            && !view.payload && !view.version;
    }
    check("consumers reject deferred, unavailable, and failed contributions",
          rejected_noncomplete_states);

    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

typedef struct {
    core_link_declaration_t *from;
    core_link_declaration_t *to;
    const void *site;
    core_link_stage_result_t result;
    core_link_dependency_state_t state;
} dependency_stage_probe_t;

static core_link_stage_result_t
dependency_stage(core_link_unit_t *link, uint32_t kind, void *context)
{
    dependency_stage_probe_t *probe = context;
    if (!core_link_unit_add_dependency(link, 1, kind, probe->state,
            probe->from, probe->to, core_link_unit_root(link, 0), probe->site,
            (core_diag_location_t){.line = 4, .column = 7,
                .has_location = true},
            CORE_STRING_STATIC("test.call-site"), 2, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    return probe->result;
}

static void
test_link_unit_dependencies(void)
{
    core_translation_unit_t root = {0};
    init_unit(&root, "pkg.dependency_root");
    core_link_unit_t *link = core_link_unit_new();
    core_link_unit_add_root(link, &root);
    core_link_unit_finalize_graph(link);
    core_link_declaration_t *caller = nullptr, *callee = nullptr;
    core_link_unit_define(link, &root, CORE_STRING_STATIC("caller"), nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr, &caller);
    core_link_unit_define(link, &root, CORE_STRING_STATIC("callee"), nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr, &callee);
    static int call_site;
    dependency_stage_probe_t probe = {
        .from = caller,
        .to = callee,
        .site = &call_site,
        .result = CORE_LINK_STAGE_RESULT_COMPLETE,
        .state = CORE_LINK_DEPENDENCY_RESOLVED,
    };
    check("dependency edges require canonical declarations and a running producer",
          core_link_unit_register_stage(link, 91, nullptr, 0,
              CORE_STRING_STATIC("test.call-graph"), true,
              dependency_stage, &probe)
              && core_link_unit_run_stage(link, 91)
              && core_link_unit_dependency_count(link) == 1);
    core_link_dependency_view_t edge;
    check("dependency edge queries retain canonical endpoints and origin",
          core_link_unit_dependency_at(link, 0, &edge)
              && edge.kind == 1 && edge.producer_stage == 91
              && edge.state == CORE_LINK_DEPENDENCY_RESOLVED
              && edge.from == caller && edge.to == callee
              && edge.source_unit == &root && edge.source_site == &call_site
              && edge.source_location.line == 4
              && edge.source_location.column == 7
              && core_string_eq(edge.provenance,
                                CORE_STRING_STATIC("test.call-site"))
              && edge.provenance_version == 2);
    check("stage invalidation removes its published dependency edges",
          core_link_unit_invalidate_stage(link, 91)
              && core_link_unit_dependency_count(link) == 0);
    probe.to = nullptr;
    probe.state = CORE_LINK_DEPENDENCY_UNAVAILABLE;
    check("unresolved sites are retained as explicit dependency facts",
          core_link_unit_run_stage(link, 91)
              && core_link_unit_dependency_count(link) == 1
              && core_link_unit_dependency_at(link, 0, &edge)
              && edge.state == CORE_LINK_DEPENDENCY_UNAVAILABLE
              && edge.from == caller && edge.to == nullptr
              && edge.source_site == &call_site);
    core_link_unit_invalidate_stage(link, 91);
    probe.to = callee;
    probe.state = CORE_LINK_DEPENDENCY_RESOLVED;
    probe.result = CORE_LINK_STAGE_RESULT_FAILED;
    check("failed dependency stages do not leave partial edges",
          !core_link_unit_run_stage(link, 91)
              && core_link_unit_dependency_count(link) == 0);
    core_link_unit_free(link);
    core_translation_unit_release(&root);
}

int
main(void)
{
    test_link_unit_closure_and_linkage();
    test_link_unit_conflicts_and_payload_ownership();
    test_link_unit_graph_refresh();
    test_link_unit_typed_declarations();
    test_link_unit_stage_driver();
    test_link_unit_contributions();
    test_link_unit_dependencies();
    test_link_unit_deferred_references();
    test_link_unit_declaration_bindings();
    return report("lib-link-unit");
}
