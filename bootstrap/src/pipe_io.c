/**
 * @file pipe_io.c
 * @brief Non-blocking pipe I/O implementation.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ncc_limits.h"
#include "pipe_io.h"

ncc_buf_t *
ncc_pipe_io(int write_fd, int read_fd,
            const char *data, size_t len,
            const char *prog_name)
{
    signal(SIGPIPE, SIG_IGN);

    // Set non-blocking on write fd
    int wflags = fcntl(write_fd, F_GETFL, 0);
    fcntl(write_fd, F_SETFL, wflags | O_NONBLOCK);

    // Set non-blocking on read fd (if present)
    bool has_read = (read_fd >= 0);
    if (has_read) {
        int rflags = fcntl(read_fd, F_GETFL, 0);
        fcntl(read_fd, F_SETFL, rflags | O_NONBLOCK);
    }

    ncc_buf_t  *output       = has_read ? ncc_buf_alloc(0) : nullptr;
    const char *write_ptr    = data;
    size_t      write_remain = len;

    struct pollfd fds[2];
    fds[0].fd = write_fd;
    fds[1].fd = has_read ? read_fd : -1; // poll() ignores fd < 0

    int write_open = 1;
    int read_open  = has_read ? 1 : 0;

    while (write_open || read_open) {
        fds[0].events  = write_open ? POLLOUT : 0;
        fds[1].events  = read_open ? POLLIN : 0;
        fds[0].revents = 0;
        fds[1].revents = 0;

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (prog_name) {
                fprintf(stderr, "%s: poll: %s\n", prog_name, strerror(errno));
            }
            break;
        }

        // Write to child stdin
        if (write_open && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (write_remain > 0) {
                ssize_t written = write(write_fd, write_ptr, write_remain);
                if (written > 0) {
                    write_ptr += written;
                    write_remain -= written;
                }
                else if (written < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Retry later
                    }
                    else {
                        // EPIPE or fatal — stop writing
                        close(write_fd);
                        write_open = 0;
                    }
                }
            }
            if (write_open && write_remain == 0) {
                close(write_fd);
                write_open = 0;
            }
        }

        // Read from child stdout
        if (read_open && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            char    buf[NCC_IO_CHUNK];
            ssize_t n = read(read_fd, buf, sizeof(buf));
            if (n > 0) {
                output = ncc_buf_concat(output, buf, n);
            }
            else if (n == 0) {
                close(read_fd);
                read_open = 0;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close(read_fd);
                read_open = 0;
            }
        }
    }

    return output;
}
