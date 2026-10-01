/* test/unit/bench_rocs_catalog.c - what a catalog write costs.
 *
 * The catalog is rewritten whole on every seal, flush and close, while the
 * store's commit lock is held. This times that write (and the parse on open)
 * so a change to the encoder has a number to show, rather than a guess from
 * the shape of the code. It prints rather than asserting a threshold.
 *
 * Two workloads:
 *   - default: a memory-backed store with ROCS_BENCH_SHARDS sealed shards.
 *   - ROCS_BENCH_STORE_DIR=<dir>: an existing on-disk store whose files live
 *     at <dir>/store (the gateway layout). The store is opened read-write and
 *     its catalog is rewritten in place, so point this at a copy.
 */

#include <stdint.h>
#include <stdlib.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/runtime.h"
#include "core/time.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "util/assert.h"
#include "vfs/backend_local.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include <rocs/n00b_rocs.h>

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "bench check failed: " #expr);                    \
    } while (0)

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

static n00b_vfs_t *
new_memory_vfs(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    auto be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static n00b_vfs_t *
new_local_vfs(n00b_string_t *dir)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    auto be_r = n00b_vfs_backend_local_new(dir);
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static uint64_t
catalog_bytes(n00b_vfs_t *vfs, n00b_string_t *root)
{
    auto stat_r = n00b_vfs_stat(vfs, n00b_cformat("«#»/catalog.rocs", root));
    if (n00b_result_is_err(stat_r)) {
        return 0;
    }
    return (uint64_t)n00b_result_get(stat_r).size;
}

static int64_t
ms_since(int64_t start_ns)
{
    return (n00b_ns_timestamp() - start_ns) / 1000000;
}

// Rewrite the catalog (flush with an empty hot shard writes only the
// catalog), then close. Both go through the same encoder.
static void
time_flush_and_close(n00b_vfs_t *vfs, n00b_string_t *root, n00b_store_t *store)
{
    int64_t start = n00b_ns_timestamp();
    CHECK(n00b_result_is_ok(n00b_store_flush(store)));
    int64_t flush_ms = ms_since(start);

    start = n00b_ns_timestamp();
    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    int64_t close_ms = ms_since(start);

    n00b_printf("  catalog [|#|] bytes: flush [|#|] ms, close [|#|] ms",
                (int64_t)catalog_bytes(vfs, root),
                flush_ms,
                close_ms);
}

static void
bench_synthetic(uint64_t shards)
{
    n00b_vfs_t *vfs = new_memory_vfs();

    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    n00b_store_schema_t *schema = n00b_result_get(schema_r);
    CHECK(n00b_result_is_ok(n00b_store_schema_add_field(schema, r"level")));
    CHECK(n00b_result_is_ok(n00b_store_schema_add_field(
        schema,
        r"name",
        .index_kind = N00B_STORE_INDEX_TERM)));

    auto store_r = n00b_store_open_vfs(vfs, r"/rocs", schema);
    CHECK(n00b_result_is_ok(store_r));
    n00b_store_t *store = n00b_result_get(store_r);

    for (uint64_t sh = 0; sh < shards; sh++) {
        for (uint64_t r = 0; r < 4; r++) {
            n00b_json_node_t *record = n00b_json_object_new();
            n00b_json_object_put(record,
                                 "level",
                                 n00b_json_int_new((int64_t)(sh * 10 + r)));
            n00b_json_object_put(record,
                                 "name",
                                 n00b_json_string_new_from_n00b(
                                     n00b_cformat("name-«#»", (int64_t)r)));
            CHECK(n00b_result_is_ok(n00b_store_ingest(store, record)));
        }
        CHECK(n00b_result_is_ok(
            n00b_store_seal_hot_shard(store, .seal_ts = 1000 + sh)));
    }

    n00b_printf("synthetic store, [|#|] sealed shards:", (int64_t)shards);
    time_flush_and_close(vfs, r"/rocs", store);
}

static void
bench_existing(n00b_string_t *dir)
{
    n00b_vfs_t *vfs = new_local_vfs(dir);

    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));

    n00b_printf("existing store at «#»:", dir);
    int64_t start   = n00b_ns_timestamp();
    auto    store_r = n00b_store_open_vfs(vfs,
                                          r"/store",
                                          n00b_result_get(schema_r));
    CHECK(n00b_result_is_ok(store_r));
    n00b_printf("  open [|#|] ms", ms_since(start));

    time_flush_and_close(vfs, r"/store", n00b_result_get(store_r));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    const char *dir = getenv("ROCS_BENCH_STORE_DIR");
    if (dir != nullptr && *dir != '\0') {
        bench_existing(n00b_string_from_cstr(dir));
    }
    else {
        bench_synthetic(env_u64("ROCS_BENCH_SHARDS", 200));
    }

    n00b_shutdown();
    return 0;
}
