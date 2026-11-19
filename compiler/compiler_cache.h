#ifndef N00BB_COMPILER_CACHE_H
#define N00BB_COMPILER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler_link.h"
#include "specialize.h"
#include "llvm_backend.h"
#include "ir/archive.h"
#include "ir/ir.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "parse/commander.h"
#include "th/environment.h"

typedef struct {
    core_string_t module;
    const uint8_t *object;
    size_t object_bytes;
    const uint8_t *abi;
    size_t abi_bytes;
    const uint8_t *runtime_object;
    size_t runtime_object_bytes;
    const uint8_t *mutators;
    size_t mutators_bytes;
    // The module's index table as its code was compiled against it (see
    // n00bb_abi_index_table_t): its symbol, type hashes and the IR names of
    // its functions, for emitting the module's code again from cached IR.
    const uint8_t *index;
    size_t index_bytes;
} n00bb_cache_module_object_t;
core_list_decl(n00bb_cache_module_object_t);

typedef struct n00bb_cache_t n00bb_cache_t;

typedef bool (*n00bb_cache_locate_fn)(void *context, core_string_t module,
                                      core_string_t *path);

// A module's kept warnings can depend on more than its key covers: the
// syntax warning settings, and how many subpackages a package it imports
// has. What they were when the warnings were kept is stored with them as
// records, each a byte saying which:
// - N00BB_WARNING_INPUT_SYNTAX, then n00bb_syntax_warning_settings() as a
//   little-endian u64;
// - N00BB_WARNING_INPUT_SUBPACKAGES, then the package's id and name, each
//   a little-endian u32 length and its bytes, then the number of its
//   direct subpackages, 2 standing for more than one.
// A compile that reuses the module checks them, and has it compiled again
// (n00bb_cache_mark_stale) when one no longer holds.
enum {
    N00BB_WARNING_INPUT_SYNTAX = 1,
    N00BB_WARNING_INPUT_SUBPACKAGES = 2,
};
bool n00bb_cache_warning_inputs(const n00bb_cache_t *cache,
                                core_string_t module, const uint8_t **data,
                                size_t *length);
void n00bb_cache_mark_stale(n00bb_cache_t *cache, core_string_t module);
uint64_t n00bb_syntax_warning_settings(void);

void n00bb_symbol_use_key(core_buffer_t *out,
                          const core_translation_unit_t *unit,
                          const parse_scope_t *scope,
                          const parse_sym_entry_t *symbol);

n00bb_cache_t *n00bb_cache_open(core_string_t root_path,
                                parse_cmdr_result_t *args,
                                core_string_t triple, ir_ctx_t *ir_context,
                                n00bb_cache_locate_fn locate,
                                void *locate_context,
                                core_string_t libraries);
bool n00bb_cache_library_module(const n00bb_cache_t *cache,
                                core_string_t module);
bool n00bb_cache_module_source(const n00bb_cache_t *cache,
                               core_string_t module, core_string_t *path,
                               uint64_t *source_hash);
bool n00bb_cache_declaration_sites(const n00bb_cache_t *cache,
                                   core_string_t module, core_buffer_t *out);
bool n00bb_cache_unused_facts(const n00bb_cache_t *cache,
                              core_string_t module, core_buffer_t *out);
bool n00bb_cache_module_summaries(const n00bb_cache_t *cache,
                                  core_string_t module, core_string_t kind,
                                  const uint8_t **data, size_t *length);
bool n00bb_cache_replay_warnings(const n00bb_cache_t *cache,
                                 const core_translation_unit_t *unit,
                                 core_diagnostics_t *into);
void n00bb_cache_free(n00bb_cache_t *cache);
size_t n00bb_cache_interface_count(const n00bb_cache_t *cache);
const th_env *n00bb_cache_interface(const n00bb_cache_t *cache,
                                    size_t index);
bool n00bb_cache_has_module(const n00bb_cache_t *cache,
                            core_string_t module);
bool n00bb_cache_optimistic(const n00bb_cache_t *cache);
bool n00bb_cache_module_speculative(const n00bb_cache_t *cache,
                                    core_string_t module);
void n00bb_cache_log_restart(core_string_t reason);
void n00bb_cache_log_runtime_object(core_string_t module);
void n00bb_cache_stale_modules(const n00bb_cache_t *cache,
                               core_buffer_t *out);
void n00bb_cache_unloadable_modules(const n00bb_cache_t *cache,
                                    core_buffer_t *out);
void n00bb_cache_note_refreeze(n00bb_cache_t *cache, core_string_t module);
void n00bb_cache_skip_store(n00bb_cache_t *cache, core_string_t module);
void n00bb_cache_refreeze_modules(const n00bb_cache_t *cache,
                                  core_buffer_t *out);
bool n00bb_cache_assumptions_hold(n00bb_cache_t *cache,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis);
bool n00bb_cache_record_summaries(n00bb_cache_t *cache,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis);
bool n00bb_cache_summary_assumptions_hold(const n00bb_cache_t *cache);
bool n00bb_cache_summary_hints(const n00bb_cache_t *cache, core_buffer_t *out);
typedef bool (*n00bb_cache_module_used_fn)(void *context,
                                           core_string_t module);
bool n00bb_cache_load_ir(n00bb_cache_t *cache, ir_module_lookup_fn lookup,
                         n00bb_cache_module_used_fn used, void *context);
bool n00bb_cache_interface_has_module(const n00bb_cache_t *cache,
                                      size_t index, core_string_t module);
ir_module_t *n00bb_cache_module(const n00bb_cache_t *cache,
                                core_string_t module);
bool n00bb_cache_owns(const n00bb_cache_t *cache, const ir_module_t *module);
bool n00bb_cache_module_object(const n00bb_cache_t *cache,
                               core_string_t module,
                               n00bb_cache_module_object_t *object);
core_list_decl(n00bb_specialization_seed_t);
void n00bb_cache_seeds(const n00bb_cache_t *cache, th_ctx *types,
    ir_module_lookup_fn lookup, void *context,
    core_list_t(n00bb_specialization_seed_t) *seeds);
bool n00bb_cache_instance_object(n00bb_cache_t *cache,
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    const n00bb_specialization_t *item, n00bb_cache_module_object_t *object);
// With `clean_only`, an SCC is snapshotted only when none of its modules,
// nor any module it imports, reported an error.
bool n00bb_cache_snapshot(n00bb_cache_t *cache, core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis, bool clean_only);
void n00bb_cache_capture_summaries(n00bb_cache_t *cache,
                                   n00bb_link_analysis_t *analysis,
                                   uint8_t phase);
void n00bb_cache_set_check_only(bool check_only);
// A library build: like a check, it makes no native code; it keeps the
// modules' ABI objects in a phase of its own.
void n00bb_cache_set_library(void);
// The modules a compile builds from source, comma separated: cache entries
// holding any of them are not loaded.
void n00bb_cache_set_roots(core_string_t names);
bool n00bb_cache_store(n00bb_cache_t *cache, core_link_unit_t *link,
                       n00bb_link_analysis_t *analysis,
                       const n00bb_cache_module_object_t *objects,
                       size_t object_count);

#endif
