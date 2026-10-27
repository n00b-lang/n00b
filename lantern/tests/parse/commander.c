#include <stdio.h>

#include "parse/commander.h"
#include "harness.h"

static parse_cmdr_t *tool(void)
{
    static const parse_cmdr_flag_t global_flags[] = {
        {CORE_STRING_STATIC("--trace"), {0}, PARSE_CMDR_TYPE_BOOL, false,
         CORE_STRING_STATIC("enable tracing"), false, nullptr},
        {0},
    };
    static const parse_cmdr_flag_t build_flags[] = {
        {CORE_STRING_STATIC("--json"), {0}, PARSE_CMDR_TYPE_BOOL, false,
         CORE_STRING_STATIC("j"), false, nullptr},
        {CORE_STRING_STATIC("--out"), CORE_STRING_STATIC("-o"),
         PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("o"), false, nullptr},
        {0},
    };

    static const parse_cmdr_cmd_t commands[] = {
        {CORE_STRING_STATIC("build"), CORE_STRING_STATIC("build it"),
         build_flags, CORE_STRING_STATIC("file"), 1, 1, false},
        {CORE_STRING_STATIC("clean"), CORE_STRING_STATIC("clean it"),
         nullptr, {0}, 0, 0, false},
        {CORE_STRING_STATIC("library"), CORE_STRING_STATIC("build a library"),
         nullptr, CORE_STRING_STATIC("file"), 1, -1, false},
        {0},
    };

    static const parse_cmdr_spec_t spec = {
        .tool     = CORE_STRING_STATIC("tool"),
        .flags    = global_flags,
        .commands = commands,
    };

    return parse_cmdr_new(&spec);
}

static void test_a_command_and_its_arguments_come_back(void)
{
    parse_cmdr_t        *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 2,
                                          (const char *[]){"build", "a.c"});

    check("a subcommand parses", parse_cmdr_result_ok(r));
    check("and names itself",
          parse_cmdr_result_ok(r) && core_string_eq(parse_cmdr_result_command(r),
                                          CORE_STRING_STATIC("build")));
    check("and keeps its positional",
          parse_cmdr_arg_count(r) == 1
              && core_string_eq(parse_cmdr_arg_str(r, 0),
                                CORE_STRING_STATIC("a.c")));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_flags_in_every_spelling(void)
{
    static const struct {
        core_string_t what;
        int           n;
        const char   *av[4];
    } cases[] = {
        {CORE_STRING_STATIC("a boolean flag"), 3, {"build", "--json", "a.c"}},
        {CORE_STRING_STATIC("a valued flag"), 4, {"build", "--out", "x", "a.c"}},
        {CORE_STRING_STATIC("the --flag=value form"), 3,
         {"build", "--out=x", "a.c"}},
        {CORE_STRING_STATIC("a short alias"), 4, {"build", "-o", "x", "a.c"}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        parse_cmdr_t        *c = tool();
        parse_cmdr_result_t *r = parse_cmdr_parse(c, cases[i].n,
                                              (const char **)cases[i].av);

        if (!parse_cmdr_result_ok(r)) {
            printf("FAIL  %.*s does not parse\n", (int)cases[i].what.u8_bytes,
                   cases[i].what.data);
            fails++;
        }
        else if (parse_cmdr_arg_count(r) != 1
                 || !core_string_eq(parse_cmdr_arg_str(r, 0),
                                  CORE_STRING_STATIC("a.c"))) {
            printf("FAIL  %.*s swallows the positional\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data);
            fails++;
        }

        parse_cmdr_result_free(r);
        parse_cmdr_free(c);
    }

    parse_cmdr_t        *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 4,
        (const char *[]){"build", "--out", "x", "a.c"});

    check("a valued flag keeps its value",
          parse_cmdr_result_ok(r)
              && core_string_eq(parse_cmdr_flag_str(r,
                                                 CORE_STRING_STATIC("--out")),
                               CORE_STRING_STATIC("x")));
    check("and a flag that was not given is absent",
          !parse_cmdr_flag_present(r, CORE_STRING_STATIC("--json")));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_an_undeclared_flag_is_an_error(void)
{
    parse_cmdr_t        *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 3,
        (const char *[]){"build", "--nope", "a.c"});

    check("a flag the command never declared fails the parse", !parse_cmdr_result_ok(r));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_trailing_junk_is_an_error(void)
{
    parse_cmdr_t        *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 2,
        (const char *[]){"nosuchcommand", "a.c"});

    check("an unknown command fails the parse", !parse_cmdr_result_ok(r));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_a_double_dash_passes_words_through(void)
{
    parse_cmdr_t        *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 3,
        (const char *[]){"build", "--", "a.c"});

    check("everything after -- is a word, not a flag", parse_cmdr_result_ok(r));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_positional_minimum_and_maximum_are_enforced(void)
{
    parse_cmdr_t *c = tool();
    parse_cmdr_result_t *too_few = parse_cmdr_parse(c, 1,
                                                    (const char *[]) {"build"});
    check("a required positional cannot be omitted",
          !parse_cmdr_result_ok(too_few));
    check("the positional error is available to the caller",
          parse_cmdr_result_error_count(too_few) == 1
              && core_string_eq(parse_cmdr_result_error(too_few, 0),
                                CORE_STRING_STATIC("expected at least 1 argument")));
    parse_cmdr_result_free(too_few);

    parse_cmdr_result_t *too_many = parse_cmdr_parse(c, 3,
        (const char *[]) {"build", "a.c", "b.c"});
    check("a bounded positional rejects extras",
          !parse_cmdr_result_ok(too_many)
              && core_string_eq(parse_cmdr_result_error(too_many, 0),
                                CORE_STRING_STATIC("expected at most 1 argument")));
    parse_cmdr_result_free(too_many);

    parse_cmdr_result_t *many = parse_cmdr_parse(c, 4,
        (const char *[]) {"library", "a.c", "b.c", "c.c"});
    check("a negative maximum allows any number above the minimum",
          parse_cmdr_result_ok(many) && parse_cmdr_arg_count(many) == 3);
    parse_cmdr_result_free(many);
    parse_cmdr_free(c);
}

static void test_global_flags_are_inherited_by_commands(void)
{
    parse_cmdr_t *c = tool();
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 3,
        (const char *[]) {"build", "--trace", "a.c"});
    check("a root flag is accepted after a subcommand",
          parse_cmdr_result_ok(r)
              && parse_cmdr_flag_present(r, CORE_STRING_STATIC("--trace")));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_flags_before_commands_and_command_scope_precedence(void)
{
    parse_cmdr_t *tool_cli = tool();
    parse_cmdr_result_t *valued = parse_cmdr_parse(tool_cli, 4,
        (const char *[]) {"--out", "artifact", "build", "a.c"});
    check("a leading valued option moves with its value",
          parse_cmdr_result_ok(valued)
              && core_string_eq(parse_cmdr_flag_str(
                    valued, CORE_STRING_STATIC("--out")),
                                CORE_STRING_STATIC("artifact")));
    parse_cmdr_result_free(valued);
    parse_cmdr_free(tool_cli);

    static const parse_cmdr_flag_t root_flags[] = {
        {CORE_STRING_STATIC("--scope"), {0}, PARSE_CMDR_TYPE_WORD, true, {0}, false,
         nullptr},
        {0},
    };
    static const parse_cmdr_flag_t local_flags[] = {
        {CORE_STRING_STATIC("--scope"), {0}, PARSE_CMDR_TYPE_BOOL, false, {0}, false,
         nullptr},
        {0},
    };
    static const parse_cmdr_cmd_t commands[] = {
        {CORE_STRING_STATIC("build"), {0}, local_flags,
         CORE_STRING_STATIC("file"), 1, 1, false},
        {0},
    };
    static const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("tool"), .flags = root_flags,
        .commands = commands,
    };

    parse_cmdr_t *c = parse_cmdr_new(&spec);
    parse_cmdr_result_t *leading = parse_cmdr_parse(c, 3,
        (const char *[]) {"--scope", "build", "a.c"});
    check("a command flag is accepted before its command",
          parse_cmdr_result_ok(leading)
              && parse_cmdr_flag_present(leading, CORE_STRING_STATIC("--scope"))
              && !parse_cmdr_flag_str(leading,
                                      CORE_STRING_STATIC("--scope")).data);
    parse_cmdr_result_free(leading);

    parse_cmdr_result_t *local = parse_cmdr_parse(c, 3,
        (const char *[]) {"build", "--scope", "a.c"});
    check("the command definition wins even when it conflicts with the root",
          parse_cmdr_result_ok(local)
              && parse_cmdr_flag_present(local, CORE_STRING_STATIC("--scope"))
              && !parse_cmdr_flag_str(local,
                                      CORE_STRING_STATIC("--scope")).data);
    parse_cmdr_result_free(local);

    parse_cmdr_result_t *root = parse_cmdr_parse(c, 2,
        (const char *[]) {"--scope", "root-value"});
    check("the root definition applies when no command is selected",
          parse_cmdr_result_ok(root)
              && core_string_eq(parse_cmdr_flag_str(root,
                                   CORE_STRING_STATIC("--scope")),
                                CORE_STRING_STATIC("root-value")));
    parse_cmdr_result_free(root);
    parse_cmdr_free(c);
}

static void test_repeatable_flags_preserve_all_values(void)
{
    static const parse_cmdr_flag_t flags[] = {
        {CORE_STRING_STATIC("--define"), CORE_STRING_STATIC("-D"),
         PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("definition"), false,
         nullptr},
        {0},
    };
    static const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("tool"), .flags = flags,
        .arg = CORE_STRING_STATIC("file"), .min_args = 1, .max_args = 1,
    };
    parse_cmdr_t *c = parse_cmdr_new(&spec);
    check("a repeatable flag can be configured",
          parse_cmdr_flag_set_repeatable(c, (core_string_t){0},
              CORE_STRING_STATIC("--define"), true));
    parse_cmdr_result_t *r = parse_cmdr_parse(c, 5,
        (const char *[]) {"-D", "A=1", "--define", "B=2", "unit.n00b"});
    check("repeatable flags retain the number of values",
          parse_cmdr_result_ok(r)
              && parse_cmdr_flag_value_count(r, CORE_STRING_STATIC("--define")) == 2);
    check("repeatable flags retain values in command-line order",
          core_string_eq(parse_cmdr_flag_str_at(r,
              CORE_STRING_STATIC("--define"), 0), CORE_STRING_STATIC("A=1"))
              && core_string_eq(parse_cmdr_flag_str_at(r,
                  CORE_STRING_STATIC("--define"), 1), CORE_STRING_STATIC("B=2")));
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);
}

static void test_a_bare_invocation_runs_the_default_command(void)
{
    static const parse_cmdr_flag_t build_flags[] = {
        {CORE_STRING_STATIC("--out"), CORE_STRING_STATIC("-o"),
         PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("o"), false, nullptr},
        {0},
    };
    static const parse_cmdr_cmd_t commands[] = {
        {CORE_STRING_STATIC("build"), CORE_STRING_STATIC("build it"),
         build_flags, CORE_STRING_STATIC("file"), 1, 1, false},
        {CORE_STRING_STATIC("clean"), CORE_STRING_STATIC("clean it"),
         nullptr, {0}, 0, 0, false},
        {0},
    };
    static const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("tool"), .commands = commands,
        .default_command = CORE_STRING_STATIC("build"),
    };
    parse_cmdr_t *c = parse_cmdr_new(&spec);
    parse_cmdr_result_t *bare = parse_cmdr_parse(c, 3,
        (const char *[]) {"-o", "x", "a.c"});
    check("a bare invocation parses as the default command",
          parse_cmdr_result_ok(bare)
              && core_string_eq(parse_cmdr_result_command(bare),
                                CORE_STRING_STATIC("build"))
              && core_string_eq(parse_cmdr_flag_str(bare,
                                    CORE_STRING_STATIC("--out")),
                                CORE_STRING_STATIC("x")));
    parse_cmdr_result_free(bare);
    parse_cmdr_result_t *other = parse_cmdr_parse(c, 3,
        (const char *[]) {"clean", "-o", "x"});
    check("another command does not accept the default command's flags",
          !parse_cmdr_result_ok(other));
    parse_cmdr_result_free(other);
    parse_cmdr_free(c);
}

static void test_integer_options_reject_words(void)
{
    static const parse_cmdr_flag_t flags[] = {
        {CORE_STRING_STATIC("--keep"), CORE_STRING_STATIC("-k"),
         PARSE_CMDR_TYPE_INT, true, CORE_STRING_STATIC("count"), false,
         nullptr},
        {0},
    };
    static const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("tool"), .flags = flags,
        .arg = CORE_STRING_STATIC("file"), .min_args = 1, .max_args = 1,
    };
    parse_cmdr_t *c = parse_cmdr_new(&spec);
    parse_cmdr_result_t *word = parse_cmdr_parse(c, 3,
        (const char *[]) {"--keep", "abc", "a.n"});
    parse_cmdr_error_t error = parse_cmdr_result_error_detail(word, 0);
    check("an integer option rejects a word",
          !parse_cmdr_result_ok(word)
              && error.kind == PARSE_CMDR_ERROR_INVALID_INTEGER
              && core_string_eq(error.value, CORE_STRING_STATIC("abc")));
    parse_cmdr_result_free(word);
    parse_cmdr_result_t *assigned = parse_cmdr_parse(c, 2,
        (const char *[]) {"--keep=4", "a.n"});
    check("an integer option accepts --flag=value",
          parse_cmdr_result_ok(assigned)
              && parse_cmdr_flag_int(assigned, CORE_STRING_STATIC("--keep"),
                                     0) == 4);
    parse_cmdr_result_free(assigned);
    parse_cmdr_result_t *missing = parse_cmdr_parse(c, 2,
        (const char *[]) {"a.n", "--keep"});
    check("a valued option at the end reports its missing value",
          parse_cmdr_result_error_detail(missing, 0).kind
              == PARSE_CMDR_ERROR_MISSING_VALUE);
    parse_cmdr_result_free(missing);
    parse_cmdr_free(c);
}

int main(void)
{
    test_integer_options_reject_words();
    test_a_command_and_its_arguments_come_back();
    test_flags_in_every_spelling();
    test_an_undeclared_flag_is_an_error();
    test_trailing_junk_is_an_error();
    test_a_double_dash_passes_words_through();
    test_positional_minimum_and_maximum_are_enforced();
    test_global_flags_are_inherited_by_commands();
    test_flags_before_commands_and_command_scope_precedence();
    test_repeatable_flags_preserve_all_values();
    test_a_bare_invocation_runs_the_default_command();

    return report("fe commander");
}
