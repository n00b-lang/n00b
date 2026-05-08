/*
 * file_change.c - Filesystem change monitoring implementation for conduit
 */

#include "conduit/conduit.h"
#include "conduit/file_change.h"
#include "conduit/io.h"

// ============================================================================
// File Change Watch Creation
// ============================================================================

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_file_change_topic(n00b_conduit_t *c, int fd, uint32_t events)
{
    if (!c) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }
    if (fd < 0) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }

    // Allocate and initialize the watch struct.
    n00b_conduit_vnode_watch_t *watch =
        n00b_alloc_with_opts(n00b_conduit_vnode_watch_t,
            &(n00b_alloc_opts_t){.allocator = c->allocator});

    watch->fd   = fd;
    watch->ops  = events;
    watch->next = nullptr;

    // Create or retrieve the topic for this fd.
    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_VNODE(fd),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_file_change_payload_t)));
    if (n00b_result_is_err(topic_res)) {
        return topic_res;
    }
    watch->topic = n00b_result_get(topic_res);

    // Register with I/O backend.
    if (!n00b_conduit_file_change_register(c, watch)) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_ALLOC);
    }

    return n00b_result_ok(n00b_conduit_topic_base_t *, watch->topic);
}

void
n00b_conduit_file_change_unwatch(n00b_conduit_t *c, int fd)
{
    if (!c || fd < 0) {
        return;
    }

    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_VNODE(fd), 0);
    if (n00b_result_is_ok(topic_res)) {
        n00b_conduit_topic_close(n00b_result_get(topic_res));
    }
}

// ============================================================================
// File Change Registration (with I/O backend)
// ============================================================================

bool
n00b_conduit_file_change_register(n00b_conduit_t *c,
                                   n00b_conduit_vnode_watch_t *watch)
{
    if (!c || !watch) {
        return false;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return false;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->vnode_add) {
        return false;
    }

    return io->ops->vnode_add(io->ctx, watch);
}

void
n00b_conduit_file_change_unregister(n00b_conduit_t *c,
                                     n00b_conduit_vnode_watch_t *watch)
{
    if (!c || !watch) {
        return;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->vnode_remove) {
        return;
    }

    io->ops->vnode_remove(io->ctx, watch);
}

// ============================================================================
// File Change Firing
// ============================================================================

void
n00b_conduit_vnode_fire(n00b_conduit_vnode_watch_t *watch, uint32_t ops)
{
    if (!watch || !watch->topic) {
        return;
    }

    n00b_conduit_t *c = watch->topic->conduit;
    if (!c || n00b_conduit_is_shutdown(c)) {
        return;
    }

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(watch->topic);
    if (n00b_result_is_err(pub_res)) {
        n00b_atomic_add(&watch->topic->epoch, 1);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_conduit_file_change_msg_t *msg =
        n00b_alloc(n00b_conduit_file_change_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = watch->topic;
    msg->header.generation = n00b_conduit_topic_generation(watch->topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(watch->topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.fd     = watch->fd;
    msg->payload.events = ops;

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_file_change_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_file_change_payload_t) *)watch->topic,
        msg,
        ops);

    n00b_conduit_publish_yield(pub);

    // DELETE and REVOKE are terminal — the watched resource is gone.
    if (ops & (N00B_CONDUIT_VNODE_DELETE | N00B_CONDUIT_VNODE_REVOKE)) {
        n00b_conduit_topic_close(watch->topic);
    }
}
