/**
 * @file compile_common.c
 * @brief Shared compilation utilities used by both bootstrap and main NCC.
 *
 * Provides compiler discovery, passthrough mode, and C preprocessor
 * invocation. The actual compile_file() entry point is provided
 * separately by each backend (compile_packrat.c / compile_pwz.c).
 */

#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "compile.h"
#include "buf.h"

// Top-level compiler logic.

#if !defined(NCC_DEFAULT_CC)
#define NCC_DEFAULT_CC "cc"
#endif

#if !defined(NCC_COMPILER_ENV)
#define NCC_COMPILER_ENV "NCC_COMPILER"
#endif

#if !defined(NCC_SYS_COMPILER_ENV)
#define NCC_SYS_COMPILER_ENV "CC"
#endif

// Check if a path refers to ncc itself (to avoid infinite recursion).
// Matches "ncc", "/path/to/ncc", "ncc-bootstrap", etc.
static bool
is_ncc_path(const char *path)
{
    if (!path) {
        return false;
    }

    // Find the basename
    const char *base = strrchr(path, '/');
    base             = base ? base + 1 : path;

    // Match if basename is "ncc" or starts with "ncc-"
    if (strcmp(base, "ncc") == 0) {
        return true;
    }
    if (strncmp(base, "ncc-", 4) == 0) {
        return true;
    }

    return false;
}

// First, we look in the NCC_COMPILER env var, then check CC, and if
// we find nothing, we use "cc" and will rely on the path searching in
// execvp.
//
// We skip any value that points to ncc itself to avoid infinite recursion
// when ncc is used as the CC wrapper.

char *
ncc_find_compiler(void)
{
    char *result = getenv(NCC_COMPILER_ENV);

    if (!result || is_ncc_path(result)) {
        result = getenv(NCC_SYS_COMPILER_ENV);
    }
    if (!result || is_ncc_path(result)) {
        result = NCC_DEFAULT_CC;
    }
    // If NCC_DEFAULT_CC itself points to ncc (e.g. when the full ncc was
    // compiled with CC=ncc-bootstrap), fall back to "clang".
    if (is_ncc_path(result)) {
        result = "clang";
    }

    return result;
}
