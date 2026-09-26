#pragma once

#include "parse/tree.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdio.h>

typedef enum {
    PARSE_CMDR_TYPE_BOOL,
    PARSE_CMDR_TYPE_WORD,
    PARSE_CMDR_TYPE_INT,
    PARSE_CMDR_TYPE_FLOAT,
} parse_cmdr_arg_type_t;

typedef struct {
    core_string_t        name;
    core_string_t        alias;
    parse_cmdr_arg_type_t type;
    bool                takes_value;
    core_string_t        doc;
} parse_cmdr_flag_t;

typedef struct {
    core_string_t           name;
    core_string_t           doc;
    const parse_cmdr_flag_t *flags;
    core_string_t           arg;
    int                    min_args;
    int                    max_args; /* Negative means unbounded. */
} parse_cmdr_cmd_t;

typedef struct {
    core_string_t           tool;
    core_string_t           usage;
    const parse_cmdr_flag_t *flags;
    const parse_cmdr_cmd_t  *commands;
    core_string_t           arg;
    int                    min_args;
    int                    max_args; /* Negative means unbounded. */
} parse_cmdr_spec_t;

typedef struct parse_cmdr_t        parse_cmdr_t;
typedef struct parse_cmdr_result_t parse_cmdr_result_t;

parse_cmdr_t *parse_cmdr_new(const parse_cmdr_spec_t *spec);
void        parse_cmdr_free(parse_cmdr_t *c);
bool        parse_cmdr_flag_set_repeatable(parse_cmdr_t *c,
                                           core_string_t command,
                                           core_string_t flag,
                                           bool repeatable);
void        parse_cmdr_print_usage(parse_cmdr_t *c, FILE *out);

parse_cmdr_result_t *parse_cmdr_parse(parse_cmdr_t *c, int nargs,
                                  const char **args);
void               parse_cmdr_result_free(parse_cmdr_result_t *r);

bool         parse_cmdr_result_ok(parse_cmdr_result_t *r);
core_string_t parse_cmdr_result_command(parse_cmdr_result_t *r);
int32_t      parse_cmdr_result_error_count(parse_cmdr_result_t *r);
core_string_t parse_cmdr_result_error(parse_cmdr_result_t *r, int index);

bool         parse_cmdr_flag_present(parse_cmdr_result_t *r, core_string_t flag);
core_string_t parse_cmdr_flag_str(parse_cmdr_result_t *r, core_string_t flag);
int32_t      parse_cmdr_flag_value_count(parse_cmdr_result_t *r,
                                         core_string_t flag);
core_string_t parse_cmdr_flag_str_at(parse_cmdr_result_t *r,
                                     core_string_t flag, int32_t index);
int64_t      parse_cmdr_flag_int(parse_cmdr_result_t *r, core_string_t flag,
                               int64_t dflt);

int32_t      parse_cmdr_arg_count(parse_cmdr_result_t *r);
core_string_t parse_cmdr_arg_str(parse_cmdr_result_t *r, int index);
