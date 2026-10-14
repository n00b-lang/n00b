#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "internal.h"

extern char **environ;

static bool
drain(int fd, core_buffer_t *into)
{
    char chunk[4096];
    ssize_t got = read(fd, chunk, sizeof(chunk));
    if (got > 0) {
        core_buffer_append(into, chunk, (size_t)got);
        return true;
    }
    return got < 0 && errno == EINTR;
}

static writ_status_t
spawn(writ_session_t *session, const char *const *argv, writ_str_t input, writ_str_t *out, writ_str_t *err,
      int *exit_code)
{
    int pipes[3][2];
    for (int i = 0; i < 3; i++) {
        if (pipe(pipes[i])) {
            return WRIT_TOOL_FAILED;
        }
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[0][0], 0);
    posix_spawn_file_actions_adddup2(&actions, pipes[1][1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipes[2][1], 2);
    for (int i = 0; i < 3; i++) {
        posix_spawn_file_actions_addclose(&actions, pipes[i][0]);
        posix_spawn_file_actions_addclose(&actions, pipes[i][1]);
    }
    pid_t pid;
    int failed = posix_spawnp(&pid, argv[0], &actions, nullptr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[0][0]);
    close(pipes[1][1]);
    close(pipes[2][1]);
    if (failed) {
        close(pipes[0][1]);
        close(pipes[1][0]);
        close(pipes[2][0]);
        return WRIT_TOOL_FAILED;
    }
    fcntl(pipes[0][1], F_SETFL, O_NONBLOCK);
    core_buffer_t *buffers[2] = {core_buffer_empty(), core_buffer_empty()};
    struct pollfd fds[3] = {{pipes[1][0], POLLIN, 0}, {pipes[2][0], POLLIN, 0}, {pipes[0][1], POLLOUT, 0}};
    size_t sent = 0;
    if (!input.length) {
        close(pipes[0][1]);
        fds[2].fd = -1;
    }
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 3, -1) < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP)) && !drain(fds[i].fd, buffers[i])) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
        if (fds[2].fd >= 0 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t wrote = write(fds[2].fd, input.data + sent, input.length - sent);
            sent += wrote > 0 ? (size_t)wrote : 0;
            if (sent == input.length || (wrote < 0 && errno != EAGAIN && errno != EINTR)) {
                close(fds[2].fd);
                fds[2].fd = -1;
            }
        }
    }
    if (fds[2].fd >= 0) {
        close(fds[2].fd);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    *out = writ_str_from_buffer(session, buffers[0]);
    *err = writ_str_from_buffer(session, buffers[1]);
    return WRIT_OK;
}

static writ_status_t
default_runner(void *context, const char *const *argv, writ_str_t input, writ_str_t *out, writ_str_t *err,
               int *exit_code)
{
    return spawn(context, argv, input, out, err, exit_code);
}

writ_status_t
writ_run(writ_session_t *session, const char *const *argv, writ_str_t input, writ_run_result_t *result)
{
    *result = (writ_run_result_t){0};
    writ_process_runner_t runner = session->runner.run ? session->runner
                                                       : (writ_process_runner_t){default_runner, session};
    writ_status_t status = runner.run(runner.context, argv, input, &result->out, &result->err, &result->exit_code);
    return status;
}

writ_str_t
writ_tool_path(writ_session_t *session, const char *tool)
{
    if (!session->llvm_prefix.length) {
        return writ_cstr(tool);
    }
    return writ_str_format(session, "%.*s/bin/%s", (int)session->llvm_prefix.length, session->llvm_prefix.data, tool);
}
