/**
 * @file unicode_classes_classes.h
 * @brief Algebra-side regex character class builders.
 *
 * Mechanically translated UTF-8 byte-range tree builders for the
 * pre-defined Unicode character classes (`\d`, `\s`, `\w` and their
 * full-Unicode variants).  Each builder returns a `NodeId` rooted at
 * the regex builder's expression DAG.
 *
 * The builders are pure data: the per-class byte-range trees are
 * baked into the source as sequences of `R1` / `RX` / `CONCAT` /
 * `UNION` macro invocations whose closeness to the upstream Rust is
 * the point.  This file does NOT call into n00b's Unicode property
 * tables at runtime — the ranges are hard-coded.
 *
 * These names stay un-prefixed (no `n00b_` prefix) because they form
 * the regex algorithmic vocabulary; the header lives under
 * `include/internal/regex/` and is not part of the public n00b
 * surface.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "internal/regex/ids.h"
#include "internal/regex/algebra.h"

/**
 * Inclusive byte range used by `regex_builder_mk_ranges_u8`.
 *
 * The unicode-classes builder file packs many of these into stack
 * literals via the `RX` macro so that each compound class reads as
 * a flat sequence of `(lo, hi)` pairs.
 */
// `range_u8_t` is defined in `internal/regex/algebra.h` (transitively
// included above) — see the comment near `regex_builder_mk_ranges_u8`.

/** Builds the ASCII `\d` class, `[0-9]`. */
NodeId build_digit_class(RegexBuilder *b);

/** Builds the full-Unicode `\d` class (every `\p{Number}` codepoint). */
NodeId build_digit_class_full(RegexBuilder *b);

/** Builds the ASCII `\s` class, `[\t\n\v\f\r ]`. */
NodeId build_space_class(RegexBuilder *b);

/** Builds the full-Unicode `\s` class (every `\p{White_Space}` codepoint). */
NodeId build_space_class_full(RegexBuilder *b);

/** Builds the ASCII `\w` class, `[0-9A-Z_a-z]`. */
NodeId build_word_class(RegexBuilder *b);

/** Builds the full-Unicode `\w` class (`\p{Alphabetic}` ∪ `\p{Mark}` ∪ `\p{Number}` ∪ `\p{Connector_Punctuation}` ∪ `\p{Join_Control}`). */
NodeId build_word_class_full(RegexBuilder *b);
