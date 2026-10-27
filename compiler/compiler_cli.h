#pragma once

#include "diag/diagnostics.h"
#include "parse/commander.h"

#include <stdio.h>

typedef struct n00bb_invocation_t {
    int                  argc;
    char               **argv;
    core_string_t        argv0;
    core_string_t       *words;
    int                  end;
    int                  command_at;
    parse_cmdr_t        *cli;
    parse_cmdr_result_t *parsed;
} n00bb_invocation_t;

void n00bb_invocation_open(n00bb_invocation_t *invocation, int argc,
                           char **argv, core_string_t argv0);
void n00bb_invocation_close(n00bb_invocation_t *invocation);
bool n00bb_invocation_flag(const n00bb_invocation_t *invocation,
                           core_string_t name, core_string_t alias);
bool n00bb_invocation_command_is(const n00bb_invocation_t *invocation,
                                 core_string_t name);
int n00bb_cli_preflight(n00bb_invocation_t *invocation, bool *handled);
void n00bb_cli_report_errors(parse_cmdr_result_t *args);
void n00bb_print_version(FILE *out);
core_string_t n00bb_default_output_name(core_string_t input);
bool n00bb_stderr_color(void);
core_diag_render_t n00bb_diag_render_options(void);
void n00bb_print_diagnostics(const core_diagnostics_t *diagnostics);
int n00bb_finish_command_diagnostics(core_diagnostics_t *diagnostics,
                                     int status);
int n00bb_env_command(core_string_t argv0);
bool n00bb_print_completions(FILE *out, core_string_t shell);
size_t n00bb_cli_command_names(core_string_t *out, size_t capacity);
size_t n00bb_cli_option_names(core_string_t command, core_string_t *out,
                              size_t capacity);
core_string_t n00bb_cli_command_argument(core_string_t command);
const parse_cmdr_flag_t *n00bb_cli_global_flags(void);
const parse_cmdr_flag_t *n00bb_cli_command_flags(core_string_t command);
core_string_t n00bb_cli_option_command(core_string_t option);
