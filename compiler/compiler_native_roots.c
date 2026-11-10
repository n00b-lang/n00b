#include <stdlib.h>

#include "compiler_native_roots.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef size_t native_root_index_t;
core_list_decl(native_root_index_t);

typedef struct {
    n00bb_runtime_native_root_t root;
    core_string_t               name;
} registered_root_t;

static registered_root_t *registered;
static size_t             registered_count;
static size_t             registered_capacity;
static core_string_t     *runtime_names;
static bool               indexed;
static core_dict_t        by_name;
static core_dict_t        by_module;

static size_t
module_length(core_string_t name)
{
    int64_t dot = -1;
    for (size_t i = 0; i < name.u8_bytes; i++)
        if (name.data[i] == '.') dot = (int64_t)i;
    return dot < 0 ? 0 : (size_t)dot;
}

static bool
index_root(size_t index, core_string_t name)
{
    core_dict_put(&by_name, core_dict_string_key_copy(name),
                  (void *)(uintptr_t)(index + 1));
    core_string_t module = core_string_slice(name, 0, module_length(name));
    bool found = false;
    core_list_t(native_root_index_t) *bucket = core_dict_get(&by_module,
        &module, &found);
    if (!found) {
        bucket = core_alloc(core_list_t(native_root_index_t));
        *bucket = core_list_new(native_root_index_t);
        core_dict_put(&by_module, core_dict_string_key_copy(module), bucket);
    }
    core_list_push(*bucket, index);
    return true;
}

static bool
ensure_index(void)
{
    if (indexed) return true;
    core_dict_init_string(&by_name);
    core_dict_init_string(&by_module);
    runtime_names = core_alloc_array(core_string_t,
        n00bb_runtime_native_root_count ? n00bb_runtime_native_root_count : 1);
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++) {
        runtime_names[i] = core_string_from_cstr(
            n00bb_runtime_native_roots[i].qualified_name);
        index_root(i, runtime_names[i]);
    }
    indexed = true;
    return true;
}

static core_string_t
root_name(const n00bb_runtime_native_root_t *root)
{
    if (root >= n00bb_runtime_native_roots
        && root < n00bb_runtime_native_roots + n00bb_runtime_native_root_count)
        return runtime_names[root - n00bb_runtime_native_roots];
    const registered_root_t *entry = (const registered_root_t *)root;
    return registered && entry >= registered
        && entry < registered + registered_count ? entry->name
                                                 : (core_string_t){};
}

static const n00bb_runtime_native_root_t *
root_at(size_t index)
{
    return index < n00bb_runtime_native_root_count
        ? &n00bb_runtime_native_roots[index]
        : &registered[index - n00bb_runtime_native_root_count].root;
}

size_t
n00bb_native_root_registered_count(void)
{
    ensure_index();
    return registered_count;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_registered_at(size_t index)
{
    ensure_index();
    return index < registered_count ? &registered[index].root : nullptr;
}

bool
n00bb_native_root_register(core_string_t qualified_name,
                           core_string_t c_alias)
{
    if (!qualified_name.data || !c_alias.data || !ensure_index()) return false;
    bool found = false;
    (void)core_dict_get(&by_name, &qualified_name, &found);
    if (found) return true;
    if (registered_count == registered_capacity) {
        size_t capacity = registered_capacity ? 2 * registered_capacity : 16;
        registered_root_t *grown = core_realloc(registered,
            capacity * sizeof(*grown));
        if (!grown) return false;
        registered = grown;
        registered_capacity = capacity;
    }
    core_string_t name = core_string_dup(qualified_name);
    char *native_name = core_string_to_cstr(qualified_name);
    char *alias = core_string_to_cstr(c_alias);
    if (!name.data || !native_name || !alias) {
        core_free(name.data);
        core_free(native_name);
        core_free(alias);
        return false;
    }
    registered[registered_count] = (registered_root_t){
        .root = {.qualified_name = native_name, .c_alias = alias},
        .name = name,
    };
    index_root(n00bb_runtime_native_root_count + registered_count++, name);
    return true;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_in_module(core_string_t module, size_t *cursor)
{
    if (!module.data || !module.u8_bytes || !cursor || !ensure_index())
        return nullptr;
    bool found = false;
    core_list_t(native_root_index_t) *bucket = core_dict_get(&by_module,
        &module, &found);
    if (found && *cursor < bucket->len)
        return root_at(bucket->data[(*cursor)++]);
    return nullptr;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_named(core_string_t module, core_string_t function)
{
    if (!module.data || !function.data || !ensure_index()) return nullptr;
    core_buffer_t name = {0};
    core_buffer_append_string(&name, module);
    core_buffer_putc(&name, '.');
    core_buffer_append_string(&name, function);
    core_string_t key = core_buffer_string(&name);
    bool found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&by_name, &key, &found);
    core_buffer_release(&name);
    return found ? root_at(index - 1) : nullptr;
}

core_string_t
n00bb_native_root_function(const n00bb_runtime_native_root_t *root,
                           size_t module_length)
{
    core_string_t name = root && ensure_index() ? root_name(root)
                                                : (core_string_t){};
    if (!name.data || name.u8_bytes <= module_length) return (core_string_t){};
    return core_string_slice(name, module_length + 1,
                             name.u8_bytes - module_length - 1);
}
