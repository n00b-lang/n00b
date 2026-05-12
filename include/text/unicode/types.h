#pragma once
/** @file types.h
 *  @brief Core type definitions for the n00b unicode library.
 *
 *  Enumerations and basic types for Unicode properties: General_Category,
 *  Bidi_Class, East_Asian_Width, Script, Block, Line_Break, Grapheme/Word/
 *  Sentence_Break, Joining_Type, binary properties, numeric types,
 *  normalization forms, BOM detection, line break actions, emoji scanning,
 *  IDNA errors, script restriction levels, and break iterator types.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/** @brief A Unicode codepoint (U+0000..U+10FFFF). */
typedef uint32_t n00b_codepoint_t;

/** @brief An inclusive `[lo, hi]` codepoint range (both endpoints inclusive). */
typedef struct n00b_codepoint_pair_t {
    n00b_codepoint_t lo; /**< First codepoint in the range (inclusive). */
    n00b_codepoint_t hi; /**< Last codepoint in the range (inclusive). */
} n00b_codepoint_pair_t;

// ===========================================================================
// General_Category (field 2 of UnicodeData.txt)
// ===========================================================================

/** @brief Unicode General_Category values (UnicodeData.txt field 2). */
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

// ===========================================================================
// Bidi_Class
// ===========================================================================

/** @brief Unicode Bidi_Class values (UAX #9). */
typedef enum {
    N00B_UNICODE_BIDI_L,   /**< Left-to-Right */
    N00B_UNICODE_BIDI_R,   /**< Right-to-Left */
    N00B_UNICODE_BIDI_AL,  /**< Arabic Letter */
    N00B_UNICODE_BIDI_EN,  /**< European Number */
    N00B_UNICODE_BIDI_ES,  /**< European Separator */
    N00B_UNICODE_BIDI_ET,  /**< European Terminator */
    N00B_UNICODE_BIDI_AN,  /**< Arabic Number */
    N00B_UNICODE_BIDI_CS,  /**< Common Separator */
    N00B_UNICODE_BIDI_NSM, /**< Nonspacing Mark */
    N00B_UNICODE_BIDI_BN,  /**< Boundary Neutral */
    N00B_UNICODE_BIDI_B,   /**< Paragraph Separator */
    N00B_UNICODE_BIDI_S,   /**< Segment Separator */
    N00B_UNICODE_BIDI_WS,  /**< Whitespace */
    N00B_UNICODE_BIDI_ON,  /**< Other Neutral */
    N00B_UNICODE_BIDI_LRE, /**< Left-to-Right Embedding */
    N00B_UNICODE_BIDI_LRO, /**< Left-to-Right Override */
    N00B_UNICODE_BIDI_RLE, /**< Right-to-Left Embedding */
    N00B_UNICODE_BIDI_RLO, /**< Right-to-Left Override */
    N00B_UNICODE_BIDI_PDF, /**< Pop Directional Format */
    N00B_UNICODE_BIDI_LRI, /**< Left-to-Right Isolate */
    N00B_UNICODE_BIDI_RLI, /**< Right-to-Left Isolate */
    N00B_UNICODE_BIDI_FSI, /**< First Strong Isolate */
    N00B_UNICODE_BIDI_PDI, /**< Pop Directional Isolate */
} n00b_unicode_bidi_class_t;

// ===========================================================================
// East_Asian_Width
// ===========================================================================

/** @brief Unicode East_Asian_Width property values (UAX #11). */
typedef enum {
    N00B_UNICODE_EAW_N,  /**< Neutral */
    N00B_UNICODE_EAW_NA, /**< Narrow */
    N00B_UNICODE_EAW_H,  /**< Halfwidth */
    N00B_UNICODE_EAW_W,  /**< Wide */
    N00B_UNICODE_EAW_F,  /**< Fullwidth */
    N00B_UNICODE_EAW_A,  /**< Ambiguous */
} n00b_unicode_eaw_t;

// ===========================================================================
// Script (enum values match gen_scripts.c indices)
// ===========================================================================

/** @brief Unicode Script property value (UAX #24). Index into script name table. */
typedef uint8_t n00b_unicode_script_t;

// ===========================================================================
// Block (enum values match gen_blocks.c indices)
// ===========================================================================

/** @brief Unicode Block property value. Index into block name table. */
typedef uint16_t n00b_unicode_block_t;

// ===========================================================================
// Line_Break
// ===========================================================================

/** @brief Unicode Line_Break property values (UAX #14). */
typedef enum {
    N00B_UNICODE_LB_XX,  /**< Unknown */
    N00B_UNICODE_LB_BK,  /**< Mandatory Break */
    N00B_UNICODE_LB_CR,  /**< Carriage Return */
    N00B_UNICODE_LB_LF,  /**< Line Feed */
    N00B_UNICODE_LB_CM,  /**< Combining Mark */
    N00B_UNICODE_LB_NL,  /**< Next Line */
    N00B_UNICODE_LB_SG,  /**< Surrogates */
    N00B_UNICODE_LB_WJ,  /**< Word Joiner */
    N00B_UNICODE_LB_ZW,  /**< Zero Width Space */
    N00B_UNICODE_LB_GL,  /**< Non-breaking (Glue) */
    N00B_UNICODE_LB_SP,  /**< Space */
    N00B_UNICODE_LB_ZWJ, /**< Zero Width Joiner */
    N00B_UNICODE_LB_B2,  /**< Break Opportunity Before and After */
    N00B_UNICODE_LB_BA,  /**< Break After */
    N00B_UNICODE_LB_BB,  /**< Break Before */
    N00B_UNICODE_LB_HY,  /**< Hyphen */
    N00B_UNICODE_LB_CB,  /**< Contingent Break Opportunity */
    N00B_UNICODE_LB_CL,  /**< Close Punctuation */
    N00B_UNICODE_LB_CP,  /**< Close Parenthesis */
    N00B_UNICODE_LB_EX,  /**< Exclamation/Interrogation */
    N00B_UNICODE_LB_IN,  /**< Inseparable */
    N00B_UNICODE_LB_NS,  /**< Nonstarter */
    N00B_UNICODE_LB_OP,  /**< Open Punctuation */
    N00B_UNICODE_LB_QU,  /**< Quotation */
    N00B_UNICODE_LB_IS,  /**< Infix Numeric Separator */
    N00B_UNICODE_LB_NU,  /**< Numeric */
    N00B_UNICODE_LB_PO,  /**< Postfix Numeric */
    N00B_UNICODE_LB_PR,  /**< Prefix Numeric */
    N00B_UNICODE_LB_SY,  /**< Symbols Allowing Break After */
    N00B_UNICODE_LB_AI,  /**< Ambiguous (Alphabetic or Ideographic) */
    N00B_UNICODE_LB_AL,  /**< Alphabetic */
    N00B_UNICODE_LB_CJ,  /**< Conditional Japanese Starter */
    N00B_UNICODE_LB_EB,  /**< Emoji Base */
    N00B_UNICODE_LB_EM,  /**< Emoji Modifier */
    N00B_UNICODE_LB_H2,  /**< Hangul LV Syllable */
    N00B_UNICODE_LB_H3,  /**< Hangul LVT Syllable */
    N00B_UNICODE_LB_HL,  /**< Hebrew Letter */
    N00B_UNICODE_LB_ID,  /**< Ideographic */
    N00B_UNICODE_LB_JL,  /**< Hangul L Jamo */
    N00B_UNICODE_LB_JT,  /**< Hangul T Jamo */
    N00B_UNICODE_LB_JV,  /**< Hangul V Jamo */
    N00B_UNICODE_LB_RI,  /**< Regional Indicator */
    N00B_UNICODE_LB_SA,  /**< Complex Context Dependent (South East Asian) */
    N00B_UNICODE_LB_AK,  /**< Aksara */
    N00B_UNICODE_LB_AP,  /**< Aksara Pre-Base */
    N00B_UNICODE_LB_AS,  /**< Aksara Start */
    N00B_UNICODE_LB_VF,  /**< Virama Final */
    N00B_UNICODE_LB_VI,  /**< Virama */
} n00b_unicode_lb_t;

// ===========================================================================
// Grapheme_Cluster_Break
// ===========================================================================

/** @brief Unicode Grapheme_Cluster_Break property values (UAX #29). */
typedef enum {
    N00B_UNICODE_GCB_OTHER,              /**< Any other */
    N00B_UNICODE_GCB_CR,                 /**< Carriage Return */
    N00B_UNICODE_GCB_LF,                 /**< Line Feed */
    N00B_UNICODE_GCB_CONTROL,            /**< Control character */
    N00B_UNICODE_GCB_EXTEND,             /**< Grapheme Extend */
    N00B_UNICODE_GCB_ZWJ,                /**< Zero Width Joiner */
    N00B_UNICODE_GCB_REGIONAL_INDICATOR, /**< Regional Indicator */
    N00B_UNICODE_GCB_PREPEND,            /**< Prepend */
    N00B_UNICODE_GCB_SPACINGMARK,        /**< SpacingMark */
    N00B_UNICODE_GCB_L,                  /**< Hangul Syllable Type L */
    N00B_UNICODE_GCB_V,                  /**< Hangul Syllable Type V */
    N00B_UNICODE_GCB_T,                  /**< Hangul Syllable Type T */
    N00B_UNICODE_GCB_LV,                 /**< Hangul Syllable Type LV */
    N00B_UNICODE_GCB_LVT,                /**< Hangul Syllable Type LVT */
    N00B_UNICODE_GCB_INCB_CONSONANT,     /**< InCB=Consonant (Indic Conjunct Break) */
    N00B_UNICODE_GCB_INCB_EXTEND,        /**< InCB=Extend (Indic Conjunct Break) */
    N00B_UNICODE_GCB_INCB_LINKER,        /**< InCB=Linker (Indic Conjunct Break) */
} n00b_unicode_gcb_t;

// ===========================================================================
// Word_Break
// ===========================================================================

/** @brief Unicode Word_Break property values (UAX #29). */
typedef enum {
    N00B_UNICODE_WB_OTHER,              /**< Any other */
    N00B_UNICODE_WB_CR,                 /**< Carriage Return */
    N00B_UNICODE_WB_LF,                 /**< Line Feed */
    N00B_UNICODE_WB_NEWLINE,            /**< Newline */
    N00B_UNICODE_WB_EXTEND,             /**< Extend */
    N00B_UNICODE_WB_ZWJ,                /**< Zero Width Joiner */
    N00B_UNICODE_WB_REGIONAL_INDICATOR, /**< Regional Indicator */
    N00B_UNICODE_WB_FORMAT,             /**< Format */
    N00B_UNICODE_WB_KATAKANA,           /**< Katakana */
    N00B_UNICODE_WB_HEBREW_LETTER,      /**< Hebrew Letter */
    N00B_UNICODE_WB_ALETTER,            /**< ALetter */
    N00B_UNICODE_WB_SINGLE_QUOTE,       /**< Single Quote */
    N00B_UNICODE_WB_DOUBLE_QUOTE,       /**< Double Quote */
    N00B_UNICODE_WB_MIDNUMLET,          /**< MidNumLet */
    N00B_UNICODE_WB_MIDLETTER,          /**< MidLetter */
    N00B_UNICODE_WB_MIDNUM,             /**< MidNum */
    N00B_UNICODE_WB_NUMERIC,            /**< Numeric */
    N00B_UNICODE_WB_EXTENDNUMLET,       /**< ExtendNumLet */
    N00B_UNICODE_WB_WSEGSPACE,          /**< WSegSpace */
} n00b_unicode_wb_t;

// ===========================================================================
// Sentence_Break
// ===========================================================================

/** @brief Unicode Sentence_Break property values (UAX #29). */
typedef enum {
    N00B_UNICODE_SB_OTHER,     /**< Any other */
    N00B_UNICODE_SB_CR,        /**< Carriage Return */
    N00B_UNICODE_SB_LF,        /**< Line Feed */
    N00B_UNICODE_SB_EXTEND,    /**< Extend */
    N00B_UNICODE_SB_SEP,       /**< Separator */
    N00B_UNICODE_SB_FORMAT,    /**< Format */
    N00B_UNICODE_SB_SP,        /**< Space */
    N00B_UNICODE_SB_LOWER,     /**< Lower */
    N00B_UNICODE_SB_UPPER,     /**< Upper */
    N00B_UNICODE_SB_OLETTER,   /**< OLetter */
    N00B_UNICODE_SB_NUMERIC,   /**< Numeric */
    N00B_UNICODE_SB_ATERM,     /**< ATerm */
    N00B_UNICODE_SB_STERM,     /**< STerm */
    N00B_UNICODE_SB_CLOSE,     /**< Close */
    N00B_UNICODE_SB_SCONTINUE, /**< SContinue */
} n00b_unicode_sb_t;

// ===========================================================================
// Joining_Type
// ===========================================================================

/** @brief Unicode Joining_Type property values (for Arabic shaping). */
typedef enum {
    N00B_UNICODE_JT_U, /**< Non_Joining */
    N00B_UNICODE_JT_C, /**< Join_Causing */
    N00B_UNICODE_JT_D, /**< Dual_Joining */
    N00B_UNICODE_JT_L, /**< Left_Joining */
    N00B_UNICODE_JT_R, /**< Right_Joining */
    N00B_UNICODE_JT_T, /**< Transparent */
} n00b_unicode_jt_t;

// ===========================================================================
// Binary properties (bit positions in uint64_t)
// ===========================================================================

/** @brief Unicode binary properties (bit positions in a uint64_t bitmask). */
typedef enum {
    N00B_UNICODE_PROP_WHITE_SPACE,                  /**< White_Space */
    N00B_UNICODE_PROP_ALPHABETIC,                   /**< Alphabetic */
    N00B_UNICODE_PROP_NONCHARACTER_CODE_POINT,      /**< Noncharacter_Code_Point */
    N00B_UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT, /**< Default_Ignorable_Code_Point */
    N00B_UNICODE_PROP_DEPRECATED,                   /**< Deprecated */
    N00B_UNICODE_PROP_LOGICAL_ORDER_EXCEPTION,      /**< Logical_Order_Exception */
    N00B_UNICODE_PROP_VARIATION_SELECTOR,           /**< Variation_Selector */
    N00B_UNICODE_PROP_UPPERCASE,                    /**< Uppercase */
    N00B_UNICODE_PROP_LOWERCASE,                    /**< Lowercase */
    N00B_UNICODE_PROP_SOFT_DOTTED,                  /**< Soft_Dotted */
    N00B_UNICODE_PROP_CASE_IGNORABLE,               /**< Case_Ignorable */
    N00B_UNICODE_PROP_CASED,                        /**< Cased */
    N00B_UNICODE_PROP_CHANGES_WHEN_LOWERCASED,      /**< Changes_When_Lowercased */
    N00B_UNICODE_PROP_CHANGES_WHEN_UPPERCASED,      /**< Changes_When_Uppercased */
    N00B_UNICODE_PROP_CHANGES_WHEN_TITLECASED,      /**< Changes_When_Titlecased */
    N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED,      /**< Changes_When_Casefolded */
    N00B_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED,      /**< Changes_When_Casemapped */
    N00B_UNICODE_PROP_ID_START,                     /**< ID_Start */
    N00B_UNICODE_PROP_ID_CONTINUE,                  /**< ID_Continue */
    N00B_UNICODE_PROP_XID_START,                    /**< XID_Start */
    N00B_UNICODE_PROP_XID_CONTINUE,                 /**< XID_Continue */
    N00B_UNICODE_PROP_PATTERN_SYNTAX,               /**< Pattern_Syntax */
    N00B_UNICODE_PROP_PATTERN_WHITE_SPACE,          /**< Pattern_White_Space */
    N00B_UNICODE_PROP_DASH,                         /**< Dash */
    N00B_UNICODE_PROP_QUOTATION_MARK,               /**< Quotation_Mark */
    N00B_UNICODE_PROP_TERMINAL_PUNCTUATION,         /**< Terminal_Punctuation */
    N00B_UNICODE_PROP_SENTENCE_TERMINAL,            /**< Sentence_Terminal */
    N00B_UNICODE_PROP_DIACRITIC,                    /**< Diacritic */
    N00B_UNICODE_PROP_EXTENDER,                     /**< Extender */
    N00B_UNICODE_PROP_GRAPHEME_BASE,                /**< Grapheme_Base */
    N00B_UNICODE_PROP_GRAPHEME_EXTEND,              /**< Grapheme_Extend */
    N00B_UNICODE_PROP_GRAPHEME_LINK,                /**< Grapheme_Link */
    N00B_UNICODE_PROP_MATH,                         /**< Math */
    N00B_UNICODE_PROP_HEX_DIGIT,                    /**< Hex_Digit */
    N00B_UNICODE_PROP_ASCII_HEX_DIGIT,              /**< ASCII_Hex_Digit */
    N00B_UNICODE_PROP_IDEOGRAPHIC,                  /**< Ideographic */
    N00B_UNICODE_PROP_UNIFIED_IDEOGRAPH,            /**< Unified_Ideograph */
    N00B_UNICODE_PROP_RADICAL,                      /**< Radical */
    N00B_UNICODE_PROP_IDS_BINARY_OPERATOR,          /**< IDS_Binary_Operator */
    N00B_UNICODE_PROP_IDS_TRINARY_OPERATOR,         /**< IDS_Trinary_Operator */
    N00B_UNICODE_PROP_JOIN_CONTROL,                 /**< Join_Control */
    N00B_UNICODE_PROP_EMOJI,                        /**< Emoji */
    N00B_UNICODE_PROP_EMOJI_PRESENTATION,           /**< Emoji_Presentation */
    N00B_UNICODE_PROP_EMOJI_MODIFIER,               /**< Emoji_Modifier */
    N00B_UNICODE_PROP_EMOJI_MODIFIER_BASE,          /**< Emoji_Modifier_Base */
    N00B_UNICODE_PROP_EMOJI_COMPONENT,              /**< Emoji_Component */
    N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC,        /**< Extended_Pictographic */
} n00b_unicode_property_t;

// ===========================================================================
// Numeric_Type / Numeric_Value
// ===========================================================================

/** @brief Unicode Numeric_Type property values. */
typedef enum {
    N00B_UNICODE_NUMERIC_NONE,    /**< Not numeric */
    N00B_UNICODE_NUMERIC_DECIMAL, /**< Decimal digit (0-9 per script, field 6) */
    N00B_UNICODE_NUMERIC_DIGIT,   /**< Digit (e.g. superscript, field 7) */
    N00B_UNICODE_NUMERIC_NUMERIC, /**< General numeric (fractions, etc., field 8) */
} n00b_unicode_numeric_type_t;

/** @brief A rational numeric value associated with a codepoint. */
typedef struct {
    n00b_unicode_numeric_type_t type;        /**< The kind of numeric value */
    int32_t                     numerator;   /**< Numerator of the rational value */
    int32_t                     denominator; /**< Denominator (always > 0) */
} n00b_unicode_numeric_value_t;

// ===========================================================================
// Normalization forms
// ===========================================================================

/** @brief Unicode Normalization Form selectors (UAX #15). */
typedef enum {
    N00B_UNICODE_NFC,  /**< Canonical Decomposition, then Canonical Composition */
    N00B_UNICODE_NFD,  /**< Canonical Decomposition */
    N00B_UNICODE_NFKC, /**< Compatibility Decomposition, then Canonical Composition */
    N00B_UNICODE_NFKD, /**< Compatibility Decomposition */
} n00b_unicode_norm_form_t;

// ===========================================================================
// BOM detection
// ===========================================================================

/** @brief Byte Order Mark (BOM) types detected at the start of a byte stream. */
typedef enum {
    N00B_UNICODE_BOM_NONE,     /**< No BOM detected */
    N00B_UNICODE_BOM_UTF8,     /**< UTF-8 BOM (EF BB BF) */
    N00B_UNICODE_BOM_UTF16_LE, /**< UTF-16 Little-Endian BOM (FF FE) */
    N00B_UNICODE_BOM_UTF16_BE, /**< UTF-16 Big-Endian BOM (FE FF) */
    N00B_UNICODE_BOM_UTF32_LE, /**< UTF-32 Little-Endian BOM (FF FE 00 00) */
    N00B_UNICODE_BOM_UTF32_BE, /**< UTF-32 Big-Endian BOM (00 00 FE FF) */
} n00b_unicode_bom_t;

// ===========================================================================
// Line break action
// ===========================================================================

/** @brief Action at a line break opportunity (UAX #14). */
typedef enum {
    N00B_UNICODE_LB_ACTION_NONE,      /**< No break allowed at this position */
    N00B_UNICODE_LB_ACTION_ALLOWED,   /**< Break opportunity (optional) */
    N00B_UNICODE_LB_ACTION_MANDATORY, /**< Mandatory line break */
} n00b_unicode_lb_action_t;

// ===========================================================================
// Emoji scanning
// ===========================================================================

/** @brief Classification of an emoji sequence found by scanning. */
typedef enum {
    N00B_UNICODE_EMOJI_NONE,             /**< Not an emoji sequence */
    N00B_UNICODE_EMOJI_BASIC,            /**< Single emoji character */
    N00B_UNICODE_EMOJI_PRESENTATION_SEQ, /**< Emoji with presentation selector */
    N00B_UNICODE_EMOJI_KEYCAP,           /**< Keycap sequence */
    N00B_UNICODE_EMOJI_MODIFIER_SEQ,     /**< Emoji with skin tone modifier */
    N00B_UNICODE_EMOJI_FLAG,             /**< Regional indicator flag sequence */
    N00B_UNICODE_EMOJI_TAG_SEQ,          /**< Tag sequence (e.g. subdivision flags) */
    N00B_UNICODE_EMOJI_ZWJ_SEQ,          /**< ZWJ (Zero Width Joiner) sequence */
} n00b_unicode_emoji_type_t;

// ===========================================================================
// IDNA errors
// ===========================================================================

/** @brief IDNA processing error codes (UTS #46). */
typedef enum {
    N00B_UNICODE_IDNA_OK = 0,              /**< No error */
    N00B_UNICODE_IDNA_PROCESSING_ERROR,     /**< General processing failure */
    N00B_UNICODE_IDNA_DISALLOWED,           /**< Disallowed codepoint in label */
    N00B_UNICODE_IDNA_LABEL_TOO_LONG,       /**< Label exceeds 63 bytes */
    N00B_UNICODE_IDNA_DOMAIN_TOO_LONG,      /**< Domain exceeds 253 bytes */
    N00B_UNICODE_IDNA_PUNYCODE_ERROR,       /**< Punycode encode/decode failure */
    N00B_UNICODE_IDNA_INVALID_ACE,          /**< Invalid ACE prefix (xn--) label */
    N00B_UNICODE_IDNA_BIDI_ERROR,           /**< Bidi rule violation (RFC 5893) */
    N00B_UNICODE_IDNA_CONTEXTJ_ERROR,       /**< CONTEXTJ rule violation */
    N00B_UNICODE_IDNA_CONTEXTO_ERROR,       /**< CONTEXTO rule violation */
    N00B_UNICODE_IDNA_LEADING_COMBINING,    /**< Label starts with combining mark */
    N00B_UNICODE_IDNA_EMPTY_LABEL,          /**< Empty label in domain */
} n00b_unicode_idna_error_t;

// ===========================================================================
// Script restriction levels (UTS #39)
// ===========================================================================

/** @brief Script restriction levels for identifier security (UTS #39). */
typedef enum {
    N00B_UNICODE_RESTRICTION_ASCII_ONLY,            /**< ASCII characters only */
    N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT,         /**< Single script (plus Common/Inherited) */
    N00B_UNICODE_RESTRICTION_HIGHLY_RESTRICTIVE,    /**< Highly restrictive */
    N00B_UNICODE_RESTRICTION_MODERATELY_RESTRICTIVE, /**< Moderately restrictive */
    N00B_UNICODE_RESTRICTION_MINIMALLY_RESTRICTIVE, /**< Minimally restrictive */
    N00B_UNICODE_RESTRICTION_UNRESTRICTED,          /**< No script restriction */
} n00b_unicode_restriction_level_t;

// ===========================================================================
// Break iterator types
// ===========================================================================

/** @brief Type selector for break iteration (grapheme, word, or sentence). */
typedef enum {
    N00B_UNICODE_BREAK_GRAPHEME, /**< Grapheme cluster boundaries (UAX #29) */
    N00B_UNICODE_BREAK_WORD,     /**< Word boundaries (UAX #29) */
    N00B_UNICODE_BREAK_SENTENCE, /**< Sentence boundaries (UAX #29) */
} n00b_unicode_break_type_t;

