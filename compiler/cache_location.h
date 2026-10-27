#pragma once

#include "core.h"

core_string_t n00bb_cache_root(void);
void n00bb_cache_set_key(core_string_t key);
void n00bb_cache_set_root(core_string_t root);
core_string_t n00bb_cache_directory(core_string_t component);
core_string_t n00bb_cache_mirror_directory(core_string_t component,
                                           core_string_t absolute_path);
bool n00bb_make_directories(core_string_t directory);
