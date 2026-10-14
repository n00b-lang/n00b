#include <stdio.h>
#include <string.h>

#include "resharp_writ.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static writ_resharp_regex *
compile(const char *pattern)
{
    writ_resharp_regex *regex = NULL;
    resharp_bytes_t failed = {0}, panicked = {0};
    int32_t got = resharp_regex_new(pattern, strlen(pattern), &regex, &failed, &panicked);
    expect(got == resharp_regex_new_ok, "the pattern compiles");
    resharp_bytes_free(&failed);
    resharp_bytes_free(&panicked);
    return regex;
}

static void
matching(void)
{
    writ_resharp_regex *digits = compile("[0-9]+");
    Match *spans = NULL;
    size_t count = 0;
    resharp_bytes_t failed = {0}, panicked = {0};
    const char *haystack = "a12b345c";
    int32_t got = resharp_regex_find_all(digits, (const uint8_t *)haystack, strlen(haystack), 1, &spans, &count,
                                         &failed, &panicked);
    expect(got == resharp_regex_find_all_ok, "find_all succeeds");
    expect(count == 2, "find_all retries until every match fits");
    expect(count == 2 && spans[0].start == 1 && spans[0].end == 3 && spans[1].start == 4 && spans[1].end == 7,
           "find_all reports the spans");
    resharp_free(spans);
    bool found = false;
    got = resharp_regex_is_match(digits, (const uint8_t *)"abc", 3, &found, &failed, &panicked);
    expect(got == resharp_regex_is_match_ok && !found, "is_match is false without digits");
    got = resharp_regex_is_match(digits, NULL, 0, &found, &failed, &panicked);
    expect(got == resharp_regex_is_match_ok && !found, "an empty haystack crosses as null");
    Match anchored = {0};
    got = resharp_regex_find_anchored(digits, (const uint8_t *)"42x", 3, &anchored, &failed, &panicked);
    expect(got == resharp_regex_find_anchored_ok && anchored.start == 0 && anchored.end == 2, "anchored match");
    got = resharp_regex_find_anchored(digits, (const uint8_t *)"x42", 3, &anchored, &failed, &panicked);
    expect(got == resharp_regex_find_anchored_none, "no anchored match is the none outcome");
    resharp_regex_free(digits);
}

static void
errors(void)
{
    writ_resharp_regex *regex = NULL;
    resharp_bytes_t failed = {0}, panicked = {0};
    int32_t got = resharp_regex_new("(", 1, &regex, &failed, &panicked);
    expect(got == resharp_regex_new_failed, "an unbalanced group fails");
    expect(failed.length > 0 && failed.data, "the failure carries the error's Display text");
    expect(regex == NULL, "a failed compile leaves the handle unset");
    resharp_bytes_free(&failed);
    got = resharp_regex_new("\xff", 1, &regex, &failed, &panicked);
    expect(got == resharp_regex_new_failed, "invalid UTF-8 is rejected before it reaches Rust");
    resharp_bytes_free(&failed);
}

static void
options(void)
{
    writ_resharp_regex_options *options = NULL, *folded = NULL;
    resharp_bytes_t failed = {0}, panicked = {0};
    expect(resharp_regex_options_default(&options, &failed, &panicked) == resharp_regex_options_default_ok,
           "default options");
    expect(resharp_regex_options_case_insensitive(options, true, &folded, &failed, &panicked)
               == resharp_regex_options_case_insensitive_ok,
           "builders consume and return the options");
    writ_resharp_regex *regex = NULL;
    expect(resharp_regex_with_options("abc", 3, folded, &regex, &failed, &panicked) == resharp_regex_with_options_ok,
           "with_options compiles");
    bool found = false;
    resharp_regex_is_match(regex, (const uint8_t *)"xABCx", 5, &found, &failed, &panicked);
    expect(found, "case-insensitive matching");
    resharp_regex_free(regex);
}

#ifdef RESHARP_CAPTURE_GROUPS
static void
captures(void)
{
    writ_resharp_regex *regex = compile("abc");
    size_t index = 0;
    resharp_bytes_t failed = {0}, panicked = {0};
    int32_t got = resharp_regex_capture_index_for_name(regex, "missing", 7, &index, &failed, &panicked);
    expect(got == resharp_regex_capture_index_for_name_none, "an unknown capture name is the none outcome");
    resharp_regex_free(regex);
}
#endif

int
main(void)
{
    matching();
    errors();
    options();
#ifdef RESHARP_CAPTURE_GROUPS
    captures();
#endif
    if (!failures) {
        printf("resharp roundtrip ok\n");
    }
    return failures != 0;
}
