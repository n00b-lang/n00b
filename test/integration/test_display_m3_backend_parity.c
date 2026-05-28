#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display_m3_parity_fixture.h"

static void
assert_expected_contract(const n00b_m3_parity_result_t *result)
{
    assert(result->resize_calls == 1);
    assert(result->resize_rows == 8);
    assert(result->resize_cols == 40);
    assert(result->left_key_events == 1);
    assert(result->right_key_events == 1);
    assert(result->left_activations == 0);
    assert(result->right_activations == 2);
    assert(result->left_mouse_presses == 0);
    assert(result->right_mouse_presses == 1);
    assert(result->cursor_hide);
    assert(result->cursor_show);
    assert(result->events_consumed + 1 == result->events_total);
    assert(result->next_key_after_stop == (uint32_t)'z');
}

static void
test_m3_backend_parity(void)
{
    n00b_m3_parity_result_t terminal = {};
    n00b_m3_parity_result_t gui = {};

    assert(n00b_m3_parity_run_case(false, &terminal) == 0);
    assert(n00b_m3_parity_run_case(true, &gui) == 0);

    assert_expected_contract(&terminal);
    assert_expected_contract(&gui);
    assert(n00b_m3_parity_equivalent(&terminal, &gui));

    printf("  [PASS] m3 synthetic GUI-profile parity\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display M3 synthetic parity integration test...\n");
    test_m3_backend_parity();

    printf("Display M3 synthetic parity integration test passed.\n");
    n00b_shutdown();
    return 0;
}
