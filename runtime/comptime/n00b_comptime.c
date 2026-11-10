#include "builtin_abi.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

enum {
    COMPTIME_NO_TARGET = 70,
    COMPTIME_TARGET_NOT_REACHED = 71,
    COMPTIME_OUTPUT_FAILED = 72,
    HEAP_ALIGNMENT = 16,
};

extern bool n00b_comptime;
extern void (*n00b_comptime_exit)(int64_t code);
int __n00bb_main(int argc, char **argv);
void n00b_comptime_freeze(uint64_t module);
bool n00b_comptime_frozen(uint64_t module);
uint64_t n00b_gc_object_payload(uint64_t address);
uint64_t n00b_gc_object_bytes(uint64_t address);
uint32_t n00b_gc_object_type(uint64_t address);
bool n00b_gc_object_is_array_backing(uint64_t address);
void *n00b_gc_allocate(uint32_t type_index, uint64_t byte_size,
                       uint32_t alignment, const void *instance_descriptor);
uint64_t n00b_gc_root_depth(void);
void n00b_gc_root_push(void *slot);
void n00b_gc_root_pop(uint64_t depth);
void n00b_gc_barrier_value(void *address, uint32_t type_index);
void n00b_os_set_thread_seed_hooks(void *(*spawn)(void),
                                   void (*seed)(void *seed));

typedef struct {
    uint8_t *base;
    uint64_t size;
    uint32_t alignment;
    uint32_t type_index;
    uint32_t flags;
    int64_t parent;
} node_t;

typedef struct {
    uint32_t source;
    uint64_t source_offset;
    uint32_t target;
    uint64_t target_offset;
} edge_t;

typedef struct {
    uint32_t node;
    uint64_t offset;
    uint32_t table;
    uint32_t kind;
    uint32_t index;
    const n00b_abi_string_v1 *native;
    uint32_t ordinal;
    uint64_t addend;
} slot_t;

enum {
    EXTERNAL_STORAGE = 1,
    EXTERNAL_BAKED = 2,
    EXTERNAL_FUNCTION = 3,
};

typedef struct {
    const n00b_abi_string_v1 *module;
    const n00b_abi_string_v1 *name;
    uint32_t node;
} root_t;

typedef struct {
    void *data;
    size_t count;
    size_t capacity;
} list_t;

typedef struct {
    const n00b_abi_snapshot_v1 *snapshot;
    list_t nodes;
    list_t edges;
    list_t slots;
    list_t roots;
    list_t order;
    list_t pending;
    const char *failure;
    uint32_t failure_node;
    uint32_t module_row;
} graph_t;

static uint64_t target;
static const char *output_path;
static int output = -1;
static bool entry;
static bool exiting;

#define AT(list, type, i) (((type *)(list).data)[i])

static void *
push(list_t *list, size_t size)
{
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        void *grown = realloc(list->data, capacity * size);
        if (!grown) _exit(COMPTIME_OUTPUT_FAILED);
        list->data = grown;
        list->capacity = capacity;
    }
    return (uint8_t *)list->data + size * list->count++;
}

static bool
parse_key(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    size_t digits = 0;
    for (; text && text[digits]; digits++) {
        char c = text[digits];
        uint64_t digit = c >= '0' && c <= '9' ? (uint64_t)(c - '0')
            : c >= 'a' && c <= 'f' ? (uint64_t)(c - 'a' + 10)
            : 16;
        if (digit > 15 || digits >= 16) return false;
        value = value << 4 | digit;
    }
    *out = value;
    return digits != 0;
}

static uint64_t
module_key(const uint8_t *name, uint64_t count)
{
    uint64_t key = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < count; i++)
        key = (key ^ name[i]) * 0x100000001b3ull;
    return key;
}

static bool
put(const void *bytes, uint64_t count)
{
    const uint8_t *at = bytes;
    while (count) {
        ssize_t written = write(output, at, count);
        if (written <= 0) return false;
        at += written;
        count -= (uint64_t)written;
    }
    return true;
}

static bool
put_word(uint64_t value, uint32_t bytes)
{
    return put(&value, bytes);
}

static bool
put_abi_string(const graph_t *graph, const n00b_abi_string_v1 *text)
{
    const uint8_t *bytes = NULL;
    uint64_t count = 0;
    return text && n00b_abi_v1_string_bytes(graph->snapshot, text, &bytes,
                                             &count) == 1
        && put_word(count, 8) && put(bytes, count);
}

static const n00b_abi_type_v1 *
type_row(uint32_t index)
{
    return index ? n00b_builtin_abi_type(index) : NULL;
}

static uint64_t
size_bytes(const n00b_abi_type_v1 *row)
{
    return row ? (row->size_bits + 7) / 8 : 0;
}

static int64_t
range_index(n00b_abi_range_v1 range, uint32_t ordinal)
{
    return n00b_builtin_abi_range_index(range.first, range.count, ordinal);
}

static uint64_t
unit_at(const uint8_t *address, uint32_t bits)
{
    switch (bits) {
    case 8: return *(const uint8_t *)address;
    case 16: return *(const uint16_t *)(const void *)address;
    case 32: return *(const uint32_t *)(const void *)address;
    case 64: return *(const uint64_t *)(const void *)address;
    default: return 0;
    }
}

static void *
pointer_at(const uint8_t *address)
{
    return *(void *const *)(const void *)address;
}

static void
fail(graph_t *graph, uint32_t node, const char *message)
{
    if (graph->failure) return;
    graph->failure = message;
    graph->failure_node = node;
}

static size_t
order_position(graph_t *graph, uintptr_t address)
{
    size_t low = 0, high = graph->order.count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        node_t *node = &AT(graph->nodes, node_t,
                           AT(graph->order, uint32_t, middle));
        if ((uintptr_t)node->base <= address) low = middle + 1;
        else high = middle;
    }
    return low;
}

static int64_t
node_containing(graph_t *graph, uintptr_t address)
{
    size_t at = order_position(graph, address);
    if (!at) return -1;
    uint32_t index = AT(graph->order, uint32_t, at - 1);
    node_t *node = &AT(graph->nodes, node_t, index);
    return address < (uintptr_t)node->base + node->size
        || (node->size == 0 && address == (uintptr_t)node->base)
        ? (int64_t)index : -1;
}

static uint32_t
add_node(graph_t *graph, uint8_t *base, uint64_t size, uint32_t alignment,
         uint32_t type_index, uint32_t flags, bool trace, int64_t parent)
{
    uint32_t index = (uint32_t)graph->nodes.count;
    *(node_t *)push(&graph->nodes, sizeof(node_t)) = (node_t){
        .base = base, .size = size, .alignment = alignment ? alignment : 1,
        .type_index = type_index, .flags = flags, .parent = parent,
    };
    size_t at = order_position(graph, (uintptr_t)base);
    push(&graph->order, sizeof(uint32_t));
    uint32_t *order = graph->order.data;
    for (size_t i = graph->order.count - 1; i > at; i--)
        order[i] = order[i - 1];
    order[at] = index;
    if (trace) *(uint32_t *)push(&graph->pending, sizeof(uint32_t)) = index;
    return index;
}

static int64_t
heap_node(graph_t *graph, uintptr_t address, uint32_t parent)
{
    uint64_t payload = n00b_gc_object_payload(address);
    if (!payload) return -1;
    return add_node(graph, (uint8_t *)(uintptr_t)payload,
        n00b_gc_object_bytes(address), HEAP_ALIGNMENT,
        n00b_gc_object_type(address),
        n00b_gc_object_is_array_backing(address)
            ? N00B_ABI_BAKED_NODE_ARRAY_BACKING : 0,
        true, parent);
}

static int64_t
baked_node(graph_t *graph, uintptr_t address, uint32_t parent)
{
    uint32_t count = n00b_builtin_abi_baked_node_count();
    for (uint32_t i = 1; i < count; i++) {
        const n00b_abi_baked_node_v1 *row = n00b_builtin_abi_baked_node(i);
        const uint8_t *base = n00b_builtin_abi_baked_node_address(i);
        if (!row || !base || row->module_index != graph->module_row
            || address < (uintptr_t)base
            || address >= (uintptr_t)base + row->byte_length) continue;
        const n00b_abi_type_v1 *type = type_row(row->type_index);
        return add_node(graph, (uint8_t *)base, row->byte_length,
            type ? type->alignment_bytes : 1, row->type_index, row->flags,
            true, parent);
    }
    return -1;
}

static const n00b_abi_native_binding_v1 *
native_binding(uint32_t index)
{
    const uint8_t *base = n00b_builtin_abi_table_base(
        N00B_ABI_TABLE_NATIVE_BINDING);
    uint32_t bytes = n00b_builtin_abi_table_record_bytes(
        N00B_ABI_TABLE_NATIVE_BINDING);
    return base && bytes && index
        && index < n00b_builtin_abi_table_count(N00B_ABI_TABLE_NATIVE_BINDING)
        ? (const n00b_abi_native_binding_v1 *)(const void *)
              (base + (size_t)index * bytes)
        : NULL;
}

static bool
external_target(graph_t *graph, uint32_t source, const uint8_t *slot,
                uintptr_t address)
{
    uint32_t count = n00b_builtin_abi_storage_count();
    for (uint32_t i = 1; i < count; i++) {
        const n00b_abi_storage_v1 *row = n00b_builtin_abi_storage(i);
        if (!row || row->module_index == graph->module_row
            || row->segment_kind == N00B_ABI_STORAGE_TLS) continue;
        const uint8_t *base = n00b_builtin_abi_storage_address(i);
        if (!base || address < (uintptr_t)base
            || address >= (uintptr_t)base + row->byte_length) continue;
        const n00b_abi_symbol_v1 *symbol = n00b_builtin_abi_symbol(
            row->symbol_index);
        const n00b_abi_native_binding_v1 *binding = symbol
            ? native_binding(symbol->native_binding_index) : NULL;
        if (!binding) return false;
        node_t *node = &AT(graph->nodes, node_t, source);
        *(slot_t *)push(&graph->slots, sizeof(slot_t)) = (slot_t){
            .node = source, .offset = (uint64_t)(slot - node->base),
            .table = N00B_ABI_TABLE_NATIVE_BINDING, .kind = EXTERNAL_STORAGE,
            .native = &binding->native_name,
            .addend = address - (uintptr_t)base,
        };
        return true;
    }
    count = n00b_builtin_abi_baked_node_count();
    for (uint32_t i = 1; i < count; i++) {
        const n00b_abi_baked_node_v1 *row = n00b_builtin_abi_baked_node(i);
        const uint8_t *base = n00b_builtin_abi_baked_node_address(i);
        if (!row || !base || row->module_index == graph->module_row
            || address < (uintptr_t)base
            || address >= (uintptr_t)base + row->byte_length) continue;
        uint32_t ordinal = 1;
        for (uint32_t j = 1; j < i; j++) {
            const n00b_abi_baked_node_v1 *earlier =
                n00b_builtin_abi_baked_node(j);
            ordinal += earlier && earlier->module_index == row->module_index;
        }
        node_t *node = &AT(graph->nodes, node_t, source);
        *(slot_t *)push(&graph->slots, sizeof(slot_t)) = (slot_t){
            .node = source, .offset = (uint64_t)(slot - node->base),
            .table = N00B_ABI_TABLE_NATIVE_BINDING, .kind = EXTERNAL_BAKED,
            .index = row->module_index, .ordinal = ordinal,
            .addend = address - (uintptr_t)base,
        };
        return true;
    }
    return false;
}


static void
add_edge(graph_t *graph, uint32_t source, const uint8_t *slot,
         uint32_t target_index, const void *pointer)
{
    node_t *from = &AT(graph->nodes, node_t, source);
    node_t *to = &AT(graph->nodes, node_t, target_index);
    *(edge_t *)push(&graph->edges, sizeof(edge_t)) = (edge_t){
        .source = source,
        .source_offset = (uint64_t)(slot - from->base),
        .target = target_index,
        .target_offset = (uint64_t)((const uint8_t *)pointer - to->base),
    };
}

static void
add_slot(graph_t *graph, uint32_t source, const uint8_t *address,
         uint32_t table, uint32_t kind, uint32_t index)
{
    node_t *node = &AT(graph->nodes, node_t, source);
    *(slot_t *)push(&graph->slots, sizeof(slot_t)) = (slot_t){
        .node = source, .offset = (uint64_t)(address - node->base),
        .table = table, .kind = kind, .index = index,
    };
}

static void visit_value(graph_t *graph, uint32_t source, uint8_t *address,
                        uint32_t type_index);

static void
reach(graph_t *graph, uint32_t source, uint8_t *slot, void *pointer,
      bool foreign)
{
    if (!pointer) return;
    int64_t found = node_containing(graph, (uintptr_t)pointer);
    if (found < 0) found = heap_node(graph, (uintptr_t)pointer, source);
    if (found < 0 && external_target(graph, source, slot,
                                     (uintptr_t)pointer)) return;
    if (found < 0) found = baked_node(graph, (uintptr_t)pointer, source);
    if (found < 0 && foreign) {
        fail(graph, source, "points to foreign memory, which cannot be frozen");
        return;
    }
    uint32_t function = found < 0
        ? n00b_abi_v1_runtime_function_index(pointer) : 0;
    if (function) {
        node_t *node = &AT(graph->nodes, node_t, source);
        *(slot_t *)push(&graph->slots, sizeof(slot_t)) = (slot_t){
            .node = source, .offset = (uint64_t)(slot - node->base),
            .table = N00B_ABI_TABLE_NATIVE_BINDING, .kind = EXTERNAL_FUNCTION,
            .index = function,
        };
        return;
    }
    if (found < 0) {
        fail(graph, source, "points to memory that is not a heap object");
        return;
    }
    add_edge(graph, source, slot, (uint32_t)found, pointer);
}

static void
visit_pointer(graph_t *graph, uint32_t source, uint8_t *slot, void *pointer)
{
    reach(graph, source, slot, pointer, false);
}

static void
visit_box(graph_t *graph, uint32_t source, uint8_t *address)
{
    const n00b_abi_box_v1 *box = (const n00b_abi_box_v1 *)(void *)address;
    uint32_t item_index = box->type_field & 0x7fffffffu;
    if (!item_index) return;
    add_slot(graph, source, address + 4, N00B_ABI_TABLE_TYPE,
             N00B_ABI_PATCH_BOX_TYPE, item_index);
    const n00b_abi_type_v1 *item = type_row(item_index);
    if (item && item->size_bits > 64)
        visit_pointer(graph, source, address + 8, box->payload.out_of_line);
    else visit_value(graph, source, address + 8, item_index);
}

static void
visit_callback(graph_t *graph, uint32_t source, uint8_t *address)
{
    const n00b_abi_callback_value_v1 *handler =
        (const n00b_abi_callback_value_v1 *)(void *)address;
    if (handler->handler_function_index)
        add_slot(graph, source, address, N00B_ABI_TABLE_FUNCTION,
                 N00B_ABI_PATCH_INDEX, handler->handler_function_index);
    add_slot(graph, source, address + 4, N00B_ABI_TABLE_TYPE,
             N00B_ABI_PATCH_INDEX, handler->callback_type_index);
    visit_pointer(graph, source, address + 8, handler->environment);
}

static uint32_t
foreign_target(uint32_t type_index)
{
    const n00b_abi_type_v1 *row = type_row(type_index);
    if (!row) return 0;
    if (row->kind == N00B_ABI_TYPE_FOREIGN) return type_index;
    if (row->kind != N00B_ABI_TYPE_OPTION) return 0;
    int64_t inner = range_index(row->components, 0);
    const n00b_abi_type_v1 *item = inner > 0 ? type_row((uint32_t)inner)
                                             : NULL;
    return item && item->kind == N00B_ABI_TYPE_FOREIGN ? (uint32_t)inner : 0;
}

static void visit_children(graph_t *graph, uint32_t source, uint8_t *address,
                           n00b_abi_range_v1 children);

static void
visit_arm(graph_t *graph, uint32_t source, uint8_t *address,
          const n00b_abi_trace_v1 *trace, bool unset_is_empty)
{
    const n00b_abi_tag_extractor_v1 *tag =
        n00b_builtin_abi_tag_extractor(trace->tag_extractor_index);
    if (!tag) return;
    uint64_t active = (unit_at(address + tag->unit_byte_offset,
                               tag->unit_width_bits) & tag->mask)
        >> tag->lsb_position;
    if (unset_is_empty && !active) return;
    for (uint32_t i = 0; i < trace->arms.count; i++) {
        int64_t index = range_index(trace->arms, i);
        const n00b_abi_trace_arm_v1 *arm = index > 0
            ? n00b_builtin_abi_trace_arm((uint32_t)index) : NULL;
        if (arm && arm->tag_value == active) {
            visit_children(graph, source, address, arm->children);
            return;
        }
    }
}

static void
visit_children(graph_t *graph, uint32_t source, uint8_t *address,
               n00b_abi_range_v1 children)
{
    for (uint32_t i = 0; i < children.count; i++) {
        int64_t index = range_index(children, i);
        const n00b_abi_trace_child_v1 *child = index > 0
            ? n00b_builtin_abi_trace_child((uint32_t)index) : NULL;
        if (!child) continue;
        uint64_t count = child->repeat_count ? child->repeat_count : 1;
        bool foreign = foreign_target(child->child_type_index) != 0;
        for (uint64_t r = 0; r < count; r++) {
            uint8_t *at = address + child->byte_offset
                + r * (child->stride_bits / 8);
            if (foreign) reach(graph, source, at, pointer_at(at), true);
            else visit_value(graph, source, at, child->child_type_index);
        }
    }
}

static uint32_t
flat_element(uint32_t type_index, uint32_t flat_rank)
{
    uint32_t current = type_index;
    for (uint32_t level = 0; level < flat_rank; level++) {
        const n00b_abi_type_v1 *row = type_row(current);
        int64_t component = row && row->kind == N00B_ABI_TYPE_ARRAY
            ? range_index(row->components, 0) : -1;
        if (component < 0) return 0;
        current = (uint32_t)component;
    }
    return current;
}

static uint64_t
element_stride(const n00b_abi_type_v1 *element)
{
    uint64_t alignment = element->alignment_bytes ? element->alignment_bytes
                                                  : 1;
    return (size_bytes(element) + alignment - 1) / alignment * alignment;
}

static void
visit_elements(graph_t *graph, uint32_t source, uint8_t *data,
               const n00b_abi_array_descriptor_header_v1 *shape,
               uint32_t type_index)
{
    uint32_t element_index = flat_element(type_index, shape->flat_rank);
    const n00b_abi_type_v1 *element = type_row(element_index);
    if (!element || !element->tracing_index) return;
    uint64_t stride = element_stride(element);
    for (uint64_t i = 0; i < shape->total_count; i++)
        visit_value(graph, source, data + i * stride, element_index);
}

static uint64_t
descriptor_bytes(const n00b_abi_array_descriptor_header_v1 *shape)
{
    return sizeof(*shape) + (uint64_t)shape->rank * 8;
}

static uint64_t
backing_offset(uint32_t type_index,
               const n00b_abi_array_descriptor_header_v1 *shape)
{
    const n00b_abi_type_v1 *element = type_row(flat_element(type_index,
                                                            shape->flat_rank));
    if (!element || !shape->rank || !shape->flat_rank) return 0;
    uint64_t mask = element->alignment_bytes ? element->alignment_bytes - 1
                                             : 0;
    uint64_t data_mask = mask > 7 ? mask : 7;
    return (descriptor_bytes(shape) + data_mask) & ~data_mask;
}

static uint32_t
static_node(graph_t *graph, uint8_t *base, uint64_t size, uint32_t alignment,
            uint32_t parent)
{
    int64_t found = node_containing(graph, (uintptr_t)base);
    return found >= 0 ? (uint32_t)found
        : add_node(graph, base, size, alignment, 0,
                   N00B_ABI_BAKED_NODE_IMMUTABLE, false, parent);
}

static void
visit_shape(graph_t *graph, uint32_t source, uint8_t *slot,
            const n00b_abi_array_descriptor_header_v1 *shape)
{
    uintptr_t at = (uintptr_t)shape;
    if (node_containing(graph, at) >= 0 || n00b_gc_object_payload(at)
        || n00b_abi_v1_runtime_function_index((void *)shape)) {
        visit_pointer(graph, source, slot, (void *)shape);
        return;
    }
    uint32_t node = static_node(graph, (uint8_t *)shape,
                                descriptor_bytes(shape), 8, source);
    add_edge(graph, source, slot, node, (void *)shape);
}

static void
visit_array(graph_t *graph, uint32_t source, uint8_t *address,
            uint32_t type_index)
{
    const n00b_abi_array_value_v1 *value =
        (const n00b_abi_array_value_v1 *)(void *)address;
    uint8_t *data = value->data;
    const n00b_abi_array_descriptor_header_v1 *shape = value->descriptor;
    if (!data || !shape) return;
    int64_t holder = node_containing(graph, (uintptr_t)data);
    if (holder >= 0 || n00b_gc_object_payload((uintptr_t)data)
        || external_target(graph, source, address, (uintptr_t)data)) {
        if (holder >= 0 || n00b_gc_object_payload((uintptr_t)data))
            visit_pointer(graph, source, address, data);
        visit_shape(graph, source, address + 8, shape);
        if (holder == (int64_t)source)
            visit_elements(graph, source, data, shape, type_index);
        return;
    }
    if (baked_node(graph, (uintptr_t)data, source) >= 0) {
        visit_pointer(graph, source, address, data);
        visit_shape(graph, source, address + 8, shape);
        return;
    }
    const n00b_abi_type_v1 *element = type_row(flat_element(type_index,
                                                            shape->flat_rank));
    if (!element) {
        fail(graph, source, "is an array whose elements have no description");
        return;
    }
    uint64_t stride = element_stride(element);
    uint32_t data_node = static_node(graph, data, shape->total_count * stride,
                                     element->alignment_bytes, source);
    uint32_t shape_node = static_node(graph, (uint8_t *)shape,
                                      descriptor_bytes(shape), 8, source);
    add_edge(graph, source, address, data_node, data);
    add_edge(graph, source, address + 8, shape_node, shape);
    visit_elements(graph, data_node, data, shape, type_index);
}

static void
visit_string(graph_t *graph, uint32_t source, uint8_t *address)
{
    const n00b_abi_string_v1 *value =
        (const n00b_abi_string_v1 *)(void *)address;
    uint64_t tag = value->words[1] >> N00B_ABI_STRING_TAG_SHIFT;
    uint8_t *backing = value->long_value.data;
    if (!tag || !backing) return;
    if (tag == 3) {
        visit_pointer(graph, source, address, backing);
        return;
    }
    int64_t found = node_containing(graph, (uintptr_t)backing);
    if (found < 0) found = heap_node(graph, (uintptr_t)backing, source);
    if (found < 0 && external_target(graph, source, address,
                                     (uintptr_t)backing)) return;
    if (found < 0) found = baked_node(graph, (uintptr_t)backing, source);
    if (found < 0)
        found = add_node(graph, backing,
            value->words[1] & 0x0fffffffffffffffull, 1, 0,
            N00B_ABI_BAKED_NODE_IMMUTABLE, false, source);
    add_edge(graph, source, address, (uint32_t)found, backing);
}

static bool
option_set(uint8_t *address, const n00b_abi_trace_v1 *trace,
           uint32_t type_index)
{
    const n00b_abi_tag_extractor_v1 *tag =
        n00b_builtin_abi_tag_extractor(trace->tag_extractor_index);
    if (!tag) return false;
    uint8_t *unit = address + tag->unit_byte_offset;
    if (tag->kind == N00B_ABI_TAG_DIRECT_BITS)
        return (unit_at(unit, tag->unit_width_bits) & tag->mask) != 0;
    if (tag->kind == N00B_ABI_TAG_POINTER_NULL)
        return pointer_at(unit) != NULL;
    if (pointer_at(address)) return true;
    uint64_t depth = 0;
    for (uint32_t current = type_index;;) {
        const n00b_abi_type_v1 *row = type_row(current);
        if (!row || row->kind != N00B_ABI_TYPE_OPTION) break;
        depth++;
        int64_t child = range_index(row->components, 0);
        if (child <= 0) break;
        current = (uint32_t)child;
    }
    uint64_t code = unit_at(unit, tag->unit_width_bits);
    return depth && code < depth - 1;
}

static void
visit_value(graph_t *graph, uint32_t source, uint8_t *address,
            uint32_t type_index)
{
    const n00b_abi_type_v1 *row = type_row(type_index);
    if (!row || !row->tracing_index || graph->failure) return;
    if (foreign_target(type_index)) {
        reach(graph, source, address, pointer_at(address), true);
        return;
    }
    if (row->flags & N00B_ABI_TYPE_FLAG_TYPE_INDEX) {
        add_slot(graph, source, address, N00B_ABI_TABLE_TYPE,
                 N00B_ABI_PATCH_INDEX,
                 (uint32_t)unit_at(address, 32));
        return;
    }
    switch (row->kind) {
    case N00B_ABI_TYPE_BOX:
    case N00B_ABI_TYPE_UNION:
        visit_box(graph, source, address);
        return;
    case N00B_ABI_TYPE_CALLBACK:
        visit_callback(graph, source, address);
        return;
    case N00B_ABI_TYPE_ARRAY:
        visit_array(graph, source, address, type_index);
        return;
    case N00B_ABI_TYPE_STRING:
        visit_string(graph, source, address);
        return;
    case N00B_ABI_TYPE_REFERENCE:
    case N00B_ABI_TYPE_OBJECT:
        visit_pointer(graph, source, address, pointer_at(address));
        return;
    default:
        break;
    }
    const n00b_abi_trace_v1 *trace = n00b_builtin_abi_trace(
        row->tracing_index);
    if (!trace) return;
    switch (row->kind) {
    case N00B_ABI_TYPE_RECORD:
    case N00B_ABI_TYPE_TUPLE:
        visit_children(graph, source, address, trace->fixed_children);
        if (trace->tag_extractor_index)
            visit_arm(graph, source, address, trace, false);
        return;
    case N00B_ABI_TYPE_VARIANT:
        visit_arm(graph, source, address, trace, true);
        return;
    case N00B_ABI_TYPE_OPTION:
    case N00B_ABI_TYPE_WRAPPER:
    case N00B_ABI_TYPE_ATOMIC: {
        if (row->kind == N00B_ABI_TYPE_OPTION
            && !option_set(address, trace, type_index)) return;
        const n00b_abi_trace_v1 *child = n00b_builtin_abi_trace(
            trace->child_trace_index);
        if (child) visit_value(graph, source, address, child->type_index);
        return;
    }
    default:
        fail(graph, source, "has a value whose layout cannot be traced");
    }
}

static void
trace_node(graph_t *graph, uint32_t index)
{
    node_t node = AT(graph->nodes, node_t, index);
    if (node.flags & N00B_ABI_BAKED_NODE_ARRAY_BACKING) {
        const n00b_abi_array_descriptor_header_v1 *shape =
            (const n00b_abi_array_descriptor_header_v1 *)(void *)node.base;
        visit_elements(graph, index,
                       node.base + backing_offset(node.type_index, shape),
                       shape, node.type_index);
        return;
    }
    visit_value(graph, index, node.base, node.type_index);
}

static bool
put_type_name(const graph_t *graph, uint32_t type_index)
{
    const n00b_abi_type_v1 *row = type_row(type_index);
    return row ? put_abi_string(graph, &row->canonical_name)
               : put_word(0, 8);
}

static bool
put_slot(const graph_t *graph, const slot_t *slot)
{
    bool ok = put_word(slot->node, 4) && put_word(slot->offset, 8)
        && put_word(slot->table, 4) && put_word(slot->kind, 4);
    if (!ok) return false;
    if (slot->table == N00B_ABI_TABLE_NATIVE_BINDING
        && slot->kind == EXTERNAL_STORAGE)
        return put_abi_string(graph, slot->native)
            && put_word(slot->addend, 8);
    if (slot->table == N00B_ABI_TABLE_NATIVE_BINDING
        && slot->kind == EXTERNAL_FUNCTION) {
        const n00b_abi_function_v1 *function = n00b_builtin_abi_function(
            slot->index);
        const n00b_abi_module_v1 *module = function
            ? n00b_builtin_abi_module(function->module_index) : NULL;
        return module && put_abi_string(graph, &module->full_path)
            && put_abi_string(graph, &function->name);
    }
    if (slot->table == N00B_ABI_TABLE_NATIVE_BINDING) {
        const n00b_abi_module_v1 *owner = n00b_builtin_abi_module(
            slot->index);
        return owner && put_abi_string(graph, &owner->full_path)
            && put_word(slot->ordinal, 4) && put_word(slot->addend, 8);
    }
    if (slot->table == N00B_ABI_TABLE_TYPE) {
        const n00b_abi_type_v1 *row = type_row(slot->index);
        return row && put_word(row->typehash, 8)
            && put_type_name(graph, slot->index);
    }
    const n00b_abi_function_v1 *function = n00b_builtin_abi_function(
        slot->index);
    const n00b_abi_module_v1 *module = function
        ? n00b_builtin_abi_module(function->module_index) : NULL;
    return module && put_abi_string(graph, &module->full_path)
        && put_abi_string(graph, &function->name);
}

static bool
put_graph(const graph_t *graph, const n00b_abi_module_v1 *module)
{
    bool ok = put_abi_string(graph, &module->full_path)
        && put_word(graph->nodes.count, 8);
    for (size_t i = 0; ok && i < graph->nodes.count; i++) {
        const node_t *node = &AT(graph->nodes, node_t, i);
        const n00b_abi_type_v1 *row = type_row(node->type_index);
        ok = put_word(node->size, 8) && put_word(node->alignment, 4)
            && put_word(row ? row->typehash : 0, 8)
            && put_type_name(graph, node->type_index)
            && put_word(node->flags, 4) && put(node->base, node->size);
    }
    ok = ok && put_word(graph->edges.count, 8);
    for (size_t i = 0; ok && i < graph->edges.count; i++) {
        const edge_t *edge = &AT(graph->edges, edge_t, i);
        ok = put_word(edge->source, 4) && put_word(edge->source_offset, 8)
            && put_word(edge->target, 4) && put_word(edge->target_offset, 8);
    }
    ok = ok && put_word(graph->roots.count, 8);
    for (size_t i = 0; ok && i < graph->roots.count; i++) {
        const root_t *root = &AT(graph->roots, root_t, i);
        ok = put_abi_string(graph, root->module)
            && put_abi_string(graph, root->name)
            && put_word(root->node, 4);
    }
    ok = ok && put_word(graph->slots.count, 8);
    for (size_t i = 0; ok && i < graph->slots.count; i++)
        ok = put_slot(graph, &AT(graph->slots, slot_t, i));
    return ok;
}

static bool
put_failure(const graph_t *graph)
{
    uint32_t node = graph->failure_node;
    while (AT(graph->nodes, node_t, node).parent >= 0)
        node = (uint32_t)AT(graph->nodes, node_t, node).parent;
    const root_t *root = NULL;
    for (size_t i = 0; i < graph->roots.count; i++)
        if (AT(graph->roots, root_t, i).node == node)
            root = &AT(graph->roots, root_t, i);
    uint64_t length = 0;
    while (graph->failure[length]) length++;
    return root && put("N00BCTFL", 8) && put_abi_string(graph, root->module)
        && put_abi_string(graph, root->name)
        && put_word(length, 8) && put(graph->failure, length);
}

static void
release_graph(graph_t *graph)
{
    list_t *lists[] = {&graph->nodes, &graph->edges, &graph->slots,
                       &graph->roots, &graph->order, &graph->pending};
    for (size_t i = 0; i < sizeof(lists) / sizeof(*lists); i++)
        free(lists[i]->data);
}

static void
drain(graph_t *graph)
{
    while (graph->pending.count && !graph->failure) {
        uint32_t index = AT(graph->pending, uint32_t, --graph->pending.count);
        trace_node(graph, index);
    }
}

static bool
add_storage(graph_t *graph, const n00b_abi_module_v1 *module,
            n00b_abi_range_v1 range)
{
    for (uint32_t i = 0; i < range.count; i++) {
        uint32_t index = 0;
        if (n00b_abi_v1_index_at(graph->snapshot, range, i, &index) != 1)
            return false;
        const n00b_abi_storage_v1 *storage = n00b_abi_v1_storage(
            graph->snapshot, index);
        if (storage && !storage->symbol_index) continue;
        const n00b_abi_symbol_v1 *symbol = storage
            ? n00b_abi_v1_symbol(graph->snapshot, storage->symbol_index)
            : NULL;
        uint8_t *address = (uint8_t *)n00b_builtin_abi_storage_address(index);
        if (!symbol || !address) return false;
        const n00b_abi_type_v1 *row = type_row(storage->type_index);
        uint32_t node = add_node(graph, address, storage->byte_length,
            row ? row->alignment_bytes : 1, storage->type_index, 0, true, -1);
        *(root_t *)push(&graph->roots, sizeof(root_t)) = (root_t){
            .module = &module->full_path, .name = &symbol->name, .node = node,
        };
    }
    return true;
}

typedef struct {
    uint32_t module;
    uint32_t storage;
    uint64_t digest;
} settled_t;

static list_t settled;
static bool keep_imports;

static uint32_t
module_row(const n00b_abi_snapshot_v1 *snapshot, uint64_t key)
{
    uint32_t count = n00b_builtin_abi_table_count(N00B_ABI_TABLE_MODULE);
    for (uint32_t i = 1; i < count; i++) {
        const n00b_abi_module_v1 *row = n00b_abi_v1_module(snapshot, i);
        const uint8_t *name = NULL;
        uint64_t name_bytes = 0;
        if (row && n00b_abi_v1_string_bytes(snapshot, &row->full_path, &name,
                                            &name_bytes) == 1
            && module_key(name, name_bytes) == key) return i;
    }
    return 0;
}

static uint64_t
storage_digest(const n00b_abi_snapshot_v1 *snapshot, uint32_t module,
               uint32_t storage)
{
    graph_t graph = {.snapshot = snapshot, .module_row = module};
    uint64_t digest = 0xcbf29ce484222325ull;
    {
        const n00b_abi_storage_v1 *row = n00b_abi_v1_storage(snapshot,
                                                              storage);
        uint8_t *address = (uint8_t *)n00b_builtin_abi_storage_address(
            storage);
        const n00b_abi_type_v1 *type = row ? type_row(row->type_index) : NULL;
        if (row && address)
            add_node(&graph, address, row->byte_length,
                     type ? type->alignment_bytes : 1, row->type_index, 0,
                     true, -1);
        drain(&graph);
    }
    for (size_t i = 0; i < graph.nodes.count; i++) {
        const node_t *node = &AT(graph.nodes, node_t, i);
        for (uint64_t b = 0; b < node->size; b++)
            digest = (digest ^ node->base[b]) * 0x100000001b3ull;
    }
    release_graph(&graph);
    return digest;
}

static void
each_storage(const n00b_abi_snapshot_v1 *snapshot, uint32_t module,
             void (*visit)(const n00b_abi_snapshot_v1 *, uint32_t, uint32_t))
{
    const n00b_abi_module_v1 *row = n00b_abi_v1_module(snapshot, module);
    n00b_abi_range_v1 ranges[] = {row->static_storage, row->tls_storage};
    for (size_t r = 0; r < 2; r++)
        for (uint32_t i = 0; i < ranges[r].count; i++) {
            uint32_t index = 0;
            if (n00b_abi_v1_index_at(snapshot, ranges[r], i, &index) == 1)
                visit(snapshot, module, index);
        }
}

static void
settle(const n00b_abi_snapshot_v1 *snapshot, uint32_t module,
       uint32_t storage)
{
    *(settled_t *)push(&settled, sizeof(settled_t)) = (settled_t){
        .module = module, .storage = storage,
        .digest = storage_digest(snapshot, module, storage),
    };
}

static bool
build_module_graph(graph_t *graph, const n00b_abi_snapshot_v1 *snapshot,
                   uint32_t index)
{
    const n00b_abi_module_v1 *module = n00b_abi_v1_module(snapshot, index);
    *graph = (graph_t){.snapshot = snapshot, .module_row = index};
    if (!add_storage(graph, module, module->static_storage)
        || !add_storage(graph, module, module->tls_storage)) return false;
    drain(graph);
    return true;
}

static bool
freeze_program(const n00b_abi_snapshot_v1 *snapshot, uint32_t module)
{
    list_t changed = {0};
    for (size_t i = 0; i < settled.count; i++) {
        settled_t *entry = &AT(settled, settled_t, i);
        if (storage_digest(snapshot, entry->module, entry->storage)
            != entry->digest)
            *(settled_t *)push(&changed, sizeof(settled_t)) = *entry;
    }
    list_t frozen = {0};
    *(uint32_t *)push(&frozen, sizeof(uint32_t)) = module;
    for (size_t i = 0; keep_imports && i < changed.count; i++) {
        uint32_t other = AT(changed, settled_t, i).module;
        bool seen = false;
        for (size_t j = 0; j < frozen.count; j++)
            seen |= AT(frozen, uint32_t, j) == other;
        if (!seen) *(uint32_t *)push(&frozen, sizeof(uint32_t)) = other;
    }
    graph_t *graphs = calloc(frozen.count, sizeof(graph_t));
    bool ok = graphs != NULL;
    const graph_t *failed = NULL;
    for (size_t i = 0; ok && i < frozen.count; i++) {
        ok = build_module_graph(&graphs[i], snapshot,
                                AT(frozen, uint32_t, i));
        if (ok && graphs[i].failure && !failed) failed = &graphs[i];
    }
    if (ok && failed) ok = put_failure(failed);
    else if (ok) {
        ok = put(exiting ? "N00BCTX3" : "N00BCT03", 8)
            && put_word(frozen.count, 4);
        for (size_t i = 0; ok && i < frozen.count; i++)
            ok = put_graph(&graphs[i], n00b_abi_v1_module(snapshot,
                AT(frozen, uint32_t, i)));
        ok = ok && put_word(changed.count, 8);
        for (size_t i = 0; ok && i < changed.count; i++) {
            settled_t *entry = &AT(changed, settled_t, i);
            const n00b_abi_storage_v1 *row = n00b_abi_v1_storage(snapshot,
                entry->storage);
            const n00b_abi_symbol_v1 *symbol = row
                ? n00b_abi_v1_symbol(snapshot, row->symbol_index) : NULL;
            ok = symbol && put_abi_string(&graphs[0],
                    &n00b_abi_v1_module(snapshot, entry->module)->full_path)
                && put_abi_string(&graphs[0], &symbol->name);
        }
    }
    for (size_t i = 0; graphs && i < frozen.count; i++)
        release_graph(&graphs[i]);
    free(graphs);
    free(frozen.data);
    free(changed.data);
    return ok;
}

void
n00b_comptime_freeze(uint64_t module)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t row = module_row(snapshot, module);
    if (module != target) {
        if (row) each_storage(snapshot, row, settle);
        return;
    }
    n00b_comptime_exit = nullptr;
    output = output_path
        ? open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
    bool ok = output >= 0 && row && freeze_program(snapshot, row);
    if (output >= 0) close(output);
    _exit(ok ? 0 : COMPTIME_OUTPUT_FAILED);
}

static list_t frozen_keys;

bool
n00b_comptime_frozen(uint64_t module)
{
    for (size_t i = 0; i < frozen_keys.count; i++)
        if (AT(frozen_keys, uint64_t, i) == module) return true;
    return false;
}

static bool
parse_keys(const char *text)
{
    while (text && *text) {
        char digits[17] = {0};
        size_t length = 0;
        while (text[length] && text[length] != ',' && length < 16) {
            digits[length] = text[length];
            length++;
        }
        uint64_t key = 0;
        if (!parse_key(digits, &key)) return false;
        *(uint64_t *)push(&frozen_keys, sizeof(uint64_t)) = key;
        text += length;
        if (*text == ',') text++;
        else if (*text) return false;
    }
    return true;
}

static void
exited(int64_t code)
{
    exiting = true;
    if (entry && !code) n00b_comptime_freeze(target);
    output = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = output >= 0 && put("N00BCTEX", 8) && put(&code, sizeof(code));
    if (output >= 0) close(output);
    _exit(ok ? 0 : COMPTIME_OUTPUT_FAILED);
}

typedef struct {
    uint32_t storage;
    uint8_t *address;
} main_tls_t;

static list_t main_tls;
static bool main_tls_recorded;

static void *
remember_main_tls(void)
{
    if (main_tls_recorded) return NULL;
    main_tls_recorded = true;
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t row = snapshot ? module_row(snapshot, target) : 0;
    const n00b_abi_module_v1 *module = row
        ? n00b_abi_v1_module(snapshot, row) : NULL;
    for (uint32_t i = 0; module && i < module->tls_storage.count; i++) {
        uint32_t index = 0;
        if (n00b_abi_v1_index_at(snapshot, module->tls_storage, i, &index)
            != 1) continue;
        *(main_tls_t *)push(&main_tls, sizeof(main_tls_t)) = (main_tls_t){
            .storage = index,
            .address = (uint8_t *)n00b_builtin_abi_storage_address(index),
        };
    }
    return NULL;
}

static bool
heap_object(const node_t *node)
{
    uint64_t base = (uint64_t)(uintptr_t)node->base;
    return node->parent >= 0 && n00b_gc_object_payload(base) == base;
}

static void
seed_thread(void *unused)
{
    (void)unused;
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    if (!main_tls.count || !snapshot) return;
    graph_t graph = {.snapshot = snapshot,
                     .module_row = module_row(snapshot, target)};
    for (size_t i = 0; i < main_tls.count; i++) {
        main_tls_t *at = &AT(main_tls, main_tls_t, i);
        const n00b_abi_storage_v1 *storage = n00b_abi_v1_storage(snapshot,
                                                                 at->storage);
        const n00b_abi_type_v1 *row = storage
            ? type_row(storage->type_index) : NULL;
        if (!storage || !at->address) return;
        add_node(&graph, at->address, storage->byte_length,
                 row ? row->alignment_bytes : 1, storage->type_index, 0,
                 true, -1);
    }
    const n00b_abi_module_v1 *module = graph.module_row
        ? n00b_abi_v1_module(snapshot, graph.module_row) : NULL;
    for (uint32_t i = 0; module && i < module->static_storage.count; i++) {
        uint32_t index = 0;
        const n00b_abi_storage_v1 *storage =
            n00b_abi_v1_index_at(snapshot, module->static_storage, i, &index)
                == 1 ? n00b_abi_v1_storage(snapshot, index) : NULL;
        uint8_t *address = storage
            ? (uint8_t *)n00b_builtin_abi_storage_address(index) : NULL;
        if (address)
            add_node(&graph, address, storage->byte_length, 1,
                     storage->type_index, 0, false, -1);
    }
    drain(&graph);
    size_t count = graph.nodes.count;
    uint8_t **copies = graph.failure ? NULL : calloc(count, sizeof(*copies));
    uint64_t depth = copies ? n00b_gc_root_depth() : 0;
    bool ok = copies != NULL;
    for (size_t i = 0; ok && i < count; i++) {
        node_t *node = &AT(graph.nodes, node_t, i);
        if (i < main_tls.count) {
            copies[i] = (uint8_t *)n00b_builtin_abi_storage_address(
                AT(main_tls, main_tls_t, i).storage);
            ok = copies[i] != NULL;
            continue;
        }
        if (!heap_object(node)) continue;
        copies[i] = n00b_gc_allocate(node->type_index, node->size,
            node->alignment,
            node->flags & N00B_ABI_BAKED_NODE_ARRAY_BACKING ? node->base
                                                            : NULL);
        ok = copies[i] != NULL;
        if (ok) n00b_gc_root_push(&copies[i]);
    }
    for (size_t i = 0; ok && i < count; i++)
        if (copies[i])
            __builtin_memcpy(copies[i], AT(graph.nodes, node_t, i).base,
                             AT(graph.nodes, node_t, i).size);
    for (size_t i = 0; ok && i < graph.edges.count; i++) {
        edge_t *edge = &AT(graph.edges, edge_t, i);
        if (copies[edge->source] && copies[edge->target])
            *(uint8_t **)(void *)(copies[edge->source] + edge->source_offset)
                = copies[edge->target] + edge->target_offset;
    }
    for (size_t i = 0; ok && i < count; i++) {
        node_t *node = &AT(graph.nodes, node_t, i);
        if (copies[i] && !(node->flags & N00B_ABI_BAKED_NODE_ARRAY_BACKING))
            n00b_gc_barrier_value(copies[i], node->type_index);
    }
    if (copies) n00b_gc_root_pop(depth);
    free(copies);
    release_graph(&graph);
}

int
main(int argc, char **argv)
{
    if (argc < 5 || !parse_key(argv[1], &target) || !parse_keys(argv[4]))
        return COMPTIME_NO_TARGET;
    output_path = argv[2];
    entry = argv[3][0] == 'e' || argv[3][0] == 's';
    keep_imports = argv[3][0] == 's';
    argv[4] = argv[0];
    n00b_comptime = true;
    n00b_comptime_exit = exited;
    n00b_os_set_thread_seed_hooks(remember_main_tls, seed_thread);
    __n00bb_main(argc - 4, argv + 4);
    return COMPTIME_TARGET_NOT_REACHED;
}
