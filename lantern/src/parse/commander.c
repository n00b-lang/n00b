
#include "parse/commander.h"
#include "lib/buffer.h"

enum {
    PARSE_CMDR_TID_WORD  = 0,
    PARSE_CMDR_TID_INT   = 1,
    PARSE_CMDR_TID_FLOAT = 2,
    PARSE_CMDR_TID_BOOL  = 3,
    PARSE_CMDR_TID_EQ    = 4,
    PARSE_CMDR_TID_COMMA = 5,
    PARSE_CMDR_TID_DD    = 6,
    PARSE_CMDR_TID_FLAG  = 7,
    PARSE_CMDR_TID_COUNT = 8,
};

typedef enum {
    PARSE_CMDR_VAL_NONE,
    PARSE_CMDR_VAL_BOOL,
    PARSE_CMDR_VAL_INT,
    PARSE_CMDR_VAL_FLOAT,
    PARSE_CMDR_VAL_STR,
} parse_cmdr_val_tag_t;

typedef struct {
    parse_cmdr_val_tag_t tag;
    union {
        bool         b;
        int64_t      i;
        double       f;
        core_string_t s;
    };
} parse_cmdr_val_t;
core_list_decl(parse_cmdr_val_t);
#include "parse/pwz.h"
#include "parse/tree_visit.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    core_string_t value;
    int64_t      int_val;
    double       float_val;
} parse_cmdr_arg_t;

typedef struct {
    core_string_t        name;
    core_string_t        short_name;
    parse_cmdr_arg_type_t value_type;
    bool                takes_value;
    bool                repeatable;
    core_string_t        doc;
    int64_t             terminal_id;
} parse_cmdr_flag_spec_t;

typedef struct {
    core_string_t        name;
    parse_cmdr_arg_type_t type;
    int                 min;
    int                 max;
} parse_cmdr_positional_spec_t;

typedef struct parse_cmdr_command parse_cmdr_command_t;

core_list_decl(parse_cmdr_arg_t);
core_list_decl(parse_cmdr_flag_spec_t);
core_list_decl(parse_cmdr_positional_spec_t);
core_list_decl(parse_cmdr_command_t);

struct parse_cmdr_command {
    core_string_t                              name;
    core_string_t                              doc;
    core_list_t(parse_cmdr_flag_spec_t)          flags;
    core_list_t(parse_cmdr_positional_spec_t)    positionals;
    core_list_t(parse_cmdr_command_t)            subcommands;
};

struct parse_cmdr_t {
    parse_grammar_t     *grammar;
    parse_cmdr_command_t root;
    int64_t            next_flag_id;
    bool               finalized;
    core_string_t       name;
    core_string_t       banner;
    int64_t            tok_ids[PARSE_CMDR_TID_COUNT];
};

struct parse_cmdr_result_t {
    core_string_t                 command;
    core_dict_t                   flags;
    core_dict_t                   repeated_flags;
    core_list_t(parse_cmdr_arg_t)   args;
    core_list_t(core_string_t)     errors;
    bool                         ok;
    parse_tree_t            *tree;
};

static core_string_t
argv_str(const char *s)
{
    return core_string_borrow(s, s ? strlen(s) : 0);
}

static core_string_t
cmdr_intern(core_string_t s)
{
    if (!s.data) {
        return core_string_empty();
    }

    return core_string_from_raw(s.data, (int64_t)s.u8_bytes);
}

static parse_cmdr_command_t *
find_command(parse_cmdr_t *c, core_string_t name)
{
    if (!name.data || !name.u8_bytes) {
        return &c->root;
    }

    for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
        if (core_string_eq(c->root.subcommands.data[i].name, name)) {
            return &c->root.subcommands.data[i];
        }
    }

    return nullptr;
}

static parse_cmdr_flag_spec_t *
find_flag(parse_cmdr_command_t *cmd, core_string_t flag_name)
{
    if (!cmd || !flag_name.data) {
        return nullptr;
    }

    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        if (core_string_eq(cmd->flags.data[i].name, flag_name)
            || core_string_eq(cmd->flags.data[i].short_name, flag_name)) {
            return &cmd->flags.data[i];
        }
    }

    return nullptr;
}

static core_hash_value_t
flag_key_hash(void *k)
{
    return core_string_hash(*(core_string_t *)k);
}

static bool
flag_key_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a, *(const core_string_t *)b);
}

static core_string_t *
flag_key(core_string_t s)
{
    core_string_t *k = core_alloc(core_string_t);

    *k = core_string_from_raw(s.data, (int64_t)s.u8_bytes);

    return k;
}

static void parse_cmdr_set_name(parse_cmdr_t *c, core_string_t name);
static void parse_cmdr_add_command(parse_cmdr_t *c, core_string_t name,
                                 core_string_t doc);
static void parse_cmdr_add_flag(parse_cmdr_t *c, core_string_t command,
                              core_string_t flag_name,
                              parse_cmdr_arg_type_t type, bool takes_value,
                              core_string_t doc);
static void parse_cmdr_add_flag_alias(parse_cmdr_t *c, core_string_t command,
                                    core_string_t flag_name,
                                    core_string_t alias);
static void parse_cmdr_add_positional(parse_cmdr_t *c, core_string_t command,
                                    core_string_t name,
                                    parse_cmdr_arg_type_t type, int min,
                                    int max);
static void parse_cmdr_finalize(parse_cmdr_t *c);

static int32_t      parse_cmdr_command_count(parse_cmdr_t *c);
static core_string_t parse_cmdr_command_name(parse_cmdr_t *c, int32_t index);
static core_string_t parse_cmdr_command_doc(parse_cmdr_t *c, int32_t index);
static int32_t      parse_cmdr_flag_count(parse_cmdr_t *c, core_string_t command);
static core_string_t parse_cmdr_flag_name(parse_cmdr_t *c, core_string_t command,
                                       int32_t index);
static core_string_t parse_cmdr_flag_doc(parse_cmdr_t *c, core_string_t command,
                                      int32_t index);
static bool parse_cmdr_flag_takes_value(parse_cmdr_t *c, core_string_t command,
                                      int32_t index);

static void
add_flags(parse_cmdr_t *c, core_string_t cmd, const parse_cmdr_flag_t *flags)
{
    for (const parse_cmdr_flag_t *f = flags; f && f->name.data; f++) {
        parse_cmdr_add_flag(c, cmd, f->name, f->type, f->takes_value, f->doc);

        if (f->alias.data) {
            parse_cmdr_add_flag_alias(c, cmd, f->name, f->alias);
        }
    }
}

parse_cmdr_t *
parse_cmdr_new(const parse_cmdr_spec_t *spec)
{
    parse_cmdr_t *c = core_alloc(parse_cmdr_t);

    if (!spec) {
        return c;
    }

    c->banner = core_string_dup(spec->usage);

    parse_cmdr_set_name(c, spec->tool);
    add_flags(c, (core_string_t){0}, spec->flags);

    if (spec->arg.data) {
        parse_cmdr_add_positional(c, (core_string_t){0}, spec->arg,
                                PARSE_CMDR_TYPE_WORD, spec->min_args,
                                spec->max_args);
    }

    for (const parse_cmdr_cmd_t *k = spec->commands; k && k->name.data; k++) {
        parse_cmdr_add_command(c, k->name, k->doc);
        add_flags(c, k->name, k->flags);

        if (k->arg.data) {
            parse_cmdr_add_positional(c, k->name, k->arg, PARSE_CMDR_TYPE_WORD,
                                    k->min_args, k->max_args);
        }
    }

    parse_cmdr_finalize(c);

    return c;
}

bool
parse_cmdr_flag_set_repeatable(parse_cmdr_t *c, core_string_t command,
                               core_string_t flag_name, bool repeatable)
{
    if (!c) return false;
    parse_cmdr_command_t *scope = find_command(c, command);
    parse_cmdr_flag_spec_t *flag = find_flag(scope, flag_name);
    if (!flag) return false;
    flag->repeatable = repeatable;
    return true;
}

void
parse_cmdr_print_usage(parse_cmdr_t *c, FILE *out)
{
    if (!c || !out) {
        return;
    }

    if (c->banner.data) {
        fprintf(out, "%s\n\n", c->banner.data);
    }

    int32_t n = parse_cmdr_command_count(c);

    for (int32_t i = 0; i < n; i++) {
        fprintf(out, "  %-9s %s\n", parse_cmdr_command_name(c, i).data,
                parse_cmdr_command_doc(c, i).data);
    }

    if (n > 0) {
        fputc('\n', out);
    }

    for (int32_t k = -1; k < n; k++) {
        core_string_t cmd = k < 0 ? (core_string_t){0}
                                 : parse_cmdr_command_name(c, k);
        int32_t      nf  = parse_cmdr_flag_count(c, cmd);

        for (int32_t i = 0; i < nf; i++) {
            core_string_t name = parse_cmdr_flag_name(c, cmd, i);

            if (core_string_eq(name, CORE_STRING_STATIC("--help"))) {
                continue;
            }

            core_buffer_t sb = {0};

            core_buffer_printf(&sb, "%s%s", name.data,
                     parse_cmdr_flag_takes_value(c, cmd, i) ? " <value>" : "");
            fprintf(out, "  %-18s %s\n", sb.data,
                    parse_cmdr_flag_doc(c, cmd, i).data);
            core_buffer_release(&sb);
        }

        if (k < 0 && nf > 0) {
            fputc('\n', out);
        }
    }
}

static void
free_command(parse_cmdr_command_t *cmd)
{
    if (!cmd) {
        return;
    }

    for (int32_t i = 0; i < (int32_t)cmd->subcommands.len; i++) {
        free_command(&cmd->subcommands.data[i]);
    }

    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        core_free(cmd->flags.data[i].name.data);
        core_free(cmd->flags.data[i].short_name.data);
        core_free(cmd->flags.data[i].doc.data);
    }

    for (int32_t i = 0; i < (int32_t)cmd->positionals.len; i++) {
        core_free(cmd->positionals.data[i].name.data);
    }

    core_free(cmd->name.data);
    core_free(cmd->doc.data);
    core_list_free(cmd->flags);
    core_list_free(cmd->positionals);
    core_list_free(cmd->subcommands);
}

void
parse_cmdr_free(parse_cmdr_t *c)
{
    if (!c) {
        return;
    }

    free_command(&c->root);

    if (c->grammar) {
        parse_grammar_free(c->grammar);
    }

    core_free(c->name.data);
    core_free(c->banner.data);
    core_free(c);
}

static void
parse_cmdr_set_name(parse_cmdr_t *c, core_string_t name)
{
    if (c) {
        core_free(c->name.data);
        c->name = cmdr_intern(name);
    }
}

static void
parse_cmdr_add_command(parse_cmdr_t *c, core_string_t name, core_string_t doc)
{
    if (!c || !name.data) {
        return;
    }

    core_list_push(c->root.subcommands, ((parse_cmdr_command_t){0}));

    parse_cmdr_command_t *cmd
        = &c->root.subcommands.data[c->root.subcommands.len - 1];

    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}


static void
parse_cmdr_add_flag(parse_cmdr_t *c, core_string_t command,
                  core_string_t flag_name, parse_cmdr_arg_type_t type,
                  bool takes_value, core_string_t doc)
{
    if (!c || !flag_name.data) {
        return;
    }

    parse_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    core_list_push(cmd->flags, ((parse_cmdr_flag_spec_t){0}));

    parse_cmdr_flag_spec_t *f = &cmd->flags.data[cmd->flags.len - 1];

    f->name        = cmdr_intern(flag_name);
    f->value_type  = type;
    f->takes_value = takes_value;
    f->doc         = cmdr_intern(doc);
}

static void
parse_cmdr_add_flag_alias(parse_cmdr_t *c, core_string_t command,
                        core_string_t flag_name, core_string_t alias)
{
    if (!c || !flag_name.data || !alias.data) {
        return;
    }

    parse_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    parse_cmdr_flag_spec_t *f = find_flag(cmd, flag_name);

    if (!f) {
        return;
    }

    f->short_name = cmdr_intern(alias);
}

static void
parse_cmdr_add_positional(parse_cmdr_t *c, core_string_t command,
                        core_string_t name, parse_cmdr_arg_type_t type,
                        int min, int max)
{
    if (!c || !name.data) {
        return;
    }

    parse_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    core_list_push(cmd->positionals, ((parse_cmdr_positional_spec_t){0}));

    parse_cmdr_positional_spec_t *p
        = &cmd->positionals.data[cmd->positionals.len - 1];

    p->name = cmdr_intern(name);
    p->type = type;
    p->min  = min;
    p->max  = max;
}

static void
register_command_terminals(parse_cmdr_t *c, parse_cmdr_command_t *cmd)
{
    for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
        parse_cmdr_flag_spec_t *f = &cmd->flags.data[i];
        f->terminal_id = parse_register_terminal(c->grammar, f->name);

        if (f->short_name.data) {
            parse_register_terminal(c->grammar, f->short_name);
        }
    }
}

static int64_t
build_flag_nt(parse_cmdr_t *c, [[maybe_unused]] parse_cmdr_command_t *cmd,
              parse_cmdr_flag_spec_t *f, const char *prefix)
{
    core_buffer_t nb = {0};

    core_buffer_printf(&nb, "%s-flag-%s", prefix, f->name.data);

    int64_t nt_id = parse_nonterm_id(
        parse_nonterm(c->grammar,
                    core_string_from_raw(nb.data, (int64_t)nb.byte_len)));

    core_buffer_release(&nb);

    int64_t flag_tid = f->terminal_id;
    int64_t eq_tid   = c->tok_ids[PARSE_CMDR_TID_EQ];
    int64_t word_tid = c->tok_ids[PARSE_CMDR_TID_WORD];
    int64_t int_tid  = c->tok_ids[PARSE_CMDR_TID_INT];
    int64_t flt_tid  = c->tok_ids[PARSE_CMDR_TID_FLOAT];
    int64_t bool_tid = c->tok_ids[PARSE_CMDR_TID_BOOL];

    if (f->takes_value) {
        parse_add_rule_v(c->grammar, nt_id, 3,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid),
                            PARSE_TERMINAL(eq_tid),
                            PARSE_TERMINAL(word_tid)});

        if (f->short_name.data) {
            int64_t short_tid = parse_register_terminal(
                c->grammar, f->short_name);
            parse_add_rule_v(c->grammar, nt_id, 3,
                (parse_match_t[]){PARSE_TERMINAL(short_tid),
                                PARSE_TERMINAL(eq_tid),
                                PARSE_TERMINAL(word_tid)});
            parse_add_rule_v(c->grammar, nt_id, 2,
                (parse_match_t[]){PARSE_TERMINAL(short_tid),
                                PARSE_TERMINAL(word_tid)});
        }

        parse_add_rule_v(c->grammar, nt_id, 2,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid),
                            PARSE_TERMINAL(word_tid)});

        parse_add_rule_v(c->grammar, nt_id, 2,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid), PARSE_TERMINAL(int_tid)});
        parse_add_rule_v(c->grammar, nt_id, 2,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid), PARSE_TERMINAL(flt_tid)});
        parse_add_rule_v(c->grammar, nt_id, 2,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid), PARSE_TERMINAL(bool_tid)});
    }
    else {
        parse_add_rule_v(c->grammar, nt_id, 1,
            (parse_match_t[]){PARSE_TERMINAL(flag_tid)});

        if (f->short_name.data) {
            int64_t short_tid = parse_register_terminal(
                c->grammar, f->short_name);
            parse_add_rule_v(c->grammar, nt_id, 1,
                (parse_match_t[]){PARSE_TERMINAL(short_tid)});
        }
    }

    return nt_id;
}

static bool flag_shadowed_by_command(parse_cmdr_command_t *cmd,
                                    const parse_cmdr_flag_spec_t *flag);

static int64_t
build_items_nt(parse_cmdr_t *c, parse_cmdr_command_t *cmd, const char *prefix)
{
    core_buffer_t nb = {0};

    core_buffer_printf(&nb, "%s-items", prefix);

    int64_t items_id = parse_nonterm_id(
        parse_nonterm(c->grammar,
                    core_string_from_raw(nb.data, (int64_t)nb.byte_len)));

    core_buffer_release(&nb);

    parse_match_t items_nt = {.kind = PARSE_MATCH_NT, .nt_id = items_id};

    parse_add_rule_v(c->grammar, items_id, 1,
        (parse_match_t[]){PARSE_EPSILON()});

    for (int32_t i = 0; i < (int32_t)c->root.flags.len; i++) {
        if (cmd != &c->root
            && flag_shadowed_by_command(cmd, &c->root.flags.data[i])) continue;
        int64_t fnt_id = build_flag_nt(c, cmd, &c->root.flags.data[i], prefix);
        parse_add_rule_v(c->grammar, items_id, 2,
            (parse_match_t[]){{.kind = PARSE_MATCH_NT, .nt_id = fnt_id},
                            items_nt});
    }

    if (cmd != &c->root) {
        for (int32_t i = 0; i < (int32_t)cmd->flags.len; i++) {
            parse_cmdr_flag_spec_t *flag = &cmd->flags.data[i];
            int64_t fnt_id = build_flag_nt(c, cmd, flag, prefix);
            parse_add_rule_v(c->grammar, items_id, 2,
                (parse_match_t[]){{.kind = PARSE_MATCH_NT, .nt_id = fnt_id},
                                items_nt});
        }
    }

    parse_add_rule_v(c->grammar, items_id, 2,
        (parse_match_t[]){PARSE_TERMINAL(c->tok_ids[PARSE_CMDR_TID_WORD]),
                        items_nt});
    parse_add_rule_v(c->grammar, items_id, 2,
        (parse_match_t[]){PARSE_TERMINAL(c->tok_ids[PARSE_CMDR_TID_INT]),
                        items_nt});
    parse_add_rule_v(c->grammar, items_id, 2,
        (parse_match_t[]){PARSE_TERMINAL(c->tok_ids[PARSE_CMDR_TID_FLOAT]),
                        items_nt});
    parse_add_rule_v(c->grammar, items_id, 2,
        (parse_match_t[]){PARSE_TERMINAL(c->tok_ids[PARSE_CMDR_TID_BOOL]),
                        items_nt});

    parse_add_rule_v(c->grammar, items_id, 2,
        (parse_match_t[]){PARSE_TERMINAL(c->tok_ids[PARSE_CMDR_TID_DD]),
                        items_nt});

    return items_id;
}

static void
build_command_grammar(parse_cmdr_t *c, parse_cmdr_command_t *cmd,
                      int64_t parent_id, const char *prefix)
{
    int64_t items_id = build_items_nt(c, cmd, prefix);
    parse_match_t items_nt = {.kind = PARSE_MATCH_NT, .nt_id = items_id};

    if (cmd->name.data) {
        int64_t name_tid = parse_register_terminal(c->grammar, cmd->name);
        parse_add_rule_v(c->grammar, parent_id, 3,
            (parse_match_t[]){PARSE_TERMINAL(name_tid), items_nt,
                            PARSE_TERMINAL(PARSE_TOK_EOF)});
    }
    else {
        parse_add_rule_v(c->grammar, parent_id, 2,
            (parse_match_t[]){items_nt, PARSE_TERMINAL(PARSE_TOK_EOF)});
    }

    for (int32_t i = 0; i < (int32_t)cmd->subcommands.len; i++) {
        parse_cmdr_command_t *sub = &cmd->subcommands.data[i];
        core_buffer_t sp = {0};

        core_buffer_printf(&sp, "%s-%s", prefix, sub->name.data);
        build_command_grammar(c, sub, parent_id, sp.data);
        core_buffer_release(&sp);
    }
}

static void
parse_cmdr_finalize(parse_cmdr_t *c)
{
    if (!c || c->finalized) {
        return;
    }

    c->grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(c->grammar, false);

    c->tok_ids[PARSE_CMDR_TID_WORD]  = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("WORD"));
    c->tok_ids[PARSE_CMDR_TID_INT]   = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("INT"));
    c->tok_ids[PARSE_CMDR_TID_FLOAT] = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("FLOAT"));
    c->tok_ids[PARSE_CMDR_TID_BOOL]  = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("BOOL"));
    c->tok_ids[PARSE_CMDR_TID_EQ]    = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("EQ"));
    c->tok_ids[PARSE_CMDR_TID_COMMA] = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("COMMA"));
    c->tok_ids[PARSE_CMDR_TID_DD]    = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("DD"));
    c->tok_ids[PARSE_CMDR_TID_FLAG]  = parse_register_terminal(
        c->grammar, CORE_STRING_STATIC("FLAG"));

    register_command_terminals(c, &c->root);

    for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
        register_command_terminals(c, &c->root.subcommands.data[i]);
        parse_register_terminal(c->grammar, c->root.subcommands.data[i].name);
    }

    int64_t start_id = parse_nonterm_id(
        parse_nonterm(c->grammar, CORE_STRING_STATIC("cmd")));
    if ((int32_t)c->root.subcommands.len > 0) {
        for (int32_t i = 0; i < (int32_t)c->root.subcommands.len; i++) {
            parse_cmdr_command_t *sub = &c->root.subcommands.data[i];
            core_buffer_t pb = {0};

            core_buffer_printf(&pb, "cmd-%s", sub->name.data);
            build_command_grammar(c, sub, start_id, pb.data);
            core_buffer_release(&pb);
        }

        if ((int32_t)c->root.flags.len > 0) {
            build_command_grammar(c, &c->root, start_id, "cmd-root");
        }
    }
    else {
        build_command_grammar(c, &c->root, start_id, "cmd");
    }

    parse_grammar_finalize(c->grammar);
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

static parse_cmdr_flag_spec_t *
find_flag_scoped(parse_cmdr_t *c, parse_cmdr_command_t *cmd, core_string_t name)
{
    if (!c) {
        return nullptr;
    }

    parse_cmdr_flag_spec_t *f = cmd && cmd != &c->root
        ? find_flag(cmd, name) : nullptr;
    return f ? f : find_flag(&c->root, name);
}

static bool
flag_shadowed_by_command(parse_cmdr_command_t *cmd,
                         const parse_cmdr_flag_spec_t *flag)
{
    return cmd && (find_flag(cmd, flag->name)
                   || (flag->short_name.data
                       && find_flag(cmd, flag->short_name)));
}

static int64_t
find_flag_tid(parse_cmdr_t *c, parse_cmdr_command_t *cmd, const char *name)
{
    parse_cmdr_flag_spec_t *f = find_flag_scoped(c, cmd, argv_str(name));

    return f ? f->terminal_id : 0;
}

static bool
flag_takes_value(parse_cmdr_t *c, parse_cmdr_command_t *cmd, const char *name)
{
    parse_cmdr_flag_spec_t *f = find_flag_scoped(c, cmd, argv_str(name));

    return f && f->takes_value;
}

static int64_t
classify_word(parse_cmdr_t *c, const char *s)
{
    if (is_int_str(s)) {
        return c->tok_ids[PARSE_CMDR_TID_INT];
    }

    if (is_float_str(s)) {
        return c->tok_ids[PARSE_CMDR_TID_FLOAT];
    }

    if (is_bool_str(s)) {
        return c->tok_ids[PARSE_CMDR_TID_BOOL];
    }

    return c->tok_ids[PARSE_CMDR_TID_WORD];
}



static parse_token_info_t *
make_token(const char *value, int64_t tid, int32_t index)
{
    parse_token_info_t *tok = core_alloc(parse_token_info_t);

    if (value && *value) {
        core_string_t s = cmdr_intern(argv_str(value));
        tok->value = core_option_set(core_string_t, s);
    }

    tok->tid    = (int32_t)tid;
    tok->index  = index;
    tok->line   = 1;
    tok->column = 1;
    tok->endline = 1;
    tok->endcol  = value ? (uint32_t)strlen(value) + 1 : 1;
    tok->origin = PARSE_TOKEN_SOURCE;

    return tok;
}

static parse_cmdr_command_t *
command_in_args(parse_cmdr_t *c, const char **argv, int argc)
{
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strcmp(arg, "--") == 0) break;
        if (arg[0] == '-') continue;
        for (int32_t si = 0; si < (int32_t)c->root.subcommands.len; si++) {
            parse_cmdr_command_t *cmd = &c->root.subcommands.data[si];
            if (cmd->name.data && strcmp(arg, cmd->name.data) == 0) return cmd;
        }
    }
    return &c->root;
}

static const char **
move_leading_flags_after_command(parse_cmdr_command_t *scope,
                                 parse_cmdr_t *c,
                                 const char **argv, int argc)
{
    if (!scope || !scope->name.data || !argv || argc < 2) return nullptr;
    int command_index = -1;
    for (int i = 0; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], scope->name.data) == 0) {
            command_index = i;
            break;
        }
        if (argv[i] && strcmp(argv[i], "--") == 0) break;
    }
    if (command_index <= 0) return nullptr;
    for (int i = 0; i < command_index; i++) {
        if (!argv[i] || argv[i][0] != '-') return nullptr;
        if (strchr(argv[i], '=')) continue;
        parse_cmdr_flag_spec_t *flag =
            find_flag_scoped(c, scope, argv_str(argv[i]));
        if (flag && flag->takes_value) {
            if (i + 1 >= command_index) return nullptr;
            i++;
        }
    }

    const char **ordered = core_alloc_array(const char *, argc);
    ordered[0] = argv[command_index];
    for (int i = 0; i < command_index; i++) ordered[i + 1] = argv[i];
    for (int i = command_index + 1; i < argc; i++) ordered[i] = argv[i];
    return ordered;
}

static int32_t
cmdr_tokenize(const char **argv, int argc, parse_cmdr_t *c,
              parse_cmdr_command_t *scope,
              parse_token_info_t ***tokens_out, int32_t *n_tokens_out)
{
    if (!argv || argc <= 0 || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    core_list_t(parse_token_info_ptr_t) tl
        = core_list_new_cap(parse_token_info_ptr_t, 32);

    bool past_dd = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg) {
            continue;
        }

        if (!past_dd && strcmp(arg, "--") == 0) {
            past_dd = true;
            core_list_push(tl, make_token("--",
                                           c->tok_ids[PARSE_CMDR_TID_DD],
                                           (int32_t)tl.len));
            continue;
        }

        if (past_dd) {
            core_list_push(tl, make_token(arg, classify_word(c, arg),
                                           (int32_t)tl.len));
            continue;
        }

        if (strncmp(arg, "--", 2) == 0 && strlen(arg) > 2) {
            const char *eq = strchr(arg, '=');

            if (eq) {
                size_t flag_len = (size_t)(eq - arg);
                char *flag_name = core_alloc_size(1, flag_len + 1);
                memcpy(flag_name, arg, flag_len);

                int64_t ftid = find_flag_tid(c, scope, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[PARSE_CMDR_TID_FLAG];
                }

                core_list_push(tl, make_token(flag_name, ftid, (int32_t)tl.len));
                core_free(flag_name);

                core_list_push(tl, make_token("=",
                                               c->tok_ids[PARSE_CMDR_TID_EQ],
                                               (int32_t)tl.len));

                const char *val = eq + 1;
                core_list_push(tl, make_token(val, classify_word(c, val),
                                               (int32_t)tl.len));
            }
            else {
                int64_t ftid = find_flag_tid(c, scope, arg);

                if (!ftid) {
                    ftid = c->tok_ids[PARSE_CMDR_TID_FLAG];
                }

                core_list_push(tl, make_token(arg, ftid, (int32_t)tl.len));
            }

            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0'
            && !isdigit((unsigned char)arg[1])) {
            int64_t ftid = find_flag_tid(c, scope, arg);

            if (ftid) {
                core_list_push(tl, make_token(arg, ftid, (int32_t)tl.len));
                continue;
            }

            bool can_split = false;

            if (strlen(arg) > 2) {
                can_split = true;

                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, scope, short_flag);

                    if (!stid || flag_takes_value(c, scope, short_flag)) {
                        can_split = false;
                        break;
                    }
                }
            }

            if (can_split) {
                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, scope, short_flag);
                    core_list_push(tl, make_token(short_flag, stid, (int32_t)tl.len));
                }
            }
            else {
                core_list_push(tl, make_token(arg,
                                               c->tok_ids[PARSE_CMDR_TID_FLAG],
                                               (int32_t)tl.len));
            }

            continue;
        }

        int64_t tid = classify_word(c, arg);

        for (int32_t si = 0; si < (int32_t)c->root.subcommands.len; si++) {
            if (c->root.subcommands.data[si].name.data
                && strcmp(arg, c->root.subcommands.data[si].name.data) == 0) {
                tid = parse_register_terminal(c->grammar,
                                              core_string_borrow(arg, strlen(arg)));
                break;
            }
        }

        core_list_push(tl, make_token(arg, tid, (int32_t)tl.len));
    }

    core_list_push(tl, make_token("", PARSE_TOK_EOF, (int32_t)tl.len));

    *tokens_out   = tl.data;
    *n_tokens_out = (int32_t)tl.len;

    return 0;
}

static parse_cmdr_result_t *
make_error_result(const char *msg)
{
    parse_cmdr_result_t *r = core_alloc(parse_cmdr_result_t);
    core_dict_init(&r->flags, flag_key_hash, flag_key_eq);
    core_dict_init(&r->repeated_flags, flag_key_hash, flag_key_eq);
    r->ok       = false;
    r->errors = core_list_new_cap(core_string_t, 1);

    core_list_push(r->errors, cmdr_intern(argv_str(msg)));

    return r;
}

static parse_cmdr_val_t
clone_flag_value(const parse_cmdr_val_t *value)
{
    parse_cmdr_val_t copy = *value;
    if (copy.tag == PARSE_CMDR_VAL_STR && copy.s.data)
        copy.s = core_string_from_raw(copy.s.data, (int64_t)copy.s.u8_bytes);
    return copy;
}

static void
append_repeat_value(parse_cmdr_result_t *result, core_string_t name,
                    const parse_cmdr_val_t *value)
{
    bool found = false;
    core_list_t(parse_cmdr_val_t) *values =
        _core_dict_get(&result->repeated_flags, &name, &found);
    if (!found) {
        values = core_alloc(core_list_t(parse_cmdr_val_t));
        *values = core_list_new(parse_cmdr_val_t);
        _core_dict_put(&result->repeated_flags, flag_key(name), values);
    }
    core_list_push(*values, clone_flag_value(value));
}

static void
store_flag_value(parse_cmdr_result_t *result, core_string_t name,
                 parse_cmdr_val_t *value)
{
    core_string_t *key = flag_key(name);
    parse_cmdr_val_t *old = _core_dict_put(&result->flags, key, value);
    if (old) {
        core_free(key->data);
        core_free(key);
        if (old->tag == PARSE_CMDR_VAL_STR) core_free(old->s.data);
        core_free(old);
    }
}

static const char *
get_token_text(parse_tree_t *node)
{
    if (!node || !core_tree_is_leaf(node)) {
        return nullptr;
    }

    parse_token_info_t *tok = core_tree_leaf_value(node);

    if (!tok || !core_option_is_set(tok->value)) {
        return nullptr;
    }

    core_string_t val = core_option_get(tok->value);
    return val.data;
}

static bool
collect_terminal_text(parse_visit_t *v)
{
    const char *text = get_token_text(v->node);

    if (text && *text) {
        core_list_t(core_const_charptr_t) *texts = v->user;

        core_list_push(*texts, text);
    }

    return true;
}

static void
extract_result_from_tree(parse_cmdr_t *c, parse_tree_t *tree,
                          parse_cmdr_result_t *r)
{
    if (!tree || !r) {
        return;
    }

    core_list_t(core_const_charptr_t) texts
        = core_list_new(core_const_charptr_t);
    parse_tree_walk_t                *w = parse_tree_walk_new(c->grammar, &texts);

    parse_walk_on_leaf(w, collect_terminal_text, nullptr);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    int32_t n = (int32_t)texts.len;
    for (int32_t i = 0; i < n; i++) {
        if (!texts.data[i] || strcmp(texts.data[i], "--") == 0) break;
        parse_cmdr_command_t *cmd = find_command(c, argv_str(texts.data[i]));
        if (cmd && cmd != &c->root) {
            r->command = cmdr_intern(argv_str(texts.data[i]));
            break;
        }
    }

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

        if (r->command.data && !past_dd
            && strcmp(text, r->command.data) == 0) goto next;

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
            parse_cmdr_command_t *cmd = r->command.data
                ? find_command(c, r->command) : &c->root;

            if (!cmd) {
                cmd = &c->root;
            }

            parse_cmdr_flag_spec_t *flag = find_flag(cmd, argv_str(text));

            if (!flag) {
                flag = find_flag(&c->root, argv_str(text));
            }

            if (flag) {
                parse_cmdr_val_t *v = core_alloc(parse_cmdr_val_t);

                if (flag->takes_value && i + 1 < n) {
                    if (i + 1 < n && strcmp(texts.data[i + 1], "=") == 0) {
                        i++;
                    }

                    if (i + 1 < n) {
                        i++;
                        const char *val = texts.data[i];

                        switch (flag->value_type) {
                        case PARSE_CMDR_TYPE_INT:
                            v->tag = PARSE_CMDR_VAL_INT;
                            v->i   = strtoll(val, nullptr, 10);
                            break;
                        case PARSE_CMDR_TYPE_FLOAT:
                            v->tag = PARSE_CMDR_VAL_FLOAT;
                            v->f   = strtod(val, nullptr);
                            break;
                        case PARSE_CMDR_TYPE_BOOL:
                            v->tag = PARSE_CMDR_VAL_BOOL;
                            v->b   = (strcmp(val, "true") == 0
                                      || strcmp(val, "yes") == 0);
                            break;
                        default:
                            v->tag = PARSE_CMDR_VAL_STR;
                            v->s   = core_string_from_cstr(val);
                            break;
                        }
                    }
                    else {
                        core_free(v);
                        goto next;
                    }
                }
                else {
                    v->tag = PARSE_CMDR_VAL_BOOL;
                    v->b   = true;
                }

                if (flag->repeatable)
                    append_repeat_value(r, flag->name, v);
                store_flag_value(r, flag->name, v);

                if (flag->short_name.data) {
                    parse_cmdr_val_t *alias_v = core_alloc(parse_cmdr_val_t);
                    *alias_v = *v;
                    if (alias_v->tag == PARSE_CMDR_VAL_STR && alias_v->s.data) {
                        alias_v->s = core_string_from_raw(
                            alias_v->s.data, (int64_t)alias_v->s.u8_bytes);
                    }
                    store_flag_value(r, flag->short_name, alias_v);
                    if (flag->repeatable)
                        append_repeat_value(r, flag->short_name, v);
                }

                goto next;
            }
        }

        if (strcmp(text, "=") == 0) {
            goto next;
        }

    add_arg:
        {
            core_list_push(r->args, ((parse_cmdr_arg_t){0}));

            parse_cmdr_arg_t *arg = &r->args.data[r->args.len - 1];
            arg->value     = core_string_from_cstr(text);
            arg->int_val   = strtoll(text, nullptr, 10);
            arg->float_val = strtod(text, nullptr);
        }

    next:;
    }

    core_list_free(texts);
}

static void
validate_positional_count(parse_cmdr_t *c, parse_cmdr_result_t *r)
{
    parse_cmdr_command_t *cmd = find_command(c, r->command);
    size_t min = 0;
    size_t max = 0;
    bool unbounded = false;

    for (size_t i = 0; i < cmd->positionals.len; i++) {
        parse_cmdr_positional_spec_t *p = &cmd->positionals.data[i];
        min += p->min > 0 ? (size_t)p->min : 0;
        if (p->max < 0) unbounded = true;
        else max += (size_t)p->max;
    }

    if (r->args.len >= min && (unbounded || r->args.len <= max)) return;

    r->ok = false;
    if (!r->errors.data) r->errors = core_list_new(core_string_t);
    core_buffer_t message = {0};
    if (r->args.len < min) {
        core_buffer_printf(&message, "expected at least %zu argument%s",
                           min, min == 1 ? "" : "s");
    }
    else {
        core_buffer_printf(&message, "expected at most %zu argument%s",
                           max, max == 1 ? "" : "s");
    }
    core_list_push(r->errors,
                   core_string_from_raw(message.data,
                                        (int64_t)message.byte_len));
    core_buffer_release(&message);
}

static void
free_tokens(parse_token_info_t **tokens, int32_t n_tokens)
{
    if (!tokens) {
        return;
    }

    for (int32_t i = 0; i < n_tokens; i++) {
        if (tokens[i]) {
            if (core_option_is_set(tokens[i]->value)) {
                core_string_t val = core_option_get(tokens[i]->value);
                core_free(val.data);
            }
            core_free(tokens[i]);
        }
    }

    core_free(tokens);
}

static int32_t
parse_cmdr_command_count(parse_cmdr_t *c)
{
    return c ? (int32_t)c->root.subcommands.len : 0;
}

static parse_cmdr_command_t *
command_at(parse_cmdr_t *c, int32_t index)
{
    if (!c || index < 0 || (size_t)index >= c->root.subcommands.len) {
        return nullptr;
    }

    return &c->root.subcommands.data[index];
}

static core_string_t
parse_cmdr_command_name(parse_cmdr_t *c, int32_t index)
{
    parse_cmdr_command_t *cmd = command_at(c, index);

    return cmd ? cmd->name : (core_string_t){0};
}

static core_string_t
parse_cmdr_command_doc(parse_cmdr_t *c, int32_t index)
{
    parse_cmdr_command_t *cmd = command_at(c, index);

    return cmd ? cmd->doc : (core_string_t){0};
}

static parse_cmdr_flag_spec_t *
flag_at(parse_cmdr_t *c, core_string_t command, int32_t index)
{
    parse_cmdr_command_t *cmd = c ? find_command(c, command) : nullptr;

    if (!cmd || index < 0 || (size_t)index >= cmd->flags.len) {
        return nullptr;
    }

    return &cmd->flags.data[index];
}

static int32_t
parse_cmdr_flag_count(parse_cmdr_t *c, core_string_t command)
{
    parse_cmdr_command_t *cmd = c ? find_command(c, command) : nullptr;

    return cmd ? (int32_t)cmd->flags.len : 0;
}

static core_string_t
parse_cmdr_flag_name(parse_cmdr_t *c, core_string_t command, int32_t index)
{
    parse_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f ? f->name : (core_string_t){0};
}

static core_string_t
parse_cmdr_flag_doc(parse_cmdr_t *c, core_string_t command, int32_t index)
{
    parse_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f ? f->doc : (core_string_t){0};
}

static bool
parse_cmdr_flag_takes_value(parse_cmdr_t *c, core_string_t command, int32_t index)
{
    parse_cmdr_flag_spec_t *f = flag_at(c, command, index);

    return f && f->takes_value;
}

parse_cmdr_result_t *
parse_cmdr_parse(parse_cmdr_t *c, int nargs, const char **args)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        parse_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    parse_token_info_t **tokens = nullptr;
    int32_t             n_tokens = 0;

    parse_cmdr_command_t *scope = command_in_args(c, args, nargs);
    const char **ordered = move_leading_flags_after_command(scope, c, args, nargs);
    const char **input = ordered ? ordered : args;
    if (cmdr_tokenize(input, nargs, c, scope, &tokens, &n_tokens) < 0) {
        core_free(ordered);
        return make_error_result("tokenization failed");
    }
    core_free(ordered);

    parse_token_stream_t *ts = scan_token_stream_from_array(tokens, n_tokens);
    parse_pwz_parser_t   *pp
        = parse_pwz_new_for_start(c->grammar,
                                  parse_nonterm_id(parse_nonterm(
                                      c->grammar,
                                      CORE_STRING_STATIC("cmd"))));
    bool parse_ok = parse_pwz_parse(pp, ts);

    parse_cmdr_result_t *r = core_alloc(parse_cmdr_result_t);
    core_dict_init(&r->flags, flag_key_hash, flag_key_eq);
    core_dict_init(&r->repeated_flags, flag_key_hash, flag_key_eq);

    if (!parse_ok) {
        r->ok       = false;
        r->errors = core_list_new_cap(core_string_t, 1);

        core_list_push(r->errors,
                      cmdr_intern(CORE_STRING_STATIC("parse failed")));
    }
    else {
        r->ok   = true;
        r->tree = parse_pwz_get_tree(pp);

        if (r->tree) {
            extract_result_from_tree(c, r->tree, r);
            validate_positional_count(c, r);
        }
    }

    parse_pwz_free(pp);
    scan_token_stream_free(ts);
    free_tokens(tokens, n_tokens);

    return r;
}


bool
parse_cmdr_result_ok(parse_cmdr_result_t *r)
{
    return r && r->ok;
}




core_string_t
parse_cmdr_result_command(parse_cmdr_result_t *r)
{
    return r ? r->command : (core_string_t){0};
}

int32_t
parse_cmdr_result_error_count(parse_cmdr_result_t *r)
{
    return r ? (int32_t)r->errors.len : 0;
}

core_string_t
parse_cmdr_result_error(parse_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || (size_t)index >= r->errors.len)
        return (core_string_t){0};
    return r->errors.data[index];
}

bool
parse_cmdr_flag_present(parse_cmdr_result_t *r, core_string_t flag)
{
    if (!r || !flag.data) {
        return false;
    }

    return core_dict_contains(&r->flags, &flag);
}

static parse_cmdr_val_t *
parse_cmdr_flag_get(parse_cmdr_result_t *r, core_string_t flag)
{
    if (!r || !flag.data) {
        return nullptr;
    }

    bool  found;
    void *val = _core_dict_get(&r->flags, &flag, &found);

    return found ? (parse_cmdr_val_t *)val : nullptr;
}

core_string_t
parse_cmdr_flag_str(parse_cmdr_result_t *r, core_string_t flag)
{
    parse_cmdr_val_t *v = parse_cmdr_flag_get(r, flag);

    if (!v || v->tag != PARSE_CMDR_VAL_STR) {
        return (core_string_t){0};
    }

    return v->s;
}

int32_t
parse_cmdr_flag_value_count(parse_cmdr_result_t *r, core_string_t flag)
{
    if (!r || !flag.data) return 0;
    bool found = false;
    core_list_t(parse_cmdr_val_t) *values =
        _core_dict_get(&r->repeated_flags, &flag, &found);
    if (found) return (int32_t)values->len;
    return parse_cmdr_flag_get(r, flag) ? 1 : 0;
}

core_string_t
parse_cmdr_flag_str_at(parse_cmdr_result_t *r, core_string_t flag,
                       int32_t index)
{
    if (!r || !flag.data || index < 0) return (core_string_t){0};
    bool found = false;
    core_list_t(parse_cmdr_val_t) *values =
        _core_dict_get(&r->repeated_flags, &flag, &found);
    if (found) {
        if ((size_t)index >= values->len
            || values->data[index].tag != PARSE_CMDR_VAL_STR)
            return (core_string_t){0};
        return values->data[index].s;
    }
    return index == 0 ? parse_cmdr_flag_str(r, flag) : (core_string_t){0};
}

int64_t
parse_cmdr_flag_int(parse_cmdr_result_t *r, core_string_t flag, int64_t dflt)
{
    parse_cmdr_val_t *v = parse_cmdr_flag_get(r, flag);

    if (!v) {
        return dflt;
    }

    switch (v->tag) {
    case PARSE_CMDR_VAL_INT:
        return v->i;
    case PARSE_CMDR_VAL_FLOAT:
        return (int64_t)v->f;
    case PARSE_CMDR_VAL_STR:
        return v->s.data ? strtoll(v->s.data, nullptr, 10) : dflt;
    default:
        return dflt;
    }
}



int32_t
parse_cmdr_arg_count(parse_cmdr_result_t *r)
{
    return r ? (int32_t)r->args.len : 0;
}

core_string_t
parse_cmdr_arg_str(parse_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= (int32_t)r->args.len) {
        return (core_string_t){0};
    }

    return r->args.data[index].value;
}


void
parse_cmdr_result_free(parse_cmdr_result_t *r)
{
    if (!r) {
        return;
    }

    core_free(r->command.data);

    core_dict_foreach (&r->flags, e) {
        core_free(((core_string_t *)e->key)->data);
        core_free(e->key);

        parse_cmdr_val_t *v = (parse_cmdr_val_t *)e->value;

        if (v) {
            if (v->tag == PARSE_CMDR_VAL_STR) {
                core_free(v->s.data);
            }
            core_free(v);
        }
    }

    core_dict_free(&r->flags);

    core_dict_foreach (&r->repeated_flags, e) {
        core_free(((core_string_t *)e->key)->data);
        core_free(e->key);
        core_list_t(parse_cmdr_val_t) *values = e->value;
        for (size_t i = 0; i < values->len; i++)
            if (values->data[i].tag == PARSE_CMDR_VAL_STR)
                core_free(values->data[i].s.data);
        core_list_free(*values);
        core_free(values);
    }
    core_dict_free(&r->repeated_flags);

    for (size_t i = 0; i < r->args.len; i++) {
        core_free(r->args.data[i].value.data);
    }

    core_list_free(r->args);

    for (size_t i = 0; i < r->errors.len; i++) {
        core_free(r->errors.data[i].data);
    }

    core_list_free(r->errors);

    r->tree = nullptr;

    core_free(r);
}
