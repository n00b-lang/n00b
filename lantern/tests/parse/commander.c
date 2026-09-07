#include <stdio.h>
#include <string.h>

#include "parse/commander.h"
#include "harness.h"

static parse_cmdr_t *tool(void)
{
    static const parse_cmdr_flag_t build_flags[] = {
        {CORE_STRING_STATIC("--json"), {0}, PARSE_CMDR_TYPE_BOOL, false,
         CORE_STRING_STATIC("j")},
        {CORE_STRING_STATIC("--out"), CORE_STRING_STATIC("-o"),
         PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("o")},
        {{0}, {0}, 0, false, {0}},
    };

    static const parse_cmdr_cmd_t commands[] = {
        {CORE_STRING_STATIC("build"), CORE_STRING_STATIC("build it"),
         build_flags, CORE_STRING_STATIC("file"), 1, 1},
        {CORE_STRING_STATIC("clean"), CORE_STRING_STATIC("clean it"),
         nullptr, {0}, 0, 0},
        {{0}, {0}, nullptr, {0}, 0, 0},
    };

    static const parse_cmdr_spec_t spec = {
        .tool     = CORE_STRING_STATIC("tool"),
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
        const char *what;
        int         n;
        const char *av[4];
    } cases[] = {
        {"a boolean flag",      3, {"build", "--json", "a.c"}},
        {"a valued flag",       4, {"build", "--out", "x", "a.c"}},
        {"the --flag=value form", 3, {"build", "--out=x", "a.c"}},
        {"a short alias",       4, {"build", "-o", "x", "a.c"}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        parse_cmdr_t        *c = tool();
        parse_cmdr_result_t *r = parse_cmdr_parse(c, cases[i].n,
                                              (const char **)cases[i].av);

        if (!parse_cmdr_result_ok(r)) {
            printf("FAIL  %s does not parse\n", cases[i].what);
            fails++;
        }
        else if (parse_cmdr_arg_count(r) != 1
                 || !core_string_eq(parse_cmdr_arg_str(r, 0),
                                  CORE_STRING_STATIC("a.c"))) {
            printf("FAIL  %s swallows the positional\n", cases[i].what);
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

int main(void)
{
    test_a_command_and_its_arguments_come_back();
    test_flags_in_every_spelling();
    test_an_undeclared_flag_is_an_error();
    test_trailing_junk_is_an_error();
    test_a_double_dash_passes_words_through();

    return report("fe commander");
}
