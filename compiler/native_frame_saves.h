#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/list.h"
#include "lib/target.h"

#define N00BB_FRAME_SAVE_SLOTS 16

typedef struct {
    uint32_t section;
    uint64_t offset;
    int32_t record;
    uint32_t count;
    int32_t slots[N00BB_FRAME_SAVE_SLOTS];
} n00bb_frame_saves_t;
core_list_decl(n00bb_frame_saves_t);

bool n00bb_frame_saves_collect(const void *object, size_t object_bytes,
    core_target_object_format_t format,
    core_list_t(n00bb_frame_saves_t) *out);
const n00bb_frame_saves_t *n00bb_frame_saves_find(
    const core_list_t(n00bb_frame_saves_t) *list, uint32_t section,
    uint64_t offset);
