// commander.c — Grammar-based command-line parser.
//
// Builds a formal grammar from your CLI spec, tokenizes argv,
// then parses with PWZ. Supports flags, positionals, subcommands,
// --flag=value syntax, short flag aliases, and -- separator.

#include "slay/commander.h"
#include "slay/pwz.h"
#include "parsers/token_stream.h"
#include "internal/slay/grammar_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static char *
cmdr_intern(const char *s)
{
    if (!s) {
        return NULL;
    }

    size_t len = strlen(s);
    char  *out = calloc(1, len + 1);
    memcpy(out, s, len);

    return out;
}

static n00b_cmdr_command_t *
find_command(n00b_cmdr_t *c, const char *name)
{
    if (!name || !*name) {
        return &c->root;
    }

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        if (strcmp(c->root.subcommands[i].name, name) == 0) {
            return &c->root.subcommands[i];
        }
    }

    return NULL;
}

static n00b_cmdr_flag_spec_t *
find_flag(n00b_cmdr_command_t *cmd, const char *flag_name)
{
    if (!cmd || !flag_name) {
        return NULL;
    }

    for (int32_t i = 0; i < cmd->n_flags; i++) {
        if (strcmp(cmd->flags[i].name, flag_name) == 0) {
            return &cmd->flags[i];
        }

        if (cmd->flags[i].short_name
            && strcmp(cmd->flags[i].short_name, flag_name) == 0) {
            return &cmd->flags[i];
        }
    }

    return NULL;
}

// Wrap a C string as n00b_string_t for grammar API calls.
static n00b_string_t
cstr_to_n00b(const char *s)
{
    return (n00b_string_t){
        .data     = (char *)s,
        .u8_bytes = s ? strlen(s) : 0,
    };
}

// ============================================================================
// Lifecycle
// ============================================================================

n00b_cmdr_t *
n00b_cmdr_new(void)
{
    return calloc(1, sizeof(n00b_cmdr_t));
}

static void
free_command(n00b_cmdr_command_t *cmd)
{
    if (!cmd) {
        return;
    }

    for (int32_t i = 0; i < cmd->n_subcommands; i++) {
        free_command(&cmd->subcommands[i]);
    }

    // Free interned strings in flag specs.
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        free((void *)cmd->flags[i].name);
        free((void *)cmd->flags[i].short_name);
        free((void *)cmd->flags[i].doc);
    }

    // Free interned strings in positional specs.
    for (int32_t i = 0; i < cmd->n_positionals; i++) {
        free((void *)cmd->positionals[i].name);
    }

    free((void *)cmd->name);
    free((void *)cmd->doc);
    free(cmd->flags);
    free(cmd->positionals);
    free(cmd->subcommands);
}

void
n00b_cmdr_free(n00b_cmdr_t *c)
{
    if (!c) {
        return;
    }

    free_command(&c->root);

    if (c->grammar) {
        n00b_grammar_free(c->grammar);
    }

    free((void *)c->name);
    free(c);
}

// ============================================================================
// Builder API
// ============================================================================

void
n00b_cmdr_set_name(n00b_cmdr_t *c, const char *name)
{
    if (c) {
        free((void *)c->name);
        c->name = cmdr_intern(name);
    }
}

static void
ensure_flag_capacity(n00b_cmdr_command_t *cmd)
{
    if (cmd->n_flags >= cmd->flags_cap) {
        cmd->flags_cap = cmd->flags_cap ? cmd->flags_cap * 2 : 8;
        cmd->flags     = realloc(cmd->flags,
                                 (size_t)cmd->flags_cap
                                     * sizeof(n00b_cmdr_flag_spec_t));
    }
}

static void
ensure_positional_capacity(n00b_cmdr_command_t *cmd)
{
    if (cmd->n_positionals >= cmd->positionals_cap) {
        cmd->positionals_cap = cmd->positionals_cap
                                   ? cmd->positionals_cap * 2 : 4;
        cmd->positionals = realloc(cmd->positionals,
                                   (size_t)cmd->positionals_cap
                                       * sizeof(n00b_cmdr_positional_spec_t));
    }
}

static void
ensure_subcommand_capacity(n00b_cmdr_command_t *cmd)
{
    if (cmd->n_subcommands >= cmd->subcommands_cap) {
        cmd->subcommands_cap = cmd->subcommands_cap
                                   ? cmd->subcommands_cap * 2 : 4;
        cmd->subcommands = realloc(cmd->subcommands,
                                   (size_t)cmd->subcommands_cap
                                       * sizeof(n00b_cmdr_command_t));
    }
}

void
n00b_cmdr_add_command(n00b_cmdr_t *c, const char *name, const char *doc)
{
    if (!c || !name) {
        return;
    }

    ensure_subcommand_capacity(&c->root);

    n00b_cmdr_command_t *cmd = &c->root.subcommands[c->root.n_subcommands++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
n00b_cmdr_add_subcommand(n00b_cmdr_t *c, const char *parent,
                          const char *name, const char *doc)
{
    if (!c || !name) {
        return;
    }

    n00b_cmdr_command_t *pcmd = find_command(c, parent);

    if (!pcmd) {
        return;
    }

    ensure_subcommand_capacity(pcmd);

    n00b_cmdr_command_t *cmd = &pcmd->subcommands[pcmd->n_subcommands++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
n00b_cmdr_add_flag(n00b_cmdr_t *c, const char *command,
                    const char *flag_name, n00b_cmdr_arg_type_t type,
                    bool takes_value, const char *doc)
{
    if (!c || !flag_name) {
        return;
    }

    n00b_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ensure_flag_capacity(cmd);

    n00b_cmdr_flag_spec_t *f = &cmd->flags[cmd->n_flags++];
    memset(f, 0, sizeof(*f));
    f->name        = cmdr_intern(flag_name);
    f->value_type  = type;
    f->takes_value = takes_value;
    f->doc         = cmdr_intern(doc);
}

void
n00b_cmdr_add_flag_alias(n00b_cmdr_t *c, const char *command,
                          const char *flag_name, const char *alias)
{
    if (!c || !flag_name || !alias) {
        return;
    }

    n00b_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    n00b_cmdr_flag_spec_t *f = find_flag(cmd, flag_name);

    if (!f) {
        return;
    }

    f->short_name = cmdr_intern(alias);
}

void
n00b_cmdr_add_positional(n00b_cmdr_t *c, const char *command,
                          const char *name, n00b_cmdr_arg_type_t type,
                          int min, int max)
{
    if (!c || !name) {
        return;
    }

    n00b_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ensure_positional_capacity(cmd);

    n00b_cmdr_positional_spec_t *p = &cmd->positionals[cmd->n_positionals++];
    p->name = cmdr_intern(name);
    p->type = type;
    p->min  = min;
    p->max  = max;
}

// ============================================================================
// Grammar generation
// ============================================================================

static void
register_command_terminals(n00b_cmdr_t *c, n00b_cmdr_command_t *cmd)
{
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        n00b_cmdr_flag_spec_t *f = &cmd->flags[i];
        f->terminal_id = n00b_register_terminal(c->grammar,
                                                 cstr_to_n00b(f->name));

        if (f->short_name) {
            n00b_register_terminal(c->grammar,
                                    cstr_to_n00b(f->short_name));
        }
    }
}

static n00b_nonterm_t *
build_flag_nt(n00b_cmdr_t *c, n00b_cmdr_command_t *cmd __attribute__((unused)),
              n00b_cmdr_flag_spec_t *f, const char *prefix)
{
    char nt_name[256];
    snprintf(nt_name, sizeof(nt_name), "%s-flag-%s", prefix, f->name);

    n00b_nonterm_t *nt = n00b_nonterm(c->grammar, cstr_to_n00b(nt_name));

    int64_t flag_tid = f->terminal_id;
    int64_t eq_tid   = c->tok_ids[N00B_CMDR_TID_EQ];
    int64_t word_tid = c->tok_ids[N00B_CMDR_TID_WORD];
    int64_t int_tid  = c->tok_ids[N00B_CMDR_TID_INT];
    int64_t flt_tid  = c->tok_ids[N00B_CMDR_TID_FLOAT];
    int64_t bool_tid = c->tok_ids[N00B_CMDR_TID_BOOL];

    if (f->takes_value) {
        // --flag=value
        n00b_add_rule(c->grammar, nt,
                       N00B_TERMINAL(flag_tid),
                       N00B_TERMINAL(eq_tid),
                       N00B_TERMINAL(word_tid));

        if (f->short_name) {
            int64_t short_tid = n00b_register_terminal(
                c->grammar, cstr_to_n00b(f->short_name));
            n00b_add_rule(c->grammar, nt,
                           N00B_TERMINAL(short_tid),
                           N00B_TERMINAL(eq_tid),
                           N00B_TERMINAL(word_tid));
            n00b_add_rule(c->grammar, nt,
                           N00B_TERMINAL(short_tid),
                           N00B_TERMINAL(word_tid));
        }

        // --flag value
        n00b_add_rule(c->grammar, nt,
                       N00B_TERMINAL(flag_tid),
                       N00B_TERMINAL(word_tid));

        // Also accept int/float/bool tokens as values.
        n00b_add_rule(c->grammar, nt,
                       N00B_TERMINAL(flag_tid), N00B_TERMINAL(int_tid));
        n00b_add_rule(c->grammar, nt,
                       N00B_TERMINAL(flag_tid), N00B_TERMINAL(flt_tid));
        n00b_add_rule(c->grammar, nt,
                       N00B_TERMINAL(flag_tid), N00B_TERMINAL(bool_tid));
    }
    else {
        // Boolean flag (no value).
        n00b_add_rule(c->grammar, nt, N00B_TERMINAL(flag_tid));

        if (f->short_name) {
            int64_t short_tid = n00b_register_terminal(
                c->grammar, cstr_to_n00b(f->short_name));
            n00b_add_rule(c->grammar, nt, N00B_TERMINAL(short_tid));
        }
    }

    return nt;
}

static n00b_nonterm_t *
build_items_nt(n00b_cmdr_t *c, n00b_cmdr_command_t *cmd, const char *prefix)
{
    char nt_name[256];
    snprintf(nt_name, sizeof(nt_name), "%s-items", prefix);

    n00b_nonterm_t *items = n00b_nonterm(c->grammar, cstr_to_n00b(nt_name));

    // items -> ""
    n00b_add_rule(c->grammar, items, N00B_EPSILON());

    // items -> flag items  (for each flag)
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        n00b_nonterm_t *fnt = build_flag_nt(c, cmd, &cmd->flags[i], prefix);
        n00b_add_rule(c->grammar, items,
                       N00B_NT(fnt), N00B_NT(items));
    }

    // items -> WORD items | INT items | FLOAT items | BOOL items
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_WORD]),
                   N00B_NT(items));
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_INT]),
                   N00B_NT(items));
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_FLOAT]),
                   N00B_NT(items));
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_BOOL]),
                   N00B_NT(items));

    // items -> FLAG items  (unknown flags become positional args)
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_FLAG]),
                   N00B_NT(items));

    // items -> DD items  (-- separator)
    n00b_add_rule(c->grammar, items,
                   N00B_TERMINAL(c->tok_ids[N00B_CMDR_TID_DD]),
                   N00B_NT(items));

    return items;
}

static void
build_command_grammar(n00b_cmdr_t *c, n00b_cmdr_command_t *cmd,
                      n00b_nonterm_t *parent_nt, const char *prefix)
{
    n00b_nonterm_t *items = build_items_nt(c, cmd, prefix);

    if (cmd->name) {
        int64_t name_tid = n00b_register_terminal(
            c->grammar, cstr_to_n00b(cmd->name));
        n00b_add_rule(c->grammar, parent_nt,
                       N00B_TERMINAL(name_tid), N00B_NT(items));
    }
    else {
        n00b_add_rule(c->grammar, parent_nt, N00B_NT(items));
    }

    for (int32_t i = 0; i < cmd->n_subcommands; i++) {
        n00b_cmdr_command_t *sub = &cmd->subcommands[i];
        char sub_prefix[256];
        snprintf(sub_prefix, sizeof(sub_prefix), "%s-%s",
                 prefix, sub->name);
        build_command_grammar(c, sub, parent_nt, sub_prefix);
    }
}

void
n00b_cmdr_finalize(n00b_cmdr_t *c)
{
    if (!c || c->finalized) {
        return;
    }

    c->grammar = n00b_grammar_new();
    n00b_grammar_set_error_recovery(c->grammar, false);

    // Register base token types.
    c->tok_ids[N00B_CMDR_TID_WORD]  = n00b_register_terminal(
        c->grammar, cstr_to_n00b("WORD"));
    c->tok_ids[N00B_CMDR_TID_INT]   = n00b_register_terminal(
        c->grammar, cstr_to_n00b("INT"));
    c->tok_ids[N00B_CMDR_TID_FLOAT] = n00b_register_terminal(
        c->grammar, cstr_to_n00b("FLOAT"));
    c->tok_ids[N00B_CMDR_TID_BOOL]  = n00b_register_terminal(
        c->grammar, cstr_to_n00b("BOOL"));
    c->tok_ids[N00B_CMDR_TID_EQ]    = n00b_register_terminal(
        c->grammar, cstr_to_n00b("EQ"));
    c->tok_ids[N00B_CMDR_TID_COMMA] = n00b_register_terminal(
        c->grammar, cstr_to_n00b("COMMA"));
    c->tok_ids[N00B_CMDR_TID_DD]    = n00b_register_terminal(
        c->grammar, cstr_to_n00b("DD"));
    c->tok_ids[N00B_CMDR_TID_FLAG]  = n00b_register_terminal(
        c->grammar, cstr_to_n00b("FLAG"));

    // Register all flag and subcommand terminals.
    register_command_terminals(c, &c->root);

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        register_command_terminals(c, &c->root.subcommands[i]);
        n00b_register_terminal(c->grammar,
                                cstr_to_n00b(c->root.subcommands[i].name));
    }

    // Build grammar.
    n00b_nonterm_t *start = n00b_nonterm(c->grammar, cstr_to_n00b("cmd"));
    n00b_grammar_set_start(c->grammar, start);

    if (c->root.n_subcommands > 0) {
        for (int32_t i = 0; i < c->root.n_subcommands; i++) {
            n00b_cmdr_command_t *sub = &c->root.subcommands[i];
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "cmd-%s", sub->name);
            build_command_grammar(c, sub, start, prefix);
        }

        if (c->root.n_flags > 0) {
            build_command_grammar(c, &c->root, start, "cmd-root");
        }
    }
    else {
        build_command_grammar(c, &c->root, start, "cmd");
    }

    n00b_grammar_finalize(c->grammar);
    c->finalized = true;
}

// ============================================================================
// Tokenizer
// ============================================================================

static bool
is_int_str(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    const char *p = s;

    if (*p == '-' || *p == '+') {
        p++;
    }

    if (!*p) {
        return false;
    }

    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        p++;
    }

    return true;
}

static bool
is_float_str(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    char *end = NULL;
    strtod(s, &end);

    return end && *end == '\0' && end != s
           && (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E'));
}

static bool
is_bool_str(const char *s)
{
    if (!s) {
        return false;
    }

    return strcmp(s, "true") == 0 || strcmp(s, "false") == 0
        || strcmp(s, "yes") == 0 || strcmp(s, "no") == 0;
}

static int64_t
find_flag_tid(n00b_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return 0;
    }

    n00b_cmdr_command_t *root = &c->root;

    for (int32_t i = 0; i < root->n_flags; i++) {
        if (strcmp(root->flags[i].name, name) == 0) {
            return root->flags[i].terminal_id;
        }

        if (root->flags[i].short_name
            && strcmp(root->flags[i].short_name, name) == 0) {
            return root->flags[i].terminal_id;
        }
    }

    for (int32_t si = 0; si < root->n_subcommands; si++) {
        n00b_cmdr_command_t *sub = &root->subcommands[si];

        for (int32_t i = 0; i < sub->n_flags; i++) {
            if (strcmp(sub->flags[i].name, name) == 0) {
                return sub->flags[i].terminal_id;
            }

            if (sub->flags[i].short_name
                && strcmp(sub->flags[i].short_name, name) == 0) {
                return sub->flags[i].terminal_id;
            }
        }
    }

    return 0;
}

static bool
flag_takes_value(n00b_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return false;
    }

    n00b_cmdr_command_t *root = &c->root;

    for (int32_t i = 0; i < root->n_flags; i++) {
        if (strcmp(root->flags[i].name, name) == 0) {
            return root->flags[i].takes_value;
        }

        if (root->flags[i].short_name
            && strcmp(root->flags[i].short_name, name) == 0) {
            return root->flags[i].takes_value;
        }
    }

    for (int32_t si = 0; si < root->n_subcommands; si++) {
        n00b_cmdr_command_t *sub = &root->subcommands[si];

        for (int32_t i = 0; i < sub->n_flags; i++) {
            if (strcmp(sub->flags[i].name, name) == 0) {
                return sub->flags[i].takes_value;
            }

            if (sub->flags[i].short_name
                && strcmp(sub->flags[i].short_name, name) == 0) {
                return sub->flags[i].takes_value;
            }
        }
    }

    return false;
}

static int64_t
classify_word(n00b_cmdr_t *c, const char *s)
{
    if (is_int_str(s)) {
        return c->tok_ids[N00B_CMDR_TID_INT];
    }

    if (is_float_str(s)) {
        return c->tok_ids[N00B_CMDR_TID_FLOAT];
    }

    if (is_bool_str(s)) {
        return c->tok_ids[N00B_CMDR_TID_BOOL];
    }

    return c->tok_ids[N00B_CMDR_TID_WORD];
}

typedef struct {
    n00b_token_info_t **tokens;
    int32_t             len;
    int32_t             cap;
} tok_list_t;

static void
tok_list_init(tok_list_t *tl)
{
    tl->cap    = 32;
    tl->len    = 0;
    tl->tokens = calloc((size_t)tl->cap, sizeof(n00b_token_info_t *));
}

static void
tok_list_push(tok_list_t *tl, n00b_token_info_t *tok)
{
    if (tl->len >= tl->cap) {
        tl->cap *= 2;
        tl->tokens = realloc(tl->tokens,
                              (size_t)tl->cap * sizeof(n00b_token_info_t *));
    }

    tl->tokens[tl->len++] = tok;
}

static n00b_token_info_t *
make_token(const char *value, int64_t tid, int32_t index)
{
    n00b_token_info_t *tok = calloc(1, sizeof(n00b_token_info_t));

    if (value && *value) {
        n00b_string_t s = {
            .data     = cmdr_intern(value),
            .u8_bytes = strlen(value),
        };
        tok->value = n00b_option_set(n00b_string_t, s);
    }

    tok->tid    = (int32_t)tid;
    tok->index  = index;
    tok->line   = 1;
    tok->column = 1;

    return tok;
}

static int32_t
cmdr_tokenize(const char **argv, int argc, n00b_cmdr_t *c,
              n00b_token_info_t ***tokens_out, int32_t *n_tokens_out)
{
    if (!argv || argc <= 0 || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    tok_list_t tl;
    tok_list_init(&tl);

    bool past_dd = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg) {
            continue;
        }

        // -- means end of flags.
        if (!past_dd && strcmp(arg, "--") == 0) {
            past_dd = true;
            tok_list_push(&tl, make_token("--",
                                           c->tok_ids[N00B_CMDR_TID_DD],
                                           tl.len));
            continue;
        }

        // After --, everything is a word.
        if (past_dd) {
            tok_list_push(&tl, make_token(arg, classify_word(c, arg),
                                           tl.len));
            continue;
        }

        // --flag=value
        if (strncmp(arg, "--", 2) == 0 && strlen(arg) > 2) {
            const char *eq = strchr(arg, '=');

            if (eq) {
                size_t flag_len = (size_t)(eq - arg);
                char *flag_name = calloc(1, flag_len + 1);
                memcpy(flag_name, arg, flag_len);

                int64_t ftid = find_flag_tid(c, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[N00B_CMDR_TID_FLAG];
                }

                tok_list_push(&tl, make_token(flag_name, ftid, tl.len));
                free(flag_name);

                tok_list_push(&tl, make_token("=",
                                               c->tok_ids[N00B_CMDR_TID_EQ],
                                               tl.len));

                const char *val = eq + 1;
                tok_list_push(&tl, make_token(val, classify_word(c, val),
                                               tl.len));
            }
            else {
                int64_t ftid = find_flag_tid(c, arg);

                if (!ftid) {
                    ftid = c->tok_ids[N00B_CMDR_TID_FLAG];
                }

                tok_list_push(&tl, make_token(arg, ftid, tl.len));
            }

            continue;
        }

        // -x short flag
        if (arg[0] == '-' && arg[1] != '\0'
            && !isdigit((unsigned char)arg[1])) {
            int64_t ftid = find_flag_tid(c, arg);

            if (ftid) {
                tok_list_push(&tl, make_token(arg, ftid, tl.len));
                continue;
            }

            // Try splitting combined short flags (-abc -> -a -b -c).
            bool can_split = false;

            if (strlen(arg) > 2) {
                can_split = true;

                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, short_flag);

                    if (!stid || flag_takes_value(c, short_flag)) {
                        can_split = false;
                        break;
                    }
                }
            }

            if (can_split) {
                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, short_flag);
                    tok_list_push(&tl, make_token(short_flag, stid, tl.len));
                }
            }
            else {
                tok_list_push(&tl, make_token(arg,
                                               c->tok_ids[N00B_CMDR_TID_FLAG],
                                               tl.len));
            }

            continue;
        }

        // Bare word (may be a subcommand name).
        int64_t tid = classify_word(c, arg);

        for (int32_t si = 0; si < c->root.n_subcommands; si++) {
            if (strcmp(arg, c->root.subcommands[si].name) == 0) {
                tid = n00b_register_terminal(c->grammar,
                                              cstr_to_n00b(arg));
                break;
            }
        }

        tok_list_push(&tl, make_token(arg, tid, tl.len));
    }

    // Add EOF.
    tok_list_push(&tl, make_token("", N00B_TOK_EOF, tl.len));

    *tokens_out   = tl.tokens;
    *n_tokens_out = tl.len;

    return 0;
}

// ============================================================================
// Parsing
// ============================================================================

static n00b_cmdr_result_t *
make_error_result(const char *msg)
{
    n00b_cmdr_result_t *r = calloc(1, sizeof(n00b_cmdr_result_t));
    r->ok       = false;
    r->errors   = calloc(1, sizeof(const char *));
    r->errors[0] = cmdr_intern(msg);
    r->n_errors = 1;

    return r;
}

// Get token text as a C string from a parse tree node.
static const char *
get_token_text(n00b_parse_tree_t *node)
{
    if (!node || !n00b_tree_is_leaf(node)) {
        return NULL;
    }

    n00b_token_info_t *tok = n00b_tree_leaf_value(node);

    if (!tok || !n00b_option_is_set(tok->value)) {
        return NULL;
    }

    n00b_string_t val = n00b_option_get(tok->value);
    return val.data;
}

// Recursively collect all terminal text from a parse tree.
static void
collect_terminal_text(n00b_parse_tree_t *tree, const char ***texts,
                      int32_t *n, int32_t *cap)
{
    if (!tree) {
        return;
    }

    if (n00b_tree_is_leaf(tree)) {
        const char *text = get_token_text(tree);

        if (text && *text) {
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *texts = realloc(*texts, (size_t)*cap * sizeof(const char *));
            }

            (*texts)[(*n)++] = text;
        }

        return;
    }

    size_t nc = n00b_tree_num_children(tree);

    for (size_t i = 0; i < nc; i++) {
        collect_terminal_text(n00b_tree_child(tree, i), texts, n, cap);
    }
}

static void
extract_result_from_tree(n00b_cmdr_t *c, n00b_parse_tree_t *tree,
                          n00b_cmdr_result_t *r)
{
    if (!tree || !r) {
        return;
    }

    const char **texts = NULL;
    int32_t n   = 0;
    int32_t cap = 0;

    collect_terminal_text(tree, &texts, &n, &cap);

    int32_t args_cap = 0;
    bool    past_dd  = false;

    for (int32_t i = 0; i < n; i++) {
        const char *text = texts[i];

        if (!text || !*text) {
            continue;
        }

        if (strcmp(text, "--") == 0) {
            past_dd = true;
            goto next;
        }

        if (!r->command && !past_dd) {
            for (int32_t si = 0; si < c->root.n_subcommands; si++) {
                if (strcmp(text, c->root.subcommands[si].name) == 0) {
                    r->command = cmdr_intern(text);
                    goto next;
                }
            }
        }

        if (past_dd) {
            goto add_arg;
        }

        // Check if this is a flag.
        {
            n00b_cmdr_command_t *cmd = r->command
                ? find_command(c, r->command) : &c->root;

            if (!cmd) {
                cmd = &c->root;
            }

            n00b_cmdr_flag_spec_t *flag = find_flag(cmd, text);

            if (!flag) {
                flag = find_flag(&c->root, text);
            }

            if (flag) {
                n00b_cmdr_val_t *v = calloc(1, sizeof(n00b_cmdr_val_t));

                if (flag->takes_value && i + 1 < n) {
                    if (i + 1 < n && strcmp(texts[i + 1], "=") == 0) {
                        i++;
                    }

                    if (i + 1 < n) {
                        i++;
                        const char *val = texts[i];

                        switch (flag->value_type) {
                        case N00B_CMDR_TYPE_INT:
                            v->tag = N00B_CMDR_VAL_INT;
                            v->i   = strtoll(val, NULL, 10);
                            break;
                        case N00B_CMDR_TYPE_FLOAT:
                            v->tag = N00B_CMDR_VAL_FLOAT;
                            v->f   = strtod(val, NULL);
                            break;
                        case N00B_CMDR_TYPE_BOOL:
                            v->tag = N00B_CMDR_VAL_BOOL;
                            v->b   = (strcmp(val, "true") == 0
                                      || strcmp(val, "yes") == 0);
                            break;
                        default:
                            v->tag = N00B_CMDR_VAL_STR;
                            v->s   = cmdr_intern(val);
                            break;
                        }
                    }
                    else {
                        free(v);
                        goto next;
                    }
                }
                else {
                    v->tag = N00B_CMDR_VAL_BOOL;
                    v->b   = true;
                }

                // Store under long flag name.
                _n00b_dict_put(&r->flags,
                                       (void *)cmdr_intern(flag->name),
                                       (void *)v);

                // Also store under alias.
                if (flag->short_name) {
                    n00b_cmdr_val_t *alias_v = calloc(1, sizeof(n00b_cmdr_val_t));
                    *alias_v = *v;
                    if (alias_v->tag == N00B_CMDR_VAL_STR && alias_v->s) {
                        alias_v->s = cmdr_intern(alias_v->s);
                    }
                    _n00b_dict_put(
                        &r->flags,
                        (void *)cmdr_intern(flag->short_name),
                        (void *)alias_v);
                }

                goto next;
            }
        }

        if (strcmp(text, "=") == 0) {
            goto next;
        }

    add_arg:
        if (r->n_args >= args_cap) {
            args_cap = args_cap ? args_cap * 2 : 8;
            r->args = realloc(r->args,
                               (size_t)args_cap * sizeof(n00b_cmdr_arg_t));
        }

        {
            n00b_cmdr_arg_t *arg = &r->args[r->n_args++];
            arg->value     = cmdr_intern(text);
            arg->int_val   = strtoll(text, NULL, 10);
            arg->float_val = strtod(text, NULL);
        }

    next:;
    }

    free(texts);
}

static void
free_tokens(n00b_token_info_t **tokens, int32_t n_tokens)
{
    if (!tokens) {
        return;
    }

    for (int32_t i = 0; i < n_tokens; i++) {
        if (tokens[i]) {
            if (n00b_option_is_set(tokens[i]->value)) {
                n00b_string_t val = n00b_option_get(tokens[i]->value);
                free(val.data);
            }
            free(tokens[i]);
        }
    }

    free(tokens);
}

n00b_cmdr_result_t *
n00b_cmdr_parse(n00b_cmdr_t *c, int argc, const char **argv)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        n00b_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    n00b_token_info_t **tokens = NULL;
    int32_t             n_tokens = 0;

    if (cmdr_tokenize(argv, argc, c, &tokens, &n_tokens) < 0) {
        return make_error_result("tokenization failed");
    }

    // Parse using PWZ with a token stream from the array.
    n00b_token_stream_t *ts = n00b_token_stream_from_array(tokens, n_tokens);
    n00b_pwz_parser_t   *pp = n00b_pwz_new(c->grammar);
    bool parse_ok = n00b_pwz_parse(pp, ts);

    n00b_cmdr_result_t *r = calloc(1, sizeof(n00b_cmdr_result_t));
    n00b_dict_init(&r->flags, n00b_hash_cstring, n00b_dict_cstr_eq);

    if (!parse_ok) {
        r->ok       = false;
        r->errors   = calloc(1, sizeof(const char *));
        r->errors[0] = cmdr_intern("parse failed");
        r->n_errors = 1;
    }
    else {
        r->ok   = true;
        r->tree = n00b_pwz_get_tree(pp);

        if (r->tree) {
            extract_result_from_tree(c, r->tree, r);
        }
    }

    n00b_pwz_free(pp);
    n00b_token_stream_free(ts);
    free_tokens(tokens, n_tokens);

    return r;
}

n00b_cmdr_result_t *
n00b_cmdr_parse_string(n00b_cmdr_t *c, const char *cmdline)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        n00b_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    // Split cmdline into argv.
    const char **argv = NULL;
    int          argc = 0;
    int          cap  = 16;
    argv = calloc((size_t)cap, sizeof(const char *));

    const char *p = cmdline;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        if (!*p) {
            break;
        }

        const char *start = p;
        bool in_quote      = false;
        char quote_char    = 0;

        while (*p) {
            if (in_quote) {
                if (*p == quote_char) {
                    in_quote = false;
                }
                p++;
            }
            else if (*p == '"' || *p == '\'') {
                in_quote   = true;
                quote_char = *p;
                p++;
            }
            else if (isspace((unsigned char)*p)) {
                break;
            }
            else {
                p++;
            }
        }

        size_t len = (size_t)(p - start);

        if (len >= 2
            && ((start[0] == '"' && start[len - 1] == '"')
                || (start[0] == '\'' && start[len - 1] == '\''))) {
            start++;
            len -= 2;
        }

        char *word = calloc(1, len + 1);
        memcpy(word, start, len);

        if (argc >= cap) {
            cap *= 2;
            argv = realloc(argv, (size_t)cap * sizeof(const char *));
        }

        argv[argc++] = word;
    }

    n00b_cmdr_result_t *result = n00b_cmdr_parse(c, argc, argv);

    for (int i = 0; i < argc; i++) {
        free((void *)argv[i]);
    }
    free(argv);

    return result;
}

// ============================================================================
// Result queries
// ============================================================================

const char *
n00b_cmdr_result_command(n00b_cmdr_result_t *r)
{
    return r ? r->command : NULL;
}

bool
n00b_cmdr_flag_present(n00b_cmdr_result_t *r, const char *flag)
{
    if (!r || !flag) {
        return false;
    }

    return n00b_dict_contains(&r->flags, (void *)flag);
}

n00b_cmdr_val_t *
n00b_cmdr_flag_get(n00b_cmdr_result_t *r, const char *flag)
{
    if (!r || !flag) {
        return NULL;
    }

    bool found;
    void *val = n00b_dict_get(&r->flags, (void *)flag, &found);

    return found ? (n00b_cmdr_val_t *)val : NULL;
}

const char *
n00b_cmdr_flag_str(n00b_cmdr_result_t *r, const char *flag)
{
    n00b_cmdr_val_t *v = n00b_cmdr_flag_get(r, flag);

    if (!v || v->tag != N00B_CMDR_VAL_STR) {
        return NULL;
    }

    return v->s;
}

int64_t
n00b_cmdr_flag_int(n00b_cmdr_result_t *r, const char *flag)
{
    n00b_cmdr_val_t *v = n00b_cmdr_flag_get(r, flag);

    if (!v || v->tag != N00B_CMDR_VAL_INT) {
        return 0;
    }

    return v->i;
}

bool
n00b_cmdr_flag_bool(n00b_cmdr_result_t *r, const char *flag)
{
    n00b_cmdr_val_t *v = n00b_cmdr_flag_get(r, flag);

    if (!v || v->tag != N00B_CMDR_VAL_BOOL) {
        return false;
    }

    return v->b;
}

int32_t
n00b_cmdr_arg_count(n00b_cmdr_result_t *r)
{
    return r ? r->n_args : 0;
}

const char *
n00b_cmdr_arg_str(n00b_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= r->n_args) {
        return NULL;
    }

    return r->args[index].value;
}

int64_t
n00b_cmdr_arg_int(n00b_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= r->n_args) {
        return 0;
    }

    return r->args[index].int_val;
}

// ============================================================================
// Result cleanup
// ============================================================================

void
n00b_cmdr_result_free(n00b_cmdr_result_t *r)
{
    if (!r) {
        return;
    }

    free((void *)r->command);

    // Free interned keys and value structs in the flags dict.
    for (size_t i = 0; i < r->flags.capacity; i++) {
        if (r->flags.buckets[i].state != _N00B_BUCKET_OCCUPIED) {
            continue;
        }
        free(r->flags.buckets[i].key);
        n00b_cmdr_val_t *v = (n00b_cmdr_val_t *)r->flags.buckets[i].value;
        if (v) {
            if (v->tag == N00B_CMDR_VAL_STR) {
                free((void *)v->s);
            }
            free(v);
        }
    }

    n00b_dict_free(&r->flags);

    for (int32_t i = 0; i < r->n_args; i++) {
        free((void *)r->args[i].value);
    }

    free(r->args);

    for (int32_t i = 0; i < r->n_errors; i++) {
        free((void *)r->errors[i]);
    }

    free(r->errors);

    // Parse tree nodes are arena-allocated (n00b_alloc); we just
    // clear the pointer to avoid stale access.
    r->tree = NULL;

    free(r);
}

// ============================================================================
// Error output
// ============================================================================

int32_t
n00b_cmdr_print_errors(n00b_cmdr_result_t *r, FILE *out)
{
    if (!r || r->ok || !r->errors) {
        return 0;
    }

    for (int32_t i = 0; i < r->n_errors; i++) {
        fprintf(out, "error: %s\n", r->errors[i]);
    }

    return r->n_errors;
}
