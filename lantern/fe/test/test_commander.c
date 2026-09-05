#include <stdio.h>
#include <string.h>

#include "parse/commander.h"

static int fails = 0;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static ncc_cmdr_t *tool(void)
{
    ncc_cmdr_t *c = ncc_cmdr_new();

    ncc_cmdr_set_name(c, "tool");
    ncc_cmdr_add_command(c, "build", "build it");
    ncc_cmdr_add_flag(c, "build", "--json", NCC_CMDR_TYPE_BOOL, false, "j");
    ncc_cmdr_add_flag(c, "build", "--out", NCC_CMDR_TYPE_WORD, true, "o");
    ncc_cmdr_add_flag_alias(c, "build", "--out", "-o");
    ncc_cmdr_add_positional(c, "build", "file", NCC_CMDR_TYPE_WORD, 1, 1);
    ncc_cmdr_add_command(c, "clean", "clean it");
    ncc_cmdr_finalize(c);

    return c;
}

static void test_a_command_and_its_arguments_come_back(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 2,
                                          (const char *[]){"build", "a.c"});

    check("a subcommand parses", r->ok);
    check("and names itself",
          r->ok && !strcmp(ncc_cmdr_result_command(r), "build"));
    check("and keeps its positional",
          ncc_cmdr_arg_count(r) == 1
              && !strcmp(ncc_cmdr_arg_str(r, 0), "a.c"));
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

        if (!r->ok) {
            printf("FAIL  %s does not parse\n", cases[i].what);
            fails++;
        }
        else if (ncc_cmdr_arg_count(r) != 1
                 || strcmp(ncc_cmdr_arg_str(r, 0), "a.c")) {
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
          r->ok && ncc_cmdr_flag_str(r, "--out")
              && !strcmp(ncc_cmdr_flag_str(r, "--out"), "x"));
    check("and a flag that was not given is absent",
          !ncc_cmdr_flag_present(r, "--json"));
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_an_undeclared_flag_is_an_error(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 3,
        (const char *[]){"build", "--nope", "a.c"});

    check("a flag the command never declared fails the parse", !r->ok);
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_trailing_junk_is_an_error(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 2,
        (const char *[]){"nosuchcommand", "a.c"});

    check("an unknown command fails the parse", !r->ok);
    ncc_cmdr_result_free(r);
    ncc_cmdr_free(c);
}

static void test_a_double_dash_passes_words_through(void)
{
    ncc_cmdr_t        *c = tool();
    ncc_cmdr_result_t *r = ncc_cmdr_parse(c, 3,
        (const char *[]){"build", "--", "a.c"});

    check("everything after -- is a word, not a flag", r->ok);
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

    if (!fails) {
        printf("fe commander: all checks passed\n");
    }

    return fails ? 1 : 0;
}
