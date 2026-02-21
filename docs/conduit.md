# n00b Conduit Library

## Overview

The conduit library provides a type-safe, publish-subscribe event system
built on top of platform-native I/O multiplexing.  It unifies file
descriptors, sockets, timers, signals, and user-defined events behind a
single topic/inbox model with typed message delivery, backpressure, and
thread-safe operation.

The library is organized into seven layers:

1. **Core types** &mdash; `conduit/conduit_types.h`, `conduit/conduit.h`:
   error codes, result types, conduit instance, topic registry.
2. **Pub/sub primitives** &mdash; `conduit/message.h`, `conduit/inbox.h`,
   `conduit/subscription.h`, `conduit/publisher.h`, `conduit/topic.h`:
   typed messages, FIFO inboxes, subscription management, publisher
   ownership protocol.
3. **I/O backends** &mdash; `conduit/io.h`: backend-agnostic multiplexing
   interface with implementations for kqueue, epoll, poll, io_uring, and
   WSAPoll.
4. **Managed FDs** &mdash; `conduit/fd_managed.h`: single-owner FD
   lifecycle, Layer 1 (as-done) and Layer 2 (stream) I/O.
5. **High-level resources** &mdash; `conduit/socket.h`, `conduit/file.h`:
   TCP listener/connection and file abstractions.
6. **System events** &mdash; `conduit/signal.h`, `conduit/timer.h`,
   `conduit/user_event.h`: Unix signals, timers, cross-thread wakeups.
7. **Service** &mdash; `conduit/service.h`: background thread pool with
   per-thread I/O backends.

### Design principles

- **Fully typed end-to-end.**  Topics, inboxes, subscriptions, and
  messages are all parameterized by a payload type `T` via `typeid()`.
  No casts needed at call sites; the compiler enforces type correctness.
- **Zero-copy Layer 1.**  When an FD becomes readable, the owner
  publishes immutable shared buffers.  All subscribers see every buffer
  without copying.
- **Decentralized I/O.**  Each service thread owns its own I/O backend
  and runs an independent poll loop.  No centralized dispatch queue.
- **Publisher ownership.**  Topics have at most one active publisher at a
  time, providing linearizable event ordering.  The publisher protocol
  handles contention, liveness detection, and graceful handoff.
- **Platform-native multiplexing.**  The best available backend is
  selected automatically: kqueue on macOS/BSD, epoll on Linux (with
  io_uring optional), poll as universal fallback, WSAPoll on Windows.

---

## Getting started

### Minimal example: FD monitoring

```c
#include "conduit/conduit.h"
#include "conduit/io.h"

// Create a conduit instance.
n00b_conduit_t *c = n00b_conduit_unwrap(n00b_conduit_new());

// Create an I/O backend (platform-default: kqueue, epoll, etc.)
n00b_conduit_io_backend_t *io = n00b_conduit_io_unwrap(
    n00b_conduit_io_new_default(c));

// Watch stdin for readability.
n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_io_watch(
    io, STDIN_FILENO, N00B_CONDUIT_IO_READ, nullptr);

// Poll for events.
n00b_conduit_io_poll(io, 1000);  // 1 second timeout

// Cleanup.
n00b_conduit_io_destroy(io);
n00b_conduit_destroy(c);
```

### Full example: timer with inbox

```c
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/timer.h"

n00b_conduit_t *c = n00b_conduit_unwrap(n00b_conduit_new());
n00b_conduit_io_backend_t *io = n00b_conduit_io_unwrap(
    n00b_conduit_io_new_default(c));

// Create a repeating 500ms timer.
n00b_conduit_topic_base_t *topic =
    n00b_conduit_topic_unwrap(n00b_conduit_timer_repeat(c, 500));

// Create an inbox and subscribe.
n00b_conduit_timer_inbox_t *inbox = n00b_conduit_timer_inbox_new(c);
n00b_conduit_timer_subscribe(topic, inbox,
    .operations = N00B_CONDUIT_OP_ALL);

// Event loop.
for (int i = 0; i < 10; i++) {
    n00b_conduit_io_poll(io, 600);

    while (n00b_conduit_timer_inbox_has_messages(inbox)) {
        n00b_conduit_timer_msg_t *msg =
            n00b_conduit_timer_inbox_pop(inbox);
        printf("Timer fired! count=%llu\n",
               msg->payload.fire_count);
    }
}

n00b_conduit_timer_cancel(topic);
n00b_conduit_io_destroy(io);
n00b_conduit_destroy(c);
```

---

## Core types &mdash; `conduit/conduit_types.h`

### Error codes

All conduit operations that can fail return `n00b_result_t` values with
error codes from this set:

| Code | Meaning |
|------|---------|
| `N00B_CONDUIT_ERR_NULL_ARG` | Null pointer passed to API |
| `N00B_CONDUIT_ERR_ALLOC` | Memory allocation failed |
| `N00B_CONDUIT_ERR_SHUTDOWN` | Conduit is shutting down |
| `N00B_CONDUIT_ERR_CLOSED` | Topic or resource is closed |
| `N00B_CONDUIT_ERR_NOT_OWNER` | Not the publisher for this topic |
| `N00B_CONDUIT_ERR_ALREADY_CLAIMED` | Publisher already claimed |
| `N00B_CONDUIT_ERR_REGISTRY_FULL` | Backend registry at capacity |
| `N00B_CONDUIT_ERR_SOCKET` | Socket creation failed |
| `N00B_CONDUIT_ERR_BIND` | Bind failed |
| `N00B_CONDUIT_ERR_LISTEN` | Listen failed |
| `N00B_CONDUIT_ERR_CONNECT` | Connect failed |
| `N00B_CONDUIT_ERR_TIMEOUT` | Operation timed out |

### IO target variant

When the I/O backend delivers a readiness event, the target is a
variant that discriminates between FD owners and listeners:

```c
typedef n00b_variant_t(n00b_conduit_fd_owner_t *,
                        n00b_conduit_listener_t *)
    n00b_conduit_io_target_t;
```

### Common result types

```c
n00b_result_t(bool)
n00b_result_t(n00b_conduit_t *)
n00b_result_t(n00b_conduit_topic_base_t *)
n00b_result_t(n00b_conduit_io_backend_t *)
n00b_result_t(n00b_conduit_publisher_t *)
n00b_result_t(n00b_conduit_service_t *)
n00b_result_t(n00b_conduit_svc_thread_t *)
```

---

## Conduit instance &mdash; `conduit/conduit.h`

The `n00b_conduit_t` is the central coordinator.  It owns:
- Two topic dictionaries (integer-keyed and string-keyed)
- An FD owner hash table (for managed FDs)
- A listener hash table (for TCP listeners)
- An I/O backend registry (up to `N00B_CONDUIT_MAX_BACKENDS`)
- An optional service thread pool

### Lifecycle

```c
n00b_result_t(n00b_conduit_t *)  n00b_conduit_new(void);
void                    n00b_conduit_destroy(n00b_conduit_t *c);
bool                    n00b_conduit_is_shutdown(n00b_conduit_t *c);
```

### Topic registry

```c
// Get-or-create a topic by URI.
n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_topic_get(n00b_conduit_t *c, n00b_conduit_uri_t uri,
                        size_t topic_size);

// Convenience: topic for a file descriptor.
n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_topic_for_fd(n00b_conduit_t *c, int fd);

// Close a topic (notifies subscribers, increments generation).
uint64_t n00b_conduit_topic_close(n00b_conduit_topic_base_t *topic);
```

### Backend registry

```c
// Register/unregister an I/O backend.
n00b_result_t(bool)
n00b_conduit_register_backend(n00b_conduit_t *c,
                              n00b_conduit_io_backend_t *io);
void
n00b_conduit_unregister_backend(n00b_conduit_t *c,
                                n00b_conduit_io_backend_t *io);

// Get the default backend.
n00b_option_t(n00b_conduit_io_backend_t *)
n00b_conduit_default_backend(n00b_conduit_t *c);
```

---

## Pub/sub primitives

### Topics &mdash; `conduit/topic.h`

Topics are the unit of event publication.  Each topic has:
- A **URI** (integer or string variant) for registry lookup
- A **generation** counter for resource-reuse safety (e.g., FD recycling)
- An **epoch** counter incremented on every event
- A **publisher** slot (at most one active publisher)
- A **subscription list** (typed per payload type)

#### URI system

Integer URIs encode a 16-bit tag in the top 16 bits and a 48-bit ID:

| Tag | Resource |
|-----|----------|
| `N00B_CONDUIT_TAG_FD` | File descriptor |
| `N00B_CONDUIT_TAG_TIMER` | Timer |
| `N00B_CONDUIT_TAG_SIGNAL` | Unix signal |
| `N00B_CONDUIT_TAG_FD_READ` | FD read events |
| `N00B_CONDUIT_TAG_FD_WRITE` | FD write events |
| `N00B_CONDUIT_TAG_FD_STATUS` | FD status events |
| `N00B_CONDUIT_TAG_USER_EVENT` | User event |
| `N00B_CONDUIT_TAG_XFORM` | Pipeline transform |

URI constructors:

```c
N00B_CONDUIT_URI_FD(fd)           // FD URI
N00B_CONDUIT_URI_TIMER(id)        // Timer URI
N00B_CONDUIT_URI_SIGNAL(signum)   // Signal URI
N00B_CONDUIT_URI_USER_EVENT(id)   // User event URI
```

String URIs are used for named resources like files:

```c
n00b_conduit_uri_t uri = n00b_conduit_str_uri(my_string);
```

#### Parameterized topics

`n00b_conduit_topic_t(T)` is generated by `N00B_CONDUIT_TOPIC_IMPL(T)`
and extends the base with typed subscription lists and delivery functions.
The common fields are at identical offsets, so any
`n00b_conduit_topic_t(T) *` can be safely cast to
`n00b_conduit_topic_base_t *`.

#### Topic accessors

```c
uint64_t n00b_conduit_topic_generation(n00b_conduit_topic_base_t *topic);
uint64_t n00b_conduit_topic_epoch(n00b_conduit_topic_base_t *topic);
bool     n00b_conduit_topic_is_active(n00b_conduit_topic_base_t *topic);
```

### Messages &mdash; `conduit/message.h`

Messages carry a common header plus a typed payload:

```c
typedef struct {
    n00b_conduit_msg_type_t     type;
    n00b_conduit_topic_base_t  *topic;
    uint64_t                    generation;
    uint64_t                    epoch;
    uint64_t                    timestamp;
    n00b_conduit_msg_hdr_t     *next;
} n00b_conduit_msg_hdr_t;
```

`N00B_CONDUIT_MESSAGE_IMPL(T)` generates a struct with header + payload:

```c
N00B_CONDUIT_MESSAGE_IMPL(my_payload_t);
// Generates: n00b_conduit_message_t(my_payload_t)

// Accessors:
n00b_conduit_msg_type(msg)       // message type enum
n00b_conduit_msg_topic(msg)      // topic pointer
n00b_conduit_msg_payload(msg)    // typed payload
```

#### Message types

| Type | Meaning |
|------|---------|
| `N00B_CONDUIT_MSG_READABLE` | FD is readable |
| `N00B_CONDUIT_MSG_WRITABLE` | FD is writable |
| `N00B_CONDUIT_MSG_ERROR` | Error on FD |
| `N00B_CONDUIT_MSG_HUP` | Hangup |
| `N00B_CONDUIT_MSG_EOF` | End of file |
| `N00B_CONDUIT_MSG_TIMER` | Timer fired |
| `N00B_CONDUIT_MSG_TOPIC_CLOSED` | Topic was closed |
| `N00B_CONDUIT_MSG_PUBLISHER_LOST` | Publisher died |
| `N00B_CONDUIT_MSG_OVERFLOW` | Inbox backpressure |
| `N00B_CONDUIT_MSG_USER` | User-defined (base ID) |

### Inboxes &mdash; `conduit/inbox.h`

Inboxes are lock-free MPSC FIFO queues.  Every inbox has two channels:
a typed message queue and a system message queue for lifecycle events.

```c
// Instantiate an inbox type for payload T:
N00B_CONDUIT_INBOX_IMPL(my_payload_t);

// Create and initialize:
n00b_conduit_inbox_t(my_payload_t) *inbox =
    n00b_alloc(n00b_conduit_inbox_t(my_payload_t));
n00b_conduit_inbox_init(my_payload_t, inbox, conduit,
                         N00B_CONDUIT_BP_UNBOUNDED, 0);

// Push/pop typed messages:
n00b_conduit_inbox_push_msg(my_payload_t, inbox, msg);
n00b_conduit_message_t(my_payload_t) *msg =
    n00b_conduit_inbox_pop_msg(my_payload_t, inbox);

// Check state:
bool has = n00b_conduit_inbox_has_msg(my_payload_t, inbox);
bool full = n00b_conduit_inbox_full(my_payload_t, inbox);
uint32_t n = n00b_conduit_inbox_msg_count(my_payload_t, inbox);

// System messages (lifecycle):
n00b_conduit_sys_msg_t *sys = n00b_conduit_inbox_pop_sys(inbox);
bool has_sys = n00b_conduit_inbox_has_sys(inbox);
```

#### Backpressure policies

| Policy | Behavior |
|--------|----------|
| `N00B_CONDUIT_BP_UNBOUNDED` | No limit (can grow indefinitely) |
| `N00B_CONDUIT_BP_DROP_OLDEST` | Drop oldest message when full |
| `N00B_CONDUIT_BP_DROP_NEWEST` | Drop new message when full |
| `N00B_CONDUIT_BP_SIGNAL` | Send OVERFLOW message, drop new |

### Subscriptions &mdash; `conduit/subscription.h`

Subscriptions connect a topic to an inbox.  They filter by operation
flags and support one-shot mode, timeouts, and backpressure per
subscription.

```c
// Subscribe to a topic:
n00b_conduit_sub_handle_t handle =
    n00b_conduit_subscribe(my_payload_t, topic, inbox,
        .operations = N00B_CONDUIT_OP_ALL);

// Manage subscription lifecycle:
n00b_conduit_sub_cancel(handle);
n00b_conduit_sub_suspend(handle);
n00b_conduit_sub_resume(handle);
bool active = n00b_conduit_sub_is_active(handle);
```

#### Subscription flags

| Flag | Effect |
|------|--------|
| `N00B_CONDUIT_SUB_F_ONE_SHOT` | Auto-cancel after first delivery |
| `N00B_CONDUIT_SUB_F_NOTIFY_ON_DELIVERY` | CV notify on delivery |
| `N00B_CONDUIT_SUB_F_CONFIRM_CANCEL` | Deliver CANCEL_ACK before remove |
| `N00B_CONDUIT_SUB_F_NOTIFY_UNSUB` | Deliver SUB_REMOVED |
| `N00B_CONDUIT_SUB_F_TIMEOUT_RELATIVE` | Timeout is relative, not absolute |

### Publishers &mdash; `conduit/publisher.h`

Publishers are threads that have claimed the exclusive right to deliver
events on a topic.  The protocol provides linearizable event ordering.

```c
// Claim publisher role (blocking -- waits if claimed by another):
n00b_result_t(n00b_conduit_publisher_t *) pr =
    n00b_conduit_publish_claim(topic);

// Non-blocking try-claim:
n00b_result_t(n00b_conduit_publisher_t *) pr =
    n00b_conduit_publish_try_claim(topic);

// Yield the publisher role:
n00b_conduit_publish_yield(publisher);

// Check liveness (detects dead publisher threads):
bool alive = n00b_conduit_publish_check_liveness(topic);
```

---

## I/O backends &mdash; `conduit/io.h`

### Backend interface

Every backend implements the `n00b_conduit_io_ops_t` vtable:

| Operation | Purpose |
|-----------|---------|
| `init` | Create backend context |
| `cleanup` | Destroy backend context |
| `add` | Add FD to watch set |
| `modify` | Change FD watch flags |
| `remove` | Remove FD from watch set |
| `wait` | Poll for events (blocking) |
| `name` | Backend name (e.g., "kqueue") |
| `timer_add/remove` | Optional timer support |
| `signal_add/remove` | Optional signal support |
| `proc_add/remove` | Optional process watch |
| `vnode_add/remove` | Optional filesystem watch |
| `user_event_add/remove/trigger` | Optional user events |

### Creating a backend

```c
// Platform-default (kqueue/epoll/poll):
n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);

// Specific backend:
const n00b_conduit_io_ops_t *ops = n00b_conduit_io_kqueue_ops()!;
n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new(c, ops);
```

### Platform availability

| Backend | Platforms | Header |
|---------|-----------|--------|
| kqueue | macOS, FreeBSD, OpenBSD, NetBSD | `io_kqueue.c` |
| epoll | Linux | `io_epoll.c` |
| io_uring | Linux (optional) | `io_uring.c` |
| poll | All POSIX | `io_poll.c` |
| WSAPoll | Windows | `io_wsa.c` |

Functions for unavailable backends return `n00b_result_err(ENOTSUP)`.

### Watching and polling

```c
// Watch an FD for readability:
n00b_result_t(n00b_conduit_topic_base_t *) tr =
    n00b_conduit_io_watch(io, fd, N00B_CONDUIT_IO_READ, target);

// Change watch flags:
n00b_conduit_io_modify(io, fd, N00B_CONDUIT_IO_READ | N00B_CONDUIT_IO_WRITE);

// Stop watching:
n00b_conduit_io_unwatch(io, fd);

// Poll for events (returns number of events delivered):
n00b_result_t(int) r = n00b_conduit_io_poll(io, timeout_ms);

// Run the event loop until shutdown:
n00b_conduit_io_run(io);
```

### I/O operation flags

| Flag | Meaning |
|------|---------|
| `N00B_CONDUIT_IO_READ` | FD is readable |
| `N00B_CONDUIT_IO_WRITE` | FD is writable |
| `N00B_CONDUIT_IO_ERROR` | Error condition |
| `N00B_CONDUIT_IO_HUP` | Hangup |

---

## Managed FDs &mdash; `conduit/fd_managed.h`

### Overview

Managed FDs provide two layers of I/O:

- **Layer 1 (as-done):** The FD owner does non-blocking reads/writes
  and publishes immutable shared buffers as events.  All subscribers
  see every buffer (zero-copy fan-out).
- **Layer 2 (stream):** A stream reader accumulates Layer 1 buffers
  into contiguous responses.  Consumers request N bytes and block on
  their inbox.

### Creating an FD owner

```c
n00b_result_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_manage(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       int fd, bool close_on_done);

// Lookup existing owner:
n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_get_owner(n00b_conduit_t *c, int fd);
```

Each FD owner creates four topics:
- **Read topic** &mdash; Layer 1 read data events
- **Write topic** &mdash; write completion confirmations
- **Status topic** &mdash; EOF, error, and close events
- **Write-request topic** &mdash; consumer-to-owner write requests

### Layer 1: subscribing to read events

```c
n00b_conduit_fd_owner_t *owner = n00b_result_get(
    n00b_conduit_fd_manage(c, io, fd, true));

// Get the read topic.
auto topic_opt = n00b_conduit_fd_read_topic(owner);
n00b_conduit_topic_base_t *topic = n00b_option_get(topic_opt);

// Create inbox and subscribe.
n00b_conduit_fd_read_inbox_t *inbox =
    n00b_conduit_fd_read_inbox_new(c);
n00b_conduit_fd_read_subscribe(topic, inbox,
    .operations = N00B_CONDUIT_OP_ALL);

// Activate reads (done automatically by subscribe helpers).
n00b_conduit_fd_activate_reads(owner);

// Poll and consume messages.
n00b_conduit_io_poll(io, 1000);
while (n00b_conduit_fd_read_inbox_has_messages(inbox)) {
    n00b_conduit_fd_read_msg_t *msg =
        n00b_conduit_fd_read_inbox_pop(inbox);
    // msg->payload.data, msg->payload.len, msg->payload.stream_pos
}
```

### Layer 1: submitting writes

```c
n00b_result_t(uint64_t) req_r =
    n00b_conduit_fd_write_submit(owner, data, len,
                                 reply_inbox, reply_push_fn);
uint64_t req_id = n00b_result_get(req_r);
```

### Layer 2: stream reader

```c
n00b_conduit_stream_reader_t *reader = n00b_result_get(
    n00b_conduit_stream_reader_new(c, owner));

// Request exactly 1024 bytes.
n00b_conduit_stream_read(reader, 1024, reply_inbox, reply_push);

// Read until newline (max 4096 bytes).
n00b_conduit_stream_read_until(reader, '\n', 4096,
    reply_inbox, reply_push);

// Blocking write convenience.
n00b_result_t(int) r =
    n00b_write(owner, data, len);

n00b_conduit_stream_reader_destroy(reader);
```

### FD status events

| Status flag | Meaning |
|-------------|---------|
| `N00B_CONDUIT_FD_ST_READ_EOF` | Read EOF |
| `N00B_CONDUIT_FD_ST_WRITE_EPIPE` | Write EPIPE |
| `N00B_CONDUIT_FD_ST_READ_ERR` | Read error |
| `N00B_CONDUIT_FD_ST_WRITE_ERR` | Write error |
| `N00B_CONDUIT_FD_ST_CLOSED` | FD closed |

---

## Sockets &mdash; `conduit/socket.h`

Two distinct concepts:
- **Listener**: Owns a listening socket, accepts connections, publishes
  them as events.
- **Connection**: Wraps an `n00b_conduit_fd_owner_t` for byte-stream I/O
  with a lifecycle/status topic.

### TCP listener

```c
// Listen on port 8080, backlog 128.
n00b_result_t(n00b_conduit_listener_t *) lr =
    n00b_conduit_listen_tcp(c, io, "127.0.0.1", 8080, 128);
n00b_conduit_listener_t *listener = n00b_result_get(lr);

// Subscribe to accept events.
n00b_conduit_topic_base_t *accept_topic =
    n00b_conduit_listener_accept_topic(listener);
n00b_conduit_sock_accept_inbox_t *inbox =
    n00b_conduit_sock_accept_inbox_new(c);
n00b_conduit_sock_accept_subscribe(accept_topic, inbox,
    .operations = N00B_CONDUIT_OP_ALL);

// Drive IO and handle accepts.
n00b_conduit_io_poll(io, 1000);

if (n00b_conduit_sock_accept_inbox_has_messages(inbox)) {
    n00b_conduit_sock_accept_msg_t *msg =
        n00b_conduit_sock_accept_inbox_pop(inbox);
    int client_fd = msg->payload.client_fd;
    // Wrap in a connection for managed I/O:
    auto conn_r = n00b_conduit_conn_from_fd(c, io, client_fd);
}

n00b_conduit_listener_close(listener);
```

### TCP connection

```c
// Outbound connection (non-blocking connect):
n00b_result_t(n00b_conduit_conn_t *) cr =
    n00b_conduit_conn_tcp(c, io, "example.com", 80);
n00b_conduit_conn_t *conn = n00b_result_get(cr);

// Get the FD owner for Layer 1/2 I/O:
n00b_conduit_fd_owner_t *owner = n00b_conduit_conn_fd_owner(conn);

// Get connection lifecycle topic:
n00b_conduit_topic_base_t *status = n00b_conduit_conn_status_topic(conn);

n00b_conduit_conn_close(conn);
```

---

## Files &mdash; `conduit/file.h`

Opens a file and wraps the FD with managed I/O:

```c
n00b_result_t(n00b_conduit_file_t *) fr =
    n00b_conduit_file_open(c, io, "/tmp/log", N00B_CONDUIT_FILE_R);
n00b_conduit_file_t *f = n00b_result_get(fr);

// Get the FD owner for Layer 1/2 I/O:
n00b_conduit_fd_owner_t *owner = n00b_conduit_file_fd_owner(f);

// Or get the file topic ("file:r:/tmp/log"):
n00b_conduit_topic_base_t *topic = n00b_conduit_file_topic(f);

n00b_conduit_file_close(f);
```

### File modes

| Constant | Flags |
|----------|-------|
| `N00B_CONDUIT_FILE_R` | Read-only |
| `N00B_CONDUIT_FILE_W` | Write, create, truncate |
| `N00B_CONDUIT_FILE_A` | Write, create, append |
| `N00B_CONDUIT_FILE_RW` | Read-write, create |
| `N00B_CONDUIT_FILE_RA` | Read-append, create |

---

## Signals &mdash; `conduit/signal.h`

Unix-only.  Signals become topics that fire when raised.

```c
// Get the topic for SIGINT.
n00b_conduit_topic_base_t *topic =
    n00b_conduit_topic_unwrap(n00b_conduit_signal_topic(c, SIGINT));

// Create inbox and subscribe.
n00b_conduit_signal_inbox_t *inbox = n00b_conduit_signal_inbox_new(c);
n00b_conduit_signal_subscribe(topic, inbox,
    .operations = N00B_CONDUIT_OP_ALL);

// In event loop:
while (n00b_conduit_signal_inbox_has_messages(inbox)) {
    n00b_conduit_signal_msg_t *msg = n00b_conduit_signal_inbox_pop(inbox);
    printf("Got signal %d (raised %llu times)\n",
           msg->payload.signum, msg->payload.raise_count);
}

// Stop monitoring:
n00b_conduit_signal_unwatch(c, SIGINT);
```

### Signal helpers

```c
int  n00b_conduit_signal_num(topic);           // Extract signal number
bool n00b_conduit_topic_is_signal(topic);      // Is this a signal topic?
```

---

## Timers &mdash; `conduit/timer.h`

One-shot and repeating timers:

```c
// One-shot (fires once after 5 seconds):
n00b_conduit_topic_base_t *once_topic =
    n00b_conduit_topic_unwrap(n00b_conduit_timer_once(c, 5000));

// Repeating (fires every 100ms):
n00b_conduit_topic_base_t *repeat_topic =
    n00b_conduit_topic_unwrap(n00b_conduit_timer_repeat(c, 100));

// Cancel a timer:
n00b_conduit_timer_cancel(repeat_topic);
```

### Timer payload

```c
typedef struct {
    uint64_t timer_id;
    uint64_t fire_count;
    uint32_t interval_ms;
    bool     repeating;
} n00b_conduit_timer_payload_t;
```

---

## User events &mdash; `conduit/user_event.h`

Cross-thread wakeups.  Create an event, subscribe to it, trigger from
any thread.

```c
// Create a user event.
n00b_conduit_topic_base_t *event =
    n00b_conduit_topic_unwrap(n00b_conduit_user_event_new(c));

// Subscribe.
n00b_conduit_user_event_inbox_t *inbox =
    n00b_conduit_user_event_inbox_new(c);
n00b_conduit_user_event_subscribe(event, inbox,
    .operations = N00B_CONDUIT_OP_ALL);

// Trigger from another thread:
n00b_conduit_user_event_trigger(c, event);

// Cleanup:
n00b_conduit_user_event_destroy(c, event);
```

Supported on all platforms: kqueue (EVFILT_USER), Linux (eventfd),
Windows (CreateEvent).

---

## Service thread pool &mdash; `conduit/service.h`

For applications that want background event processing without manually
managing I/O threads.

```c
// Create a service pool.
n00b_conduit_service_t *svc = n00b_conduit_service_unwrap(
    n00b_conduit_service_new(c));

// Start default threads (1 IO + 1 signal on Unix).
n00b_conduit_service_start(svc);

// Add extra IO threads with specific backends:
auto ops = n00b_conduit_io_poll_ops()!;
n00b_conduit_service_add_io(svc, ops);

// Get the default IO thread:
auto default_io = n00b_conduit_service_default_io(svc);
if (n00b_option_is_set(default_io)) {
    n00b_conduit_svc_thread_t *st = n00b_option_get(default_io);
    auto io_opt = n00b_conduit_svc_thread_io(st);
    // Use io_opt for watching FDs...
}

// Shutdown:
n00b_conduit_service_stop(svc);
n00b_conduit_service_destroy(svc);
```

### Service thread roles

| Role | Purpose |
|------|---------|
| `N00B_CONDUIT_SVC_IO` | Runs an I/O backend poll loop |
| `N00B_CONDUIT_SVC_SIGNAL` | Dedicated signal handler thread (Unix) |

---

## Extending the type system

The conduit's type-safe pub/sub is built on four `IMPL` macros that
generate all the types and inline functions for a given payload type:

```c
// Step 1: Define your payload type.
typedef struct {
    int         sensor_id;
    double      value;
    uint64_t    timestamp;
} my_sensor_payload_t;

// Step 2: Instantiate message, inbox, subscription, and topic types.
N00B_CONDUIT_MESSAGE_IMPL(my_sensor_payload_t);
N00B_CONDUIT_INBOX_IMPL_NO_MSG(my_sensor_payload_t);
N00B_CONDUIT_SUBSCRIPTION_IMPL(my_sensor_payload_t);
N00B_CONDUIT_TOPIC_IMPL(my_sensor_payload_t);

// Step 3: Define convenience aliases (optional but recommended).
typedef n00b_conduit_message_t(my_sensor_payload_t)
    my_sensor_msg_t;
typedef n00b_conduit_inbox_t(my_sensor_payload_t)
    my_sensor_inbox_t;

// Step 4: Use it.
my_sensor_inbox_t *inbox = n00b_alloc(my_sensor_inbox_t);
n00b_conduit_inbox_init(my_sensor_payload_t, inbox, c,
                         N00B_CONDUIT_BP_DROP_OLDEST, 100);
```

---

## Cross-cutting patterns

### Error handling

All conduit APIs return `n00b_result_t` on failure or `n00b_option_t`
for absent values.  See [best_practices.md](best_practices.md) for the
full error handling guide.

The `!` postfix operator propagates errors through result chains:

```c
n00b_result_t(n00b_conduit_io_backend_t *)
my_setup(n00b_conduit_t *c)
{
    // If default_ops fails, this function returns its error.
    const n00b_conduit_io_ops_t *ops = n00b_conduit_io_default_ops()!;
    return n00b_conduit_io_new(c, ops);
}
```

### Thread safety

| Component | Mechanism |
|-----------|-----------|
| Topic registry | Lock-free dictionary |
| Publisher slot | CAS + futex |
| Inbox queues | Lock-free MPSC |
| FD owner table | Per-bucket spinlock |
| Backend registry | Atomic count + CAS |
| Service pool | Atomic slot counter |

### Memory ownership

- `n00b_conduit_t` owns topics and the backend registry.
- `n00b_conduit_fd_owner_t` owns its four topics and the write-request
  inbox.  Destroyed when the conduit is destroyed or FD is unmanaged.
- `n00b_conduit_listener_t` owns its accept topic.
- `n00b_conduit_conn_t` owns its status topic and wraps an fd_owner.
- `n00b_conduit_service_t` owns its thread pool and per-thread backends.
- Inboxes are owned by the consumer (caller-allocated).
- Messages delivered to inboxes are owned by the inbox until popped.

---

## Quick reference

| Task | API |
|------|-----|
| Create conduit | `n00b_conduit_new()` |
| Destroy conduit | `n00b_conduit_destroy(c)` |
| Create IO backend | `n00b_conduit_io_new_default(c)` |
| Watch FD | `n00b_conduit_io_watch(io, fd, ops, target)` |
| Poll events | `n00b_conduit_io_poll(io, timeout_ms)` |
| Run event loop | `n00b_conduit_io_run(io)` |
| Manage FD | `n00b_conduit_fd_manage(c, io, fd, close)` |
| Subscribe to reads | `n00b_conduit_fd_read_subscribe(topic, inbox, ...)` |
| Submit write | `n00b_conduit_fd_write_submit(owner, data, len, ...)` |
| Stream read N | `n00b_conduit_stream_read(reader, n, inbox, push)` |
| Stream read until | `n00b_conduit_stream_read_until(reader, delim, max, ...)` |
| Listen TCP | `n00b_conduit_listen_tcp(c, io, host, port, backlog)` |
| Connect TCP | `n00b_conduit_conn_tcp(c, io, host, port)` |
| Open file | `n00b_conduit_file_open(c, io, path, mode)` |
| Create timer | `n00b_conduit_timer_once(c, ms)` / `_repeat(c, ms)` |
| Watch signal | `n00b_conduit_signal_topic(c, signum)` |
| User event | `n00b_conduit_user_event_new(c)` |
| Trigger event | `n00b_conduit_user_event_trigger(c, topic)` |
| Start service | `n00b_conduit_service_new(c)` + `_start(svc)` |
| Stop service | `n00b_conduit_service_stop(svc)` |
