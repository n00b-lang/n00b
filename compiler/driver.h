#pragma once

#include <pthread.h>

#include "lib/module.h"
#include "compiler_module_literal.h"
#include "toolchain.h"

typedef struct n00bb_resolver_t n00bb_resolver_t;

n00bb_resolver_t *n00bb_resolver_new(core_string_t argv0,
                                     core_diagnostics_t *diagnostics);
void n00bb_resolver_free(n00bb_resolver_t *resolver);
bool n00bb_resolver_prepare_packages(n00bb_resolver_t *resolver);
core_module_resolver_t n00bb_resolver_interface(n00bb_resolver_t *resolver);
bool n00bb_operand_module_spec(core_string_t operand,
                               core_module_spec_t *out);
bool n00bb_resolver_add_generated(n00bb_resolver_t *resolver,
                                  core_string_t identifier,
                                  core_string_t text,
                                  const core_translation_unit_t *owner,
                                  n00bb_native_declarations_t natives,
                                  n00bb_line_origins_t origins);
void n00bb_resolver_remap_diagnostics(n00bb_resolver_t *resolver,
                                      core_diagnostics_t *diagnostics);
n00bb_native_declarations_t n00bb_source_natives(
    const core_translation_unit_t *unit);
bool n00bb_resolver_generated_text(n00bb_resolver_t *resolver,
                                   core_string_t identifier,
                                   core_string_t *text);
core_module_resolve_status_t n00bb_locate_operand(
    n00bb_resolver_t *resolver, core_string_t operand,
    core_module_spec_t *module_out, core_source_file_t *source_out);
bool n00bb_resolver_same_root(const n00bb_resolver_t *resolver,
                              const core_translation_unit_t *left,
                              const core_translation_unit_t *right);
void n00bb_source_file_mark_interface(core_source_file_t *source,
                                      uint64_t source_hash);
bool n00bb_source_file_is_interface(const core_source_file_t *source);
bool n00bb_same_package(const core_translation_unit_t *left,
                        const core_translation_unit_t *right);
core_string_t n00bb_package_id(const core_translation_unit_t *unit);
core_string_t n00bb_package_namespace(const core_translation_unit_t *unit);
void n00bb_source_file_set_package(core_source_file_t *source,
                                  core_string_t package_id);
void n00bb_resolver_note_roots(n00bb_resolver_t *resolver,
                               core_diagnostics_t *diagnostics);
void n00bb_resolver_note_search(n00bb_resolver_t *resolver,
                                core_string_t operand,
                                core_diagnostics_t *diagnostics);
bool n00bb_resolver_supports_locator(n00bb_resolver_t *resolver,
                                     core_string_t operand,
                                     core_string_t *unsupported_scheme);
bool n00bb_resolver_has_source_module(n00bb_resolver_t *resolver,
                                      core_string_t identifier);
bool n00bb_resolver_package_manifests(n00bb_resolver_t *resolver,
                                      core_string_t package_id,
                                      core_module_discovery_emit_fn emit,
                                      void *emit_context);
bool n00bb_resolver_known_manifest(n00bb_resolver_t *resolver,
                                   core_string_t package_id,
                                   bool *has_manifest);
void n00bb_resolver_remember_manifest(n00bb_resolver_t *resolver,
                                      core_string_t package_id,
                                      bool has_manifest);
void n00bb_mark_open_module(core_translation_unit_t *unit);
bool n00bb_open_module(const core_translation_unit_t *unit);
bool n00bb_resolver_internal_import(n00bb_resolver_t *resolver,
                                    const core_translation_unit_t *importer,
                                    core_string_t name,
                                    core_module_spec_t *resolved);
bool n00bb_resolver_ambiguous_module(n00bb_resolver_t *resolver,
                                     const core_translation_unit_t *importer,
                                     core_string_t name,
                                     core_string_t *module_path,
                                     core_string_t *directory_path);
void n00bb_resolver_store_package_syntax(n00bb_resolver_t *resolver,
                                         core_string_t source_path,
                                         const void *names, size_t size);
bool n00bb_start_worker(pthread_t *thread, void *(*start)(void *),
                        void *argument);
