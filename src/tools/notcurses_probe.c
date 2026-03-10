#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/backend_notcurses.h"

typedef struct {
    int startup_ok;
    int has_pixel;
    int has_freetype;
    int font_metrics_cap;
    int timed_out;
} probe_result_t;

static void
print_result(int built, const probe_result_t *r)
{
    printf("notcurses_built=%d\n", built);
    printf("startup_ok=%d\n", r->startup_ok);
    printf("has_pixel=%d\n", r->has_pixel);
    printf("has_freetype=%d\n", r->has_freetype);
    printf("font_metrics_cap=%d\n", r->font_metrics_cap);
    printf("timed_out=%d\n", r->timed_out);
}

static int
parse_timeout_ms(void)
{
    const char *env = getenv("N00B_NOTCURSES_PROBE_TIMEOUT_MS");
    if (!env || !env[0]) {
        return 3000;
    }

    char *end = nullptr;
    long  v   = strtol(env, &end, 10);
    if (end == env || *end != '\0') {
        return 3000;
    }
    if (v < 100) {
        v = 100;
    }
    if (v > 30000) {
        v = 30000;
    }
    return (int)v;
}

static void
run_probe(probe_result_t *out, int argc, char **argv)
{
    memset(out, 0, sizeof(*out));

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

#if defined(N00B_HAVE_NOTCURSES)
#ifndef _WIN32
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    int devnull_fd   = open("/dev/null", O_WRONLY);
    bool muted_stdio = false;
    if (saved_stdout >= 0 && saved_stderr >= 0 && devnull_fd >= 0) {
        fflush(stdout);
        fflush(stderr);
        if (dup2(devnull_fd, STDOUT_FILENO) >= 0
            && dup2(devnull_fd, STDERR_FILENO) >= 0) {
            muted_stdio = true;
        }
    }
    if (devnull_fd >= 0) {
        close(devnull_fd);
    }
#endif

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    if (!ctx) {
#ifndef _WIN32
        if (muted_stdio) {
            fflush(stdout);
            fflush(stderr);
            (void)dup2(saved_stdout, STDOUT_FILENO);
            (void)dup2(saved_stderr, STDERR_FILENO);
        }
        if (saved_stdout >= 0) {
            close(saved_stdout);
        }
        if (saved_stderr >= 0) {
            close(saved_stderr);
        }
#endif
        n00b_shutdown();
        return;
    }

    n00b_render_cap_t caps = n00b_renderer_notcurses.capabilities(ctx);
    out->startup_ok        = 1;
    out->has_pixel         = n00b_notcurses_has_pixel_support(ctx) ? 1 : 0;
    out->has_freetype      = n00b_notcurses_has_freetype(ctx) ? 1 : 0;
    out->font_metrics_cap  = (caps & N00B_RCAP_FONT_METRICS) ? 1 : 0;

    n00b_renderer_notcurses.destroy(ctx);
#ifndef _WIN32
    if (muted_stdio) {
        fflush(stdout);
        fflush(stderr);
        (void)dup2(saved_stdout, STDOUT_FILENO);
        (void)dup2(saved_stderr, STDERR_FILENO);
    }
    if (saved_stdout >= 0) {
        close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        close(saved_stderr);
    }
#endif
#endif

    n00b_shutdown();
}

int
main(int argc, char **argv)
{
#if !defined(N00B_HAVE_NOTCURSES)
    probe_result_t r = {0};
    print_result(0, &r);
    return 0;
#else
#ifndef _WIN32
    int timeout_ms = parse_timeout_ms();

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        probe_result_t r = {0};
        run_probe(&r, argc, argv);
        print_result(1, &r);
        return r.startup_ok ? 0 : 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        probe_result_t r = {0};
        close(pipefd[0]);
        run_probe(&r, argc, argv);
        (void)write(pipefd[1], &r, sizeof(r));
        close(pipefd[1]);
        _exit(0);
    }
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        probe_result_t r = {0};
        run_probe(&r, argc, argv);
        print_result(1, &r);
        return r.startup_ok ? 0 : 1;
    }

    close(pipefd[1]);

    struct pollfd pfd = {
        .fd     = pipefd[0],
        .events = POLLIN,
    };

    int pr;
    do {
        pr = poll(&pfd, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR);

    probe_result_t r = {0};
    if (pr <= 0) {
        r.timed_out = 1;
        kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
        close(pipefd[0]);
        print_result(1, &r);
        return 2;
    }

    ssize_t n = read(pipefd[0], &r, sizeof(r));
    close(pipefd[0]);
    (void)waitpid(pid, nullptr, 0);

    if (n != (ssize_t)sizeof(r)) {
        memset(&r, 0, sizeof(r));
    }

    print_result(1, &r);
    return r.startup_ok ? 0 : 1;
#else
    probe_result_t r = {0};
    run_probe(&r, argc, argv);
    print_result(1, &r);
    return r.startup_ok ? 0 : 1;
#endif
#endif
}
