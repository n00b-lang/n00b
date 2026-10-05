#pragma once

#include "lib/module.h"

typedef struct n00bb_resolver_t n00bb_resolver_t;

n00bb_resolver_t *n00bb_resolver_new(const char *argv0);
void n00bb_resolver_free(n00bb_resolver_t *resolver);
core_module_resolver_t n00bb_resolver_interface(n00bb_resolver_t *resolver);
bool n00bb_operand_module_spec(const char *operand,
                               core_module_spec_t *out);
core_module_resolve_status_t n00bb_locate_operand(
    n00bb_resolver_t *resolver, const char *operand,
    core_module_spec_t *module_out, core_source_file_t *source_out);
bool n00bb_same_package(const core_translation_unit_t *left,
                        const core_translation_unit_t *right);
core_string_t n00bb_package_id(const core_translation_unit_t *unit);
void n00bb_source_file_set_package(core_source_file_t *source,
                                  const char *package_id);
core_string_t n00bb_resolver_search_description(
    n00bb_resolver_t *resolver, const char *operand);
bool n00bb_resolver_supports_locator(n00bb_resolver_t *resolver,
                                     const char *operand,
                                     core_string_t *unsupported_scheme);
char *n00bb_grammar_path(const char *argv0);
