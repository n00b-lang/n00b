#pragma once

#include "lib/module.h"
#include "compiler_module_literal.h"

typedef struct n00bb_resolver_t n00bb_resolver_t;

char *n00bb_executable_path(const char *argv0);
n00bb_resolver_t *n00bb_resolver_new(const char *argv0);
void n00bb_resolver_free(n00bb_resolver_t *resolver);
bool n00bb_resolver_prepare_packages(n00bb_resolver_t *resolver);
core_module_resolver_t n00bb_resolver_interface(n00bb_resolver_t *resolver);
bool n00bb_operand_module_spec(const char *operand,
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
    n00bb_resolver_t *resolver, const char *operand,
    core_module_spec_t *module_out, core_source_file_t *source_out);
bool n00bb_same_package(const core_translation_unit_t *left,
                        const core_translation_unit_t *right);
core_string_t n00bb_package_id(const core_translation_unit_t *unit);
core_string_t n00bb_package_namespace(const core_translation_unit_t *unit);
void n00bb_source_file_set_package(core_source_file_t *source,
                                  const char *package_id);
core_string_t n00bb_resolver_search_description(
    n00bb_resolver_t *resolver, const char *operand);
bool n00bb_resolver_supports_locator(n00bb_resolver_t *resolver,
                                     const char *operand,
                                     core_string_t *unsupported_scheme);
bool n00bb_resolver_has_source_module(n00bb_resolver_t *resolver,
                                      core_string_t identifier);
bool n00bb_resolver_internal_import(n00bb_resolver_t *resolver,
                                    const core_translation_unit_t *importer,
                                    core_string_t name,
                                    core_module_spec_t *resolved);
void n00bb_resolver_store_package_syntax(n00bb_resolver_t *resolver,
                                         core_string_t source_path,
                                         const void *names, size_t size);
char *n00bb_grammar_path(const char *argv0);
