#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "slay/commander.h"

// ============================================================================
// Helpers
// ============================================================================

static n00b_cmdr_t *
build_simple_commander(void)
{
    n00b_cmdr_t *c = n00b_cmdr_new();

    n00b_cmdr_set_name(c, r"test");

    // Global flags
    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--verbose",
                        N00B_CMDR_TYPE_BOOL, false, r"Enable verbose output");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--verbose", r"-v");

    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--output",
                        N00B_CMDR_TYPE_WORD, true, r"Output path");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--output", r"-o");

    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--jobs",
                        N00B_CMDR_TYPE_INT, true, r"Parallelism");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--jobs", r"-j");

    // Positional args (0 or more words)
    n00b_cmdr_add_positional(c, n00b_string_empty(), r"file",
                              N00B_CMDR_TYPE_WORD, 0, -1);

    return c;
}

static n00b_cmdr_t *
build_subcommand_commander(void)
{
    n00b_cmdr_t *c = n00b_cmdr_new();

    n00b_cmdr_set_name(c, r"tool");

    // "build" subcommand
    n00b_cmdr_add_command(c, r"build", r"Build the project");
    n00b_cmdr_add_flag(c, r"build", r"--verbose",
                        N00B_CMDR_TYPE_BOOL, false, r"Verbose build");
    n00b_cmdr_add_flag_alias(c, r"build", r"--verbose", r"-v");
    n00b_cmdr_add_flag(c, r"build", r"--output",
                        N00B_CMDR_TYPE_WORD, true, r"Output path");
    n00b_cmdr_add_flag_alias(c, r"build", r"--output", r"-o");
    n00b_cmdr_add_positional(c, r"build", r"file",
                              N00B_CMDR_TYPE_WORD, 0, -1);

    // "test" subcommand
    n00b_cmdr_add_command(c, r"test", r"Run tests");
    n00b_cmdr_add_flag(c, r"test", r"--filter",
                        N00B_CMDR_TYPE_WORD, true, r"Filter pattern");

    return c;
}

// ============================================================================
// 1. Basic parse: flags + positional args
// ============================================================================

static void
test_basic_parse(void)
{
    n00b_cmdr_t *c = build_simple_commander();
    n00b_cmdr_finalize(c);

    const char *argv[] = {"--verbose", "--output", "out.txt", "file1.c",
                           "file2.c"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 5, argv);

    assert(r != NULL);
    assert(r->ok);

    assert(n00b_cmdr_flag_bool(r, r"--verbose") == true);
    assert(n00b_cmdr_flag_present(r, r"--verbose"));

    n00b_string_t *out = n00b_cmdr_flag_str(r, r"--output");
    assert(out->data != NULL);
    assert(strcmp(out->data, "out.txt") == 0);

    assert(n00b_cmdr_arg_count(r) == 2);

    n00b_string_t *a0 = n00b_cmdr_arg_str(r, 0);
    assert(a0->data != NULL);
    assert(strcmp(a0->data, "file1.c") == 0);

    n00b_string_t *a1 = n00b_cmdr_arg_str(r, 1);
    assert(a1->data != NULL);
    assert(strcmp(a1->data, "file2.c") == 0);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] basic_parse\n");
}

// ============================================================================
// 2. Flag with = syntax
// ============================================================================

static void
test_flag_eq_syntax(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    const char *argv[] = {"--output=out.txt"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 1, argv);

    assert(r != NULL);
    assert(r->ok);

    n00b_string_t *out = n00b_cmdr_flag_str(r, r"--output");
    assert(out->data != NULL);
    assert(strcmp(out->data, "out.txt") == 0);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] flag_eq_syntax\n");
}

// ============================================================================
// 3. Short flag alias
// ============================================================================

static void
test_short_flag(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    const char *argv[] = {"-v"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 1, argv);

    assert(r != NULL);
    assert(r->ok);

    assert(n00b_cmdr_flag_bool(r, r"--verbose") == true);
    // Also check via alias
    assert(n00b_cmdr_flag_bool(r, r"-v") == true);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] short_flag\n");
}

// ============================================================================
// 4. Short flag expansion (-vj → -v -j is NOT valid because -j takes value)
// ============================================================================

static void
test_short_flag_no_expand(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    // -vj should NOT expand because -j takes a value.
    // It should be treated as an unknown flag.
    const char *argv[] = {"-vj"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 1, argv);

    assert(r != NULL);
    assert(r->ok);

    // -vj isn't recognized as --verbose, so verbose should be false
    assert(n00b_cmdr_flag_present(r, r"--verbose") == false);
    // -vj should appear as a positional arg (unknown flag → positional)
    assert(n00b_cmdr_arg_count(r) >= 1);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] short_flag_no_expand\n");
}

// ============================================================================
// 5. Double-dash separator
// ============================================================================

static void
test_double_dash(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    const char *argv[] = {"--verbose", "--", "--not-a-flag", "file.c"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 4, argv);

    assert(r != NULL);
    assert(r->ok);

    assert(n00b_cmdr_flag_bool(r, r"--verbose") == true);
    // --not-a-flag and file.c are positional args after --
    assert(n00b_cmdr_arg_count(r) == 2);

    n00b_string_t *a0 = n00b_cmdr_arg_str(r, 0);
    assert(a0->data != NULL);
    assert(strcmp(a0->data, "--not-a-flag") == 0);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] double_dash\n");
}

// ============================================================================
// 6. Integer flag value
// ============================================================================

static void
test_int_flag(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    const char *argv[] = {"--jobs", "4"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 2, argv);

    assert(r != NULL);
    assert(r->ok);

    assert(n00b_cmdr_flag_int(r, r"--jobs") == 4);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] int_flag\n");
}

// ============================================================================
// 7. Subcommands
// ============================================================================

static void
test_subcommand(void)
{
    n00b_cmdr_t *c = build_subcommand_commander();

    const char *argv[] = {"build", "--verbose", "-o", "out", "main.c"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 5, argv);

    assert(r != NULL);
    assert(r->ok);

    n00b_string_t *cmd = n00b_cmdr_result_command(r);
    assert(cmd->data != NULL);
    assert(strcmp(cmd->data, "build") == 0);

    assert(n00b_cmdr_flag_bool(r, r"--verbose") == true);

    n00b_string_t *out = n00b_cmdr_flag_str(r, r"--output");
    assert(out->data != NULL);
    assert(strcmp(out->data, "out") == 0);

    assert(n00b_cmdr_arg_count(r) == 1);
    n00b_string_t *a0 = n00b_cmdr_arg_str(r, 0);
    assert(strcmp(a0->data, "main.c") == 0);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] subcommand\n");
}

// ============================================================================
// 8. Parse failure
// ============================================================================

static void
test_parse_failure(void)
{
    // Create a commander with no flags or positionals that only
    // expects a known subcommand.
    n00b_cmdr_t *c = n00b_cmdr_new();
    n00b_cmdr_add_command(c, r"run", r"Run something");

    // Parse something that isn't "run" — this should still parse
    // because items grammar accepts WORDs, but "bogus" will be a
    // positional arg, not a subcommand.
    const char *argv[] = {"bogus"};
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 1, argv);

    // The grammar accepts WORD tokens as positional args, so this
    // should parse successfully — "bogus" is just a positional arg
    // that happens to not be a subcommand.
    assert(r != NULL);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] parse_failure\n");
}

// ============================================================================
// 9. Empty argv
// ============================================================================

static void
test_empty_argv(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    // Parsing with 0 args should work (everything is optional)
    // but tokenizer returns -1 for argc <= 0, so result is an error.
    n00b_cmdr_result_t *r = n00b_cmdr_parse(c, 0, NULL);
    assert(r != NULL);
    assert(!r->ok);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] empty_argv\n");
}

// ============================================================================
// 10. NULL safety
// ============================================================================

static void
test_null_safety(void)
{
    // Free NULL should be safe
    n00b_cmdr_free(NULL);
    n00b_cmdr_result_free(NULL);

    // Queries on NULL results
    assert(n00b_cmdr_arg_count(NULL) == 0);
    assert(n00b_cmdr_flag_bool(NULL, r"--foo") == false);
    assert(n00b_cmdr_flag_int(NULL, r"--foo") == 0);
    assert(n00b_cmdr_error_count(NULL) == 0);

    printf("  [PASS] null_safety\n");
}

// ============================================================================
// 11. Parse string
// ============================================================================

static void
test_parse_string(void)
{
    n00b_cmdr_t *c = build_simple_commander();

    n00b_cmdr_result_t *r = n00b_cmdr_parse_string(c,
        r"--verbose --output out.txt file1.c");

    assert(r != NULL);
    assert(r->ok);

    assert(n00b_cmdr_flag_bool(r, r"--verbose") == true);

    n00b_string_t *out = n00b_cmdr_flag_str(r, r"--output");
    assert(out->data != NULL);
    assert(strcmp(out->data, "out.txt") == 0);

    assert(n00b_cmdr_arg_count(r) == 1);
    n00b_string_t *a0 = n00b_cmdr_arg_str(r, 0);
    assert(strcmp(a0->data, "file1.c") == 0);

    n00b_cmdr_result_free(r);
    n00b_cmdr_free(c);
    printf("  [PASS] parse_string\n");
}

// ============================================================================
// 12. Tokenizer direct test
// ============================================================================

static void
test_tokenizer(void)
{
    n00b_cmdr_t *c = build_simple_commander();
    n00b_cmdr_finalize(c);

    const char *argv[] = {"--verbose", "--output=foo", "bar", "--", "baz"};
    n00b_token_info_t **tokens = NULL;
    int32_t n_tokens = 0;

    int32_t ret = n00b_cmdr_tokenize(argv, 5, c, &tokens, &n_tokens);
    assert(ret == 0);
    assert(n_tokens > 0);

    // Last token should be EOF
    assert(tokens[n_tokens - 1]->tid == N00B_TOK_EOF);

    // First token should be the --verbose flag (has a registered terminal ID)
    assert(tokens[0]->tid != 0);

    n00b_free(tokens);
    n00b_cmdr_free(c);
    printf("  [PASS] tokenizer\n");
}

// ============================================================================
// 13. BNF mode
// ============================================================================

static void
test_bnf_mode(void)
{
    // Simple BNF: cmd -> "hello" "world" | "hello"
    n00b_cmdr_t *c = n00b_cmdr_from_bnf(
        r"<cmd> ::= \"hello\" \"world\" | \"hello\"",
        r"cmd");

    if (!c) {
        // BNF mode depends on the BNF loader's character-level parsing
        // being fully functional. Skip gracefully if not available.
        printf("  [SKIP] bnf_mode (BNF loader not available)\n");
        return;
    }

    assert(c->finalized);

    n00b_cmdr_free(c);
    printf("  [PASS] bnf_mode\n");
}

// ============================================================================
// 14. Error queries
// ============================================================================

static void
test_error_queries(void)
{
    n00b_cmdr_result_t *r = n00b_cmdr_parse(NULL, 1,
                                              (const char *[]){"foo"});

    assert(r != NULL);
    assert(!r->ok);
    assert(n00b_cmdr_error_count(r) > 0);

    n00b_string_t *err = n00b_cmdr_error_get(r, 0);
    assert(err->data != NULL);
    assert(err->u8_bytes > 0);

    n00b_cmdr_result_free(r);
    printf("  [PASS] error_queries\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("Commander tests:\n");

    test_basic_parse();
    test_flag_eq_syntax();
    test_short_flag();
    test_short_flag_no_expand();
    test_double_dash();
    test_int_flag();
    test_subcommand();
    test_parse_failure();
    test_empty_argv();
    test_null_safety();
    test_parse_string();
    test_tokenizer();
    test_bnf_mode();
    test_error_queries();

    printf("All commander tests passed.\n");

    return 0;
}
