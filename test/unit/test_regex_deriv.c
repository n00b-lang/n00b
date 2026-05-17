// Phase 11 / deriv_test.c — typed translation.
//
// Source: ~/resharp-c/tests/deriv_test.c.  The original loaded a TOML
// fixture of (name, pattern[, input, fwd, rev, fwd_nulls, rev_nulls])
// entries, parsed each pattern via the algebra-level
// `resharp_parser_parse_ast`, walked symbolic Brzozowski derivatives
// forward and (after `regex_builder_reverse` / `regex_builder_normalize_rev`)
// in reverse, and cross-checked the pretty-printed transition node and
// nullability positions against the fixture.  n00b-regex has no TOML
// reader; the cases below are embedded byte-identical from
// `test/data/regex/deriv.toml`.
//
// The data values (pattern strings, input bytes, expected pretty-print
// strings, and expected nullability positions) are correctness oracles
// and remain byte-identical to the TOML; code translates per § 7.5 +
// § 19a (allocate-by-type, `n00b_result_t(T)`, bare `assert`).

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"

#include "adt/result.h"

#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"
#include "internal/regex/parser.h"
#include "internal/regex/solver.h"

// ---------------------------------------------------------------------------
// Optional<owned cstr> — expected pretty-print element.
// `some == false` corresponds to the TOML `"?"` placeholder (wildcard).
// ---------------------------------------------------------------------------

typedef struct opt_str_t {
    bool        some;
    const char *value;
} opt_str_t;

// Sentinel wildcard (matches anything).
#define WC ((opt_str_t){.some = false, .value = nullptr})
// Literal expected pretty-print value.
#define LIT(s) ((opt_str_t){.some = true, .value = (s)})

// ---------------------------------------------------------------------------
// Embedded test fixture — byte-identical with test/data/regex/deriv.toml.
// ---------------------------------------------------------------------------

typedef struct deriv_case_t {
    const char      *name;
    const char      *pattern;
    bool             ignore;
    const char      *input;
    const opt_str_t *rev;
    size_t           rev_len;
    const opt_str_t *fwd;
    size_t           fwd_len;
    bool             rev_nulls_some;
    const size_t    *rev_nulls;
    size_t           rev_nulls_len;
    bool             fwd_nulls_some;
    const size_t    *fwd_nulls;
    size_t           fwd_nulls_len;
} deriv_case_t;

// Per-case expected arrays.
static const opt_str_t rev_word_boundary_x_rev[] = {WC, WC, WC};
static const opt_str_t fwd_la_compl_has_y_fwd[]  = {WC, WC, WC};
static const opt_str_t fwd_word_border_star_fwd[] = {WC, WC, WC, WC};
static const opt_str_t fwd_la_compl_has_y_from1_fwd[] = {WC, WC};

static const size_t fwd_nulls_simple_literal[] = {2};
static const size_t rev_nulls_simple_literal[] = {2};

static const deriv_case_t k_cases[] = {
    {
        .name      = "word_boundary_x_rev",
        .pattern   = "\\bx\\b",
        .input     = "x x",
        .rev       = rev_word_boundary_x_rev,
        .rev_len   = sizeof(rev_word_boundary_x_rev) / sizeof(opt_str_t),
    },
    {
        .name      = "la_compl_has_y_fwd",
        .pattern   = "(?=.*z)[^y]*",
        .input     = "ayz",
        .fwd       = fwd_la_compl_has_y_fwd,
        .fwd_len   = sizeof(fwd_la_compl_has_y_fwd) / sizeof(opt_str_t),
    },
    {
        .name      = "word_border_star_fwd",
        .pattern   = "\\bTrue\\b *",
        .input     = "True",
        .fwd       = fwd_word_border_star_fwd,
        .fwd_len   = sizeof(fwd_word_border_star_fwd) / sizeof(opt_str_t),
    },
    {
        .name      = "la_compl_has_y_from1_fwd",
        .pattern   = "(?=.*z)[^y]*",
        .input     = "yz",
        .fwd       = fwd_la_compl_has_y_from1_fwd,
        .fwd_len   = sizeof(fwd_la_compl_has_y_from1_fwd) / sizeof(opt_str_t),
    },
    {
        .name           = "simple_literal_null_tracking",
        .pattern        = "ab",
        .input          = "ab",
        .fwd_nulls_some = true,
        .fwd_nulls      = fwd_nulls_simple_literal,
        .fwd_nulls_len  = sizeof(fwd_nulls_simple_literal) / sizeof(size_t),
        .rev_nulls_some = true,
        .rev_nulls      = rev_nulls_simple_literal,
        .rev_nulls_len  = sizeof(rev_nulls_simple_literal) / sizeof(size_t),
    },
    {
        .name      = "smaller_repro",
        .pattern   = "a(?!b).*(?<!a)b",
        .input     = "a b",
    },
    {
        .name      = "lb_inference_pp",
        .pattern   = "<.*(?<=<)bg",
        .input     = "<bg",
    },
};

// ---------------------------------------------------------------------------
// pos_mask — three-bit nullability mask for a position within [0, n].
// ---------------------------------------------------------------------------

static Nullability
pos_mask(size_t pos, size_t n)
{
    if (n == 0) {
        return nullability_or(NULLABILITY_BEGIN, NULLABILITY_END);
    } else if (pos == 0) {
        return NULLABILITY_BEGIN;
    } else if (pos == n) {
        return NULLABILITY_END;
    } else {
        return NULLABILITY_CENTER;
    }
}

// ---------------------------------------------------------------------------
// report_null — query and log whether `node` is nullable at position `pos`.
// ---------------------------------------------------------------------------

static bool
report_null(RegexBuilder *b, NodeId node, size_t pos, size_t n,
            const char *dir, const char *label)
{
    Nullability mask = pos_mask(pos, n);
    bool null = nullability_has(regex_builder_nullability(b, node), mask);
    fprintf(stderr,
            "  [%s] %s pos=%zu mask={bits:%u} nullable=%s\n",
            dir, label, pos, mask.v, null ? "true" : "false");
    return null;
}

// ---------------------------------------------------------------------------
// walk_bytes — iterate symbolic derivative through `bytes`, comparing
// against `expected` (length must match) and accumulating `got_nulls`
// for comparison against `expected_nulls` (when present).
// ---------------------------------------------------------------------------

static void
walk_bytes(RegexBuilder *b,
           NodeId        node,
           const uint8_t *bytes,
           size_t         bytes_len,
           const opt_str_t *expected,
           size_t         expected_len,
           const size_t  *expected_nulls,    // nullptr = None
           size_t         expected_nulls_len,
           bool           expected_nulls_some,
           const char    *dir,
           const char    *name)
{
    if (bytes_len != expected_len) {
        fprintf(stderr,
                "input length must match %s expected length for %s "
                "(got %zu, expected %zu)\n",
                dir, name, bytes_len, expected_len);
        assert(bytes_len == expected_len);
    }
    size_t n = bytes_len;

    size_t *got_nulls     = nullptr;
    size_t  got_nulls_len = 0;
    size_t  got_nulls_cap = 0;
    if (expected_nulls_some) {
        got_nulls_cap = n + 1;
        got_nulls     = n00b_alloc_array(size_t, got_nulls_cap);
    }

    if (report_null(b, node, 0, n, dir, "initial")) {
        if (got_nulls) got_nulls[got_nulls_len++] = 0;
    }

    for (size_t i = 0; i < bytes_len; ++i) {
        uint8_t byte = bytes[i];
        Nullability der_mask = pos_mask(i, n);

        TSetId tset = solver_u8_to_set_id(regex_builder_solver(b), byte);

        n00b_result_t(TRegexId) rd = regex_builder_der(b, node, der_mask);
        if (n00b_result_is_err(rd)) {
            fprintf(stderr, "regex_builder_der failed: name=%s dir=%s step=%zu\n",
                    name, dir, i);
            assert(n00b_result_is_ok(rd));
        }
        TRegexId tregex = n00b_result_get(rd);

        NodeId next = regex_builder_transition_term(b, tregex, tset);
        char *pp = regex_builder_pp(b, next);

        fprintf(stderr,
                "  [%s] step=%zu byte='%c' (0x%02x) der_mask={bits:%u} "
                "node={id:%u} => %s\n",
                dir, i, (char)byte, byte, der_mask.v, next.v, pp);

        if (expected[i].some) {
            const char *exp = expected[i].value;
            if (strcmp(pp, exp) != 0) {
                fprintf(stderr,
                        "deriv pp mismatch: name=%s dir=%s step=%zu byte='%c'"
                        " got=%s want=%s\n",
                        name, dir, i, (char)byte, pp, exp);
                assert(strcmp(pp, exp) == 0);
            }
        }

        node = next;
        if (report_null(b, node, i + 1, n, dir, "after")) {
            if (got_nulls) got_nulls[got_nulls_len++] = i + 1;
        }
    }

    if (expected_nulls_some) {
        bool eq = (got_nulls_len == expected_nulls_len);
        if (eq) {
            for (size_t i = 0; i < got_nulls_len; ++i) {
                if (got_nulls[i] != expected_nulls[i]) {
                    eq = false;
                    break;
                }
            }
        }
        if (!eq) {
            fprintf(stderr, "nullability mismatch: name=%s dir=%s\n",
                    name, dir);
            fprintf(stderr, "  got:     ");
            for (size_t i = 0; i < got_nulls_len; ++i) {
                fprintf(stderr, " %zu", got_nulls[i]);
            }
            fprintf(stderr, "\n  expected:");
            for (size_t i = 0; i < expected_nulls_len; ++i) {
                fprintf(stderr, " %zu", expected_nulls[i]);
            }
            fprintf(stderr, "\n");
            assert(eq);
        }
    }

    if (got_nulls) n00b_free(got_nulls);
}

// ---------------------------------------------------------------------------
// test_deriv_toml — entry point, one iteration per fixture case.
// ---------------------------------------------------------------------------

static void
test_deriv_toml(void)
{
    constexpr size_t n_cases = sizeof(k_cases) / sizeof(k_cases[0]);

    for (size_t ci = 0; ci < n_cases; ++ci) {
        const deriv_case_t *tc = &k_cases[ci];
        if (tc->ignore) continue;

        RegexBuilder *b = regex_builder_new(nullptr);

        NodeId node = (NodeId){0};
        bool parsed_ok = resharp_parser_parse_ast(b, tc->pattern, &node);
        if (!parsed_ok) {
            fprintf(stderr, "resharp_parser_parse_ast failed: name=%s\n",
                    tc->name);
            assert(parsed_ok);
        }

        size_t input_len = strlen(tc->input);

        if (tc->rev_len != 0 || tc->rev_nulls_some) {
            n00b_result_t(NodeId) e1 = regex_builder_reverse(b, node);
            if (n00b_result_is_err(e1)) {
                fprintf(stderr, "regex_builder_reverse failed: name=%s\n",
                        tc->name);
                assert(n00b_result_is_ok(e1));
            }
            NodeId r1 = n00b_result_get(e1);

            n00b_result_t(NodeId) e2 = regex_builder_normalize_rev(b, r1);
            if (n00b_result_is_err(e2)) {
                fprintf(stderr, "regex_builder_normalize_rev failed: name=%s\n",
                        tc->name);
                assert(n00b_result_is_ok(e2));
            }
            NodeId r2 = n00b_result_get(e2);

            NodeId rev = regex_builder_mk_concat(b, NODE_ID_TS, r2);

            char *rev_pp_str = regex_builder_pp(b, rev);
            fprintf(stderr,
                    "\n[%s] rev initial: node={id:%u} pp=%s\n",
                    tc->name, rev.v, rev_pp_str);

            // bytes = tc.input.as_bytes().iter().rev().copied().collect()
            uint8_t *bytes = nullptr;
            if (input_len > 0) {
                bytes = n00b_alloc_array(uint8_t, input_len);
                for (size_t i = 0; i < input_len; ++i) {
                    bytes[i] = (uint8_t)tc->input[input_len - 1 - i];
                }
            }

            // Use tc.rev if non-empty, else a vector of None of input_len.
            opt_str_t       *empty_rev = nullptr;
            const opt_str_t *rev_pp    = nullptr;
            size_t           rev_pp_len = 0;
            if (tc->rev_len == 0) {
                if (input_len > 0) {
                    empty_rev = n00b_alloc_array(opt_str_t, input_len);
                }
                rev_pp     = empty_rev;
                rev_pp_len = input_len;
            } else {
                rev_pp     = tc->rev;
                rev_pp_len = tc->rev_len;
            }

            walk_bytes(b, rev,
                       bytes, input_len,
                       rev_pp, rev_pp_len,
                       tc->rev_nulls,
                       tc->rev_nulls_len,
                       tc->rev_nulls_some,
                       "rev", tc->name);

            if (empty_rev) n00b_free(empty_rev);
            if (bytes)     n00b_free(bytes);
        }

        if (tc->fwd_len != 0 || tc->fwd_nulls_some) {
            char *fwd_pp_str = regex_builder_pp(b, node);
            fprintf(stderr,
                    "\n[%s] fwd initial: node={id:%u} kind=%d pp=%s\n",
                    tc->name,
                    node.v,
                    (int)regex_builder_get_kind(b, node),
                    fwd_pp_str);

            uint8_t *bytes = nullptr;
            if (input_len > 0) {
                bytes = n00b_alloc_array(uint8_t, input_len);
                for (size_t i = 0; i < input_len; ++i) {
                    bytes[i] = (uint8_t)tc->input[i];
                }
            }

            opt_str_t       *empty_fwd = nullptr;
            const opt_str_t *fwd_pp    = nullptr;
            size_t           fwd_pp_len = 0;
            if (tc->fwd_len == 0) {
                if (input_len > 0) {
                    empty_fwd = n00b_alloc_array(opt_str_t, input_len);
                }
                fwd_pp     = empty_fwd;
                fwd_pp_len = input_len;
            } else {
                fwd_pp     = tc->fwd;
                fwd_pp_len = tc->fwd_len;
            }

            walk_bytes(b, node,
                       bytes, input_len,
                       fwd_pp, fwd_pp_len,
                       tc->fwd_nulls,
                       tc->fwd_nulls_len,
                       tc->fwd_nulls_some,
                       "fwd", tc->name);

            if (empty_fwd) n00b_free(empty_fwd);
            if (bytes)     n00b_free(bytes);
        }

        regex_builder_free(b);
    }

    printf("  [PASS] deriv_toml (%zu cases)\n", n_cases);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex deriv tests...\n");
    test_deriv_toml();
    printf("All regex deriv tests passed.\n");

    n00b_shutdown();
    return 0;
}
