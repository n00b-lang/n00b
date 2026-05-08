/*
 * user_event.c - User-triggered event implementation for conduit
 */

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
 || defined(__NetBSD__) || defined(__linux__) || defined(_WIN32)

#include "conduit/conduit.h"
#include "conduit/user_event.h"
#include "conduit/io.h"

// ============================================================================
// User Event Creation
// ============================================================================

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_user_event_new(n00b_conduit_t *c)
{
    if (!c) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }

    uint64_t event_id = n00b_atomic_add(&c->next_user_event_id, 1) + 1;

    n00b_conduit_user_event_t *event = n00b_alloc_with_opts(n00b_conduit_user_event_t,
                                         &(n00b_alloc_opts_t){.allocator = c->allocator});

    event->event_id = event_id;
    n00b_atomic_store(&event->trigger_count, (uint64_t)0);
    event->next     = nullptr;

    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_USER_EVENT(event_id),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_user_event_payload_t)));
    if (n00b_result_is_err(topic_res)) {
        return topic_res;
    }
    event->topic = n00b_result_get(topic_res);

    if (!n00b_conduit_user_event_register(c, event)) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_ALLOC);
    }

    return n00b_result_ok(n00b_conduit_topic_base_t *, event->topic);
}

void
n00b_conduit_user_event_trigger(n00b_conduit_t *c,
                                n00b_conduit_topic_base_t *topic)
{
    if (!c || !topic) {
        return;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->user_event_trigger) {
        return;
    }

    uint64_t event_id = n00b_conduit_user_event_id(topic);
    if (event_id == 0) {
        return;
    }

    n00b_conduit_user_event_t tmp = {
        .event_id = event_id,
        .topic    = topic,
    };

    io->ops->user_event_trigger(io->ctx, &tmp);
}

void
n00b_conduit_user_event_destroy(n00b_conduit_t *c,
                                n00b_conduit_topic_base_t *topic)
{
    if (!c || !topic) {
        return;
    }

    n00b_conduit_topic_close(topic);
}

// ============================================================================
// User Event Registration (with I/O backend)
// ============================================================================

bool
n00b_conduit_user_event_register(n00b_conduit_t *c,
                                 n00b_conduit_user_event_t *event)
{
    if (!c || !event) {
        return false;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return false;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->user_event_add) {
        return false;
    }

    return io->ops->user_event_add(io->ctx, event);
}

void
n00b_conduit_user_event_unregister(n00b_conduit_t *c,
                                   n00b_conduit_user_event_t *event)
{
    if (!c || !event) {
        return;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->user_event_remove) {
        return;
    }

    io->ops->user_event_remove(io->ctx, event);
}

// ============================================================================
// User Event Firing
// ============================================================================

void
n00b_conduit_user_event_fire(n00b_conduit_user_event_t *event)
{
    if (!event || !event->topic) {
        return;
    }

    n00b_conduit_t *c = event->topic->conduit;
    if (!c || n00b_conduit_is_shutdown(c)) {
        return;
    }

    n00b_atomic_add(&event->trigger_count, 1);

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(event->topic);
    if (n00b_result_is_err(pub_res)) {
        // Bump epoch so subscribers can detect the missed event.
        n00b_atomic_add(&event->topic->epoch, 1);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_conduit_user_event_msg_t *msg =
        n00b_alloc(n00b_conduit_user_event_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = event->topic;
    msg->header.generation = n00b_conduit_topic_generation(event->topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(event->topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.event_id      = event->event_id;
    msg->payload.trigger_count = n00b_atomic_load(&event->trigger_count);

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_user_event_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_user_event_payload_t) *)event->topic,
        msg,
        N00B_CONDUIT_USER_EVENT_TRIGGERED);

    n00b_conduit_publish_yield(pub);
}

#endif // kqueue/Linux platforms
