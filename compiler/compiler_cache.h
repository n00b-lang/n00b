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
    const uint8_t *native;
    size_t native_bytes;
    const uint8_t *abi;
    size_t abi_bytes;
    const uint8_t *mutators;
    size_t mutator_bytes;
} n00bb_cache_module_object_t;
core_list_decl(n00bb_cache_module_object_t);

typedef struct n00bb_cache_t n00bb_cache_t;

typedef bool (*n00bb_cache_locate_fn)(void *context, core_string_t module,
                                      core_string_t *path);

n00bb_cache_t *n00bb_cache_open(core_string_t root_path,
                                parse_cmdr_result_t *args,
                                core_string_t compiler_path,
                                core_string_t triple, ir_ctx_t *ir_context,
                                n00bb_cache_locate_fn locate,
                                void *locate_context);
bool n00bb_cache_module_source(const n00bb_cache_t *cache,
                               core_string_t module, core_string_t *path,
                               uint64_t *source_hash);
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
bool n00bb_cache_check_dispatch(n00bb_cache_t *cache,
                                n00bb_link_analysis_t *analysis,
                                const th_env *environment);
void n00bb_cache_stale_modules(const n00bb_cache_t *cache,
                               core_buffer_t *out);
bool n00bb_cache_assumptions_hold(const n00bb_cache_t *cache,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis);
typedef bool (*n00bb_cache_module_used_fn)(void *context,
                                           core_string_t module);
bool n00bb_cache_load_ir(n00bb_cache_t *cache, ir_module_lookup_fn lookup,
                         n00bb_cache_module_used_fn used, void *context);
bool n00bb_cache_interface_has_module(const n00bb_cache_t *cache,
                                      size_t index, core_string_t module);
ir_module_t *n00bb_cache_module(const n00bb_cache_t *cache,
                                core_string_t module);
bool n00bb_cache_module_object(const n00bb_cache_t *cache,
                               core_string_t module,
                               n00bb_cache_module_object_t *object);
core_list_decl(n00bb_specialization_seed_t);
bool n00bb_cache_needs_build_time_capture(const n00bb_cache_t *cache);
const n00bb_llvm_mutator_classifier_t *n00bb_cache_mutator_classifier(
    const n00bb_cache_t *cache);
void n00bb_cache_seeds(const n00bb_cache_t *cache, th_ctx *types,
    ir_module_lookup_fn lookup, void *context,
    core_list_t(n00bb_specialization_seed_t) *seeds);
bool n00bb_cache_instance_object(n00bb_cache_t *cache,
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    const n00bb_specialization_t *item, n00bb_cache_module_object_t *object);
bool n00bb_cache_snapshot(n00bb_cache_t *cache, core_link_unit_t *link);
bool n00bb_cache_store(n00bb_cache_t *cache, core_link_unit_t *link,
                       n00bb_link_analysis_t *analysis,
                       const n00bb_cache_module_object_t *objects,
                       size_t object_count);

#endif
