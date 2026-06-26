/**
 * @file types.h
 * @brief Shared types, enums, and error codes for the VKS subsystem.
 *
 * VKS (virtual KV store) is a parameterized container — a typed dict plus a
 * (Phase 2) pluggable durability backend.  This header is included by all
 * other VKS headers.  It provides:
 * - VKS-specific error codes (offset 3000+ to avoid collision with conduit
 *   error codes at 1000+ and VFS error codes at 2000+).
 * - A code->string helper mirroring `n00b_json_err_str` / `n00b_file_err_str`.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "adt/option.h"

// ============================================================================
// Error codes
//
// VKS operations return n00b_result_t with these domain-specific error
// codes (offset 3000 to avoid collision with errno, conduit errors at
// 1000+, and VFS errors at 2000+).
// ============================================================================

enum {
    N00B_VKS_ERR_NONE          = 0,
    N00B_VKS_ERR_NULL_ARG      = 3000,
    N00B_VKS_ERR_ALLOC,
    N00B_VKS_ERR_NOT_FOUND,
    N00B_VKS_ERR_EXISTS,
    N00B_VKS_ERR_BACKEND,
    N00B_VKS_ERR_NOT_SUPPORTED,
    N00B_VKS_ERR_IO,
    N00B_VKS_ERR_CLOSED,
};

// ============================================================================
// Utility: error code to string
// ============================================================================

/**
 * @brief Return a human-readable name for a VKS error code.
 * @param err  Error code from the N00B_VKS_ERR_* enum.
 * @return     Rich string, e.g. "NOT_FOUND".  "UNKNOWN" for unrecognized codes.
 */
extern n00b_string_t *n00b_vks_err_str(n00b_err_t err);
