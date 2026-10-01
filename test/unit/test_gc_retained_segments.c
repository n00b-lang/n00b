/* test/unit/test_gc_retained_segments.c
 *
 * A pin-all heap (crashappsec/wax#946) carries one `retained` segment per
 * run of pinned pages, and that chain grows with uptime: tens of thousands of
 * descriptors on a long-lived gateway. The collector resolves every pinned
 * object to its from-space segment, so the cost of that lookup, times the
 * live object count, is the stop-the-world pause.
 *
 * This builds such a chain on purpose: each round allocates N live objects
 * separated by dead page-sized filler, so after the round's collection every
 * live object sits in its own retained run. The chain then grows by N per
 * round. At each checkpoint a collection with nothing new allocated is timed;
 * that is the per-collection floor at that chain length.
 *
 * Correctness: every live object's payload survives every collection.
 * Timing is printed, not asserted. Knobs (env):
 *   GC_RETAINED_PER_ROUND   live objects per round        (default 250)
 *   GC_RETAINED_TARGET      stop at this many segments    (default 2000)
 *   GC_RETAINED_CHECKPOINTS comma list of segment counts to time
 *                           (default "1000,2000")
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/time.h"
#include "util/assert.h"

#include "test_check.h"

#define MAX_CHECKPOINTS 16

static uint64_t
segment_count(n00b_arena_t *arena, uint64_t *retained)
{
    uint64_t        n   = 0;
    uint64_t        r   = 0;
    n00b_segment_t *seg = n00b_atomic_load(&arena->current_segment);

    while (seg) {
        n++;
        if (seg->retained) {
            r++;
        }
        seg = seg->next_segment;
    }
    if (retained) {
        *retained = r;
    }
    return n;
}

static int64_t
env_int(const char *name, int64_t dflt)
{
    const char *v = getenv(name);
    return v ? atoll(v) : dflt;
}

static int
parse_checkpoints(uint64_t *out)
{
    const char *v = getenv("GC_RETAINED_CHECKPOINTS");
    char        buf[256];
    int         n = 0;

    char       *tok;

    snprintf(buf, sizeof(buf), "%s", v ? v : "1000,2000");
    tok = strtok(buf, ",");
    while (tok && n < MAX_CHECKPOINTS) {
        out[n++] = (uint64_t)atoll(tok);
        tok      = strtok(nullptr, ",");
    }
    return n;
}

static double
timed_collect_ms(n00b_arena_t *arena)
{
    int64_t t0 = n00b_ns_timestamp();
    n00b_collect(arena);
    int64_t t1 = n00b_ns_timestamp();
    return (double)(t1 - t0) / 1e6;
}

static void
check_payloads(uint8_t **live, uint64_t count, uint64_t payload)
{
    for (uint64_t i = 0; i < count; i++) {
        CHECK(live[i][0] == (uint8_t)(i & 0xff));
        CHECK(live[i][payload - 1] == (uint8_t)((i >> 8) & 0xff));
    }
}

int
main(int argc, char **argv)
{
    // Decided at the first collection; the gateway runs this way (no GC type
    // map), and it is the mode that grows the retained chain.
    setenv("N00B_GC_PIN_ALL", "1", 1);
    n00b_init_simple(argc, argv);

    n00b_arena_t *arena      = n00b_get_runtime()->default_arena;
    uint64_t      per_round  = (uint64_t)env_int("GC_RETAINED_PER_ROUND", 250);
    uint64_t      target     = (uint64_t)env_int("GC_RETAINED_TARGET", 2000);
    uint64_t      checkpoints[MAX_CHECKPOINTS];
    int           ncheck     = parse_checkpoints(checkpoints);
    int           next_check = 0;
    uint64_t      page       = (uint64_t)n00b_page_size;
    // A live object a little under one page lands on at most two pages; two
    // pages of dead filler after it guarantee at least one wholly unpinned
    // page before the next live object, so the two never share a run.
    uint64_t      payload    = page - 256;
    uint64_t      filler     = 2 * page;
    uint64_t      capacity   = target + per_round;

    uint8_t **live = n00b_alloc_array_with_opts(uint8_t *,
                                                capacity,
                                                &(n00b_alloc_opts_t){
                                                    .allocator = (n00b_allocator_t *)arena});
    uint64_t  count = 0;

    printf("gc_retained_segments: pin_all=%d page=%llu per_round=%llu target=%llu\n",
           n00b_gc_pin_all_policy() ? 1 : 0,
           (unsigned long long)page,
           (unsigned long long)per_round,
           (unsigned long long)target);
    printf("%10s %10s %12s\n", "segments", "live_objs", "collect_ms");

    while (count < target) {
        for (uint64_t i = 0; i < per_round && count < capacity; i++, count++) {
            uint8_t *obj = n00b_alloc_array_with_opts(uint8_t,
                                                      payload,
                                                      &(n00b_alloc_opts_t){
                                                          .allocator = (n00b_allocator_t *)arena,
                                                          .no_scan   = true});
            obj[0]           = (uint8_t)(count & 0xff);
            obj[payload - 1] = (uint8_t)((count >> 8) & 0xff);
            live[count]      = obj;
            // Dead on the next line; its pages are what separates the runs.
            (void)n00b_alloc_array_with_opts(uint8_t,
                                             filler,
                                             &(n00b_alloc_opts_t){
                                                 .allocator = (n00b_allocator_t *)arena,
                                                 .no_scan   = true});
        }
        n00b_collect(arena);
        check_payloads(live, count, payload);

        uint64_t retained = 0;
        uint64_t segs     = segment_count(arena, &retained);
        if (next_check < ncheck && segs >= checkpoints[next_check]) {
            // Nothing allocated since the collect above: this pause is the
            // per-collection floor at this chain length.
            double ms = timed_collect_ms(arena);
            check_payloads(live, count, payload);
            segs = segment_count(arena, &retained);
            printf("%10llu %10llu %12.1f\n",
                   (unsigned long long)segs,
                   (unsigned long long)count,
                   ms);
            fflush(stdout);
            while (next_check < ncheck && segs >= checkpoints[next_check]) {
                next_check++;
            }
        }
    }

    uint64_t retained = 0;
    uint64_t segs     = segment_count(arena, &retained);
    printf("final: segments=%llu retained=%llu live_objs=%llu\n",
           (unsigned long long)segs,
           (unsigned long long)retained,
           (unsigned long long)count);
    // The harness only measures what it claims to if the chain really did
    // grow one run per live object.
    CHECK(retained >= count * 9 / 10);
    CHECK(retained <= count * 2 + 16);

    printf("PASS\n");
    return 0;
}
