#include <stdio.h>
#include <string.h>

#include "parse/commander.h"
#include "ir_test.h"

static ncc_cmdr_t *tool(void)
{
    ncc_cmdr_t *c = ncc_cmdr_new();

    ncc_cmdr_set_name(c, NCC_STRING_STATIC("tool"));
    ncc_cmdr_add_command(c, NCC_STRING_STATIC("build"), NCC_STRING_STATIC("build it"));
    ncc_cmdr_add_flag(c, NCC_STRING_STATIC("build"), NCC_STRING_STATIC("--json"), NCC_CMDR_TYPE_BOOL, false, NCC_STRING_STATIC("j"));
    ncc_cmdr_add_flag(c, NCC_STRING_STATIC("build"), NCC_STRING_STATIC("--out"), NCC_CMDR_TYPE_WORD, true, NCC_STRING_STATIC("o"));
    ncc_cmdr_add_flag_alias(c, NCC_STRING_STATIC("build"), NCC_STRING_STATIC("--out"), NCC_STRING_STATIC("-o"));
    ncc_cmdr_add_positional(c, NCC_STRING_STATIC("build"), NCC_STRING_STATIC("file"), NCC_CMDR_TYPE_WORD, 1, 1);
    ncc_cmdr_add_command(c, NCC_STRING_STATIC("clean"), NCC_STRING_STATIC("clean it"));
    ncc_cmdr_finalize(c);

    return c;
}

static void test_a_command_and_its_arguments_come_back(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 2,
                                          (const char *[]){"build", "a.c"});

    check("a subcommand parses", ncc_cmdr_result_ok(r));
    check("and names itself",
          ncc_cmdr_result_ok(r) && ncc_string_eq(ncc_cmdr_result_command(r),
                                          NCC_STRING_STATIC("build")));
    check("and keeps its positional",
          ncc_cmdr_arg_count(r) == 1
              && ncc_string_eq(ncc_cmdr_arg_str(r, 0),
                                NCC_STRING_STATIC("a.c")));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
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
        ncc_cmdr_t        *c = tool();
        ncc_cmdr_result_t *r = ncc_cmdr_parse(c, cases[i].n,
                                              (const char **)cases[i].av);

        if (!ncc_cmdr_result_ok(r)) {
            printf("FAIL  %s does not parse\n", cases[i].what);
            fails++;
        }
        else if (ncc_cmdr_arg_count(r) != 1
                 || !ncc_string_eq(ncc_cmdr_arg_str(r, 0),
                                  NCC_STRING_STATIC("a.c"))) {
            printf("FAIL  %s swallows the positional\n", cases[i].what);
            fails++;
        }

        ncc_cmdr_result_free(r);
        ncc_cmdr_free(c);
    }

    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 4,
        (const char *[]){"build", "--out", "x", "a.c"});

    check("a valued flag keeps its value",
          ncc_cmdr_result_ok(r)
              && ncc_string_eq(ncc_cmdr_flag_str(r,
                                                 NCC_STRING_STATIC("--out")),
                               NCC_STRING_STATIC("x")));
    check("and a flag that was not given is absent",
          !ncc_cmdr_flag_present(r, NCC_STRING_STATIC("--json")));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_an_undeclared_flag_is_an_error(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 3,
        (const char *[]){"build", "--nope", "a.c"});

    check("a flag the command never declared fails the parse", !ncc_cmdr_result_ok(r));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_trailing_junk_is_an_error(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 2,
        (const char *[]){"nosuchcommand", "a.c"});

    check("an unknown command fails the parse", !ncc_cmdr_result_ok(r));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_a_double_dash_passes_words_through(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 3,
        (const char *[]){"build", "--", "a.c"});

    check("everything after -- is a word, not a flag", ncc_cmdr_result_ok(r));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
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
