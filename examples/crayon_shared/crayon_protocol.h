/**
 * @file crayon_protocol.h
 * @brief Wire-level constants for talking to a running Crayon daemon's
 *        warehouse service over XPC.
 *
 * These constants are the public client/service contract documented in
 * Crayon's `include/private/crayon_svc/protocol.h` and `include/private/
 * warehouse/protocol.h`.  Crayon's own header file says:
 *
 *     "Bit-position values are part of the public contract.  Never
 *      reorder; append new entries at the end and bump the protocol
 *      version if the meaning of an existing slot changes."
 *
 * We reproduce only what an external consumer needs to subscribe to the
 * normalized data plane.  No closed-source headers or binaries are
 * required to build against this file.
 */
#pragma once

#include <stdint.h>

#ifdef __APPLE__

// ---------------------------------------------------------------------------
// Mach service
// ---------------------------------------------------------------------------

#define CRAYON_WAREHOUSE_MACH_SERVICE_NAME "com.crashoverride.crayon.svc.warehouse"

// ---------------------------------------------------------------------------
// XPC dict keys
// ---------------------------------------------------------------------------

#define CRAYON_SVC_KEY_CONNECT_TYPE  "crayon.connect_type"
#define CRAYON_SVC_KEY_BITFIELD      "crayon.event_bitfield"
#define CRAYON_SVC_KEY_STATUS        "crayon.status"

#define CRAYON_SVC_KEY_FORMAT_VERSION "crayon.format_version"
#define CRAYON_SVC_KEY_EVENT_TYPE     "crayon.event_type"
#define CRAYON_SVC_KEY_TIMESTAMP_NS   "crayon.timestamp_ns"
#define CRAYON_SVC_KEY_SOURCE_SERVICE "crayon.source_service"

// ---------------------------------------------------------------------------
// Connect types
// ---------------------------------------------------------------------------

#define CRAYON_SVC_CONNECT_SUBSCRIBE 1

// ---------------------------------------------------------------------------
// Status codes (server reply on the SUBSCRIBE handshake)
// ---------------------------------------------------------------------------

#define CRAYON_SVC_STATUS_OK            0
#define CRAYON_SVC_STATUS_FULL          1
#define CRAYON_SVC_STATUS_BUSY          2
#define CRAYON_SVC_STATUS_NOT_READY     3
#define CRAYON_SVC_STATUS_UNAUTHORIZED  4
#define CRAYON_SVC_STATUS_BAD_REQUEST   5
#define CRAYON_SVC_STATUS_NOT_FOUND     6
#define CRAYON_SVC_STATUS_INTERNAL      7

// ---------------------------------------------------------------------------
// Warehouse-specific event types
// ---------------------------------------------------------------------------
//
// The warehouse publishes operational rollup events plus a single data
// plane: every post-policy KEPT event is republished on the bus as
// CRAYON_WH_EVENT_NORMALIZED with the schema documented in Crayon's
// `docs/warehouse/EVENT_SCHEMA.md`.

#define CRAYON_WH_EVENT_POLICY_LOADED      0
#define CRAYON_WH_EVENT_POLICY_REJECTED    1
#define CRAYON_WH_EVENT_DESTINATION_UP     2
#define CRAYON_WH_EVENT_DESTINATION_DOWN   3
#define CRAYON_WH_EVENT_QUEUE_OVERFLOW     4
#define CRAYON_WH_EVENT_FLATTENER_MISSING  5
#define CRAYON_WH_EVENT_NORMALIZED         6
#define CRAYON_WH_EVENT_TYPE_LAST          7

#define CRAYON_WH_EVENT_BITFIELD_LEN       ((CRAYON_WH_EVENT_TYPE_LAST + 7u) / 8u)

// ---------------------------------------------------------------------------
// Classification category bits (subset relevant to "developer activity")
// ---------------------------------------------------------------------------
//
// `actor.classification.categories` is a uint64 bitmask whose bits
// correspond to the values below.  The full list lives in
// `include/private/classifier/protocol.h`; here we only reproduce the
// process-invocation bits the demo needs.

#define CRAYON_CLASSIFY_CATEGORY_AI               0
#define CRAYON_CLASSIFY_CATEGORY_EDITOR           1
#define CRAYON_CLASSIFY_CATEGORY_SHELL            2
#define CRAYON_CLASSIFY_CATEGORY_COMPANY          3
#define CRAYON_CLASSIFY_CATEGORY_MCP              4
#define CRAYON_CLASSIFY_CATEGORY_COMPILER         18
#define CRAYON_CLASSIFY_CATEGORY_PACKAGE_MANAGER  19
#define CRAYON_CLASSIFY_CATEGORY_INTERPRETER      20
#define CRAYON_CLASSIFY_CATEGORY_BUILD_TOOL       21

#define CRAYON_CLASSIFY_BIT(n) (1ULL << (n))

#endif // __APPLE__
