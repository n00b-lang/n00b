/*
 * WP-001 Phase 1 skeleton regression test.
 *
 * Calls the public `n00b_audit_module_init` through the umbrella
 * header and asserts the API-version macro is at its expected
 * value. Per process § 6.5b's skeleton-phase rule, this single test
 * catches future meson-wiring + header-include regressions for the
 * `n00b-audit` project as Phases 2+ add real subsystems.
 *
 * Test files operate under the n00b-api-guidelines' relaxed test
 * convention — libc <assert.h> + <stdio.h> are acceptable here for
 * test harness scaffolding (NCC.md "NO LIBC ALLOWED" exemption for
 * test files, mirrored by n00b's own
 * /Users/viega/n00b/test/unit/test_chalk_module.c precedent which
 * uses `assert.h` + `stdio.h` directly).
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"

#include "naudit/naudit.h"

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    static_assert(N00B_AUDIT_API_VERSION == 1,
                  "N00B_AUDIT_API_VERSION must be 1 in WP-001 Phase 1");

    bool ok = n00b_audit_module_init();
    assert(ok);

    printf("  [PASS] audit_module_init\n");
    printf("All n00b-audit Phase 1 regression checks passed.\n");
    return 0;
}
