#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display_terminal_replay_fixture.h"

static void
test_m2_terminal_flow(void)
{
    n00b_display_terminal_replay_summary_t summary = {};

    assert(n00b_display_terminal_replay_run(&summary) == 0);
    assert(summary.poll_calls >= 8);
    assert(summary.render_calls >= 1);
    assert(summary.cursor_hide);
    assert(summary.cursor_show);
    assert(summary.resize_calls == 1);
    assert(summary.resize_rows == 8);
    assert(summary.resize_cols == 40);
    assert(summary.left_key_events == 1);
    assert(summary.right_key_events == 1);
    assert(summary.right_activations == 1);
    assert(summary.right_mouse_presses == 1);

    printf("  [PASS] m2 terminal flow\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display M2 terminal-flow integration test...\n");
    test_m2_terminal_flow();

    printf("Display M2 terminal-flow integration test passed.\n");
    n00b_shutdown();
    return 0;
}
