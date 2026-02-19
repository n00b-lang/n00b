#pragma once
/** @file style_registry.h
 *  @brief Named style and text role registry.
 *
 *  Provides a global registry (stored in `n00b_runtime_t`) of named
 *  styles (e.g., `"em"`, `"h1"`) and text roles (e.g., `"@code"`,
 *  `"@heading"`).  Styles and roles are looked up by name during
 *  rich-string formatting.
 *
 *  The registry is initialized with sensible defaults by
 *  `n00b_str_registry_init()`, which is called during `n00b_init()`.
 *
 *  ### Related modules
 *
 *  - `strings/style_ops.h` -- style construction / merge
 *  - `strings/text_style.h` -- type definitions
 *  - `core/runtime.h` -- registry storage
 */

#include "strings/style_ops.h"
#include "core/dict_untyped.h"

// ===================================================================
// Initialization
// ===================================================================

/** @brief Initialize the style and role registries with defaults.
 *
 *  Called automatically by `n00b_init()`.  Registers built-in named
 *  styles (`em`, `h1`-`h3`, etc.) and text roles (`@code`, `@heading`,
 *  etc.).
 *
 *  @pre The runtime must be initialized (allocator available).
 *  @post `n00b_str_style_lookup()` and `n00b_str_role_lookup()` are
 *        functional.
 */
void n00b_str_registry_init(void);

// ===================================================================
// Named styles
// ===================================================================

/** @brief Register a named style.
 *  @param name   NUL-terminated name (e.g., `"em"`).
 *  @param style  Style to associate (copied into registry).
 *  @pre Registry has been initialized.
 */
void n00b_str_style_register(const char *name,
                              const n00b_text_style_t *style);

/** @brief Look up a named style.
 *  @param name  NUL-terminated name.
 *  @return The style pointer, or nullptr if not found.
 */
n00b_text_style_t *n00b_str_style_lookup(const char *name);

// ===================================================================
// Text roles
// ===================================================================

/** @brief Register a text role.
 *  @param name   NUL-terminated role name (e.g., `"@code"`).
 *  @param style  Style to associate (copied into registry).
 */
void n00b_str_role_register(const char *name,
                             const n00b_text_style_t *style);

/** @brief Look up a text role.
 *  @param name  NUL-terminated role name.
 *  @return The style pointer, or nullptr if not found.
 */
n00b_text_style_t *n00b_str_role_lookup(const char *name);
