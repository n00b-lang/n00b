/*
 * proc_lifecycle.c - Process lifecycle monitoring implementation for conduit
 */

#ifndef _WIN32

#include "conduit/conduit.h"
#include "conduit/proc_lifecycle.h"
#include "conduit/io.h"
#include <sys/wait.h>

// ============================================================================
// Process Watch Creation
// ============================================================================

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_proc_topic(n00b_conduit_t *c, pid_t pid, uint32_t events)
{
    if (!c) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }
    if (pid <= 0) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    }

    // Allocate and initialize the watch struct.
    n00b_conduit_proc_watch_t *watch =
        n00b_alloc_with_opts(n00b_conduit_proc_watch_t,
            &(n00b_alloc_opts_t){.allocator = c->allocator});

    watch->pid  = pid;
    watch->ops  = events;
    watch->next = nullptr;

    // Create or retrieve the topic for this pid.
    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_PROC(pid),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_proc_payload_t)));
    if (n00b_result_is_err(topic_res)) {
        return topic_res;
    }
    watch->topic = n00b_result_get(topic_res);

    // Register with I/O backend.
    if (!n00b_conduit_proc_register(c, watch)) {
        return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_ALLOC);
    }

    return n00b_result_ok(n00b_conduit_topic_base_t *, watch->topic);
}

void
n00b_conduit_proc_unwatch(n00b_conduit_t *c, pid_t pid)
{
    if (!c || pid <= 0) {
        return;
    }

    n00b_result_t(n00b_conduit_topic_base_t *) topic_res =
        n00b_conduit_topic_get(c, N00B_CONDUIT_URI_PROC(pid), 0);
    if (n00b_result_is_ok(topic_res)) {
        n00b_conduit_topic_close(n00b_result_get(topic_res));
    }
}

// ============================================================================
// Process Registration (with I/O backend)
// ============================================================================

bool
n00b_conduit_proc_register(n00b_conduit_t *c,
                            n00b_conduit_proc_watch_t *watch)
{
    if (!c || !watch) {
        return false;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return false;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->proc_add) {
        return false;
    }

    return io->ops->proc_add(io->ctx, watch);
}

void
n00b_conduit_proc_unregister(n00b_conduit_t *c,
                              n00b_conduit_proc_watch_t *watch)
{
    if (!c || !watch) {
        return;
    }

    n00b_option_t(n00b_conduit_io_backend_t *) opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(opt)) {
        return;
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(opt);
    if (!io->ops->proc_remove) {
        return;
    }

    io->ops->proc_remove(io->ctx, watch);
}

// ============================================================================
// Process Firing
// ============================================================================

void
n00b_conduit_proc_fire(n00b_conduit_proc_watch_t *watch,
                        uint32_t ops, int exit_status)
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

    n00b_conduit_proc_msg_t *msg = n00b_alloc(n00b_conduit_proc_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = watch->topic;
    msg->header.generation = n00b_conduit_topic_generation(watch->topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(watch->topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.pid    = watch->pid;
    msg->payload.events = ops;

    // For exit events, reap the child to get the authoritative wait(2)
    // status.  The IO backend (kqueue/pidfd) may not provide it in
    // the portable W* format.
    if (ops & N00B_CONDUIT_PROC_EXIT) {
        int wstatus = 0;
        pid_t w = waitpid(watch->pid, &wstatus, WNOHANG);
        msg->payload.exit_status = (w > 0) ? wstatus : exit_status;
    }
    else {
        msg->payload.exit_status = 0;
    }

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_proc_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_proc_payload_t) *)watch->topic,
        msg,
        ops);

    n00b_conduit_publish_yield(pub);

    // Process exit is a terminal event — close the topic so done_topic
    // fires and subscribers know no more events will arrive.
    if (ops & N00B_CONDUIT_PROC_EXIT) {
        n00b_conduit_topic_close(watch->topic);
    }
}

#endif // !_WIN32
