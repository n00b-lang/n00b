/* src/attest/n00b_attest_module.c — Phase-1 module stub.
 *
 * Sole purpose: expose a single symbol (`n00b_attest_module_init`)
 * so the meson hookup laid down in this WP is exercised
 * end-to-end by the build smoke. If the module is mis-routed,
 * the link of the libn00b static archive (which aggregates the
 * `n00b_attest_src` list per the top-level meson.build) will
 * fail to surface this symbol and the smoke fails.
 *
 * No allocations. No globals. No constructors. The Statement
 * builder, the DSSE envelope encoder/decoder, and the rest of
 * the surface declared in `include/attest/` arrive in WP-001
 * Phase 2 as proper `.c` translation units.
 */

#include <attest/n00b_attest.h>

void
n00b_attest_module_init(void)
{
}
