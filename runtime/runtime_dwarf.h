#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *directory;
    uint64_t directory_length;
    const char *file;
    uint64_t file_length;
    uint32_t line;
    uint32_t column;
} n00bb_source_line_t;

typedef struct {
    const char *name;
    uint64_t name_length;
    n00bb_source_line_t call;
} n00bb_inline_frame_t;

bool n00bb_runtime_source_line(uintptr_t address, n00bb_source_line_t *result);
size_t n00bb_runtime_inline_frames(uintptr_t address,
                                   n00bb_inline_frame_t *frames,
                                   size_t capacity);
