#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "internal/display/table_text_primitives.h"
#include "text/unicode/properties.h"

static n00b_string_t *
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
}

static void
test_measure(void)
{
    n00b_string_t *text = make_str("alpha beta\ngamma");
    n00b_table_text_metrics_t metrics = n00b_table_text_measure(text);

    assert(metrics.longest_line == 10);
    assert(metrics.longest_word == 5);

    printf("  [PASS] table text measure\n");
}

static void
test_lines_for_width_wrap(void)
{
    n00b_string_t *text = make_str("alpha beta gamma");
    n00b_array_t(n00b_string_t *) lines =
        n00b_table_text_lines_for_width(text, 6, true);

    assert(n00b_array_len(lines) >= 2);

    for (size_t i = 0; i < n00b_array_len(lines); i++) {
        n00b_string_t *line = n00b_array_get(lines, i);
        assert(n00b_unicode_display_width(line) <= 6);
    }

    n00b_array_free(lines);
    printf("  [PASS] table text wrap lines\n");
}

static void
test_lines_for_width_truncate(void)
{
    n00b_string_t *text = make_str("abcdef\nxy");
    n00b_array_t(n00b_string_t *) lines =
        n00b_table_text_lines_for_width(text, 3, false);

    assert(n00b_array_len(lines) == 2);

    n00b_string_t *line0 = n00b_array_get(lines, 0);
    n00b_string_t *line1 = n00b_array_get(lines, 1);

    assert(n00b_unicode_display_width(line0) <= 3);
    assert(n00b_unicode_display_width(line1) <= 3);

    n00b_array_free(lines);
    printf("  [PASS] table text truncate lines\n");
}

static void
test_align_line(void)
{
    n00b_string_t *line = make_str("x");
    n00b_string_t *left = n00b_table_text_align_line(line, 3, N00B_ALIGN_LEFT);
    n00b_string_t *center = n00b_table_text_align_line(line, 3, N00B_ALIGN_CENTER);
    n00b_string_t *right = n00b_table_text_align_line(line, 3, N00B_ALIGN_RIGHT);

    assert(n00b_unicode_display_width(left) == 3);
    assert(n00b_unicode_display_width(center) == 3);
    assert(n00b_unicode_display_width(right) == 3);

    assert(left->data[0] == 'x');
    assert(center->u8_bytes >= 2 && center->data[1] == 'x');
    assert(right->u8_bytes >= 1 && right->data[right->u8_bytes - 1] == 'x');

    printf("  [PASS] table text align line\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display table text primitive tests...\n");

    test_measure();
    test_lines_for_width_wrap();
    test_lines_for_width_truncate();
    test_align_line();

    printf("Display table text primitive tests passed.\n");
    n00b_shutdown();
    return 0;
}
