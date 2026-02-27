#pragma once

/**
 * @file commander.h
 * @brief Grammar-based command-line parser.
 *
 * Commander builds a formal grammar from your command-line specification,
 * then parses argv using PWZ. This gives you unambiguous, predictable
 * parsing with proper error reporting.
 *
 * Usage:
 *   n00b_cmdr_t *c = n00b_cmdr_new();
 *   n00b_cmdr_add_flag(c, NULL, "--verbose", N00B_CMDR_TYPE_BOOL, false, NULL);
 *   n00b_cmdr_add_positional(c, NULL, "file", N00B_CMDR_TYPE_WORD, 1, -1);
 *   n00b_cmdr_result_t *r = n00b_cmdr_parse(c, argc - 1, argv + 1);
 *   if (r->ok) { ... }
 *   n00b_cmdr_result_free(r);
 *   n00b_cmdr_free(c);
 */

#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include "core/dict_untyped.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Token type indices
// ============================================================================

enum {
    N00B_CMDR_TID_WORD  = 0,
    N00B_CMDR_TID_INT   = 1,
    N00B_CMDR_TID_FLOAT = 2,
    N00B_CMDR_TID_BOOL  = 3,
    N00B_CMDR_TID_EQ    = 4,
    N00B_CMDR_TID_COMMA = 5,
    N00B_CMDR_TID_DD    = 6,
    N00B_CMDR_TID_FLAG  = 7,
    N00B_CMDR_TID_COUNT = 8,
};

// ============================================================================
// Types
// ============================================================================

typedef enum {
    N00B_CMDR_TYPE_BOOL,
    N00B_CMDR_TYPE_WORD,
    N00B_CMDR_TYPE_INT,
    N00B_CMDR_TYPE_FLOAT,
} n00b_cmdr_arg_type_t;

typedef enum {
    N00B_CMDR_VAL_NONE,
    N00B_CMDR_VAL_BOOL,
    N00B_CMDR_VAL_INT,
    N00B_CMDR_VAL_FLOAT,
    N00B_CMDR_VAL_STR,
} n00b_cmdr_val_tag_t;

typedef struct {
    n00b_cmdr_val_tag_t tag;
    union {
        bool        b;
        int64_t     i;
        double      f;
        const char *s;
    };
} n00b_cmdr_val_t;

typedef struct {
    const char *value;
    int64_t     int_val;
    double      float_val;
} n00b_cmdr_arg_t;

typedef struct {
    const char          *name;
    const char          *short_name;
    n00b_cmdr_arg_type_t value_type;
    bool                 takes_value;
    const char          *doc;
    int64_t              terminal_id;
} n00b_cmdr_flag_spec_t;

typedef struct {
    const char          *name;
    n00b_cmdr_arg_type_t type;
    int                  min;
    int                  max;
} n00b_cmdr_positional_spec_t;

typedef struct n00b_cmdr_command {
    const char                  *name;
    const char                  *doc;
    n00b_cmdr_flag_spec_t       *flags;
    int32_t                      n_flags;
    int32_t                      flags_cap;
    n00b_cmdr_positional_spec_t *positionals;
    int32_t                      n_positionals;
    int32_t                      positionals_cap;
    struct n00b_cmdr_command    *subcommands;
    int32_t                      n_subcommands;
    int32_t                      subcommands_cap;
} n00b_cmdr_command_t;

typedef struct {
    n00b_grammar_t      *grammar;
    n00b_cmdr_command_t  root;
    int64_t              next_flag_id;
    bool                 finalized;
    const char          *name;
    int64_t              tok_ids[N00B_CMDR_TID_COUNT];
} n00b_cmdr_t;

typedef struct {
    const char          *command;
    n00b_dict_untyped_t  flags;
    n00b_cmdr_arg_t     *args;
    int32_t              n_args;
    const char         **errors;
    int32_t              n_errors;
    bool                 ok;
    n00b_parse_tree_t   *tree;
} n00b_cmdr_result_t;

// ============================================================================
// Lifecycle
// ============================================================================

n00b_cmdr_t *n00b_cmdr_new(void);
void         n00b_cmdr_free(n00b_cmdr_t *c);

// ============================================================================
// Builder API
// ============================================================================

void n00b_cmdr_set_name(n00b_cmdr_t *c, const char *name);
void n00b_cmdr_add_command(n00b_cmdr_t *c, const char *name, const char *doc);
void n00b_cmdr_add_subcommand(n00b_cmdr_t *c, const char *parent,
                               const char *name, const char *doc);
void n00b_cmdr_add_flag(n00b_cmdr_t *c, const char *command,
                          const char *flag_name, n00b_cmdr_arg_type_t type,
                          bool takes_value, const char *doc);
void n00b_cmdr_add_flag_alias(n00b_cmdr_t *c, const char *command,
                                const char *flag_name, const char *alias);
void n00b_cmdr_add_positional(n00b_cmdr_t *c, const char *command,
                                const char *name, n00b_cmdr_arg_type_t type,
                                int min, int max);
void n00b_cmdr_finalize(n00b_cmdr_t *c);

// ============================================================================
// Parsing
// ============================================================================

n00b_cmdr_result_t *n00b_cmdr_parse(n00b_cmdr_t *c, int argc,
                                      const char **argv);
n00b_cmdr_result_t *n00b_cmdr_parse_string(n00b_cmdr_t *c,
                                              const char *cmdline);
void                n00b_cmdr_result_free(n00b_cmdr_result_t *r);

// ============================================================================
// Result queries
// ============================================================================

const char *n00b_cmdr_result_command(n00b_cmdr_result_t *r);
bool        n00b_cmdr_flag_present(n00b_cmdr_result_t *r, const char *flag);
n00b_cmdr_val_t *n00b_cmdr_flag_get(n00b_cmdr_result_t *r, const char *flag);
const char *n00b_cmdr_flag_str(n00b_cmdr_result_t *r, const char *flag);
int64_t     n00b_cmdr_flag_int(n00b_cmdr_result_t *r, const char *flag);
bool        n00b_cmdr_flag_bool(n00b_cmdr_result_t *r, const char *flag);
int32_t     n00b_cmdr_arg_count(n00b_cmdr_result_t *r);
const char *n00b_cmdr_arg_str(n00b_cmdr_result_t *r, int index);
int64_t     n00b_cmdr_arg_int(n00b_cmdr_result_t *r, int index);

// ============================================================================
// Error output
// ============================================================================

int32_t n00b_cmdr_print_errors(n00b_cmdr_result_t *r, FILE *out);
