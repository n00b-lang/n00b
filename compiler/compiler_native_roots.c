#include <stdlib.h>

#include "compiler_native_roots.h"
#include "lib/alloc.h"

static n00bb_runtime_native_root_t *registered_roots;
static core_string_t *registered_names;
static size_t registered_root_count;
static core_string_t *runtime_names;

static bool
runtime_names_ready(void)
{
    if (runtime_names || !n00bb_runtime_native_root_count) return true;
    core_string_t *names = calloc(n00bb_runtime_native_root_count,
                                  sizeof(*names));
    if (!names) return false;
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++)
        names[i] = core_string_from_cstr(
            n00bb_runtime_native_roots[i].qualified_name);
    runtime_names = names;
    return true;
}

static const n00bb_runtime_native_root_t *
native_root_at(size_t index)
{
    if (index < n00bb_runtime_native_root_count)
        return &n00bb_runtime_native_roots[index];
    index -= n00bb_runtime_native_root_count;
    return index < registered_root_count ? &registered_roots[index] : nullptr;
}

static core_string_t
native_root_name_at(size_t index)
{
    if (index < n00bb_runtime_native_root_count)
        return runtime_names_ready() ? runtime_names[index]
                                     : (core_string_t){};
    index -= n00bb_runtime_native_root_count;
    return index < registered_root_count ? registered_names[index]
                                         : (core_string_t){};
}

static core_string_t
native_root_name(const n00bb_runtime_native_root_t *root)
{
    if (root >= n00bb_runtime_native_roots
        && root < n00bb_runtime_native_roots + n00bb_runtime_native_root_count)
        return native_root_name_at((size_t)(root - n00bb_runtime_native_roots));
    if (registered_roots && root >= registered_roots
        && root < registered_roots + registered_root_count)
        return registered_names[root - registered_roots];
    return (core_string_t){};
}

size_t
n00bb_native_root_registered_count(void)
{
    return registered_root_count;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_registered_at(size_t index)
{
    return index < registered_root_count ? &registered_roots[index] : nullptr;
}

bool
n00bb_native_root_register(core_string_t qualified_name,
                           core_string_t c_alias)
{
    if (!qualified_name.data || !c_alias.data) return false;
    for (size_t i = 0; i < n00bb_runtime_native_root_count + registered_root_count;
         i++)
        if (core_string_eq(native_root_name_at(i), qualified_name))
            return true;
    n00bb_runtime_native_root_t *grown = realloc(registered_roots,
        (registered_root_count + 1) * sizeof(*grown));
    if (grown) registered_roots = grown;
    core_string_t *grown_names = realloc(registered_names,
        (registered_root_count + 1) * sizeof(*grown_names));
    if (grown_names) registered_names = grown_names;
    core_string_t name = core_string_dup(qualified_name);
    char *native_name = core_string_to_cstr(qualified_name);
    char *alias = core_string_to_cstr(c_alias);
    if (!grown || !grown_names || !name.data || !native_name || !alias) {
        core_free(name.data);
        core_free(native_name);
        core_free(alias);
        return false;
    }
    registered_names[registered_root_count] = name;
    registered_roots[registered_root_count++] = (n00bb_runtime_native_root_t){
        .qualified_name = native_name,
        .c_alias = alias,
    };
    return true;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_in_module(core_string_t module, size_t *cursor)
{
    if (!module.data || !module.u8_bytes || !cursor) return nullptr;
    while (*cursor < n00bb_runtime_native_root_count + registered_root_count) {
        size_t index = (*cursor)++;
        core_string_t name = native_root_name_at(index);
        if (name.u8_bytes > module.u8_bytes
            && core_string_starts_with(name, module)
            && name.data[module.u8_bytes] == '.'
            && core_string_find_byte(core_string_slice(name,
                   module.u8_bytes + 1, name.u8_bytes - module.u8_bytes - 1),
                   '.') < 0)
            return native_root_at(index);
    }
    return nullptr;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_named(core_string_t module, core_string_t function)
{
    size_t cursor = 0;
    const n00bb_runtime_native_root_t *root;
    while ((root = n00bb_native_root_in_module(module, &cursor))) {
        core_string_t name = n00bb_native_root_function(root,
                                                        module.u8_bytes);
        if (function.data && core_string_eq(name, function)) return root;
    }
    return nullptr;
}

core_string_t
n00bb_native_root_function(const n00bb_runtime_native_root_t *root,
                           size_t module_length)
{
    core_string_t name = native_root_name(root);
    if (!name.data || name.u8_bytes <= module_length) return (core_string_t){};
    return core_string_slice(name, module_length + 1,
                             name.u8_bytes - module_length - 1);
}
