// Smoke test for the Phase 4.5 unicode by-name + ranges + Age APIs.
// Verifies that the property kinds whose tables were broken/missing in the
// upstream resharp-c Python-generated regex_syntax_unicode_tables.h now
// resolve to non-empty range sets via the canonical n00b unicode tree.
// Also exercises the Script / Block / Age / Property / Script_Extensions /
// segmentation-break by-name paths added in Phase 4.5.
//
// Upstream counterpart: ~/resharp-c/tests/smoke_unicode.c.  That test
// reached into the parser-internal `resolve_unicode_class` helper; n00b
// exposes the same data via the public unicode-API surface, so this
// typed translation calls those APIs directly rather than going through
// the regex parser.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "text/unicode/properties.h"
#include "text/unicode/query.h"
#include "text/unicode/segmentation.h"
#include "text/unicode/types.h"
#include "text/unicode/types_ext.h"

static int failures = 0;
static int passes   = 0;

static void
record(const char *label, bool cond, const char *detail)
{
    if (cond) {
        passes++;
        printf("PASS  %-44s %s\n", label, detail ? detail : "");
    }
    else {
        failures++;
        printf("FAIL  %-44s %s\n", label, detail ? detail : "");
    }
}

// Sum of (end - start + 1) over a sorted, non-overlapping range array.
static uint64_t
range_count_codepoints(const n00b_codepoint_pair_t *ranges, size_t len)
{
    uint64_t total = 0;
    for (size_t i = 0; i < len; i++) {
        total += (uint64_t)ranges[i].hi - (uint64_t)ranges[i].lo + 1;
    }
    return total;
}

// ---- GC by name -> ranges --------------------------------------------------

static void
check_gc_nonempty(const char *label, const char *name)
{
    n00b_unicode_gc_t gc = {};
    bool resolved = n00b_unicode_gc_by_name(name, &gc);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_general_category_ranges(gc, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Derived GC by name -> ranges ------------------------------------------

static void
check_gc_derived_nonempty(const char *label, const char *name)
{
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    bool ok = n00b_unicode_gc_derived_ranges(name, &r, &len);
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, ok && len > 0, buf);
}

// ---- Script by name -> ranges ----------------------------------------------

static void
check_script_nonempty(const char *label, const char *name)
{
    n00b_unicode_script_t sc = {};
    bool resolved = n00b_unicode_script_by_name(name, &sc);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_script_ranges(sc, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Script_Extensions by name -> ranges -----------------------------------

static void
check_scx_nonempty(const char *label, const char *script_name)
{
    n00b_unicode_script_t sc = {};
    bool resolved = n00b_unicode_script_by_name(script_name, &sc);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_script_extensions_ranges(sc, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Binary property by name -> ranges -------------------------------------

static void
check_property_nonempty(const char *label, const char *name)
{
    n00b_unicode_property_t prop = {};
    bool resolved = n00b_unicode_property_by_name(name, &prop);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_property_ranges(prop, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Block by name -> ranges -----------------------------------------------

static void
check_block_nonempty(const char *label, const char *name)
{
    n00b_unicode_block_t bl = {};
    bool resolved = n00b_unicode_block_by_name(name, &bl);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_block_ranges_for(bl, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Age by name -> cumulative ranges --------------------------------------

static void
check_age_nonempty(const char *label, const char *name)
{
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    bool ok = n00b_unicode_age_ranges(name, &r, &len);
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, ok && len > 0, buf);
}

// ---- Bidi_Class by name -> ranges ------------------------------------------

static void
check_bidi_nonempty(const char *label, const char *name)
{
    n00b_unicode_bidi_class_t bc = {};
    bool resolved = n00b_unicode_bidi_class_by_name(name, &bc);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_bidi_class_ranges(bc, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

// ---- Grapheme/Word/Sentence/Line Break by name -> ranges -------------------

static void
check_gcb_nonempty(const char *label, const char *name)
{
    n00b_unicode_gcb_t v = {};
    bool resolved = n00b_unicode_gcb_by_name(name, &v);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_grapheme_break_ranges(v, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

static void
check_wb_nonempty(const char *label, const char *name)
{
    n00b_unicode_wb_t v = {};
    bool resolved = n00b_unicode_wb_by_name(name, &v);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_word_break_ranges(v, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

static void
check_sb_nonempty(const char *label, const char *name)
{
    n00b_unicode_sb_t v = {};
    bool resolved = n00b_unicode_sb_by_name(name, &v);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_sentence_break_ranges(v, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

static void
check_lb_nonempty(const char *label, const char *name)
{
    n00b_unicode_lb_t v = {};
    bool resolved = n00b_unicode_lb_by_name(name, &v);
    const n00b_codepoint_pair_t *r = nullptr;
    size_t len = 0;
    if (resolved) {
        n00b_unicode_line_break_ranges(v, &r, &len);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "ranges=%zu cps=%llu",
             len, (unsigned long long)range_count_codepoints(r, len));
    record(label, resolved && len > 0, buf);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // ---- Bug-fix coverage: the categories whose tables were empty before
    // the lift.  Every single one of these would assert "ranges=0" against
    // the broken upstream tables.
    check_gc_nonempty("\\p{Cc}  (Control - was empty)",            "Cc");
    check_gc_nonempty("\\p{Cf}  (Format - was empty)",             "Cf");
    check_gc_nonempty("\\p{Mn}  (Nonspacing_Mark - was empty)",    "Mn");
    check_gc_nonempty("\\p{Me}  (Enclosing_Mark - was empty)",     "Me");
    check_gc_nonempty("\\p{Zl}  (Line_Separator - was empty)",     "Zl");
    check_gc_nonempty("\\p{Zp}  (Paragraph_Separator - was empty)","Zp");
    check_gc_nonempty("\\p{Co}  (Private_Use - was empty)",        "Co");
    check_gc_nonempty("\\p{Cn}  (Unassigned - was empty)",         "Cn");
    check_gc_derived_nonempty("\\p{C}   (Other (derived) - was empty)", "C");

    // ---- Spot-checks for the categories that DID populate correctly.
    check_gc_nonempty("\\p{Lu}", "Lu");
    check_gc_derived_nonempty("\\p{L}",  "L");
    check_gc_nonempty("\\p{Nd}", "Nd");

    // ---- Pseudo / derived categories.
    // "ASCII", "Any", "Assigned" are regex-syntax pseudo classes, not in
    // n00b's by-name table; they are constructed via filter_range / scans.
    {
        // ASCII == [0..0x7F].
        n00b_array_t(n00b_codepoint_t) cps = n00b_cp_query(
            n00b_filter_range(0u, 0x7Fu));
        char buf[64];
        snprintf(buf, sizeof(buf), "cps=%u", (unsigned)cps.len);
        record("\\p{ASCII}",    cps.len == 0x80, buf);
    }
    {
        // \p{Any} spans the full codespace 0..0x10FFFF *including* the
        // surrogate range (U+D800..U+DFFF).  Use cp_query_n directly with
        // the `include_surrogates` kwarg so we don't drop them by default.
        n00b_cp_filter_t filters[] = { n00b_filter_range(0u, 0x10FFFFu) };
        n00b_array_t(n00b_codepoint_t) cps = n00b_cp_query_n(
            filters, 1, .include_surrogates = true);
        char buf[96];
        snprintf(buf, sizeof(buf), "cps=%u (expected 1114112)",
                 (unsigned)cps.len);
        record("\\p{Any} covers 0..0x10FFFF",
               cps.len == 0x110000u, buf);
    }
    {
        // Assigned == not GC=Cn.
        const n00b_codepoint_pair_t *r = nullptr;
        size_t len = 0;
        n00b_unicode_general_category_ranges(N00B_UNICODE_GC_CN, &r, &len);
        uint64_t cn_cps = range_count_codepoints(r, len);
        char buf[96];
        snprintf(buf, sizeof(buf), "Cn ranges=%zu cps=%llu",
                 len, (unsigned long long)cn_cps);
        record("\\p{Assigned} (== !Cn)", len > 0 && cn_cps > 0, buf);
    }

    // ---- One-letter (\pL, \pN, etc.) — by-name "L" / "N".
    check_gc_derived_nonempty("\\pL  (one-letter L)", "L");
    check_gc_derived_nonempty("\\pN  (one-letter N)", "N");

    // ---- Script.
    check_script_nonempty("\\p{Greek}",        "Greek");
    check_script_nonempty("\\p{Latin}",        "Latin");
    check_script_nonempty("\\p{Script=Greek}", "Greek");
    check_script_nonempty("\\p{sc=Latin}",     "Latin");

    // ---- Script_Extensions.
    check_scx_nonempty("\\p{scx=Hira}", "Hiragana");

    // ---- Binary properties.
    check_property_nonempty("\\p{Alphabetic}", "Alphabetic");
    check_property_nonempty("\\p{White_Space}","White_Space");
    check_property_nonempty("\\p{Emoji}",      "Emoji");
    check_property_nonempty("\\p{XID_Start}",  "XID_Start");

    // ---- Block.
    check_block_nonempty("\\p{Block=BasicLatin}",      "Basic Latin");
    check_block_nonempty("\\p{InBasicLatin}",          "Basic Latin");
    check_block_nonempty("\\p{blk=GreekAndCoptic}",    "Greek and Coptic");

    // ---- Age (cumulative).
    check_age_nonempty("\\p{Age=12.0}", "12.0");
    check_age_nonempty("\\p{age=V1_1}", "V1_1");

    // ---- Bidi_Class.
    check_bidi_nonempty("\\p{bc=L}", "L");

    // ---- Grapheme_Cluster_Break / Word_Break / Sentence_Break / Line_Break.
    check_gcb_nonempty("\\p{gcb=Extend}",                  "Extend");
    check_gcb_nonempty("\\p{Grapheme_Cluster_Break=Extend}","Extend");
    check_wb_nonempty("\\p{wb=ALetter}",                   "ALetter");
    check_wb_nonempty("\\p{Word_Break=ALetter}",           "ALetter");
    check_sb_nonempty("\\p{sb=ATerm}",                     "ATerm");
    check_sb_nonempty("\\p{Sentence_Break=ATerm}",         "ATerm");
    check_lb_nonempty("\\p{lb=SP}",                        "SP");
    check_lb_nonempty("\\p{Line_Break=Space}",             "Space");

    // ---- Range-count fixity check: \p{Nd} == GC=Decimal_Number.
    // regex-syntax 0.8.10 / Unicode 16 has 71 ranges for Nd.
    {
        n00b_unicode_gc_t gc = {};
        bool resolved = n00b_unicode_gc_by_name("Nd", &gc);
        const n00b_codepoint_pair_t *r = nullptr;
        size_t len = 0;
        if (resolved) {
            n00b_unicode_general_category_ranges(gc, &r, &len);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "ranges=%zu (expected 71)", len);
        record("\\p{Nd} == GC=Decimal_Number ranges",
               resolved && len == 71u, buf);
    }

    // \s (Perl) == \p{White_Space}; in Unicode 16 the table has >= 10 ranges.
    {
        n00b_unicode_property_t prop = {};
        bool resolved = n00b_unicode_property_by_name("White_Space", &prop);
        const n00b_codepoint_pair_t *r = nullptr;
        size_t len = 0;
        if (resolved) {
            n00b_unicode_property_ranges(prop, &r, &len);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "ranges=%zu (expected >=10)", len);
        record("\\s (== White_Space) ranges", resolved && len >= 10u, buf);
    }

    printf("\n%d pass / %d fail\n", passes, failures);

    // Use bare assert() for the test-runner exit signal (per the smoke-test
    // contract: any failure is a hard failure).
    assert(failures == 0);

    n00b_shutdown();
    return failures == 0 ? 0 : 1;
}
