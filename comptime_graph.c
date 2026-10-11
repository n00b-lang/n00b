#include "comptime_graph.h"

#include "builtin_os.h"
#include "comptime_allocate.h"
#include "n00b-abi-v1.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
    uint32_t alignment;
    uint32_t type_index;
    bool scanned;
    bool immutable;
} graph_node_t;

typedef struct {
    uint64_t source;
    uint64_t source_offset;
    uint64_t target;
    uint64_t target_offset;
} graph_edge_t;

typedef struct {
    const n00b_abi_snapshot_v1 *snapshot;
    graph_node_t *nodes;
    size_t node_count;
    size_t node_capacity;
    graph_edge_t *edges;
    size_t edge_count;
    size_t edge_capacity;
} graph_t;

static bool
grow(void **items, size_t *capacity, size_t element_size)
{
    size_t next = *capacity ? *capacity * 2
        : n00b_os_page_size() / element_size;
    if (!next || next <= *capacity || next > SIZE_MAX / element_size)
        return false;
    size_t bytes = next * element_size;
    void *memory = n00b_os_map_zeroed(bytes, _Alignof(max_align_t));
    if (!memory) return false;
    if (*items) {
        memcpy(memory, *items, *capacity * element_size);
        (void)n00b_os_unmap(*items, *capacity * element_size);
    }
    *items = memory;
    *capacity = next;
    return true;
}

static bool
add_node(graph_t *graph, const uint8_t *bytes, size_t size,
         uint32_t alignment, uint32_t type_index, bool immutable,
         size_t *index)
{
    if (!bytes || !size || !alignment || (alignment & (alignment - 1))
        || graph->node_count == SIZE_MAX) return false;
    if (graph->node_count == graph->node_capacity
        && !grow((void **)&graph->nodes, &graph->node_capacity,
                 sizeof(*graph->nodes))) return false;
    *index = graph->node_count++;
    graph->nodes[*index] = (graph_node_t){
        .bytes = bytes, .size = size, .alignment = alignment,
        .type_index = type_index, .immutable = immutable,
    };
    return true;
}

static bool
add_edge(graph_t *graph, size_t source, size_t source_offset,
         size_t target, size_t target_offset)
{
    if (graph->edge_count == SIZE_MAX || source >= graph->node_count
        || target >= graph->node_count
        || source_offset > graph->nodes[source].size
        || sizeof(void *) > graph->nodes[source].size - source_offset
        || target_offset >= graph->nodes[target].size) return false;
    if (graph->edge_count == graph->edge_capacity
        && !grow((void **)&graph->edges, &graph->edge_capacity,
                 sizeof(*graph->edges))) return false;
    graph->edges[graph->edge_count++] = (graph_edge_t){
        .source = source, .source_offset = source_offset,
        .target = target, .target_offset = target_offset,
    };
    return true;
}

static bool
allocation_node(graph_t *graph, const void *address, size_t *index,
                size_t *offset)
{
    n00bb_comptime_allocation_t allocation;
    if (!n00bb_comptime_allocation_containing(address, &allocation))
        return false;
    for (size_t i = 1; i < graph->node_count; i++)
        if (graph->nodes[i].bytes == allocation.base) {
            *index = i;
            *offset = (uintptr_t)address - (uintptr_t)allocation.base;
            return true;
        }
    if (!add_node(graph, allocation.base, allocation.size,
                  allocation.alignment, allocation.type_index, false, index))
        return false;
    *offset = (uintptr_t)address - (uintptr_t)allocation.base;
    return true;
}

static bool scan_value(graph_t *graph, size_t node, size_t offset,
                       uint32_t type_index);

static bool
scan_string(graph_t *graph, size_t node, size_t offset)
{
    n00b_abi_string_v1 string;
    memcpy(&string, graph->nodes[node].bytes + offset, sizeof(string));
    uint64_t tag = string.long_value.selector >> N00B_ABI_STRING_TAG_SHIFT;
    if (tag == N00B_ABI_STRING_SHORT) return true;
    if (tag != N00B_ABI_STRING_LONG && tag != N00B_ABI_STRING_SPAN)
        return false;
    size_t length = (size_t)(string.long_value.selector
        & N00B_ABI_STRING_LONG_LENGTH_MASK);
    if (!string.long_value.data || !length
        || length != (string.long_value.selector
            & N00B_ABI_STRING_LONG_LENGTH_MASK)) return false;
    size_t target;
    return add_node(graph, string.long_value.data, length, 1, 0, true,
                    &target)
        && add_edge(graph, node, offset, target, 0);
}

static bool
scan_allocation(graph_t *graph, size_t index)
{
    graph_node_t *node = &graph->nodes[index];
    if (node->scanned) return true;
    node->scanned = true;
    const n00b_abi_type_v1 *type = n00b_abi_v1_type(graph->snapshot,
                                                     node->type_index);
    if (node->immutable) return true;
    if (!type) return false;
    if (type->kind == N00B_ABI_TYPE_ARRAY) return true;
    return scan_value(graph, index, 0, node->type_index);
}

static bool
scan_pointer(graph_t *graph, size_t node, size_t offset,
             const void *pointer)
{
    if (!pointer) return true;
    size_t target, target_offset;
    if (!allocation_node(graph, pointer, &target, &target_offset)
        || !add_edge(graph, node, offset, target, target_offset))
        return false;
    return scan_allocation(graph, target);
}

static bool
scan_children(graph_t *graph, size_t node, size_t offset,
              n00b_abi_range_v1 children)
{
    for (uint32_t i = 0; i < children.count; i++) {
        uint32_t child_index;
        if (n00b_abi_v1_index_at(graph->snapshot, children, i,
                                 &child_index) != 1) return false;
        uint32_t width = 0;
        const n00b_abi_trace_child_v1 *child = n00b_abi_v1_record(
            graph->snapshot, N00B_ABI_TABLE_TRACE_CHILD, child_index,
            &width);
        if (!child || width != sizeof(*child) || child->stride_bits % 8
            || child->byte_offset > SIZE_MAX
            || child->repeat_count > SIZE_MAX
            || (size_t)child->byte_offset > SIZE_MAX - offset) return false;
        size_t repeats = child->repeat_count ? child->repeat_count : 1;
        size_t stride = child->stride_bits / 8;
        for (size_t r = 0; r < repeats; r++) {
            if (stride && r > (SIZE_MAX - offset
                - (size_t)child->byte_offset) / stride) return false;
            size_t at = offset + (size_t)child->byte_offset + r * stride;
            if (!scan_value(graph, node, at, child->child_type_index))
                return false;
        }
    }
    return true;
}

static bool
scan_array(graph_t *graph, size_t node, size_t offset,
           uint32_t type_index)
{
    if (offset > graph->nodes[node].size
        || sizeof(n00b_abi_array_value_v1)
            > graph->nodes[node].size - offset) return false;
    n00b_abi_array_value_v1 value;
    memcpy(&value, graph->nodes[node].bytes + offset, sizeof(value));
    if (!value.data && !value.descriptor) return true;
    if (!value.data || !value.descriptor) return false;
    size_t data_node, data_offset, descriptor_node, descriptor_offset;
    if (!allocation_node(graph, value.data, &data_node, &data_offset)
        || !allocation_node(graph, value.descriptor,
                            &descriptor_node, &descriptor_offset)
        || !add_edge(graph, node, offset, data_node, data_offset)
        || !add_edge(graph, node, offset + sizeof(void *),
                     descriptor_node, descriptor_offset)) return false;
    if (descriptor_offset > graph->nodes[descriptor_node].size
        || sizeof(n00b_abi_array_descriptor_header_v1)
            > graph->nodes[descriptor_node].size - descriptor_offset)
        return false;
    const n00b_abi_array_descriptor_header_v1 *descriptor =
        value.descriptor;
    if (!descriptor->rank || !descriptor->flat_rank
        || descriptor->flat_rank > descriptor->rank
        || descriptor->rank > (graph->nodes[descriptor_node].size
            - descriptor_offset - sizeof(*descriptor)) / sizeof(uint64_t))
        return false;
    uint32_t element_index = type_index;
    for (uint32_t rank = 0; rank < descriptor->flat_rank; rank++) {
        const n00b_abi_type_v1 *array = n00b_abi_v1_type(graph->snapshot,
                                                          element_index);
        if (!array || array->kind != N00B_ABI_TYPE_ARRAY
            || n00b_abi_v1_index_at(graph->snapshot, array->components, 0,
                                    &element_index) != 1) return false;
    }
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(graph->snapshot,
                                                        element_index);
    if (!element || !element->alignment_bytes
        || (element->alignment_bytes & (element->alignment_bytes - 1))
        || element->size_bits > SIZE_MAX - 7) return false;
    size_t element_bytes = (size_t)((element->size_bits + 7) / 8);
    size_t alignment = element->alignment_bytes;
    if (element_bytes > SIZE_MAX - (alignment - 1)) return false;
    size_t stride = (element_bytes + alignment - 1) & ~(alignment - 1);
    if (!stride || descriptor->total_count
        > (graph->nodes[data_node].size - data_offset) / stride) return false;
    for (uint64_t i = 0; i < descriptor->total_count; i++)
        if (!scan_value(graph, data_node, data_offset + i * stride,
                        element_index)) return false;
    return true;
}

static bool
scan_value(graph_t *graph, size_t node, size_t offset,
           uint32_t type_index)
{
    const n00b_abi_type_v1 *type = n00b_abi_v1_type(graph->snapshot,
                                                     type_index);
    if (!type || type->size_bits > SIZE_MAX - 7) return false;
    size_t bytes = (size_t)((type->size_bits + 7) / 8);
    if (offset > graph->nodes[node].size
        || bytes > graph->nodes[node].size - offset) return false;
    if (type->kind == N00B_ABI_TYPE_STRING)
        return bytes == sizeof(n00b_abi_string_v1)
            && scan_string(graph, node, offset);
    if (!type->tracing_index) return true;
    switch (type->kind) {
    case N00B_ABI_TYPE_REFERENCE: {
        const void *pointer;
        if (bytes != sizeof(pointer)) return false;
        memcpy(&pointer, graph->nodes[node].bytes + offset, sizeof(pointer));
        return scan_pointer(graph, node, offset, pointer);
    }
    case N00B_ABI_TYPE_ARRAY:
        return scan_array(graph, node, offset, type_index);
    case N00B_ABI_TYPE_RECORD:
    case N00B_ABI_TYPE_TUPLE: {
        const n00b_abi_trace_v1 *trace = n00b_abi_v1_trace(
            graph->snapshot, type->tracing_index);
        return trace && scan_children(graph, node, offset,
                                      trace->fixed_children);
    }
    default:
        return false;
    }
}

static bool
write_bytes(int32_t fd, const void *source, size_t length)
{
    const uint8_t *bytes = source;
    while (length) {
        uint32_t chunk = length > INT32_MAX ? INT32_MAX : (uint32_t)length;
        n00b_abi_string_v1 text = {0};
        text.long_value.data = (uint8_t *)bytes;
        text.long_value.selector = (uint64_t)chunk
            | ((uint64_t)N00B_ABI_STRING_LONG << N00B_ABI_STRING_TAG_SHIFT);
        int32_t written = n00b_abi_v1_raw_fd_write(fd, text);
        if (written <= 0) return false;
        bytes += written;
        length -= (uint32_t)written;
    }
    return true;
}

int32_t
n00bb_comptime_emit_graph(int32_t fd, const void *root,
                          uint32_t root_type_index)
{
    graph_t graph = {.snapshot = n00b_abi_v1_current_snapshot()};
    const n00b_abi_type_v1 *type = graph.snapshot
        ? n00b_abi_v1_type(graph.snapshot, root_type_index) : nullptr;
    size_t root_node;
    bool valid = fd >= 0 && type && type->size_bits
        && type->size_bits <= SIZE_MAX - 7
        && add_node(&graph, root, (size_t)((type->size_bits + 7) / 8),
                    type->alignment_bytes, root_type_index, false, &root_node)
        && scan_value(&graph, root_node, 0, root_type_index);
    uint64_t node_count = graph.node_count;
    if (valid) valid = write_bytes(fd, &node_count, sizeof(node_count));
    for (size_t i = 0; valid && i < graph.node_count; i++) {
        const graph_node_t *node = &graph.nodes[i];
        const n00b_abi_type_v1 *row = n00b_abi_v1_type(graph.snapshot,
                                                        node->type_index);
        uint64_t address = i ? (uint64_t)(uintptr_t)node->bytes : 0;
        uint64_t size = node->size;
        uint64_t typehash = row ? row->typehash : 0;
        uint32_t flags = node->immutable ? 1 : 0;
        valid = (row || node->immutable)
            && write_bytes(fd, &address, sizeof(address))
            && write_bytes(fd, &size, sizeof(size))
            && write_bytes(fd, &node->alignment, sizeof(node->alignment))
            && write_bytes(fd, &typehash, sizeof(typehash))
            && write_bytes(fd, &flags, sizeof(flags))
            && write_bytes(fd, node->bytes, node->size);
    }
    uint64_t edge_count = graph.edge_count;
    if (valid) valid = write_bytes(fd, &edge_count, sizeof(edge_count));
    for (size_t i = 0; valid && i < graph.edge_count; i++)
        valid = write_bytes(fd, &graph.edges[i], sizeof(graph.edges[i]));
    if (graph.edges)
        (void)n00b_os_unmap(graph.edges,
                            graph.edge_capacity * sizeof(*graph.edges));
    if (graph.nodes)
        (void)n00b_os_unmap(graph.nodes,
                            graph.node_capacity * sizeof(*graph.nodes));
    return valid ? 0 : -1;
}
