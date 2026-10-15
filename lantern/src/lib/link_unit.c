#include "lib/link_unit.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    core_string_t module;
    core_string_t format;
    uint32_t version;
    uint32_t producer_stage;
    void *payload;
    core_link_payload_ownership_t ownership;
    core_link_payload_free_fn free_fn;
    bool unit_wide;
} core_link_artifact_t;

static core_hash_value_t
artifact_key_hash(void *opaque)
{
    const core_link_artifact_t *key = opaque;
    core_hash_value_t fields[] = {
        core_string_hash(key->module),
        core_string_hash(key->format),
        key->unit_wide,
    };
    return core_hash_raw(fields, sizeof(fields));
}

static bool
artifact_key_eq(const void *left, const void *right)
{
    const core_link_artifact_t *a = left;
    const core_link_artifact_t *b = right;
    return a->unit_wide == b->unit_wide
        && core_string_eq(a->module, b->module)
        && core_string_eq(a->format, b->format);
}

typedef struct {
    core_translation_unit_t *importer;
    core_translation_unit_t *imported;
} core_link_import_edge_t;

typedef struct {
    core_translation_unit_t **members;
    size_t member_count;
} core_link_scc_t;

typedef struct {
    uint32_t kind;
    uint32_t *prerequisites;
    size_t prerequisite_count;
    core_string_t provenance;
    bool requires_finalized_graph;
    core_link_stage_fn callback;
    void *context;
    core_link_stage_state_t state;
} core_link_stage_record_t;

typedef struct {
    core_string_t module;
    core_string_t domain;
    uint32_t version;
    uint32_t producer_stage;
    core_link_contribution_state_t state;
    core_string_t provenance;
    uint32_t provenance_version;
    void *payload;
    core_link_payload_ownership_t ownership;
    core_link_payload_free_fn free_fn;
    bool unit_wide;
} core_link_contribution_t;

static core_hash_value_t
contribution_hash(void *opaque)
{
    const core_link_contribution_t *contribution = opaque;
    core_hash_value_t fields[] = {
        core_string_hash(contribution->module),
        core_string_hash(contribution->domain),
        contribution->unit_wide,
    };
    return core_hash_raw(fields, sizeof fields);
}

static bool
contribution_eq(const void *left, const void *right)
{
    const core_link_contribution_t *a = left;
    const core_link_contribution_t *b = right;
    return a->unit_wide == b->unit_wide
        && core_string_eq(a->module, b->module)
        && core_string_eq(a->domain, b->domain);
}

struct core_link_dependency_t {
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
};

static core_hash_value_t
dependency_hash(void *opaque)
{
    const core_link_dependency_t *dependency = opaque;
    uintptr_t fields[] = {
        dependency->kind, dependency->producer_stage, dependency->state,
        (uintptr_t)dependency->from, (uintptr_t)dependency->to,
        (uintptr_t)dependency->source_unit,
        (uintptr_t)dependency->source_site,
    };
    return core_hash_raw(fields, sizeof fields);
}

static bool
dependency_eq(const void *left, const void *right)
{
    const core_link_dependency_t *a = left;
    const core_link_dependency_t *b = right;
    return a->kind == b->kind
        && a->producer_stage == b->producer_stage
        && a->state == b->state && a->from == b->from && a->to == b->to
        && a->source_unit == b->source_unit
        && a->source_site == b->source_site;
}

typedef struct {
    core_translation_unit_t *unit;
    const void *source_site;
    core_diag_location_t location;
    core_string_t provenance;
    uint32_t provenance_version;
    core_link_origin_kind_t origin;
    bool is_definition;
} core_link_decl_site_t;

typedef struct {
    core_string_t role;
    uint32_t producer_stage;
    void *handle;
} core_link_decl_binding_t;

struct core_link_declaration_t {
    core_string_t module;
    core_string_t key;
    core_translation_unit_t *owner;
    uint32_t kind;
    uint32_t visibility;
    core_string_t overload_group;
    core_link_decl_site_t *sites;
    size_t site_count;
    size_t site_capacity;
    core_link_decl_binding_t *bindings;
    size_t binding_count;
    size_t binding_capacity;
    const void *contract;
    core_link_contract_compatible_fn compatible;
    void *compatibility_context;
    void *payload;
    core_link_payload_ownership_t ownership;
    core_link_payload_free_fn free_fn;
    bool external;
    bool is_defined;
    bool external_requirement;
};

struct core_link_reference_t {
    core_link_unit_t *owner;
    core_translation_unit_t *referrer;
    const void *use_site;
    const void *source_scope;
    core_string_t target_module;
    core_string_t declaration_key;
    bool has_expected_kind;
    uint32_t expected_kind;
    bool deferred;
    core_string_t deferral_reason;
    uint32_t deferral_prerequisite;
    core_diag_location_t source_location;
    core_string_t provenance;
    core_link_declaration_t *target;
    core_link_reference_t *next_at_site;
    core_link_reference_t *last_at_site;
};

static core_hash_value_t
reference_site_hash(void *opaque)
{
    const core_link_reference_t *reference = opaque;
    uintptr_t fields[] = {
        (uintptr_t)reference->referrer, (uintptr_t)reference->use_site,
    };
    return core_hash_raw(fields, sizeof fields);
}

static bool
reference_site_eq(const void *left, const void *right)
{
    const core_link_reference_t *a = left;
    const core_link_reference_t *b = right;
    return a->referrer == b->referrer && a->use_site == b->use_site;
}

static core_hash_value_t
declaration_hash(void *opaque)
{
    const core_link_declaration_t *decl = opaque;
    core_hash_value_t fields[] = {
        core_string_hash(decl->module), core_string_hash(decl->key),
    };
    return core_hash_raw(fields, sizeof fields);
}

static bool
declaration_eq(const void *left, const void *right)
{
    const core_link_declaration_t *a = left;
    const core_link_declaration_t *b = right;
    return core_string_eq(a->module, b->module)
        && core_string_eq(a->key, b->key);
}

struct core_link_unit_t {
    core_translation_unit_t **roots;
    size_t root_count;
    size_t root_capacity;
    core_dict_t root_index;
    core_dict_t root_module_index;
    core_translation_unit_t **units;
    size_t unit_count;
    size_t unit_capacity;
    core_dict_t unit_index;
    core_dict_t module_index;
    uint64_t *unit_import_revisions;
    core_link_import_edge_t *import_edges;
    size_t import_edge_count;
    size_t import_edge_capacity;
    core_link_scc_t *sccs;
    size_t scc_count;
    size_t scc_capacity;
    core_dict_t scc_index;
    core_link_declaration_t **declarations;
    size_t declaration_count;
    size_t declaration_capacity;
    core_dict_t declaration_index;
    core_link_reference_t **references;
    size_t reference_count;
    size_t reference_capacity;
    core_dict_t reference_index;
    core_link_artifact_t **artifacts;
    size_t artifact_count;
    size_t artifact_capacity;
    core_dict_t artifact_index;
    core_link_stage_record_t *stages;
    size_t stage_count;
    size_t stage_capacity;
    core_dict_t stage_index;
    core_link_contribution_t **contributions;
    size_t contribution_count;
    size_t contribution_capacity;
    core_dict_t contribution_index;
    core_link_dependency_t **dependencies;
    size_t dependency_count;
    size_t dependency_capacity;
    core_dict_t dependency_index;
    core_diagnostics_t *diagnostics;
    uint64_t graph_generation;
    bool collected;
    bool collection_valid;
    bool graph_finalized;
    bool stages_started;
    bool validated;
    bool validation_valid;
};

static core_link_stage_record_t *
find_stage(const core_link_unit_t *link, uint32_t kind)
{
    size_t index = (size_t)(uintptr_t)core_dict_get(
        (core_dict_t *)&link->stage_index,
        (void *)(uintptr_t)kind, nullptr);
    return index ? &link->stages[index - 1] : nullptr;
}

static void
release_artifact(core_link_artifact_t *artifact)
{
    if (artifact->ownership == CORE_LINK_PAYLOAD_OWNED)
        artifact->free_fn(artifact->payload);
    core_free(artifact->module.data);
    core_free(artifact->format.data);
    core_free(artifact);
}

static void
clear_contribution_payload(core_link_contribution_t *contribution)
{
    if (contribution->ownership == CORE_LINK_PAYLOAD_OWNED
        && contribution->payload)
        contribution->free_fn(contribution->payload);
    contribution->payload = nullptr;
    contribution->ownership = CORE_LINK_PAYLOAD_BORROWED;
    contribution->free_fn = nullptr;
}

static void
remove_dependency_at(core_link_unit_t *link, size_t index)
{
    core_link_dependency_t *dependency = link->dependencies[index];
    if (!core_dict_remove(&link->dependency_index, dependency)) abort();
    core_free(dependency->provenance.data);
    core_free(dependency);
    memmove(link->dependencies + index, link->dependencies + index + 1,
        (link->dependency_count - index - 1) * sizeof(*link->dependencies));
    link->dependency_count--;
}

static void
remove_stage_dependencies(core_link_unit_t *link, const bool *invalidated)
{
    for (size_t i = 0; i < link->dependency_count;) {
        core_link_dependency_t *dependency = link->dependencies[i];
        core_link_stage_record_t *producer = dependency->producer_stage
            ? find_stage(link, dependency->producer_stage) : nullptr;
        if (!producer || !invalidated[producer - link->stages]) {
            i++;
            continue;
        }
        remove_dependency_at(link, i);
    }
}

static void
finish_stage_dependencies(core_link_unit_t *link, uint32_t stage_kind)
{
    for (size_t i = 0; i < link->dependency_count;) {
        core_link_dependency_t *dependency = link->dependencies[i];
        if (dependency->producer_stage != stage_kind) {
            i++;
            continue;
        }
        remove_dependency_at(link, i);
    }
}

static void
reset_stage_contributions(core_link_unit_t *link, const bool *invalidated)
{
    for (size_t i = 0; i < link->contribution_count; i++) {
        core_link_contribution_t *contribution = link->contributions[i];
        if (!contribution->producer_stage) continue;
        core_link_stage_record_t *producer =
            find_stage(link, contribution->producer_stage);
        if (!producer || !invalidated[producer - link->stages]) continue;
        clear_contribution_payload(contribution);
        contribution->state = CORE_LINK_CONTRIBUTION_PENDING;
    }
}

static void
reset_stage_bindings(core_link_unit_t *link, const bool *invalidated)
{
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *declaration = link->declarations[i];
        for (size_t j = 0; j < declaration->binding_count;) {
            core_link_decl_binding_t *binding = &declaration->bindings[j];
            core_link_stage_record_t *producer = binding->producer_stage
                ? find_stage(link, binding->producer_stage) : nullptr;
            if (!producer || !invalidated[producer - link->stages]) {
                j++;
                continue;
            }
            core_free(binding->role.data);
            memmove(binding, binding + 1,
                (declaration->binding_count - j - 1) * sizeof(*binding));
            declaration->binding_count--;
        }
    }
}

static void
remove_stage_artifacts(core_link_unit_t *link, const bool *invalidated,
                       uint32_t stage_kind)
{
    for (size_t i = 0; i < link->artifact_count;) {
        core_link_artifact_t *artifact = link->artifacts[i];
        core_link_stage_record_t *producer = artifact->producer_stage
            ? find_stage(link, artifact->producer_stage) : nullptr;
        if (!producer || (invalidated
                ? !invalidated[producer - link->stages]
                : artifact->producer_stage != stage_kind)) {
            i++;
            continue;
        }
        if (!core_dict_remove(&link->artifact_index, artifact)) abort();
        release_artifact(artifact);
        memmove(link->artifacts + i, link->artifacts + i + 1,
            (link->artifact_count - i - 1) * sizeof(*link->artifacts));
        link->artifact_count--;
    }
}

static void
finish_stage_bindings(core_link_unit_t *link, uint32_t stage_kind)
{
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *declaration = link->declarations[i];
        for (size_t j = 0; j < declaration->binding_count;) {
            core_link_decl_binding_t *binding = &declaration->bindings[j];
            if (binding->producer_stage != stage_kind) {
                j++;
                continue;
            }
            core_free(binding->role.data);
            memmove(binding, binding + 1,
                (declaration->binding_count - j - 1) * sizeof(*binding));
            declaration->binding_count--;
        }
    }
}

static void
invalidate_stages(core_link_unit_t *link)
{
    bool *invalidated = link->stage_count
        ? core_alloc_array(bool, link->stage_count) : nullptr;
    for (size_t i = 0; i < link->stage_count; i++) {
        link->stages[i].state = CORE_LINK_STAGE_PENDING;
        invalidated[i] = true;
    }
    if (invalidated) reset_stage_contributions(link, invalidated);
    if (invalidated) reset_stage_bindings(link, invalidated);
    if (invalidated) remove_stage_artifacts(link, invalidated, 0);
    if (invalidated) remove_stage_dependencies(link, invalidated);
    core_free(invalidated);
    link->stages_started = false;
}

static void
finish_stage_contributions(core_link_unit_t *link, uint32_t stage_kind,
                           core_link_stage_result_t result)
{
    core_link_contribution_state_t state;
    if (result == CORE_LINK_STAGE_RESULT_COMPLETE) return;
    state = result == CORE_LINK_STAGE_RESULT_UNAVAILABLE
        ? CORE_LINK_CONTRIBUTION_UNAVAILABLE
        : CORE_LINK_CONTRIBUTION_FAILED;
    for (size_t i = 0; i < link->contribution_count; i++) {
        core_link_contribution_t *contribution = link->contributions[i];
        if (contribution->producer_stage != stage_kind) continue;
        clear_contribution_payload(contribution);
        contribution->state = state;
    }
    finish_stage_bindings(link, stage_kind);
}

static bool
link_report_at(core_link_unit_t *link, const core_translation_unit_t *unit,
               core_diag_location_t location, const char *code,
               const char *message)
{
    link->validated = false;
    return core_diagnostics_report_at(link->diagnostics, unit,
        CORE_DIAG_FATAL, core_string_from_cstr(code),
        core_string_from_cstr(message), location);
}

static bool
link_report(core_link_unit_t *link, const core_translation_unit_t *unit,
            const char *code, const char *message)
{
    return link_report_at(link, unit, (core_diag_location_t){0}, code,
                          message);
}

static bool
payload_args_valid(core_link_payload_ownership_t ownership,
                   core_link_payload_free_fn free_fn)
{
    return (ownership == CORE_LINK_PAYLOAD_BORROWED && !free_fn)
        || (ownership == CORE_LINK_PAYLOAD_OWNED && free_fn);
}

static bool
collect_one(core_link_unit_t *link, core_translation_unit_t *unit,
            bool *valid)
{
    if (core_dict_contains(&link->unit_index, unit)) return true;
    core_dict_add(&link->unit_index, unit, unit);

    bool found = false;
    core_dict_get(&link->module_index, &unit->module.identifier, &found);
    if (found) {
        link_report(link, unit, "LANTERN-L0001",
            "link unit contains distinct translation units with the same module identity");
        *valid = false;
    }
    else core_dict_add(&link->module_index, &unit->module.identifier, unit);

    if (!core_list_reserve_raw((void **)&link->units, &link->unit_capacity,
                               link->unit_count + 1, sizeof(*link->units)))
        abort();
    link->units[link->unit_count++] = unit;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        if (!collect_one(link, core_translation_unit_import(unit, i), valid))
            return false;
    }
    return true;
}

static void
clear_sccs(core_link_unit_t *link)
{
    for (size_t i = 0; i < link->scc_count; i++)
        core_free(link->sccs[i].members);
    link->scc_count = 0;
    core_dict_free(&link->scc_index);
    core_dict_init(&link->scc_index, nullptr, nullptr);
}

static bool
graph_snapshot_current(const core_link_unit_t *link)
{
    if (!link->collected) return false;
    for (size_t i = 0; i < link->unit_count; i++)
        if (link->unit_import_revisions[i]
            != core_translation_unit_import_revision(link->units[i]))
            return false;
    return true;
}

static bool
module_name_less(core_string_t left, core_string_t right)
{
    size_t common = left.u8_bytes < right.u8_bytes
        ? left.u8_bytes : right.u8_bytes;
    int comparison = memcmp(left.data, right.data, common);
    return comparison < 0 || (!comparison && left.u8_bytes < right.u8_bytes);
}

typedef struct {
    core_translation_unit_t *const *members;
    core_translation_unit_t **ordered;
    core_dict_t positions;
    bool *visited;
    size_t count;
} core_link_scc_order_t;

static void
order_scc_member(core_link_scc_order_t *order, size_t at)
{
    order->visited[at] = true;
    core_translation_unit_t *unit = order->members[at];
    order->ordered[order->count++] = unit;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported = core_translation_unit_import(unit, i);
        size_t position = (size_t)(uintptr_t)core_dict_get(
            &order->positions, imported, nullptr);
        if (position && !order->visited[position - 1])
            order_scc_member(order, position - 1);
    }
}

static bool
store_link_scc(void *context, core_translation_unit_t *const *members,
               size_t member_count)
{
    core_link_unit_t *link = context;
    core_translation_unit_t **copy = core_alloc_array(
        core_translation_unit_t *, member_count);
    bool *visited = core_alloc_array(bool, member_count);
    memset(visited, 0, member_count * sizeof(*visited));
    core_link_scc_order_t order = {
        .members = members, .ordered = copy, .visited = visited,
    };
    core_dict_init(&order.positions, nullptr, nullptr);
    size_t first = 0;
    for (size_t i = 0; i < member_count; i++) {
        core_dict_add(&order.positions, members[i], i + 1);
        if (module_name_less(members[i]->module.identifier,
                             members[first]->module.identifier)) first = i;
    }
    order_scc_member(&order, first);
    core_dict_free(&order.positions);
    core_free(visited);
    if (order.count != member_count) {
        core_free(copy);
        return false;
    }
    if (!core_list_reserve_raw((void **)&link->sccs, &link->scc_capacity,
                               link->scc_count + 1, sizeof(*link->sccs)))
        abort();
    link->sccs[link->scc_count++] = (core_link_scc_t){
        .members = copy,
        .member_count = member_count,
    };
    for (size_t i = 0; i < member_count; i++)
        core_dict_add(&link->scc_index, copy[i], link->scc_count);
    return true;
}

static void
build_sccs(core_link_unit_t *link)
{
    clear_sccs(link);
    if (!link->unit_count) return;
    core_translation_units_visit_sccs(link->units, link->unit_count,
                                      store_link_scc, link);
}

static void
capture_graph_snapshot(core_link_unit_t *link)
{
    link->import_edge_count = 0;
    for (size_t i = 0; i < link->unit_count; i++) {
        core_translation_unit_t *unit = link->units[i];
        for (size_t j = 0; j < core_translation_unit_import_count(unit); j++) {
            if (!core_list_reserve_raw((void **)&link->import_edges,
                                       &link->import_edge_capacity,
                                       link->import_edge_count + 1,
                                       sizeof(*link->import_edges)))
                abort();
            link->import_edges[link->import_edge_count++] =
                (core_link_import_edge_t){
                    .importer = unit,
                    .imported = core_translation_unit_import(unit, j),
                };
        }
        link->unit_import_revisions[i] =
            core_translation_unit_import_revision(unit);
    }
    build_sccs(link);
}

core_link_unit_t *
core_link_unit_new(void)
{
    core_link_unit_t *link = core_alloc(core_link_unit_t);
    *link = (core_link_unit_t){.diagnostics = core_diagnostics_new()};
    core_dict_init(&link->reference_index, reference_site_hash,
                   reference_site_eq);
    core_dict_init(&link->declaration_index, declaration_hash,
                   declaration_eq);
    core_dict_init(&link->root_index, nullptr, nullptr);
    core_dict_init_string(&link->root_module_index);
    core_dict_init(&link->unit_index, nullptr, nullptr);
    core_dict_init_string(&link->module_index);
    core_dict_init(&link->scc_index, nullptr, nullptr);
    core_dict_init(&link->artifact_index, artifact_key_hash, artifact_key_eq);
    core_dict_init(&link->contribution_index, contribution_hash,
                   contribution_eq);
    core_dict_init(&link->dependency_index, dependency_hash, dependency_eq);
    core_dict_init(&link->stage_index, nullptr, nullptr);
    return link;
}

void
core_link_unit_free(core_link_unit_t *link)
{
    if (!link) return;
    core_dict_free(&link->dependency_index);
    core_dict_free(&link->reference_index);
    core_dict_free(&link->declaration_index);
    core_dict_free(&link->root_index);
    core_dict_free(&link->root_module_index);
    core_dict_free(&link->unit_index);
    core_dict_free(&link->module_index);
    core_dict_free(&link->artifact_index);
    core_dict_free(&link->contribution_index);
    core_dict_free(&link->stage_index);
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *decl = link->declarations[i];
        if (decl->ownership == CORE_LINK_PAYLOAD_OWNED)
            decl->free_fn(decl->payload);
        core_free(decl->module.data);
        core_free(decl->key.data);
        core_free(decl->overload_group.data);
        for (size_t j = 0; j < decl->site_count; j++)
            core_free(decl->sites[j].provenance.data);
        core_free(decl->sites);
        for (size_t j = 0; j < decl->binding_count; j++)
            core_free(decl->bindings[j].role.data);
        core_free(decl->bindings);
        core_free(decl);
    }
    for (size_t i = 0; i < link->reference_count; i++) {
        core_link_reference_t *ref = link->references[i];
        core_free(ref->target_module.data);
        core_free(ref->declaration_key.data);
        core_free(ref->deferral_reason.data);
        core_free(ref->provenance.data);
        core_free(ref);
    }
    for (size_t i = 0; i < link->artifact_count; i++) {
        release_artifact(link->artifacts[i]);
    }
    for (size_t i = 0; i < link->stage_count; i++) {
        core_free(link->stages[i].prerequisites);
        core_free(link->stages[i].provenance.data);
    }
    for (size_t i = 0; i < link->contribution_count; i++) {
        core_link_contribution_t *contribution = link->contributions[i];
        clear_contribution_payload(contribution);
        core_free(contribution->module.data);
        core_free(contribution->domain.data);
        core_free(contribution->provenance.data);
        core_free(contribution);
    }
    for (size_t i = 0; i < link->dependency_count; i++) {
        core_free(link->dependencies[i]->provenance.data);
        core_free(link->dependencies[i]);
    }
    core_free(link->roots);
    core_free(link->units);
    core_free(link->unit_import_revisions);
    core_free(link->import_edges);
    clear_sccs(link);
    core_dict_free(&link->scc_index);
    core_free(link->sccs);
    core_free(link->declarations);
    core_free(link->references);
    core_free(link->artifacts);
    core_free(link->stages);
    core_free(link->contributions);
    core_free(link->dependencies);
    core_diagnostics_free(link->diagnostics);
    core_free(link);
}

core_diagnostics_t *
core_link_unit_diagnostics(core_link_unit_t *link)
{
    return link ? link->diagnostics : nullptr;
}

bool
core_link_unit_has_fatal_diagnostics(core_link_unit_t *link)
{
    if (!link || core_diagnostics_has_fatal(link->diagnostics)) return true;
    if (!core_link_unit_collect(link)) return true;
    for (size_t i = 0; i < link->unit_count; i++)
        if (core_translation_unit_has_fatal_diagnostics(link->units[i]))
            return true;
    return false;
}

bool
core_link_unit_add_root(core_link_unit_t *link, core_translation_unit_t *root)
{
    if (!link || !root) return false;
    if (core_dict_contains(&link->root_index, root)) return true;
    if (core_dict_contains(&link->root_module_index,
                           &root->module.identifier)) {
        link_report(link, root, "LANTERN-L0001",
            "link unit contains distinct translation units with the same module identity");
        return false;
    }
    if (link->graph_finalized) {
        link_report(link, root, "LANTERN-L0005",
            "cannot add a root after the link-unit graph is finalized");
        return false;
    }
    if (!core_list_reserve_raw((void **)&link->roots, &link->root_capacity,
                               link->root_count + 1, sizeof(*link->roots)))
        abort();
    link->roots[link->root_count++] = root;
    core_dict_add(&link->root_index, root, root);
    core_dict_add(&link->root_module_index,
                  &root->module.identifier, root);
    link->collected = false;
    link->validated = false;
    invalidate_stages(link);
    return true;
}

size_t
core_link_unit_root_count(const core_link_unit_t *link)
{
    return link ? link->root_count : 0;
}

core_translation_unit_t *
core_link_unit_root(const core_link_unit_t *link, size_t index)
{
    return link && index < link->root_count ? link->roots[index] : nullptr;
}

bool
core_link_unit_collect(core_link_unit_t *link)
{
    if (!link) return false;
    if (link->graph_finalized && link->collected)
        return link->collection_valid;
    if (graph_snapshot_current(link)) return link->collection_valid;
    if (link->graph_finalized) {
        link_report(link, nullptr, "LANTERN-L0006",
            "finalized link-unit graph changed unexpectedly");
        return false;
    }
    link->collected = false;
    link->unit_count = 0;
    link->import_edge_count = 0;
    clear_sccs(link);
    core_dict_free(&link->unit_index);
    core_dict_free(&link->module_index);
    core_dict_init(&link->unit_index, nullptr, nullptr);
    core_dict_init_string(&link->module_index);
    for (size_t i = 0; i < link->reference_count; i++)
        link->references[i]->target = nullptr;
    link->validated = false;
    link->validation_valid = false;
    invalidate_stages(link);
    link->collection_valid = true;
    if (!link->root_count) {
        link_report(link, nullptr, "LANTERN-L0002",
                    "link unit requires at least one root translation unit");
        link->collection_valid = false;
    }
    for (size_t i = 0; i < link->root_count; i++)
        collect_one(link, link->roots[i], &link->collection_valid);
    if (link->unit_count > 0) {
        size_t bytes = link->unit_count * sizeof(*link->unit_import_revisions);
        link->unit_import_revisions = core_realloc(
            link->unit_import_revisions, bytes);
        capture_graph_snapshot(link);
    }
    link->graph_generation++;
    link->collected = true;
    return link->collection_valid;
}

bool
core_link_unit_finalize_graph(core_link_unit_t *link)
{
    if (!link || !core_link_unit_collect(link)
        || core_link_unit_has_fatal_diagnostics(link)) return false;
    if (link->graph_finalized) return true;
    for (size_t i = 0; i < link->unit_count; i++)
        if (!core_translation_unit_finalize_imports(link->units[i]))
            return false;
    link->graph_finalized = true;
    return true;
}

bool
core_link_unit_graph_is_finalized(const core_link_unit_t *link)
{
    return link && link->graph_finalized;
}

bool
core_link_unit_register_stage(core_link_unit_t *link, uint32_t stage_kind,
                              const uint32_t *prerequisites,
                              size_t prerequisite_count,
                              core_string_t provenance,
                              bool requires_finalized_graph,
                              core_link_stage_fn callback, void *context)
{
    if (!link || !callback || (!prerequisites && prerequisite_count)
        || link->stages_started || find_stage(link, stage_kind))
        return false;
    for (size_t i = 0; i < prerequisite_count; i++) {
        if (prerequisites[i] == stage_kind) return false;
        for (size_t j = 0; j < i; j++)
            if (prerequisites[j] == prerequisites[i]) return false;
    }
    uint32_t *prereq_copy = prerequisite_count
        ? core_alloc_array(uint32_t, prerequisite_count) : nullptr;
    if (prerequisite_count)
        memcpy(prereq_copy, prerequisites,
               prerequisite_count * sizeof(*prereq_copy));
    if (!core_list_reserve_raw((void **)&link->stages, &link->stage_capacity,
                               link->stage_count + 1, sizeof(*link->stages)))
        abort();
    link->stages[link->stage_count++] = (core_link_stage_record_t){
        .kind = stage_kind,
        .prerequisites = prereq_copy,
        .prerequisite_count = prerequisite_count,
        .provenance = provenance.data ? core_string_dup(provenance)
                                      : (core_string_t){0},
        .requires_finalized_graph = requires_finalized_graph,
        .callback = callback,
        .context = context,
        .state = CORE_LINK_STAGE_PENDING,
    };
    core_dict_add(&link->stage_index,
        (void *)(uintptr_t)stage_kind, link->stage_count);
    return true;
}

static bool
run_stage_at(core_link_unit_t *link, size_t index)
{
    core_link_stage_record_t *stage = &link->stages[index];
    if (stage->state == CORE_LINK_STAGE_COMPLETE) return true;
    if (stage->state == CORE_LINK_STAGE_FAILED
        || stage->state == CORE_LINK_STAGE_UNAVAILABLE) return false;
    if (stage->state == CORE_LINK_STAGE_RUNNING) {
        stage->state = CORE_LINK_STAGE_FAILED;
        link_report(link, nullptr, "LANTERN-L0007",
                    "link-unit analysis stages contain a prerequisite cycle");
        return false;
    }
    stage->state = CORE_LINK_STAGE_RUNNING;
    link->stages_started = true;
    link->validated = false;

    for (size_t i = 0; i < stage->prerequisite_count; i++) {
        core_link_stage_record_t *prereq =
            find_stage(link, stage->prerequisites[i]);
        if (!prereq) {
            stage->state = CORE_LINK_STAGE_FAILED;
            link_report(link, nullptr, "LANTERN-L0008",
                        "link-unit analysis stage has an unregistered prerequisite");
            return false;
        }
        size_t prereq_index = (size_t)(prereq - link->stages);
        if (!run_stage_at(link, prereq_index)) {
            stage->state = prereq->state == CORE_LINK_STAGE_UNAVAILABLE
                ? CORE_LINK_STAGE_UNAVAILABLE : CORE_LINK_STAGE_FAILED;
            if (stage->state == CORE_LINK_STAGE_FAILED)
                link_report(link, nullptr, "LANTERN-L0009",
                            "link-unit analysis stage prerequisite failed");
            return false;
        }
    }

    if (stage->requires_finalized_graph && !link->graph_finalized) {
        stage->state = CORE_LINK_STAGE_FAILED;
        link_report(link, nullptr, "LANTERN-L0010",
                    "link-unit analysis stage requires a finalized import graph");
        return false;
    }
    if (core_link_unit_has_fatal_diagnostics(link)) {
        stage->state = CORE_LINK_STAGE_FAILED;
        return false;
    }

    uint32_t kind = stage->kind;
    core_link_stage_fn callback = stage->callback;
    void *context = stage->context;
    uint64_t graph_generation = link->graph_generation;
    core_link_stage_result_t result = callback(link, kind, context);
    stage = &link->stages[index];
    core_link_unit_collect(link);
    if (link->graph_generation != graph_generation) {
        stage->state = CORE_LINK_STAGE_FAILED;
        finish_stage_contributions(link, kind,
                                   CORE_LINK_STAGE_RESULT_FAILED);
        remove_stage_artifacts(link, nullptr, kind);
        finish_stage_dependencies(link, kind);
        link_report(link, nullptr, "LANTERN-L0013",
                    "link-unit graph changed while an analysis stage was running");
        return false;
    }
    if (core_link_unit_has_fatal_diagnostics(link)) {
        stage->state = CORE_LINK_STAGE_FAILED;
        finish_stage_contributions(link, kind,
                                   CORE_LINK_STAGE_RESULT_FAILED);
        remove_stage_artifacts(link, nullptr, kind);
        finish_stage_dependencies(link, kind);
        return false;
    }
    switch (result) {
    case CORE_LINK_STAGE_RESULT_COMPLETE:
        stage->state = CORE_LINK_STAGE_COMPLETE;
        return true;
    case CORE_LINK_STAGE_RESULT_UNAVAILABLE:
        stage->state = CORE_LINK_STAGE_UNAVAILABLE;
        finish_stage_contributions(link, kind, result);
        remove_stage_artifacts(link, nullptr, kind);
        finish_stage_dependencies(link, kind);
        return false;
    case CORE_LINK_STAGE_RESULT_FAILED:
    default:
        stage->state = CORE_LINK_STAGE_FAILED;
        finish_stage_contributions(link, kind,
                                   CORE_LINK_STAGE_RESULT_FAILED);
        remove_stage_artifacts(link, nullptr, kind);
        finish_stage_dependencies(link, kind);
        link_report(link, nullptr, "LANTERN-L0011",
                    "link-unit analysis stage failed without a fatal diagnostic");
        return false;
    }
}

bool
core_link_unit_run_stage(core_link_unit_t *link, uint32_t stage_kind)
{
    if (!link || !core_link_unit_collect(link)) return false;
    core_link_stage_record_t *stage = find_stage(link, stage_kind);
    if (!stage) {
        link_report(link, nullptr, "LANTERN-L0012",
                    "requested link-unit analysis stage is not registered");
        return false;
    }
    return run_stage_at(link, (size_t)(stage - link->stages));
}

bool
core_link_unit_invalidate_stage(core_link_unit_t *link, uint32_t stage_kind)
{
    if (!link || (link->root_count && !core_link_unit_collect(link)))
        return false;
    core_link_stage_record_t *target = find_stage(link, stage_kind);
    if (!target) return false;
    bool *invalidated = core_alloc_array(bool, link->stage_count);
    memset(invalidated, 0, link->stage_count * sizeof(*invalidated));
    invalidated[target - link->stages] = true;
    for (size_t i = 0; i < link->stage_count; i++)
        if (link->stages[i].state == CORE_LINK_STAGE_RUNNING) {
            core_free(invalidated);
            return false;
        }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < link->stage_count; i++) {
            if (invalidated[i]) continue;
            core_link_stage_record_t *stage = &link->stages[i];
            for (size_t j = 0; j < stage->prerequisite_count; j++) {
                core_link_stage_record_t *prereq =
                    find_stage(link, stage->prerequisites[j]);
                if (prereq && invalidated[prereq - link->stages]) {
                    invalidated[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
    for (size_t i = 0; i < link->stage_count; i++)
        if (invalidated[i]) link->stages[i].state = CORE_LINK_STAGE_PENDING;
    reset_stage_contributions(link, invalidated);
    reset_stage_bindings(link, invalidated);
    remove_stage_artifacts(link, invalidated, 0);
    remove_stage_dependencies(link, invalidated);
    core_free(invalidated);
    link->validated = false;
    return true;
}

core_link_stage_state_t
core_link_unit_stage_state(core_link_unit_t *link, uint32_t stage_kind)
{
    core_link_stage_record_t *stage = link
        && (!link->root_count || core_link_unit_collect(link))
        ? find_stage(link, stage_kind) : nullptr;
    return stage ? stage->state : CORE_LINK_STAGE_FAILED;
}

size_t
core_link_unit_stage_count(const core_link_unit_t *link)
{
    return link ? link->stage_count : 0;
}

uint32_t
core_link_unit_stage_kind(const core_link_unit_t *link, size_t index)
{
    return link && index < link->stage_count ? link->stages[index].kind : 0;
}

core_string_t
core_link_unit_stage_provenance(const core_link_unit_t *link,
                                uint32_t stage_kind)
{
    core_link_stage_record_t *stage = link ? find_stage(link, stage_kind)
                                           : nullptr;
    return stage ? stage->provenance : (core_string_t){0};
}

size_t
core_link_unit_translation_unit_count(core_link_unit_t *link)
{
    return link && core_link_unit_collect(link) ? link->unit_count : 0;
}

core_translation_unit_t *
core_link_unit_translation_unit(core_link_unit_t *link, size_t index)
{
    return link && core_link_unit_collect(link) && index < link->unit_count
        ? link->units[index] : nullptr;
}

bool
core_link_unit_visit_sccs(core_link_unit_t *link,
                          core_translation_unit_scc_fn callback,
                          void *context)
{
    if (!link || !callback || !core_link_unit_collect(link)
        || core_link_unit_has_fatal_diagnostics(link)) return false;
    for (size_t i = 0; i < link->scc_count; i++)
        if (!callback(context, link->sccs[i].members,
                      link->sccs[i].member_count)) return false;
    return true;
}

size_t
core_link_unit_import_edge_count(core_link_unit_t *link)
{
    return link && core_link_unit_collect(link)
        ? link->import_edge_count : 0;
}

bool
core_link_unit_import_edge(core_link_unit_t *link, size_t index,
                           core_translation_unit_t **importer,
                           core_translation_unit_t **imported)
{
    if (!link || !core_link_unit_collect(link)
        || index >= link->import_edge_count) return false;
    if (importer) *importer = link->import_edges[index].importer;
    if (imported) *imported = link->import_edges[index].imported;
    return true;
}

size_t
core_link_unit_scc_count(core_link_unit_t *link)
{
    return link && core_link_unit_collect(link) ? link->scc_count : 0;
}

size_t
core_link_unit_scc_member_count(core_link_unit_t *link, size_t scc_index)
{
    return link && core_link_unit_collect(link) && scc_index < link->scc_count
        ? link->sccs[scc_index].member_count : 0;
}

core_translation_unit_t *
core_link_unit_scc_member(core_link_unit_t *link, size_t scc_index,
                          size_t member_index)
{
    if (!link || !core_link_unit_collect(link) || scc_index >= link->scc_count
        || member_index >= link->sccs[scc_index].member_count) return nullptr;
    return link->sccs[scc_index].members[member_index];
}

bool
core_link_unit_scc_of(core_link_unit_t *link,
                      core_translation_unit_t *unit, size_t *scc_index)
{
    if (!link || !unit || !core_link_unit_collect(link)) return false;
    size_t index = (size_t)(uintptr_t)core_dict_get(&link->scc_index, unit,
                                                    nullptr);
    if (!index) return false;
    if (scc_index) *scc_index = index - 1;
    return true;
}

core_link_declaration_t *
core_link_unit_find_declaration(const core_link_unit_t *link,
                                core_string_t module, core_string_t key)
{
    if (!link) return nullptr;
    core_link_declaration_t probe = {.module = module, .key = key};
    return core_dict_get((core_dict_t *)&link->declaration_index,
                         &probe, nullptr);
}

static void
release_payload(void *payload, core_link_payload_ownership_t ownership,
                core_link_payload_free_fn free_fn)
{
    if (ownership == CORE_LINK_PAYLOAD_OWNED) free_fn(payload);
}

static core_link_origin_kind_t
declaration_origin(bool external, bool external_requirement)
{
    if (external_requirement) return CORE_LINK_ORIGIN_EXTERNAL_REQUIREMENT;
    return external ? CORE_LINK_ORIGIN_LIBRARY : CORE_LINK_ORIGIN_SOURCE;
}

static void
append_declaration_site(core_link_declaration_t *decl,
                        core_translation_unit_t *owner,
                        const core_link_declaration_metadata_t *metadata,
                        bool external, bool is_definition,
                        bool external_requirement)
{
    core_link_origin_kind_t origin = declaration_origin(
        external, external_requirement);
    for (size_t i = 0; i < decl->site_count; i++) {
        core_link_decl_site_t *site = &decl->sites[i];
        if (site->unit == owner && site->source_site == metadata->source_site
            && site->origin == origin
            && site->is_definition == is_definition
            && (metadata->source_site
                || (site->provenance_version == metadata->provenance_version
                    && core_string_eq(site->provenance,
                                      metadata->provenance))))
            return;
    }
    if (!core_list_reserve_raw((void **)&decl->sites, &decl->site_capacity,
                               decl->site_count + 1, sizeof(*decl->sites)))
        abort();
    decl->sites[decl->site_count++] = (core_link_decl_site_t){
        .unit = owner,
        .source_site = metadata->source_site,
        .location = metadata->source_location,
        .provenance = metadata->provenance.data
            ? core_string_dup(metadata->provenance) : (core_string_t){0},
        .provenance_version = metadata->provenance_version,
        .origin = origin,
        .is_definition = is_definition,
    };
}

static bool
register_declaration(core_link_unit_t *link, core_string_t module,
                     core_translation_unit_t *owner, bool external,
                     core_string_t key,
                     const core_link_declaration_metadata_t *metadata,
                     bool is_definition, bool external_requirement,
                     void *payload,
                     core_link_payload_ownership_t ownership,
                     core_link_payload_free_fn free_fn,
                     core_link_declaration_t **out)
{
    if (out) *out = nullptr;
    if (!link || !module.data || !module.u8_bytes || !key.data || !key.u8_bytes
        || !metadata || !payload_args_valid(ownership, free_fn)) return false;

    core_link_declaration_t *decl = core_link_unit_find_declaration(
        link, module, key);
    if (!decl) {
        decl = core_alloc(core_link_declaration_t);
        *decl = (core_link_declaration_t){
            .module = core_string_dup(module),
            .key = core_string_dup(key),
            .owner = owner,
            .kind = metadata->kind,
            .visibility = metadata->visibility,
            .overload_group = metadata->overload_group.data
                ? core_string_dup(metadata->overload_group)
                : (core_string_t){0},
            .contract = metadata->contract,
            .compatible = metadata->compatible,
            .compatibility_context = metadata->compatibility_context,
            .payload = payload,
            .ownership = ownership,
            .free_fn = free_fn,
            .external = external,
            .is_defined = is_definition,
            .external_requirement = external_requirement,
        };
        if (!core_list_reserve_raw((void **)&link->declarations,
                                   &link->declaration_capacity,
                                   link->declaration_count + 1,
                                   sizeof(*link->declarations)))
            abort();
        link->declarations[link->declaration_count++] = decl;
        if (!core_dict_add(&link->declaration_index, decl, decl)) abort();
        append_declaration_site(decl, owner, metadata, external, is_definition,
                                external_requirement);
        link->validated = false;
        if (out) *out = decl;
        return true;
    }

    core_link_contract_compatible_fn compare = decl->compatible
        ? decl->compatible : metadata->compatible;
    void *compare_context = decl->compatible
        ? decl->compatibility_context : metadata->compatibility_context;
    const char *mismatch = decl->kind != metadata->kind ? "kind"
        : decl->visibility != metadata->visibility ? "visibility"
        : !core_string_eq(decl->overload_group, metadata->overload_group)
            ? "overload group"
        : !(decl->external == external || decl->external_requirement
            || external_requirement) ? "linkage"
        : !(compare
            ? compare(decl->contract, metadata->contract, compare_context)
            : decl->contract == metadata->contract) ? "contract"
        : nullptr;
    if (mismatch) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "two declarations of '%.*s' in module '%.*s' disagree on %s",
            (int)key.u8_bytes, key.data, (int)module.u8_bytes, module.data,
            mismatch);
        link_report_at(link, owner, metadata->source_location,
            "LANTERN-L0014", message.data);
        core_buffer_release(&message);
        return false;
    }
    if (is_definition && decl->is_defined) {
        link_report_at(link, owner, metadata->source_location,
            "LANTERN-L0003",
            "link unit has duplicate definitions for one module declaration identity");
        return false;
    }

    bool same_payload = decl->payload == payload;
    if (same_payload && decl->ownership == CORE_LINK_PAYLOAD_OWNED
        && ownership == CORE_LINK_PAYLOAD_OWNED
        && decl->free_fn != free_fn) {
        link_report_at(link, owner, metadata->source_location,
            "LANTERN-L0015",
            "one declaration payload cannot have conflicting release handlers");
        return false;
    }

    if (is_definition) {
        if (!same_payload)
            release_payload(decl->payload, decl->ownership, decl->free_fn);
        else if (decl->ownership == CORE_LINK_PAYLOAD_OWNED) {
            ownership = decl->ownership;
            free_fn = decl->free_fn;
        }
        else if (ownership == CORE_LINK_PAYLOAD_BORROWED) {
            free_fn = nullptr;
        }
        decl->payload = payload;
        decl->ownership = ownership;
        decl->free_fn = free_fn;
        decl->owner = owner;
        decl->external = external;
        decl->is_defined = true;
        decl->external_requirement = false;
    }
    else if (!same_payload) {
        release_payload(payload, ownership, free_fn);
    }
    else if (decl->ownership == CORE_LINK_PAYLOAD_BORROWED
             && ownership == CORE_LINK_PAYLOAD_OWNED) {
        decl->ownership = ownership;
        decl->free_fn = free_fn;
    }
    if (external_requirement && !decl->is_defined)
        decl->external_requirement = true;
    if (!decl->compatible && metadata->compatible) {
        decl->compatible = metadata->compatible;
        decl->compatibility_context = metadata->compatibility_context;
    }
    append_declaration_site(decl, owner, metadata, external, is_definition,
                            external_requirement);
    link->validated = false;
    if (out) *out = decl;
    return true;
}

bool
core_link_unit_define(core_link_unit_t *link,
                      core_translation_unit_t *owner,
                      core_string_t declaration_key, void *payload,
                      core_link_payload_ownership_t ownership,
                      core_link_payload_free_fn free_fn,
                      core_link_declaration_t **out)
{
    if (!link || !owner || !core_link_unit_collect(link)) return false;
    if (!core_dict_contains(&link->unit_index, owner)) {
        link_report(link, owner, "LANTERN-L0004",
                    "declaration owner is not part of this link unit");
        return false;
    }
    core_link_declaration_metadata_t metadata = {
        .contract = payload,
    };
    return core_link_unit_register_declaration(link, owner, declaration_key,
        &metadata, true, payload, ownership, free_fn, out);
}

bool
core_link_unit_register_declaration(
    core_link_unit_t *link, core_translation_unit_t *owner,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata, bool is_definition,
    void *payload, core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn, core_link_declaration_t **out)
{
    if (!link || !owner || !core_link_unit_collect(link)) return false;
    if (!core_dict_contains(&link->unit_index, owner)) {
        link_report(link, owner, "LANTERN-L0004",
                    "declaration owner is not part of this link unit");
        return false;
    }
    return register_declaration(link, owner->module.identifier, owner, false,
        declaration_key, metadata, is_definition, false, payload, ownership,
        free_fn, out);
}

bool
core_link_unit_define_external(core_link_unit_t *link, core_string_t module,
                               core_string_t declaration_key, void *payload,
                               core_link_payload_ownership_t ownership,
                               core_link_payload_free_fn free_fn,
                               core_link_declaration_t **out)
{
    core_link_declaration_metadata_t metadata = {.contract = payload};
    return core_link_unit_define_external_typed(link, module, declaration_key,
        &metadata, payload, ownership, free_fn, out);
}

bool
core_link_unit_define_external_typed(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn, core_link_declaration_t **out)
{
    return register_declaration(link, module, nullptr, true, declaration_key,
        metadata, true, false, payload, ownership, free_fn, out);
}

bool
core_link_unit_redeclare_external_typed(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata,
    core_link_declaration_t **out)
{
    if (!core_link_unit_find_declaration(link, module, declaration_key))
        return false;
    return register_declaration(link, module, nullptr, true, declaration_key,
        metadata, false, false, nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr,
        out);
}

bool
core_link_unit_require_external(
    core_link_unit_t *link, core_string_t module,
    core_string_t declaration_key,
    const core_link_declaration_metadata_t *metadata,
    core_link_declaration_t **out)
{
    return register_declaration(link, module, nullptr, false, declaration_key,
        metadata, false, true, nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr,
        out);
}

size_t
core_link_unit_declaration_count(const core_link_unit_t *link)
{
    return link ? link->declaration_count : 0;
}

core_link_declaration_t *
core_link_unit_declaration_at(const core_link_unit_t *link, size_t index)
{
    return link && index < link->declaration_count
        ? link->declarations[index] : nullptr;
}

size_t
core_link_unit_overload_member_count(core_link_unit_t *link,
                                     core_string_t module,
                                     core_string_t overload_group)
{
    if (!link || !module.data || !overload_group.data
        || !core_link_unit_collect(link)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *decl = link->declarations[i];
        if (core_string_eq(decl->module, module)
            && core_string_eq(decl->overload_group, overload_group)) count++;
    }
    return count;
}

core_link_declaration_t *
core_link_unit_overload_member(core_link_unit_t *link, core_string_t module,
                               core_string_t overload_group, size_t index)
{
    if (!link || !module.data || !overload_group.data
        || !core_link_unit_collect(link)) return nullptr;
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *decl = link->declarations[i];
        if (!core_string_eq(decl->module, module)
            || !core_string_eq(decl->overload_group, overload_group)) continue;
        if (!index--) return decl;
    }
    return nullptr;
}

static bool reference_at_internal(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope,
    core_string_t target_module,
    core_string_t declaration_key, bool has_expected_kind,
    uint32_t expected_kind, bool allow_unresolved_target, bool add_distinct,
    core_link_reference_t **out);

bool
core_link_unit_reference(core_link_unit_t *link,
                         core_translation_unit_t *referrer,
                         core_string_t target_module,
                         core_string_t declaration_key,
                         core_link_reference_t **out)
{
    return core_link_unit_reference_at(link, referrer, nullptr,
        target_module, declaration_key, out);
}

bool
core_link_unit_reference_at(core_link_unit_t *link,
                            core_translation_unit_t *referrer,
                            const void *use_site,
                            core_string_t target_module,
                            core_string_t declaration_key,
                            core_link_reference_t **out)
{
    return reference_at_internal(link, referrer, use_site, nullptr,
                                 target_module, declaration_key, false, 0,
                                 false, false,
                                 out);
}

bool
core_link_unit_reference_at_kind(core_link_unit_t *link,
                                 core_translation_unit_t *referrer,
                                 const void *use_site,
                                 core_string_t target_module,
                                 core_string_t declaration_key,
                                 uint32_t expected_kind,
                                 core_link_reference_t **out)
{
    return reference_at_internal(link, referrer, use_site, nullptr,
                                 target_module, declaration_key, true,
                                 expected_kind, false, false, out);
}

bool
core_link_unit_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope,
    core_string_t target_module, core_string_t declaration_key,
    uint32_t expected_kind, core_link_reference_t **out)
{
    return reference_at_internal(link, referrer, use_site, source_scope,
                                 target_module, declaration_key, true,
                                 expected_kind, false, false, out);
}

static bool
defer_reference_at_scoped_kind_internal(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope, uint32_t expected_kind,
    core_string_t reason, uint32_t prerequisite_stage,
    bool add_distinct, core_link_reference_t **out)
{
    if (out) *out = nullptr;
    if (!use_site || !reason.data || !reason.u8_bytes) return false;
    core_link_reference_t *reference = nullptr;
    if (!reference_at_internal(link, referrer, use_site, source_scope,
            (core_string_t){0}, (core_string_t){0}, true, expected_kind,
            true, add_distinct, &reference)
        || !core_link_unit_defer_reference(link, reference, reason,
                                           prerequisite_stage))
        return false;
    if (out) *out = reference;
    return true;
}

bool
core_link_unit_defer_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope, uint32_t expected_kind,
    core_string_t reason, uint32_t prerequisite_stage,
    core_link_reference_t **out)
{
    return defer_reference_at_scoped_kind_internal(
        link, referrer, use_site, source_scope, expected_kind, reason,
        prerequisite_stage, false, out);
}

bool
core_link_unit_add_deferred_reference_at_scoped_kind(
    core_link_unit_t *link, core_translation_unit_t *referrer,
    const void *use_site, const void *source_scope, uint32_t expected_kind,
    core_string_t reason, uint32_t prerequisite_stage,
    core_link_reference_t **out)
{
    return defer_reference_at_scoped_kind_internal(
        link, referrer, use_site, source_scope, expected_kind, reason,
        prerequisite_stage, true, out);
}

static bool
reference_at_internal(core_link_unit_t *link,
                      core_translation_unit_t *referrer,
                      const void *use_site, const void *source_scope,
                      core_string_t target_module,
                      core_string_t declaration_key, bool has_expected_kind,
                      uint32_t expected_kind, bool allow_unresolved_target,
                      bool add_distinct,
                      core_link_reference_t **out)
{
    if (out) *out = nullptr;
    if (!link || !referrer
        || (!allow_unresolved_target
            && (!target_module.data || !target_module.u8_bytes
                || !declaration_key.data || !declaration_key.u8_bytes)))
        return false;
    core_link_reference_t site_key = {
        .referrer = referrer, .use_site = use_site,
    };
    bool found = false;
    core_link_reference_t *first = core_dict_get(&link->reference_index,
                                                 &site_key, &found);
    for (core_link_reference_t *prior = add_distinct || !found
             ? nullptr : first; prior; prior = prior->next_at_site) {
        if (core_string_eq(prior->target_module, target_module)
            && core_string_eq(prior->declaration_key, declaration_key)) {
            if (prior->source_scope != source_scope
                || prior->has_expected_kind != has_expected_kind
                || (has_expected_kind
                    && prior->expected_kind != expected_kind)) {
                link_report(link, referrer, "LANTERN-L0016",
                    "one source reference has conflicting expected declaration kinds");
                return false;
            }
            if (out) *out = prior;
            return true;
        }
    }
    core_link_reference_t *ref = core_alloc(core_link_reference_t);
    *ref = (core_link_reference_t){
        .owner = link,
        .referrer = referrer,
        .use_site = use_site,
        .source_scope = source_scope,
        .target_module = target_module.data
            ? core_string_dup(target_module) : (core_string_t){0},
        .declaration_key = declaration_key.data
            ? core_string_dup(declaration_key) : (core_string_t){0},
        .has_expected_kind = has_expected_kind,
        .expected_kind = expected_kind,
    };
    if (!core_list_reserve_raw((void **)&link->references,
                               &link->reference_capacity,
                               link->reference_count + 1,
                               sizeof(*link->references)))
        abort();
    link->references[link->reference_count++] = ref;
    if (found) {
        first->last_at_site->next_at_site = ref;
        first->last_at_site = ref;
    } else {
        ref->last_at_site = ref;
        core_dict_add(&link->reference_index, ref, ref);
    }
    link->validated = false;
    if (out) *out = ref;
    return true;
}

bool
core_link_unit_defer_reference(core_link_unit_t *link,
                               core_link_reference_t *reference,
                               core_string_t reason,
                               uint32_t prerequisite_stage)
{
    if (!link || !reference || !reason.data || !reason.u8_bytes) return false;
    if (reference->owner != link) return false;
    core_string_t reason_copy = core_string_dup(reason);
    core_free(reference->deferral_reason.data);
    reference->deferral_reason = reason_copy;
    reference->deferral_prerequisite = prerequisite_stage;
    reference->deferred = true;
    reference->target = nullptr;
    link->validated = false;
    return true;
}

bool
core_link_unit_resolve_reference(core_link_unit_t *link,
                                 core_link_reference_t *reference,
                                 core_string_t target_module,
                                 core_string_t declaration_key)
{
    if (!link || !reference || !target_module.data || !target_module.u8_bytes
        || !declaration_key.data || !declaration_key.u8_bytes) return false;
    if (reference->owner != link) return false;
    core_string_t module_copy = core_string_dup(target_module);
    core_string_t key_copy = core_string_dup(declaration_key);
    core_free(reference->target_module.data);
    core_free(reference->declaration_key.data);
    core_free(reference->deferral_reason.data);
    reference->target_module = module_copy;
    reference->declaration_key = key_copy;
    reference->deferral_reason = (core_string_t){0};
    reference->deferral_prerequisite = 0;
    reference->deferred = false;
    core_link_declaration_t *target = core_link_unit_find_declaration(
        link, reference->target_module, reference->declaration_key);
    reference->target = target && (!reference->has_expected_kind
        || reference->expected_kind == target->kind) ? target : nullptr;
    link->validated = false;
    return true;
}

size_t
core_link_unit_reference_count(const core_link_unit_t *link)
{
    return link ? link->reference_count : 0;
}

core_link_reference_t *
core_link_unit_reference_at_index(const core_link_unit_t *link, size_t index)
{
    return link && index < link->reference_count
        ? link->references[index] : nullptr;
}

bool
core_link_unit_validate(core_link_unit_t *link)
{
    if (!link || !core_link_unit_collect(link)) return false;
    if (link->validated)
        return link->validation_valid
            && !core_link_unit_has_fatal_diagnostics(link);
    link->validation_valid = !core_link_unit_has_fatal_diagnostics(link);
    for (size_t i = 0; i < link->declaration_count; i++) {
        core_link_declaration_t *decl = link->declarations[i];
        if (decl->owner
            && !core_dict_contains(&link->unit_index, decl->owner)) {
            link_report(link, decl->owner, "LANTERN-L0004",
                        "declaration owner is not part of this link unit");
            link->validation_valid = false;
        }
        if (!decl->is_defined && !decl->external_requirement) {
            core_diag_location_t location = decl->site_count
                ? decl->sites[decl->site_count - 1].location
                : (core_diag_location_t){0};
            link_report_at(link, decl->owner, location, "LANTERN-L0018",
                "link unit contains a declaration without a definition or external requirement");
            link->validation_valid = false;
        }
    }
    for (size_t i = 0; i < link->reference_count; i++) {
        core_link_reference_t *ref = link->references[i];
        if (!core_dict_contains(&link->unit_index, ref->referrer)) {
            link_report(link, ref->referrer, "LANTERN-L0004",
                        "reference source is not part of this link unit");
            link->validation_valid = false;
        }
        if (ref->deferred) {
            core_link_stage_record_t *prerequisite =
                find_stage(link, ref->deferral_prerequisite);
            if (!prerequisite) {
                link_report_at(link, ref->referrer, ref->source_location,
                    "LANTERN-L0019",
                    "deferred reference names an unregistered prerequisite stage");
                link->validation_valid = false;
            }
            else if (prerequisite->state == CORE_LINK_STAGE_COMPLETE) {
                link_report_at(link, ref->referrer, ref->source_location,
                    "LANTERN-L0020",
                    "reference remains deferred after its prerequisite completed");
                link->validation_valid = false;
            }
            ref->target = nullptr;
            continue;
        }
        ref->target = core_link_unit_find_declaration(link, ref->target_module,
                                       ref->declaration_key);
        if (!ref->target) {
            link_report_at(link, ref->referrer, ref->source_location,
                "LANTERN-L0005",
                        "link unit contains an unresolved declaration reference");
            link->validation_valid = false;
        }
        else if (ref->has_expected_kind
                 && ref->expected_kind != ref->target->kind) {
            link_report_at(link, ref->referrer, ref->source_location,
                "LANTERN-L0017",
                        "reference resolves to a declaration of the wrong kind");
            link->validation_valid = false;
        }
    }
    link->validated = true;
    link->validation_valid &= !core_link_unit_has_fatal_diagnostics(link);
    return link->validation_valid;
}

core_translation_unit_t *
core_link_declaration_owner(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->owner : nullptr;
}

core_string_t
core_link_declaration_module(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->module : (core_string_t){0};
}

core_string_t
core_link_declaration_key(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->key : (core_string_t){0};
}

const void *
core_link_declaration_contract(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->contract : nullptr;
}

void *
core_link_declaration_payload(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->payload : nullptr;
}

bool
core_link_declaration_set_binding(
    core_link_unit_t *link, core_link_declaration_t *declaration,
    core_string_t role, uint32_t producer_stage, void *handle)
{
    if (!link || !declaration || !role.data || !role.u8_bytes || !handle)
        return false;
    bool owned = false;
    for (size_t i = 0; i < link->declaration_count; i++)
        owned |= link->declarations[i] == declaration;
    if (!owned) return false;
    if (producer_stage) {
        core_link_stage_record_t *producer = find_stage(link, producer_stage);
        if (!producer || producer->state != CORE_LINK_STAGE_RUNNING)
            return false;
    }
    for (size_t i = 0; i < declaration->binding_count; i++) {
        core_link_decl_binding_t *binding = &declaration->bindings[i];
        if (!core_string_eq(binding->role, role)) continue;
        if (binding->handle == handle
            && binding->producer_stage == producer_stage) return true;
        link_report(link, declaration->owner, "LANTERN-L0022",
            "canonical declaration binding conflicts with an existing handle");
        return false;
    }
    if (!core_list_reserve_raw((void **)&declaration->bindings,
                               &declaration->binding_capacity,
                               declaration->binding_count + 1,
                               sizeof(*declaration->bindings)))
        abort();
    declaration->bindings[declaration->binding_count++] =
        (core_link_decl_binding_t){
            .role = core_string_dup(role),
            .producer_stage = producer_stage,
            .handle = handle,
        };
    link->validated = false;
    return true;
}

void *
core_link_declaration_binding(const core_link_declaration_t *declaration,
                              core_string_t role)
{
    if (!declaration || !role.data || !role.u8_bytes) return nullptr;
    for (size_t i = 0; i < declaration->binding_count; i++)
        if (core_string_eq(declaration->bindings[i].role, role))
            return declaration->bindings[i].handle;
    return nullptr;
}

bool
core_link_declaration_is_external(const core_link_declaration_t *declaration)
{
    return declaration && declaration->external;
}

uint32_t
core_link_declaration_kind(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->kind : 0;
}

uint32_t
core_link_declaration_visibility(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->visibility : 0;
}

core_string_t
core_link_declaration_overload_group(
    const core_link_declaration_t *declaration)
{
    return declaration ? declaration->overload_group : (core_string_t){0};
}

bool
core_link_declaration_is_defined(const core_link_declaration_t *declaration)
{
    return declaration && declaration->is_defined;
}

bool
core_link_declaration_is_external_requirement(
    const core_link_declaration_t *declaration)
{
    return declaration && declaration->external_requirement;
}

size_t
core_link_declaration_site_count(const core_link_declaration_t *declaration)
{
    return declaration ? declaration->site_count : 0;
}

bool
core_link_declaration_site(const core_link_declaration_t *declaration,
                           size_t index,
                           core_link_declaration_site_view_t *out)
{
    if (!declaration || !out || index >= declaration->site_count)
        return false;
    core_link_decl_site_t *site = &declaration->sites[index];
    *out = (core_link_declaration_site_view_t){
        .unit = site->unit,
        .source_site = site->source_site,
        .location = site->location,
        .provenance = site->provenance,
        .provenance_version = site->provenance_version,
        .origin = site->origin,
        .is_definition = site->is_definition,
    };
    return true;
}

core_link_declaration_t *
core_link_reference_target(const core_link_reference_t *reference)
{
    return reference ? reference->target : nullptr;
}

core_translation_unit_t *
core_link_reference_referrer(const core_link_reference_t *reference)
{
    return reference ? reference->referrer : nullptr;
}

const void *
core_link_reference_use_site(const core_link_reference_t *reference)
{
    return reference ? reference->use_site : nullptr;
}

const void *
core_link_reference_source_scope(const core_link_reference_t *reference)
{
    return reference ? reference->source_scope : nullptr;
}

core_string_t
core_link_reference_module(const core_link_reference_t *reference)
{
    return reference ? reference->target_module : (core_string_t){0};
}

core_string_t
core_link_reference_key(const core_link_reference_t *reference)
{
    return reference ? reference->declaration_key : (core_string_t){0};
}

bool
core_link_reference_expected_kind(const core_link_reference_t *reference,
                                  uint32_t *out)
{
    if (!reference || !reference->has_expected_kind) return false;
    if (out) *out = reference->expected_kind;
    return true;
}

bool
core_link_reference_set_source_info(core_link_unit_t *link,
                                    core_link_reference_t *reference,
                                    core_diag_location_t location,
                                    core_string_t provenance)
{
    if (!link || !reference) return false;
    if (reference->owner != link) return false;
    if (reference->source_location.has_location
        || reference->provenance.data) {
        bool same_location = reference->source_location.line == location.line
            && reference->source_location.column == location.column
            && reference->source_location.end_line == location.end_line
            && reference->source_location.end_column == location.end_column
            && reference->source_location.has_location
                   == location.has_location;
        if (!same_location
            || !core_string_eq(reference->provenance, provenance)) {
            link_report_at(link, reference->referrer, location,
                "LANTERN-L0021",
                "one source reference has conflicting provenance metadata");
            return false;
        }
        return true;
    }
    reference->source_location = location;
    reference->provenance = provenance.data ? core_string_dup(provenance)
                                            : (core_string_t){0};
    return true;
}

bool
core_link_reference_source_info(const core_link_reference_t *reference,
                                core_diag_location_t *location,
                                core_string_t *provenance)
{
    if (!reference
        || (!reference->source_location.has_location
            && !reference->provenance.data)) return false;
    if (location) *location = reference->source_location;
    if (provenance) *provenance = reference->provenance;
    return true;
}

bool
core_link_reference_is_deferred(const core_link_reference_t *reference)
{
    return reference && reference->deferred;
}

core_string_t
core_link_reference_deferral_reason(const core_link_reference_t *reference)
{
    return reference ? reference->deferral_reason : (core_string_t){0};
}

bool
core_link_reference_deferral_prerequisite(
    const core_link_reference_t *reference, uint32_t *out)
{
    if (!reference || !reference->deferred) return false;
    if (out) *out = reference->deferral_prerequisite;
    return true;
}


static bool
add_artifact(core_link_unit_t *link, core_string_t module,
             core_string_t format, uint32_t version,
             uint32_t producer_stage, void *payload,
             core_link_payload_ownership_t ownership,
             core_link_payload_free_fn free_fn, bool unit_wide)
{
    if (!link || (link->root_count && !core_link_unit_collect(link))
        || (!unit_wide && (!module.data || !module.u8_bytes))
        || (unit_wide && (module.data || module.u8_bytes))
        || !format.data || !format.u8_bytes || !version
        || !payload_args_valid(ownership, free_fn)) return false;
    if (producer_stage) {
        core_link_stage_record_t *producer = find_stage(link, producer_stage);
        if (!producer || producer->state != CORE_LINK_STAGE_RUNNING)
            return false;
    }
    core_link_artifact_t probe = {
        .module = module, .format = format, .unit_wide = unit_wide,
    };
    if (core_dict_contains(&link->artifact_index, &probe)) {
        link_report(link, nullptr, "LANTERN-L0006",
            "link unit contains duplicate artifacts for one module and format");
        return false;
    }
    if (!core_list_reserve_raw((void **)&link->artifacts,
                               &link->artifact_capacity,
                               link->artifact_count + 1,
                               sizeof(*link->artifacts)))
        abort();
    core_link_artifact_t *artifact = core_alloc(core_link_artifact_t);
    *artifact = (core_link_artifact_t){
        .module = unit_wide ? (core_string_t){0} : core_string_dup(module),
        .format = core_string_dup(format),
        .version = version,
        .producer_stage = producer_stage,
        .payload = payload,
        .ownership = ownership,
        .free_fn = free_fn,
        .unit_wide = unit_wide,
    };
    link->artifacts[link->artifact_count++] = artifact;
    if (!core_dict_add(&link->artifact_index, artifact, artifact)) abort();
    link->validated = false;
    return true;
}

bool
core_link_unit_add_artifact(core_link_unit_t *link, core_string_t module,
                            core_string_t format, uint32_t version,
                            uint32_t producer_stage, void *payload,
                            core_link_payload_ownership_t ownership,
                            core_link_payload_free_fn free_fn)
{
    return add_artifact(link, module, format, version, producer_stage,
                        payload, ownership,
                        free_fn, false);
}

bool
core_link_unit_add_unit_artifact(core_link_unit_t *link,
                                 core_string_t format, uint32_t version,
                                 uint32_t producer_stage, void *payload,
                                 core_link_payload_ownership_t ownership,
                                 core_link_payload_free_fn free_fn)
{
    return add_artifact(link, (core_string_t){0}, format, version,
                        producer_stage, payload,
                        ownership, free_fn, true);
}

bool
core_link_unit_require_artifact(core_link_unit_t *link, core_string_t module,
                                core_string_t format,
                                uint32_t required_version,
                                core_link_artifact_view_t *out)
{
    if (out) *out = (core_link_artifact_view_t){0};
    if (!link || (link->root_count && !core_link_unit_collect(link))
        || !module.data || !format.data
        || !required_version)
        return false;
    core_link_artifact_t probe = {
        .module = module, .format = format,
    };
    core_link_artifact_t *artifact = core_dict_get(
        &link->artifact_index, &probe, nullptr);
    if (artifact) {
        if (artifact->version != required_version) {
            link_report(link, nullptr, "LANTERN-L0007",
                        "linked semantic artifact has an incompatible version");
            link->validated = false;
            return false;
        }
        if (out) {
            *out = (core_link_artifact_view_t){
                .module = artifact->module,
                .format = artifact->format,
                .version = artifact->version,
                .payload = artifact->payload,
                .unit_wide = artifact->unit_wide,
            };
        }
        return true;
    }
    link_report(link, nullptr, "LANTERN-L0008",
                "required linked semantic artifact is missing");
    link->validated = false;
    return false;
}

size_t
core_link_unit_artifact_count(core_link_unit_t *link)
{
    return link && (!link->root_count || core_link_unit_collect(link))
        ? link->artifact_count : 0;
}

bool
core_link_unit_artifact(core_link_unit_t *link, size_t index,
                        core_link_artifact_view_t *out)
{
    if (!link || (link->root_count && !core_link_unit_collect(link)) || !out
        || index >= link->artifact_count) return false;
    const core_link_artifact_t *artifact = link->artifacts[index];
    *out = (core_link_artifact_view_t){
        .module = artifact->module,
        .format = artifact->format,
        .version = artifact->version,
        .payload = artifact->payload,
        .unit_wide = artifact->unit_wide,
    };
    return true;
}

static core_link_contribution_t *
find_contribution(core_link_unit_t *link, core_string_t module,
                 core_string_t domain, bool unit_wide)
{
    core_link_contribution_t probe = {
        .module = module, .domain = domain, .unit_wide = unit_wide,
    };
    return core_dict_get(&link->contribution_index, &probe, nullptr);
}

static void
contribution_view(const core_link_contribution_t *contribution,
                  core_link_contribution_view_t *out)
{
    *out = (core_link_contribution_view_t){
        .module = contribution->module,
        .domain = contribution->domain,
        .version = contribution->version,
        .producer_stage = contribution->producer_stage,
        .state = contribution->state,
        .provenance = contribution->provenance,
        .provenance_version = contribution->provenance_version,
        .payload = contribution->payload,
        .unit_wide = contribution->unit_wide,
    };
}

bool
core_link_unit_set_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t version, uint32_t producer_stage,
    core_link_contribution_state_t state, core_string_t provenance,
    uint32_t provenance_version, void *payload,
    core_link_payload_ownership_t ownership,
    core_link_payload_free_fn free_fn)
{
    if (!link || (link->root_count && !core_link_unit_collect(link))
        || !domain.data || !domain.u8_bytes || !version
        || !provenance.data || !provenance.u8_bytes || !provenance_version
        || state < CORE_LINK_CONTRIBUTION_PENDING
        || state > CORE_LINK_CONTRIBUTION_FAILED
        || (state != CORE_LINK_CONTRIBUTION_COMPLETE && payload)
        || !payload_args_valid(ownership, free_fn)
        || ((!module.data) != (module.u8_bytes == 0))) return false;
    bool unit_wide = !module.data;
    core_link_stage_record_t *producer = nullptr;
    if (producer_stage) {
        producer = find_stage(link, producer_stage);
        if (!producer || producer->state != CORE_LINK_STAGE_RUNNING)
            return false;
    }

    core_link_contribution_t *contribution = find_contribution(
        link, module, domain, unit_wide);
    if (!contribution) {
        if (!core_list_reserve_raw((void **)&link->contributions,
                                   &link->contribution_capacity,
                                   link->contribution_count + 1,
                                   sizeof(*link->contributions)))
            abort();
        contribution = core_alloc(core_link_contribution_t);
        *contribution = (core_link_contribution_t){
            .module = unit_wide ? (core_string_t){0}
                                : core_string_dup(module),
            .domain = core_string_dup(domain),
            .unit_wide = unit_wide,
        };
        link->contributions[link->contribution_count++] = contribution;
        if (!core_dict_add(&link->contribution_index, contribution,
                           contribution)) abort();
    }
    else {
        clear_contribution_payload(contribution);
        core_free(contribution->provenance.data);
    }

    contribution->version = version;
    contribution->producer_stage = producer_stage;
    contribution->state = state;
    contribution->provenance = core_string_dup(provenance);
    contribution->provenance_version = provenance_version;
    contribution->payload = payload;
    contribution->ownership = ownership;
    contribution->free_fn = free_fn;
    link->validated = false;
    return true;
}

bool
core_link_unit_contribution(core_link_unit_t *link,
                            core_string_t module, core_string_t domain,
                            core_link_contribution_view_t *out)
{
    if (out) *out = (core_link_contribution_view_t){0};
    if (!link || (link->root_count && !core_link_unit_collect(link))
        || !domain.data || !domain.u8_bytes
        || ((!module.data) != (module.u8_bytes == 0))) return false;
    bool unit_wide = !module.data;
    core_link_contribution_t *contribution = find_contribution(
        link, module, domain, unit_wide);
    if (!contribution) return false;
    if (out) contribution_view(contribution, out);
    return true;
}

bool
core_link_unit_require_contribution(
    core_link_unit_t *link, core_string_t module, core_string_t domain,
    uint32_t required_version, core_link_contribution_view_t *out)
{
    if (out) *out = (core_link_contribution_view_t){0};
    core_link_contribution_view_t contribution;
    if (!link || !required_version
        || !core_link_unit_contribution(link, module, domain, &contribution)) {
        if (link)
            link_report(link, nullptr, "LANTERN-L0014",
                "required link-unit analysis contribution is missing");
        return false;
    }
    if (contribution.version != required_version) {
        link_report(link, nullptr, "LANTERN-L0015",
            "link-unit analysis contribution has an incompatible version");
        return false;
    }
    if (contribution.state != CORE_LINK_CONTRIBUTION_COMPLETE) {
        core_buffer_t message = {0};
        const char *state = contribution.state == CORE_LINK_CONTRIBUTION_PENDING
            ? "pending" : contribution.state == CORE_LINK_CONTRIBUTION_UNAVAILABLE
            ? "unavailable" : "failed";
        core_buffer_printf(&message,
            "required link-unit analysis contribution '%.*s' is %s",
            (int)domain.u8_bytes, domain.data, state);
        link_report(link, nullptr, "LANTERN-L0016", message.data);
        core_buffer_release(&message);
        return false;
    }
    if (contribution.producer_stage
        && core_link_unit_stage_state(link, contribution.producer_stage)
            != CORE_LINK_STAGE_COMPLETE) {
        link_report(link, nullptr, "LANTERN-L0017",
            "analysis contribution producer stage is not complete");
        return false;
    }
    if (out) *out = contribution;
    return true;
}

size_t
core_link_unit_contribution_count(core_link_unit_t *link)
{
    return link && (!link->root_count || core_link_unit_collect(link))
        ? link->contribution_count : 0;
}

bool
core_link_unit_contribution_at(core_link_unit_t *link, size_t index,
                               core_link_contribution_view_t *out)
{
    if (!link || (link->root_count && !core_link_unit_collect(link)) || !out
        || index >= link->contribution_count) return false;
    contribution_view(link->contributions[index], out);
    return true;
}

bool
core_link_unit_add_dependency(
    core_link_unit_t *link, uint32_t kind, uint32_t producer_stage,
    core_link_dependency_state_t state,
    core_link_declaration_t *from, core_link_declaration_t *to,
    core_translation_unit_t *source_unit, const void *source_site,
    core_diag_location_t source_location, core_string_t provenance,
    uint32_t provenance_version, core_link_dependency_t **out)
{
    if (out) *out = nullptr;
    if (!link || !kind
        || state < CORE_LINK_DEPENDENCY_RESOLVED
        || state > CORE_LINK_DEPENDENCY_FAILED
        || (state == CORE_LINK_DEPENDENCY_RESOLVED && !to)
        || !core_link_unit_collect(link)) return false;
    bool has_from = false, has_to = false;
    for (size_t i = 0; i < link->declaration_count; i++) {
        has_from |= from && link->declarations[i] == from;
        has_to |= link->declarations[i] == to;
    }
    core_link_stage_record_t *producer = producer_stage
        ? find_stage(link, producer_stage) : nullptr;
    bool source_is_member = !source_unit
        || core_dict_contains(&link->unit_index, source_unit);
    if ((from && !has_from) || (to && !has_to) || !source_is_member
        || (producer_stage && (!producer
            || producer->state != CORE_LINK_STAGE_RUNNING))) {
        link_report_at(link, source_unit, source_location, "LANTERN-L0018",
            "dependency fact has an endpoint, source, or producer outside its link analysis");
        return false;
    }
    core_link_dependency_t probe = {
        .kind = kind, .producer_stage = producer_stage, .state = state,
        .from = from, .to = to, .source_unit = source_unit,
        .source_site = source_site,
    };
    core_link_dependency_t *existing = core_dict_get(
        &link->dependency_index, &probe, nullptr);
    if (existing) {
        if (out) *out = existing;
        return true;
    }
    if (!core_list_reserve_raw((void **)&link->dependencies,
                               &link->dependency_capacity,
                               link->dependency_count + 1,
                               sizeof(*link->dependencies)))
        abort();
    core_link_dependency_t *dependency = core_alloc(core_link_dependency_t);
    *dependency = (core_link_dependency_t){
        .kind = kind,
        .producer_stage = producer_stage,
        .state = state,
        .from = from,
        .to = to,
        .source_unit = source_unit,
        .source_site = source_site,
        .source_location = source_location,
        .provenance = provenance.data
            ? core_string_dup(provenance) : (core_string_t){0},
        .provenance_version = provenance_version,
    };
    link->dependencies[link->dependency_count++] = dependency;
    link->validated = false;
    if (!core_dict_add(&link->dependency_index, dependency, dependency))
        abort();
    if (out) *out = dependency;
    return true;
}

size_t
core_link_unit_dependency_count(core_link_unit_t *link)
{
    return link && (!link->root_count || core_link_unit_collect(link))
        ? link->dependency_count : 0;
}

bool
core_link_unit_dependency_at(core_link_unit_t *link, size_t index,
                             core_link_dependency_view_t *out)
{
    if (!link || (link->root_count && !core_link_unit_collect(link)) || !out
        || index >= link->dependency_count) return false;
    const core_link_dependency_t *dependency = link->dependencies[index];
    *out = (core_link_dependency_view_t){
        .kind = dependency->kind,
        .producer_stage = dependency->producer_stage,
        .state = dependency->state,
        .from = dependency->from,
        .to = dependency->to,
        .source_unit = dependency->source_unit,
        .source_site = dependency->source_site,
        .source_location = dependency->source_location,
        .provenance = dependency->provenance,
        .provenance_version = dependency->provenance_version,
    };
    return true;
}
