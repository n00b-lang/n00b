/*
 * WP-009 Phase 2 fixture — query-mode rule target.
 *
 * The Phase 2 test rule names `<function_definition>` as its
 * violation NT and ships no `bnf_fragment` (query-mode). The
 * engine queries the loaded C grammar's tree directly.
 *
 * This fixture contains EXACTLY TWO function definitions
 * (`helper` and `main`). The test asserts that the engine
 * emits exactly two violations against it.
 *
 * No `NULL` / no other audited construct appears here — the
 * fixture is dedicated to the function-definition count
 * assertion.
 */

int
helper(int x)
{
    return x + 1;
}

int
main(void)
{
    return helper(0);
}
