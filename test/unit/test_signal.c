/*
 * test_signal.c — Tests for conduit signal topics (Unix only).
 */

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/signal.h"
#include "core/alloc.h"
#include "core/runtime.h"

// ============================================================================
// 1. Create signal topic and verify
// ============================================================================

static void
test_signal_topic(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_signal_topic(c, SIGUSR1);

    if (n00b_result_is_err(tr)) {
        printf("  [SKIP] signal topic (not supported)\n");
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);
    assert(topic != nullptr);

    // Verify it is a signal topic.
    assert(n00b_conduit_topic_is_signal(topic));
    assert(n00b_conduit_signal_num(topic) == SIGUSR1);

    n00b_conduit_signal_unwatch(c, SIGUSR1);
    n00b_conduit_destroy(c);
    printf("  [PASS] signal topic\n");
}

// ============================================================================
// 2. Same signal returns same topic
// ============================================================================

static void
test_signal_same_topic(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr1 = n00b_conduit_signal_topic(c, SIGUSR2);
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 = n00b_conduit_signal_topic(c, SIGUSR2);

    if (n00b_result_is_err(tr1) || n00b_result_is_err(tr2)) {
        printf("  [SKIP] signal same topic (not supported)\n");
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *t1 = n00b_result_get(tr1);
    n00b_conduit_topic_base_t *t2 = n00b_result_get(tr2);
    assert(t1 == t2);

    n00b_conduit_signal_unwatch(c, SIGUSR2);
    n00b_conduit_destroy(c);
    printf("  [PASS] signal same topic\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_signal:\n");
    fflush(stdout);

    test_signal_topic();
    fflush(stdout);
    test_signal_same_topic();
    fflush(stdout);

    printf("All signal tests passed.\n");
    n00b_shutdown();
    return 0;
}
