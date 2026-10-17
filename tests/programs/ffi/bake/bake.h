#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct bake_item {
    int32_t key;
    int32_t value;
    uint64_t hash;
    const char *label;
} bake_item_t;

typedef struct bake_items {
    bake_item_t *data;
    size_t len;
    size_t cap;
} bake_items_t;

enum { BAKE_NUMBER = 1, BAKE_TEXT = 2 };

typedef struct bake_variant {
    int32_t kind;
    union {
        int64_t number;
        const char *text;
    } v;
} bake_variant_t;

typedef int64_t (*bake_op_t)(int64_t left, int64_t right);

int64_t bake_add(int64_t left, int64_t right);
int64_t bake_multiply(int64_t left, int64_t right);

typedef struct bake_slot {
    const bake_item_t *key;
    uint64_t hash;
    int32_t value;
} bake_slot_t;

typedef struct bake_table {
    bake_slot_t *slots;
    size_t len;
} bake_table_t;

typedef struct bake_root {
    const char *name;
    bake_items_t items;
    int32_t *codes;
    bake_variant_t variant;
    bake_item_t *middle;
    bake_op_t op;
    bake_table_t table;
    int64_t cache;
    int32_t fd;
    const char *path;
} bake_root_t;

enum {
    BAKE_QUERY_NAME = 0,
    BAKE_QUERY_ITEMS = 1,
    BAKE_QUERY_CODES = 2,
    BAKE_QUERY_VARIANT = 3,
    BAKE_QUERY_SHARED = 4,
    BAKE_QUERY_MIDDLE = 5,
    BAKE_QUERY_OP = 6,
    BAKE_QUERY_HASH = 7,
    BAKE_QUERY_CACHE = 8,
    BAKE_QUERY_FD = 9,
};

bake_root_t *bake_build(const char *spec);
int64_t bake_query(bake_root_t *root, int32_t key);
int32_t bake_reopen(bake_root_t *root);
int32_t bake_open(const char *path);
void bake_free(bake_root_t *root);
