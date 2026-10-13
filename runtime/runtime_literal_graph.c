#include "runtime_literal_graph.h"

#include "builtin_os.h"
#include "n00b-abi-v1.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint64_t n00b_gc_root_depth(void);
extern void n00b_gc_root_push(void **slot);
extern void n00b_gc_root_pop(uint64_t depth);
extern void n00b_gc_barrier_value(const void *address, uint32_t type_index);

void *
n00bb_literal_graph_clone(const n00bb_literal_graph_node_t *nodes,
    uint32_t node_count, const n00bb_literal_graph_edge_t *edges,
    uint32_t edge_count, uint32_t root)
{
    size_t slot_bytes = 0;
    if (!nodes || !node_count || root >= node_count
        || (edge_count && !edges)
        || __builtin_mul_overflow((size_t)node_count, sizeof(void *),
                                  &slot_bytes)) __builtin_trap();
    void **slots = n00b_os_map_zeroed(slot_bytes, _Alignof(void *));
    if (!slots) __builtin_trap();
    uint64_t depth = n00b_gc_root_depth();
    for (uint32_t i = 0; i < node_count; i++) n00b_gc_root_push(&slots[i]);
    for (uint32_t i = 0; i < node_count; i++) {
        const n00bb_literal_graph_node_t *node = &nodes[i];
        if (!node->source || !node->size || node->size > SIZE_MAX
            || (!node->type_index && !node->immutable)
            || !node->alignment || (node->alignment & (node->alignment - 1)))
            __builtin_trap();
        if (node->immutable) {
            slots[i] = (void *)node->source;
            continue;
        }
        slots[i] = n00b_allocate(N00B_ABI_ALLOCATOR_GC, node->type_index,
            node->size, node->alignment,
            node->instance_descriptor ? node->source : nullptr);
        memcpy(slots[i], node->source, (size_t)node->size);
    }
    for (uint32_t i = 0; i < edge_count; i++) {
        const n00bb_literal_graph_edge_t *edge = &edges[i];
        if (edge->source >= node_count || edge->target >= node_count
            || edge->source_offset > nodes[edge->source].size
            || sizeof(void *) > nodes[edge->source].size
                - edge->source_offset
            || edge->target_offset >= nodes[edge->target].size)
            __builtin_trap();
        void *target = (uint8_t *)slots[edge->target]
            + edge->target_offset;
        memcpy((uint8_t *)slots[edge->source] + edge->source_offset,
            &target, sizeof(target));
    }
    for (uint32_t i = 0; i < node_count; i++)
        if (!nodes[i].immutable)
            n00b_gc_barrier_value(slots[i], nodes[i].type_index);
    void *result = slots[root];
    n00b_gc_root_pop(depth);
    (void)n00b_os_unmap(slots, slot_bytes);
    return result;
}
