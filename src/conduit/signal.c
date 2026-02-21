/*
 * signal.c - Unix signal handling implementation for conduit
 */

#ifndef _WIN32

#include "conduit/conduit.h"
#include "conduit/signal.h"
#include "conduit/io.h"

// ============================================================================
// Signal Watch Creation
// ============================================================================

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_signal_topic(n00b_conduit_t *c, int signum)
{
    if (!c) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }
    if (signum <= 0) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }

    // Allocate and initialize signal watch
    n00b_conduit_signal_watch_t *watch =
        n00b_alloc(n00b_conduit_signal_watch_t);

    watch->signum = signum;
    n00b_atomic_store(&watch->raise_count, (uint64_t)0);
    watch->next   = nullptr;

    // Create topic for this signal
    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_SIGNAL(signum),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_signal_payload_t)));
    if (n00b_result_is_err(topic_res)) {
        return topic_res;
    }
    watch->topic = n00b_result_get(topic_res);

    // Register with I/O backend
    if (!n00b_conduit_signal_register(c, watch)) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_ALLOC);
    }

    return n00b_result_ok(n00b_conduit_topic_base_t *, watch->topic);
}

void
n00b_conduit_signal_unwatch(n00b_conduit_t *c, int signum)
{
    if (!c || signum <= 0) {
        return;
    }

    // Close the topic so subscribers get TOPIC_CLOSED
    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_SIGNAL(signum), 0);
    if (n00b_result_is_ok(topic_res)) {
        n00b_conduit_topic_close(n00b_result_get(topic_res));
    }
}

// ============================================================================
// Signal Registration (with I/O backend)
// ============================================================================

bool
n00b_conduit_signal_register(n00b_conduit_t *c,
                             n00b_conduit_signal_watch_t *watch)
{
    if (!c || !watch) {
        return false;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return false;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->signal_add) {
        return false;
    }

    return io->ops->signal_add(io->ctx, watch);
}

void
n00b_conduit_signal_unregister(n00b_conduit_t *c,
                               n00b_conduit_signal_watch_t *watch)
{
    if (!c || !watch) {
        return;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->signal_remove) {
        return;
    }

    io->ops->signal_remove(io->ctx, watch);
}

// ============================================================================
// Signal Firing
// ============================================================================

void
n00b_conduit_signal_fire(n00b_conduit_signal_watch_t *watch)
{
    if (!watch || !watch->topic) {
        return;
    }

    n00b_conduit_t *c = watch->topic->conduit;
    if (!c || n00b_conduit_is_shutdown(c)) {
        return;
    }

    n00b_atomic_add(&watch->raise_count, 1);

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(watch->topic);
    if (n00b_result_is_err(pub_res)) {
        // Bump epoch so subscribers can detect the missed signal.
        n00b_atomic_add(&watch->topic->epoch, 1);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_conduit_signal_msg_t *msg = n00b_alloc(n00b_conduit_signal_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = watch->topic;
    msg->header.generation = n00b_conduit_topic_generation(watch->topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(watch->topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.signum      = watch->signum;
    msg->payload.raise_count = n00b_atomic_load(&watch->raise_count);

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_signal_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_signal_payload_t) *)watch->topic,
        msg,
        N00B_CONDUIT_SIGNAL_RAISED);

    n00b_conduit_publish_yield(pub);
}

#endif // !_WIN32
