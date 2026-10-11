#include "runtime_literal_graph.h"
#include "builtin_os.h"
#include "n00b-abi-v1.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *left;
    void *right;
    const char *text;
} root_t;

typedef struct {
    root_t *parent;
    uint64_t value;
} child_t;

static void **root_slots[8];
static size_t root_count;
static size_t barriers;

void *n00b_os_map_zeroed(size_t bytes, size_t alignment)
{
    assert(alignment <= _Alignof(max_align_t));
    return calloc(1, bytes);
}

int32_t n00b_os_unmap(void *address, size_t bytes)
{
    (void)bytes;
    free(address);
    return 0;
}

uint64_t n00b_gc_root_depth(void) { return root_count; }

void n00b_gc_root_push(void **slot)
{
    assert(root_count < sizeof root_slots / sizeof root_slots[0]);
    assert(!*slot);
    root_slots[root_count++] = slot;
}

void n00b_gc_root_pop(uint64_t depth)
{
    assert(depth <= root_count);
    root_count = (size_t)depth;
}

void n00b_gc_barrier_value(const void *address, uint32_t type_index)
{
    assert(address && type_index);
    barriers++;
}

void *n00b_allocate(uint32_t allocator, uint32_t type_index,
                    uint64_t byte_size, uint32_t alignment,
                    const void *instance_descriptor)
{
    assert(allocator == N00B_ABI_ALLOCATOR_GC);
    assert(type_index && byte_size && !instance_descriptor);
    assert(alignment <= _Alignof(max_align_t));
    return calloc(1, (size_t)byte_size);
}

int main(void)
{
    static const char text[] = "an immutable string payload";
    root_t root = {0};
    child_t child = {.parent = &root, .value = 73};
    root.left = root.right = &child;
    root.text = text;
    const n00bb_literal_graph_node_t nodes[] = {
        {&root, sizeof root, 1, _Alignof(root_t), 0, 0},
        {&child, sizeof child, 2, _Alignof(child_t), 0, 0},
        {text, sizeof text, 0, 1, 0, 1},
    };
    const n00bb_literal_graph_edge_t edges[] = {
        {0, 1, offsetof(root_t, left), 0},
        {0, 1, offsetof(root_t, right), 0},
        {0, 2, offsetof(root_t, text), 0},
        {1, 0, offsetof(child_t, parent), 0},
    };
    root_t *first = n00bb_literal_graph_clone(nodes, 3, edges, 4, 0);
    root_t *second = n00bb_literal_graph_clone(nodes, 3, edges, 4, 0);
    assert(first && second && first != second && !root_count);
    assert(first->left == first->right && second->left == second->right);
    assert(first->left != second->left);
    assert(((child_t *)first->left)->parent == first);
    assert(((child_t *)second->left)->parent == second);
    assert(first->text == text && second->text == text);
    assert(((child_t *)first->left)->value == 73 && barriers == 4);
    free(first->left);
    free(second->left);
    free(first);
    free(second);
    return 0;
}
