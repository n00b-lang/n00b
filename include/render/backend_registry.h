/**
 * @file backend_registry.h
 * @brief Backend registration, lookup, and dynamic loading.
 *
 * Backends can be registered statically (built-in) or loaded
 * dynamically from shared libraries via `dlopen`/`dlsym`.
 *
 * ### Plugin shared library interface
 *
 * A backend `.so`/`.dylib` exports exactly one symbol:
 *
 * ```c
 * extern const n00b_renderer_plugin_t n00b_renderer_plugin;
 * ```
 *
 * ### Search paths for `n00b_renderer_load_by_name()`
 *
 * 1. `$N00B_RENDERER_PATH` (colon-separated)
 * 2. `$HOME/.n00b/renderers/`
 * 3. `<libdir>/n00b/renderers/`
 *
 * Looks for: `libn00b_render_<name>.so` / `.dylib`
 *
 * ### Related modules
 *
 * - `render/backend.h` — vtable and capability types
 */
#pragma once

#include "n00b.h"
#include "core/option.h"
#include "core/result.h"
#include "core/array.h"
#include "core/string.h"
#include "core/list.h"
#include "render/backend.h"

// Type-safe wrappers for registry return types.
typedef const n00b_renderer_vtable_t *n00b_renderer_vtable_ptr_t;
n00b_option_decl(n00b_renderer_vtable_ptr_t);
n00b_result_decl(n00b_renderer_vtable_ptr_t);
n00b_array_decl(n00b_renderer_vtable_ptr_t);

// ====================================================================
// Plugin structure
// ====================================================================

/**
 * @brief Plugin export structure for dynamically loaded backends.
 *
 * The shared library must define:
 * `extern const n00b_renderer_plugin_t n00b_renderer_plugin;`
 */
typedef struct n00b_renderer_plugin_t {
    uint32_t                      abi_version;
    const char                   *name;
    const n00b_renderer_vtable_t *vtable;
} n00b_renderer_plugin_t;

// ====================================================================
// Registration
// ====================================================================

/**
 * @brief Register a backend by name.
 *
 * Built-in backends are auto-registered at init time.
 *
 * @param name   Backend name (e.g. "ansi", "gtk4").
 * @param vtable Backend vtable.
 *
 * @pre `name` and `vtable` are non-null.
 */
extern void n00b_renderer_register(n00b_string_t                 name,
                                    const n00b_renderer_vtable_t *vtable);

/**
 * @brief Look up a registered backend by name.
 * @param name Backend name.
 * @return     Option containing vtable pointer, or none if not found.
 */
extern n00b_option_t(n00b_renderer_vtable_ptr_t)
    n00b_renderer_find(n00b_string_t name);

/**
 * @brief List all registered backend names.
 * @return List of backend name strings.
 */
extern n00b_list_t(n00b_string_t) n00b_renderer_list(void);

// ====================================================================
// Dynamic loading
// ====================================================================

/**
 * @brief Load a renderer from a shared library at the given path.
 *
 * The library must export `n00b_renderer_plugin` of type
 * `n00b_renderer_plugin_t`.  On success, the backend is auto-registered.
 *
 * @param path Path to the shared library.
 * @return     Result containing vtable pointer on success, or error code.
 */
extern n00b_result_t(n00b_renderer_vtable_ptr_t)
    n00b_renderer_load(n00b_string_t path);

/**
 * @brief Search standard paths for a backend named `name`.
 *
 * Searches `$N00B_RENDERER_PATH`, `~/.n00b/renderers/`,
 * `<libdir>/n00b/renderers/` for `libn00b_render_<name>.so`/`.dylib`.
 *
 * @param name Backend name.
 * @return     Result containing vtable pointer, or error code if not found.
 */
extern n00b_result_t(n00b_renderer_vtable_ptr_t)
    n00b_renderer_load_by_name(n00b_string_t name);

/**
 * @brief Initialize the backend registry with built-in backends.
 *
 * Called automatically during `n00b_init()`.  Registers ansi, dumb,
 * and stream backends.
 */
extern void n00b_renderer_registry_init(void);
