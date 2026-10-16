#include <stddef.h>
#include <stdint.h>

#include "kv.h"

static uint8_t heap[4][64];
static int     heap_used[4];

void *
kv_malloc(size_t size)
{
    for (size_t i = 0; i < 4 && size <= 64; i++) {
        if (!heap_used[i]) {
            heap_used[i] = 1;
            return heap[i];
        }
    }
    return NULL;
}

void
kv_free(void *data)
{
    for (size_t i = 0; i < 4; i++) {
        if (data == heap[i]) {
            heap_used[i] = 0;
        }
    }
}

static int
same(const uint8_t *a, const uint8_t *b, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int
kv_client_run(void)
{
    kv_db  *db       = NULL;
    uint8_t key[]    = {97, 49};
    uint8_t value[]  = {111, 110, 101};
    uint8_t absent[] = {122};
    uint8_t *found   = NULL;
    size_t   length  = 0;
    if (kv_open("store", &db) != KV_OK || !db) {
        return 1;
    }
    if (kv_put(db, key, 2, value, 3) != KV_OK) {
        return 2;
    }
    if (kv_get(db, key, 2, &found, &length) != KV_OK || length != 3 || !same(found, value, 3)) {
        return 3;
    }
    kv_free(found);
    if (kv_get(db, absent, 1, &found, &length) != KV_NOTFOUND) {
        return 4;
    }
    kv_db *unused = NULL;
    if (kv_open("", &unused) != KV_ERR) {
        return 5;
    }
    if (kv_put(db, key, 0, value, 3) != KV_ERR) {
        return 6;
    }
    if (kv_put(db, key, 2, value, 0) != KV_ERR) {
        return 12;
    }
    if (kv_get(db, key, 2, &found, &length) != KV_OK || length != 3) {
        return 13;
    }
    kv_free(found);
    kv_iter *it = kv_iter_new(db, NULL, 0);
    if (!it) {
        return 8;
    }
    const kv_entry *entry = kv_iter_next(it);
    if (!entry || entry->key_len != 2 || !same(entry->key, key, 2) || entry->val_len != 3
        || !same(entry->val, value, 3)) {
        return 9;
    }
    if (kv_iter_next(it)) {
        return 10;
    }
    kv_iter_free(it);
    const char *text = kv_errstr(KV_NOTFOUND);
    if (!text || text[0] != 'n' || text != kv_errstr(KV_NOTFOUND)) {
        return 11;
    }
    kv_close(db);
    if (kv_put(db, key, 2, value, 3) != KV_ERR) {
        return 7;
    }
    return 0;
}
