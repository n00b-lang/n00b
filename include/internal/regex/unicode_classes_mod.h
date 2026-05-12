/**
 * @file unicode_classes_mod.h
 * @brief UTF-8 acceptor + memoized Unicode-class cache for the regex builder.
 *
 * This module defines the `utf8_char` acceptor (a NodeId tree matching any
 * single UTF-8 codepoint) and `UnicodeClassCache`, a small struct that
 * memoizes the per-class NodeId values built by the `build_*_class*` family
 * (defined in the sister `unicode_classes_classes.c` translation unit).
 *
 * The names here keep the upstream Rust algorithmic vocabulary
 * (`NodeId`, `RegexBuilder`, `utf8_char`, `UnicodeClassCache`, ...).  The
 * file is internal to the regex engine; nothing here is part of the
 * public n00b surface.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/regex/ids.h"

typedef struct RegexBuilder RegexBuilder;

/** Node matching any single UTF-8 codepoint. */
NodeId utf8_char(RegexBuilder *b);

/** Complement of `positive` restricted to the UTF-8 codepoint universe. */
NodeId neg_class(RegexBuilder *b, NodeId positive);

/**
 * Per-builder memoization of the six character classes the parser asks
 * for repeatedly (\w / \W / \d / \D / \s / \S).  Each field starts as
 * `NODE_ID_MISSING` and is filled lazily by the matching `_ensure_*`
 * helper.  The `_full` variants build the full-Unicode flavour; the
 * non-`_full` variants build the ASCII-restricted flavour.
 */
typedef struct UnicodeClassCache {
    NodeId word;
    NodeId non_word;
    NodeId digit;
    NodeId non_digit;
    NodeId space;
    NodeId non_space;
} UnicodeClassCache;

UnicodeClassCache UnicodeClassCache_default(void);

void UnicodeClassCache_ensure_word(UnicodeClassCache *self, RegexBuilder *b);
void UnicodeClassCache_ensure_word_full(UnicodeClassCache *self, RegexBuilder *b);
void UnicodeClassCache_ensure_digit(UnicodeClassCache *self, RegexBuilder *b);
void UnicodeClassCache_ensure_digit_full(UnicodeClassCache *self, RegexBuilder *b);
void UnicodeClassCache_ensure_space(UnicodeClassCache *self, RegexBuilder *b);
void UnicodeClassCache_ensure_space_full(UnicodeClassCache *self, RegexBuilder *b);

/** @{ Field accessors used by the parser to read cached class NodeIds. */
NodeId UnicodeClassCache_word     (const UnicodeClassCache *c);
NodeId UnicodeClassCache_non_word (const UnicodeClassCache *c);
NodeId UnicodeClassCache_digit    (const UnicodeClassCache *c);
NodeId UnicodeClassCache_non_digit(const UnicodeClassCache *c);
NodeId UnicodeClassCache_space    (const UnicodeClassCache *c);
NodeId UnicodeClassCache_non_space(const UnicodeClassCache *c);
/** @} */

/**
 * Namespaced alias for `utf8_char`, used by callers that follow Rust's
 * `crate::module::fn` path syntax.
 */
NodeId unicode_classes_utf8_char(RegexBuilder *b);
