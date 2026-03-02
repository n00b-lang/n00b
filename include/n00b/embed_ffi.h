#pragma once

/**
 * @file embed_ffi.h
 * @brief FFI embed literal handler.
 *
 * Registers the `'ffi` embed handler, which parses FFI binding
 * declarations and returns an `n00b_ffi_module_t` object.  Calling
 * `install()` on the object generates MIR wrapper functions that
 * bridge between n00b calling conventions and C ABI.
 *
 * ## Example (inside a comptime block)
 *
 * ```n00b
 * comptime {
 *     ffi_mod = [=[
 *         strlen : (string -> cstr) -> i64 = strlen
 *         write  : (i32, buffer -> (ptr, len)) -> i64 = write
 *     ]=]'ffi
 *
 *     ffi_mod.install()
 * }
 * ```
 *
 * Each binding line declares:
 * - n00b-side name and signature
 * - Per-parameter conversion (n00b type -> C type)
 * - C symbol to link against
 * - Optional: keyword args, ownership, error mapping
 */

#include "n00b/embed.h"

/**
 * @brief Opaque FFI module object returned by the `'ffi` embed handler.
 *
 * Holds parsed binding declarations.  Call `install()` (via the
 * extension method) to emit MIR wrapper functions into the active
 * codegen session.
 */
typedef struct n00b_ffi_module_t {
    void    *bindings;      /**< Internal ffi_binding_t array. */
    int32_t  binding_count; /**< Number of bindings. */
    void    *session;       /**< `n00b_cg_session_t *` that created this. */
    bool     installed;     /**< True after install() has been called. */
} n00b_ffi_module_t;

/**
 * @brief Get the BNF grammar text for the FFI mini-language.
 * @return Static string containing the FFI BNF.
 */
extern n00b_string_t *n00b_ffi_bnf(void);

/**
 * @brief Register the `'ffi` embed handler on a registry.
 * @param registry  Embed handler registry.
 */
extern void n00b_ffi_embed_register(n00b_dict_untyped_t *registry);

/**
 * @brief Install all FFI bindings from a module, emitting MIR wrappers.
 * @param self  The FFI module object.
 */
extern void n00b_ffi_module_install(n00b_ffi_module_t *self);

/**
 * @brief Register the `n00b_ffi_module_t` type and its `install` method.
 *
 * Called during startup (after type registry init) so that comptime
 * method dispatch can find the `install` extension method.
 */
extern void n00b_ffi_module_type_register(void);
