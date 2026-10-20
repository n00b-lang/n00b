#pragma once

#include "core.h"
#include "lib/option.h"

static inline bool
unicode_cp_is_ascii_digit(core_codepoint_t cp)
{
    return cp >= '0' && cp <= '9';
}

typedef enum {
    UNICODE_GC_LU,
    UNICODE_GC_LL,
    UNICODE_GC_LT,
    UNICODE_GC_LM,
    UNICODE_GC_LO,
    UNICODE_GC_MN,
    UNICODE_GC_MC,
    UNICODE_GC_ME,
    UNICODE_GC_ND,
    UNICODE_GC_NL,
    UNICODE_GC_NO,
    UNICODE_GC_PC,
    UNICODE_GC_PD,
    UNICODE_GC_PS,
    UNICODE_GC_PE,
    UNICODE_GC_PI,
    UNICODE_GC_PF,
    UNICODE_GC_PO,
    UNICODE_GC_SM,
    UNICODE_GC_SC,
    UNICODE_GC_SK,
    UNICODE_GC_SO,
    UNICODE_GC_ZS,
    UNICODE_GC_ZL,
    UNICODE_GC_ZP,
    UNICODE_GC_CC,
    UNICODE_GC_CF,
    UNICODE_GC_CS,
    UNICODE_GC_CO,
    UNICODE_GC_CN,
} unicode_gc_t;

typedef enum {
    UNICODE_BIDI_L,
    UNICODE_BIDI_R,
    UNICODE_BIDI_AL,
    UNICODE_BIDI_EN,
    UNICODE_BIDI_ES,
    UNICODE_BIDI_ET,
    UNICODE_BIDI_AN,
    UNICODE_BIDI_CS,
    UNICODE_BIDI_NSM,
    UNICODE_BIDI_BN,
    UNICODE_BIDI_B,
    UNICODE_BIDI_S,
    UNICODE_BIDI_WS,
    UNICODE_BIDI_ON,
    UNICODE_BIDI_LRE,
    UNICODE_BIDI_LRO,
    UNICODE_BIDI_RLE,
    UNICODE_BIDI_RLO,
    UNICODE_BIDI_PDF,
    UNICODE_BIDI_LRI,
    UNICODE_BIDI_RLI,
    UNICODE_BIDI_FSI,
    UNICODE_BIDI_PDI,
} unicode_bidi_class_t;

typedef enum {
    UNICODE_EAW_N,
    UNICODE_EAW_NA,
    UNICODE_EAW_H,
    UNICODE_EAW_W,
    UNICODE_EAW_F,
    UNICODE_EAW_A,
} unicode_eaw_t;

typedef uint8_t  unicode_script_t;
typedef uint16_t unicode_block_t;

typedef enum {
    UNICODE_JT_U,
    UNICODE_JT_C,
    UNICODE_JT_D,
    UNICODE_JT_L,
    UNICODE_JT_R,
    UNICODE_JT_T,
} unicode_jt_t;

typedef enum {
    UNICODE_PROP_WHITE_SPACE,
    UNICODE_PROP_ALPHABETIC,
    UNICODE_PROP_NONCHARACTER_CODE_POINT,
    UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT,
    UNICODE_PROP_DEPRECATED,
    UNICODE_PROP_LOGICAL_ORDER_EXCEPTION,
    UNICODE_PROP_VARIATION_SELECTOR,
    UNICODE_PROP_UPPERCASE,
    UNICODE_PROP_LOWERCASE,
    UNICODE_PROP_SOFT_DOTTED,
    UNICODE_PROP_CASE_IGNORABLE,
    UNICODE_PROP_CASED,
    UNICODE_PROP_CHANGES_WHEN_LOWERCASED,
    UNICODE_PROP_CHANGES_WHEN_UPPERCASED,
    UNICODE_PROP_CHANGES_WHEN_TITLECASED,
    UNICODE_PROP_CHANGES_WHEN_CASEFOLDED,
    UNICODE_PROP_CHANGES_WHEN_CASEMAPPED,
    UNICODE_PROP_ID_START,
    UNICODE_PROP_ID_CONTINUE,
    UNICODE_PROP_XID_START,
    UNICODE_PROP_XID_CONTINUE,
    UNICODE_PROP_PATTERN_SYNTAX,
    UNICODE_PROP_PATTERN_WHITE_SPACE,
    UNICODE_PROP_DASH,
    UNICODE_PROP_QUOTATION_MARK,
    UNICODE_PROP_TERMINAL_PUNCTUATION,
    UNICODE_PROP_SENTENCE_TERMINAL,
    UNICODE_PROP_DIACRITIC,
    UNICODE_PROP_EXTENDER,
    UNICODE_PROP_GRAPHEME_BASE,
    UNICODE_PROP_GRAPHEME_EXTEND,
    UNICODE_PROP_GRAPHEME_LINK,
    UNICODE_PROP_MATH,
    UNICODE_PROP_HEX_DIGIT,
    UNICODE_PROP_ASCII_HEX_DIGIT,
    UNICODE_PROP_IDEOGRAPHIC,
    UNICODE_PROP_UNIFIED_IDEOGRAPH,
    UNICODE_PROP_RADICAL,
    UNICODE_PROP_IDS_BINARY_OPERATOR,
    UNICODE_PROP_IDS_TRINARY_OPERATOR,
    UNICODE_PROP_JOIN_CONTROL,
    UNICODE_PROP_EMOJI,
    UNICODE_PROP_EMOJI_PRESENTATION,
    UNICODE_PROP_EMOJI_MODIFIER,
    UNICODE_PROP_EMOJI_MODIFIER_BASE,
    UNICODE_PROP_EMOJI_COMPONENT,
    UNICODE_PROP_EXTENDED_PICTOGRAPHIC,
} unicode_property_t;

typedef enum {
    UNICODE_NUMERIC_NONE,
    UNICODE_NUMERIC_DECIMAL,
    UNICODE_NUMERIC_DIGIT,
    UNICODE_NUMERIC_NUMERIC,
} unicode_numeric_type_t;

typedef struct {
    unicode_numeric_type_t type;
    int32_t                     numerator;
    int32_t                     denominator;
} unicode_numeric_value_t;

typedef enum {
    UNICODE_BOM_NONE,
    UNICODE_BOM_UTF8,
    UNICODE_BOM_UTF16_LE,
    UNICODE_BOM_UTF16_BE,
    UNICODE_BOM_UTF32_LE,
    UNICODE_BOM_UTF32_BE,
} unicode_bom_t;

static inline unicode_gc_t
unicode_general_category(core_codepoint_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return UNICODE_GC_LU;
    }
    if (cp >= 'a' && cp <= 'z') {
        return UNICODE_GC_LL;
    }
    if (cp >= '0' && cp <= '9') {
        return UNICODE_GC_ND;
    }
    if (cp == ' ') {
        return UNICODE_GC_ZS;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return UNICODE_GC_CC;
    }
    if (cp < 0x80) {
        return UNICODE_GC_PO;
    }
    return UNICODE_GC_CN;
}

static inline bool
unicode_has_property(core_codepoint_t cp,
                          unicode_property_t prop)
{
    switch (prop) {
    case UNICODE_PROP_WHITE_SPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v';
    case UNICODE_PROP_ALPHABETIC:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case UNICODE_PROP_UPPERCASE:
        return cp >= 'A' && cp <= 'Z';
    case UNICODE_PROP_LOWERCASE:
        return cp >= 'a' && cp <= 'z';
    case UNICODE_PROP_HEX_DIGIT:
    case UNICODE_PROP_ASCII_HEX_DIGIT:
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'F')
            || (cp >= 'a' && cp <= 'f');
    case UNICODE_PROP_ID_START:
    case UNICODE_PROP_XID_START:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || cp == '_';
    case UNICODE_PROP_ID_CONTINUE:
    case UNICODE_PROP_XID_CONTINUE:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || (cp >= '0' && cp <= '9') || cp == '_';
    default:
        return false;
    }
}

