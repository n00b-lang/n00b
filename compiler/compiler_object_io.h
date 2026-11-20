#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lib/buffer.h"
#include "lib/target.h"

core_string_t n00bb_write_temporary_object(const void *bytes, size_t length);
core_string_t n00bb_archive_path(core_string_t archive_name);
bool n00bb_read_bitcode(core_string_t path, core_buffer_t *out);

#define N00BB_SYSTEM_LIBRARY_LIMIT 5

const char *const *n00bb_system_libraries(const core_target_desc_t *target,
                                          bool libc);
bool n00bb_links_libc(const core_target_desc_t *target,
                      const core_string_t *libraries, size_t count);
