#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "text/regex/charset.h"

// ============================================================================
// BDD boolean algebra
// ============================================================================

static void
test_bdd_true_false(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();

    assert(n00b_regex_charset_is_full(&s, s.true_id));
    assert(n00b_regex_charset_is_empty(&s, s.false_id));
    assert(!n00b_regex_charset_is_full(&s, s.false_id));
    assert(!n00b_regex_charset_is_empty(&s, s.true_id));

    printf("  [PASS] bdd_true_false\n");
}

static void
test_bdd_or_identity(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_charset_t a = n00b_regex_charset_single(&s, 'A');

    // a | FALSE = a
    assert(n00b_regex_charset_or(&s, a, s.false_id) == a);
    // a | TRUE = TRUE
    assert(n00b_regex_charset_or(&s, a, s.true_id) == s.true_id);
    // a | a = a
    assert(n00b_regex_charset_or(&s, a, a) == a);

    printf("  [PASS] bdd_or_identity\n");
}

static void
test_bdd_and_identity(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_charset_t a = n00b_regex_charset_single(&s, 'A');

    // a & TRUE = a
    assert(n00b_regex_charset_and(&s, a, s.true_id) == a);
    // a & FALSE = FALSE
    assert(n00b_regex_charset_and(&s, a, s.false_id) == s.false_id);
    // a & a = a
    assert(n00b_regex_charset_and(&s, a, a) == a);

    printf("  [PASS] bdd_and_identity\n");
}

static void
test_bdd_not_involution(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_charset_t a = n00b_regex_charset_single(&s, 'A');

    // NOT(NOT(a)) = a — not necessarily the same BDD index, but same membership
    n00b_regex_charset_t not_a = n00b_regex_charset_not(&s, a);
    n00b_regex_charset_t not_not_a = n00b_regex_charset_not(&s, not_a);

    // Should contain 'A'
    assert(n00b_regex_charset_contains(&s, not_not_a, 'A'));
    // Should not contain 'B'
    assert(!n00b_regex_charset_contains(&s, not_not_a, 'B'));

    printf("  [PASS] bdd_not_involution\n");
}

// ============================================================================
// Membership tests
// ============================================================================

static void
test_charset_single(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_charset_t cs = n00b_regex_charset_single(&s, 'Z');

    assert(n00b_regex_charset_contains(&s, cs, 'Z'));
    assert(!n00b_regex_charset_contains(&s, cs, 'A'));
    assert(!n00b_regex_charset_contains(&s, cs, 0));

    printf("  [PASS] charset_single\n");
}

static void
test_charset_range(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_charset_t cs = n00b_regex_charset_range(&s, 'a', 'z');

    for (char c = 'a'; c <= 'z'; c++) {
        assert(n00b_regex_charset_contains(&s, cs, c));
    }
    assert(!n00b_regex_charset_contains(&s, cs, 'A'));
    assert(!n00b_regex_charset_contains(&s, cs, '0'));
    assert(!n00b_regex_charset_contains(&s, cs, 0x7F));

    printf("  [PASS] charset_range\n");
}

static void
test_charset_unicode(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    // Test single unicode codepoint
    n00b_regex_charset_t cs = n00b_regex_charset_single(&s, 0x1F600); // 😀

    assert(n00b_regex_charset_contains(&s, cs, 0x1F600));
    assert(!n00b_regex_charset_contains(&s, cs, 0x1F601));
    assert(!n00b_regex_charset_contains(&s, cs, 'A'));

    printf("  [PASS] charset_unicode\n");
}

// ============================================================================
// Minterm tests
// ============================================================================

static void
test_minterms_basic(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();

    // Two predicates: [a-z] and [a-m]
    n00b_regex_charset_t p1 = n00b_regex_charset_range(&s, 'a', 'z');
    n00b_regex_charset_t p2 = n00b_regex_charset_range(&s, 'a', 'm');

    n00b_regex_charset_t preds[] = {p1, p2};
    n00b_regex_minterm_table_t *mt = n00b_regex_compute_minterms(&s, preds, 2);

    // Should produce 3 minterms: [a-m], [n-z], everything else
    assert(mt->count == 3);

    // 'a' and 'm' should be in the same minterm
    n00b_regex_minterm_id_t ma = n00b_regex_minterm_classify(mt, 'a');
    n00b_regex_minterm_id_t mm = n00b_regex_minterm_classify(mt, 'm');
    assert(ma == mm);

    // 'n' should be in a different minterm
    n00b_regex_minterm_id_t mn = n00b_regex_minterm_classify(mt, 'n');
    assert(mn != ma);

    // 'z' should be in the same minterm as 'n'
    n00b_regex_minterm_id_t mz = n00b_regex_minterm_classify(mt, 'z');
    assert(mz == mn);

    // '0' should be in yet another minterm (the "everything else")
    n00b_regex_minterm_id_t m0 = n00b_regex_minterm_classify(mt, '0');
    assert(m0 != ma && m0 != mn);

    printf("  [PASS] minterms_basic\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex charset tests...\n");

    test_bdd_true_false();
    test_bdd_or_identity();
    test_bdd_and_identity();
    test_bdd_not_involution();
    test_charset_single();
    test_charset_range();
    test_charset_unicode();
    test_minterms_basic();

    printf("All regex charset tests passed.\n");
    n00b_shutdown();
    return 0;
}
