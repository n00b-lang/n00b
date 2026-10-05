// n00b#403 probe: does a MISS on a high-cardinality mapped TERM column return
// Ok(empty), or Err(N00B_STORE_INDEX_ERR_STATE)?
//
// #403 reports that an equality query for an absent term full-scans every
// sealed shard, because n00b_store_index_lookup_mapped answers -2 (ERR_STATE)
// instead of "no postings". The columns that fail are the high-cardinality
// ones (event_id, request_id: one distinct value per record); the
// low-cardinality ones on the same store are fine.
//
// The issue says this "needs a store with real cardinality to reproduce"
// (963 sealed shards / 24.6M records / 137 GB). This probe tests that claim
// directly: same seal-and-map path as test_rocs_index.c's
// test_mapped_term_lookup, but with the distinct-term count swept upward.
//
// Reading the code first (see the issue comment), the -2 can only come from
// rocs_index_map_err's `default:` arm, which collapses every
// N00B_STORE_MAP_ERR_* except ARG into ERR_STATE. The likeliest source is
// N00B_STORE_MAP_ERR_RANGE out of rocs_map_resolve_span, which
// n00b_store_map_dict_find_hv calls three times on spans that all scale with
// bucket_count -- i.e. with cardinality.
//
// Not a gating test: it prints a table and exits 0 either way. A clean sweep
// is informative too -- it would mean raw cardinality is NOT the trigger and
// the real variable is something the synthetic path does not reproduce
// (shard size, base_address packing, or the catalog layer above).

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/codegen_abi.h"
#include "core/static_objects.h"
#include "core/atomic.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "util/assert.h"

#include <rocs/n00b_rocs.h>
#include <rocs/index.h>
#include "rocs_test_support.h"
#include "test_check.h"

static n00b_json_node_t *
record_with_term(n00b_string_t *term)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record,
                              r"event_id",
                              n00b_json_string_new_from_n00b(term));
    return record;
}

// One distinct term per record, which is what event_id/request_id look like.
static n00b_string_t *
term_for(uint64_t i)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "evt-%016llx-%llu", (unsigned long long)i,
             (unsigned long long)(i * 2654435761u));
    return n00b_string_from_raw(buf, (int64_t)strlen(buf));
}

typedef struct {
    uint64_t distinct;
    bool     sealed;
    bool     miss_ok;
    int64_t  miss_err;
    bool     hit_ok;
} probe_row_t;

static probe_row_t
probe_at(uint64_t distinct)
{
    probe_row_t row = {.distinct = distinct};

    auto index_r = n00b_store_index_new(r"event_id", N00B_STORE_INDEX_TERM);
    if (n00b_result_is_err(index_r)) {
        return row;
    }
    n00b_store_index_t *index = n00b_result_get(index_r);

    auto shard_r = n00b_store_shard_new(.shard_id   = UINT64_C(0xfeedface),
                                        .allocator  = test_shard_allocator());
    if (n00b_result_is_err(shard_r)) {
        return row;
    }
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    for (uint64_t i = 0; i < distinct; i++) {
        auto append_r = n00b_store_shard_append(shard,
                                                record_with_term(term_for(i)));
        if (n00b_result_is_err(append_r)) {
            return row;
        }
        auto add_r = n00b_store_index_add(index, shard,
                                          n00b_result_get(append_r));
        if (n00b_result_is_err(add_r)) {
            return row;
        }
    }

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 77,
                                        .base_address = 0x6e00u);
    if (n00b_result_is_err(seal_r)) {
        return row;
    }
    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    if (n00b_result_is_err(open_r)) {
        return row;
    }
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    if (n00b_result_is_err(root_r)) {
        return row;
    }
    n00b_store_map_shard_t *mapped = n00b_result_get(root_r);
    row.sealed                     = true;

    // Control: a term that IS present must still resolve. If this breaks at
    // the same cardinality as the miss, the defect is not miss-specific.
    auto hit_r = n00b_store_index_lookup_mapped(
        index, mapped, n00b_json_string_new_from_n00b(term_for(distinct / 2)));
    row.hit_ok = n00b_result_is_ok(hit_r);

    // The case from the issue: equality for a term that was never written.
    auto miss_r = n00b_store_index_lookup_mapped(
        index,
        mapped,
        n00b_json_string_new_from_n00b(r"evt-absent-never-written"));
    row.miss_ok = n00b_result_is_ok(miss_r);
    if (!row.miss_ok) {
        row.miss_err = (int64_t)n00b_result_get_err(miss_r);
    }

    (void)n00b_store_map_close(map);
    return row;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#403 probe: mapped TERM miss vs. distinct-term count\n");
    printf("  (-2 == N00B_STORE_INDEX_ERR_STATE, the code #403 reports)\n\n");
    printf("  %10s  %7s  %8s  %s\n", "distinct", "sealed", "hit", "miss");

    static const uint64_t steps[] = {
        4, 64, 512, 4096, 16384, 65536, 262144, 1048576,
    };

    bool reproduced = false;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        probe_row_t row = probe_at(steps[i]);
        char        miss[32];
        if (!row.sealed) {
            snprintf(miss, sizeof(miss), "(setup failed)");
        } else if (row.miss_ok) {
            snprintf(miss, sizeof(miss), "Ok(empty)");
        } else {
            snprintf(miss, sizeof(miss), "Err(%lld)", (long long)row.miss_err);
            reproduced = true;
        }
        printf("  %10llu  %7s  %8s  %s\n",
               (unsigned long long)row.distinct,
               row.sealed ? "yes" : "NO",
               row.sealed ? (row.hit_ok ? "ok" : "ERR") : "-",
               miss);
        fflush(stdout);
        if (reproduced) {
            printf("\n  REPRODUCED at distinct=%llu\n",
                   (unsigned long long)row.distinct);
            break;
        }
    }

    if (!reproduced) {
        printf("\n  not reproduced by cardinality alone up to the last step.\n");
        printf("  -> the trigger is something else: shard byte size, the\n");
        printf("     base_address packing across many shards, or the catalog\n");
        printf("     layer above a single sealed shard.\n");
    }

    n00b_shutdown();
    return 0;
}
