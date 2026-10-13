#pragma once

#include "lib/target.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
    uint32_t alignment;
    const char *symbol;
    const char *const *aliases;
    size_t alias_count;
} n00bb_static_graph_node_t;

typedef struct {
    size_t source;
    size_t source_offset;
    size_t target;
    size_t target_offset;
} n00bb_static_graph_edge_t;

bool n00bb_static_graph_emit(
    const core_target_desc_t *target,
    const n00bb_static_graph_node_t *nodes, size_t node_count,
    const n00bb_static_graph_edge_t *edges, size_t edge_count,
    uint8_t **object, size_t *object_size);
