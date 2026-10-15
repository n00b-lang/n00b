#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "lib/buffer.h"

typedef struct n00bb_native_cache_t n00bb_native_cache_t;

char *n00bb_native_cache_root(void);
n00bb_native_cache_t *n00bb_native_cache_open(const char *executable_path);
void n00bb_native_cache_close(n00bb_native_cache_t *cache);

core_uint128_t n00bb_native_cache_key(
    const n00bb_native_cache_t *cache, const void *bitcode,
    size_t bitcode_bytes, const char *target_triple, const char *cpu,
    const char *features, bool debug_enabled, bool frame_maps);
bool n00bb_native_cache_read(const n00bb_native_cache_t *cache,
    core_uint128_t key, core_buffer_t *object);
void n00bb_native_cache_write(const n00bb_native_cache_t *cache,
    core_uint128_t key, const void *object, size_t object_bytes);
