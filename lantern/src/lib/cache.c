#include "lib/cache.h"
#include "lib/alloc.h"
#include "lib/hash.h"

typedef struct {
    core_cache_key_t key;
    void            *result;
    uint64_t         gen;
} cache_entry_t;

static core_hash_value_t
cache_key_hash(void *k)
{
    core_cache_key_t *key = (core_cache_key_t *)k;
    core_hash_value_t h   = core_hash_word(key->subject);

    h ^= core_hash_raw(&key->param, sizeof(key->param));
    h ^= core_hash_raw(&key->query, sizeof(key->query));

    return h;
}

static bool
cache_key_eq(const void *a, const void *b)
{
    const core_cache_key_t *ka = (const core_cache_key_t *)a;
    const core_cache_key_t *kb = (const core_cache_key_t *)b;

    return ka->subject == kb->subject && ka->query == kb->query
        && ka->param == kb->param;
}

static core_dict_t *
entries_new(void)
{
    core_dict_t *d = core_alloc(core_dict_t);

    core_dict_init(d, cache_key_hash, cache_key_eq);

    return d;
}

void
core_cache_init(core_cache_t *c)
{
    if (!c) {
        return;
    }

    c->entries = entries_new();
    c->gen     = 1;
    c->enabled = true;
}

static void
free_entries(core_cache_t *c)
{
    core_dict_foreach (c->entries, e) {
        core_free(e->value);
    }

    core_dict_free(c->entries);
    core_free(c->entries);
    c->entries = nullptr;
}

void
core_cache_free(core_cache_t *c)
{
    if (!c || !c->entries) {
        return;
    }

    free_entries(c);
}

void
core_cache_clear(core_cache_t *c)
{
    if (!c || !c->entries) {
        return;
    }

    free_entries(c);
    c->entries = entries_new();
    c->gen++;
}

void
core_cache_invalidate(core_cache_t *c)
{
    if (c) {
        c->gen++;
    }
}

void
core_cache_set_enabled(core_cache_t *c, bool on)
{
    if (c) {
        c->enabled = on;
    }
}

bool
core_cache_get(core_cache_t *c, void *subject, uint32_t query, uint64_t param,
               void **out)
{
    if (!c || !c->entries || !c->enabled) {
        return false;
    }

    core_cache_key_t key = {.subject = subject, .query = query, .param = param};
    bool             found = false;
    void            *val   = _core_dict_get(c->entries, &key, &found);

    if (!found) {
        return false;
    }

    cache_entry_t *e = (cache_entry_t *)val;

    if (e->gen != c->gen) {
        return false;
    }

    if (out) {
        *out = e->result;
    }

    return true;
}

void
core_cache_put(core_cache_t *c, void *subject, uint32_t query, uint64_t param,
               void *result)
{
    if (!c || !c->entries || !c->enabled) {
        return;
    }

    core_cache_key_t key = {.subject = subject, .query = query, .param = param};
    bool             found = false;
    void            *val   = _core_dict_get(c->entries, &key, &found);

    if (found) {
        cache_entry_t *e = (cache_entry_t *)val;

        e->result = result;
        e->gen    = c->gen;

        return;
    }

    cache_entry_t *e = core_alloc(cache_entry_t);

    e->key    = key;
    e->result = result;
    e->gen    = c->gen;

    _core_dict_put(c->entries, &e->key, e);
}

