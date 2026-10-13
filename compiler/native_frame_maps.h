#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/target.h"

typedef struct n00bb_native_frames_t n00bb_native_frames_t;

/* Collect final native stack offsets from LLVM objects before final linking. */
n00bb_native_frames_t *n00bb_native_frames_new(
    core_target_object_format_t format);
bool n00bb_native_frames_add(n00bb_native_frames_t *frames,
    const void *object, size_t object_bytes);
bool n00bb_native_frames_emit(const n00bb_native_frames_t *frames,
    const core_target_desc_t *target, uint8_t **image, size_t *image_bytes);
void n00bb_native_frames_free(n00bb_native_frames_t *frames);
