#pragma once

#include "parse/grammar.h"
#include "parse/parse_tree.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdio.h>

enum {
    NCC_CMDR_TID_WORD  = 0,
    NCC_CMDR_TID_INT   = 1,
    NCC_CMDR_TID_FLOAT = 2,
    NCC_CMDR_TID_BOOL  = 3,
    NCC_CMDR_TID_EQ    = 4,
    NCC_CMDR_TID_COMMA = 5,
    NCC_CMDR_TID_DD    = 6,
    NCC_CMDR_TID_FLAG  = 7,
    NCC_CMDR_TID_COUNT = 8,
};

typedef enum {
    NCC_CMDR_TYPE_BOOL,
    NCC_CMDR_TYPE_WORD,
    NCC_CMDR_TYPE_INT,
    NCC_CMDR_TYPE_FLOAT,
} ncc_cmdr_arg_type_t;

typedef enum {
    NCC_CMDR_VAL_NONE,
    NCC_CMDR_VAL_BOOL,
    NCC_CMDR_VAL_INT,
    NCC_CMDR_VAL_FLOAT,
    NCC_CMDR_VAL_STR,
} ncc_cmdr_val_tag_t;

typedef struct {
    ncc_cmdr_val_tag_t tag;
    union {
        bool         b;
        int64_t      i;
        double       f;
        ncc_string_t s;
    };
} ncc_cmdr_val_t;

typedef struct ncc_cmdr_t        ncc_cmdr_t;
typedef struct ncc_cmdr_result_t ncc_cmdr_result_t;

ncc_cmdr_t *ncc_cmdr_new(void);
void         ncc_cmdr_free(ncc_cmdr_t *c);

void ncc_cmdr_set_name(ncc_cmdr_t *c, ncc_string_t name);
void ncc_cmdr_add_command(ncc_cmdr_t *c, ncc_string_t name, ncc_string_t doc);
void ncc_cmdr_add_subcommand(ncc_cmdr_t *c, ncc_string_t parent,
                             ncc_string_t name, ncc_string_t doc);
void ncc_cmdr_add_flag(ncc_cmdr_t *c, ncc_string_t command,
                       ncc_string_t flag_name, ncc_cmdr_arg_type_t type,
                       bool takes_value, ncc_string_t doc);
void ncc_cmdr_add_flag_alias(ncc_cmdr_t *c, ncc_string_t command,
                             ncc_string_t flag_name, ncc_string_t alias);
void ncc_cmdr_add_positional(ncc_cmdr_t *c, ncc_string_t command,
                             ncc_string_t name, ncc_cmdr_arg_type_t type,
                             int min, int max);
void ncc_cmdr_finalize(ncc_cmdr_t *c);

int32_t      ncc_cmdr_command_count(ncc_cmdr_t *c);
ncc_string_t ncc_cmdr_command_name(ncc_cmdr_t *c, int32_t index);
ncc_string_t ncc_cmdr_command_doc(ncc_cmdr_t *c, int32_t index);

int32_t      ncc_cmdr_flag_count(ncc_cmdr_t *c, ncc_string_t command);
ncc_string_t ncc_cmdr_flag_name(ncc_cmdr_t *c, ncc_string_t command,
                                int32_t index);
ncc_string_t ncc_cmdr_flag_doc(ncc_cmdr_t *c, ncc_string_t command,
                               int32_t index);
bool         ncc_cmdr_flag_takes_value(ncc_cmdr_t *c, ncc_string_t command,
                                       int32_t index);

ncc_cmdr_result_t *ncc_cmdr_parse(ncc_cmdr_t *c, int argc,
                                  const char **argv);
ncc_cmdr_result_t *ncc_cmdr_parse_string(ncc_cmdr_t *c, ncc_string_t cmdline);
void               ncc_cmdr_result_free(ncc_cmdr_result_t *r);

bool              ncc_cmdr_result_ok(ncc_cmdr_result_t *r);
ncc_parse_tree_t *ncc_cmdr_result_tree(ncc_cmdr_result_t *r);
int32_t           ncc_cmdr_error_count(ncc_cmdr_result_t *r);
ncc_string_t      ncc_cmdr_error_str(ncc_cmdr_result_t *r, int32_t index);

ncc_string_t    ncc_cmdr_result_command(ncc_cmdr_result_t *r);
bool            ncc_cmdr_flag_present(ncc_cmdr_result_t *r, ncc_string_t flag);
ncc_cmdr_val_t *ncc_cmdr_flag_get(ncc_cmdr_result_t *r, ncc_string_t flag);
ncc_string_t    ncc_cmdr_flag_str(ncc_cmdr_result_t *r, ncc_string_t flag);
int64_t         ncc_cmdr_flag_int(ncc_cmdr_result_t *r, ncc_string_t flag);
bool            ncc_cmdr_flag_bool(ncc_cmdr_result_t *r, ncc_string_t flag);
int32_t         ncc_cmdr_arg_count(ncc_cmdr_result_t *r);
ncc_string_t    ncc_cmdr_arg_str(ncc_cmdr_result_t *r, int index);
int64_t     ncc_cmdr_arg_int(ncc_cmdr_result_t *r, int index);

int32_t ncc_cmdr_print_errors(ncc_cmdr_result_t *r, FILE *out);
