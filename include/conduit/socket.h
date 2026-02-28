/**
 * @file socket.h
 * @brief TCP socket abstraction for the conduit system.
 *
 * Two distinct concepts:
 * - **Listener**: Owns a listening socket FD directly, accepts connections,
 *   publishes new connections as events. Does not use fd_manage.
 * - **Connection**: Wraps a @ref n00b_conduit_fd_owner_t for byte-stream I/O
 *   via the managed FD infrastructure. Adds a lifecycle/status topic.
 *
 * Usage:
 * @code
 *     n00b_conduit_listener_t *l =
 *         n00b_conduit_listen_tcp(c, io, nullptr, 8080, 128);
 *     // subscribe to n00b_conduit_listener_accept_topic(l) ...
 * @endcode
 */
#pragma once

#include "conduit/fd_managed.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// Forward declarations
typedef struct n00b_conduit_conn     n00b_conduit_conn_t;

// ============================================================================
// Connection Lifecycle Events
// ============================================================================

/**
 * @brief Connection lifecycle event types.
 */
typedef enum {
    N00B_CONDUIT_CONN_CONNECTED = 1, /**< Connection established */
    N00B_CONDUIT_CONN_REFUSED   = 2, /**< Connection refused */
    N00B_CONDUIT_CONN_RESET     = 3, /**< Connection reset by peer */
    N00B_CONDUIT_CONN_TIMEOUT   = 4, /**< Connection timed out */
    N00B_CONDUIT_CONN_ERROR     = 5, /**< Connection error */
    N00B_CONDUIT_CONN_CLOSED    = 6, /**< Connection closed */
} n00b_conduit_conn_event_t;

// ============================================================================
// Payload Types
// ============================================================================

/**
 * @brief Payload published when a listener accepts a new connection.
 *
 * The subscriber **owns** @c client_fd: it must either wrap it with
 * @c n00b_conduit_conn_from_fd or close it explicitly.  Failing to do
 * either will leak the file descriptor.
 */
typedef struct {
    int                     client_fd;    /**< Raw FD — owned by subscriber */
    struct sockaddr_storage addr;         /**< Client socket address */
    socklen_t               addr_len;     /**< Length of address structure */
} n00b_conduit_sock_accept_payload_t;

/**
 * @brief Connection lifecycle event payload.
 */
typedef struct {
    int                       fd;          /**< File descriptor */
    n00b_conduit_conn_event_t event;       /**< Lifecycle event */
    int                       error_code;  /**< errno if error occurred */
} n00b_conduit_sock_status_payload_t;

// ============================================================================
// Socket type instantiations
// ============================================================================

N00B_CONDUIT_FULL_IMPL(n00b_conduit_sock_accept_payload_t);
N00B_CONDUIT_FULL_IMPL(n00b_conduit_sock_status_payload_t);

// ============================================================================
// Convenience type aliases
// ============================================================================

typedef n00b_conduit_message_t(n00b_conduit_sock_accept_payload_t)
    n00b_conduit_sock_accept_msg_t;
typedef n00b_conduit_message_t(n00b_conduit_sock_status_payload_t)
    n00b_conduit_sock_status_msg_t;

typedef n00b_conduit_inbox_t(n00b_conduit_sock_accept_payload_t)
    n00b_conduit_sock_accept_inbox_t;
typedef n00b_conduit_inbox_t(n00b_conduit_sock_status_payload_t)
    n00b_conduit_sock_status_inbox_t;

// ============================================================================
// Convenience inbox macros
// ============================================================================

/** @brief Create a new inbox for socket accept events. */
#define n00b_conduit_sock_accept_inbox_new(c)                                  \
    ({                                                                         \
        n00b_conduit_sock_accept_inbox_t *_inbox =                             \
            n00b_alloc(n00b_conduit_sock_accept_inbox_t);                      \
        n00b_conduit_inbox_init(n00b_conduit_sock_accept_payload_t,            \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Create a new inbox for socket status events. */
#define n00b_conduit_sock_status_inbox_new(c)                                  \
    ({                                                                         \
        n00b_conduit_sock_status_inbox_t *_inbox =                             \
            n00b_alloc(n00b_conduit_sock_status_inbox_t);                      \
        n00b_conduit_inbox_init(n00b_conduit_sock_status_payload_t,            \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

// ============================================================================
// Convenience subscribe macros
// ============================================================================

/** @brief Subscribe to socket accept events. */
#define n00b_conduit_sock_accept_subscribe(topic, inbox, ...)                  \
    n00b_conduit_subscribe(n00b_conduit_sock_accept_payload_t,                 \
                           (n00b_conduit_topic_t(n00b_conduit_sock_accept_payload_t) *)(topic), \
                           inbox, __VA_ARGS__)

/** @brief Subscribe to socket status events. */
#define n00b_conduit_sock_status_subscribe(topic, inbox, ...)                  \
    n00b_conduit_subscribe(n00b_conduit_sock_status_payload_t,                 \
                           (n00b_conduit_topic_t(n00b_conduit_sock_status_payload_t) *)(topic), \
                           inbox, __VA_ARGS__)

// ============================================================================
// Convenience pop macros
// ============================================================================

#define n00b_conduit_sock_accept_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_sock_accept_payload_t, inbox)
#define n00b_conduit_sock_status_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_sock_status_payload_t, inbox)

// ============================================================================
// Convenience has_messages macros
// ============================================================================

#define n00b_conduit_sock_accept_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_sock_accept_payload_t, inbox)
#define n00b_conduit_sock_status_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_sock_status_payload_t, inbox)

// ============================================================================
// Connection State
// ============================================================================

/**
 * @brief Connection state machine values.
 */
typedef enum {
    N00B_CONDUIT_CONN_ST_CONNECTING = 0,
    N00B_CONDUIT_CONN_ST_CONNECTED  = 1,
    N00B_CONDUIT_CONN_ST_CLOSED     = 2,
    N00B_CONDUIT_CONN_ST_ERROR      = 3,
} n00b_conduit_conn_state_t;

// ============================================================================
// Listener Structure
// ============================================================================

/**
 * @brief TCP listener -- owns a listening socket, publishes accept events.
 */
struct n00b_conduit_listener {
    n00b_conduit_t              *conduit;
    n00b_conduit_io_backend_t   *io;
    int                          fd;
    n00b_conduit_topic_base_t   *accept_topic;
    uint64_t                     listener_id;
    _Atomic(bool)                active;
};

// ============================================================================
// Connection Structure
// ============================================================================

/**
 * @brief TCP connection -- wraps a managed FD with lifecycle tracking.
 */
struct n00b_conduit_conn {
    n00b_conduit_t              *conduit;
    n00b_conduit_fd_owner_t     *owner;
    n00b_conduit_topic_base_t   *status_topic;
    int                          fd;
    _Atomic(int)                 conn_state;
    bool                         connect_pending;
};

// ============================================================================
// Listener API
// ============================================================================

/**
 * @brief Create a TCP listener on addr:port.
 * @param c       Conduit instance.
 * @param io      I/O backend.
 * @param host    Host to bind (nullptr for INADDR_ANY).
 * @param port    Port number.
 * @param backlog Listen backlog size.
 * @return Ok(listener) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_listener_t *)
n00b_conduit_listen_tcp(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                        const char *host, uint16_t port, int backlog);

/**
 * @brief Get the accept topic for subscribing.
 */
extern n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_listener_accept_topic(n00b_conduit_listener_t *listener);

/**
 * @brief Stop listening (close socket, close topic).
 */
extern void
n00b_conduit_listener_close(n00b_conduit_listener_t *listener);

/**
 * @internal Dispatch readiness event to listener.
 */
extern void
n00b_conduit_listener_dispatch(n00b_conduit_listener_t *listener, uint32_t io_ops);

/**
 * @brief Lookup listener by FD.
 * @return Some(listener) if found, None otherwise.
 */
extern n00b_option_t(n00b_conduit_listener_t *)
n00b_conduit_listener_get(n00b_conduit_t *c, int fd);

// ============================================================================
// Connection API
// ============================================================================

/**
 * @brief Wrap an already-connected FD (e.g. from accept).
 * @return Ok(conn) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_conn_t *)
n00b_conduit_conn_from_fd(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                          int fd);

/**
 * @brief Initiate outbound TCP connection (non-blocking connect).
 * @return Ok(conn) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_conn_t *)
n00b_conduit_conn_tcp(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                      const char *host, uint16_t port);

/**
 * @brief Get connection status topic.
 */
extern n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_conn_status_topic(n00b_conduit_conn_t *conn);

/**
 * @brief Get the underlying FD owner (for Layer 1/2 I/O).
 */
extern n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_conn_fd_owner(n00b_conduit_conn_t *conn);

/**
 * @brief Close connection.
 */
extern void
n00b_conduit_conn_close(n00b_conduit_conn_t *conn);
