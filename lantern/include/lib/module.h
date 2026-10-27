#pragma once

#include <stdint.h>

#include "core.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/string.h"

typedef struct core_diagnostics_t core_diagnostics_t;
typedef struct core_translation_unit_t *core_tu_ptr_t;
core_list_decl(core_tu_ptr_t);

typedef void (*core_module_user_data_free_fn)(void *data);

typedef struct {
    core_string_t identifier;
    void *user_data;
    core_module_user_data_free_fn user_data_free;
} core_module_spec_t;

typedef struct {
    core_string_t path;
    core_buffer_t *contents;
    void *user_data;
    core_module_user_data_free_fn user_data_free;
    uint64_t identity;
    bool has_identity;
} core_source_file_t;

typedef struct core_translation_unit_t {
    core_module_spec_t module;
    core_source_file_t source;
    core_list_t(core_tu_ptr_t) imports;
    core_dict_t imports_by_unit;
    core_dict_t imports_by_module;
    uint64_t import_revision;
    bool imports_finalized;
    bool import_conflict;
    void *user_data;
    core_module_user_data_free_fn user_data_free;
    core_diagnostics_t *diagnostics;
} core_translation_unit_t;

typedef enum {
    CORE_MODULE_RESOLVED,
    CORE_MODULE_NOT_FOUND,
    CORE_MODULE_RESOLVE_ERROR,
} core_module_resolve_status_t;

typedef core_module_resolve_status_t (*core_module_resolve_fn)(
    void *context, const core_module_spec_t *module, core_source_file_t *out);

typedef bool (*core_module_discovery_emit_fn)(
    void *context, const core_module_spec_t *candidate);
typedef core_module_resolve_status_t (*core_module_discover_fn)(
    void *context, const core_module_spec_t *module,
    core_module_discovery_emit_fn emit, void *emit_context);

typedef struct {
    core_module_resolve_fn resolve;
    core_module_discover_fn discover;
    void *context;
} core_module_resolver_t;

typedef bool (*core_translation_unit_scc_fn)(
    void *context, core_translation_unit_t *const *members, size_t count);

bool core_module_spec_init(core_module_spec_t *spec, core_string_t identifier);
void core_module_spec_release(core_module_spec_t *spec);
bool core_module_spec_eq(const core_module_spec_t *a,
                         const core_module_spec_t *b);
void core_module_spec_set_user_data(core_module_spec_t *spec, void *data,
                                    core_module_user_data_free_fn free_fn);
void *core_module_spec_user_data(const core_module_spec_t *spec);

bool core_source_file_init(core_source_file_t *file, core_string_t path,
                           core_string_t contents);
void core_source_file_release(core_source_file_t *file);
core_string_t core_source_file_path(const core_source_file_t *file);
core_string_t core_source_file_contents(const core_source_file_t *file);
size_t core_source_file_size(const core_source_file_t *file);
void core_source_file_set_identity(core_source_file_t *file, uint64_t identity);
uint64_t core_source_file_identity(const core_source_file_t *file);
void core_source_file_set_user_data(core_source_file_t *file, void *data,
                                    core_module_user_data_free_fn free_fn);
void *core_source_file_user_data(const core_source_file_t *file);

bool core_translation_unit_init(core_translation_unit_t *unit,
                                const core_module_spec_t *module,
                                core_source_file_t *source);
void core_translation_unit_release(core_translation_unit_t *unit);
void core_translation_unit_set_user_data(
    core_translation_unit_t *unit, void *data,
    core_module_user_data_free_fn free_fn);
void *core_translation_unit_user_data(const core_translation_unit_t *unit);
core_diagnostics_t *core_translation_unit_diagnostics(
    core_translation_unit_t *unit);
bool core_translation_unit_has_fatal_diagnostics(
    const core_translation_unit_t *unit);
bool core_translation_unit_add_import(core_translation_unit_t *unit,
                                      core_translation_unit_t *imported);size_t core_translation_unit_import_count(
    const core_translation_unit_t *unit);
core_translation_unit_t *core_translation_unit_import(
    const core_translation_unit_t *unit, size_t index);
core_translation_unit_t *core_translation_unit_find_import(
    const core_translation_unit_t *unit, core_string_t module);
uint64_t core_translation_unit_import_revision(
    const core_translation_unit_t *unit);
bool core_translation_unit_finalize_imports(core_translation_unit_t *unit);
bool core_translation_unit_imports_finalized(
    const core_translation_unit_t *unit);
bool core_translation_unit_visit_sccs(core_translation_unit_t *root,
                                      core_translation_unit_scc_fn callback,
                                      void *context);
bool core_translation_units_visit_sccs(
    core_translation_unit_t *const *roots, size_t root_count,
    core_translation_unit_scc_fn callback, void *context);

core_module_resolve_status_t core_module_resolve(
    const core_module_resolver_t *resolver, const core_module_spec_t *module,
    core_source_file_t *out);
core_module_resolve_status_t core_module_discover(
    const core_module_resolver_t *resolver, const core_module_spec_t *module,
    core_module_discovery_emit_fn emit, void *emit_context);
