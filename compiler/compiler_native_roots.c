#include <string.h>

#include "compiler_native_roots.h"

const n00bb_runtime_native_root_t *
n00bb_native_root_in_module(const char *module, size_t module_length,
                            size_t *cursor)
{
    if (!module || !module_length || !cursor) return nullptr;
    while (*cursor < n00bb_runtime_native_root_count) {
        const n00bb_runtime_native_root_t *root =
            &n00bb_runtime_native_roots[(*cursor)++];
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
