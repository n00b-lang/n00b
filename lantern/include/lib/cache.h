#pragma once

#include "core.h"
#include "lib/dict.h"

typedef struct {
    void    *subject;
    uint64_t param;
    uint32_t query;
} core_cache_key_t;

typedef struct {
    core_dict_t *entries;
    uint64_t     gen;
    bool         enabled;
} core_cache_t;

void core_cache_init(core_cache_t *c);
void core_cache_free(core_cache_t *c);

bool core_cache_get(core_cache_t *c, void *subject, uint32_t query,
                    uint64_t param, void **out);
void core_cache_put(core_cache_t *c, void *subject, uint32_t query,
                    uint64_t param, void *result);

void     core_cache_invalidate(core_cache_t *c);
void     core_cache_clear(core_cache_t *c);

void core_cache_set_enabled(core_cache_t *c, bool on);

