#include <stdlib.h>

#include "internal.h"
#include "lib/process.h"

static writ_status_t
spawn(writ_session_t *session, const char *const *argv, writ_str_t input, writ_str_t *out, writ_str_t *err,
      int *exit_code)
{
    core_buffer_t *buffers[2] = {core_buffer_empty(), core_buffer_empty()};
    core_process_status_t status = {0};
    if (core_process_capture((char *const *)argv, input.data, input.length, buffers[0], buffers[1], &status)) {
        core_buffer_free(buffers[0]);
        core_buffer_free(buffers[1]);
        return WRIT_TOOL_FAILED;
    }
    *exit_code = status.exited ? status.code : 128 + status.code;
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
writ_tool_line(writ_session_t *session, const char *const *argv)
{
    writ_run_result_t result;
    if (writ_run(session, argv, writ_cstr(""), &result) != WRIT_OK || result.exit_code) {
        return writ_cstr("");
    }
    writ_str_t line = result.out;
    while (line.length && (line.data[line.length - 1] == '\n' || line.data[line.length - 1] == '\r')) {
        line.length--;
    }
    return line;
}

writ_str_t
writ_sysroot(writ_session_t *session, writ_str_t requested)
{
    if (requested.length) {
        return requested;
    }
    const char *environment = getenv("SDKROOT");
    if (environment && *environment) {
        return writ_cstr(environment);
    }
#ifdef __APPLE__
    const char *argv[] = {"xcrun", "--show-sdk-path", nullptr};
    return writ_tool_line(session, argv);
#else
    (void)session;
    return writ_cstr("");
#endif
}

writ_str_t
writ_tool_path(writ_session_t *session, const char *tool)
{
    if (!session->llvm_prefix.length) {
        return writ_cstr(tool);
    }
    return writ_str_format(session, "%.*s/bin/%s", (int)session->llvm_prefix.length, session->llvm_prefix.data, tool);
}
