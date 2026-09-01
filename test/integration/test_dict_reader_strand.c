// Regression coverage for the reader-side strand backoff (see the
// N00B_DICT_READER_SPIN_LIMIT path in src/adt/dict_untyped.c).
//
// Strands one bucket's MUTEX with no owner, then drives readers onto it and
// checks the mitigation's contract: the reader parks (the backoff counter
// climbs), the one-time diagnostic fires exactly once no matter how many
// readers hit it, unrelated keys stay serviceable, and once the strand clears
// every parked reader completes correctly.
//
// Two cases, each a fresh process (the diagnostic guard is process-global):
//   get  -> exercises n00b_acquire_if_present
//   add  -> exercises n00b_acquire_or_add
//
// Meaningful as a gate: without the backoff the counter never moves and the
// warning never appears, so both the wait and the final assert fail.

#define N00B_USE_INTERNAL_API
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/dict_untyped.h"

#define STRANDED_KEY   ((void *)(uintptr_t)1)
#define STRANDED_VALUE ((void *)(uintptr_t)0xAA)
#define HEALTHY_KEY    ((void *)(uintptr_t)8)
#define HEALTHY_VALUE  ((void *)(uintptr_t)0x88)
#define N_WORKERS      4
#define BACKOFF_TARGET 8
#define DEADLINE_NS    (5ULL * 1000 * 1000 * 1000)
#define WARNING_NEEDLE "bucket mutex held past the reader spin bound"

static const char *g_case = "?";

static n00b_hash_value_t
identity_hash(void *key)
{
    return (n00b_hash_value_t)(uintptr_t)key;
}

static void
fail(const char *msg)
{
    fprintf(stderr, "  [FAIL] %s: %s\n", g_case, msg);
    exit(1);
}

// Locate the reserved bucket holding `key` (robust to the hash) and OR in MUTEX.
static n00b_dict_untyped_bucket_t *
strand_bucket(n00b_dict_untyped_t *d, void *key)
{
    n00b_dict_untyped_store_t *s = atomic_load(&d->store);
    for (uint32_t i = 0; i <= s->last_slot; i++) {
        n00b_dict_untyped_bucket_t *b = &s->buckets[i];
        if (b->hv != 0 && !(atomic_load(&b->flags) & N00B_HT_FLAG_DELETED)
            && b->key == key) {
            atomic_fetch_or(&b->flags, N00B_HT_FLAG_MUTEX);
            return b;
        }
    }
    return NULL;
}

typedef struct {
    n00b_dict_untyped_t *d;
    bool                 is_add;
    _Atomic bool         returned;
    void                *result;
} worker_t;

static void *
worker_fn(void *arg)
{
    worker_t *w = arg;
    if (w->is_add) {
        // Adding an already-present key: acquire_or_add probes to the stranded
        // bucket, then reports "already present" (false) once it can lock it.
        bool r     = _n00b_dict_untyped_add(w->d, STRANDED_KEY, STRANDED_VALUE);
        w->result  = (void *)(uintptr_t)r;
    }
    else {
        bool found;
        w->result = _n00b_dict_untyped_get(w->d, STRANDED_KEY, &found);
    }
    atomic_store(&w->returned, true);
    return w->result;
}

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int
run_case(bool is_add)
{
    // Lower the reader spin bound so the backoff is reached quickly; production
    // uses a bound far above any legitimate wait (see dict_untyped.c).
    n00b_dict_reader_spin_limit_set(256);

    n00b_dict_untyped_t d;
    n00b_dict_untyped_init(&d,
                           .hash          = identity_hash,
                           .skip_obj_hash = true,
                           .start_capacity = 16);

    _n00b_dict_untyped_put(&d, STRANDED_KEY, STRANDED_VALUE);
    _n00b_dict_untyped_put(&d, HEALTHY_KEY, HEALTHY_VALUE);

    // Capture fd 2 (the diagnostic goes there via n00b_raw_write).
    fflush(stderr);
    int  saved_fd2 = dup(2);
    char tmpl[]    = "/tmp/n00b_reader_strandXXXXXX";
    int  cap_fd    = mkstemp(tmpl);
    if (saved_fd2 < 0 || cap_fd < 0) {
        fail("could not set up stderr capture");
    }
    dup2(cap_fd, 2);

    n00b_dict_untyped_bucket_t *stranded = strand_bucket(&d, STRANDED_KEY);
    if (!stranded) {
        dup2(saved_fd2, 2);
        fail("stranded key not found");
    }

    worker_t       workers[N_WORKERS] = {};
    n00b_thread_t *threads[N_WORKERS] = {};
    for (int i = 0; i < N_WORKERS; i++) {
        workers[i].d      = &d;
        workers[i].is_add = is_add;
        n00b_result_t(n00b_thread_t *) r = n00b_thread_spawn(worker_fn, &workers[i]);
        if (!n00b_result_is_ok(r)) {
            dup2(saved_fd2, 2);
            fail("could not spawn worker");
        }
        threads[i] = n00b_result_get(r);
    }

    // Wait until the readers have parked (backoff ran several times).
    uint64_t deadline    = now_ns() + DEADLINE_NS;
    bool     reached     = false;
    while (now_ns() < deadline) {
        if (n00b_dict_reader_backoff_count_get() >= BACKOFF_TARGET) {
            reached = true;
            break;
        }
        sleep_ms(5);
    }

    // Observations while the strand is still held.
    bool any_returned = false;
    for (int i = 0; i < N_WORKERS; i++) {
        if (atomic_load(&workers[i].returned)) {
            any_returned = true;
        }
    }
    bool  healthy_found = false;
    void *healthy       = _n00b_dict_untyped_get(&d, HEALTHY_KEY, &healthy_found);

    // Clear the strand and join EVERY worker before asserting anything, so a
    // failure never leaves wedged threads behind.
    atomic_fetch_and(&stranded->flags, ~N00B_HT_FLAG_MUTEX);
    bool results_ok = true;
    for (int i = 0; i < N_WORKERS; i++) {
        void *ret = n00b_thread_join(threads[i]);
        if (is_add) {
            if ((bool)(uintptr_t)ret != false) {
                results_ok = false;  // add of an existing key must report false
            }
        }
        else if (ret != STRANDED_VALUE) {
            results_ok = false;      // get must return the stored value
        }
    }

    // Restore stderr and read what the capture holds.
    fflush(stderr);
    dup2(saved_fd2, 2);
    close(saved_fd2);
    char    buf[8192];
    ssize_t n = pread(cap_fd, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';
    close(cap_fd);
    unlink(tmpl);

    int   warnings = 0;
    char *p        = buf;
    while ((p = strstr(p, WARNING_NEEDLE))) {
        warnings++;
        p += 1;
    }

    // Now evaluate — everything is cleaned up.
    if (!reached) {
        fail("readers never parked (backoff counter did not reach target)");
    }
    if (any_returned) {
        fail("a reader returned while the bucket was stranded");
    }
    if (!healthy_found || healthy != HEALTHY_VALUE) {
        fail("an unrelated key was not serviceable during the strand");
    }
    if (!results_ok) {
        fail("a worker returned the wrong result after the strand cleared");
    }
    if (warnings != 1) {
        char m[64];
        snprintf(m, sizeof(m), "diagnostic fired %d times, expected exactly 1", warnings);
        fail(m);
    }

    printf("  [PASS] dict_reader_strand:%s\n", g_case);
    return 0;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    bool is_add = (argc == 2 && strcmp(argv[1], "add") == 0);
    g_case      = is_add ? "add" : "get";

    printf("Running dict_reader_strand (%s)...\n", g_case);
    return run_case(is_add);
}
