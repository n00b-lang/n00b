#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/buffer.h"

typedef struct {
    int child_fd;
    int parent_fd;
} core_process_redirect_t;

typedef struct {
    char *const *arguments;
    bool search_path;
    const core_process_redirect_t *redirects;
    size_t redirect_count;
} core_process_spec_t;

typedef struct {
    intptr_t handle;
} core_process_t;

typedef struct {
    bool exited;
    int code;
} core_process_status_t;

int core_process_start(const core_process_spec_t *spec, core_process_t *out);
bool core_process_wait(core_process_t process, core_process_status_t *out);
bool core_process_wait_any(const core_process_t *processes, size_t count,
                           size_t *which, core_process_status_t *out);
const char *core_process_signal_name(int signal);
int core_process_exec(const core_process_spec_t *spec);
int core_process_capture(char *const *arguments, const void *input,
                         size_t input_bytes, core_buffer_t *out,
                         core_buffer_t *err, core_process_status_t *status);
