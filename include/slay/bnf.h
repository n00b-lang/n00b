#pragma once

/**
 * @file bnf.h
 * @brief BNF/EBNF text grammar loader.
 *
 * Parses BNF/EBNF text descriptions into grammar objects.  The default
 * parser engine is PWZ; callers can supply an alternative via the
 * `parse_fn` keyword argument.
 */

#include "slay/grammar.h"
#include "slay/parse_forest.h"
#include "slay/n00b_parse.h"
#include "slay/diagnostic.h"

n00b_string_t *n00b_bnf_strip_comments(n00b_string_t *input);
n00b_string_t *n00b_bnf_trim_lines(n00b_string_t *input);

/**
 * @brief Load a BNF/EBNF grammar from text into a grammar object.
 *
 * @param bnf_text      BNF text to parse.
 * @param start_symbol  Name of the start non-terminal.
 * @param user_g        Grammar to populate.
 * @return True on success, false on parse error.
 *
 * @kw parse_fn    Parser engine to use (default: `n00b_pwz_parse_grammar`).
 * @kw parse_mode  Backend selection for `n00b_parse()`.  When set and
 *                 `parse_fn` is not, uses the unified dispatch instead of
 *                 the raw function-pointer engine.
 * @kw diag        Optional diagnostic context.  When provided, detailed
 *                 error messages are pushed here instead of (or in addition
 *                 to) being printed to stderr.
 *
 * Note: the internal `n00b_grammar_finalize()` computes first-sets /
 * left-corners (PWZ needs them) but NOT the Earley-only LR0 tables; those
 * are built lazily by `n00b_grammar_compute_earley_analysis()` when Earley
 * first runs. So a PWZ-only load (naudit, the eval JIT, baked grammars)
 * skips the LR0 cost with no caller flag needed.
 */
bool n00b_bnf_load(n00b_string_t  *bnf_text,
                    n00b_string_t  *start_symbol,
                    n00b_grammar_t *user_g) _kargs {
    n00b_parse_fn_t   parse_fn;
    n00b_parse_mode_t parse_mode = N00B_PARSE_MODE_UNSET;
    n00b_diag_ctx_t  *diag;
};
