/*
 * test_ijson.c — Tests for the incremental streaming JSON parser.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "parsers/ijson.h"
#include "core/runtime.h"

// ============================================================================
// Test event recorder
// ============================================================================

#define MAX_EVENTS 128

typedef struct {
    n00b_jparse_event_t events[MAX_EVENTS];
    uint64_t            offsets[MAX_EVENTS];
    int                 count;
} event_log_t;

static void
record_event(n00b_jparse_event_t event, uint64_t offset, void *user)
{
    event_log_t *log = (event_log_t *)user;
    if (log->count < MAX_EVENTS) {
        log->events[log->count]  = event;
        log->offsets[log->count] = offset;
        log->count++;
    }
}

// ============================================================================
// Tests
// ============================================================================

static void
test_ijson_simple_object(void)
{
    event_log_t log = { .count = 0 };
    n00b_ijson_ctx_t ctx;
    n00b_ijson_init(&ctx, .callback = record_event, .user_param = &log);

    const char *json = "{\"a\":1}";
    n00b_ijson_incremental_parse(&ctx, (uint8_t *)json, (uint32_t)strlen(json));
    n00b_ijson_end_of_input(&ctx);

    // Should have events: JSON_START, OBJECT_START, STRING_START, STRING_END,
    // INT_START, INT_END, NUMBER_END, OBJECT_END, JSON_END
    assert(log.count > 0);

    // First event should be JSON_START.
    assert(log.events[0] == N00B_JSON_START);

    // Should contain OBJECT_START.
    bool found_obj_start = false;
    bool found_obj_end   = false;
    bool found_str       = false;
    bool found_int       = false;
    bool found_json_end  = false;
    for (int i = 0; i < log.count; i++) {
        if (log.events[i] == N00B_JOBJECT_START) found_obj_start = true;
        if (log.events[i] == N00B_JOBJECT_END) found_obj_end = true;
        if (log.events[i] == N00B_JSTRING_START) found_str = true;
        if (log.events[i] == N00B_JINT_START) found_int = true;
        if (log.events[i] == N00B_JSON_END) found_json_end = true;
    }
    assert(found_obj_start);
    assert(found_obj_end);
    assert(found_str);
    assert(found_int);
    assert(found_json_end);

    // No errors.
    for (int i = 0; i < log.count; i++) {
        assert(log.events[i] != N00B_JERROR);
    }

    n00b_ijson_delete(&ctx);
    printf("  [PASS] ijson simple object\n");
}

static void
test_ijson_streaming(void)
{
    event_log_t log = { .count = 0 };
    n00b_ijson_ctx_t ctx;
    n00b_ijson_init(&ctx, .callback = record_event, .user_param = &log);

    // Feed the same JSON one byte at a time.
    const char *json = "{\"a\":1}";
    size_t len = strlen(json);
    for (size_t i = 0; i < len; i++) {
        n00b_ijson_incremental_parse(&ctx, (uint8_t *)&json[i], 1);
    }
    n00b_ijson_end_of_input(&ctx);

    // Should get the same events as the non-streaming case.
    assert(log.count > 0);
    assert(log.events[0] == N00B_JSON_START);

    bool found_json_end = false;
    for (int i = 0; i < log.count; i++) {
        if (log.events[i] == N00B_JSON_END) found_json_end = true;
        assert(log.events[i] != N00B_JERROR);
    }
    assert(found_json_end);

    n00b_ijson_delete(&ctx);
    printf("  [PASS] ijson streaming (byte-at-a-time)\n");
}

static void
test_ijson_nested(void)
{
    event_log_t log = { .count = 0 };
    n00b_ijson_ctx_t ctx;
    n00b_ijson_init(&ctx, .callback = record_event, .user_param = &log);

    const char *json = "{\"a\":[1,2],\"b\":{\"c\":true}}";
    n00b_ijson_incremental_parse(&ctx, (uint8_t *)json, (uint32_t)strlen(json));
    n00b_ijson_end_of_input(&ctx);

    assert(log.count > 0);

    // Count specific events.
    int obj_starts = 0, arr_starts = 0, true_events = 0;
    for (int i = 0; i < log.count; i++) {
        if (log.events[i] == N00B_JOBJECT_START) obj_starts++;
        if (log.events[i] == N00B_JARRAY_START) arr_starts++;
        if (log.events[i] == N00B_JTRUE) true_events++;
        assert(log.events[i] != N00B_JERROR);
    }

    // Two objects (outer + inner {"c":true}).
    assert(obj_starts == 2);
    // One array [1,2].
    assert(arr_starts == 1);
    // One true literal.
    assert(true_events == 1);

    n00b_ijson_delete(&ctx);
    printf("  [PASS] ijson nested structures\n");
}

static void
test_ijson_error(void)
{
    event_log_t log = { .count = 0 };
    n00b_ijson_ctx_t ctx;
    n00b_ijson_init(&ctx, .callback = record_event, .user_param = &log);

    const char *json = "{invalid";
    n00b_ijson_incremental_parse(&ctx, (uint8_t *)json, (uint32_t)strlen(json));
    n00b_ijson_end_of_input(&ctx);

    // Should contain an error event.
    bool found_error = false;
    for (int i = 0; i < log.count; i++) {
        if (log.events[i] == N00B_JERROR) found_error = true;
    }
    assert(found_error);

    n00b_ijson_delete(&ctx);
    printf("  [PASS] ijson error detection\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_ijson:\n");
    fflush(stdout);

    test_ijson_simple_object();  fflush(stdout);
    test_ijson_streaming();      fflush(stdout);
    test_ijson_nested();         fflush(stdout);
    test_ijson_error();          fflush(stdout);

    printf("All ijson tests passed.\n");
    n00b_shutdown();
    return 0;
}
