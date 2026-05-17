/** @file test/unit/test_attest_module.c — libn00b_attest skeleton test.
 *
 *  Phase-1 regression test for the n00b_attest module skeleton.
 *  Verifies (a) the public umbrella header resolves through the
 *  ncc include path and (b) `n00b_attest_module_init` is reachable
 *  via the linked libn00b. The two failure modes this guards
 *  against are meson-wiring regressions (init symbol unresolved)
 *  and header-include path regressions (umbrella header missing
 *  from the install include set).
 *
 *  When Phase 2 lands real Statement / DSSE bodies, this file
 *  stays as the minimum link-and-load guard; Phase 2 adds
 *  separate behavior tests next to it.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "attest/n00b_attest.h"

static void
test_module_init_resolves(void)
{
    n00b_attest_module_init();
    printf("  [PASS] module_init_resolves\n");
}

static void
test_api_version_macro(void)
{
    assert(N00B_ATTEST_API_VERSION == 1);
    printf("  [PASS] api_version_macro\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== n00b_attest module skeleton ==\n");
    test_module_init_resolves();
    test_api_version_macro();

    printf("All n00b_attest module skeleton tests passed.\n");
    return 0;
}
