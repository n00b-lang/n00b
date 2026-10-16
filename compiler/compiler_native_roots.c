#include <stdlib.h>
#include <string.h>

#include "compiler_native_roots.h"

static n00bb_runtime_native_root_t *registered_roots;
static size_t registered_root_count;

static const n00bb_runtime_native_root_t *
native_root_at(size_t index)
{
    if (index < n00bb_runtime_native_root_count)
        return &n00bb_runtime_native_roots[index];
    index -= n00bb_runtime_native_root_count;
    return index < registered_root_count ? &registered_roots[index] : nullptr;
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
n00bb_native_root_register(const char *qualified_name, const char *c_alias)
{
    for (size_t i = 0; i < n00bb_runtime_native_root_count + registered_root_count;
         i++)
        if (!strcmp(native_root_at(i)->qualified_name, qualified_name))
            return true;
    n00bb_runtime_native_root_t *grown = realloc(registered_roots,
        (registered_root_count + 1) * sizeof(*grown));
    char *name = strdup(qualified_name);
    char *alias = strdup(c_alias);
    if (!grown || !name || !alias) {
        if (grown) registered_roots = grown;
        free(name);
        free(alias);
        return false;
    }
    registered_roots = grown;
    registered_roots[registered_root_count++] = (n00bb_runtime_native_root_t){
        .qualified_name = name,
        .c_alias = alias,
    };
    return true;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_in_module(const char *module, size_t module_length,
                            size_t *cursor)
{
    if (!module || !module_length || !cursor) return nullptr;
    while (*cursor < n00bb_runtime_native_root_count + registered_root_count) {
        const n00bb_runtime_native_root_t *root = native_root_at((*cursor)++);
        const char *name = root->qualified_name;
        if (!strncmp(name, module, module_length)
            && name[module_length] == '.'
            && !strchr(name + module_length + 1, '.'))
            return root;
    }
    return nullptr;
}

const n00bb_runtime_native_root_t *
n00bb_native_root_named(const char *module, size_t module_length,
                        const char *function, size_t function_length)
{
    size_t cursor = 0;
    const n00bb_runtime_native_root_t *root;
    while ((root = n00bb_native_root_in_module(module, module_length,
                                               &cursor))) {
        const char *name = n00bb_native_root_function(root, module_length);
        if (function && strlen(name) == function_length
            && !memcmp(name, function, function_length)) return root;
    }
    return nullptr;
}

const char *
n00bb_native_root_function(const n00bb_runtime_native_root_t *root,
                           size_t module_length)
{
    return root->qualified_name + module_length + 1;
}
