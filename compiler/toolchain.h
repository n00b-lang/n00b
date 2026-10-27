#pragma once

#include "lib/string.h"

typedef enum {
    N00BB_TOOLCHAIN_DEV,
    N00BB_TOOLCHAIN_INSTALLED,
    N00BB_TOOLCHAIN_HOME,
} n00bb_toolchain_mode_t;

typedef struct {
    n00bb_toolchain_mode_t mode;
    core_string_t id;
    core_string_t root;
    core_string_t store;
    core_string_t stdlib;
    core_string_t runtime;
    core_string_t grammar;
    core_string_t writ_profiles;
    core_string_t writ_contracts;
    char *clang;
    char *cargo;
    char *linker;
    char *archiver;
} n00bb_toolchain_t;

void n00bb_toolchain_init(core_string_t argv0);
const n00bb_toolchain_t *n00bb_toolchain(void);
core_string_t n00bb_toolchain_id(void);
bool n00bb_toolchain_is_root(core_string_t root);
char *n00bb_toolchain_find_on_path(core_string_t name);
bool n00bb_toolchain_runnable(const char *program);
