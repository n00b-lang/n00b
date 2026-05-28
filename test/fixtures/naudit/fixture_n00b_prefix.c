/*
 * WP-009 Phase 4 fixture — canonical n00b_-prefix rule target.
 *
 * Mixed function calls: some properly n00b_-prefixed, some not.
 * The canonical rule
 * `n00b.s_naming.public_call_prefix` against this file should
 * report violations for every non-prefixed call.
 *
 * To keep the count deterministic the fixture restricts itself to
 * straight identifier-form function calls (no member-access calls
 * like `o.foo()`, no chained postfix expressions) so the
 * `<postfix_expression>` violation NT + capture binding semantics
 * resolve unambiguously. The filter's `is_call()` check additionally
 * narrows the over-fire to call forms only (rejecting array
 * subscripts + increment/decrement; see `src/naudit/filter.c`).
 *
 * Expected violations (per Phase 4 spec):
 *   - line  21 col 5  -- bar(x, y)
 *   - line  22 col 5  -- baz()
 *
 * `n00b_foo(x, y)` and `n00b_qux()` pass the filter (their first
 * identifier descendant starts with `n00b_`).
 *
 * Array-subscript `arr[0]` exists in the fixture to exercise the
 * `is_call()` narrowing — it must NOT be reported as a violation
 * even though `<postfix_expression>` matches it (per c_ncc.bnf
 * line 536 / D-020).
 */

int n00b_foo(int a, int b);
int bar(int a, int b);
int n00b_qux(void);
int baz(void);

int
helper(void)
{
    int x = 1;
    int y = 2;
    int arr[4];
    n00b_foo(x, y);    /* call — passes filter (starts with n00b_) */
    bar(x, y);         /* call — FAILS filter, line 21 */
    baz();             /* call — FAILS filter, line 22 */
    n00b_qux();        /* call — passes filter */
    return arr[0];     /* postfix_expression but NOT a call */
}
