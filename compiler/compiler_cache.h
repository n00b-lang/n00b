#ifndef N00BB_COMPILER_CACHE_H
#define N00BB_COMPILER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler_link.h"
#include "lib/link_unit.h"
#include "lib/list.h"
#include "parse/commander.h"

typedef struct {
    const char *module;
    const uint8_t *native;
    size_t native_bytes;
    const uint8_t *abi;
    size_t abi_bytes;
} n00bb_cache_module_object_t;
core_list_decl(n00bb_cache_module_object_t);

bool n00bb_cache_store(core_link_unit_t *link,
                       n00bb_link_analysis_t *analysis,
                       parse_cmdr_result_t *args, const char *compiler_path,
                       const n00bb_cache_module_object_t *objects,
                       size_t object_count);

#endif
