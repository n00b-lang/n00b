// Phase 11 / auto_harden_test.c — typed translation.
//
// Source: ~/resharp-c/tests/auto_harden_test.c.  The original loaded a
// TOML fixture of (pattern, hardened[, fwd]) entries, compiled each
// pattern with default options, asserted regex_is_hardened (and, when
// present, regex_has_fwd_prefix) matched the expectation, and (when
// hardened was expected) cross-checked regex_find_all against the same
// pattern compiled with hardened=true forced on.  n00b-regex has no
// TOML reader; the cases below are embedded byte-identical from
// `test/data/regex/auto_harden.toml`.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"

#include "internal/regex/regex.h"
#include "internal/regex/accel.h"

// ---- Embedded fixture (byte-identical with auto_harden.toml) --------------

typedef struct test_case_t {
    const char *pattern;
    bool        hardened;
    bool        has_fwd;
    bool        check_fwd; // True iff the TOML entry had a `fwd` key.
} test_case_t;

static const test_case_t k_cases[] = {
    {.pattern = "\\[[^\\]]*\\]",                                                 .hardened = false, .has_fwd = false, .check_fwd = true},
    {.pattern = "[\\s\\S]+<Token>([^<]+)<\\/Token>[\\s\\S]+",                    .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "[\\s\\S]+(## Usage\\n[\\s\\S]*)\\n## [\\s\\S]+",                .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "[\\s\\S]+<Refresh_Token>([^<]+)<\\/Refresh_Token>[\\s\\S]+",    .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "^[\\s\\S]*foo[\\s\\S]*$",                                       .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "%(-?[0-9]+)?(\\.?[0-9]+)?([acdfmMnpr%])(\\{([^\\}]+)\\})?|([^%]+)", .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "[^:]*:\\/?\\/?([^?]*)",                                         .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "([^>]*-->)*",                                                   .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "([^$]*\\{@[^}]+\\})|.*$",                                       .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = ".*b|a",                                                         .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "a|_*b",                                                         .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "^[\\\\s\\\\S]*\\\\.js$",                                        .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "_*foo_*",                                                       .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "^([\\|\\s+\\S+]+\\s+\\|\\s*)$",                                 .hardened = true,  .has_fwd = false, .check_fwd = false},
    {.pattern = "x(a|_*b)",                                                      .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "Sherlock Holmes",                                               .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "Sherlock|Holmes|Watson",                                        .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "^\\s*(/)?([a-z]+)\\s*(?::([\\s\\S]*))?$",                       .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "\\n\\s*\\S[\\s\\S]*$",                                          .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "_*Holmes",                                                      .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "\\{_*\\}",                                                      .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "\\{[^{]*\\}",                                                   .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "^\\s*((?:[\\S\\s]*\\S)?)\\s*$",                                 .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "([\\s\\S]+) as ([\\s\\S]+)",                                    .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "([\\s\\S]+)\\.[\\s\\S]+",                                       .hardened = false, .has_fwd = false, .check_fwd = false},
    {.pattern = "<%[\\s\\S]*|[\\s\\S]*%>",                                       .hardened = true,  .has_fwd = false, .check_fwd = false},
};

// ---- Inputs used for the hardened cross-check (byte-identical) ------------

typedef struct byte_slice_t {
    const uint8_t *data;
    size_t         len;
} byte_slice_t;

static const byte_slice_t k_inputs[] = {
    {.data = (const uint8_t *)"",                .len = 0},
    {.data = (const uint8_t *)"aaaaaaaa",        .len = 8},
    {.data = (const uint8_t *)"abcdefg",         .len = 7},
    {.data = (const uint8_t *)"|  |\n| a |\n|  |", .len = 15},
};

// ---- Helpers --------------------------------------------------------------

static bool
match_list_equal(n00b_list_t(Match) *a, n00b_list_t(Match) *b)
{
    size_t na = n00b_list_len(*a);
    size_t nb = n00b_list_len(*b);
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; ++i) {
        Match ma = n00b_list_get(*a, i);
        Match mb = n00b_list_get(*b, i);
        if (ma.start != mb.start || ma.end != mb.end) {
            return false;
        }
    }
    return true;
}

// ---- The test -------------------------------------------------------------

static void
test_auto_harden(void)
{
    constexpr size_t n_cases  = sizeof(k_cases)  / sizeof(k_cases[0]);
    constexpr size_t n_inputs = sizeof(k_inputs) / sizeof(k_inputs[0]);

    for (size_t i = 0; i < n_cases; ++i) {
        const test_case_t *tc = &k_cases[i];

        Regex *re = regex_new(tc->pattern);
        assert(re != nullptr);

        bool got = regex_is_hardened(re);
        assert(got == tc->hardened);

        if (tc->check_fwd) {
            bool got_fwd = regex_has_fwd_prefix(re);
            assert(got_fwd == tc->has_fwd);
        }

        if (tc->hardened) {
            RegexOptions opts =
                regex_options_hardened(regex_options_default(), true);
            Regex *hardened = regex_with_options(tc->pattern, opts);
            assert(hardened != nullptr);

            for (size_t k = 0; k < n_inputs; ++k) {
                const byte_slice_t *input = &k_inputs[k];

                n00b_list_t(Match) a = n00b_list_new_private(Match);
                n00b_list_t(Match) b = n00b_list_new_private(Match);

                n00b_regex_engine_err_t ea =
                    regex_find_all(re, input->data, input->len, &a);
                n00b_regex_engine_err_t eb =
                    regex_find_all(hardened, input->data, input->len, &b);

                assert(ea == N00B_REGEX_ENGINE_ERR_NONE);
                assert(eb == N00B_REGEX_ENGINE_ERR_NONE);
                assert(match_list_equal(&a, &b));

                n00b_list_free(a);
                n00b_list_free(b);
            }

            regex_free(hardened);
        }

        regex_free(re);
    }

    printf("  [PASS] auto_harden\n");
}

// ---- Test runner entry point ----------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex auto_harden tests...\n");
    test_auto_harden();
    printf("All regex auto_harden tests passed.\n");

    n00b_shutdown();
    return 0;
}
