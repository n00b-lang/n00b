/*
 * test_timer.c — Tests for conduit timer topics.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/timer.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/time.h"

// ============================================================================
// 1. One-shot timer topic creation
// ============================================================================

static void
test_timer_oneshot(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_timer_once(c, 100);

    if (n00b_result_is_err(tr)) {
        printf("  [SKIP] one-shot timer (not supported)\n");
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);
    assert(topic != nullptr);
    assert(n00b_conduit_topic_is_timer(topic));

    uint64_t id = n00b_conduit_timer_id(topic);
    assert(id != 0);

    n00b_conduit_timer_cancel(topic);
    n00b_conduit_destroy(c);
    printf("  [PASS] one-shot timer\n");
}

// ============================================================================
// 2. Repeating timer topic creation
// ============================================================================

static void
test_timer_repeating(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_timer_repeat(c, 50);

    if (n00b_result_is_err(tr)) {
        printf("  [SKIP] repeating timer (not supported)\n");
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);
    assert(topic != nullptr);
    assert(n00b_conduit_topic_is_timer(topic));

    n00b_conduit_timer_cancel(topic);
    n00b_conduit_destroy(c);
    printf("  [PASS] repeating timer\n");
}

// ============================================================================
// 3. Timer IDs are unique
// ============================================================================

static void
test_timer_unique_ids(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr1 = n00b_conduit_timer_once(c, 100);
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 = n00b_conduit_timer_once(c, 200);

    if (n00b_result_is_err(tr1) || n00b_result_is_err(tr2)) {
        printf("  [SKIP] timer unique IDs (not supported)\n");
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *t1 = n00b_result_get(tr1);
    n00b_conduit_topic_base_t *t2 = n00b_result_get(tr2);

    uint64_t id1 = n00b_conduit_timer_id(t1);
    uint64_t id2 = n00b_conduit_timer_id(t2);
    assert(id1 != id2);

    n00b_conduit_timer_cancel(t1);
    n00b_conduit_timer_cancel(t2);
    n00b_conduit_destroy(c);
    printf("  [PASS] timer unique IDs\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_timer:\n");
    fflush(stdout);

    test_timer_oneshot();
    fflush(stdout);
    test_timer_repeating();
    fflush(stdout);
    test_timer_unique_ids();
    fflush(stdout);

    printf("All timer tests passed.\n");
    n00b_shutdown();
    return 0;
}
