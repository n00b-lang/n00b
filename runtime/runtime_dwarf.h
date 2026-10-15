#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *directory;
    uint64_t directory_length;
    const char *file;
    uint64_t file_length;
    uint32_t line;
} n00bb_source_line_t;

bool n00bb_runtime_source_line(uintptr_t address, n00bb_source_line_t *result);
