#pragma once

#include "../core/model/model.h"
#include "writ/toolchain.h"

typedef struct {
    const char **argv;
    uint32_t     used;
} writ_command_t;

writ_command_t writ_compile_command(const writ_model_t *model, writ_str_t sysroot, uint32_t extra);
void           writ_command_add(writ_command_t *command, const char *arg);
writ_str_t     writ_probe_includes(const writ_model_t *model);
