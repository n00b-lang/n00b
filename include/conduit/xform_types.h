/**
 * @file xform_types.h
 * @brief Centralized typed pipeline instantiations for common payload types.
 *
 * Provides `N00B_CONDUIT_FULL_IMPL` and `N00B_CONDUIT_FILTER_IMPL` for
 * `n00b_buffer_t *`, the common wire type used by linebuf, ANSI strip,
 * hexdump, and other byte-oriented transforms.
 *
 * Include this instead of redeclaring per-type IMPL macros.
 */
#pragma once

#include "conduit/xform.h"
#include "core/buffer.h"

// ============================================================================
// n00b_buffer_t * — the standard wire type for byte-oriented pipelines
// ============================================================================

// n00b_buffer_t * FULL_IMPL and option_decl are in fd_managed.h
// (needed there for the FD read topic type).
N00B_CONDUIT_FILTER_IMPL(n00b_buffer_t *);
