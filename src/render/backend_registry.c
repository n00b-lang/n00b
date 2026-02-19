/**
 * Backend registry: static registration, name lookup, dynamic loading.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include "n00b.h"
#include "render/backend_registry.h"

// -------------------------------------------------------------------
// Registry storage
// -------------------------------------------------------------------

#define MAX_BACKENDS 32

typedef struct {
    const char                   *name;
    const n00b_renderer_vtable_t *vtable;
} registry_entry_t;

static registry_entry_t registry[MAX_BACKENDS];
static n00b_isize_t      registry_count = 0;
static bool              registry_initialized = false;

// -------------------------------------------------------------------
// Registration
// -------------------------------------------------------------------

void
n00b_renderer_register(const char                   *name,
                        const n00b_renderer_vtable_t *vtable)
{
    if (!name || !vtable) {
        return;
    }

    // Check for duplicate.
    for (n00b_isize_t i = 0; i < registry_count; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            // Update existing.
            registry[i].vtable = vtable;
            return;
        }
    }

    if (registry_count >= MAX_BACKENDS) {
        fprintf(stderr, "n00b: renderer registry full (max %d)\n", MAX_BACKENDS);
        return;
    }

    registry[registry_count].name   = name;
    registry[registry_count].vtable = vtable;
    registry_count++;
}

n00b_option_t(n00b_renderer_vtable_ptr_t)
n00b_renderer_find(const char *name)
{
    if (!name) {
        return n00b_option_none(n00b_renderer_vtable_ptr_t);
    }

    for (n00b_isize_t i = 0; i < registry_count; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            return n00b_option_set(n00b_renderer_vtable_ptr_t,
                                    registry[i].vtable);
        }
    }

    return n00b_option_none(n00b_renderer_vtable_ptr_t);
}

void
n00b_renderer_list(const char ***out_names, n00b_isize_t *out_count)
{
    static const char *names[MAX_BACKENDS];

    for (n00b_isize_t i = 0; i < registry_count; i++) {
        names[i] = registry[i].name;
    }

    if (out_names) {
        *out_names = names;
    }
    if (out_count) {
        *out_count = registry_count;
    }
}

// -------------------------------------------------------------------
// Dynamic loading
// -------------------------------------------------------------------

n00b_result_t(n00b_renderer_vtable_ptr_t)
n00b_renderer_load(const char *path)
{
    if (!path) {
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EINVAL);
    }

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "n00b: failed to load renderer '%s': %s\n",
                path, dlerror());
        return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOENT);
    }

    const n00b_renderer_plugin_t *plugin =
        (const n00b_renderer_plugin_t *)dlsym(handle, "n00b_renderer_plugin");

    if (!plugin) {
        fprintf(stderr, "n00b: no n00b_renderer_plugin symbol in '%s': %s\n",
                path, dlerror());
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOENT);
    }

    if (plugin->abi_version != N00B_RENDERER_ABI_VERSION) {
        fprintf(stderr,
                "n00b: ABI version mismatch in '%s': expected %u, got %u\n",
                path, N00B_RENDERER_ABI_VERSION, plugin->abi_version);
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EPROTO);
    }

    if (!plugin->vtable || !plugin->name) {
        fprintf(stderr, "n00b: invalid plugin in '%s'\n", path);
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EINVAL);
    }

    n00b_renderer_register(plugin->name, plugin->vtable);

    // Don't dlclose — the vtable is still in use.
    return n00b_result_ok(n00b_renderer_vtable_ptr_t, plugin->vtable);
}

n00b_result_t(n00b_renderer_vtable_ptr_t)
n00b_renderer_load_by_name(const char *name)
{
    if (!name) {
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EINVAL);
    }

    // First check if already registered.
    n00b_option_t(n00b_renderer_vtable_ptr_t) found =
        n00b_renderer_find(name);

    if (n00b_option_is_set(found)) {
        return n00b_result_ok(n00b_renderer_vtable_ptr_t,
                               n00b_option_get(found));
    }

#if defined(__APPLE__)
    const char *ext = "dylib";
#else
    const char *ext = "so";
#endif

    char path_buf[1024];

    // Search 1: $N00B_RENDERER_PATH.
    const char *search_path = getenv("N00B_RENDERER_PATH");
    if (search_path) {
        char buf[4096];
        strncpy(buf, search_path, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *saveptr = nullptr;
        char *dir     = strtok_r(buf, ":", &saveptr);

        while (dir) {
            snprintf(path_buf, sizeof(path_buf),
                     "%s/libn00b_render_%s.%s", dir, name, ext);

            n00b_result_t(n00b_renderer_vtable_ptr_t) res =
                n00b_renderer_load(path_buf);

            if (n00b_result_is_ok(res)) {
                return res;
            }
            dir = strtok_r(nullptr, ":", &saveptr);
        }
    }

    // Search 2: $HOME/.n00b/renderers/.
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path_buf, sizeof(path_buf),
                 "%s/.n00b/renderers/libn00b_render_%s.%s", home, name, ext);

        n00b_result_t(n00b_renderer_vtable_ptr_t) res =
            n00b_renderer_load(path_buf);

        if (n00b_result_is_ok(res)) {
            return res;
        }
    }

    // Search 3: /usr/local/lib/n00b/renderers/.
    snprintf(path_buf, sizeof(path_buf),
             "/usr/local/lib/n00b/renderers/libn00b_render_%s.%s", name, ext);

    n00b_result_t(n00b_renderer_vtable_ptr_t) res =
        n00b_renderer_load(path_buf);

    if (n00b_result_is_ok(res)) {
        return res;
    }

    return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOENT);
}

// -------------------------------------------------------------------
// Initialization
// -------------------------------------------------------------------

void
n00b_renderer_registry_init(void)
{
    if (registry_initialized) {
        return;
    }
    registry_initialized = true;

    n00b_renderer_register("stream", &n00b_renderer_stream);
    n00b_renderer_register("ansi",   &n00b_renderer_ansi);
    n00b_renderer_register("dumb",   &n00b_renderer_dumb);
}
