#pragma once
/**
 * @file properties.h
 * @brief Unicode property types and ASCII-only stubs (standalone extraction).
 *
 * Provides the type definitions (enums) from the full n00b Unicode library
 * and stub implementations that handle ASCII only. The parser does not
 * need full Unicode property lookup -- just the types for compilation.
 */

#include "n00b.h"
#include "core/option.h"

// ============================================================================
// General_Category
// ============================================================================

typedef enum {
    N00B_UNICODE_GC_LU, /**< Letter, uppercase */
    N00B_UNICODE_GC_LL, /**< Letter, lowercase */
    N00B_UNICODE_GC_LT, /**< Letter, titlecase */
    N00B_UNICODE_GC_LM, /**< Letter, modifier */
    N00B_UNICODE_GC_LO, /**< Letter, other */
    N00B_UNICODE_GC_MN, /**< Mark, nonspacing */
    N00B_UNICODE_GC_MC, /**< Mark, spacing combining */
    N00B_UNICODE_GC_ME, /**< Mark, enclosing */
    N00B_UNICODE_GC_ND, /**< Number, decimal digit */
    N00B_UNICODE_GC_NL, /**< Number, letter */
    N00B_UNICODE_GC_NO, /**< Number, other */
    N00B_UNICODE_GC_PC, /**< Punctuation, connector */
    N00B_UNICODE_GC_PD, /**< Punctuation, dash */
    N00B_UNICODE_GC_PS, /**< Punctuation, open */
    N00B_UNICODE_GC_PE, /**< Punctuation, close */
    N00B_UNICODE_GC_PI, /**< Punctuation, initial quote */
    N00B_UNICODE_GC_PF, /**< Punctuation, final quote */
    N00B_UNICODE_GC_PO, /**< Punctuation, other */
    N00B_UNICODE_GC_SM, /**< Symbol, math */
    N00B_UNICODE_GC_SC, /**< Symbol, currency */
    N00B_UNICODE_GC_SK, /**< Symbol, modifier */
    N00B_UNICODE_GC_SO, /**< Symbol, other */
    N00B_UNICODE_GC_ZS, /**< Separator, space */
    N00B_UNICODE_GC_ZL, /**< Separator, line */
    N00B_UNICODE_GC_ZP, /**< Separator, paragraph */
    N00B_UNICODE_GC_CC, /**< Other, control */
    N00B_UNICODE_GC_CF, /**< Other, format */
    N00B_UNICODE_GC_CS, /**< Other, surrogate */
    N00B_UNICODE_GC_CO, /**< Other, private use */
    N00B_UNICODE_GC_CN, /**< Other, not assigned */
} n00b_unicode_gc_t;

// ============================================================================
// Bidi_Class
// ============================================================================

typedef enum {
    N00B_UNICODE_BIDI_L,
    N00B_UNICODE_BIDI_R,
    N00B_UNICODE_BIDI_AL,
    N00B_UNICODE_BIDI_EN,
    N00B_UNICODE_BIDI_ES,
    N00B_UNICODE_BIDI_ET,
    N00B_UNICODE_BIDI_AN,
    N00B_UNICODE_BIDI_CS,
    N00B_UNICODE_BIDI_NSM,
    N00B_UNICODE_BIDI_BN,
    N00B_UNICODE_BIDI_B,
    N00B_UNICODE_BIDI_S,
    N00B_UNICODE_BIDI_WS,
    N00B_UNICODE_BIDI_ON,
    N00B_UNICODE_BIDI_LRE,
    N00B_UNICODE_BIDI_LRO,
    N00B_UNICODE_BIDI_RLE,
    N00B_UNICODE_BIDI_RLO,
    N00B_UNICODE_BIDI_PDF,
    N00B_UNICODE_BIDI_LRI,
    N00B_UNICODE_BIDI_RLI,
    N00B_UNICODE_BIDI_FSI,
    N00B_UNICODE_BIDI_PDI,
} n00b_unicode_bidi_class_t;

// ============================================================================
// East_Asian_Width
// ============================================================================

typedef enum {
    N00B_UNICODE_EAW_N,
    N00B_UNICODE_EAW_NA,
    N00B_UNICODE_EAW_H,
    N00B_UNICODE_EAW_W,
    N00B_UNICODE_EAW_F,
    N00B_UNICODE_EAW_A,
} n00b_unicode_eaw_t;

// ============================================================================
// Script / Block
// ============================================================================

typedef uint8_t  n00b_unicode_script_t;
typedef uint16_t n00b_unicode_block_t;

// ============================================================================
// Joining_Type
// ============================================================================

typedef enum {
    N00B_UNICODE_JT_U,
    N00B_UNICODE_JT_C,
    N00B_UNICODE_JT_D,
    N00B_UNICODE_JT_L,
    N00B_UNICODE_JT_R,
    N00B_UNICODE_JT_T,
} n00b_unicode_jt_t;

// ============================================================================
// Binary properties
// ============================================================================

typedef enum {
    N00B_UNICODE_PROP_WHITE_SPACE,
    N00B_UNICODE_PROP_ALPHABETIC,
    N00B_UNICODE_PROP_NONCHARACTER_CODE_POINT,
    N00B_UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT,
    N00B_UNICODE_PROP_DEPRECATED,
    N00B_UNICODE_PROP_LOGICAL_ORDER_EXCEPTION,
    N00B_UNICODE_PROP_VARIATION_SELECTOR,
    N00B_UNICODE_PROP_UPPERCASE,
    N00B_UNICODE_PROP_LOWERCASE,
    N00B_UNICODE_PROP_SOFT_DOTTED,
    N00B_UNICODE_PROP_CASE_IGNORABLE,
    N00B_UNICODE_PROP_CASED,
    N00B_UNICODE_PROP_CHANGES_WHEN_LOWERCASED,
    N00B_UNICODE_PROP_CHANGES_WHEN_UPPERCASED,
    N00B_UNICODE_PROP_CHANGES_WHEN_TITLECASED,
    N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED,
    N00B_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED,
    N00B_UNICODE_PROP_ID_START,
    N00B_UNICODE_PROP_ID_CONTINUE,
    N00B_UNICODE_PROP_XID_START,
    N00B_UNICODE_PROP_XID_CONTINUE,
    N00B_UNICODE_PROP_PATTERN_SYNTAX,
    N00B_UNICODE_PROP_PATTERN_WHITE_SPACE,
    N00B_UNICODE_PROP_DASH,
    N00B_UNICODE_PROP_QUOTATION_MARK,
    N00B_UNICODE_PROP_TERMINAL_PUNCTUATION,
    N00B_UNICODE_PROP_SENTENCE_TERMINAL,
    N00B_UNICODE_PROP_DIACRITIC,
    N00B_UNICODE_PROP_EXTENDER,
    N00B_UNICODE_PROP_GRAPHEME_BASE,
    N00B_UNICODE_PROP_GRAPHEME_EXTEND,
    N00B_UNICODE_PROP_GRAPHEME_LINK,
    N00B_UNICODE_PROP_MATH,
    N00B_UNICODE_PROP_HEX_DIGIT,
    N00B_UNICODE_PROP_ASCII_HEX_DIGIT,
    N00B_UNICODE_PROP_IDEOGRAPHIC,
    N00B_UNICODE_PROP_UNIFIED_IDEOGRAPH,
    N00B_UNICODE_PROP_RADICAL,
    N00B_UNICODE_PROP_IDS_BINARY_OPERATOR,
    N00B_UNICODE_PROP_IDS_TRINARY_OPERATOR,
    N00B_UNICODE_PROP_JOIN_CONTROL,
    N00B_UNICODE_PROP_EMOJI,
    N00B_UNICODE_PROP_EMOJI_PRESENTATION,
    N00B_UNICODE_PROP_EMOJI_MODIFIER,
    N00B_UNICODE_PROP_EMOJI_MODIFIER_BASE,
    N00B_UNICODE_PROP_EMOJI_COMPONENT,
    N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC,
} n00b_unicode_property_t;

// ============================================================================
// Numeric types
// ============================================================================

typedef enum {
    N00B_UNICODE_NUMERIC_NONE,
    N00B_UNICODE_NUMERIC_DECIMAL,
    N00B_UNICODE_NUMERIC_DIGIT,
    N00B_UNICODE_NUMERIC_NUMERIC,
} n00b_unicode_numeric_type_t;

typedef struct {
    n00b_unicode_numeric_type_t type;
    int32_t                     numerator;
    int32_t                     denominator;
} n00b_unicode_numeric_value_t;

// ============================================================================
// BOM detection
// ============================================================================

typedef enum {
    N00B_UNICODE_BOM_NONE,
    N00B_UNICODE_BOM_UTF8,
    N00B_UNICODE_BOM_UTF16_LE,
    N00B_UNICODE_BOM_UTF16_BE,
    N00B_UNICODE_BOM_UTF32_LE,
    N00B_UNICODE_BOM_UTF32_BE,
} n00b_unicode_bom_t;

// ============================================================================
// Stub implementations (ASCII-only)
// ============================================================================

/**
 * @brief Return the General_Category of a codepoint (ASCII-only stub).
 */
static inline n00b_unicode_gc_t
n00b_unicode_general_category(n00b_codepoint_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return N00B_UNICODE_GC_LU;
    }
    if (cp >= 'a' && cp <= 'z') {
        return N00B_UNICODE_GC_LL;
    }
    if (cp >= '0' && cp <= '9') {
        return N00B_UNICODE_GC_ND;
    }
    if (cp == ' ') {
        return N00B_UNICODE_GC_ZS;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return N00B_UNICODE_GC_CC;
    }
    // Default: treat remaining ASCII punctuation/symbols as PO.
    if (cp < 0x80) {
        return N00B_UNICODE_GC_PO;
    }
    // Non-ASCII: unassigned.
    return N00B_UNICODE_GC_CN;
}

/**
 * @brief Test whether a codepoint has a given binary property (stub).
 *
 * Only handles the most common ASCII cases; returns false otherwise.
 */
static inline bool
n00b_unicode_has_property(n00b_codepoint_t cp,
                          n00b_unicode_property_t prop)
{
    switch (prop) {
    case N00B_UNICODE_PROP_WHITE_SPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v';
    case N00B_UNICODE_PROP_ALPHABETIC:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case N00B_UNICODE_PROP_UPPERCASE:
        return cp >= 'A' && cp <= 'Z';
    case N00B_UNICODE_PROP_LOWERCASE:
        return cp >= 'a' && cp <= 'z';
    case N00B_UNICODE_PROP_HEX_DIGIT:
    case N00B_UNICODE_PROP_ASCII_HEX_DIGIT:
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'F')
            || (cp >= 'a' && cp <= 'f');
    case N00B_UNICODE_PROP_ID_START:
    case N00B_UNICODE_PROP_XID_START:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || cp == '_';
    case N00B_UNICODE_PROP_ID_CONTINUE:
    case N00B_UNICODE_PROP_XID_CONTINUE:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || (cp >= '0' && cp <= '9') || cp == '_';
    default:
        return false;
    }
}

/**
 * @brief Return the display column width of a codepoint (ASCII stub).
 */
static inline int
n00b_unicode_char_width(n00b_codepoint_t cp)
{
    if (cp == 0) {
        return 0;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return 0; // Control characters.
    }
    return 1;
}
