#pragma once

#include <stdint.h>

typedef struct {
    const void *source;
    uint64_t size;
    uint32_t type_index;
    uint32_t alignment;
    uint32_t instance_descriptor;
    uint32_t immutable;
} n00bb_literal_graph_node_t;

typedef struct {
    uint32_t source;
    uint32_t target;
    uint64_t source_offset;
    uint64_t target_offset;
} n00bb_literal_graph_edge_t;

void *n00bb_literal_graph_clone(const n00bb_literal_graph_node_t *nodes,
    uint32_t node_count, const n00bb_literal_graph_edge_t *edges,
    uint32_t edge_count, uint32_t root);
