/**
 * @file parse.h
 * @brief Recursive descent parser for regex patterns (standard + extensions).
 *
 * Grammar:
 * ```
 * regex     = sequence ('|' sequence)* ('&' sequence)*
 * sequence  = quantified*
 * quantified = atom ('*' | '+' | '?' | '{n}' | '{n,}' | '{n,m}') '?'?
 * atom      = '.' | '_' | literal | escape | charclass | group | anchor
 * group     = '(' ('?:' | '?=' | '?!' | '?<=' | '?<!') regex ')'
 *           | '~(' regex ')'
 * charclass = '[' '^'? (range | escape | literal)+ ']'
 * escape    = '\d' | '\w' | '\s' | '\D' | '\W' | '\S' | '\p{Name}'
 *           | '\P{Name}' | '\n' | '\r' | '\t' | '\xHH' | '\uHHHH' | ...
 * anchor    = '^' | '$'
 * ```
 *
 * Extension operators: `&` (intersection), `~(...)` (complement),
 * `_` (universal wildcard = any single codepoint).
 */
#pragma once

#include "n00b.h"
#include "text/regex/node.h"
#include "adt/result.h"

/** Error codes for parse failures. */
typedef enum {
    N00B_RE_PARSE_OK             = 0,
    N00B_RE_PARSE_UNEXPECTED_END = 1,
    N00B_RE_PARSE_BAD_ESCAPE     = 2,
    N00B_RE_PARSE_BAD_CHARCLASS  = 3,
    N00B_RE_PARSE_BAD_QUANTIFIER = 4,
    N00B_RE_PARSE_UNBALANCED     = 5,
    N00B_RE_PARSE_BAD_PROPERTY   = 6,
    N00B_RE_PARSE_INTERNAL       = 7,
    N00B_RE_PARSE_NOT_LOOKAROUND = 8,  /**< Complement of lookaround unsupported. */
    N00B_RE_PARSE_NOT_ANCHOR     = 9,  /**< Complement of anchor-dependent node unsupported. */
} n00b_regex_parse_error_t;

/**
 * @brief Parse a regex pattern into the builder's node pool.
 *
 * @param builder The regex builder to add nodes to.
 * @param pattern The regex pattern string (UTF-8).
 * @param case_insensitive Expand chars to casefold equivalents.
 * @param multiline Make ^ and $ match line boundaries.
 * @param dot_all  Make . match newlines.
 * @return On success, result_ok with the root node ID.
 *         On failure, result_err with a parse error code.
 */
n00b_result_t(uint32_t)
n00b_regex_parse(n00b_regex_builder_t *builder,
                 n00b_string_t        *pattern,
                 bool                  case_insensitive,
                 bool                  multiline,
                 bool                  dot_all);
