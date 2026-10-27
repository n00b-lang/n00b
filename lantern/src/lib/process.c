#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "lib/process.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "os_internal.h"
#include <fcntl.h>
#include <io.h>

#include "lib/alloc.h"

enum { process_fd_limit = 16 };

static void
append_quoted(core_buffer_t *line, core_string_t argument)
{
    bool plain = argument.u8_bytes != 0;
    for (size_t i = 0; plain && i < argument.u8_bytes; i++) {
        char at = argument.data[i];
        if (at == ' ' || at == '\t' || at == '\n' || at == '\v' || at == '"')
            plain = false;
    }
    if (plain) {
        core_buffer_append_string(line, argument);
        return;
    }
    core_buffer_append(line, "\"", 1);
    size_t backslashes = 0;
    for (size_t i = 0; i <= argument.u8_bytes; i++) {
        bool end = i == argument.u8_bytes;
        if (!end && argument.data[i] == '\\') {
            backslashes++;
            continue;
        }
        size_t repeat = end ? 2 * backslashes
                      : argument.data[i] == '"' ? 2 * backslashes + 1
                      : backslashes;
        for (size_t j = 0; j < repeat; j++) core_buffer_append(line, "\\", 1);
        backslashes = 0;
        if (!end) core_buffer_append(line, argument.data + i, 1);
    }
    core_buffer_append(line, "\"", 1);
}

static wchar_t *
command_line(char *const *arguments)
{
    core_buffer_t line = {0};
    for (size_t i = 0; arguments[i]; i++) {
        if (i) core_buffer_append(&line, " ", 1);
        core_string_t argument = core_string_from_cstr(arguments[i]);
        append_quoted(&line, argument);
        core_free(argument.data);
    }
    wchar_t *wide = core_os_wide(core_buffer_string(&line));
    core_buffer_release(&line);
    return wide;
}

static HANDLE
inheritable(HANDLE handle)
{
    HANDLE copy = INVALID_HANDLE_VALUE;
    if (handle == INVALID_HANDLE_VALUE || !handle
        || !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                            &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return INVALID_HANDLE_VALUE;
    return copy;
}

static int
launch(char *const *arguments, HANDLE *slots, int slot_count,
       PROCESS_INFORMATION *info)
{
    wchar_t *command = command_line(arguments);
    if (!command) return ENOMEM;
    core_string_t program = core_string_from_cstr(arguments[0]);
    bool named = core_string_find_byte(program, '/') >= 0
        || core_string_find_byte(program, '\\') >= 0;
    wchar_t *application = named ? core_os_wide(program) : nullptr;
    if (application && GetFileAttributesW(application) == INVALID_FILE_ATTRIBUTES
        && !core_string_ends_with(program, CORE_STRING_STATIC(".exe"))) {
        core_buffer_t executable = {0};
        core_buffer_append_string(&executable, program);
        core_buffer_append(&executable, ".exe", 4);
        wchar_t *suffixed = core_os_wide(core_buffer_string(&executable));
        core_buffer_release(&executable);
        if (suffixed && GetFileAttributesW(suffixed) != INVALID_FILE_ATTRIBUTES) {
            core_free(application);
            application = suffixed;
        }
        else core_free(suffixed);
    }
    core_free(program.data);
    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = slots[0],
        .hStdOutput = slots[1],
        .hStdError = slots[2],
    };
    uint8_t *table = nullptr;
    if (slot_count > 3) {
        size_t bytes = sizeof(int) + (size_t)slot_count * (1 + sizeof(HANDLE));
        table = core_alloc_size(bytes, 1);
        if (!table) {
            core_free(application);
            core_free(command);
            return ENOMEM;
        }
        int count = slot_count;
        memcpy(table, &count, sizeof(count));
        for (int i = 0; i < slot_count; i++) {
            table[sizeof(int) + i] =
                slots[i] == INVALID_HANDLE_VALUE ? 0 : 0x01;
            memcpy(table + sizeof(int) + slot_count
                       + (size_t)i * sizeof(HANDLE),
                   &slots[i], sizeof(HANDLE));
        }
        startup.cbReserved2 = (WORD)bytes;
        startup.lpReserved2 = table;
    }
    BOOL created = CreateProcessW(application, command, nullptr, nullptr, TRUE,
                                  0, nullptr, nullptr, &startup, info);
    DWORD error = created ? 0 : GetLastError();
    core_free(application);
    core_free(table);
    core_free(command);
    return created ? 0 : core_os_errno(error);
}

int
core_process_start(const core_process_spec_t *spec, core_process_t *out)
{
    if (!spec || !spec->arguments || !spec->arguments[0] || !out)
        return EINVAL;
    HANDLE slots[process_fd_limit];
    int slot_count = 3;
    for (int i = 0; i < process_fd_limit; i++) slots[i] = INVALID_HANDLE_VALUE;
    for (size_t i = 0; i < spec->redirect_count; i++) {
        int child = spec->redirects[i].child_fd;
        if (child < 0 || child >= process_fd_limit) return EINVAL;
        if (slots[child] != INVALID_HANDLE_VALUE) CloseHandle(slots[child]);
        slots[child] = inheritable(
            (HANDLE)_get_osfhandle(spec->redirects[i].parent_fd));
        if (child + 1 > slot_count) slot_count = child + 1;
    }
    DWORD standard[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
                         STD_ERROR_HANDLE};
    for (int i = 0; i < 3; i++)
        if (slots[i] == INVALID_HANDLE_VALUE)
            slots[i] = inheritable(GetStdHandle(standard[i]));
    PROCESS_INFORMATION info = {0};
    int error = launch(spec->arguments, slots, slot_count, &info);
    for (int i = 0; i < slot_count; i++)
        if (slots[i] != INVALID_HANDLE_VALUE) CloseHandle(slots[i]);
    if (error) return error;
    CloseHandle(info.hThread);
    out->handle = (intptr_t)info.hProcess;
    return 0;
}

const char *
core_process_signal_name(int signal)
{
    (void)signal;
    return "unknown signal";
}

bool
core_process_wait(core_process_t process, core_process_status_t *out)
{
    HANDLE handle = (HANDLE)process.handle;
    DWORD code = 0;
    bool ok = WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0
        && GetExitCodeProcess(handle, &code);
    CloseHandle(handle);
    if (ok && out) *out = (core_process_status_t){
        .exited = true, .code = (int)code,
    };
    return ok;
}

bool
core_process_wait_any(const core_process_t *processes, size_t count,
                      size_t *which, core_process_status_t *out)
{
    if (!processes || !count || count > MAXIMUM_WAIT_OBJECTS || !which)
        return false;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    for (size_t i = 0; i < count; i++)
        handles[i] = (HANDLE)processes[i].handle;
    DWORD done = WaitForMultipleObjects((DWORD)count, handles, FALSE,
                                        INFINITE);
    if (done >= WAIT_OBJECT_0 + count) return false;
    *which = done - WAIT_OBJECT_0;
    return core_process_wait(processes[*which], out);
}

int
core_process_exec(const core_process_spec_t *spec)
{
    core_process_t child;
    core_process_status_t status = {0};
    int error = core_process_start(spec, &child);
    if (error) return error;
    if (!core_process_wait(child, &status)) return ECHILD;
    exit(status.code);
}

typedef struct {
    int fd;
    core_buffer_t *buffer;
    const uint8_t *input;
    size_t input_bytes;
} pipe_job_t;

static DWORD WINAPI
read_pipe(LPVOID raw)
{
    pipe_job_t *job = raw;
    char chunk[4096];
    int got = 0;
    while ((got = _read(job->fd, chunk, sizeof(chunk))) > 0)
        core_buffer_append(job->buffer, chunk, (size_t)got);
    return 0;
}

static DWORD WINAPI
write_pipe(LPVOID raw)
{
    pipe_job_t *job = raw;
    size_t sent = 0;
    while (sent < job->input_bytes) {
        size_t left = job->input_bytes - sent;
        int wrote = _write(job->fd, job->input + sent,
                           left > 0x40000000u ? 0x40000000u : (unsigned)left);
        if (wrote <= 0) break;
        sent += (size_t)wrote;
    }
    _close(job->fd);
    return 0;
}

int
core_process_capture(char *const *arguments, const void *input,
                     size_t input_bytes, core_buffer_t *out,
                     core_buffer_t *err, core_process_status_t *status)
{
    if (!arguments || !arguments[0] || !out || !err || !status)
        return EINVAL;
    int pipes[3][2];
    for (int i = 0; i < 3; i++) {
        if (_pipe(pipes[i], 65536, _O_BINARY | _O_NOINHERIT)) {
            int error = errno;
            for (int j = 0; j < i; j++) {
                _close(pipes[j][0]);
                _close(pipes[j][1]);
            }
            return error;
        }
    }
    core_process_redirect_t redirects[3] = {
        {0, pipes[0][0]}, {1, pipes[1][1]}, {2, pipes[2][1]},
    };
    core_process_spec_t spec = {
        .arguments = arguments, .search_path = true,
        .redirects = redirects, .redirect_count = 3,
    };
    core_process_t child;
    int error = core_process_start(&spec, &child);
    _close(pipes[0][0]);
    _close(pipes[1][1]);
    _close(pipes[2][1]);
    if (error) {
        _close(pipes[0][1]);
        _close(pipes[1][0]);
        _close(pipes[2][0]);
        return error;
    }
    pipe_job_t writer = {
        .fd = pipes[0][1], .input = input, .input_bytes = input_bytes,
    };
    pipe_job_t errors = {.fd = pipes[2][0], .buffer = err};
    pipe_job_t output = {.fd = pipes[1][0], .buffer = out};
    HANDLE threads[2] = {
        CreateThread(nullptr, 0, write_pipe, &writer, 0, nullptr),
        CreateThread(nullptr, 0, read_pipe, &errors, 0, nullptr),
    };
    if (!threads[0]) write_pipe(&writer);
    if (!threads[1]) read_pipe(&errors);
    read_pipe(&output);
    for (int i = 0; i < 2; i++)
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
    _close(pipes[1][0]);
    _close(pipes[2][0]);
    return core_process_wait(child, status) ? 0 : ECHILD;
}

#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int
core_process_start(const core_process_spec_t *spec, core_process_t *out)
{
    if (!spec || !spec->arguments || !spec->arguments[0] || !out)
        return EINVAL;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return error;
    for (size_t i = 0; !error && i < spec->redirect_count; i++)
        error = posix_spawn_file_actions_adddup2(&actions,
            spec->redirects[i].parent_fd, spec->redirects[i].child_fd);
    pid_t child = 0;
    if (!error)
        error = spec->search_path
            ? posix_spawnp(&child, spec->arguments[0], &actions, nullptr,
                           spec->arguments, environ)
            : posix_spawn(&child, spec->arguments[0], &actions, nullptr,
                          spec->arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (!error) out->handle = (intptr_t)child;
    return error;
}

const char *
core_process_signal_name(int signal)
{
    return strsignal(signal);
}

bool
core_process_wait(core_process_t process, core_process_status_t *out)
{
    pid_t child = (pid_t)process.handle;
    int status = 0;
    pid_t got;
    while ((got = waitpid(child, &status, 0)) < 0 && errno == EINTR) {}
    if (got != child) return false;
    if (out) *out = WIFEXITED(status)
        ? (core_process_status_t){.exited = true, .code = WEXITSTATUS(status)}
        : (core_process_status_t){
              .code = WIFSIGNALED(status) ? WTERMSIG(status) : 0,
          };
    return true;
}

bool
core_process_wait_any(const core_process_t *processes, size_t count,
                      size_t *which, core_process_status_t *out)
{
    if (!processes || !count || !which) return false;
    for (;;) {
        int status = 0;
        pid_t done = waitpid(-1, &status, 0);
        if (done < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        for (size_t i = 0; i < count; i++)
            if ((pid_t)processes[i].handle == done) {
                *which = i;
                if (out) *out = WIFEXITED(status)
                    ? (core_process_status_t){
                          .exited = true, .code = WEXITSTATUS(status),
                      }
                    : (core_process_status_t){
                          .code = WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                      };
                return true;
            }
    }
}

int
core_process_exec(const core_process_spec_t *spec)
{
    if (!spec || !spec->arguments || !spec->arguments[0]) return EINVAL;
    for (size_t i = 0; i < spec->redirect_count; i++) {
        int from = spec->redirects[i].parent_fd;
        int to = spec->redirects[i].child_fd;
        int flags = from == to ? fcntl(to, F_GETFD) : 0;
        if (from == to ? flags < 0 || fcntl(to, F_SETFD, flags & ~FD_CLOEXEC) < 0
                       : dup2(from, to) < 0)
            return errno;
    }
    if (spec->search_path) execvp(spec->arguments[0], spec->arguments);
    else execv(spec->arguments[0], spec->arguments);
    return errno;
}

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

int
core_process_capture(char *const *arguments, const void *input,
                     size_t input_bytes, core_buffer_t *out,
                     core_buffer_t *err, core_process_status_t *status)
{
    if (!arguments || !arguments[0] || !out || !err || !status)
        return EINVAL;
    int pipes[3][2];
    for (int i = 0; i < 3; i++) {
        if (pipe(pipes[i])) {
            int error = errno;
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return error;
        }
        fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC);
    }
    core_process_redirect_t redirects[3] = {
        {0, pipes[0][0]}, {1, pipes[1][1]}, {2, pipes[2][1]},
    };
    core_process_spec_t spec = {
        .arguments = arguments, .search_path = true,
        .redirects = redirects, .redirect_count = 3,
    };
    core_process_t child;
    int error = core_process_start(&spec, &child);
    close(pipes[0][0]);
    close(pipes[1][1]);
    close(pipes[2][1]);
    if (error) {
        close(pipes[0][1]);
        close(pipes[1][0]);
        close(pipes[2][0]);
        return error;
    }
    fcntl(pipes[0][1], F_SETFL, O_NONBLOCK);
    core_buffer_t *buffers[2] = {out, err};
    struct pollfd fds[3] = {
        {pipes[1][0], POLLIN, 0}, {pipes[2][0], POLLIN, 0},
        {pipes[0][1], POLLOUT, 0},
    };
    const char *bytes = input;
    size_t sent = 0;
    if (!input_bytes) {
        close(pipes[0][1]);
        fds[2].fd = -1;
    }
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 3, -1) < 0 && errno != EINTR) break;
        for (int i = 0; i < 2; i++)
            if (fds[i].fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP))
                && !drain(fds[i].fd, buffers[i])) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        if (fds[2].fd >= 0
            && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t wrote = write(fds[2].fd, bytes + sent, input_bytes - sent);
            sent += wrote > 0 ? (size_t)wrote : 0;
            if (sent == input_bytes
                || (wrote < 0 && errno != EAGAIN && errno != EINTR)) {
                close(fds[2].fd);
                fds[2].fd = -1;
            }
        }
    }
    for (int i = 0; i < 3; i++)
        if (fds[i].fd >= 0) close(fds[i].fd);
    return core_process_wait(child, status) ? 0 : ECHILD;
}
#endif
