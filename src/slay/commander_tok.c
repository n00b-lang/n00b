#include "slay/commander.h"

#include "core/alloc.h"
#include "core/string.h"
#include "core/option.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static bool
cmdr_is_int(const char *s)
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
cmdr_is_float(const char *s)
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
cmdr_is_bool(const char *s)
{
    if (!s) {
        return false;
    }

    return strcmp(s, "true") == 0 || strcmp(s, "false") == 0
        || strcmp(s, "yes") == 0 || strcmp(s, "no") == 0;
}

static int64_t
cmdr_find_flag_tid(n00b_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return 0;
    }

    // Search root flags
    int32_t n_root_flags = n00b_list_len(c->root.flags);

    for (int32_t i = 0; i < n_root_flags; i++) {
        n00b_cmdr_flag_spec_t f = n00b_list_get(c->root.flags, i);

        if (strcmp(f.name.data, name) == 0) {
            return f.terminal_id;
        }

        if (f.has_short && strcmp(f.short_name.data, name) == 0) {
            return f.terminal_id;
        }
    }

    // Search subcommand flags
    int32_t n_subs = n00b_list_len(c->root.subcommands);

    for (int32_t si = 0; si < n_subs; si++) {
        n00b_cmdr_command_t sub = n00b_list_get(c->root.subcommands, si);
        int32_t n_flags = n00b_list_len(sub.flags);

        for (int32_t i = 0; i < n_flags; i++) {
            n00b_cmdr_flag_spec_t f = n00b_list_get(sub.flags, i);

            if (strcmp(f.name.data, name) == 0) {
                return f.terminal_id;
            }

            if (f.has_short && strcmp(f.short_name.data, name) == 0) {
                return f.terminal_id;
            }
        }
    }

    return 0;
}

static bool
cmdr_flag_takes_value(n00b_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return false;
    }

    int32_t n_root_flags = n00b_list_len(c->root.flags);

    for (int32_t i = 0; i < n_root_flags; i++) {
        n00b_cmdr_flag_spec_t f = n00b_list_get(c->root.flags, i);

        if (strcmp(f.name.data, name) == 0) {
            return f.takes_value;
        }

        if (f.has_short && strcmp(f.short_name.data, name) == 0) {
            return f.takes_value;
        }
    }

    int32_t n_subs = n00b_list_len(c->root.subcommands);

    for (int32_t si = 0; si < n_subs; si++) {
        n00b_cmdr_command_t sub = n00b_list_get(c->root.subcommands, si);
        int32_t n_flags = n00b_list_len(sub.flags);

        for (int32_t i = 0; i < n_flags; i++) {
            n00b_cmdr_flag_spec_t f = n00b_list_get(sub.flags, i);

            if (strcmp(f.name.data, name) == 0) {
                return f.takes_value;
            }

            if (f.has_short && strcmp(f.short_name.data, name) == 0) {
                return f.takes_value;
            }
        }
    }

    return false;
}

static int64_t
cmdr_classify_word(n00b_cmdr_t *c, const char *s)
{
    if (cmdr_is_int(s)) {
        return c->tok_ids[N00B_CMDR_TID_INT];
    }

    if (cmdr_is_float(s)) {
        return c->tok_ids[N00B_CMDR_TID_FLOAT];
    }

    if (cmdr_is_bool(s)) {
        return c->tok_ids[N00B_CMDR_TID_BOOL];
    }

    return c->tok_ids[N00B_CMDR_TID_WORD];
}

// ============================================================================
// Token array management
// ============================================================================

typedef struct {
    n00b_token_info_t **tokens;
    int32_t             len;
    int32_t             cap;
} cmdr_token_list_t;

static void
cmdr_tok_list_init(cmdr_token_list_t *tl)
{
    tl->cap    = 32;
    tl->len    = 0;
    tl->tokens = n00b_alloc_array(n00b_token_info_t *, 32);
}

static void
cmdr_tok_list_push(cmdr_token_list_t *tl, n00b_token_info_t *tok)
{
    if (tl->len >= tl->cap) {
        int32_t new_cap = tl->cap * 2;
        n00b_token_info_t **new_arr = n00b_alloc_array(n00b_token_info_t *, new_cap);
        memcpy(new_arr, tl->tokens, tl->len * sizeof(n00b_token_info_t *));
        n00b_free(tl->tokens);
        tl->tokens = new_arr;
        tl->cap    = new_cap;
    }

    tl->tokens[tl->len++] = tok;
}

static n00b_token_info_t *
cmdr_make_token(const char *value, int64_t tid, int32_t index)
{
    n00b_token_info_t *tok = n00b_alloc(n00b_token_info_t);

    if (value && *value) {
        tok->value = n00b_option_set(n00b_string_t,
                                      n00b_string_from_cstr(value));
    }

    tok->tid    = tid;
    tok->index  = index;
    tok->line   = 1;
    tok->column = 1;

    return tok;
}

// ============================================================================
// Public tokenizer API
// ============================================================================

int32_t
n00b_cmdr_tokenize(const char **argv, int argc,
                    n00b_cmdr_t *c,
                    n00b_token_info_t ***tokens_out,
                    int32_t *n_tokens_out)
{
    if (!argv || argc <= 0 || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    cmdr_token_list_t tl;
    cmdr_tok_list_init(&tl);

    bool past_dd = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg) {
            continue;
        }

        // -- means end of flags
        if (!past_dd && strcmp(arg, "--") == 0) {
            past_dd = true;
            cmdr_tok_list_push(&tl,
                               cmdr_make_token("--",
                                               c->tok_ids[N00B_CMDR_TID_DD],
                                               tl.len));
            continue;
        }

        // After --, everything is a word
        if (past_dd) {
            cmdr_tok_list_push(&tl,
                               cmdr_make_token(arg,
                                               cmdr_classify_word(c, arg),
                                               tl.len));
            continue;
        }

        // --flag=value
        if (strncmp(arg, "--", 2) == 0 && strlen(arg) > 2) {
            const char *eq = strchr(arg, '=');

            if (eq) {
                size_t flag_len  = (size_t)(eq - arg);
                char  *flag_name = n00b_alloc_array(char, flag_len + 1);
                memcpy(flag_name, arg, flag_len);

                int64_t ftid = cmdr_find_flag_tid(c, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[N00B_CMDR_TID_FLAG];
                }

                cmdr_tok_list_push(&tl,
                                   cmdr_make_token(flag_name, ftid, tl.len));
                n00b_free(flag_name);

                cmdr_tok_list_push(&tl,
                                   cmdr_make_token("=",
                                                   c->tok_ids[N00B_CMDR_TID_EQ],
                                                   tl.len));

                const char *val = eq + 1;
                cmdr_tok_list_push(&tl,
                                   cmdr_make_token(val,
                                                   cmdr_classify_word(c, val),
                                                   tl.len));
            }
            else {
                int64_t ftid = cmdr_find_flag_tid(c, arg);

                if (!ftid) {
                    ftid = c->tok_ids[N00B_CMDR_TID_FLAG];
                }

                cmdr_tok_list_push(&tl,
                                   cmdr_make_token(arg, ftid, tl.len));
            }

            continue;
        }

        // -x short flag or multi-char unknown flag
        if (arg[0] == '-' && arg[1] != '\0'
            && !isdigit((unsigned char)arg[1])) {
            // Check if the whole arg is a known flag
            int64_t ftid = cmdr_find_flag_tid(c, arg);

            if (ftid) {
                cmdr_tok_list_push(&tl,
                                   cmdr_make_token(arg, ftid, tl.len));
                continue;
            }

            // Try splitting: only if ALL chars are registered
            // one-char no-value flags
            bool can_split = false;

            if (strlen(arg) > 2) {
                can_split = true;

                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = cmdr_find_flag_tid(c, short_flag);

                    if (!stid || cmdr_flag_takes_value(c, short_flag)) {
                        can_split = false;
                        break;
                    }
                }
            }

            if (can_split) {
                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = cmdr_find_flag_tid(c, short_flag);
                    cmdr_tok_list_push(&tl,
                                       cmdr_make_token(short_flag, stid,
                                                       tl.len));
                }
            }
            else {
                cmdr_tok_list_push(&tl,
                                   cmdr_make_token(arg,
                                                   c->tok_ids[N00B_CMDR_TID_FLAG],
                                                   tl.len));
            }

            continue;
        }

        // Bare word — also check if it's a command name
        int64_t tid = cmdr_classify_word(c, arg);

        // Check if this is a registered subcommand name
        if (c->grammar) {
            int32_t n_subs = n00b_list_len(c->root.subcommands);

            for (int32_t si = 0; si < n_subs; si++) {
                n00b_cmdr_command_t sub = n00b_list_get(c->root.subcommands,
                                                         si);

                if (sub.has_name && strcmp(arg, sub.name.data) == 0) {
                    tid = n00b_register_terminal(c->grammar,
                                                  n00b_string_from_cstr(arg));
                    break;
                }
            }
        }

        cmdr_tok_list_push(&tl, cmdr_make_token(arg, tid, tl.len));
    }

    // Add EOF
    cmdr_tok_list_push(&tl, cmdr_make_token("", N00B_TOK_EOF, tl.len));

    *tokens_out   = tl.tokens;
    *n_tokens_out = tl.len;

    return 0;
}

int32_t
n00b_cmdr_tokenize_string(n00b_string_t cmdline,
                           n00b_cmdr_t *c,
                           n00b_token_info_t ***tokens_out,
                           int32_t *n_tokens_out)
{
    if (!cmdline.data || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    // Split on whitespace, respecting quotes
    const char **argv = NULL;
    int          argc = 0;
    int          cap  = 16;

    argv = n00b_alloc_array(const char *, 16);

    const char *p = cmdline.data;
    const char *end = cmdline.data + cmdline.u8_bytes;

    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }

        if (p >= end) {
            break;
        }

        const char *start  = p;
        bool        in_quote   = false;
        char        quote_char = 0;

        while (p < end) {
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

        // Strip outer quotes
        if (len >= 2
            && ((start[0] == '"' && start[len - 1] == '"')
                || (start[0] == '\'' && start[len - 1] == '\''))) {
            start++;
            len -= 2;
        }

        // Create null-terminated copy
        char *word = n00b_alloc_array(char, len + 1);
        memcpy(word, start, len);

        if (argc >= cap) {
            int new_cap = cap * 2;
            const char **new_argv = n00b_alloc_array(const char *, new_cap);
            memcpy(new_argv, argv, argc * sizeof(const char *));
            n00b_free(argv);
            argv = new_argv;
            cap  = new_cap;
        }

        argv[argc++] = word;
    }

    int32_t ret = n00b_cmdr_tokenize(argv, argc, c, tokens_out, n_tokens_out);

    for (int i = 0; i < argc; i++) {
        n00b_free((void *)argv[i]);
    }

    n00b_free(argv);

    return ret;
}
