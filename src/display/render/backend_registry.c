/*
 * Backend registry: static registration, name lookup, dynamic loading.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "adt/list.h"
#include "text/strings/string_ops.h"
#include "display/render/backend_registry.h"

// -------------------------------------------------------------------
// Registry storage
// -------------------------------------------------------------------

#define MAX_BACKENDS 32

typedef struct {
    n00b_string_t                *name;
    const n00b_renderer_vtable_t *vtable;
} registry_entry_t;

static registry_entry_t registry[MAX_BACKENDS];
static n00b_isize_t      registry_count = 0;
static bool              registry_initialized = false;

// -------------------------------------------------------------------
// Registration
// -------------------------------------------------------------------

void
n00b_renderer_register(n00b_string_t                *name,
                        const n00b_renderer_vtable_t *vtable)
{
    if (!name || !vtable) {
        return;
    }

    // Check for duplicate.
    for (n00b_isize_t i = 0; i < registry_count; i++) {
        if (n00b_unicode_str_eq(registry[i].name, name)) {
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
n00b_renderer_find(n00b_string_t *name)
{
    if (!name) {
        return n00b_option_none(n00b_renderer_vtable_ptr_t);
    }

    for (n00b_isize_t i = 0; i < registry_count; i++) {
        if (n00b_unicode_str_eq(registry[i].name, name)) {
            return n00b_option_set(n00b_renderer_vtable_ptr_t,
                                    registry[i].vtable);
        }
    }

    return n00b_option_none(n00b_renderer_vtable_ptr_t);
}

n00b_list_t(n00b_string_t *)
n00b_renderer_list(void)
{
    n00b_list_t(n00b_string_t *) result = n00b_list_new(n00b_string_t *);

    for (n00b_isize_t i = 0; i < registry_count; i++) {
        n00b_list_push(result, registry[i].name);
    }

    return result;
}

// -------------------------------------------------------------------
// Dynamic loading
// -------------------------------------------------------------------

n00b_result_t(n00b_renderer_vtable_ptr_t)
n00b_renderer_load(n00b_string_t *path)
{
    if (!path) {
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EINVAL);
    }

#ifdef _WIN32
    (void)path;
    return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOTSUP);
#else
    void *handle = dlopen(path->data, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "n00b: failed to load renderer '%s': %s\n",
                path->data, dlerror());
        return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOENT);
    }

    const n00b_renderer_plugin_t *plugin =
        (const n00b_renderer_plugin_t *)dlsym(handle, "n00b_renderer_plugin");

    if (!plugin) {
        fprintf(stderr, "n00b: no n00b_renderer_plugin symbol in '%s': %s\n",
                path->data, dlerror());
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, ENOENT);
    }

    if (plugin->abi_version != N00B_RENDERER_ABI_VERSION) {
        fprintf(stderr,
                "n00b: ABI version mismatch in '%s': expected %u, got %u\n",
                path->data, N00B_RENDERER_ABI_VERSION, plugin->abi_version);
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EPROTO);
    }

    if (!plugin->vtable || !plugin->name) {
        fprintf(stderr, "n00b: invalid plugin in '%s'\n", path->data);
        dlclose(handle);
        return n00b_result_err(n00b_renderer_vtable_ptr_t, EINVAL);
    }

    n00b_renderer_register(n00b_string_from_cstr(plugin->name),
                            plugin->vtable);

    // Don't dlclose — the vtable is still in use.
    return n00b_result_ok(n00b_renderer_vtable_ptr_t, plugin->vtable);
#endif
}

n00b_result_t(n00b_renderer_vtable_ptr_t)
n00b_renderer_load_by_name(n00b_string_t *name)
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
#elif defined(_WIN32)
    const char *ext = "dll";
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
                     "%s/libn00b_render_%s.%s", dir, name->data, ext);

            n00b_result_t(n00b_renderer_vtable_ptr_t) res =
                n00b_renderer_load(n00b_string_from_cstr(path_buf));

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
                 "%s/.n00b/renderers/libn00b_render_%s.%s", home, name->data, ext);

        n00b_result_t(n00b_renderer_vtable_ptr_t) res =
            n00b_renderer_load(n00b_string_from_cstr(path_buf));

        if (n00b_result_is_ok(res)) {
            return res;
        }
    }

    // Search 3: /usr/local/lib/n00b/renderers/.
    snprintf(path_buf, sizeof(path_buf),
             "/usr/local/lib/n00b/renderers/libn00b_render_%s.%s", name->data, ext);

    n00b_result_t(n00b_renderer_vtable_ptr_t) res =
        n00b_renderer_load(n00b_string_from_cstr(path_buf));

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

    n00b_renderer_register(r"stream", &n00b_renderer_stream);
    n00b_renderer_register(r"ansi",   &n00b_renderer_ansi);
    n00b_renderer_register(r"dumb",   &n00b_renderer_dumb);

    const n00b_renderer_vtable_t *gui_vtable = nullptr;

#if defined(__APPLE__)
    n00b_renderer_register(r"cocoa", &n00b_renderer_cocoa);
    gui_vtable = &n00b_renderer_cocoa;
#endif

#if defined(N00B_HAVE_X11)
    n00b_renderer_register(r"x11", &n00b_renderer_x11);
    if (!gui_vtable) {
        gui_vtable = &n00b_renderer_x11;
    }
#endif

#if defined(N00B_HAVE_NOTCURSES)
    n00b_renderer_register(r"notcurses", &n00b_renderer_notcurses);
#endif

    if (gui_vtable) {
        // Portable GUI alias (actual window backend):
        //   macOS -> cocoa
        //   Linux/Unix -> x11
        n00b_renderer_register(r"gui", gui_vtable);
    }
}
