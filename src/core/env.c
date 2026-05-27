/** @file env.c
 *  @brief libn00b environment-variable accessors.
 *
 *  Reads and writes go through the envp cache that `n00b_init()`
 *  stashes in `n00b_get_runtime()->envp`.  Growth (`n00b_putenv` on
 *  an unseen name) allocates the new slot array and the new
 *  `NAME=value` byte buffer from the runtime's `system_pool` and
 *  rebinds the libc-visible `__environ` to the new slot array; the
 *  system pool is non-arena and non-GC-scanned so `__environ` never
 *  needs to be registered as a GC root.
 *
 *  See `include/core/env.h` for the public API.
 */

#define N00B_USE_INTERNAL_API

#include "n00b.h"
#include "core/env.h"
#include "core/runtime.h"
#include "core/string.h"

#include <string.h>

extern char **environ;

static inline n00b_allocator_t *
env_pool(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
}

static bool
env_name_matches(const char *entry, const char *name, size_t name_len)
{
    for (size_t i = 0; i < name_len; i++) {
        if (entry[i] != name[i]) {
            return false;
        }
    }
    return entry[name_len] == '=';
}

n00b_string_t *
n00b_getenv(n00b_string_t *name)
{
    if (!name || name->u8_bytes == 0) {
        return nullptr;
    }

    n00b_runtime_t *rt        = n00b_get_runtime();
    char          **entries   = rt->envp.data;
    size_t          slot_count = rt->envp.len;
    if (!entries) {
        return nullptr;
    }

    const char *name_data = name->data;
    size_t      name_len  = (size_t)name->u8_bytes;
    for (size_t i = 0; i < slot_count; i++) {
        char *entry = entries[i];
        if (!entry) {
            continue;
        }
        if (env_name_matches(entry, name_data, name_len)) {
            return n00b_string_from_cstr(entry + name_len + 1);
        }
    }
    return nullptr;
}

static bool
name_is_valid(n00b_string_t *name)
{
    if (!name || name->u8_bytes == 0) {
        return false;
    }
    const char *p   = name->data;
    size_t      len = (size_t)name->u8_bytes;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '=' || p[i] == '\0') {
            return false;
        }
    }
    return true;
}

static char *
build_entry(n00b_string_t *name, n00b_string_t *value, n00b_allocator_t *pool)
{
    size_t name_len  = (size_t)name->u8_bytes;
    size_t value_len = (size_t)value->u8_bytes;
    size_t total     = name_len + 1 + value_len + 1; /* "name=value\0" */

    char *buf = n00b_alloc_array(char, total, .allocator = pool);
    memcpy(buf, name->data, name_len);
    buf[name_len] = '=';
    if (value_len > 0) {
        memcpy(buf + name_len + 1, value->data, value_len);
    }
    buf[total - 1] = '\0';
    return buf;
}

bool
n00b_putenv(n00b_string_t *name, n00b_string_t *value)
{
    if (!name_is_valid(name) || !value) {
        return false;
    }

    n00b_allocator_t *pool   = env_pool();
    n00b_runtime_t   *rt     = n00b_get_runtime();
    char             *entry  = build_entry(name, value, pool);
    char            **slots  = rt->envp.data;
    size_t            count  = rt->envp.len;
    size_t            name_len = (size_t)name->u8_bytes;

    /* Replace existing slot in place if the name already has one. */
    for (size_t i = 0; i < count; i++) {
        char *cur = slots ? slots[i] : nullptr;
        if (!cur) {
            continue;
        }
        if (env_name_matches(cur, name->data, name_len)) {
            slots[i] = entry;
            /* `environ` and `rt->envp.data` already point at the
             * same slot array, so libc consumers see the update. */
            return true;
        }
    }

    /* Append a new slot.  Allocate a new slot array (one larger,
     * plus the trailing null terminator that libc expects on
     * `environ`) from the system_pool, copy existing slots in, set
     * the new entry, and rebind both `rt->envp` and `environ`. */
    size_t  new_count = count + 1;
    char  **new_slots = n00b_alloc_array(char *,
                                         new_count + 1,
                                         .allocator = pool);
    for (size_t i = 0; i < count; i++) {
        new_slots[i] = slots ? slots[i] : nullptr;
    }
    new_slots[count]     = entry;
    new_slots[new_count] = nullptr;

    rt->envp.data = new_slots;
    rt->envp.cap  = new_count;
    rt->envp.len  = new_count;
    environ       = new_slots;
    return true;
}
