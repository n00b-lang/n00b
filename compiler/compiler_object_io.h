#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lib/buffer.h"
#include "lib/target.h"

core_string_t n00bb_write_temporary_object(const void *bytes, size_t length);
core_string_t n00bb_archive_path(core_string_t archive_name);
bool n00bb_read_bitcode(core_string_t path, core_buffer_t *out);

#define N00BB_SYSTEM_LIBRARY_LIMIT 5

// The system libraries and link flags for `target`; `libc` when the program
// links libc (see n00bb_links_libc).
const char *const *n00bb_system_libraries(const core_target_desc_t *target,
                                          bool libc);
// Whether a program linking `libraries` links libc: on Linux only when an
// FFI contract asks for "c", since programs otherwise run without it.
// Other targets always link their C library and need no separate runtime.
bool n00bb_links_libc(const core_target_desc_t *target,
                      const core_string_t *libraries, size_t count);
