/*
 * fd_managed.c - Managed FD I/O implementation
 *
 * Implements the two-layer FD I/O model:
 *   Layer 1: Non-blocking reads/writes, publish as-done buffers as events
 *   Layer 2: Stream readers that accumulate as-done buffers into N-byte
 *            responses
 */

#include "conduit/conduit.h"
#include "conduit/fd_managed.h"
#include "conduit/io.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#define N00B_FD_READ(fd, buf, len)    recv((SOCKET)(fd), (char *)(buf), (int)(len), 0)
#define N00B_FD_WRITE(fd, buf, len)   send((SOCKET)(fd), (const char *)(buf), (int)(len), 0)
#define N00B_FD_CLOSE(fd)             closesocket((SOCKET)(fd))
#define N00B_FD_ERRNO                 WSAGetLastError()
#define N00B_FD_EAGAIN               WSAEWOULDBLOCK
#define N00B_FD_EPIPE                WSAECONNRESET
#else
#include <fcntl.h>
#include <unistd.h>
#define N00B_FD_READ(fd, buf, len)    read((fd), (buf), (len))
#define N00B_FD_WRITE(fd, buf, len)   write((fd), (buf), (len))
#define N00B_FD_CLOSE(fd)             close((fd))
#define N00B_FD_ERRNO                 errno
#define N00B_FD_EAGAIN               EAGAIN
#define N00B_FD_EPIPE                EPIPE
#endif

#ifndef PIPE_BUF
#define PIPE_BUF 512
#endif

#define READ_BUF_SIZE 4096

// ============================================================================
// on_first_subscribe callback for FD read topics
// ============================================================================

static void fd_owner_update_io_mask(n00b_conduit_fd_owner_t *owner);
static void wq_drain_with_error(n00b_conduit_fd_owner_t *owner, int error_code);

static void
fd_read_on_first_subscribe(n00b_conduit_topic_base_t *topic, void *ctx)
{
    (void)topic;
    n00b_conduit_fd_owner_t *owner = (n00b_conduit_fd_owner_t *)ctx;
    n00b_atomic_store(&owner->read_active, true);
    fd_owner_update_io_mask(owner);
}

static void
fd_read_on_last_unsubscribe(n00b_conduit_topic_base_t *topic, void *ctx)
{
    (void)topic;
    n00b_conduit_fd_owner_t *owner = (n00b_conduit_fd_owner_t *)ctx;
    n00b_atomic_store(&owner->read_active, false);
    fd_owner_update_io_mask(owner);
}

/**
 * Recompute and apply the IO watch mask from read_active/write_active.
 */
static void
fd_owner_update_io_mask(n00b_conduit_fd_owner_t *owner)
{
    n00b_conduit_io_op_t ops = 0;
    if (n00b_atomic_load(&owner->read_active))  ops |= N00B_CONDUIT_IO_READ;
    if (n00b_atomic_load(&owner->write_active)) ops |= N00B_CONDUIT_IO_WRITE;

    n00b_conduit_io_modify(owner->io, owner->fd, ops, owner->io_target);
}

// ============================================================================
// FD Owner creation + lookup
// ============================================================================

n00b_result_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_manage(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       int fd, bool close_on_done)
{
    if (!c || !io || fd < 0) {
        return n00b_result_err(n00b_conduit_fd_owner_t *, EINVAL);
    }

    void *fd_key = (void *)(intptr_t)fd;

    // Check if already managed
    bool found = false;
    void *existing = n00b_dict_untyped_get(&c->fd_owners, fd_key, &found);
    if (found) {
        return n00b_result_ok(n00b_conduit_fd_owner_t *, (n00b_conduit_fd_owner_t *)existing);
    }

    // Allocate from the system pool — fd owners are accessed by IO
    // threads and must not be relocated by the GC.
    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    n00b_conduit_fd_owner_t *owner = n00b_alloc_with_opts(n00b_conduit_fd_owner_t,
                                        &(n00b_alloc_opts_t){.allocator = sp});

    owner->conduit       = c;
    owner->io            = io;
    owner->fd            = fd;
    owner->close_on_done = close_on_done;
    n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_ACTIVE);
    n00b_atomic_store(&owner->read_pos, 0);
    n00b_atomic_store(&owner->write_pos, 0);
    n00b_atomic_store(&owner->next_request_id, 1);
    n00b_atomic_store(&owner->read_active, false);
    n00b_atomic_store(&owner->write_active, false);

    // Create the 4 topics
    n00b_result_t(n00b_conduit_topic_base_t *) res;

    res = n00b_conduit_topic_get(c, N00B_CONDUIT_URI_FD_READ(fd),
                                sizeof(n00b_conduit_topic_t(n00b_buffer_t *)));
    if (n00b_result_is_err(res)) {
        return n00b_result_err(n00b_conduit_fd_owner_t *, ENOMEM);
    }
    owner->read_topic = n00b_result_get(res);

    // Edge-triggered read activation: reads start when the first subscriber
    // registers and stop when the last subscriber is removed.
    owner->read_topic->on_first_subscribe      = fd_read_on_first_subscribe;
    owner->read_topic->on_first_subscribe_ctx  = owner;
    owner->read_topic->on_last_unsubscribe     = fd_read_on_last_unsubscribe;
    owner->read_topic->on_last_unsubscribe_ctx = owner;

    res = n00b_conduit_topic_get(c, N00B_CONDUIT_URI_FD_WRITE(fd),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_fd_write_payload_t)));
    if (n00b_result_is_err(res)) {
        return n00b_result_err(n00b_conduit_fd_owner_t *, ENOMEM);
    }
    owner->write_topic = n00b_result_get(res);

    res = n00b_conduit_topic_get(c, N00B_CONDUIT_URI_FD_STATUS(fd),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_fd_status_payload_t)));
    if (n00b_result_is_err(res)) {
        return n00b_result_err(n00b_conduit_fd_owner_t *, ENOMEM);
    }
    owner->status_topic = n00b_result_get(res);

    res = n00b_conduit_topic_get(c, N00B_CONDUIT_URI_FD_WREQ(fd),
                                sizeof(n00b_conduit_topic_t(n00b_conduit_fd_write_req_payload_t)));
    if (n00b_result_is_err(res)) {
        return n00b_result_err(n00b_conduit_fd_owner_t *, ENOMEM);
    }
    owner->wreq_topic = n00b_result_get(res);

    // Write queue starts empty; write monitoring is demand-driven.
    owner->wq_head = nullptr;
    owner->wq_tail = nullptr;

    // Make FD non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    n00b_runtime_t *_rt = n00b_get_runtime();
    n00b_allocator_t *cp = (n00b_allocator_t *)&_rt->conduit_pool;
    n00b_conduit_io_target_t *target = n00b_alloc_with_opts(n00b_conduit_io_target_t,
                                                           &(n00b_alloc_opts_t){.allocator = cp});
    _n00b_variant_set_ptr(target, n00b_conduit_fd_owner_t *, owner);
    owner->io_target = target;
    // Register with the IO backend but with no events initially.
    // Read and write monitoring are activated on demand by the
    // on_first_subscribe callbacks on the read and wreq topics.
    n00b_conduit_io_watch(io, fd, 0, target);

    // Insert into fd_owners dict
    n00b_dict_untyped_put(&c->fd_owners, fd_key, owner);

    return n00b_result_ok(n00b_conduit_fd_owner_t *, owner);
}

n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_get_owner(n00b_conduit_t *c, int fd)
{
    if (!c || fd < 0) {
        return n00b_option_none(n00b_conduit_fd_owner_t *);
    }

    bool found = false;
    void *val = n00b_dict_untyped_get(&c->fd_owners, (void *)(intptr_t)fd, &found);
    if (found) {
        return n00b_option_set(n00b_conduit_fd_owner_t *, (n00b_conduit_fd_owner_t *)val);
    }
    return n00b_option_none(n00b_conduit_fd_owner_t *);
}

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_read_topic(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    return n00b_result_ok(n00b_conduit_topic_base_t *, owner->read_topic);
}

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_write_topic(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    return n00b_result_ok(n00b_conduit_topic_base_t *, owner->write_topic);
}

n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_status_topic(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) return n00b_result_err(n00b_conduit_topic_base_t *, N00B_CONDUIT_ERR_NULL_ARG);
    return n00b_result_ok(n00b_conduit_topic_base_t *, owner->status_topic);
}

void
n00b_conduit_fd_activate_reads(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) {
        return;
    }

    bool expected = false;
    if (n00b_atomic_cas(&owner->read_active, &expected, true)) {
        fd_owner_update_io_mask(owner);
    }
}

void
n00b_conduit_fd_deactivate_reads(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) {
        return;
    }

    bool expected = true;
    if (n00b_atomic_cas(&owner->read_active, &expected, false)) {
        fd_owner_update_io_mask(owner);
    }
}

// ============================================================================
// FD Owner teardown
// ============================================================================

void
n00b_conduit_fd_owner_close(n00b_conduit_fd_owner_t *owner)
{
    if (!owner) {
        return;
    }

    int state = n00b_atomic_load(&owner->state);
    if (state == N00B_CONDUIT_FD_CLOSED) {
        return;
    }

    // Drain the write queue — send error completions for pending entries.
    wq_drain_with_error(owner, ECANCELED);

    // Unwatch from IO backend.
    n00b_conduit_io_unwatch(owner->io, owner->fd);

    // Close all four topics (notifies subscribers with TOPIC_CLOSED).
    n00b_conduit_topic_close(owner->read_topic);
    n00b_conduit_topic_close(owner->write_topic);
    n00b_conduit_topic_close(owner->status_topic);
    n00b_conduit_topic_close(owner->wreq_topic);

    // Transition to fully closed.
    n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_CLOSED);

    if (owner->close_on_done) {
        N00B_FD_CLOSE(owner->fd);
    }
}

// ============================================================================
// Read path (Layer 1)
// ============================================================================

static void
publish_status(n00b_conduit_fd_owner_t *owner,
               n00b_conduit_fd_status_op_t status, int error_code)
{
    n00b_conduit_topic_base_t *topic = owner->status_topic;

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(topic);
    if (n00b_result_is_err(pub_res)) {
        n00b_atomic_add(&topic->epoch, 1);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_conduit_fd_status_msg_t *msg =
        n00b_alloc(n00b_conduit_fd_status_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = topic;
    msg->header.generation = n00b_conduit_topic_generation(topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.fd         = owner->fd;
    msg->payload.status     = status;
    msg->payload.error_code = error_code;

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_fd_status_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_fd_status_payload_t) *)topic,
        msg,
        N00B_CONDUIT_FD_OP_STATUS);

    n00b_conduit_publish_yield(pub);
}

static void
transition_state(n00b_conduit_fd_owner_t *owner, bool read_closed, bool write_closed)
{
    int current = n00b_atomic_load(&owner->state);

    if (read_closed && write_closed) {
        n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_CLOSED);
    }
    else if (read_closed) {
        if (current == N00B_CONDUIT_FD_WRITE_CLOSED) {
            n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_CLOSED);
        }
        else if (current == N00B_CONDUIT_FD_ACTIVE) {
            n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_READ_CLOSED);
        }
    }
    else if (write_closed) {
        if (current == N00B_CONDUIT_FD_READ_CLOSED) {
            n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_CLOSED);
        }
        else if (current == N00B_CONDUIT_FD_ACTIVE) {
            n00b_atomic_store(&owner->state, N00B_CONDUIT_FD_WRITE_CLOSED);
        }
    }

    // If fully closed and close_on_done is set, close the FD
    if (n00b_atomic_load(&owner->state) == N00B_CONDUIT_FD_CLOSED) {
        publish_status(owner, N00B_CONDUIT_FD_ST_CLOSED, 0);
        if (owner->close_on_done) {
            N00B_FD_CLOSE(owner->fd);
        }
    }
}

static void
fd_owner_do_reads(n00b_conduit_fd_owner_t *owner)
{
    if (!n00b_atomic_load(&owner->read_active)) {
        return;
    }

    int state = n00b_atomic_load(&owner->state);
    if (state == N00B_CONDUIT_FD_READ_CLOSED || state == N00B_CONDUIT_FD_CLOSED) {
        return;
    }

    n00b_conduit_topic_base_t *topic = owner->read_topic;

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(topic);
    if (n00b_result_is_err(pub_res)) {
        // Bump epoch so subscribers know a read event was dropped.
        n00b_atomic_add(&topic->epoch, 1);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    uint8_t buf[READ_BUF_SIZE];

    while (1) {
        ssize_t n = N00B_FD_READ(owner->fd, buf, sizeof(buf));

        if (n > 0) {
            n00b_atomic_add(&owner->read_pos, (uint64_t)n);

            n00b_buffer_t *payload = n00b_buffer_from_bytes((char *)buf, (int64_t)n);

            n00b_conduit_message_t(n00b_buffer_t *) *msg =
                n00b_alloc(n00b_conduit_message_t(n00b_buffer_t *));

            msg->header.type       = N00B_CONDUIT_MSG_USER;
            msg->header.topic      = topic;
            msg->header.generation = n00b_conduit_topic_generation(topic);
            msg->header.epoch      = n00b_conduit_topic_epoch(topic);
            msg->header.timestamp  = 0;
            msg->header.next       = nullptr;

            msg->payload = payload;

            n00b_conduit_topic_deliver_msg(
                n00b_buffer_t *,
                (n00b_conduit_topic_t(n00b_buffer_t *) *)topic,
                msg,
                N00B_CONDUIT_FD_OP_READ_DATA);
            continue;
        }

        if (n == 0) {
            // EOF
            publish_status(owner, N00B_CONDUIT_FD_ST_READ_EOF, 0);
            transition_state(owner, true, false);
            break;
        }

        // n < 0 -- error
        int read_err = N00B_FD_ERRNO;
        if (read_err == N00B_FD_EAGAIN || read_err == EWOULDBLOCK
#ifndef _WIN32
            || read_err == EINTR
#endif
            ) {
            break; // Normal: no more data available right now
        }

        // Real error
        publish_status(owner, N00B_CONDUIT_FD_ST_READ_ERR, read_err);
        transition_state(owner, true, false);
        break;
    }

    n00b_conduit_publish_yield(pub);
}

// ============================================================================
// Write path (Layer 1)
// ============================================================================

static void fd_owner_do_writes(n00b_conduit_fd_owner_t *owner);

/**
 * Append a write entry to the tail of the owner's write queue.
 * If the queue was empty, activate write monitoring and attempt
 * an immediate write (kqueue EV_CLEAR may miss the initial
 * writable edge on an already-writable fd).
 */
static void
wq_enqueue(n00b_conduit_fd_owner_t *owner, n00b_conduit_write_entry_t *entry)
{
    entry->next = nullptr;

    if (owner->wq_tail) {
        owner->wq_tail->next = entry;
    }
    else {
        owner->wq_head = entry;
    }
    owner->wq_tail = entry;

    // Activate write monitoring if this is the first entry.
    bool expected = false;
    if (n00b_atomic_cas(&owner->write_active, &expected, true)) {
        fd_owner_update_io_mask(owner);
        // Attempt an immediate write — kqueue with EV_CLEAR may not
        // deliver a writable event if the fd was already writable
        // before we registered EVFILT_WRITE.
        fd_owner_do_writes(owner);
    }
}

/**
 * Dequeue the head entry from the write queue.
 * If the queue becomes empty, deactivate write monitoring.
 */
static void
wq_dequeue_head(n00b_conduit_fd_owner_t *owner)
{
    n00b_conduit_write_entry_t *head = owner->wq_head;
    if (!head) {
        return;
    }

    owner->wq_head = head->next;
    if (!owner->wq_head) {
        owner->wq_tail = nullptr;
        // Queue is empty — deactivate write monitoring.
        bool expected = true;
        if (n00b_atomic_cas(&owner->write_active, &expected, false)) {
            fd_owner_update_io_mask(owner);
        }
    }
}

n00b_result_t(uint64_t)
n00b_conduit_fd_write_submit(n00b_conduit_fd_owner_t *owner,
                             const void *data, size_t len,
                             void *reply_inbox,
                             bool (*reply_push)(void *, void *))
{
    if (!owner || !data || len == 0) {
        return n00b_result_err(uint64_t, EINVAL);
    }

    int state = n00b_atomic_load(&owner->state);
    if (state == N00B_CONDUIT_FD_WRITE_CLOSED || state == N00B_CONDUIT_FD_CLOSED) {
        return n00b_result_err(uint64_t, EPIPE);
    }

    uint64_t request_id = n00b_atomic_add(&owner->next_request_id, 1);

    // Copy the data into a GC-managed buffer so the caller can free
    // or reuse their original immediately.
    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    uint8_t *copy = n00b_alloc_array_with_opts(uint8_t, len,
                        &(n00b_alloc_opts_t){.allocator = sp});
    memcpy(copy, data, len);

    n00b_conduit_write_entry_t *entry = n00b_alloc_with_opts(n00b_conduit_write_entry_t,
                                            &(n00b_alloc_opts_t){.allocator = sp});
    entry->data        = copy;
    entry->total_len   = len;
    entry->bytes_sent  = 0;
    entry->request_id  = request_id;
    entry->reply_inbox = reply_inbox;
    entry->reply_push  = reply_push;

    wq_enqueue(owner, entry);

    return n00b_result_ok(uint64_t, request_id);
}

static void
send_write_done(n00b_conduit_fd_owner_t *owner,
                void *reply_inbox, bool (*reply_push)(void *, void *),
                uint64_t request_id, size_t bytes_written,
                bool error, int error_code)
{
    if (!reply_inbox || !reply_push) {
        return;
    }

    n00b_conduit_fd_write_done_msg_t *msg =
        n00b_alloc(n00b_conduit_fd_write_done_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = nullptr;
    msg->header.generation = 0;
    msg->header.epoch      = 0;
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.fd            = owner->fd;
    msg->payload.request_id    = request_id;
    msg->payload.bytes_written = bytes_written;
    msg->payload.error         = error;
    msg->payload.error_code    = error_code;

    reply_push(reply_inbox, msg);
}

static void
publish_write_done_event(n00b_conduit_fd_owner_t *owner,
                         n00b_conduit_publisher_t *pub,
                         const uint8_t *data, size_t len,
                         uint64_t stream_pos, uint64_t request_id)
{
    (void)pub;

    // Allocate immutable copy
    void *copy = n00b_alloc_array(uint8_t, len);
    memcpy(copy, data, len);

    n00b_conduit_fd_write_msg_t *msg =
        n00b_alloc(n00b_conduit_fd_write_msg_t);

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = owner->write_topic;
    msg->header.generation = n00b_conduit_topic_generation(owner->write_topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(owner->write_topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.fd         = owner->fd;
    msg->payload.data       = copy;
    msg->payload.len        = len;
    msg->payload.stream_pos = stream_pos;
    msg->payload.request_id = request_id;

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_fd_write_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_fd_write_payload_t) *)owner->write_topic,
        msg,
        N00B_CONDUIT_FD_OP_WRITE_DATA);
}

/**
 * Drain the write queue, sending error completions for all entries.
 * Used on fatal write error and on owner close.
 */
static void
wq_drain_with_error(n00b_conduit_fd_owner_t *owner, int error_code)
{
    while (owner->wq_head) {
        n00b_conduit_write_entry_t *entry = owner->wq_head;
        send_write_done(owner, entry->reply_inbox, entry->reply_push,
                        entry->request_id, entry->bytes_sent,
                        true, error_code);
        wq_dequeue_head(owner);
    }
}

static void
fd_owner_do_writes(n00b_conduit_fd_owner_t *owner)
{
    int state = n00b_atomic_load(&owner->state);
    if (state == N00B_CONDUIT_FD_WRITE_CLOSED || state == N00B_CONDUIT_FD_CLOSED) {
        return;
    }

    // Fire one-shot on_first_writable hook (used by outbound connect).
    if (owner->on_first_writable) {
        void (*cb)(n00b_conduit_fd_owner_t *, void *) = owner->on_first_writable;
        void *ctx                                      = owner->on_first_writable_ctx;
        owner->on_first_writable                       = nullptr;
        owner->on_first_writable_ctx                   = nullptr;
        cb(owner, ctx);
    }

    // Process entries from the write queue head.
    while (owner->wq_head) {
        n00b_conduit_write_entry_t *entry = owner->wq_head;
        size_t remaining = entry->total_len - entry->bytes_sent;

        // Claim publisher on write topic for as-done events.
        n00b_result_t(n00b_conduit_publisher_t *) pub_res =
            n00b_conduit_publish_try_claim(owner->write_topic);
        n00b_conduit_publisher_t *write_pub = nullptr;
        if (n00b_result_is_ok(pub_res)) {
            write_pub = n00b_result_get(pub_res);
        }
        else {
            n00b_atomic_add(&owner->write_topic->epoch, 1);
        }

        bool     blocked = false;
        bool     error   = false;
        int      err_code = 0;

        while (remaining > 0) {
            size_t  chunk = remaining < PIPE_BUF ? remaining : PIPE_BUF;
            ssize_t n     = N00B_FD_WRITE(owner->fd,
                                          entry->data + entry->bytes_sent,
                                          chunk);

            if (n > 0) {
                uint64_t pos = n00b_atomic_add(&owner->write_pos, (uint64_t)n);

                if (write_pub) {
                    publish_write_done_event(owner, write_pub,
                                             entry->data + entry->bytes_sent,
                                             (size_t)n, pos,
                                             entry->request_id);
                }

                entry->bytes_sent += (size_t)n;
                remaining         -= (size_t)n;
                continue;
            }

            if (n < 0) {
                int write_err = N00B_FD_ERRNO;
                if (write_err == N00B_FD_EAGAIN || write_err == EWOULDBLOCK
#ifndef _WIN32
                    || write_err == EINTR
#endif
                    ) {
                    // Can't write more right now — leave entry at head,
                    // next WRITE readiness will continue.
                    blocked = true;
                    break;
                }

                if (write_err == N00B_FD_EPIPE) {
                    publish_status(owner, N00B_CONDUIT_FD_ST_WRITE_EPIPE,
                                   N00B_FD_EPIPE);
                    transition_state(owner, false, true);
                    error   = true;
                    err_code = N00B_FD_EPIPE;
                    break;
                }

                publish_status(owner, N00B_CONDUIT_FD_ST_WRITE_ERR, write_err);
                transition_state(owner, false, true);
                error    = true;
                err_code = write_err;
                break;
            }
        }

        if (write_pub) {
            n00b_conduit_publish_yield(write_pub);
        }

        if (error) {
            // Fatal write error — send error for this entry and drain
            // the rest of the queue with errors.
            send_write_done(owner, entry->reply_inbox, entry->reply_push,
                            entry->request_id, entry->bytes_sent,
                            true, err_code);
            wq_dequeue_head(owner);
            wq_drain_with_error(owner, err_code);
            return;
        }

        if (blocked) {
            // Short write / EAGAIN — leave entry at head, return.
            // Next WRITE readiness event will pick up where we left off.
            return;
        }

        // Entry fully written — send success completion and advance.
        send_write_done(owner, entry->reply_inbox, entry->reply_push,
                        entry->request_id, entry->bytes_sent,
                        false, 0);
        wq_dequeue_head(owner);
    }
}

// ============================================================================
// FD Owner dispatch (called from io.c when readiness event arrives)
// ============================================================================

void
n00b_conduit_fd_owner_dispatch(n00b_conduit_fd_owner_t *owner, uint32_t io_ops)
{
    if (!owner) {
        return;
    }

    if (io_ops & N00B_CONDUIT_IO_READ) {
        fd_owner_do_reads(owner);
    }

    if (io_ops & N00B_CONDUIT_IO_WRITE) {
        fd_owner_do_writes(owner);
    }

    if (io_ops & (N00B_CONDUIT_IO_HUP | N00B_CONDUIT_IO_ERROR)) {
        // If a connect completion hook is pending, fire it now
        if (owner->on_first_writable) {
            void (*cb)(n00b_conduit_fd_owner_t *, void *) = owner->on_first_writable;
            void *ctx                                      = owner->on_first_writable_ctx;
            owner->on_first_writable                       = nullptr;
            owner->on_first_writable_ctx                   = nullptr;
            cb(owner, ctx);
        }

        int fstate = n00b_atomic_load(&owner->state);
        if (fstate == N00B_CONDUIT_FD_ACTIVE) {
            if (io_ops & N00B_CONDUIT_IO_HUP) {
                fd_owner_do_reads(owner);
            }
        }
    }
}

// ============================================================================
// Stream reader (Layer 2)
// ============================================================================

static inline size_t
accum_len(n00b_conduit_stream_reader_t *reader)
{
    return reader->accum ? reader->accum->byte_len : 0;
}

static inline char *
accum_data(n00b_conduit_stream_reader_t *reader)
{
    return reader->accum ? reader->accum->data : nullptr;
}

static void
accum_consume(n00b_conduit_stream_reader_t *reader, size_t nbytes)
{
    if (nbytes > 0 && nbytes < reader->accum->byte_len) {
        memmove(reader->accum->data, reader->accum->data + nbytes,
                reader->accum->byte_len - nbytes);
    }
    reader->accum->byte_len -= nbytes;
    reader->accum_pos       += nbytes;
}

static void
try_fulfill_request(n00b_conduit_stream_reader_t *reader)
{
    while (reader->pending_head) {
        n00b_conduit_stream_request_t *req = reader->pending_head;
        size_t alen = accum_len(reader);

        if (req->use_delimiter) {
            size_t scan_limit = req->max_bytes;
            if (scan_limit == 0 || scan_limit > alen) {
                scan_limit = alen;
            }

            const uint8_t *found = memchr(accum_data(reader), req->delimiter, scan_limit);
            size_t          nbytes;

            if (found) {
                nbytes = (size_t)(found - (const uint8_t *)accum_data(reader)) + 1;
            }
            else if (req->max_bytes > 0 && alen >= req->max_bytes) {
                nbytes = req->max_bytes;
            }
            else if (reader->eof || reader->error) {
                nbytes = alen;
            }
            else {
                break;
            }

            if (nbytes == 0 && !reader->eof && !reader->error) {
                break;
            }

            void *result_data = nullptr;
            if (nbytes > 0) {
                result_data = n00b_alloc_array(uint8_t, nbytes);
                memcpy(result_data, accum_data(reader), nbytes);
            }

            n00b_conduit_fd_stream_msg_t *msg =
                n00b_alloc(n00b_conduit_fd_stream_msg_t);

            msg->header.type       = N00B_CONDUIT_MSG_USER;
            msg->header.topic      = nullptr;
            msg->header.generation = 0;
            msg->header.epoch      = 0;
            msg->header.timestamp  = 0;
            msg->header.next       = nullptr;

            msg->payload.fd         = reader->owner->fd;
            msg->payload.data       = result_data;
            msg->payload.len        = nbytes;
            msg->payload.stream_pos = reader->accum_pos;
            msg->payload.eof        = reader->eof && nbytes >= alen;
            msg->payload.error      = reader->error;
            msg->payload.error_code = reader->error_code;

            if (req->reply_push) {
                req->reply_push(req->reply_inbox, msg);
            }

            accum_consume(reader, nbytes);
        }
        else {
            // read N bytes
            size_t nbytes = req->requested;

            if (alen >= nbytes) {
                // Have enough
            }
            else if (reader->eof || reader->error) {
                nbytes = alen;
            }
            else {
                break;
            }

            if (nbytes == 0 && !reader->eof && !reader->error) {
                break;
            }

            void *result_data = nullptr;
            if (nbytes > 0) {
                result_data = n00b_alloc_array(uint8_t, nbytes);
                memcpy(result_data, accum_data(reader), nbytes);
            }

            n00b_conduit_fd_stream_msg_t *msg =
                n00b_alloc(n00b_conduit_fd_stream_msg_t);

            msg->header.type       = N00B_CONDUIT_MSG_USER;
            msg->header.topic      = nullptr;
            msg->header.generation = 0;
            msg->header.epoch      = 0;
            msg->header.timestamp  = 0;
            msg->header.next       = nullptr;

            msg->payload.fd         = reader->owner->fd;
            msg->payload.data       = result_data;
            msg->payload.len        = nbytes;
            msg->payload.stream_pos = reader->accum_pos;
            msg->payload.eof        = reader->eof && nbytes >= alen;
            msg->payload.error      = reader->error;
            msg->payload.error_code = reader->error_code;

            if (req->reply_push) {
                req->reply_push(req->reply_inbox, msg);
            }

            accum_consume(reader, nbytes);
        }

        reader->pending_head = req->next;
        if (!reader->pending_head) {
            reader->pending_tail = nullptr;
        }
    }
}

void
n00b_conduit_stream_reader_process(n00b_conduit_stream_reader_t *reader)
{
    if (!reader) {
        return;
    }

    // Drain as-done read buffers from internal inbox
    n00b_conduit_message_t(n00b_buffer_t *) *msg;
    while ((msg = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, reader->internal_inbox)) != nullptr) {
        n00b_buffer_t *buf = msg->payload;
        if (buf && buf->byte_len > 0) {
            if (!reader->accum) {
                reader->accum = n00b_buffer_empty();
            }
            n00b_buffer_concat(reader->accum, buf);
        }
    }

    // Check for status events (EOF/error) via system queue
    n00b_conduit_sys_msg_t *sys;
    while ((sys = n00b_conduit_inbox_pop_sys(reader->internal_inbox)) != nullptr) {
        if (sys->header.type == N00B_CONDUIT_MSG_TOPIC_CLOSED) {
            reader->eof = true;
        }
    }

    try_fulfill_request(reader);
}

n00b_result_t(n00b_conduit_stream_reader_t *)
n00b_conduit_stream_reader_new(n00b_conduit_t *c, n00b_conduit_fd_owner_t *owner)
{
    if (!c || !owner) {
        return n00b_result_err(n00b_conduit_stream_reader_t *, EINVAL);
    }

    n00b_conduit_stream_reader_t *reader =
        n00b_alloc(n00b_conduit_stream_reader_t);

    reader->conduit = c;
    reader->owner   = owner;

    reader->internal_inbox = ({
        n00b_conduit_inbox_t(n00b_buffer_t *) *_inbox =
            n00b_alloc(n00b_conduit_inbox_t(n00b_buffer_t *));
        n00b_conduit_inbox_init(n00b_buffer_t *,
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);
        _inbox;
    });

    // Subscribe to the owner's read topic (now publishes n00b_buffer_t *).
    // The on_first_subscribe callback on the read topic will automatically
    // activate reads when this (or any) first subscriber registers.
    reader->sub_handle = n00b_conduit_subscribe(
        n00b_buffer_t *,
        (n00b_conduit_topic_t(n00b_buffer_t *) *)owner->read_topic,
        reader->internal_inbox,
        .operations = N00B_CONDUIT_FD_OP_READ_DATA);

    reader->accum        = nullptr;
    reader->accum_pos    = 0;
    reader->pending_head = nullptr;
    reader->pending_tail = nullptr;
    reader->eof          = false;
    reader->error        = false;
    reader->error_code   = 0;

    return n00b_result_ok(n00b_conduit_stream_reader_t *, reader);
}

void
n00b_conduit_stream_reader_destroy(n00b_conduit_stream_reader_t *reader)
{
    if (!reader) {
        return;
    }

    n00b_conduit_sub_cancel(reader->sub_handle);
    reader->pending_head = nullptr;
    reader->pending_tail = nullptr;
}

void
n00b_conduit_stream_read(n00b_conduit_stream_reader_t *reader, size_t nbytes,
                         void *reply_inbox, bool (*reply_push)(void *, void *))
{
    if (!reader || nbytes == 0 || !reply_inbox || !reply_push) {
        return;
    }

    n00b_conduit_stream_request_t *req =
        n00b_alloc(n00b_conduit_stream_request_t);

    req->requested     = nbytes;
    req->delimiter     = 0;
    req->max_bytes     = 0;
    req->use_delimiter = false;
    req->reply_inbox   = reply_inbox;
    req->reply_push    = reply_push;
    req->next          = nullptr;

    if (reader->pending_tail) {
        reader->pending_tail->next = req;
    }
    else {
        reader->pending_head = req;
    }
    reader->pending_tail = req;

    n00b_conduit_stream_reader_process(reader);
}

void
n00b_conduit_stream_read_until(n00b_conduit_stream_reader_t *reader,
                               uint8_t delimiter, size_t max_bytes,
                               void *reply_inbox,
                               bool (*reply_push)(void *, void *))
{
    if (!reader || !reply_inbox || !reply_push) {
        return;
    }

    n00b_conduit_stream_request_t *req =
        n00b_alloc(n00b_conduit_stream_request_t);

    req->requested     = 0;
    req->delimiter     = delimiter;
    req->max_bytes     = max_bytes;
    req->use_delimiter = true;
    req->reply_inbox   = reply_inbox;
    req->reply_push    = reply_push;
    req->next          = nullptr;

    if (reader->pending_tail) {
        reader->pending_tail->next = req;
    }
    else {
        reader->pending_head = req;
    }
    reader->pending_tail = req;

    n00b_conduit_stream_reader_process(reader);
}

// ============================================================================
// Stream writer convenience
// ============================================================================

n00b_result_t(int)
n00b_fd_owner_write(n00b_conduit_fd_owner_t *owner,
                    const void *data, size_t len)
{
    if (!owner || !data || len == 0) {
        return n00b_result_err(int, N00B_CONDUIT_ERR_NULL_ARG);
    }

    int state = n00b_atomic_load(&owner->state);
    if (state == N00B_CONDUIT_FD_WRITE_CLOSED || state == N00B_CONDUIT_FD_CLOSED) {
        return n00b_result_err(int, N00B_CONDUIT_ERR_FD_CLOSED);
    }

    n00b_conduit_fd_write_done_inbox_t *done_inbox =
        n00b_conduit_fd_write_done_inbox_new(owner->conduit);

    auto submit_r = n00b_conduit_fd_write_submit(
        owner, data, len, done_inbox,
        (bool (*)(void *, void *))_N00B_INBOX_FN(push, n00b_conduit_fd_write_done_payload_t));

    if (n00b_result_is_err(submit_r)) {
        return n00b_result_err(int, N00B_CONDUIT_ERR_ALLOC);
    }

    // Drive the write queue directly until our entry completes.
    n00b_conduit_fd_write_done_msg_t *done = nullptr;

    for (int attempts = 0; attempts < 500 && !done; attempts++) {
        fd_owner_do_writes(owner);
        done = n00b_conduit_fd_write_done_inbox_pop(done_inbox);
        if (!done) {
            // Brief wait for FD writability.
            n00b_condition_wait(&done_inbox->cv, .timeout = 10000000); // 10ms
        }
    }

    if (!done) {
        return n00b_result_err(int, N00B_CONDUIT_ERR_IO);
    }

    if (done->payload.error) {
        if (done->payload.error_code == N00B_FD_EPIPE) {
            return n00b_result_err(int, N00B_CONDUIT_ERR_EPIPE);
        }
        return n00b_result_err(int, N00B_CONDUIT_ERR_IO);
    }

    return n00b_result_ok(int, (int)done->payload.bytes_written);
}
