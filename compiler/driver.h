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
// The current directory with symlinks resolved, as package roots are; the
// raw directory if it cannot be resolved. Relative paths.path entries are
// resolved against it everywhere they are read.
core_string_t n00bb_canonical_cwd(void);
// True for a module of the `__default` package (a top-level module of the
// current directory, or a file named on the command line), which holds no
// system package.
bool n00bb_default_package(const core_translation_unit_t *unit);
// True for a module of a system package, judged by where it came from (see
// n00bb_system_package), for source and cached modules alike.
bool n00bb_system_unit(const core_translation_unit_t *unit);
bool n00bb_resolver_add_generated(n00bb_resolver_t *resolver,
                                  core_string_t identifier,
                                  core_string_t text,
                                  const core_translation_unit_t *owner,
                                  n00bb_native_declarations_t natives,
                                  n00bb_line_origins_t origins,
                                  core_string_t inputs);
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
bool n00bb_resolver_has_source_module(n00bb_resolver_t *resolver,
                                      core_string_t identifier);
void n00bb_source_file_mark_interface(core_source_file_t *source,
                                      uint64_t source_hash);
bool n00bb_source_file_is_interface(const core_source_file_t *source);
// True for a module a module literal generated, giving the module the
// literal is in and that module's source path.
bool n00bb_source_file_generated_owner(const core_source_file_t *source,
                                       core_string_t *owner,
                                       core_string_t *owner_path);
// The files a generated module's literal reads, one per line.
core_string_t n00bb_source_file_generated_inputs(
    const core_source_file_t *source);
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
// The standard library cannot be shadowed. These find the pieces of a clash:
// whether a unit came from the toolchain library, the module file a root
// other than the library holds for `identifier` (`path` is owned by the
// caller), and the library's manifests that may declare `declared_name`.
// A standard library module that failed because a file elsewhere has its
// module path. A user's import reports it where it finds it; otherwise it
// is reported once loading is over.
typedef struct n00bb_stdlib_shadow_t {
    core_string_t name;
    core_string_t shadow;
    core_string_t stdlib;
    bool          reported;
    // Not a clash: `shadow` is a top-level module of the current directory
    // (the default package), which the library's name wins over, with a
    // warning rather than an error.
    bool          warning;
} n00bb_stdlib_shadow_t;

// Records a clash (or, with `warning`, a passed-over default-package
// module); the resolver owns the record for its lifetime.
n00bb_stdlib_shadow_t *n00bb_resolver_record_shadow(
    n00bb_resolver_t *resolver, core_string_t name, core_string_t shadow,
    core_string_t stdlib, bool warning);
// True for the one caller that gets to report the clash.
bool n00bb_resolver_claim_shadow(n00bb_resolver_t *resolver,
                                 n00bb_stdlib_shadow_t *shadow);
size_t n00bb_resolver_shadow_count(n00bb_resolver_t *resolver);
n00bb_stdlib_shadow_t *n00bb_resolver_shadow_at(n00bb_resolver_t *resolver,
                                                size_t index);
bool n00bb_resolver_unit_in_stdlib(const n00bb_resolver_t *resolver,
                                   const core_translation_unit_t *unit);
bool n00bb_resolver_shadowing_source(n00bb_resolver_t *resolver,
                                     core_string_t identifier,
                                     core_string_t *path);
bool n00bb_resolver_stdlib_manifests(n00bb_resolver_t *resolver,
                                     core_string_t declared_name,
                                     core_module_discovery_emit_fn emit,
                                     void *emit_context);
void n00bb_resolver_store_package_syntax(n00bb_resolver_t *resolver,
                                         core_string_t source_path,
                                         const void *names, size_t size);
bool n00bb_start_worker(pthread_t *thread, void *(*start)(void *),
                        void *argument);
