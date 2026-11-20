#pragma once

#include "lib/string.h"

// The package id of the declarations the compiler makes itself (module
// `builtins`). It is no path and not "default", so no source module has it.
#define N00BB_COMPILER_PACKAGE CORE_STRING_STATIC("<compiler>")

// Whether a declaration is one the compiler makes itself: its package is
// N00BB_COMPILER_PACKAGE, as is the link-unit module its link declarations
// live in. This is the one test; the module name `builtins` says nothing,
// since a default-package builtins.n is the user's.
static inline bool
n00bb_compiler_package(core_string_t package)
{
    return core_string_eq(package, N00BB_COMPILER_PACKAGE);
}

// Whether a package is a system one: the standard library's (its package
// ids lie under the canonical library directory) or the compiler's own.
// This is the one test of system-ness; a module's name says nothing, since
// a default-package module may be called `builtin` or `sys`.
bool n00bb_system_package(core_string_t package_id);
// Records the canonical standard library directory; the resolver sets it.
void n00bb_system_set_stdlib(core_string_t canonical);
bool n00bb_generated_source(core_string_t path);
bool n00bb_reserved_name(core_string_t name);
bool n00bb_builtin_member(core_string_t name);
