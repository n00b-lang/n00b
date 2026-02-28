/**
 * @file xform_ansi_strip.h
 * @brief ANSI escape sequence stripping transform for conduit pipelines.
 *
 * Wraps `n00b_ansi_parse()` and `n00b_ansi_nodes_to_string()` to remove
 * all ANSI/VT escape sequences from a byte stream, passing through only
 * the text content.  Newlines and tabs are preserved.
 *
 * ### Usage
 *
 * ```c
 * auto r = n00b_conduit_ansi_strip_new(conduit, upstream_topic);
 * auto xf = n00b_result_get(r);
 * ```
 */
#pragma once

#include "conduit/xform_types.h"
#include "text/strings/ansi.h"

// ============================================================================
// API
// ============================================================================

/**
 * @brief Create an ANSI-stripping filter transform.
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_buffer_t *` payloads.
 * @return         Result with filter pointer on success.
 */
extern n00b_result_t(n00b_conduit_filter_t(n00b_buffer_t *) *)
n00b_conduit_ansi_strip_new(n00b_conduit_t                        *c,
                            n00b_conduit_topic_t(n00b_buffer_t *)  *upstream);

// ============================================================================
// Chain spec
// ============================================================================

typedef struct {
    n00b_conduit_xform_spec_base_t base;
} n00b_conduit_ansi_strip_spec_t;

extern const n00b_conduit_ansi_strip_spec_t n00b_conduit_ansi_strip_default_spec;
