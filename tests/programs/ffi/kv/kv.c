#include <stddef.h>
#include <stdint.h>

typedef struct kv_entry {
    const uint8_t *key;
    size_t         key_len;
    const uint8_t *val;
    size_t         val_len;
} kv_entry;

typedef struct kv_db {
    uint8_t keys[8][16];
    size_t  key_lens[8];
    uint8_t vals[8][16];
    size_t  val_lens[8];
    size_t  count;
    int     open;
} kv_db;

typedef struct kv_iter {
    kv_db         *db;
    uint8_t        prefix[16];
    size_t         prefix_len;
    size_t         position;
    kv_entry       entry;
    int            open;
} kv_iter;

static kv_db   databases[4];
static kv_iter iterators[4];
static uint8_t heap[8][64];
static int     heap_used[8];

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

static void
copy(uint8_t *to, const uint8_t *from, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
}

void *
kv_malloc(size_t size)
{
    for (size_t i = 0; i < 8 && size <= 64; i++) {
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
    for (size_t i = 0; i < 8; i++) {
        if (data == heap[i]) {
            heap_used[i] = 0;
        }
    }
}

int
kv_live(void)
{
    int live = 0;
    for (size_t i = 0; i < 8; i++) {
        live += heap_used[i];
    }
    return live;
}

int
kv_open(const char *path, kv_db **out_db)
{
    if (!path || !path[0]) {
        return 2;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!databases[i].open) {
            databases[i] = (kv_db){.open = 1};
            *out_db = &databases[i];
            return 0;
        }
    }
    return 2;
}

void
kv_close(kv_db *db)
{
    db->open = 0;
}

int
kv_put(kv_db *db, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen)
{
    if (db->count == 8 || klen > 16 || vlen > 16) {
        return 2;
    }
    copy(db->keys[db->count], key, klen);
    db->key_lens[db->count] = klen;
    copy(db->vals[db->count], val, vlen);
    db->val_lens[db->count] = vlen;
    db->count++;
    return 0;
}

int
kv_get(kv_db *db, const uint8_t *key, size_t klen, uint8_t **out_val, size_t *out_len)
{
    for (size_t i = 0; i < db->count; i++) {
        if (db->key_lens[i] == klen && same(db->keys[i], key, klen)) {
            uint8_t *value = kv_malloc(db->val_lens[i]);
            if (!value) {
                return 2;
            }
            copy(value, db->vals[i], db->val_lens[i]);
            *out_val = value;
            *out_len = db->val_lens[i];
            return 0;
        }
    }
    return 1;
}

kv_iter *
kv_iter_new(kv_db *db, const uint8_t *prefix, size_t plen)
{
    for (size_t i = 0; i < 4 && plen <= 16; i++) {
        if (!iterators[i].open) {
            iterators[i] = (kv_iter){.db = db, .prefix_len = plen, .open = 1};
            copy(iterators[i].prefix, prefix, plen);
            return &iterators[i];
        }
    }
    return NULL;
}

kv_entry *
kv_iter_next(kv_iter *it)
{
    while (it->position < it->db->count) {
        size_t i = it->position++;
        if (it->db->key_lens[i] >= it->prefix_len && same(it->db->keys[i], it->prefix, it->prefix_len)) {
            it->entry = (kv_entry){it->db->keys[i], it->db->key_lens[i], it->db->vals[i], it->db->val_lens[i]};
            return &it->entry;
        }
    }
    return NULL;
}

void
kv_iter_free(kv_iter *it)
{
    it->open = 0;
}

const char *
kv_errstr(int status)
{
    return status == 1 ? "not found" : "failed";
}
