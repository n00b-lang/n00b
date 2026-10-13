#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lib/buffer.h"

char *n00bb_write_temporary_object(const void *bytes, size_t length);
char *n00bb_archive_path(const char *compiler_path, const char *archive_name);
bool n00bb_read_exact(int fd, void *data, size_t length);
bool n00bb_read_bitcode(const char *path, core_buffer_t *out);
