#include "bake.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int64_t
bake_add(int64_t left, int64_t right)
{
    return left + right;
}

int64_t
bake_multiply(int64_t left, int64_t right)
{
    return left * right;
}

static uint64_t
address_hash(const void *address)
{
    return ((uint64_t)(uintptr_t)address * UINT64_C(0x9e3779b97f4a7c15)) >> 7;
}

static char *
copy_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = calloc(length + 1, 1);
    if (copy) memcpy(copy, text, length);
    return copy;
}

bake_root_t *
bake_build(const char *spec)
{
    bake_root_t *root = calloc(1, sizeof(*root));
    if (!root) return NULL;
    static const char *const labels[] = {"even", "odd"};
    char *even = copy_text(labels[0]);
    char *odd = copy_text(labels[1]);
    root->name = copy_text("baked graph");
    root->items.cap = 6;
    root->items.len = 4;
    root->items.data = calloc(root->items.cap, sizeof(bake_item_t));
    root->codes = calloc(4, sizeof(int32_t));
    root->table.len = 8;
    root->table.slots = calloc(root->table.len, sizeof(bake_slot_t));
    root->path = copy_text(spec);
    if (!even || !odd || !root->name || !root->items.data || !root->codes
        || !root->table.slots || !root->path) {
        free(even);
        free(odd);
        bake_free(root);
        return NULL;
    }
    for (size_t i = 0; i < root->items.len; i++) {
        bake_item_t *item = &root->items.data[i];
        item->key = (int32_t)i + 1;
        item->value = (int32_t)(i + 1) * 10;
        item->label = i % 2 ? odd : even;
        item->hash = address_hash(item);
        size_t at = item->hash % root->table.len;
        while (root->table.slots[at].key) at = (at + 1) % root->table.len;
        root->table.slots[at] = (bake_slot_t){
            .key = item, .hash = item->hash, .value = item->value,
        };
    }
    root->codes[0] = 3;
    root->codes[1] = 5;
    root->codes[2] = 7;
    root->variant.kind = BAKE_NUMBER;
    root->variant.v.number = 42;
    root->middle = &root->items.data[2];
    root->op = bake_multiply;
    root->fd = open(spec, O_RDONLY);
    return root;
}

static int64_t
table_lookup(const bake_root_t *root, const bake_item_t *item)
{
    size_t at = item->hash % root->table.len;
    for (size_t probe = 0; probe < root->table.len; probe++) {
        const bake_slot_t *slot = &root->table.slots[at];
        if (!slot->key) return -1;
        if (slot->key == item && slot->hash == item->hash) return slot->value;
        at = (at + 1) % root->table.len;
    }
    return -1;
}

int64_t
bake_query(bake_root_t *root, int32_t key)
{
    if (!root) return -1;
    switch (key) {
    case BAKE_QUERY_NAME:
        return (int64_t)strlen(root->name);
    case BAKE_QUERY_ITEMS: {
        int64_t sum = 0;
        for (size_t i = 0; i < root->items.len; i++)
            sum += root->items.data[i].value;
        return sum;
    }
    case BAKE_QUERY_CODES: {
        int64_t count = 0;
        while (root->codes[count]) count++;
        return count;
    }
    case BAKE_QUERY_VARIANT:
        return root->variant.kind == BAKE_NUMBER
            ? root->variant.v.number
            : (int64_t)strlen(root->variant.v.text);
    case BAKE_QUERY_SHARED:
        return root->items.data[0].label == root->items.data[2].label;
    case BAKE_QUERY_MIDDLE:
        return root->middle - root->items.data;
    case BAKE_QUERY_OP:
        return root->op(6, 7);
    case BAKE_QUERY_HASH:
        return table_lookup(root, &root->items.data[3]);
    case BAKE_QUERY_CACHE:
        return ++root->cache;
    case BAKE_QUERY_FD: {
        unsigned char byte = 0;
        return root->fd >= 0 && pread(root->fd, &byte, 1, 0) == 1
            ? (int64_t)byte : -1;
    }
    default:
        return -1;
    }
}

int32_t
bake_reopen(bake_root_t *root)
{
    if (!root) return -1;
    root->fd = bake_open(root->path);
    return root->fd >= 0 ? 0 : -1;
}

int32_t
bake_open(const char *path)
{
    return path ? open(path, O_RDONLY) : -1;
}

void
bake_free(bake_root_t *root)
{
    if (!root) return;
    if (root->items.data && root->items.len) {
        free((void *)root->items.data[0].label);
        if (root->items.len > 1) free((void *)root->items.data[1].label);
    }
    if (root->fd >= 0) close(root->fd);
    free((void *)root->name);
    free(root->items.data);
    free(root->codes);
    free(root->table.slots);
    free((void *)root->path);
    free(root);
}
