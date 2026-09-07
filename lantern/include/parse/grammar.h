#pragma once

#include "parse/types.h"

#include <stdio.h>

parse_grammar_t *parse_grammar_new(void);
void            parse_grammar_free(parse_grammar_t *g);
void            parse_grammar_set_start_id(parse_grammar_t *g, parse_nt_id_t nt_id);

#define parse_grammar_set_start(g, nt) \
    parse_grammar_set_start_id((g), parse_nonterm_id(nt))
void            parse_grammar_set_error_recovery(parse_grammar_t *g, bool enable);
void            parse_grammar_set_max_penalty(parse_grammar_t *g, uint32_t max);
void            parse_grammar_finalize(parse_grammar_t *g);

size_t parse_grammar_undefined(const parse_grammar_t *g, core_string_t *out,
                             size_t max);
bool   parse_grammar_report_undefined(const parse_grammar_t *g, FILE *out);

parse_nonterm_t *parse_nonterm(parse_grammar_t *g, core_string_t name);
parse_nonterm_t *parse_find_nonterm(parse_grammar_t *g, core_string_t name);
int64_t         parse_nonterm_id(parse_nonterm_t *nt);
int64_t         parse_register_terminal(parse_grammar_t *g, core_string_t name);

int64_t parse_lookup_terminal(parse_grammar_t *g, core_string_t name);

core_string_t parse_terminal_name(const parse_grammar_t *g, int64_t tid);

int64_t parse_builtin_terminal_id(core_string_t name);
int64_t parse_terminal_id(parse_grammar_t *g, core_string_t name);

void  parse_nonterm_set_action(parse_nonterm_t *nt, parse_walk_action_t action);
void  parse_nonterm_set_user_data(parse_nonterm_t *nt, void *data);
void *parse_nonterm_get_user_data(parse_nonterm_t *nt);
void  parse_grammar_set_default_action(parse_grammar_t *g, parse_walk_action_t a);
void  parse_grammar_set_terminal_category(parse_grammar_t *g, int64_t tid,
                                          core_string_t category);

#define PARSE_NT(nt)       ((parse_match_t){.kind = PARSE_MATCH_NT, .nt_id = parse_nonterm_id(nt)})
#define PARSE_CHAR(cp)     ((parse_match_t){.kind = PARSE_MATCH_TERMINAL, .terminal_id = (int64_t)(cp)})
#define PARSE_TERMINAL(id) ((parse_match_t){.kind = PARSE_MATCH_TERMINAL, .terminal_id = (id)})
#define PARSE_ANY()        ((parse_match_t){.kind = PARSE_MATCH_ANY})
#define PARSE_SET(s)       ((parse_match_t){.kind = PARSE_MATCH_SET, \
                                        .set_items = (void *)(s)})
#define PARSE_EPSILON()    ((parse_match_t){.kind = PARSE_MATCH_EMPTY})
#define PARSE_CLASS(cc)    ((parse_match_t){.kind = PARSE_MATCH_CLASS, .char_class = (cc)})

parse_match_t parse_group_match_v(parse_grammar_t *g, int min, int max,
                                 int n, parse_match_t *items);

#define parse_optional(g, ...)                                                  \
    parse_group_match_v((g), 0, 1,                                              \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})

#define parse_star(g, ...)                                                      \
    parse_group_match_v((g), 0, 0,                                              \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})

#define parse_plus_group(g, ...)                                                \
    parse_group_match_v((g), 1, 0,                                              \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})

#define parse_repeat(g, min, max, ...)                                          \
    parse_group_match_v((g), (min), (max),                                      \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})

parse_rule_t *parse_add_rule_v(parse_grammar_t *g, parse_nt_id_t nt_id,
                                    int n, parse_match_t *items);
parse_rule_t *parse_add_rule_with_cost_v(parse_grammar_t *g,
                                              parse_nt_id_t nt_id,
                                              int cost,
                                              int n, parse_match_t *items);

#define parse_add_rule(g, nt, ...)                                              \
    parse_add_rule_v((g), parse_nonterm_id(nt),                                  \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})

#define parse_add_rule_with_cost(g, nt, cost, ...)                              \
    parse_add_rule_with_cost_v((g), parse_nonterm_id(nt), (cost),                \
        sizeof((parse_match_t[]){__VA_ARGS__}) / sizeof(parse_match_t),           \
        (parse_match_t[]){__VA_ARGS__})
