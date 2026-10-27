
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
    bool                hidden;
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
core_list_decl(parse_cmdr_error_t);
core_list_decl(parse_cmdr_flag_spec_t);
core_list_decl(parse_cmdr_positional_spec_t);
core_list_decl(parse_cmdr_command_t);

struct parse_cmdr_command {
    core_string_t                              name;
    core_string_t                              doc;
    core_list_t(parse_cmdr_flag_spec_t)          flags;
    core_list_t(parse_cmdr_positional_spec_t)    positionals;
    core_list_t(parse_cmdr_command_t)            subcommands;
    bool                                       hidden;
};

struct parse_cmdr_t {
    parse_grammar_t     *grammar;
    parse_cmdr_command_t root;
    int64_t            next_flag_id;
    bool               finalized;
    core_string_t       name;
    core_string_t       banner;
    core_string_t       default_command;
    int64_t            tok_ids[PARSE_CMDR_TID_COUNT];
};

struct parse_cmdr_result_t {
    core_string_t                 command;
    core_dict_t                   flags;
    core_dict_t                   repeated_flags;
    core_list_t(parse_cmdr_arg_t)   args;
    core_list_t(core_string_t)     errors;
    core_list_t(parse_cmdr_error_t) details;
    bool                         command_given;
    bool                         ok;
    parse_tree_t            *tree;
};

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

static void parse_cmdr_set_name(parse_cmdr_t *c, core_string_t name);
static void parse_cmdr_add_command(parse_cmdr_t *c,
                                 const parse_cmdr_cmd_t *spec);
static void parse_cmdr_add_flag(parse_cmdr_t *c, core_string_t command,
                              const parse_cmdr_flag_t *spec);
static void parse_cmdr_add_flag_alias(parse_cmdr_t *c, core_string_t command,
                                    core_string_t flag_name,
                                    core_string_t alias);
static void parse_cmdr_add_positional(parse_cmdr_t *c, core_string_t command,
                                    core_string_t name,
                                    parse_cmdr_arg_type_t type, int min,
                                    int max);
static void parse_cmdr_finalize(parse_cmdr_t *c);

static void
add_flags(parse_cmdr_t *c, core_string_t cmd, const parse_cmdr_flag_t *flags)
{
    for (const parse_cmdr_flag_t *f = flags; f && f->name.data; f++) {
        parse_cmdr_add_flag(c, cmd, f);

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
    c->default_command = core_string_dup(spec->default_command);

    parse_cmdr_set_name(c, spec->tool);
    add_flags(c, (core_string_t){0}, spec->flags);

    if (spec->arg.data) {
        parse_cmdr_add_positional(c, (core_string_t){0}, spec->arg,
                                PARSE_CMDR_TYPE_WORD, spec->min_args,
                                spec->max_args);
    }

    for (const parse_cmdr_cmd_t *k = spec->commands; k && k->name.data; k++) {
        parse_cmdr_add_command(c, k);
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

static void
flag_label(core_buffer_t *out, const parse_cmdr_flag_spec_t *f)
{
    if (f->short_name.data) {
        core_buffer_append_string(out, f->short_name);
        core_buffer_puts(out, ", ");
    }
    else {
        core_buffer_puts(out, "    ");
    }

    core_buffer_append_string(out, f->name);

    if (f->takes_value && f->value_type == PARSE_CMDR_TYPE_INT) {
        core_buffer_puts(out, " <n>");
    }
    else if (f->takes_value) {
        core_buffer_puts(out, " <value>");
    }
}

static void
print_flag_section(FILE *out, core_string_t title,
                   const parse_cmdr_command_t *cmd, bool all)
{
    size_t width = 0;
    size_t shown = 0;

    for (size_t i = 0; i < cmd->flags.len; i++) {
        const parse_cmdr_flag_spec_t *f = &cmd->flags.data[i];

        if (f->hidden && !all) {
            continue;
        }

        core_buffer_t label = {0};

        flag_label(&label, f);
        if (label.byte_len > width) {
            width = label.byte_len;
        }
        core_buffer_release(&label);
        shown++;
    }

    if (!shown) {
        return;
    }

    fprintf(out, "\n%.*s:\n", (int)title.u8_bytes, title.data);

    for (size_t i = 0; i < cmd->flags.len; i++) {
        const parse_cmdr_flag_spec_t *f = &cmd->flags.data[i];

        if (f->hidden && !all) {
            continue;
        }

        core_buffer_t label = {0};

        flag_label(&label, f);
        fprintf(out, "  %-*.*s  %.*s\n", (int)width, (int)label.byte_len,
                label.data, (int)f->doc.u8_bytes, f->doc.data);
        core_buffer_release(&label);
    }
}

static void
print_command_list(FILE *out, parse_cmdr_t *c, bool all)
{
    size_t width = 0;

    for (size_t i = 0; i < c->root.subcommands.len; i++) {
        const parse_cmdr_command_t *cmd = &c->root.subcommands.data[i];

        if ((!cmd->hidden || all) && cmd->name.u8_bytes > width) {
            width = cmd->name.u8_bytes;
        }
    }

    if (!width) {
        return;
    }

    fputs("\ncommands:\n", out);

    for (size_t i = 0; i < c->root.subcommands.len; i++) {
        const parse_cmdr_command_t *cmd = &c->root.subcommands.data[i];

        if (cmd->hidden && !all) {
            continue;
        }

        fprintf(out, "  %-*.*s  %.*s\n", (int)width, (int)cmd->name.u8_bytes,
                cmd->name.data, (int)cmd->doc.u8_bytes, cmd->doc.data);
    }
}

static void
print_banner(FILE *out, parse_cmdr_t *c)
{
    if (c->banner.data) {
        fprintf(out, "%.*s\n", (int)c->banner.u8_bytes, c->banner.data);
    }
}

static void
print_listing(parse_cmdr_t *c, FILE *out, bool all)
{
    if (!c || !out) {
        return;
    }

    print_banner(out, c);
    print_command_list(out, c, all);
    print_flag_section(out, CORE_STRING_STATIC("options"), &c->root, all);

    for (size_t i = 0; i < c->root.subcommands.len; i++) {
        const parse_cmdr_command_t *cmd = &c->root.subcommands.data[i];

        if (cmd->hidden && !all) {
            continue;
        }

        core_buffer_t title = {0};

        core_buffer_append_string(&title, cmd->name);
        core_buffer_puts(&title, " options");
        print_flag_section(out, core_buffer_string(&title), cmd, all);
        core_buffer_release(&title);
    }
}

void
parse_cmdr_print_usage(parse_cmdr_t *c, FILE *out)
{
    print_listing(c, out, false);
}

void
parse_cmdr_print_all(parse_cmdr_t *c, FILE *out)
{
    print_listing(c, out, true);
}

void
parse_cmdr_print_summary(parse_cmdr_t *c, FILE *out)
{
    if (!c || !out) {
        return;
    }

    print_banner(out, c);
    print_command_list(out, c, false);
    print_flag_section(out, CORE_STRING_STATIC("options"), &c->root, false);
}

bool
parse_cmdr_has_command(parse_cmdr_t *c, core_string_t command)
{
    return c && command.u8_bytes && find_command(c, command);
}

bool
parse_cmdr_print_command_help(parse_cmdr_t *c, core_string_t command,
                              FILE *out)
{
    if (!parse_cmdr_has_command(c, command) || !out) {
        return false;
    }

    const parse_cmdr_command_t *cmd = find_command(c, command);

    fprintf(out, "usage: %.*s %.*s [options]", (int)c->name.u8_bytes,
            c->name.data, (int)cmd->name.u8_bytes, cmd->name.data);

    for (size_t i = 0; i < cmd->positionals.len; i++) {
        const parse_cmdr_positional_spec_t *p = &cmd->positionals.data[i];

        fprintf(out, " %s<%.*s>%s%s", p->min ? "" : "[",
                (int)p->name.u8_bytes, p->name.data,
                p->max == 1 ? "" : "...", p->min ? "" : "]");
    }

    fprintf(out, "\n\n%.*s\n", (int)cmd->doc.u8_bytes, cmd->doc.data);
    print_flag_section(out, CORE_STRING_STATIC("options"), cmd, false);
    print_flag_section(out, CORE_STRING_STATIC("global options"), &c->root,
                       false);

    return true;
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
    core_free(c->default_command.data);
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
parse_cmdr_add_command(parse_cmdr_t *c, const parse_cmdr_cmd_t *spec)
{
    if (!c || !spec->name.data) {
        return;
    }

    core_list_push(c->root.subcommands, ((parse_cmdr_command_t){0}));

    parse_cmdr_command_t *cmd
        = &c->root.subcommands.data[c->root.subcommands.len - 1];

    cmd->name   = cmdr_intern(spec->name);
    cmd->doc    = cmdr_intern(spec->doc);
    cmd->hidden = spec->hidden;
}


static void
parse_cmdr_add_flag(parse_cmdr_t *c, core_string_t command,
                  const parse_cmdr_flag_t *spec)
{
    if (!c || !spec->name.data) {
        return;
    }

    parse_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    core_list_push(cmd->flags, ((parse_cmdr_flag_spec_t){0}));

    parse_cmdr_flag_spec_t *f = &cmd->flags.data[cmd->flags.len - 1];

    f->name        = cmdr_intern(spec->name);
    f->value_type  = spec->type;
    f->takes_value = spec->takes_value;
    f->hidden      = spec->hidden;
    f->doc         = cmdr_intern(spec->doc);
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
              parse_cmdr_flag_spec_t *f, core_string_t prefix)
{
    core_buffer_t nb = {0};

    core_buffer_append_string(&nb, prefix);
    core_buffer_puts(&nb, "-flag-");
    core_buffer_append_string(&nb, f->name);

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
        int64_t value_tids[] = {word_tid, int_tid, flt_tid, bool_tid};
        int64_t short_tid = f->short_name.data
            ? parse_register_terminal(c->grammar, f->short_name) : 0;
        for (size_t v = 0; v < sizeof(value_tids) / sizeof(value_tids[0]);
             v++) {
            parse_add_rule_v(c->grammar, nt_id, 3,
                (parse_match_t[]){PARSE_TERMINAL(flag_tid),
                                PARSE_TERMINAL(eq_tid),
                                PARSE_TERMINAL(value_tids[v])});
            if (!short_tid) continue;
            parse_add_rule_v(c->grammar, nt_id, 3,
                (parse_match_t[]){PARSE_TERMINAL(short_tid),
                                PARSE_TERMINAL(eq_tid),
                                PARSE_TERMINAL(value_tids[v])});
            parse_add_rule_v(c->grammar, nt_id, 2,
                (parse_match_t[]){PARSE_TERMINAL(short_tid),
                                PARSE_TERMINAL(value_tids[v])});
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
build_items_nt(parse_cmdr_t *c, parse_cmdr_command_t *cmd,
               core_string_t prefix)
{
    core_buffer_t nb = {0};

    core_buffer_append_string(&nb, prefix);
    core_buffer_puts(&nb, "-items");

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
                      int64_t parent_id, core_string_t prefix)
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

        core_buffer_append_string(&sp, prefix);
        core_buffer_putc(&sp, '-');
        core_buffer_append_string(&sp, sub->name);
        build_command_grammar(c, sub, parent_id, core_buffer_string(&sp));
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

            core_buffer_puts(&pb, "cmd-");
            core_buffer_append_string(&pb, sub->name);
            build_command_grammar(c, sub, start_id, core_buffer_string(&pb));
            core_buffer_release(&pb);
        }

        if ((int32_t)c->root.flags.len > 0) {
            build_command_grammar(c, &c->root, start_id,
                                  CORE_STRING_STATIC("cmd-root"));
        }
    }
    else {
        build_command_grammar(c, &c->root, start_id,
                              CORE_STRING_STATIC("cmd"));
    }

    parse_grammar_finalize(c->grammar);
    c->finalized = true;
}

static bool
is_int_str(core_string_t s)
{
    if (!s.data || !s.u8_bytes) {
        return false;
    }

    size_t p = 0;

    if (s.data[p] == '-' || s.data[p] == '+') {
        p++;
    }

    if (p == s.u8_bytes) {
        return false;
    }

    while (p < s.u8_bytes) {
        if (!isdigit((unsigned char)s.data[p])) {
            return false;
        }
        p++;
    }

    return true;
}

static bool
is_float_str(core_string_t s)
{
    if (!s.data || !s.u8_bytes) {
        return false;
    }

    double value    = 0;
    size_t consumed = 0;

    core_string_parse_f64(s, &value, &consumed);

    return consumed == s.u8_bytes
           && (core_string_find_byte(s, '.') >= 0
               || core_string_find_byte(s, 'e') >= 0
               || core_string_find_byte(s, 'E') >= 0);
}

static bool
is_bool_str(core_string_t s)
{
    if (!s.data) {
        return false;
    }

    return core_string_eq(s, CORE_STRING_STATIC("true"))
        || core_string_eq(s, CORE_STRING_STATIC("false"))
        || core_string_eq(s, CORE_STRING_STATIC("yes"))
        || core_string_eq(s, CORE_STRING_STATIC("no"));
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
find_flag_tid(parse_cmdr_t *c, parse_cmdr_command_t *cmd, core_string_t name)
{
    parse_cmdr_flag_spec_t *f = find_flag_scoped(c, cmd, name);

    return f ? f->terminal_id : 0;
}

static bool
flag_takes_value(parse_cmdr_t *c, parse_cmdr_command_t *cmd,
                 core_string_t name)
{
    parse_cmdr_flag_spec_t *f = find_flag_scoped(c, cmd, name);

    return f && f->takes_value;
}

static int64_t
classify_word(parse_cmdr_t *c, core_string_t s)
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
make_token(core_string_t value, int64_t tid, int32_t index)
{
    parse_token_info_t *tok = parse_token_new();

    if (value.data && value.u8_bytes) {
        core_string_t s = cmdr_intern(value);
        parse_token_set_value(tok, core_option_set(core_string_t, s), true);
    }

    parse_token_set_tid(tok, (int32_t)tid);
    parse_token_set_index(tok, index);
    parse_token_set_position(tok, 1, 1, 1,
                             value.data ? (uint32_t)value.u8_bytes + 1 : 1);
    parse_token_set_origin(tok, PARSE_TOKEN_SOURCE);

    return tok;
}

static parse_cmdr_command_t *
command_in_args(parse_cmdr_t *c, const core_string_t *argv, int argc,
                int *command_index)
{
    *command_index = -1;
    for (int i = 0; i < argc; i++) {
        core_string_t arg = argv[i];
        if (!arg.data) continue;
        if (core_string_eq(arg, CORE_STRING_STATIC("--"))) break;
        if (arg.u8_bytes && arg.data[0] == '-') continue;
        for (int32_t si = 0; si < (int32_t)c->root.subcommands.len; si++) {
            parse_cmdr_command_t *cmd = &c->root.subcommands.data[si];
            if (cmd->name.data && core_string_eq(arg, cmd->name)) {
                *command_index = i;
                return cmd;
            }
        }
    }
    return &c->root;
}

static core_string_t *
move_leading_flags_after_command(parse_cmdr_command_t *scope,
                                 parse_cmdr_t *c,
                                 const core_string_t *argv, int argc,
                                 int command_index)
{
    if (!scope || !scope->name.data || !argv || argc < 2) return nullptr;
    if (command_index <= 0) return nullptr;
    for (int i = 0; i < command_index; i++) {
        if (!argv[i].data || !argv[i].u8_bytes || argv[i].data[0] != '-')
            return nullptr;
        if (core_string_find_byte(argv[i], '=') >= 0) continue;
        parse_cmdr_flag_spec_t *flag = find_flag_scoped(c, scope, argv[i]);
        if (flag && flag->takes_value) {
            if (i + 1 >= command_index) return nullptr;
            i++;
        }
    }

    core_string_t *ordered = core_alloc_array(core_string_t, argc);
    ordered[0] = argv[command_index];
    for (int i = 0; i < command_index; i++) ordered[i + 1] = argv[i];
    for (int i = command_index + 1; i < argc; i++) ordered[i] = argv[i];
    return ordered;
}

static int32_t
cmdr_tokenize(const core_string_t *argv, int argc, parse_cmdr_t *c,
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
        core_string_t arg = argv[i];

        if (!arg.data) {
            continue;
        }

        if (!past_dd && core_string_eq(arg, CORE_STRING_STATIC("--"))) {
            past_dd = true;
            core_list_push(tl, make_token(CORE_STRING_STATIC("--"),
                                           c->tok_ids[PARSE_CMDR_TID_DD],
                                           (int32_t)tl.len));
            continue;
        }

        if (past_dd) {
            core_list_push(tl, make_token(arg, classify_word(c, arg),
                                           (int32_t)tl.len));
            continue;
        }

        if (core_string_starts_with(arg, CORE_STRING_STATIC("--"))
            && arg.u8_bytes > 2) {
            int64_t eq = core_string_find_byte(arg, '=');

            if (eq >= 0) {
                core_string_t flag_name = core_string_slice(arg, 0,
                                                            (size_t)eq);

                int64_t ftid = find_flag_tid(c, scope, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[PARSE_CMDR_TID_FLAG];
                }

                core_list_push(tl, make_token(flag_name, ftid, (int32_t)tl.len));

                core_list_push(tl, make_token(CORE_STRING_STATIC("="),
                                               c->tok_ids[PARSE_CMDR_TID_EQ],
                                               (int32_t)tl.len));

                core_string_t val = core_string_slice(
                    arg, (size_t)eq + 1, arg.u8_bytes - (size_t)eq - 1);
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

        if (arg.u8_bytes > 1 && arg.data[0] == '-'
            && !isdigit((unsigned char)arg.data[1])) {
            int64_t ftid = find_flag_tid(c, scope, arg);

            if (ftid) {
                core_list_push(tl, make_token(arg, ftid, (int32_t)tl.len));
                continue;
            }

            bool can_split = false;

            if (arg.u8_bytes > 2) {
                can_split = true;

                for (size_t j = 1; j < arg.u8_bytes; j++) {
                    char          short_flag[2] = {'-', arg.data[j]};
                    core_string_t short_name =
                        core_string_borrow(short_flag, sizeof(short_flag));
                    int64_t       stid = find_flag_tid(c, scope, short_name);

                    if (!stid || flag_takes_value(c, scope, short_name)) {
                        can_split = false;
                        break;
                    }
                }
            }

            if (can_split) {
                for (size_t j = 1; j < arg.u8_bytes; j++) {
                    char          short_flag[2] = {'-', arg.data[j]};
                    core_string_t short_name =
                        core_string_borrow(short_flag, sizeof(short_flag));
                    int64_t       stid = find_flag_tid(c, scope, short_name);
                    core_list_push(tl, make_token(short_name, stid,
                                                  (int32_t)tl.len));
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

        parse_cmdr_command_t *word_command = find_command(c, arg);
        if (word_command && word_command != &c->root)
            tid = parse_register_terminal(c->grammar, word_command->name);

        core_list_push(tl, make_token(arg, tid, (int32_t)tl.len));
    }

    core_list_push(tl, make_token(CORE_STRING_STATIC(""), PARSE_TOK_EOF,
                                  (int32_t)tl.len));

    *tokens_out   = tl.data;
    *n_tokens_out = (int32_t)tl.len;

    return 0;
}

static void
record_error(parse_cmdr_result_t *r, parse_cmdr_error_t detail,
             core_string_t message)
{
    r->ok = false;
    if (!r->errors.data) r->errors = core_list_new(core_string_t);
    if (!r->details.data) r->details = core_list_new(parse_cmdr_error_t);
    detail.flag = detail.flag.data ? cmdr_intern(detail.flag)
                                   : (core_string_t){0};
    detail.value = detail.value.data ? cmdr_intern(detail.value)
                                     : (core_string_t){0};
    core_list_push(r->details, detail);
    core_list_push(r->errors, cmdr_intern(message));
}

static parse_cmdr_result_t *
make_error_result(core_string_t msg)
{
    parse_cmdr_result_t *r = core_alloc(parse_cmdr_result_t);
    core_dict_init_string(&r->flags);
    core_dict_init_string(&r->repeated_flags);
    record_error(r, (parse_cmdr_error_t){.kind = PARSE_CMDR_ERROR_SYNTAX},
                 msg);

    return r;
}

static core_string_t
token_text(parse_token_info_t *token)
{
    core_option_t(core_string_t) value = parse_token_value(token);
    return core_option_is_set(value) ? core_option_get(value)
                                     : (core_string_t){0};
}

static bool
flag_token(parse_cmdr_t *c, parse_token_info_t *token)
{
    int64_t tid = parse_token_tid(token);
    if (tid == c->tok_ids[PARSE_CMDR_TID_FLAG]) return true;
    if (tid == c->tok_ids[PARSE_CMDR_TID_INT]
        || tid == c->tok_ids[PARSE_CMDR_TID_FLOAT]
        || tid == c->tok_ids[PARSE_CMDR_TID_DD] || tid == PARSE_TOK_EOF)
        return false;
    core_string_t text = token_text(token);
    return text.u8_bytes > 1 && text.data[0] == '-';
}

static void
report_flag_error(parse_cmdr_result_t *r, parse_cmdr_error_kind_t kind,
                  core_string_t flag, core_string_t value)
{
    core_buffer_t message = {0};
    switch (kind) {
    case PARSE_CMDR_ERROR_UNKNOWN_FLAG:
        core_buffer_printf(&message, "unknown option '%.*s'",
                           (int)flag.u8_bytes, flag.data);
        break;
    case PARSE_CMDR_ERROR_MISSING_VALUE:
        core_buffer_printf(&message, "option '%.*s' needs a value",
                           (int)flag.u8_bytes, flag.data);
        break;
    case PARSE_CMDR_ERROR_INVALID_INTEGER:
        core_buffer_printf(&message, "option '%.*s' takes an integer, not '%.*s'",
                           (int)flag.u8_bytes, flag.data,
                           (int)value.u8_bytes, value.data);
        break;
    case PARSE_CMDR_ERROR_INVALID_NUMBER:
        core_buffer_printf(&message, "option '%.*s' takes a number, not '%.*s'",
                           (int)flag.u8_bytes, flag.data,
                           (int)value.u8_bytes, value.data);
        break;
    default:
        core_buffer_printf(&message, "option '%.*s' does not take a value",
                           (int)flag.u8_bytes, flag.data);
        break;
    }
    record_error(r, (parse_cmdr_error_t){.kind = kind, .flag = flag,
                                         .value = value},
                 core_buffer_string(&message));
    core_buffer_release(&message);
}

static bool
integer_text(core_string_t text)
{
    int64_t value = 0;
    size_t consumed = 0;
    return text.u8_bytes && core_string_parse_i64(text, 10, &value, &consumed)
        && consumed == text.u8_bytes;
}

static bool
validate_flag_tokens(parse_cmdr_t *c, parse_cmdr_command_t *scope,
                     parse_token_info_t **tokens, int32_t count,
                     parse_cmdr_result_t *r)
{
    int64_t eq_tid = c->tok_ids[PARSE_CMDR_TID_EQ];
    for (int32_t i = 0; i < count; i++) {
        int64_t tid = parse_token_tid(tokens[i]);
        if (tid == c->tok_ids[PARSE_CMDR_TID_DD]) break;
        if (!flag_token(c, tokens[i])) continue;
        core_string_t text = token_text(tokens[i]);
        parse_cmdr_flag_spec_t *flag = tid == c->tok_ids[PARSE_CMDR_TID_FLAG]
            ? nullptr : find_flag_scoped(c, scope, text);
        if (!flag) {
            if (tid == c->tok_ids[PARSE_CMDR_TID_FLAG])
                report_flag_error(r, PARSE_CMDR_ERROR_UNKNOWN_FLAG, text,
                                  (core_string_t){0});
            if (i + 1 < count && parse_token_tid(tokens[i + 1]) == eq_tid)
                i += 2;
            continue;
        }
        bool assigned = i + 1 < count
            && parse_token_tid(tokens[i + 1]) == eq_tid;
        if (!flag->takes_value) {
            if (assigned) {
                report_flag_error(r, PARSE_CMDR_ERROR_UNEXPECTED_VALUE, text,
                                  token_text(tokens[i + 2]));
                i += 2;
            }
            continue;
        }
        int32_t at = assigned ? i + 2 : i + 1;
        int64_t value_tid = at < count ? parse_token_tid(tokens[at])
                                       : PARSE_TOK_EOF;
        core_string_t value = at < count ? token_text(tokens[at])
                                         : (core_string_t){0};
        bool missing = value_tid == PARSE_TOK_EOF
            || value_tid == c->tok_ids[PARSE_CMDR_TID_DD]
            || (!assigned && at < count && flag_token(c, tokens[at]))
            || !value.u8_bytes;
        if (missing) {
            report_flag_error(r, PARSE_CMDR_ERROR_MISSING_VALUE, text,
                              (core_string_t){0});
            if (assigned) i = at;
            continue;
        }
        if (flag->value_type == PARSE_CMDR_TYPE_INT && !integer_text(value))
            report_flag_error(r, PARSE_CMDR_ERROR_INVALID_INTEGER, text,
                              value);
        else if (flag->value_type == PARSE_CMDR_TYPE_FLOAT
                 && value_tid != c->tok_ids[PARSE_CMDR_TID_FLOAT]
                 && !integer_text(value))
            report_flag_error(r, PARSE_CMDR_ERROR_INVALID_NUMBER, text,
                              value);
        i = at;
    }
    return r->errors.len == 0;
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
        _core_dict_put(&result->repeated_flags,
                       core_dict_string_key_copy(name), values);
    }
    core_list_push(*values, clone_flag_value(value));
}

static void
store_flag_value(parse_cmdr_result_t *result, core_string_t name,
                 parse_cmdr_val_t *value)
{
    core_string_t *key = core_dict_string_key_copy(name);
    parse_cmdr_val_t *old = _core_dict_put(&result->flags, key, value);
    if (old) {
        core_dict_string_key_free(key);
        if (old->tag == PARSE_CMDR_VAL_STR) core_free(old->s.data);
        core_free(old);
    }
}

static core_string_t
get_token_text(parse_tree_t *node)
{
    if (!node || !core_tree_is_leaf(node)) {
        return (core_string_t){};
    }

    parse_token_info_t *tok = core_tree_leaf_value(node);

    if (!tok) {
        return (core_string_t){};
    }

    core_option_t(core_string_t) value = parse_token_value(tok);

    if (!core_option_is_set(value)) {
        return (core_string_t){};
    }

    return core_option_get(value);
}

static bool
collect_terminal_text(parse_visit_t *v)
{
    core_string_t text = get_token_text(v->node);

    if (text.data && text.u8_bytes) {
        core_list_t(core_string_t) *texts = v->user;

        core_list_push(*texts, text);
    }

    return true;
}

static void
extract_result_from_tree(parse_cmdr_t *c, parse_tree_t *tree,
                          parse_cmdr_result_t *r,
                          parse_cmdr_command_t *scope)
{
    if (!tree || !r) {
        return;
    }

    core_list_t(core_string_t) texts = core_list_new(core_string_t);
    parse_tree_walk_t         *w     = parse_tree_walk_new(c->grammar, &texts);

    parse_walk_on_leaf(w, collect_terminal_text, nullptr);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    int32_t n = (int32_t)texts.len;
    if (!r->command.data && scope && scope != &c->root)
        r->command = cmdr_intern(scope->name);
    bool past_dd = false;

    for (int32_t i = 0; i < n; i++) {
        core_string_t text = texts.data[i];

        if (!text.data || !text.u8_bytes) {
            continue;
        }

        if (core_string_eq(text, CORE_STRING_STATIC("--"))) {
            past_dd = true;
            goto next;
        }

        if (r->command.data && !past_dd
            && core_string_eq(text, r->command)) goto next;

        if (past_dd) {
            goto add_arg;
        }

        {
            parse_cmdr_command_t *cmd = r->command.data
                ? find_command(c, r->command) : &c->root;

            if (!cmd) {
                cmd = &c->root;
            }

            parse_cmdr_flag_spec_t *flag = find_flag(cmd, text);

            if (!flag) {
                flag = find_flag(&c->root, text);
            }

            if (flag) {
                parse_cmdr_val_t *v = core_alloc(parse_cmdr_val_t);

                if (flag->takes_value && i + 1 < n) {
                    if (i + 1 < n
                        && core_string_eq(texts.data[i + 1],
                                          CORE_STRING_STATIC("="))) {
                        i++;
                    }

                    if (i + 1 < n) {
                        i++;
                        core_string_t val = texts.data[i];

                        switch (flag->value_type) {
                        case PARSE_CMDR_TYPE_INT:
                            v->tag = PARSE_CMDR_VAL_INT;
                            v->i   = core_string_strtoll(val, 10);
                            break;
                        case PARSE_CMDR_TYPE_FLOAT:
                            v->tag = PARSE_CMDR_VAL_FLOAT;
                            v->f   = 0;
                            core_string_parse_f64(val, &v->f, nullptr);
                            break;
                        case PARSE_CMDR_TYPE_BOOL:
                            v->tag = PARSE_CMDR_VAL_BOOL;
                            v->b   = core_string_eq(val,
                                                    CORE_STRING_STATIC("true"))
                                  || core_string_eq(val,
                                                    CORE_STRING_STATIC("yes"));
                            break;
                        default:
                            v->tag = PARSE_CMDR_VAL_STR;
                            v->s   = core_string_dup(val);
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

        if (core_string_eq(text, CORE_STRING_STATIC("="))) {
            goto next;
        }

    add_arg:
        {
            core_list_push(r->args, ((parse_cmdr_arg_t){0}));

            parse_cmdr_arg_t *arg = &r->args.data[r->args.len - 1];
            arg->value     = core_string_dup(text);
            arg->int_val   = core_string_strtoll(text, 10);
            core_string_parse_f64(text, &arg->float_val, nullptr);
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

    core_buffer_t message = {0};
    parse_cmdr_error_t detail = {.given = r->args.len};
    if (r->args.len < min) {
        detail.kind = PARSE_CMDR_ERROR_TOO_FEW_ARGUMENTS;
        detail.limit = min;
        core_buffer_printf(&message, "expected at least %zu argument", min);
        if (min != 1) {
            core_buffer_putc(&message, 's');
        }
    }
    else {
        detail.kind = PARSE_CMDR_ERROR_TOO_MANY_ARGUMENTS;
        detail.limit = max;
        detail.value = r->args.data[max].value;
        core_buffer_printf(&message, "expected at most %zu argument", max);
        if (max != 1) {
            core_buffer_putc(&message, 's');
        }
    }
    record_error(r, detail, core_buffer_string(&message));
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
            parse_token_free(tokens[i]);
        }
    }

    core_free(tokens);
}

parse_cmdr_result_t *
parse_cmdr_parse(parse_cmdr_t *c, int nargs, const char **args)
{
    if (!c) {
        return make_error_result(
            CORE_STRING_STATIC("commander not initialized"));
    }

    if (!c->finalized) {
        parse_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result(CORE_STRING_STATIC("grammar not available"));
    }

    parse_token_info_t **tokens = nullptr;
    int32_t             n_tokens = 0;

    if (!args) {
        nargs = 0;
    }

    int            given = nargs > 0 ? nargs : 0;
    core_string_t *words = args ? core_alloc_array(core_string_t, given + 1)
                                : nullptr;

    for (int i = 0; i < given; i++) {
        words[i] = core_string_from_cstr(args[i]);
    }

    const core_string_t *argv = words;
    int command_index = -1;
    parse_cmdr_command_t *scope = command_in_args(c, argv, nargs,
                                                   &command_index);
    bool command_given = scope != &c->root;
    core_string_t *defaulted = nullptr;
    if (scope == &c->root && c->default_command.data
        && find_command(c, c->default_command) != &c->root) {
        defaulted = core_alloc_array(core_string_t, nargs + 1);
        defaulted[0] = c->default_command;
        for (int i = 0; i < nargs; i++) {
            defaulted[i + 1] = argv[i];
        }
        argv = defaulted;
        nargs++;
        scope = command_in_args(c, argv, nargs, &command_index);
    }
    core_string_t *ordered = move_leading_flags_after_command(scope, c, argv,
                                                              nargs,
                                                              command_index);
    const core_string_t *input = ordered ? ordered : argv;
    int32_t tokenized = cmdr_tokenize(input, nargs, c, scope, &tokens,
                                      &n_tokens);

    core_free(ordered);
    core_free(defaulted);
    for (int i = 0; i < given; i++) {
        core_free(words[i].data);
    }
    core_free(words);

    if (tokenized < 0) {
        return make_error_result(CORE_STRING_STATIC("tokenization failed"));
    }

    parse_cmdr_result_t *r = core_alloc(parse_cmdr_result_t);
    core_dict_init_string(&r->flags);
    core_dict_init_string(&r->repeated_flags);
    r->command_given = command_given;
    if (scope != &c->root) r->command = cmdr_intern(scope->name);

    if (!validate_flag_tokens(c, scope, tokens, n_tokens, r)) {
        free_tokens(tokens, n_tokens);
        return r;
    }

    parse_token_stream_t *ts = scan_token_stream_from_array(tokens, n_tokens);
    parse_pwz_parser_t   *pp
        = parse_pwz_new_for_start(c->grammar,
                                  parse_nonterm_id(parse_nonterm(
                                      c->grammar,
                                      CORE_STRING_STATIC("cmd"))));
    bool parse_ok = parse_pwz_parse(pp, ts);

    if (!parse_ok) {
        int32_t             at    = parse_pwz_error_pos(pp);
        parse_token_info_t *token = at >= 0 && at < n_tokens ? tokens[at] : nullptr;
        core_buffer_t       message = {0};
        core_option_t(core_string_t) token_value = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        if (core_option_is_set(token_value)) {
            core_string_t value = core_option_get(token_value);
            core_buffer_printf(&message, "unexpected argument '%.*s'",
                               (int)value.u8_bytes, value.data);
            if (scope && scope->name.data) {
                core_buffer_printf(&message, " for command '%.*s'",
                                   (int)scope->name.u8_bytes, scope->name.data);
            }
        }
        else {
            core_buffer_printf(&message, "parse failed");
        }
        record_error(r, (parse_cmdr_error_t){
                .kind = PARSE_CMDR_ERROR_SYNTAX,
                .value = core_option_is_set(token_value)
                    ? core_option_get(token_value) : (core_string_t){0},
            },
            core_buffer_string(&message));
        core_buffer_release(&message);
    }
    else {
        r->ok   = true;
        r->tree = parse_pwz_get_tree(pp);

        if (r->tree) {
            extract_result_from_tree(c, r->tree, r, scope);
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

parse_cmdr_error_t
parse_cmdr_result_error_detail(parse_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || (size_t)index >= r->details.len)
        return (parse_cmdr_error_t){.kind = PARSE_CMDR_ERROR_SYNTAX};
    return r->details.data[index];
}

bool
parse_cmdr_result_command_given(parse_cmdr_result_t *r)
{
    return r && r->command_given;
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
        return v->s.data ? core_string_strtoll(v->s, 10) : dflt;
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
        core_dict_string_key_free(e->key);

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
        core_dict_string_key_free(e->key);
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

    for (size_t i = 0; i < r->details.len; i++) {
        core_free(r->details.data[i].flag.data);
        core_free(r->details.data[i].value.data);
    }

    core_list_free(r->details);

    r->tree = nullptr;

    core_free(r);
}
