
#include "parse/commander.h"
#include "parse/pwz.h"
#include "parse/tree_visit.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ncc_string_t value;
    int64_t      int_val;
    double       float_val;
} ncc_cmdr_arg_t;

typedef struct {
    ncc_string_t        name;
    ncc_string_t        short_name;
    ncc_cmdr_arg_type_t value_type;
    bool                takes_value;
    ncc_string_t        doc;
    int64_t             terminal_id;
} ncc_cmdr_flag_spec_t;

typedef struct {
    ncc_string_t        name;
    ncc_cmdr_arg_type_t type;
    int                 min;
    int                 max;
} ncc_cmdr_positional_spec_t;

typedef struct ncc_cmdr_command ncc_cmdr_command_t;

ncc_list_decl(ncc_cmdr_arg_t);
ncc_list_decl(ncc_cmdr_flag_spec_t);
ncc_list_decl(ncc_cmdr_positional_spec_t);
ncc_list_decl(ncc_cmdr_command_t);

struct ncc_cmdr_command {
    ncc_string_t                              name;
    ncc_string_t                              doc;
    ncc_list_t(ncc_cmdr_flag_spec_t)          flags;
    ncc_list_t(ncc_cmdr_positional_spec_t)    positionals;
    ncc_list_t(ncc_cmdr_command_t)            subcommands;
};

struct ncc_cmdr_t {
    ncc_grammar_t     *grammar;
    ncc_cmdr_command_t root;
    int64_t            next_flag_id;
    bool               finalized;
    ncc_string_t       name;
    int64_t            tok_ids[NCC_CMDR_TID_COUNT];
};

struct ncc_cmdr_result_t {
    ncc_string_t                 command;
    ncc_dict_t                   flags;
    ncc_list_t(ncc_cmdr_arg_t)   args;
    ncc_list_t(ncc_string_t)     errors;
    bool                         ok;
    ncc_parse_tree_t            *tree;
};

static ncc_string_t
argv_str(const char *s)
{
    return ncc_string_borrow(s, s ? strlen(s) : 0);
}

static ncc_string_t
cmdr_intern(ncc_string_t s)
{
    if (!s.data) {
        return ncc_string_empty();
    }

    return ncc_string_from_raw(s.data, (int64_t)s.u8_bytes);
}

static ncc_cmdr_command_t *
find_command(ncc_cmdr_t *c, ncc_string_t name)
{
    if (!name.data || !name.u8_bytes) {
        return &c->root;
    }

    for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
        if (ncc_string_eq(c->root.subcommands.data[i].name, name)) {
            return &c->root.subcommands.data[i];
        }
    }

    return nullptr;
}

static ncc_cmdr_flag_spec_t *
find_flag(ncc_cmdr_command_t *cmd, ncc_string_t flag_name)
{
    if (!cmd || !flag_name.data) {
        return nullptr;
    }

    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        if (ncc_string_eq(cmd->flags.data[i].name, flag_name)
            || ncc_string_eq(cmd->flags.data[i].short_name, flag_name)) {
            return &cmd->flags.data[i];
        }
    }

    return nullptr;
}

static ncc_hash_value_t
flag_key_hash(void *k)
{
    return ncc_string_hash(*(ncc_string_t *)k);
}

static bool
flag_key_eq(const void *a, const void *b)
{
    return ncc_string_eq(*(const ncc_string_t *)a, *(const ncc_string_t *)b);
}

static ncc_string_t *
flag_key(ncc_string_t s)
{
    ncc_string_t *k = ncc_alloc(ncc_string_t);

    *k = ncc_string_from_raw(s.data, (int64_t)s.u8_bytes);

    return k;
}

ncc_cmdr_t *
ncc_cmdr_new(void)
{
    return ncc_alloc(ncc_cmdr_t);
}

static void
free_command(ncc_cmdr_command_t *cmd)
{
    if (!cmd) {
        return;
    }

    for (int32_t i = 0; i < (int32_t)cmd->subcommands.len; i++) {
        free_command(&cmd->subcommands.data[i]);
    }

    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        ncc_free(cmd->flags.data[i].name.data);
        ncc_free(cmd->flags.data[i].short_name.data);
        ncc_free(cmd->flags.data[i].doc.data);
    }

    for (int32_t i = 0; i < (int32_t)cmd->positionals.len; i++) {
        ncc_free(cmd->positionals.data[i].name.data);
    }

    ncc_free(cmd->name.data);
    ncc_free(cmd->doc.data);
    ncc_list_free(cmd->flags);
    ncc_list_free(cmd->positionals);
    ncc_list_free(cmd->subcommands);
}

void
ncc_cmdr_free(ncc_cmdr_t *c)
{
    if (!c) {
        return;
    }

    free_command(&c->root);

    if (c->grammar) {
        ncc_grammar_free(c->grammar);
    }

    ncc_free(c->name.data);
    ncc_free(c);
}

void
ncc_cmdr_set_name(ncc_cmdr_t *c, ncc_string_t name)
{
    if (c) {
        ncc_free(c->name.data);
        c->name = cmdr_intern(name);
    }
}

void
ncc_cmdr_add_command(ncc_cmdr_t *c, ncc_string_t name, ncc_string_t doc)
{
    if (!c || !name.data) {
        return;
    }

    ncc_list_push(c->root.subcommands, ((ncc_cmdr_command_t){0}));

    ncc_cmdr_command_t *cmd
        = &c->root.subcommands.data[c->root.subcommands.len - 1];

    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
ncc_cmdr_add_subcommand(ncc_cmdr_t *c, ncc_string_t parent,
                        ncc_string_t name, ncc_string_t doc)
{
    if (!c || !name.data) {
        return;
    }

    ncc_cmdr_command_t *pcmd = find_command(c, parent);

    if (!pcmd) {
        return;
    }

    ncc_list_push(pcmd->subcommands, ((ncc_cmdr_command_t){0}));

    ncc_cmdr_command_t *cmd
        = &pcmd->subcommands.data[pcmd->subcommands.len - 1];

    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
ncc_cmdr_add_flag(ncc_cmdr_t *c, ncc_string_t command,
                  ncc_string_t flag_name, ncc_cmdr_arg_type_t type,
                  bool takes_value, ncc_string_t doc)
{
    if (!c || !flag_name.data) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ncc_list_push(cmd->flags, ((ncc_cmdr_flag_spec_t){0}));

    ncc_cmdr_flag_spec_t *f = &cmd->flags.data[cmd->flags.len - 1];

    f->name        = cmdr_intern(flag_name);
    f->value_type  = type;
    f->takes_value = takes_value;
    f->doc         = cmdr_intern(doc);
}

void
ncc_cmdr_add_flag_alias(ncc_cmdr_t *c, ncc_string_t command,
                        ncc_string_t flag_name, ncc_string_t alias)
{
    if (!c || !flag_name.data || !alias.data) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ncc_cmdr_flag_spec_t *f = find_flag(cmd, flag_name);

    if (!f) {
        return;
    }

    f->short_name = cmdr_intern(alias);
}

void
ncc_cmdr_add_positional(ncc_cmdr_t *c, ncc_string_t command,
                        ncc_string_t name, ncc_cmdr_arg_type_t type,
                        int min, int max)
{
    if (!c || !name.data) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ncc_list_push(cmd->positionals, ((ncc_cmdr_positional_spec_t){0}));

    ncc_cmdr_positional_spec_t *p
        = &cmd->positionals.data[cmd->positionals.len - 1];

    p->name = cmdr_intern(name);
    p->type = type;
    p->min  = min;
    p->max  = max;
}

static void
register_command_terminals(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd)
{
    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        ncc_cmdr_flag_spec_t *f = &cmd->flags.data[i];
        f->terminal_id = ncc_register_terminal(c->grammar, f->name);

        if (f->short_name.data) {
            ncc_register_terminal(c->grammar, f->short_name);
        }
    }
}

static int64_t
build_flag_nt(ncc_cmdr_t *c, [[maybe_unused]] ncc_cmdr_command_t *cmd,
              ncc_cmdr_flag_spec_t *f, const char *prefix)
{
    char nt_name[256];
    int  nlen = snprintf(nt_name, sizeof(nt_name), "%s-flag-%s", prefix,
                         f->name.data);

    int64_t nt_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar,
                    ncc_string_borrow(nt_name,
                                      nlen > 0 ? (size_t)nlen : 0)));

    int64_t flag_tid = f->terminal_id;
    int64_t eq_tid   = c->tok_ids[NCC_CMDR_TID_EQ];
    int64_t word_tid = c->tok_ids[NCC_CMDR_TID_WORD];
    int64_t int_tid  = c->tok_ids[NCC_CMDR_TID_INT];
    int64_t flt_tid  = c->tok_ids[NCC_CMDR_TID_FLOAT];
    int64_t bool_tid = c->tok_ids[NCC_CMDR_TID_BOOL];

    if (f->takes_value) {
        ncc_add_rule_v(c->grammar, nt_id, 3,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid),
                            NCC_TERMINAL(eq_tid),
                            NCC_TERMINAL(word_tid)});

        if (f->short_name.data) {
            int64_t short_tid = ncc_register_terminal(
                c->grammar, f->short_name);
            ncc_add_rule_v(c->grammar, nt_id, 3,
                (ncc_match_t[]){NCC_TERMINAL(short_tid),
                                NCC_TERMINAL(eq_tid),
                                NCC_TERMINAL(word_tid)});
            ncc_add_rule_v(c->grammar, nt_id, 2,
                (ncc_match_t[]){NCC_TERMINAL(short_tid),
                                NCC_TERMINAL(word_tid)});
        }

        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid),
                            NCC_TERMINAL(word_tid)});

        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(int_tid)});
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(flt_tid)});
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(bool_tid)});
    }
    else {
        ncc_add_rule_v(c->grammar, nt_id, 1,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid)});

        if (f->short_name.data) {
            int64_t short_tid = ncc_register_terminal(
                c->grammar, f->short_name);
            ncc_add_rule_v(c->grammar, nt_id, 1,
                (ncc_match_t[]){NCC_TERMINAL(short_tid)});
        }
    }

    return nt_id;
}

static int64_t
build_items_nt(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd, const char *prefix)
{
    char nt_name[256];
    int  nlen = snprintf(nt_name, sizeof(nt_name), "%s-items", prefix);

    int64_t items_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar,
                    ncc_string_borrow(nt_name,
                                      nlen > 0 ? (size_t)nlen : 0)));

    ncc_match_t items_nt = {.kind = NCC_MATCH_NT, .nt_id = items_id};

    ncc_add_rule_v(c->grammar, items_id, 1,
        (ncc_match_t[]){NCC_EPSILON()});

    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        int64_t fnt_id = build_flag_nt(c, cmd, &cmd->flags.data[i], prefix);
        ncc_add_rule_v(c->grammar, items_id, 2,
            (ncc_match_t[]){{.kind = NCC_MATCH_NT, .nt_id = fnt_id},
                            items_nt});
    }

    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_WORD]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_INT]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_FLOAT]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_BOOL]),
                        items_nt});

    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_DD]),
                        items_nt});

    return items_id;
}

static void
build_command_grammar(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd,
                      int64_t parent_id, const char *prefix)
{
    int64_t items_id = build_items_nt(c, cmd, prefix);
    ncc_match_t items_nt = {.kind = NCC_MATCH_NT, .nt_id = items_id};

    if (cmd->name.data) {
        int64_t name_tid = ncc_register_terminal(c->grammar, cmd->name);
        ncc_add_rule_v(c->grammar, parent_id, 3,
            (ncc_match_t[]){NCC_TERMINAL(name_tid), items_nt,
                            NCC_TERMINAL(NCC_TOK_EOF)});
    }
    else {
        ncc_add_rule_v(c->grammar, parent_id, 2,
            (ncc_match_t[]){items_nt, NCC_TERMINAL(NCC_TOK_EOF)});
    }

    for (int32_t i = 0; i < (int32_t)cmd->subcommands.len; i++) {
        ncc_cmdr_command_t *sub = &cmd->subcommands.data[i];
        char sub_prefix[256];
        snprintf(sub_prefix, sizeof(sub_prefix), "%s-%s",
                 prefix, sub->name.data);
        build_command_grammar(c, sub, parent_id, sub_prefix);
    }
}

void
ncc_cmdr_finalize(ncc_cmdr_t *c)
{
    if (!c || c->finalized) {
        return;
    }

    c->grammar = ncc_grammar_new();
    ncc_grammar_set_error_recovery(c->grammar, false);

    c->tok_ids[NCC_CMDR_TID_WORD]  = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("WORD"));
    c->tok_ids[NCC_CMDR_TID_INT]   = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("INT"));
    c->tok_ids[NCC_CMDR_TID_FLOAT] = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("FLOAT"));
    c->tok_ids[NCC_CMDR_TID_BOOL]  = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("BOOL"));
    c->tok_ids[NCC_CMDR_TID_EQ]    = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("EQ"));
    c->tok_ids[NCC_CMDR_TID_COMMA] = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("COMMA"));
    c->tok_ids[NCC_CMDR_TID_DD]    = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("DD"));
    c->tok_ids[NCC_CMDR_TID_FLAG]  = ncc_register_terminal(
        c->grammar, NCC_STRING_STATIC("FLAG"));

    register_command_terminals(c, &c->root);

    for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
        register_command_terminals(c, &c->root.subcommands.data[i]);
        ncc_register_terminal(c->grammar, c->root.subcommands.data[i].name);
    }

    int64_t start_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar, NCC_STRING_STATIC("cmd")));
    ncc_grammar_set_start_id(c->grammar, start_id);

    if ((int32_t)c->root.subcommands.len > 0) {
        for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
            ncc_cmdr_command_t *sub = &c->root.subcommands.data[i];
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "cmd-%s", sub->name.data);
            build_command_grammar(c, sub, start_id, prefix);
        }

        if ((int32_t)c->root.flags.len > 0) {
            build_command_grammar(c, &c->root, start_id, "cmd-root");
        }
    }
    else {
        build_command_grammar(c, &c->root, start_id, "cmd");
    }

    ncc_grammar_finalize(c->grammar);
    c->finalized = true;
}

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

    char *end = nullptr;
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

static ncc_cmdr_flag_spec_t *
find_flag_anywhere(ncc_cmdr_t *c, ncc_string_t name)
{
    if (!c) {
        return nullptr;
    }

    ncc_cmdr_flag_spec_t *f = find_flag(&c->root, name);

    for (int32_t si = 0; !f && si < (int32_t)c->root.subcommands.len; si++) {
        f = find_flag(&c->root.subcommands.data[si], name);
    }

    return f;
}

static int64_t
find_flag_tid(ncc_cmdr_t *c, const char *name)
{
    ncc_cmdr_flag_spec_t *f = find_flag_anywhere(c, argv_str(name));

    return f ? f->terminal_id : 0;
}

static bool
flag_takes_value(ncc_cmdr_t *c, const char *name)
{
    ncc_cmdr_flag_spec_t *f = find_flag_anywhere(c, argv_str(name));

    return f && f->takes_value;
}

static int64_t
classify_word(ncc_cmdr_t *c, const char *s)
{
    if (is_int_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_INT];
    }

    if (is_float_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_FLOAT];
    }

    if (is_bool_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_BOOL];
    }

    return c->tok_ids[NCC_CMDR_TID_WORD];
}



static ncc_token_info_t *
make_token(const char *value, int64_t tid, int32_t index)
{
    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);

    if (value && *value) {
        ncc_string_t s = cmdr_intern(argv_str(value));
        tok->value = ncc_option_set(ncc_string_t, s);
    }

    tok->tid    = (int32_t)tid;
    tok->index  = index;
    tok->line   = 1;
    tok->column = 1;

    return tok;
}

static int32_t
cmdr_tokenize(const char **argv, int argc, ncc_cmdr_t *c,
              ncc_token_info_t ***tokens_out, int32_t *n_tokens_out)
{
    if (!argv || argc <= 0 || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    ncc_list_t(ncc_token_info_ptr_t) tl
        = ncc_list_new_cap(ncc_token_info_ptr_t, 32);

    bool past_dd = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg) {
            continue;
        }

        if (!past_dd && strcmp(arg, "--") == 0) {
            past_dd = true;
            ncc_list_push(tl, make_token("--",
                                           c->tok_ids[NCC_CMDR_TID_DD],
                                           (int32_t)tl.len));
            continue;
        }

        if (past_dd) {
            ncc_list_push(tl, make_token(arg, classify_word(c, arg),
                                           (int32_t)tl.len));
            continue;
        }

        if (strncmp(arg, "--", 2) == 0 && strlen(arg) > 2) {
            const char *eq = strchr(arg, '=');

            if (eq) {
                size_t flag_len = (size_t)(eq - arg);
                char *flag_name = ncc_alloc_size(1, flag_len + 1);
                memcpy(flag_name, arg, flag_len);

                int64_t ftid = find_flag_tid(c, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[NCC_CMDR_TID_FLAG];
                }

                ncc_list_push(tl, make_token(flag_name, ftid, (int32_t)tl.len));
                ncc_free(flag_name);

                ncc_list_push(tl, make_token("=",
                                               c->tok_ids[NCC_CMDR_TID_EQ],
                                               (int32_t)tl.len));

                const char *val = eq + 1;
                ncc_list_push(tl, make_token(val, classify_word(c, val),
                                               (int32_t)tl.len));
            }
            else {
                int64_t ftid = find_flag_tid(c, arg);

                if (!ftid) {
                    ftid = c->tok_ids[NCC_CMDR_TID_FLAG];
                }

                ncc_list_push(tl, make_token(arg, ftid, (int32_t)tl.len));
            }

            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0'
            && !isdigit((unsigned char)arg[1])) {
            int64_t ftid = find_flag_tid(c, arg);

            if (ftid) {
                ncc_list_push(tl, make_token(arg, ftid, (int32_t)tl.len));
                continue;
            }

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
                    ncc_list_push(tl, make_token(short_flag, stid, (int32_t)tl.len));
                }
            }
            else {
                ncc_list_push(tl, make_token(arg,
                                               c->tok_ids[NCC_CMDR_TID_FLAG],
                                               (int32_t)tl.len));
            }

            continue;
        }

        int64_t tid = classify_word(c, arg);

        for (int32_t si = 0; si < (int32_t)c->root.subcommands.len; si++) {
            if (c->root.subcommands.data[si].name.data
                && strcmp(arg, c->root.subcommands.data[si].name.data) == 0) {
                tid = ncc_register_terminal(c->grammar,
                                              ncc_string_borrow(arg, strlen(arg)));
                break;
            }
        }

        ncc_list_push(tl, make_token(arg, tid, (int32_t)tl.len));
    }

    ncc_list_push(tl, make_token("", NCC_TOK_EOF, (int32_t)tl.len));

    *tokens_out   = tl.data;
    *n_tokens_out = (int32_t)tl.len;

    return 0;
}

static ncc_cmdr_result_t *
make_error_result(const char *msg)
{
    ncc_cmdr_result_t *r = ncc_alloc(ncc_cmdr_result_t);
    r->ok       = false;
    r->errors = ncc_list_new_cap(ncc_string_t, 1);

    ncc_list_push(r->errors, cmdr_intern(argv_str(msg)));

    return r;
}

static const char *
get_token_text(ncc_parse_tree_t *node)
{
    if (!node || !ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(node);

    if (!tok || !ncc_option_is_set(tok->value)) {
        return nullptr;
    }

    ncc_string_t val = ncc_option_get(tok->value);
    return val.data;
}

static bool
collect_terminal_text(ncc_visit_t *v)
{
    const char *text = get_token_text(v->node);

    if (text && *text) {
        ncc_list_t(ncc_const_charptr_t) *texts = v->user;

        ncc_list_push(*texts, text);
    }

    return true;
}

static void
extract_result_from_tree(ncc_cmdr_t *c, ncc_parse_tree_t *tree,
                          ncc_cmdr_result_t *r)
{
    if (!tree || !r) {
        return;
    }

    ncc_list_t(ncc_const_charptr_t) texts
        = ncc_list_new(ncc_const_charptr_t);
    ncc_tree_walk_t                *w = ncc_tree_walk_new(c->grammar, &texts);

    ncc_walk_on_leaf(w, collect_terminal_text, nullptr);
    ncc_tree_walk_run(w, tree);
    ncc_tree_walk_free(w);

    int32_t n = (int32_t)texts.len;

    bool past_dd = false;

    for (int32_t i = 0; i < n; i++) {
        const char *text = texts.data[i];

        if (!text || !*text) {
            continue;
        }

        if (strcmp(text, "--") == 0) {
            past_dd = true;
            goto next;
        }

        if (!r->command.data && !past_dd) {
            for (int32_t si = 0; si < (int32_t)c->root.subcommands.len; si++) {
                if (c->root.subcommands.data[si].name.data
                    && strcmp(text, c->root.subcommands.data[si].name.data) == 0) {
                    r->command = cmdr_intern(argv_str(text));
                    goto next;
                }
            }
        }

        if (past_dd) {
            goto add_arg;
        }

        {
            ncc_cmdr_command_t *cmd = r->command.data
                ? find_command(c, r->command) : &c->root;

            if (!cmd) {
                cmd = &c->root;
            }

            ncc_cmdr_flag_spec_t *flag = find_flag(cmd, argv_str(text));

            if (!flag) {
                flag = find_flag(&c->root, argv_str(text));
            }

            if (flag) {
                ncc_cmdr_val_t *v = ncc_alloc(ncc_cmdr_val_t);

                if (flag->takes_value && i + 1 < n) {
                    if (i + 1 < n && strcmp(texts.data[i + 1], "=") == 0) {
                        i++;
                    }

                    if (i + 1 < n) {
                        i++;
                        const char *val = texts.data[i];

                        switch (flag->value_type) {
                        case NCC_CMDR_TYPE_INT:
                            v->tag = NCC_CMDR_VAL_INT;
                            v->i   = strtoll(val, nullptr, 10);
                            break;
                        case NCC_CMDR_TYPE_FLOAT:
                            v->tag = NCC_CMDR_VAL_FLOAT;
                            v->f   = strtod(val, nullptr);
                            break;
                        case NCC_CMDR_TYPE_BOOL:
                            v->tag = NCC_CMDR_VAL_BOOL;
                            v->b   = (strcmp(val, "true") == 0
                                      || strcmp(val, "yes") == 0);
                            break;
                        default:
                            v->tag = NCC_CMDR_VAL_STR;
                            v->s   = ncc_string_from_cstr(val);
                            break;
                        }
                    }
                    else {
                        ncc_free(v);
                        goto next;
                    }
                }
                else {
                    v->tag = NCC_CMDR_VAL_BOOL;
                    v->b   = true;
                }

                _ncc_dict_put(&r->flags, flag_key(flag->name), (void *)v);

                if (flag->short_name.data) {
                    ncc_cmdr_val_t *alias_v = ncc_alloc(ncc_cmdr_val_t);
                    *alias_v = *v;
                    if (alias_v->tag == NCC_CMDR_VAL_STR && alias_v->s.data) {
                        alias_v->s = ncc_string_from_raw(
                            alias_v->s.data, (int64_t)alias_v->s.u8_bytes);
                    }
                    _ncc_dict_put(&r->flags, flag_key(flag->short_name),
                                  (void *)alias_v);
                }

                goto next;
            }
        }

        if (strcmp(text, "=") == 0) {
            goto next;
        }

    add_arg:
        {
            ncc_list_push(r->args, ((ncc_cmdr_arg_t){0}));

            ncc_cmdr_arg_t *arg = &r->args.data[r->args.len - 1];
            arg->value     = ncc_string_from_cstr(text);
            arg->int_val   = strtoll(text, nullptr, 10);
            arg->float_val = strtod(text, nullptr);
        }

    next:;
    }

    ncc_list_free(texts);
}

static void
free_tokens(ncc_token_info_t **tokens, int32_t n_tokens)
{
    if (!tokens) {
        return;
    }

    for (int32_t i = 0; i < n_tokens; i++) {
        if (tokens[i]) {
            if (ncc_option_is_set(tokens[i]->value)) {
                ncc_string_t val = ncc_option_get(tokens[i]->value);
                ncc_free(val.data);
            }
            ncc_free(tokens[i]);
        }
    }

    ncc_free(tokens);
}

int32_t
ncc_cmdr_command_count(ncc_cmdr_t *c)
{
    return c ? (int32_t)c->root.subcommands.len : 0;
}

static ncc_cmdr_command_t *
command_at(ncc_cmdr_t *c, int32_t index)
{
    if (!c || index < 0 || (size_t)index >= c->root.subcommands.len) {
        return nullptr;
    }

    return &c->root.subcommands.data[index];
}

ncc_string_t
ncc_cmdr_command_name(ncc_cmdr_t *c, int32_t index)
{
    ncc_cmdr_command_t *cmd = command_at(c, index);

    return cmd ? cmd->name : (ncc_string_t){0};
}

ncc_string_t
ncc_cmdr_command_doc(ncc_cmdr_t *c, int32_t index)
{
    ncc_cmdr_command_t *cmd = command_at(c, index);

    return cmd ? cmd->doc : (ncc_string_t){0};
}

static ncc_cmdr_flag_spec_t *
flag_at(ncc_cmdr_t *c, ncc_string_t command, int32_t index)
{
    ncc_cmdr_command_t *cmd = c ? find_command(c, command) : nullptr;

    if (!cmd || index < 0 || (size_t)index >= cmd->flags.len) {
        return nullptr;
    }

    return &cmd->flags.data[index];
}

int32_t
ncc_cmdr_flag_count(ncc_cmdr_t *c, ncc_string_t command)
{
    ncc_cmdr_command_t *cmd = c ? find_command(c, command) : nullptr;

    return cmd ? (int32_t)cmd->flags.len : 0;
}

ncc_string_t
ncc_cmdr_flag_name(ncc_cmdr_t *c, ncc_string_t command, int32_t index)
{
    ncc_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f ? f->name : (ncc_string_t){0};
}

ncc_string_t
ncc_cmdr_flag_doc(ncc_cmdr_t *c, ncc_string_t command, int32_t index)
{
    ncc_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f ? f->doc : (ncc_string_t){0};
}

bool
ncc_cmdr_flag_takes_value(ncc_cmdr_t *c, ncc_string_t command, int32_t index)
{
    ncc_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f && f->takes_value;
}

ncc_cmdr_result_t *
ncc_cmdr_parse(ncc_cmdr_t *c, int argc, const char **argv)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        ncc_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    ncc_token_info_t **tokens = nullptr;
    int32_t             n_tokens = 0;

    if (cmdr_tokenize(argv, argc, c, &tokens, &n_tokens) < 0) {
        return make_error_result("tokenization failed");
    }

    ncc_token_stream_t *ts = ncc_token_stream_from_array(tokens, n_tokens);
    ncc_pwz_parser_t   *pp = ncc_pwz_new(c->grammar);
    bool parse_ok = ncc_pwz_parse(pp, ts);

    ncc_cmdr_result_t *r = ncc_alloc(ncc_cmdr_result_t);
    ncc_dict_init(&r->flags, flag_key_hash, flag_key_eq);

    if (!parse_ok) {
        r->ok       = false;
        r->errors = ncc_list_new_cap(ncc_string_t, 1);

        ncc_list_push(r->errors,
                      cmdr_intern(NCC_STRING_STATIC("parse failed")));
    }
    else {
        r->ok   = true;
        r->tree = ncc_pwz_get_tree(pp);

        if (r->tree) {
            extract_result_from_tree(c, r->tree, r);
        }
    }

    ncc_pwz_free(pp);
    ncc_token_stream_free(ts);
    free_tokens(tokens, n_tokens);

    return r;
}

ncc_cmdr_result_t *
ncc_cmdr_parse_string(ncc_cmdr_t *c, ncc_string_t cmdline)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        ncc_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    ncc_list_t(ncc_const_charptr_t) argv
        = ncc_list_new_cap(ncc_const_charptr_t, 16);

    const char *p = cmdline.data;

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

        char *word = ncc_alloc_size(1, len + 1);
        memcpy(word, start, len);

        ncc_list_push(argv, word);
    }

    ncc_cmdr_result_t *result = ncc_cmdr_parse(c, (int)argv.len, argv.data);

    ncc_list_foreach (argv, w) {
        ncc_free((void *)*w);
    }
    ncc_list_free(argv);

    return result;
}

bool
ncc_cmdr_result_ok(ncc_cmdr_result_t *r)
{
    return r && r->ok;
}

ncc_parse_tree_t *
ncc_cmdr_result_tree(ncc_cmdr_result_t *r)
{
    return r ? r->tree : nullptr;
}

int32_t
ncc_cmdr_error_count(ncc_cmdr_result_t *r)
{
    return r ? (int32_t)r->errors.len : 0;
}

ncc_string_t
ncc_cmdr_error_str(ncc_cmdr_result_t *r, int32_t index)
{
    if (!r || index < 0 || (size_t)index >= r->errors.len) {
        return (ncc_string_t){0};
    }

    return r->errors.data[index];
}

ncc_string_t
ncc_cmdr_result_command(ncc_cmdr_result_t *r)
{
    return r ? r->command : (ncc_string_t){0};
}

bool
ncc_cmdr_flag_present(ncc_cmdr_result_t *r, ncc_string_t flag)
{
    if (!r || !flag.data) {
        return false;
    }

    return ncc_dict_contains(&r->flags, &flag);
}

ncc_cmdr_val_t *
ncc_cmdr_flag_get(ncc_cmdr_result_t *r, ncc_string_t flag)
{
    if (!r || !flag.data) {
        return nullptr;
    }

    bool  found;
    void *val = _ncc_dict_get(&r->flags, &flag, &found);

    return found ? (ncc_cmdr_val_t *)val : nullptr;
}

ncc_string_t
ncc_cmdr_flag_str(ncc_cmdr_result_t *r, ncc_string_t flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_STR) {
        return (ncc_string_t){0};
    }

    return v->s;
}

int64_t
ncc_cmdr_flag_int(ncc_cmdr_result_t *r, ncc_string_t flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_INT) {
        return 0;
    }

    return v->i;
}

bool
ncc_cmdr_flag_bool(ncc_cmdr_result_t *r, ncc_string_t flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_BOOL) {
        return false;
    }

    return v->b;
}

int32_t
ncc_cmdr_arg_count(ncc_cmdr_result_t *r)
{
    return r ? (int32_t)r->args.len : 0;
}

ncc_string_t
ncc_cmdr_arg_str(ncc_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= (int32_t)r->args.len) {
        return (ncc_string_t){0};
    }

    return r->args.data[index].value;
}

int64_t
ncc_cmdr_arg_int(ncc_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= (int32_t)r->args.len) {
        return 0;
    }

    return r->args.data[index].int_val;
}

void
ncc_cmdr_result_free(ncc_cmdr_result_t *r)
{
    if (!r) {
        return;
    }

    ncc_free(r->command.data);

    ncc_dict_foreach (&r->flags, e) {
        ncc_free(((ncc_string_t *)e->key)->data);
        ncc_free(e->key);

        ncc_cmdr_val_t *v = (ncc_cmdr_val_t *)e->value;

        if (v) {
            if (v->tag == NCC_CMDR_VAL_STR) {
                ncc_free(v->s.data);
            }
            ncc_free(v);
        }
    }

    ncc_dict_free(&r->flags);

    for (size_t i = 0; i < r->args.len; i++) {
        ncc_free(r->args.data[i].value.data);
    }

    ncc_list_free(r->args);

    for (size_t i = 0; i < r->errors.len; i++) {
        ncc_free(r->errors.data[i].data);
    }

    ncc_list_free(r->errors);

    r->tree = nullptr;

    ncc_free(r);
}

int32_t
ncc_cmdr_print_errors(ncc_cmdr_result_t *r, FILE *out)
{
    if (!r || r->ok || !r->errors.len) {
        return 0;
    }

    for (int32_t i = 0; i < (int32_t)r->errors.len; i++) {
        fprintf(out, "error: %s\n", r->errors.data[i].data);
    }

    return (int32_t)r->errors.len;
}
