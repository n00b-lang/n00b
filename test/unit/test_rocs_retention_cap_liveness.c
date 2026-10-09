/*
 * The crashappsec/wax#1229 acceptance test: a store AT its retention cap, with
 * queries in flight, must still answer a status read.
 *
 * wax#1229 asks for exactly this -- "fill a store to the cap and assert
 * /v1/status still answers" -- and the part that makes it meaningful is the
 * query workload. Retention at the cap with NOTHING pinned is the easy case.
 * The reported failure is a gateway serving live queries, and a query pins the
 * shards it may read, so retention's candidates are pinned and it takes the
 * path that used to be O(N^3).
 *
 * WHAT THE "STATUS READ" IS HERE. n00b's rocs service has no /v1/status --
 * that route is crayon-gw's. Its equivalent is /metrics, and the relevant
 * thing is not the HTTP layer but which lock the handler needs:
 *
 *     rocs_service_metrics_handler                 (service_runtime.c)
 *       -> rocs_service_store_residency_stats_metric
 *         -> n00b_store_residency_stats            (store.c)
 *           -> n00b_mutex_lock(store->residency_lock)     <-- blocks HERE
 *
 * and a retention pass holds residency_lock (and commit_lock) for its entire
 * duration. So a status read cannot answer while retention is running, and
 * "how long is a retention pass" IS "how long can /v1/status stall". This
 * polls n00b_store_residency_stats directly -- the exact call the handler
 * makes -- because the HTTP layer adds latency but not mechanism.
 *
 * The store drives retention itself: rocs_store_apply_default_retention runs
 * on every seal, so once the cap is reached every rotation evicts. That is
 * the production shape, not a test harness simulating one.
 *
 * WHAT IT ASSERTS. Not a throughput number, which would be a flaky gate.
 * It asserts that no status read exceeds a deadline while all of this runs
 * concurrently, and it reports max/p99 so a regression that degrades without
 * crossing the line is still visible in the log.
 *
 * Registered as a test at a small size. ROCS_LIVENESS_SECONDS,
 * ROCS_LIVENESS_CAP (sealed-shard cap), ROCS_LIVENESS_QUERIERS,
 * ROCS_LIVENESS_RECORDS (per shard) and ROCS_LIVENESS_MAX_STALL_MS size it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "rocs/filter.h"
#include "rocs/query.h"
#include "rocs/store.h"
#include "rocs/wax.h"
#include "util/assert.h"
#include "util/path.h"
#include "vfs/backend_local.h"
#include "vfs/vfs.h"

#include "internal/rocs/store.h"
#include "rocs_test_support.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "retention cap liveness check failed: " #expr);   \
    } while (0)

#define MAX_SAMPLES 200000

static uint64_t run_seconds  = 3;
static uint64_t cap_shards   = 24;
static uint64_t queriers     = 2;
static uint64_t per_shard    = 40;
static uint64_t max_stall_ms = 2000;

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

// Shared state. The workers are plain n00b threads; everything they publish
// is atomic so the reporting at the end is a consistent read.
static n00b_store_t  *store;
static _Atomic(bool)  stop_flag;
static _Atomic(uint64_t) ingested;
static _Atomic(uint64_t) seals;
static _Atomic(uint64_t) queries_run;
static _Atomic(uint64_t) query_errors;
// Longest single seal, in ns. A seal includes the auto-retention pass
// (rocs_store_apply_default_retention), so comparing this against the worst
// status read attributes the stall: if they track, the status read was
// waiting on a seal rather than on anything else.
static _Atomic(uint64_t) worst_seal_ns;

// Status-read latencies, in ns, recorded by the status thread only.
static uint64_t samples[MAX_SAMPLES];
static uint64_t nsamples;

static n00b_vfs_t *
new_local_vfs(n00b_string_t **dir)
{
    auto tmp_r = n00b_new_temp_dir(r"n00b_retention_liveness_", nullptr);
    CHECK(n00b_result_is_ok(tmp_r));
    *dir = n00b_result_get(tmp_r);

    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);
    auto        be_r = n00b_vfs_backend_local_new(*dir);
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static const char *kinds[6] = {
    "proc.spawn",
    "file.modify",
    "net.connect",
    "ai.session_start",
    "host.heartbeat",
    "repo.snapshot",
};

static n00b_json_node_t *
record(uint64_t g)
{
    char              buf[64];
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec,
                         "schema",
                         n00b_json_string_new("wax.normalized.v1"));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kinds[g % 6]));
    snprintf(buf, sizeof(buf), "wax:live:%llu", (unsigned long long)g);
    n00b_json_object_put(rec, "event_id", n00b_json_string_new(buf));
    n00b_json_object_put(rec, "ts_ns", n00b_json_int_new((int64_t)g));

    n00b_json_node_t *source = n00b_json_object_new();
    snprintf(buf, sizeof(buf), "host-%03llu", (unsigned long long)(g % 50));
    n00b_json_object_put(source, "name", n00b_json_string_new(buf));
    n00b_json_object_put(rec, "source", source);
    return rec;
}

// Ingest + seal forever. Past the cap, rocs_store_apply_default_retention
// runs inside every seal, so this is also the retention driver.
static void *
ingest_worker(void *unused)
{
    (void)unused;
    uint64_t g = 0;
    uint64_t s = 0;
    while (!n00b_atomic_load(&stop_flag)) {
        for (uint64_t i = 0; i < per_shard; i++) {
            auto r = n00b_store_ingest(store, record(g++));
            if (n00b_result_is_ok(r)) {
                n00b_atomic_add(&ingested, 1);
            }
        }
        uint64_t sealed_at = now_ns();
        auto     seal_r    = n00b_store_seal_hot_shard(store,
                                                .seal_ts = (++s) * 1000);
        uint64_t seal_ns = now_ns() - sealed_at;
        if (n00b_atomic_load(&worst_seal_ns) < seal_ns) {
            n00b_atomic_store(&worst_seal_ns, seal_ns);
        }
        if (n00b_result_is_ok(seal_r)) {
            n00b_atomic_add(&seals, 1);
        }
    }
    return nullptr;
}

// Real queries through the real planner, which is what takes the pins:
// n00b_query_view narrows a store pin to the shards the query may read
// (query.c, n00b_store_pin_narrow_to_shards), so retention's candidates are
// genuinely blocked rather than blocked by a hand-built pin.
static void *
query_worker(void *unused)
{
    (void)unused;
    uint64_t n = 0;
    while (!n00b_atomic_load(&stop_flag)) {
        char value[64];
        snprintf(value, sizeof(value), "host-%03llu", (unsigned long long)(n++ % 50));

        auto field_r = n00b_filter_field(r"source.name");
        if (n00b_result_is_err(field_r)) {
            n00b_atomic_add(&query_errors, 1);
            continue;
        }
        auto filter_r = n00b_filter_eq(n00b_result_get(field_r),
                                       n00b_fv_utf8(n00b_string_from_cstr(value)));
        if (n00b_result_is_err(filter_r)) {
            n00b_atomic_add(&query_errors, 1);
            continue;
        }
        auto query_r = n00b_query_new(n00b_result_get(filter_r), .limit = 200);
        if (n00b_result_is_err(query_r)) {
            n00b_atomic_add(&query_errors, 1);
            continue;
        }
        auto result_r = n00b_query_run(store, n00b_result_get(query_r));
        if (n00b_result_is_err(result_r)) {
            // A query racing a drop can legitimately fail; it is not what
            // this test is about, so count it and keep going.
            n00b_atomic_add(&query_errors, 1);
            continue;
        }
        (void)n00b_query_result_close(n00b_result_get(result_r));
        n00b_atomic_add(&queries_run, 1);
    }
    return nullptr;
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    run_seconds  = env_u64("ROCS_LIVENESS_SECONDS", run_seconds);
    cap_shards   = env_u64("ROCS_LIVENESS_CAP", cap_shards);
    queriers     = env_u64("ROCS_LIVENESS_QUERIERS", queriers);
    per_shard    = env_u64("ROCS_LIVENESS_RECORDS", per_shard);
    max_stall_ms = env_u64("ROCS_LIVENESS_MAX_STALL_MS", max_stall_ms);

    n00b_string_t *dir = nullptr;
    n00b_vfs_t    *vfs = new_local_vfs(&dir);

    auto schema_r = n00b_rocs_wax_schema_new();
    CHECK(n00b_result_is_ok(schema_r));

    // The cap is the whole point: with it set, every seal past it evicts,
    // which is the steady state wax#1229 describes as "the designed steady
    // state, not an abuse".
    auto store_r = n00b_store_open_vfs(vfs,
                                       r"/rocs",
                                       n00b_result_get(schema_r),
                                       .retention_max_sealed_shards = cap_shards);
    CHECK(n00b_result_is_ok(store_r));
    store = n00b_result_get(store_r);

    printf("rocs retention-cap liveness (crashappsec/wax#1229 acceptance)\n");
    printf("  cap=%llu sealed shards, %llu querier(s), %llu s, "
           "status deadline %llu ms\n\n",
           (unsigned long long)cap_shards,
           (unsigned long long)queriers,
           (unsigned long long)run_seconds,
           (unsigned long long)max_stall_ms);

    // Fill to the cap BEFORE measuring, so the measured window is entirely
    // "at the cap" rather than partly spent getting there.
    printf("  filling to the cap ...\n");
    fflush(stdout);
    uint64_t g = 0;
    for (uint64_t s = 0; s < cap_shards + 2; s++) {
        for (uint64_t i = 0; i < per_shard; i++) {
            CHECK(n00b_result_is_ok(n00b_store_ingest(store, record(g++))));
        }
        CHECK(n00b_result_is_ok(
            n00b_store_seal_hot_shard(store, .seal_ts = (s + 1) * 1000)));
    }
    auto count_r = n00b_store_catalog_get_entry_count(store);
    CHECK(n00b_result_is_ok(count_r));
    printf("  at cap: %llu sealed shards\n\n",
           (unsigned long long)n00b_result_get(count_r));

    n00b_atomic_store(&stop_flag, false);

    n00b_thread_t *ingest_t;
    {
        n00b_result_t(n00b_thread_t *) r = n00b_thread_spawn(ingest_worker,
                                                              nullptr);
        CHECK(n00b_result_is_ok(r));
        ingest_t = n00b_result_get(r);
    }

    n00b_thread_t *query_t[16];
    uint64_t       nq = queriers > 16 ? 16 : queriers;
    for (uint64_t i = 0; i < nq; i++) {
        n00b_result_t(n00b_thread_t *) r = n00b_thread_spawn(query_worker,
                                                              nullptr);
        CHECK(n00b_result_is_ok(r));
        query_t[i] = n00b_result_get(r);
    }

    // The status reader runs on THIS thread so its latency is never confused
    // with scheduling of a worker we also spawned.
    uint64_t deadline_ns = run_seconds * 1000000000ull;
    uint64_t t0          = now_ns();
    uint64_t worst       = 0;
    while (now_ns() - t0 < deadline_ns && nsamples < MAX_SAMPLES) {
        uint64_t s0 = now_ns();
        auto     st = n00b_store_residency_stats(store); // what /metrics calls
        uint64_t dt = now_ns() - s0;
        CHECK(n00b_result_is_ok(st));
        samples[nsamples++] = dt;
        if (dt > worst) {
            worst = dt;
        }
        base_nanosleep_ns(1000000ull); // ~1 kHz, well above any real prober
    }

    n00b_atomic_store(&stop_flag, true);
    n00b_thread_join(ingest_t);
    for (uint64_t i = 0; i < nq; i++) {
        n00b_thread_join(query_t[i]);
    }

    CHECK(nsamples > 0);
    qsort(samples, nsamples, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = samples[nsamples / 2];
    uint64_t p99 = samples[(nsamples * 99) / 100];
    uint64_t over = 0;
    for (uint64_t i = 0; i < nsamples; i++) {
        if (samples[i] > max_stall_ms * 1000000ull) {
            over++;
        }
    }

    printf("  workload:  %llu ingested, %llu seals, %llu queries"
           " (%llu query errors)\n",
           (unsigned long long)n00b_atomic_load(&ingested),
           (unsigned long long)n00b_atomic_load(&seals),
           (unsigned long long)n00b_atomic_load(&queries_run),
           (unsigned long long)n00b_atomic_load(&query_errors));
    printf("  worst seal (incl. its retention pass): %.3f ms\n",
           (double)n00b_atomic_load(&worst_seal_ns) / 1e6);
    printf("  status reads: %llu\n", (unsigned long long)nsamples);
    printf("    p50 %8.3f ms\n", (double)p50 / 1e6);
    printf("    p99 %8.3f ms\n", (double)p99 / 1e6);
    printf("    max %8.3f ms\n", (double)worst / 1e6);
    printf("    over %llu ms deadline: %llu\n\n",
           (unsigned long long)max_stall_ms,
           (unsigned long long)over);

    // The seal count is the guard against a vacuous pass: if nothing sealed,
    // retention never ran and a fast status read proves nothing.
    if (n00b_atomic_load(&seals) == 0) {
        printf("  [FAIL] no seals during the measured window -- retention\n"
               "         never ran, so this measured nothing\n");
        return 1;
    }
    if (n00b_atomic_load(&queries_run) == 0) {
        printf("  [FAIL] no queries completed -- nothing was pinned, so the\n"
               "         pinned retention path was never exercised\n");
        return 1;
    }
    if (over != 0) {
        printf("  [FAIL] %llu status read(s) exceeded %llu ms while retention\n"
               "         ran at the cap -- this is the wax#1229 symptom\n",
               (unsigned long long)over,
               (unsigned long long)max_stall_ms);
        return 1;
    }

    printf("  [PASS] status answered within %llu ms on every one of %llu reads,\n"
           "         with retention evicting at the cap and queries pinning\n"
           "         shards throughout\n",
           (unsigned long long)max_stall_ms,
           (unsigned long long)nsamples);

    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    return 0;
}
