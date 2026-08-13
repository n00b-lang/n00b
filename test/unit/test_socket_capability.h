#pragma once

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

static int
n00b_test_skip_if_tcp_listener_unavailable(const char *suite)
{
    (void)suite;
    return 0;
}

static int
n00b_test_skip_if_unix_listener_unavailable(const char *suite)
{
    (void)suite;
    return 0;
}

static int
n00b_test_skip_if_udp_bind_unavailable(const char *suite)
{
    (void)suite;
    return 0;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
n00b_test_socket_capability_denied(int err)
{
    return err == EPERM || err == EACCES || err == EAFNOSUPPORT
        || err == EPROTONOSUPPORT || err == ENOSYS;
}

static int
n00b_test_socket_skip(const char *suite, const char *capability, int err)
{
    printf("  [SKIP] %s requires %s; host denied local sockets: %s\n",
           suite, capability, strerror(err));
    return 77;
}

static int
n00b_test_skip_if_tcp_listener_unavailable(const char *suite)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        int err = errno;
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "TCP bind/listen", err);
        }
        return 0;
    }

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(0);
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        close(fd);
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "TCP bind/listen", err);
        }
        return 0;
    }

    if (listen(fd, 1) != 0) {
        int err = errno;
        close(fd);
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "TCP bind/listen", err);
        }
        return 0;
    }

    close(fd);
    return 0;
}

static int
n00b_test_skip_if_unix_listener_unavailable(const char *suite)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        int err = errno;
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "AF_UNIX bind/listen", err);
        }
        return 0;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    snprintf(addr.sun_path,
             sizeof(addr.sun_path),
             "/tmp/n00b-socket-probe-%ld.sock",
             (long)getpid());
    unlink(addr.sun_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        close(fd);
        unlink(addr.sun_path);
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "AF_UNIX bind/listen", err);
        }
        return 0;
    }

    if (listen(fd, 1) != 0) {
        int err = errno;
        close(fd);
        unlink(addr.sun_path);
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "AF_UNIX bind/listen", err);
        }
        return 0;
    }

    close(fd);
    unlink(addr.sun_path);
    return 0;
}

static int
n00b_test_skip_if_udp_bind_unavailable(const char *suite)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        int err = errno;
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "UDP bind", err);
        }
        return 0;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(0);
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        close(fd);
        if (n00b_test_socket_capability_denied(err)) {
            return n00b_test_socket_skip(suite, "UDP bind", err);
        }
        return 0;
    }

    close(fd);
    return 0;
}

#endif
