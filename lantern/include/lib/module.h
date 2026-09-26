#pragma once

#include "core.h"
#include "lib/buffer.h"
#include "lib/string.h"

typedef struct core_diagnostics_t core_diagnostics_t;

typedef void (*core_module_user_data_free_fn)(void *data);

/* user_data is opaque to Lantern; release invokes user_data_free if set. */
typedef struct {
    core_string_t identifier;
    void *user_data;
    core_module_user_data_free_fn user_data_free;
} core_module_spec_t;

/* Initialization owns path/contents; release also invokes the data cleanup. */
typedef struct {
    core_string_t path;
    core_buffer_t *contents;
    void *user_data;
    core_module_user_data_free_fn user_data_free;
} core_source_file_t;

/* TU initialization copies module identity and takes the source file. */
typedef struct core_translation_unit_t {
    core_module_spec_t module;
    core_source_file_t source;
    struct core_translation_unit_t **imports;
    size_t import_count;
    size_t import_capacity;
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

typedef struct {
    core_module_resolve_fn resolve;
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
                           const char *contents, size_t byte_len);
void core_source_file_release(core_source_file_t *file);
core_string_t core_source_file_path(const core_source_file_t *file);
const char *core_source_file_contents(const core_source_file_t *file);
size_t core_source_file_size(const core_source_file_t *file);
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
/* Imported TUs are borrowed; their owner must keep them alive as long as the
 * importing TU may use these references. Duplicate imports are ignored. */
bool core_translation_unit_add_import(core_translation_unit_t *unit,
                                      core_translation_unit_t *imported);
size_t core_translation_unit_import_count(
    const core_translation_unit_t *unit);
core_translation_unit_t *core_translation_unit_import(
    const core_translation_unit_t *unit, size_t index);
/* Visits reachable import-graph SCCs dependency-first. The member array is
 * borrowed for the duration of the callback. */
bool core_translation_unit_visit_sccs(core_translation_unit_t *root,
                                      core_translation_unit_scc_fn callback,
                                      void *context);

core_module_resolve_status_t core_module_resolve(
    const core_module_resolver_t *resolver, const core_module_spec_t *module,
    core_source_file_t *out);
