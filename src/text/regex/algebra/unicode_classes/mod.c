// UTF-8 acceptor and memoized Unicode-class cache for the regex builder.
//
// Translated from ~/resharp-c/src/algebra/unicode_classes/mod.c.  The
// algorithmic vocabulary (NodeId, RegexBuilder, utf8_char, ...) tracks
// upstream Rust and stays unprefixed; this file is internal to the
// regex engine.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/regex/algebra.h"
#include "internal/regex/ids.h"
#include "internal/regex/unicode_classes_classes.h"
#include "internal/regex/unicode_classes_mod.h"

NodeId utf8_char(RegexBuilder *b)
{
    NodeId ascii        = regex_builder_mk_range_u8(b, 0, 127);
    NodeId cont         = regex_builder_mk_range_u8(b, 0x80, 0xBF);
    NodeId c2           = regex_builder_mk_range_u8(b, 0xC0, 0xDF);
    NodeId c2s          = regex_builder_mk_concat(b, c2, cont);
    NodeId e0           = regex_builder_mk_range_u8(b, 0xE0, 0xEF);
    NodeId e0s_items[3] = {e0, cont, cont};
    NodeId e0s          = regex_builder_mk_concats(b, e0s_items, 3);
    NodeId f0           = regex_builder_mk_range_u8(b, 0xF0, 0xF7);
    NodeId f0s_items[4] = {f0, cont, cont, cont};
    NodeId f0s          = regex_builder_mk_concats(b, f0s_items, 4);
    NodeId u_items[4]   = {ascii, c2s, e0s, f0s};
    return regex_builder_mk_unions(b, u_items, 4);
}

NodeId neg_class(RegexBuilder *b, NodeId positive)
{
    NodeId neg      = regex_builder_mk_compl(b, positive);
    NodeId uc       = utf8_char(b);
    NodeId items[2] = {neg, uc};
    return regex_builder_mk_inters(b, items, 2);
}

UnicodeClassCache UnicodeClassCache_default(void)
{
    return (UnicodeClassCache){
        .word      = NODE_ID_MISSING,
        .non_word  = NODE_ID_MISSING,
        .digit     = NODE_ID_MISSING,
        .non_digit = NODE_ID_MISSING,
        .space     = NODE_ID_MISSING,
        .non_space = NODE_ID_MISSING,
    };
}

void UnicodeClassCache_ensure_word(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->word, NODE_ID_MISSING)) {
        self->word     = build_word_class(b);
        self->non_word = neg_class(b, self->word);
    }
}

void UnicodeClassCache_ensure_word_full(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->word, NODE_ID_MISSING)) {
        self->word     = build_word_class_full(b);
        self->non_word = neg_class(b, self->word);
    }
}

void UnicodeClassCache_ensure_digit(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->digit, NODE_ID_MISSING)) {
        self->digit     = build_digit_class(b);
        self->non_digit = neg_class(b, self->digit);
    }
}

void UnicodeClassCache_ensure_digit_full(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->digit, NODE_ID_MISSING)) {
        self->digit     = build_digit_class_full(b);
        self->non_digit = neg_class(b, self->digit);
    }
}

void UnicodeClassCache_ensure_space(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->space, NODE_ID_MISSING)) {
        self->space     = build_space_class(b);
        self->non_space = neg_class(b, self->space);
    }
}

void UnicodeClassCache_ensure_space_full(UnicodeClassCache *self, RegexBuilder *b)
{
    if (nodeid_eq(self->space, NODE_ID_MISSING)) {
        self->space     = build_space_class_full(b);
        self->non_space = neg_class(b, self->space);
    }
}

// Field accessors — parser/lib.c reads cached class NodeIds via these.
NodeId UnicodeClassCache_word     (const UnicodeClassCache *c) { return c->word;      }
NodeId UnicodeClassCache_non_word (const UnicodeClassCache *c) { return c->non_word;  }
NodeId UnicodeClassCache_digit    (const UnicodeClassCache *c) { return c->digit;     }
NodeId UnicodeClassCache_non_digit(const UnicodeClassCache *c) { return c->non_digit; }
NodeId UnicodeClassCache_space    (const UnicodeClassCache *c) { return c->space;     }
NodeId UnicodeClassCache_non_space(const UnicodeClassCache *c) { return c->non_space; }

NodeId unicode_classes_utf8_char(RegexBuilder *b)
{
    return utf8_char(b);
}
