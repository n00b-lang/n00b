/**
 * @file crayon_subscriber.m
 * @brief Mac-only libxpc client for the Crayon warehouse data plane.
 *
 * The wire shape is straightforward: an XPC connection to the warehouse
 * mach service, one SUBSCRIBE handshake message containing the event
 * bitfield, then a stream of event dictionaries on the same connection.
 * `XPC_ERROR_CONNECTION_INTERRUPTED` triggers a handshake replay; we
 * stop trying after the server explicitly rejects the handshake.
 */

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>

#include "crayon_protocol.h"
#include "crayon_subscriber.h"

struct crayon_subscriber {
    xpc_connection_t     conn;
    dispatch_queue_t     queue;
    crayon_event_fn      handler;
    void                *user_data;
    _Atomic uint64_t     interrupts;
    _Atomic bool         closed;
    _Atomic bool         handshake_failed; // server rejected SUBSCRIBE
};

// Build a SUBSCRIBE handshake dict requesting only the normalized event
// type (bit 6).  The bitfield is exactly CRAYON_WH_EVENT_BITFIELD_LEN
// bytes long.
static xpc_object_t
build_subscribe_message(void)
{
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(msg, CRAYON_SVC_KEY_CONNECT_TYPE,
                               CRAYON_SVC_CONNECT_SUBSCRIBE);

    uint8_t bf[CRAYON_WH_EVENT_BITFIELD_LEN] = {0};
    int     bit = CRAYON_WH_EVENT_NORMALIZED;
    bf[bit / 8] = (uint8_t)(1u << (bit % 8));
    xpc_dictionary_set_data(msg, CRAYON_SVC_KEY_BITFIELD, bf, sizeof(bf));

    return msg;
}

// Send the SUBSCRIBE handshake; on rejection, mark handshake_failed so
// we stop replaying.  On a transient reply error, the next INTERRUPTED
// will retry.
static void
send_handshake(crayon_subscriber_t *s)
{
    if (atomic_load(&s->closed) || atomic_load(&s->handshake_failed)) return;

    xpc_object_t msg = build_subscribe_message();
    xpc_connection_send_message_with_reply(s->conn, msg, s->queue,
                                           ^(xpc_object_t reply) {
        if (atomic_load(&s->closed)) return;
        xpc_type_t tt = xpc_get_type(reply);
        if (tt == XPC_TYPE_DICTIONARY) {
            uint64_t status = xpc_dictionary_get_uint64(reply,
                                                       CRAYON_SVC_KEY_STATUS);
            if (status != CRAYON_SVC_STATUS_OK) {
                fprintf(stderr,
                        "crayon_subscriber: SUBSCRIBE rejected "
                        "(status=%llu); subscription is permanently dead\n",
                        (unsigned long long)status);
                atomic_store(&s->handshake_failed, true);
            }
            return;
        }
        // XPC_TYPE_ERROR or other transient — the next INTERRUPTED
        // event will retry the handshake.
    });
}

crayon_subscriber_t *
crayon_subscriber_open(crayon_event_fn handler, void *user_data)
{
    if (!handler) return NULL;

    crayon_subscriber_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->handler   = handler;
    s->user_data = user_data;
    s->queue     = dispatch_queue_create("crayon.subscriber",
                                          DISPATCH_QUEUE_SERIAL);

    s->conn = xpc_connection_create_mach_service(
        CRAYON_WAREHOUSE_MACH_SERVICE_NAME, s->queue, 0);
    if (!s->conn) {
        s->queue = NULL;
        free(s);
        return NULL;
    }

    xpc_connection_set_event_handler(s->conn, ^(xpc_object_t evt) {
        if (atomic_load(&s->closed)) return;

        if (evt == XPC_ERROR_CONNECTION_INTERRUPTED) {
            atomic_fetch_add(&s->interrupts, 1);
            // libxpc has already kicked off a reconnect on this same
            // connection; replay the handshake so the stream resumes.
            send_handshake(s);
            return;
        }
        if (evt == XPC_ERROR_CONNECTION_INVALID) {
            // Either we cancelled (close path) or the mach service is
            // gone for good.  Surface as one final error event.
            if (s->handler) s->handler(evt, s->user_data);
            return;
        }
        // Any other event — including XPC_TYPE_DICTIONARY events from
        // the server and XPC_TYPE_ERROR for transient issues — flows
        // through to the user handler unchanged.
        if (s->handler) s->handler(evt, s->user_data);
    });

    xpc_connection_resume(s->conn);
    send_handshake(s);
    return s;
}

void
crayon_subscriber_close(crayon_subscriber_t *s)
{
    if (!s) return;
    atomic_store(&s->closed, true);
    if (s->conn) {
        xpc_connection_cancel(s->conn);
        s->conn = NULL;
    }
    s->queue = NULL;
    free(s);
}

uint64_t
crayon_subscriber_interrupt_count(crayon_subscriber_t *s)
{
    return s ? atomic_load(&s->interrupts) : 0;
}

#endif // __APPLE__
