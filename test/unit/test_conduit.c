/*
 * test_conduit.c — Tests for conduit lifecycle and topic registry.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "core/alloc.h"
#include "core/runtime.h"

// ============================================================================
// 1. Conduit create / destroy
// ============================================================================

static void
test_conduit_create_destroy(void)
{
    n00b_result_t(n00b_conduit_t *) r = n00b_conduit_new();
    assert(n00b_result_is_ok(r));

    n00b_conduit_t *c = n00b_result_get(r);
    assert(c != nullptr);
    assert(!n00b_conduit_is_shutdown(c));

    n00b_conduit_destroy(c);
    assert(n00b_conduit_is_shutdown(c));

    printf("  [PASS] conduit create/destroy\n");
}

// ============================================================================
// 2. Topic get-or-create (integer URI)
// ============================================================================

static void
test_topic_get_or_create_int(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    // Create a topic for FD 42.
    n00b_result_t(n00b_conduit_topic_base_t *) tr1 = n00b_conduit_topic_for_fd(c, 42);
    assert(n00b_result_is_ok(tr1));
    n00b_conduit_topic_base_t *topic1 = n00b_result_get(tr1);
    assert(topic1 != nullptr);
    assert(n00b_conduit_topic_is_active(topic1));

    // Getting the same FD should return the same topic.
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 = n00b_conduit_topic_for_fd(c, 42);
    assert(n00b_result_is_ok(tr2));
    n00b_conduit_topic_base_t *topic2 = n00b_result_get(tr2);
    assert(topic1 == topic2);

    // Different FD should return a different topic.
    n00b_result_t(n00b_conduit_topic_base_t *) tr3 = n00b_conduit_topic_for_fd(c, 99);
    assert(n00b_result_is_ok(tr3));
    n00b_conduit_topic_base_t *topic3 = n00b_result_get(tr3);
    assert(topic3 != topic1);

    n00b_conduit_destroy(c);
    printf("  [PASS] topic get-or-create (int URI)\n");
}

// ============================================================================
// 3. Topic generation counter
// ============================================================================

static void
test_topic_generation(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_for_fd(c, 7);
    assert(n00b_result_is_ok(tr));
    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);

    uint64_t gen1 = n00b_conduit_topic_generation(topic);
    assert(gen1 > 0);

    // Creating another topic should get a different generation.
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 = n00b_conduit_topic_for_fd(c, 8);
    assert(n00b_result_is_ok(tr2));
    n00b_conduit_topic_base_t *topic2 = n00b_result_get(tr2);
    uint64_t gen2 = n00b_conduit_topic_generation(topic2);
    assert(gen2 != gen1);

    n00b_conduit_destroy(c);
    printf("  [PASS] topic generation counter\n");
}

// ============================================================================
// 4. Topic close and reactivation
// ============================================================================

static void
test_topic_close_reactivate(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_for_fd(c, 5);
    assert(n00b_result_is_ok(tr));
    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);
    uint64_t gen1 = n00b_conduit_topic_generation(topic);

    // Close the topic.
    uint64_t close_gen = n00b_conduit_topic_close(topic);
    assert(close_gen > gen1);
    assert(!n00b_conduit_topic_is_active(topic));

    // Getting the same FD should reactivate the topic with a new generation.
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 = n00b_conduit_topic_for_fd(c, 5);
    assert(n00b_result_is_ok(tr2));
    n00b_conduit_topic_base_t *topic2 = n00b_result_get(tr2);
    assert(topic2 == topic); // Same pointer, reactivated.
    assert(n00b_conduit_topic_is_active(topic2));
    uint64_t gen3 = n00b_conduit_topic_generation(topic2);
    assert(gen3 > gen1);

    n00b_conduit_destroy(c);
    printf("  [PASS] topic close and reactivation\n");
}

// ============================================================================
// 5. Topic set name
// ============================================================================

static void
test_topic_set_name(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_for_fd(c, 3);
    assert(n00b_result_is_ok(tr));
    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);

    n00b_result_t(bool) nr = n00b_conduit_topic_set_name(topic, "stdin");
    assert(n00b_result_is_ok(nr));

    n00b_conduit_destroy(c);
    printf("  [PASS] topic set name\n");
}

// ============================================================================
// 6. Topic set policy
// ============================================================================

static void
test_topic_set_policy(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_for_fd(c, 10);
    assert(n00b_result_is_ok(tr));
    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);

    n00b_conduit_topic_set_policy(topic, N00B_CONDUIT_POLICY_MUST_SERVE);
    // No assertion on reading policy back — just verify no crash.

    n00b_conduit_destroy(c);
    printf("  [PASS] topic set policy\n");
}

// ============================================================================
// 7. Shutdown prevents topic creation
// ============================================================================

static void
test_shutdown_prevents_topics(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_conduit_destroy(c);
    assert(n00b_conduit_is_shutdown(c));

    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_for_fd(c, 1);
    assert(n00b_result_is_err(tr));
    assert(n00b_result_get_err(tr) == N00B_CONDUIT_ERR_SHUTDOWN);

    printf("  [PASS] shutdown prevents topic creation\n");
}

// ============================================================================
// 8. Null arg handling
// ============================================================================

static void
test_null_arg_handling(void)
{
    n00b_result_t(n00b_conduit_topic_base_t *) tr = n00b_conduit_topic_get(nullptr, N00B_CONDUIT_URI_FD(0), 0);
    assert(n00b_result_is_err(tr));
    assert(n00b_result_get_err(tr) == N00B_CONDUIT_ERR_NULL_ARG);

    n00b_result_t(bool) nr = n00b_conduit_topic_set_name(nullptr, "test");
    assert(n00b_result_is_err(nr));

    // topic_close with null should return 0.
    assert(n00b_conduit_topic_close(nullptr) == 0);

    // topic_set_policy with null should not crash.
    n00b_conduit_topic_set_policy(nullptr, N00B_CONDUIT_POLICY_OPEN);

    printf("  [PASS] null arg handling\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_conduit:\n");
    fflush(stdout);

    test_conduit_create_destroy();
    fflush(stdout);
    test_topic_get_or_create_int();
    fflush(stdout);
    test_topic_generation();
    fflush(stdout);
    test_topic_close_reactivate();
    fflush(stdout);
    test_topic_set_name();
    fflush(stdout);
    test_topic_set_policy();
    fflush(stdout);
    test_shutdown_prevents_topics();
    fflush(stdout);
    test_null_arg_handling();
    fflush(stdout);

    printf("All conduit tests passed.\n");
    n00b_shutdown();
    return 0;
}
