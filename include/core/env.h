#pragma once
/** @file env.h
 *  @brief libn00b environment-variable accessors backed by the
 *         `n00b_init()` envp cache.
 *
 *  Consumer code should call these instead of libc `getenv()` /
 *  `setenv()` / `putenv()` so the same env view is observable from
 *  ncc-extended code that wants `n00b_string_t *` values and from
 *  libc code that still calls `getenv()`.  The implementation keeps
 *  `__environ` and `n00b_get_runtime()->envp` in lock-step.
 *
 *  Growth (via `n00b_putenv`) allocates from the runtime's
 *  `system_pool`, which is intentionally non-arena and non-GC-scanned,
 *  so `__environ` never needs to be registered as a GC root.
 */

#include "n00b.h"
#include "core/string.h"

/**
 * @brief Look up an environment variable.
 *
 * @param name  Variable name.  May be `nullptr` or empty (returns
 *              `nullptr`).
 * @return  The value as an `n00b_string_t *`, or `nullptr` if @p
 *          name is not set in the cached envp.
 *
 * @note Reads are unsynchronized; callers that race with
 *       `n00b_putenv` from another thread must coordinate externally.
 */
extern n00b_string_t *
n00b_getenv(n00b_string_t *name);

/**
 * @brief Set or replace an environment variable.
 *
 * Replaces the existing slot if @p name is already set; otherwise
 * grows the envp cache by one slot.  Growth allocates the new slot
 * array and the new `NAME=value` storage from the runtime's
 * `system_pool` and rebinds the libc-visible `__environ` to the new
 * slot array, so any non-n00b code that still calls libc `getenv()`
 * sees the same value.
 *
 * @param name   Variable name.  Must be a non-empty string with no
 *               `'='` byte.
 * @param value  Variable value.  May be empty.
 * @return  `true` on success, `false` on invalid input (null name,
 *          empty name, name containing `'='`, or null value).
 *
 * @note Not thread-safe with concurrent `n00b_getenv` from other
 *       threads.  Callers that mutate env at runtime must coordinate
 *       externally.
 */
extern bool
n00b_putenv(n00b_string_t *name, n00b_string_t *value);
