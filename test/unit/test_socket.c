/*
 * test_socket.c — Tests for conduit socket listener and connections.
 *
 * Uses loopback TCP to test the socket lifecycle via the IO backend.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/socket.h"
#include "core/alloc.h"
#include "core/runtime.h"

// ============================================================================
// 1. Create listener on loopback
// ============================================================================

static void
test_socket_listener(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    // Create a TCP listener on port 0 (OS picks free port).
    auto listen_r = n00b_conduit_listen_tcp(c, io, "127.0.0.1", 0, 5);

    if (n00b_result_is_err(listen_r)) {
        // Might fail in sandboxed CI — skip gracefully.
        printf("  [SKIP] socket listener (bind/listen failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_conduit_listener_t *listener = n00b_result_get(listen_r);

    assert(listener->fd >= 0);

    // Accept topic should be non-null.
    auto at_opt = n00b_conduit_listener_accept_topic(listener);
    assert(n00b_option_is_set(at_opt));

    n00b_conduit_listener_close(listener);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] socket listener\n");
}

// ============================================================================
// 2. Loopback connect and accept — exercises the full IO dispatch path
// ============================================================================

static void
test_socket_connect_accept(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto listen_r = n00b_conduit_listen_tcp(c, io, "127.0.0.1", 0, 5);

    if (n00b_result_is_err(listen_r)) {
        printf("  [SKIP] socket connect/accept (bind/listen failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_conduit_listener_t *listener = n00b_result_get(listen_r);

    // Get the port the OS assigned by querying the socket.
    struct sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    int grc = getsockname(listener->fd, (struct sockaddr *)&bound, &bound_len);
    assert(grc == 0);
    uint16_t port = ntohs(bound.sin_port);
    assert(port > 0);

    // Subscribe an inbox to the accept topic.
    auto accept_topic_opt = n00b_conduit_listener_accept_topic(listener);
    assert(n00b_option_is_set(accept_topic_opt));
    n00b_conduit_topic_base_t *accept_topic = n00b_option_get(accept_topic_opt);

    n00b_conduit_sock_accept_inbox_t *inbox =
        n00b_alloc(n00b_conduit_sock_accept_inbox_t);
    n00b_conduit_inbox_init(n00b_conduit_sock_accept_payload_t,
                            inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);
    assert(inbox != nullptr);

    n00b_conduit_sub_handle_t handle =
        n00b_conduit_sock_accept_subscribe(accept_topic, inbox,
                                            .operations = N00B_CONDUIT_OP_ALL);
    assert(handle != N00B_CONDUIT_INVALID_SUB_HANDLE);

    // Connect a raw client socket.
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_fd >= 0);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {0},
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = connect(client_fd, (struct sockaddr *)&addr, sizeof(addr));
    assert(rc == 0);

    // Drive the IO backend — this triggers the kqueue/poll readiness event
    // on the listener fd, which routes through deliver_io_event() →
    // n00b_conduit_listener_dispatch() → accept() → publish to accept topic.
    bool got_message = false;
    for (int attempts = 0; attempts < 20; attempts++) {
        n00b_conduit_io_poll(io, 100);

        if (n00b_conduit_sock_accept_inbox_has_messages(inbox)) {
            got_message = true;
            break;
        }
    }

    assert(got_message);

    // Pop the accept message and verify the client_fd is valid.
    n00b_conduit_sock_accept_msg_t *msg =
        n00b_conduit_sock_accept_inbox_pop(inbox);
    assert(msg != nullptr);
    assert(msg->payload.client_fd >= 0);

    // Clean up the accepted fd and client fd.
    close(msg->payload.client_fd);
    close(client_fd);

    n00b_conduit_listener_close(listener);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] socket connect/accept\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_socket:\n");
    fflush(stdout);

    test_socket_listener();
    fflush(stdout);
    test_socket_connect_accept();
    fflush(stdout);

    printf("All socket tests passed.\n");
    n00b_shutdown();
    return 0;
}
