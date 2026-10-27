#pragma once

#include "diag/diagnostics.h"
#include "lib/string.h"
#include "parse/commander.h"

typedef enum {
    N00BB_SETTING_BOOL,
    N00BB_SETTING_INT,
    N00BB_SETTING_CHOICE,
    N00BB_SETTING_STRINGS,
    N00BB_SETTING_PATH,
    N00BB_SETTING_PATHS,
    N00BB_SETTING_PROGRAM,
} n00bb_setting_type_t;

typedef struct {
    core_string_t        section;
    core_string_t        name;
    n00bb_setting_type_t type;
    core_string_t        default_value;
    core_string_t        environment;
    core_string_t        environment_match;
    core_string_t        choices;
    int64_t              minimum;
    int64_t              maximum;
} n00bb_setting_spec_t;

typedef enum {
#define N00BB_SETTING(id, ...) N00BB_KEY_##id,
#include "settings.def"
#undef N00BB_SETTING
    N00BB_KEY_COUNT,
} n00bb_setting_id_t;

extern const n00bb_setting_spec_t n00bb_setting_specs[N00BB_KEY_COUNT];

typedef struct {
    n00bb_setting_id_t id;
    bool               value;
} n00bb_flag_setting_t;

typedef enum {
    N00BB_SETTING_FROM_DEFAULT,
    N00BB_SETTING_FROM_USER_FILE,
    N00BB_SETTING_FROM_PROJECT_FILE,
    N00BB_SETTING_FROM_ENVIRONMENT,
    N00BB_SETTING_FROM_FLAG,
} n00bb_setting_source_t;

struct n00bb_invocation_t;

void n00bb_config_load(const struct n00bb_invocation_t *invocation);
int n00bb_config_report(void);
int n00bb_config_command(void);

bool n00bb_setting_bool(n00bb_setting_id_t id);
int64_t n00bb_setting_int(n00bb_setting_id_t id);
core_string_t n00bb_setting_string(n00bb_setting_id_t id);
size_t n00bb_setting_count(n00bb_setting_id_t id);
core_string_t n00bb_setting_at(n00bb_setting_id_t id, size_t index);
n00bb_setting_source_t n00bb_setting_source(n00bb_setting_id_t id);
core_string_t n00bb_setting_origin(n00bb_setting_id_t id);
core_string_t n00bb_setting_key(n00bb_setting_id_t id);
