/**
 * @file n00b_demangle.h
 * @brief Symbol name demangling for C++ (Itanium ABI) and Rust (v0).
 *
 * Provides demangling of mangled symbol names found in ELF and Mach-O
 * binaries.  Supports the Itanium C++ ABI mangling scheme (symbols
 * beginning with `_Z` or `__Z`) and Rust v0 mangling (symbols beginning
 * with `_R`).
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "adt/option.h"

/**
 * @brief Demangle a symbol name, auto-detecting the mangling scheme.
 *
 * Tries C++ Itanium first, then Rust v0.  If the name is not recognized
 * as mangled, returns a copy of the original name.
 *
 * @param mangled  NUL-terminated mangled symbol name.
 * @return Demangled name, or copy of original if not mangled.
 *         Returns nullptr only if `mangled` is nullptr.
 */
extern n00b_string_t *n00b_demangle(const char *mangled);

/**
 * @brief Demangle a C++ Itanium ABI mangled name.
 *
 * Handles `_Z...` (standard) and `__Z...` (macOS extra underscore).
 * Returns a copy of the original on parse failure.
 *
 * @param mangled  NUL-terminated mangled symbol name.
 * @return Demangled name, or copy of original on failure.
 */
extern n00b_string_t *n00b_demangle_itanium(const char *mangled);

/**
 * @brief Demangle a Rust v0 mangled name.
 *
 * Handles `_R...` symbols per RFC 2603.
 *
 * @param mangled  NUL-terminated mangled symbol name.
 * @return Some(demangled name) on success; none on parse failure or if
 *         @p mangled is nullptr.
 */
extern n00b_option_t(n00b_string_t *) n00b_demangle_rust(const char *mangled);

/**
 * @brief Check whether a symbol name appears to be mangled.
 *
 * Returns true for C++ Itanium (`_Z`, `__Z`) and Rust v0 (`_R`) prefixes.
 */
extern bool n00b_is_mangled(const char *name);
