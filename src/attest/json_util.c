/* json_util.c — module-internal JSON helpers for n00b_attest.
 *
 * Implementation for the helpers declared in
 * `include/internal/attest/json_util.h`. Shared between
 * `statement.c` and `dsse.c`; not part of the public surface.
 * Doxygen for these symbols lives in the header.
 */

#include "internal/attest/json_util.h"

#include <string.h>

#include "core/runtime.h"

n00b_json_node_t *
n00b_attest_json_obj_lookup(n00b_json_node_t *obj, n00b_string_t *key)
{
    if (obj == nullptr || obj->type != N00B_JSON_OBJECT) {
        return nullptr;
    }
    if (key == nullptr || key->data == nullptr) {
        return nullptr;
    }
    n00b_dict_untyped_store_t *s = atomic_load(&obj->object->store);
    if (s == nullptr) {
        return nullptr;
    }
    size_t klen = key->u8_bytes;
    for (uint32_t i = 0; i <= s->last_slot; i++) {
        n00b_dict_untyped_bucket_t *b = &s->buckets[i];
        if (b->hv == 0) {
            continue;
        }
        const char *bk = (const char *)b->key;
        if (bk == nullptr) {
            continue;
        }
        if (strlen(bk) == klen && memcmp(bk, key->data, klen) == 0) {
            return (n00b_json_node_t *)b->value;
        }
    }
    return nullptr;
}
