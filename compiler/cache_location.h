#pragma once

#include "core.h"

core_string_t n00bb_cache_root(void);
void n00bb_cache_set_key(core_string_t key);
void n00bb_cache_set_root(core_string_t root);
core_string_t n00bb_cache_directory(core_string_t component);
core_string_t n00bb_cache_mirror_directory(core_string_t component,
                                           core_string_t absolute_path);
bool n00bb_make_directories(core_string_t directory);

// A lock file shared between compiler processes: opened (created if needed)
// without being inherited by children, then locked as a whole, exclusively.
int n00bb_open_lock_file(core_string_t path);
bool n00bb_lock_file(int fd, bool wait);
void n00bb_unlock_file(int fd);
