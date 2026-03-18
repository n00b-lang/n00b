#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display_m4_showcase_fixture.h"
#include "text/strings/string_ops.h"

static void
test_m4_widget_table_flow(void)
{
    n00b_display_m4_showcase_summary_t summary = {};

    assert(n00b_display_m4_showcase_run(&summary) == 0);
    assert(summary.enter_handled);
    assert(summary.checkbox_click_handled);
    assert(summary.button_clicks == 1);
    assert(summary.status_is_run);
    assert(summary.checkbox_checked);
    assert(summary.showcase_stream != nullptr);
    assert(n00b_unicode_str_contains(summary.showcase_stream, r"status=run"));
    assert(n00b_unicode_str_contains(summary.showcase_stream, r"Component"));
    assert(n00b_unicode_str_contains(summary.showcase_stream, r"00000000"));

    printf("  [PASS] display m4 widget/table/hexdump flow\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display m4 widget/table flow integration test...\n");
    test_m4_widget_table_flow();
    printf("Display m4 widget/table flow integration test passed.\n");

    n00b_shutdown();
    return 0;
}
