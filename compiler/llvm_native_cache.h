#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "lib/buffer.h"

typedef struct n00bb_native_cache_t n00bb_native_cache_t;

core_string_t n00bb_native_cache_root(void);
n00bb_native_cache_t *n00bb_native_cache_open(core_string_t executable_path);
void n00bb_native_cache_close(n00bb_native_cache_t *cache);

core_uint128_t n00bb_native_cache_key(
    const n00bb_native_cache_t *cache, const void *bitcode,
    size_t bitcode_bytes, core_string_t target_triple, core_string_t cpu,
    core_string_t features, bool debug_enabled, uint8_t opt_level,
    bool frame_maps);
bool n00bb_native_cache_read(const n00bb_native_cache_t *cache,
    core_uint128_t key, core_buffer_t *object);
void n00bb_native_cache_write(const n00bb_native_cache_t *cache,
    core_uint128_t key, const void *object, size_t object_bytes);
