// Phase 11 typed translation of resharp-c/tests/smoke_utf8seq.c.
//
// Parser-side smoke test for the faithful Utf8Sequences port.  Verifies
// that rs_Utf8Sequences_next emits the same byte-range tuples (and in the
// same order) as Rust's regex-syntax 0.8.10 utf8 iterator.  The "acid
// test" is the property that the union of all emitted sequences, expanded
// to byte tuples, equals the set of UTF-8 encodings of all codepoints in
// the input range minus the surrogate hole.
//
// Per § 7.5 the resharp-c file pulled in the parser TU directly (#include
// "../parser/regex_syntax.c") to reach the rs_Utf8Sequences_* iterator.
// The iterator is part of the parser-internal API surface declared in
// `internal/regex/ast.h`, and `encode_utf8` translates to
// `n00b_unicode_utf8_encode` from `text/unicode/encoding.h`.  Data values
// (codepoint ranges, sample table, BMP/full golden tuples, labels) are
// byte-identical with the resharp-c source.

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "text/unicode/encoding.h"
#include "internal/regex/ast.h"

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

// One emitted sequence: nbytes byte ranges, indexed [0..nbytes).
typedef struct {
    size_t       nbytes;
    rs_Utf8Range br[4];
} seq_t;

// Run the iterator and collect all sequences.
// Returns 0 on success, -1 if iterator overflowed our buffer.
static int
collect_sequences(uint32_t lo, uint32_t hi, seq_t *out, size_t cap, size_t *n_out)
{
    rs_Utf8Sequences *it = rs_Utf8Sequences_new(lo, hi);
    size_t            n  = 0;
    rs_Utf8Range      buf[4];
    size_t            k  = 0;
    while (rs_Utf8Sequences_next(it, buf, &k)) {
        if (n >= cap) {
            rs_Utf8Sequences_free(it);
            return -1;
        }
        out[n].nbytes = k;
        for (size_t i = 0; i < k; i++) out[n].br[i] = buf[i];
        n++;
    }
    rs_Utf8Sequences_free(it);
    *n_out = n;
    return 0;
}

// Returns true iff the byte tuple `bytes[0..nbytes]` is matched by `s`.
static bool
seq_matches(const seq_t *s, const uint8_t *bytes, size_t nbytes)
{
    if (s->nbytes != nbytes) return false;
    for (size_t i = 0; i < nbytes; i++) {
        if (bytes[i] < s->br[i].start || bytes[i] > s->br[i].end) return false;
    }
    return true;
}

// Acid test: for every codepoint in [lo, hi] (excluding surrogates), encode
// it to UTF-8 and verify exactly one emitted sequence matches it.
// Also verify that no emitted sequence matches any encoded surrogate.
static void
acid_test(uint32_t lo, uint32_t hi, const seq_t *seqs, size_t nseq,
          const char *label)
{
    char detail[128];

    // 1) Forward direction: every cp in range produces exactly one match.
    uint32_t bad_cp    = 0;
    int      bad_count = -1;
    bool     fwd_ok    = true;
    for (uint32_t cp = lo; cp <= hi; cp++) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) continue;
        char     buf[4];
        uint32_t n       = n00b_unicode_utf8_encode((n00b_codepoint_t)cp, buf);
        int      matches = 0;
        for (size_t i = 0; i < nseq; i++) {
            if (seq_matches(&seqs[i], (const uint8_t *)buf, n)) matches++;
        }
        if (matches != 1) {
            fwd_ok    = false;
            bad_cp    = cp;
            bad_count = matches;
            break;
        }
    }
    if (!fwd_ok) {
        snprintf(detail, sizeof(detail),
                 "[U+%05X..U+%05X] cp U+%05X matched %d times",
                 lo, hi, bad_cp, bad_count);
    }
    else {
        snprintf(detail, sizeof(detail),
                 "[U+%05X..U+%05X] %zu seqs", lo, hi, nseq);
    }
    record(label, fwd_ok, detail);

    // 2) Surrogate exclusion.  No emitted sequence should match the encoding
    //    of any surrogate codepoint.
    bool sur_ok = true;
    for (uint32_t cp = 0xD800u; cp <= 0xDFFFu; cp++) {
        // Manually encode as 3-byte (invalid UTF-8, but matches exercise).
        uint8_t buf[3];
        buf[0] = (uint8_t)(0xE0u | (cp >> 12));
        buf[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        for (size_t i = 0; i < nseq; i++) {
            if (seq_matches(&seqs[i], buf, 3)) { sur_ok = false; break; }
        }
        if (!sur_ok) break;
    }
    snprintf(detail, sizeof(detail), "[U+%05X..U+%05X] no surrogate match",
             lo, hi);
    record("surrogate exclusion", sur_ok, detail);
}

static void
print_seqs(const seq_t *seqs, size_t nseq, const char *label)
{
    printf("  %s: %zu seqs\n", label, nseq);
    for (size_t i = 0; i < nseq; i++) {
        printf("    [%zu]", i);
        for (size_t k = 0; k < seqs[i].nbytes; k++) {
            if (seqs[i].br[k].start == seqs[i].br[k].end)
                printf(" [%02X]", seqs[i].br[k].start);
            else
                printf(" [%02X-%02X]", seqs[i].br[k].start, seqs[i].br[k].end);
        }
        printf("\n");
    }
}

// Compute the number of byte sequences the OLD stub would emit: one per
// codepoint in [lo, hi], minus surrogates.
static size_t
stub_count(uint32_t lo, uint32_t hi)
{
    size_t n = 0;
    for (uint32_t cp = lo; cp <= hi; cp++) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) continue;
        n++;
    }
    return n;
}

static void
run_one(uint32_t lo, uint32_t hi, const char *label, bool dump)
{
    seq_t  seqs[256];
    size_t nseq = 0;
    int    rc   = collect_sequences(lo, hi, seqs,
                                    sizeof(seqs) / sizeof(seqs[0]), &nseq);
    char   detail[128];
    if (rc != 0) {
        snprintf(detail, sizeof(detail),
                 "[U+%05X..U+%05X] iterator overflowed (>256 seqs)", lo, hi);
        record(label, false, detail);
        return;
    }
    snprintf(detail, sizeof(detail),
             "[U+%05X..U+%05X] %zu seqs (stub would emit %zu)",
             lo, hi, nseq, stub_count(lo, hi));
    record(label, true, detail);
    if (dump) print_seqs(seqs, nseq, label);
    acid_test(lo, hi, seqs, nseq, label);
}

// Verify the BMP "golden" output from upstream utf8 (the bmp test).
static void
run_bmp_golden(void)
{
    seq_t  seqs[16];
    size_t nseq = 0;
    int    rc   = collect_sequences(0u, 0xFFFFu, seqs, 16, &nseq);
    char   detail[128];
    if (rc != 0) {
        record("BMP golden", false, "iterator overflowed");
        return;
    }
    // Expected (from the upstream test):
    //   One([0x00-0x7F])
    //   Two([0xC2-0xDF][0x80-0xBF])
    //   Three([0xE0][0xA0-0xBF][0x80-0xBF])
    //   Three([0xE1-0xEC][0x80-0xBF][0x80-0xBF])
    //   Three([0xED][0x80-0x9F][0x80-0xBF])
    //   Three([0xEE-0xEF][0x80-0xBF][0x80-0xBF])
    bool ok = (nseq == 6);
    if (ok && nseq == 6) {
        ok = ok && seqs[0].nbytes == 1
                && seqs[0].br[0].start == 0x00 && seqs[0].br[0].end == 0x7F;
        ok = ok && seqs[1].nbytes == 2
                && seqs[1].br[0].start == 0xC2 && seqs[1].br[0].end == 0xDF
                && seqs[1].br[1].start == 0x80 && seqs[1].br[1].end == 0xBF;
        ok = ok && seqs[2].nbytes == 3
                && seqs[2].br[0].start == 0xE0 && seqs[2].br[0].end == 0xE0
                && seqs[2].br[1].start == 0xA0 && seqs[2].br[1].end == 0xBF
                && seqs[2].br[2].start == 0x80 && seqs[2].br[2].end == 0xBF;
        ok = ok && seqs[3].nbytes == 3
                && seqs[3].br[0].start == 0xE1 && seqs[3].br[0].end == 0xEC
                && seqs[3].br[1].start == 0x80 && seqs[3].br[1].end == 0xBF
                && seqs[3].br[2].start == 0x80 && seqs[3].br[2].end == 0xBF;
        ok = ok && seqs[4].nbytes == 3
                && seqs[4].br[0].start == 0xED && seqs[4].br[0].end == 0xED
                && seqs[4].br[1].start == 0x80 && seqs[4].br[1].end == 0x9F
                && seqs[4].br[2].start == 0x80 && seqs[4].br[2].end == 0xBF;
        ok = ok && seqs[5].nbytes == 3
                && seqs[5].br[0].start == 0xEE && seqs[5].br[0].end == 0xEF
                && seqs[5].br[1].start == 0x80 && seqs[5].br[1].end == 0xBF
                && seqs[5].br[2].start == 0x80 && seqs[5].br[2].end == 0xBF;
    }
    snprintf(detail, sizeof(detail), "BMP got %zu seqs (expected 6)", nseq);
    record("BMP golden (matches the upstream test)", ok, detail);
    if (!ok) print_seqs(seqs, nseq, "BMP got");
}

// Verify the full-Unicode "golden" output from the module-level docs of
// upstream utf8.
static void
run_full_golden(void)
{
    seq_t  seqs[16];
    size_t nseq = 0;
    int    rc   = collect_sequences(0u, 0x10FFFFu, seqs, 16, &nseq);
    if (rc != 0) {
        record("full golden", false, "iterator overflowed");
        return;
    }
    // Expected (from upstream utf8 module doc):
    //   One([0x00-0x7F])
    //   Two([0xC2-0xDF][0x80-0xBF])
    //   Three([0xE0][0xA0-0xBF][0x80-0xBF])
    //   Three([0xE1-0xEC][0x80-0xBF][0x80-0xBF])
    //   Three([0xED][0x80-0x9F][0x80-0xBF])
    //   Three([0xEE-0xEF][0x80-0xBF][0x80-0xBF])
    //   Four([0xF0][0x90-0xBF][0x80-0xBF][0x80-0xBF])
    //   Four([0xF1-0xF3][0x80-0xBF][0x80-0xBF][0x80-0xBF])
    //   Four([0xF4][0x80-0x8F][0x80-0xBF][0x80-0xBF])
    bool ok = (nseq == 9);
    print_seqs(seqs, nseq, "full");
    char detail[64];
    snprintf(detail, sizeof(detail), "full got %zu seqs (expected 9)", nseq);
    record("full golden (matches upstream utf8 module doc)", ok, detail);
}

// Single-codepoint test (from the upstream test):
// every single-cp range must produce exactly one sequence.
static void
run_single_cp(void)
{
    bool     ok    = true;
    uint32_t bad   = 0;
    size_t   bad_n = 0;
    // Skip the full sweep (1.1M cps takes too long); spot-check 1024 widely
    // spread codepoints.
    static const uint32_t samples[] = {
        0x0000, 0x0041, 0x007F, 0x0080, 0x00FF, 0x0100, 0x07FF, 0x0800,
        0x0FFF, 0x1000, 0xCFFF, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x1FFFF,
        0xFFFFF, 0x10FFFF, 0x10000, 0x103FF
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint32_t cp   = samples[i];
        seq_t    seqs[4];
        size_t   nseq = 0;
        if (collect_sequences(cp, cp, seqs, 4, &nseq) != 0 || nseq != 1) {
            ok = false; bad = cp; bad_n = nseq;
            break;
        }
    }
    char detail[80];
    if (ok)
        snprintf(detail, sizeof(detail), "all sampled single-cps produced 1 seq");
    else
        snprintf(detail, sizeof(detail), "U+%05X produced %zu seqs", bad, bad_n);
    record("single-cp -> single sequence", ok, detail);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("=== Utf8Sequences faithful-port smoke test ===\n");

    // Golden tests against upstream utf8.
    run_bmp_golden();
    run_full_golden();
    run_single_cp();

    // Required range tests.
    run_one(0u, 0x7Fu,        "[0..0x7F]    ASCII", false);
    run_one(0u, 0xFFu,        "[0..0xFF]    ASCII+Latin", false);
    run_one(0u, 0x7FFu,       "[0..0x7FF]   1+2 byte", false);
    run_one(0u, 0xFFFFu,      "[0..0xFFFF]  BMP", false);
    run_one(0u, 0x10FFFFu,    "[0..10FFFF]  full", false);
    run_one(0xD7FFu, 0xE000u, "[D7FF..E000] surrogate-edge", true);
    run_one(0x100u, 0x200u,   "[100..200]   small mid", false);

    // A few extra spot checks — a Cyrillic block, the Cyrillic supplementary
    // example from the upstream utf8 module doc.
    run_one(0x0400u, 0x04FFu, "[0400..04FF] Cyrillic", false);
    run_one(0x0400u, 0x052Fu, "[0400..052F] Cyrillic+sup", false);

    printf("\n%d passed, %d failed\n", passes, failures);
    n00b_shutdown();
    return failures == 0 ? 0 : 1;
}
