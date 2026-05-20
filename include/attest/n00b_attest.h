#pragma once

/**
 * @file n00b_attest.h
 * @brief Umbrella header for the n00b attest module.
 *
 * Single include for callers that want every public n00b_attest
 * symbol. The module produces in-toto Statement v1 documents,
 * wraps them in DSSE envelopes, and (in later WPs) signs them
 * against pluggable signer backends and publishes them to OCI
 * 1.1 registries as referrers.
 *
 * # Scope of this header
 *
 * This is the ncc-flavor public surface. It pulls in libn00b's
 * own umbrella (`<n00b.h>`) for the runtime types
 * (`n00b_string_t`, `n00b_buffer_t`, `n00b_result_t(T)`,
 * `n00b_allocator_t`, etc.) the per-area headers depend on, then
 * `#include`s each per-area header. Non-ncc callers do not
 * consume this header directly; the C-ABI flattening that was
 * originally planned is deferred indefinitely per D-022 (Crayon
 * is migrating to ncc, eliminating the last plain-C consumer).
 *
 * # Symbol-prefix discipline
 *
 * Every exported symbol in this module begins with the
 * `n00b_attest_` prefix. Constants and macros use the
 * `N00B_ATTEST_` upper-case form (e.g. @ref
 * N00B_ATTEST_API_VERSION). The in-container client
 * (`co_attest_*`) is a separate project (IC-2) and does not
 * appear here.
 */

#include <n00b.h>

/**
 * @brief Integer version of the n00b_attest module's public API.
 *
 * Bumped on any breaking change to the symbol surface. Phase 1
 * lays the module down at version 1; subsequent WPs may bump
 * this when introducing breaking changes to header-visible
 * signatures.
 */
#define N00B_ATTEST_API_VERSION 1

#include <attest/n00b_attest_error.h>
#include <attest/n00b_attest_statement.h>
#include <attest/n00b_attest_signer.h>
#include <attest/n00b_attest_verifier.h>
#include <attest/n00b_attest_dsse.h>
#include <attest/n00b_attest_cli.h>
#include <attest/n00b_attest_oci.h>

/**
 * @brief Module init stub.
 *
 * Phase-1 placeholder symbol the build smoke uses to verify
 * meson wiring: if the module's source file is mis-routed the
 * link of any test linking libn00b will fail to resolve this
 * symbol. Has no runtime effect and is **not** a constructor —
 * the module carries no global state (per api guidelines §10.9).
 */
extern void n00b_attest_module_init(void);
