/**
 * @file crayon_subscriber.h
 * @brief Mac-only XPC subscriber for the Crayon warehouse data plane.
 *
 * Opens a connection to the warehouse mach service, subscribes to the
 * normalized event stream, and delivers each event as an `xpc_object_t`
 * dictionary on a private dispatch queue.  Reconnects automatically on
 * `XPC_ERROR_CONNECTION_INTERRUPTED` (peer crash / restart) and replays
 * the SUBSCRIBE handshake so the stream resumes without caller help.
 *
 * The handler runs on a dedicated dispatch queue — keep work in it
 * short, or hand off to a queue / thread you control.
 */
#pragma once

#ifdef __APPLE__

#include <xpc/xpc.h>

typedef struct crayon_subscriber crayon_subscriber_t;

/**
 * @brief Per-event callback.
 *
 * @param event      The event dictionary (do not retain past the call).
 * @param user_data  Opaque pointer passed through from `_open`.
 */
typedef void (*crayon_event_fn)(xpc_object_t event, void *user_data);

/**
 * @brief Open a warehouse subscription and begin receiving events.
 *
 * @return  Opaque handle on success, NULL on hard failure (mach service
 *          not registered).  Subscribe rejection arrives via the
 *          handler with an `XPC_TYPE_ERROR` event; transient connect
 *          failures self-recover.
 */
crayon_subscriber_t *crayon_subscriber_open(crayon_event_fn handler,
                                            void           *user_data);

/**
 * @brief Tear down the connection and free resources.
 *
 * Safe to call from any thread.  Pending events queued before the
 * close are dropped.
 */
void crayon_subscriber_close(crayon_subscriber_t *sub);

/**
 * @brief How many times the connection has surfaced
 *        `XPC_ERROR_CONNECTION_INTERRUPTED` since open.
 *
 * Each one is a peer crash/restart that the subscriber recovered from.
 * Useful for a "warehouse reconnect count" status pane.
 */
uint64_t crayon_subscriber_interrupt_count(crayon_subscriber_t *sub);

#endif // __APPLE__
