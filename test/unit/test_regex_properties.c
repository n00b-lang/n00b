// Phase 11 typed translation of resharp-c/tests/properties_test.c.
//
// The original test exercised the internal regex-builder / parser / engine
// surface (RegexBuilder, NodeId, MinMax, regex_builder_*, resharp_parser_parse_ast,
// Regex / regex_new / regex_has_accel / regex_bdfa_stats_is_some).  The internal
// names are preserved in n00b-regex (algorithmic vocabulary is intentionally
// unprefixed — see internal/regex/algebra.h, internal/regex/regex.h,
// internal/regex/parser.h).  Pattern data is byte-identical; only the test
// scaffolding, asserts, and includes are translated per § 7.5 + § 19a.

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"

#include "internal/regex/algebra.h"
#include "internal/regex/parser.h"
#include "internal/regex/regex.h"

// ---------------------------------------------------------------------------
// Option<u32> helper
// ---------------------------------------------------------------------------

typedef struct {
    bool     present;
    uint32_t value;
} OptU32;

// ---------------------------------------------------------------------------
// Helpers mirroring the Rust top-level functions.
// ---------------------------------------------------------------------------

static OptU32
fixed_length(const char *pattern)
{
    RegexBuilder *b = regex_builder_new(nullptr);
    NodeId node = (NodeId){};
    bool ok = resharp_parser_parse_ast(b, pattern, &node);
    assert(ok); // .unwrap()
    OptU32 out = {.present = false, .value = 0};
    out.present = regex_builder_get_fixed_length(b, node, &out.value);
    regex_builder_free(b);
    return out;
}

static MinMax
min_max(const char *pattern)
{
    RegexBuilder *b = regex_builder_new(nullptr);
    NodeId node = (NodeId){};
    bool ok = resharp_parser_parse_ast(b, pattern, &node);
    assert(ok);
    MinMax mm = regex_builder_get_min_max_length(b, node);
    regex_builder_free(b);
    return mm;
}

static bool
is_infinite(const char *pattern)
{
    RegexBuilder *b = regex_builder_new(nullptr);
    NodeId node = (NodeId){};
    bool ok = resharp_parser_parse_ast(b, pattern, &node);
    assert(ok);
    bool r = regex_builder_is_infinite(b, node);
    regex_builder_free(b);
    return r;
}

static bool
has_look(const char *pattern)
{
    RegexBuilder *b = regex_builder_new(nullptr);
    NodeId node = (NodeId){};
    bool ok = resharp_parser_parse_ast(b, pattern, &node);
    assert(ok);
    bool r = regex_builder_contains_look(b, node);
    regex_builder_free(b);
    return r;
}

static bool
has_anchors(const char *pattern)
{
    RegexBuilder *b = regex_builder_new(nullptr);
    NodeId node = (NodeId){};
    bool ok = resharp_parser_parse_ast(b, pattern, &node);
    assert(ok);
    bool r = regex_builder_contains_anchors(b, node);
    regex_builder_free(b);
    return r;
}

static bool
bdfa_eligible(const char *pattern)
{
    OptU32 fl = fixed_length(pattern);
    MinMax mm = min_max(pattern);
    // max_length = if max != u32::MAX { Some(max) } else { None }
    bool max_length_some = (mm.max != UINT32_MAX);
    return max_length_some && !fl.present && !has_look(pattern)
           && !has_anchors(pattern);
}

typedef struct {
    bool has_bdfa;
    bool fwd_accel;
    bool rev_accel;
} DispatchInfo;

static DispatchInfo
dispatch_info(const char *pattern)
{
    Regex *re = regex_new(pattern);
    assert(re != nullptr); // .unwrap()
    DispatchInfo di = {false, false, false};
    regex_has_accel(re, &di.fwd_accel, &di.rev_accel);
    di.has_bdfa = regex_bdfa_stats_is_some(re);
    regex_free(re);
    return di;
}

// ---------------------------------------------------------------------------
// Tests — fixed_length
// ---------------------------------------------------------------------------

static void
test_fixed_literal(void)
{
    OptU32 o = fixed_length("abc");
    assert(o.present);
    assert(o.value == 3);
    printf("  [PASS] fixed_literal\n");
}

static void
test_fixed_pred(void)
{
    OptU32 o = fixed_length("[A-Z]");
    assert(o.present);
    assert(o.value == 1);
    printf("  [PASS] fixed_pred\n");
}

static void
test_fixed_union_same(void)
{
    OptU32 o = fixed_length("abc|def");
    assert(o.present);
    assert(o.value == 3);
    printf("  [PASS] fixed_union_same\n");
}

static void
test_fixed_union_different(void)
{
    OptU32 o = fixed_length("ab|cde");
    assert(!o.present);
    printf("  [PASS] fixed_union_different\n");
}

static void
test_fixed_repeat_exact(void)
{
    OptU32 o = fixed_length("[A-Z]{5}");
    assert(o.present);
    assert(o.value == 5);
    printf("  [PASS] fixed_repeat_exact\n");
}

static void
test_fixed_repeat_range(void)
{
    OptU32 o = fixed_length("[A-Z]{3,5}");
    assert(!o.present);
    printf("  [PASS] fixed_repeat_range\n");
}

// ---------------------------------------------------------------------------
// Tests — min_max
// ---------------------------------------------------------------------------

static void
test_minmax_literal(void)
{
    MinMax m = min_max("abc");
    assert(m.min == 3);
    assert(m.max == 3);
    printf("  [PASS] minmax_literal\n");
}

static void
test_minmax_bounded_repeat(void)
{
    MinMax m = min_max("[A-Za-z]{8,13}");
    assert(m.min == 8);
    assert(m.max == 13);
    printf("  [PASS] minmax_bounded_repeat\n");
}

static void
test_minmax_star(void)
{
    MinMax m = min_max("a*");
    assert(m.min == 0);
    assert(m.max == UINT32_MAX);
    printf("  [PASS] minmax_star\n");
}

static void
test_minmax_plus(void)
{
    MinMax m = min_max("a+");
    assert(m.min == 1);
    assert(m.max == UINT32_MAX);
    printf("  [PASS] minmax_plus\n");
}

static void
test_minmax_optional(void)
{
    MinMax m = min_max("a?");
    assert(m.min == 0);
    assert(m.max == 1);
    printf("  [PASS] minmax_optional\n");
}

static void
test_minmax_union(void)
{
    MinMax m = min_max("ab|cde");
    assert(m.min == 2);
    assert(m.max == 3);
    printf("  [PASS] minmax_union\n");
}

static void
test_minmax_concat_bounded(void)
{
    MinMax m = min_max("a{2,3}b{1,2}");
    assert(m.min == 3);
    assert(m.max == 5);
    printf("  [PASS] minmax_concat_bounded\n");
}

static void
test_minmax_dotstar_literal(void)
{
    MinMax m = min_max(".*abc");
    assert(m.min == 3);
    assert(m.max == UINT32_MAX);
    printf("  [PASS] minmax_dotstar_literal\n");
}

static void
test_minmax_aws_key(void)
{
    MinMax m = min_max("(?:ASIA|AKIA|AROA|AIDA)[A-Z0-7]{16}");
    assert(m.min == 20);
    assert(m.max == 20);
    printf("  [PASS] minmax_aws_key\n");
}

static void
test_minmax_alt_suffix(void)
{
    // "Sherlock" = 8, "Holmes" = 6, suffix = 0..5
    MinMax m = min_max("(Sherlock|Holmes)[a-z]{0,5}");
    assert(m.min == 6);
    assert(m.max == 13);
    printf("  [PASS] minmax_alt_suffix\n");
}

// ---------------------------------------------------------------------------
// Tests — is_infinite
// ---------------------------------------------------------------------------

static void
test_inf_star(void)
{
    assert(is_infinite("a*"));
    printf("  [PASS] inf_star\n");
}

static void
test_inf_plus(void)
{
    assert(is_infinite("a+"));
    printf("  [PASS] inf_plus\n");
}

static void
test_inf_bounded(void)
{
    assert(!is_infinite("[A-Za-z]{8,13}"));
    printf("  [PASS] inf_bounded\n");
}

static void
test_inf_literal(void)
{
    assert(!is_infinite("abc"));
    printf("  [PASS] inf_literal\n");
}

static void
test_inf_dotstar_prefix(void)
{
    assert(is_infinite(".*abc"));
    printf("  [PASS] inf_dotstar_prefix\n");
}

static void
test_inf_optional(void)
{
    assert(!is_infinite("a?"));
    printf("  [PASS] inf_optional\n");
}

// ---------------------------------------------------------------------------
// Tests — has_look
// ---------------------------------------------------------------------------

static void
test_look_lookahead(void)
{
    assert(has_look("a(?=b)"));
    printf("  [PASS] look_lookahead\n");
}

static void
test_look_lookbehind(void)
{
    assert(has_look("(?<=a)b"));
    printf("  [PASS] look_lookbehind\n");
}

static void
test_look_word_boundary(void)
{
    // \b in concat context is rewritten to lookaround
    assert(has_look("\\bfoo\\b"));
    printf("  [PASS] look_word_boundary\n");
}

static void
test_look_none(void)
{
    assert(!has_look("abc"));
    printf("  [PASS] look_none\n");
}

// ---------------------------------------------------------------------------
// Tests — bdfa_eligible
// ---------------------------------------------------------------------------

static void
test_bdfa_bounded_repeat(void)
{
    assert(bdfa_eligible("[A-Za-z]{8,13}"));
    printf("  [PASS] bdfa_bounded_repeat\n");
}

static void
test_bdfa_alt_suffix(void)
{
    assert(bdfa_eligible("(Sherlock|Holmes)[a-z]{0,5}"));
    printf("  [PASS] bdfa_alt_suffix\n");
}

static void
test_bdfa_not_fixed(void)
{
    // fixed length uses faster path
    assert(!bdfa_eligible("abc"));
    printf("  [PASS] bdfa_not_fixed\n");
}

static void
test_bdfa_not_unbounded(void)
{
    assert(!bdfa_eligible("a+"));
    printf("  [PASS] bdfa_not_unbounded\n");
}

static void
test_bdfa_not_look(void)
{
    assert(!bdfa_eligible("(?<=\\s)[A-Z]{3,5}"));
    printf("  [PASS] bdfa_not_look\n");
}

static void
test_bdfa_union_variable(void)
{
    assert(bdfa_eligible("ab|cde"));
    printf("  [PASS] bdfa_union_variable\n");
}

static void
test_bdfa_aws_key(void)
{
    assert(!bdfa_eligible("(?:ASIA|AKIA|AROA|AIDA)[A-Z0-7]{16}"));
    printf("  [PASS] bdfa_aws_key\n");
}

static void
test_bdfa_phone_bounded(void)
{
    assert(!bdfa_eligible("[0-9_ \\-()]{7,}"));
    printf("  [PASS] bdfa_phone_bounded\n");
}

// ---------------------------------------------------------------------------
// Tests — dispatch_info
// ---------------------------------------------------------------------------

static void
test_dispatch_literal(void)
{
    DispatchInfo di = dispatch_info("Sherlock Holmes");
    assert(!di.has_bdfa);
    printf("  [PASS] dispatch_literal\n");
}

static void
test_dispatch_word_boundary_the(void)
{
    DispatchInfo di = dispatch_info("\\bthe\\b");
    // \bthe\b should have rev accel via strip_lb fallback
    assert(di.rev_accel);
    printf("  [PASS] dispatch_word_boundary_the\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex properties tests...\n");

    test_fixed_literal();
    test_fixed_pred();
    test_fixed_union_same();
    test_fixed_union_different();
    test_fixed_repeat_exact();
    test_fixed_repeat_range();
    test_minmax_literal();
    test_minmax_bounded_repeat();
    test_minmax_star();
    test_minmax_plus();
    test_minmax_optional();
    test_minmax_union();
    test_minmax_concat_bounded();
    test_minmax_dotstar_literal();
    test_minmax_aws_key();
    test_minmax_alt_suffix();
    test_inf_star();
    test_inf_plus();
    test_inf_bounded();
    test_inf_literal();
    test_inf_dotstar_prefix();
    test_inf_optional();
    test_look_lookahead();
    test_look_lookbehind();
    test_look_word_boundary();
    test_look_none();
    test_bdfa_bounded_repeat();
    test_bdfa_alt_suffix();
    test_bdfa_not_fixed();
    test_bdfa_not_unbounded();
    test_bdfa_not_look();
    test_bdfa_union_variable();
    test_bdfa_aws_key();
    test_bdfa_phone_bounded();
    test_dispatch_literal();
    test_dispatch_word_boundary_the();

    printf("All regex properties tests passed.\n");
    n00b_shutdown();
    return 0;
}
