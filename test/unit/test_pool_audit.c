/* Verify pool-audit availability reporting and per-site attribution. */

#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/pool.h"
#include "core/runtime.h"

#define AUDIT_LIVE_ALLOCS 512
#define AUDIT_OBJ_BYTES   4096

typedef struct {
    char bytes[AUDIT_OBJ_BYTES];
} audit_obj_t;

[[n00b::nomap]] static n00b_pool_t g_audited;

// Keep allocations live until after the snapshot assertions.
static void *g_live[AUDIT_LIVE_ALLOCS];

static int
fail(const char *why)
{
    fprintf(stderr, "test_pool_audit: FAIL — %s\n", why);
    return 1;
}

int
main(int argc, char **argv)
{
    bool require_compiled = argc > 1
                            && strcmp(argv[1], "--require-compiled") == 0;
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_allocator_t *audited = n00b_pool_init(&g_audited,
                                               .inline_headers = true,
                                               .alloc_audit    = true,
                                               .name = "test_pool_audit");

    if (!n00b_pool_audit_compiled()) {
        if (require_compiled) {
            n00b_shutdown();
            return fail("audit required but not compiled");
        }
        int rc = 0;
        if (n00b_pool_alloc_audit_enabled(audited)
            || n00b_user_pool_audit_enabled()
            || n00b_conduit_pool_audit_enabled()
            || n00b_system_pool_audit_enabled()) {
            rc = fail("audit reported enabled in a build without it compiled");
        }
        else {
            n00b_system_pool_audit_stats_t a = n00b_pool_audit_stats(&g_audited);
            if (a.total_alloc_count != 0 || a.live_bytes != 0
                || a.top_count != 0) {
                rc = fail("audit reported numbers in a build without it "
                          "compiled");
            }
        }
        if (rc == 0) {
            fprintf(stderr,
                    "test_pool_audit: PASS — audit absent from this build and "
                    "reported as absent (-Ddebug_live_census=true compiles "
                    "it)\n");
        }
        n00b_shutdown();
        return rc;
    }

    if (!n00b_pool_alloc_audit_enabled(audited)) {
        n00b_shutdown();
        return fail("an .alloc_audit pool did not register");
    }
    if (!n00b_user_pool_audit_enabled()
        || !n00b_conduit_pool_audit_enabled()) {
        n00b_shutdown();
        return fail("a runtime pool opened with .alloc_audit reports disabled");
    }
    if (n00b_system_pool_audit_enabled()) {
        n00b_shutdown();
        return fail("system_pool unexpectedly reports audit enabled");
    }

    for (int i = 0; i < AUDIT_LIVE_ALLOCS; i++) {
        g_live[i] = n00b_alloc_with_opts(
            audit_obj_t,
            &(n00b_alloc_opts_t){.allocator = audited});
        if (g_live[i] == nullptr) {
            n00b_shutdown();
            return fail("allocation from the audited pool returned null");
        }
    }

    n00b_system_pool_audit_stats_t a = n00b_pool_audit_stats(&g_audited);

    int rc = 0;
    if (a.total_alloc_count < AUDIT_LIVE_ALLOCS) {
        rc = fail("alloc_count under-counts allocations this test made");
    }
    else if (a.live_alloc_count < AUDIT_LIVE_ALLOCS) {
        rc = fail("live_alloc_count is short of the allocations still held");
    }
    else if (a.live_bytes < (uint64_t)AUDIT_LIVE_ALLOCS * AUDIT_OBJ_BYTES) {
        rc = fail("live_bytes is below the bytes this test is holding");
    }
    else if (a.top_count == 0) {
        rc = fail("no top site reported for a pool holding live allocations");
    }
    else if (a.top_site[0] == nullptr || a.top_site[0][0] == '\0') {
        rc = fail("top site 0 is empty on a pool holding live allocations");
    }
    else if (strstr(a.top_site[0], "test_pool_audit.c") == nullptr) {
        fprintf(stderr,
                "test_pool_audit: FAIL — top site 0 is \"%s\", expected this "
                "file\n",
                a.top_site[0]);
        rc = 1;
    }
    else if (a.top_live_bytes[0]
             < (uint64_t)AUDIT_LIVE_ALLOCS * AUDIT_OBJ_BYTES) {
        rc = fail("top site 0 under-reports the bytes attributed to it");
    }

    // Live bytes must fall after the allocations are released.
    if (rc == 0) {
        uint64_t live_before = a.live_bytes;
        for (int i = 0; i < AUDIT_LIVE_ALLOCS; i++) {
            n00b_free(g_live[i]);
            g_live[i] = nullptr;
        }
        n00b_system_pool_audit_stats_t b = n00b_pool_audit_stats(&g_audited);
        if (b.total_free_count < AUDIT_LIVE_ALLOCS) {
            rc = fail("frees were not counted");
        }
        else if (b.live_bytes >= live_before) {
            rc = fail("live_bytes did not fall after the allocations were "
                      "freed");
        }
    }

    if (rc == 0) {
        fprintf(stderr,
                "test_pool_audit: PASS — %llu live bytes attributed to %s\n",
                (unsigned long long)a.live_bytes,
                a.top_site[0]);
    }

    n00b_shutdown();
    return rc;
}
