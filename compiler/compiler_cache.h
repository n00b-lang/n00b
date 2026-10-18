#ifndef N00BB_COMPILER_CACHE_H
#define N00BB_COMPILER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler_link.h"
#include "specialize.h"
#include "llvm_backend.h"
#include "ir/ir.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "parse/commander.h"
#include "th/environment.h"

typedef struct {
    const char *module;
    const uint8_t *native;
    size_t native_bytes;
    const uint8_t *abi;
    size_t abi_bytes;
    const uint8_t *mutators;
    size_t mutator_bytes;
} n00bb_cache_module_object_t;
core_list_decl(n00bb_cache_module_object_t);

typedef struct n00bb_cache_t n00bb_cache_t;

n00bb_cache_t *n00bb_cache_open(const char *root_path,
                                parse_cmdr_result_t *args,
                                const char *compiler_path,
                                const char *triple, ir_ctx_t *ir_context);
void n00bb_cache_free(n00bb_cache_t *cache);
size_t n00bb_cache_interface_count(const n00bb_cache_t *cache);
const th_env *n00bb_cache_interface(const n00bb_cache_t *cache,
                                    size_t index);
ir_module_t *n00bb_cache_module(const n00bb_cache_t *cache,
                                core_string_t module);
bool n00bb_cache_module_object(const n00bb_cache_t *cache,
                               const char *module,
                               n00bb_cache_module_object_t *object);
core_list_decl(n00bb_specialization_seed_t);
const n00bb_llvm_mutator_classifier_t *n00bb_cache_mutator_classifier(
    const n00bb_cache_t *cache);
void n00bb_cache_seeds(const n00bb_cache_t *cache, th_ctx *types,
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
