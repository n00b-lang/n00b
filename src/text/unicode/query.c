#include "text/unicode/query.h"
#include "text/unicode/properties.h"
#include "text/unicode/encoding.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include <string.h>

// ===========================================================================
// Internal filter predicates
// ===========================================================================

static bool
_pred_gc(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_general_category(cp) == (n00b_unicode_gc_t)(uintptr_t)ctx;
}

static bool
_pred_script(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_script(cp) == (n00b_unicode_script_t)(uintptr_t)ctx;
}

static bool
_pred_bidi(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_bidi_class(cp) == (n00b_unicode_bidi_class_t)(uintptr_t)ctx;
}

static bool
_pred_property(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_has_property(cp, (n00b_unicode_property_t)(uintptr_t)ctx);
}

typedef struct {
    n00b_codepoint_t lo;
    n00b_codepoint_t hi;
} _range_ctx_t;

static bool
_pred_range(n00b_codepoint_t cp, void *ctx)
{
    _range_ctx_t *r = (_range_ctx_t *)ctx;
    return cp >= r->lo && cp <= r->hi;
}

static bool
_pred_block(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_block(cp) == (n00b_unicode_block_t)(uintptr_t)ctx;
}

static bool
_pred_eaw(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_east_asian_width(cp) == (n00b_unicode_eaw_t)(uintptr_t)ctx;
}

// ===========================================================================
// Filter constructors
// ===========================================================================

n00b_cp_filter_t
n00b_filter_gc(n00b_unicode_gc_t gc)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_gc,
        .ctx       = (void *)(uintptr_t)gc,
    };
}

n00b_cp_filter_t
n00b_filter_script(n00b_unicode_script_t script)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_script,
        .ctx       = (void *)(uintptr_t)script,
    };
}

n00b_cp_filter_t
n00b_filter_bidi(n00b_unicode_bidi_class_t bidi)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_bidi,
        .ctx       = (void *)(uintptr_t)bidi,
    };
}

n00b_cp_filter_t
n00b_filter_property(n00b_unicode_property_t prop)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_property,
        .ctx       = (void *)(uintptr_t)prop,
    };
}

n00b_cp_filter_t
n00b_filter_range(n00b_codepoint_t lo, n00b_codepoint_t hi)
{
    static _range_ctx_t ranges[64];
    static int          next_range = 0;

    int idx        = next_range++ % 64;
    ranges[idx].lo = lo;
    ranges[idx].hi = hi;

    return (n00b_cp_filter_t){
        .predicate = _pred_range,
        .ctx       = &ranges[idx],
    };
}

n00b_cp_filter_t
n00b_filter_block(n00b_unicode_block_t block)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_block,
        .ctx       = (void *)(uintptr_t)block,
    };
}

n00b_cp_filter_t
n00b_filter_eaw(n00b_unicode_eaw_t eaw)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_eaw,
        .ctx       = (void *)(uintptr_t)eaw,
    };
}

// ===========================================================================
// Query implementation
// ===========================================================================

n00b_array_t(n00b_codepoint_t)
n00b_cp_query_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start        = 0;
    n00b_codepoint_t  range_end          = 0x10FFFF;
    size_t            max_results        = 0;
    n00b_allocator_t *allocator          = nullptr;
    // Surrogates (U+D800..U+DFFF) are not assigned characters and are
    // omitted by default; set true to include them (e.g. for `\p{Any}`
    // semantics, which spans the full codespace).
    bool              include_surrogates = false;
}
{
    n00b_ensure_allocator(allocator);

    size_t            cap     = 256;
    size_t            count   = 0;
    n00b_codepoint_t *results = n00b_alloc_array_with_opts(n00b_codepoint_t, cap, &(n00b_alloc_opts_t){.allocator = allocator});

    for (n00b_codepoint_t cp = range_start; cp <= range_end; cp++) {
        if (!include_surrogates && cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }

        // AND: all filters must match.
        bool match = true;
        for (int i = 0; i < nfilters; i++) {
            if (!filters[i].predicate(cp, filters[i].ctx)) {
                match = false;
                break;
            }
        }

        if (match) {
            if (count >= cap) {
                size_t            new_cap = cap * 2;
                n00b_codepoint_t *new_results
                    = n00b_alloc_array_with_opts(n00b_codepoint_t, new_cap, &(n00b_alloc_opts_t){.allocator = allocator});
                memcpy(new_results, results, count * sizeof(n00b_codepoint_t));
                n00b_free(results);
                results = new_results;
                cap     = new_cap;
            }
            results[count++] = cp;

            if (max_results > 0 && count >= max_results) {
                break;
            }
        }
    }

    n00b_array_t(n00b_codepoint_t) result = n00b_array_checked_ptr(n00b_codepoint_t,
                                                                    cap, results);
    result.len = count;
    return result;
}

n00b_array_t(n00b_codepoint_t)
n00b_cp_query_any_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start        = 0;
    n00b_codepoint_t  range_end          = 0x10FFFF;
    size_t            max_results        = 0;
    n00b_allocator_t *allocator          = nullptr;
    bool              include_surrogates = false;
}
{
    n00b_ensure_allocator(allocator);

    size_t            cap     = 256;
    size_t            count   = 0;
    n00b_codepoint_t *results = n00b_alloc_array_with_opts(n00b_codepoint_t, cap, &(n00b_alloc_opts_t){.allocator = allocator});

    for (n00b_codepoint_t cp = range_start; cp <= range_end; cp++) {
        if (!include_surrogates && cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }

        // OR: any filter matches.
        bool match = false;
        for (int i = 0; i < nfilters; i++) {
            if (filters[i].predicate(cp, filters[i].ctx)) {
                match = true;
                break;
            }
        }

        if (match) {
            if (count >= cap) {
                size_t            new_cap = cap * 2;
                n00b_codepoint_t *new_results
                    = n00b_alloc_array_with_opts(n00b_codepoint_t, new_cap, &(n00b_alloc_opts_t){.allocator = allocator});
                memcpy(new_results, results, count * sizeof(n00b_codepoint_t));
                n00b_free(results);
                results = new_results;
                cap     = new_cap;
            }
            results[count++] = cp;

            if (max_results > 0 && count >= max_results) {
                break;
            }
        }
    }

    n00b_array_t(n00b_codepoint_t) result = n00b_array_checked_ptr(n00b_codepoint_t,
                                                                    cap, results);
    result.len = count;
    return result;
}

// ===========================================================================
// Name lookup (stub - requires name table from generator)
// ===========================================================================

n00b_option_t(const char *)
n00b_unicode_cp_name(n00b_codepoint_t cp)
{
    (void)cp;
    // TODO: Implement when gen_names.c is generated by gen_tables.py.
    return n00b_option_none(const char *);
}

n00b_option_t(n00b_codepoint_t) n00b_unicode_cp_from_name(const char *name)
{
    (void)name;
    // TODO: Implement when gen_names.c is generated by gen_tables.py.
    return n00b_option_none(n00b_codepoint_t);
}

// ===========================================================================
// Property-name -> enum lookups
//
// Used by regex \p{...} resolution. Loose matching per UAX #44 LM3:
// case-insensitive (ASCII fold) and ignoring whitespace, underscores, and
// hyphens.
// ===========================================================================

// External generated tables for script/block names.
extern const char    *n00b_unicode_script_names[];
extern const uint32_t n00b_unicode_script_count;
extern const char    *n00b_unicode_block_names[];
extern const uint32_t n00b_unicode_block_count;

// ---------------------------------------------------------------------------
// Loose name matching per UAX #44 LM3:
//   case-insensitive (ASCII fold), ignore spaces, underscores, and medial
//   hyphens.
// We compare two strings while skipping ' ', '_', '-' on either side.
// ASCII fold is inlined to avoid a libc <ctype.h> dependency and any
// locale sensitivity that comes with tolower(3).
// ---------------------------------------------------------------------------

static inline unsigned char
_ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool
loose_eq(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a == ' ' || *a == '_' || *a == '-') {
            a++;
        }
        while (*b == ' ' || *b == '_' || *b == '-') {
            b++;
        }
        if (!*a || !*b) {
            break;
        }
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (_ascii_lower(ca) != _ascii_lower(cb)) {
            return false;
        }
    }
    while (*a == ' ' || *a == '_' || *a == '-') {
        a++;
    }
    while (*b == ' ' || *b == '_' || *b == '-') {
        b++;
    }
    return *a == 0 && *b == 0;
}

// ---------------------------------------------------------------------------
// General_Category names (abbrev + long, plus a couple of common aliases).
// ---------------------------------------------------------------------------

typedef struct {
    const char       *name;
    n00b_unicode_gc_t value;
} _gc_name_t;

static const _gc_name_t _gc_names[] = {
    {"Lu", N00B_UNICODE_GC_LU}, {"Uppercase_Letter", N00B_UNICODE_GC_LU},
    {"Ll", N00B_UNICODE_GC_LL}, {"Lowercase_Letter", N00B_UNICODE_GC_LL},
    {"Lt", N00B_UNICODE_GC_LT}, {"Titlecase_Letter", N00B_UNICODE_GC_LT},
    {"Lm", N00B_UNICODE_GC_LM}, {"Modifier_Letter",  N00B_UNICODE_GC_LM},
    {"Lo", N00B_UNICODE_GC_LO}, {"Other_Letter",     N00B_UNICODE_GC_LO},

    {"Mn", N00B_UNICODE_GC_MN}, {"Nonspacing_Mark", N00B_UNICODE_GC_MN},
    {"Mc", N00B_UNICODE_GC_MC}, {"Spacing_Mark",    N00B_UNICODE_GC_MC},
    {"Me", N00B_UNICODE_GC_ME}, {"Enclosing_Mark",  N00B_UNICODE_GC_ME},

    {"Nd", N00B_UNICODE_GC_ND}, {"Decimal_Number", N00B_UNICODE_GC_ND},
    {"digit", N00B_UNICODE_GC_ND},
    {"Nl", N00B_UNICODE_GC_NL}, {"Letter_Number", N00B_UNICODE_GC_NL},
    {"No", N00B_UNICODE_GC_NO}, {"Other_Number",  N00B_UNICODE_GC_NO},

    {"Pc", N00B_UNICODE_GC_PC}, {"Connector_Punctuation", N00B_UNICODE_GC_PC},
    {"Pd", N00B_UNICODE_GC_PD}, {"Dash_Punctuation",      N00B_UNICODE_GC_PD},
    {"Ps", N00B_UNICODE_GC_PS}, {"Open_Punctuation",      N00B_UNICODE_GC_PS},
    {"Pe", N00B_UNICODE_GC_PE}, {"Close_Punctuation",     N00B_UNICODE_GC_PE},
    {"Pi", N00B_UNICODE_GC_PI}, {"Initial_Punctuation",   N00B_UNICODE_GC_PI},
    {"Pf", N00B_UNICODE_GC_PF}, {"Final_Punctuation",     N00B_UNICODE_GC_PF},
    {"Po", N00B_UNICODE_GC_PO}, {"Other_Punctuation",     N00B_UNICODE_GC_PO},

    {"Sm", N00B_UNICODE_GC_SM}, {"Math_Symbol",     N00B_UNICODE_GC_SM},
    {"Sc", N00B_UNICODE_GC_SC}, {"Currency_Symbol", N00B_UNICODE_GC_SC},
    {"Sk", N00B_UNICODE_GC_SK}, {"Modifier_Symbol", N00B_UNICODE_GC_SK},
    {"So", N00B_UNICODE_GC_SO}, {"Other_Symbol",    N00B_UNICODE_GC_SO},

    {"Zs", N00B_UNICODE_GC_ZS}, {"Space_Separator",     N00B_UNICODE_GC_ZS},
    {"Zl", N00B_UNICODE_GC_ZL}, {"Line_Separator",      N00B_UNICODE_GC_ZL},
    {"Zp", N00B_UNICODE_GC_ZP}, {"Paragraph_Separator", N00B_UNICODE_GC_ZP},

    {"Cc", N00B_UNICODE_GC_CC}, {"Control",     N00B_UNICODE_GC_CC},
    {"cntrl", N00B_UNICODE_GC_CC},
    {"Cf", N00B_UNICODE_GC_CF}, {"Format",      N00B_UNICODE_GC_CF},
    {"Cs", N00B_UNICODE_GC_CS}, {"Surrogate",   N00B_UNICODE_GC_CS},
    {"Co", N00B_UNICODE_GC_CO}, {"Private_Use", N00B_UNICODE_GC_CO},
    {"Cn", N00B_UNICODE_GC_CN}, {"Unassigned",  N00B_UNICODE_GC_CN},
};

bool
n00b_unicode_gc_by_name(const char *name, n00b_unicode_gc_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_gc_names) / sizeof(_gc_names[0]); i++) {
        if (loose_eq(_gc_names[i].name, name)) {
            if (out) {
                *out = _gc_names[i].value;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Script lookup: walks the generated name table.
// ---------------------------------------------------------------------------

bool
n00b_unicode_script_by_name(const char *name, n00b_unicode_script_t *out)
{
    if (!name) {
        return false;
    }
    for (uint32_t i = 0; i < n00b_unicode_script_count; i++) {
        if (loose_eq(n00b_unicode_script_names[i], name)) {
            if (out) {
                *out = (n00b_unicode_script_t)i;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Block lookup: walks the generated name table.
// The names table is sized n00b_unicode_block_count + 1 (index 0 is
// "No_Block"; indices 1..count map to the count block ranges). We match
// across the inclusive range [0 .. count].
// ---------------------------------------------------------------------------

bool
n00b_unicode_block_by_name(const char *name, n00b_unicode_block_t *out)
{
    if (!name) {
        return false;
    }
    for (uint32_t i = 0; i <= n00b_unicode_block_count; i++) {
        if (loose_eq(n00b_unicode_block_names[i], name)) {
            if (out) {
                *out = (n00b_unicode_block_t)i;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Binary property lookup.
// ---------------------------------------------------------------------------

typedef struct {
    const char            *name;
    n00b_unicode_property_t value;
} _prop_name_t;

static const _prop_name_t _prop_names[] = {
    {"White_Space", N00B_UNICODE_PROP_WHITE_SPACE},
    {"WSpace",      N00B_UNICODE_PROP_WHITE_SPACE},
    {"space",       N00B_UNICODE_PROP_WHITE_SPACE},

    {"Alphabetic", N00B_UNICODE_PROP_ALPHABETIC},
    {"Alpha",      N00B_UNICODE_PROP_ALPHABETIC},

    {"Noncharacter_Code_Point", N00B_UNICODE_PROP_NONCHARACTER_CODE_POINT},
    {"NChar",                   N00B_UNICODE_PROP_NONCHARACTER_CODE_POINT},

    {"Default_Ignorable_Code_Point",
     N00B_UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT},
    {"DI", N00B_UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT},

    {"Deprecated", N00B_UNICODE_PROP_DEPRECATED},
    {"Dep",        N00B_UNICODE_PROP_DEPRECATED},

    {"Logical_Order_Exception", N00B_UNICODE_PROP_LOGICAL_ORDER_EXCEPTION},
    {"LOE",                     N00B_UNICODE_PROP_LOGICAL_ORDER_EXCEPTION},

    {"Variation_Selector", N00B_UNICODE_PROP_VARIATION_SELECTOR},
    {"VS",                 N00B_UNICODE_PROP_VARIATION_SELECTOR},

    {"Uppercase",   N00B_UNICODE_PROP_UPPERCASE},
    {"Upper",       N00B_UNICODE_PROP_UPPERCASE},
    {"Lowercase",   N00B_UNICODE_PROP_LOWERCASE},
    {"Lower",       N00B_UNICODE_PROP_LOWERCASE},
    {"Soft_Dotted", N00B_UNICODE_PROP_SOFT_DOTTED},
    {"SD",          N00B_UNICODE_PROP_SOFT_DOTTED},
    {"Case_Ignorable", N00B_UNICODE_PROP_CASE_IGNORABLE},
    {"CI",             N00B_UNICODE_PROP_CASE_IGNORABLE},
    {"Cased",       N00B_UNICODE_PROP_CASED},

    {"Changes_When_Lowercased", N00B_UNICODE_PROP_CHANGES_WHEN_LOWERCASED},
    {"CWL",                     N00B_UNICODE_PROP_CHANGES_WHEN_LOWERCASED},
    {"Changes_When_Uppercased", N00B_UNICODE_PROP_CHANGES_WHEN_UPPERCASED},
    {"CWU",                     N00B_UNICODE_PROP_CHANGES_WHEN_UPPERCASED},
    {"Changes_When_Titlecased", N00B_UNICODE_PROP_CHANGES_WHEN_TITLECASED},
    {"CWT",                     N00B_UNICODE_PROP_CHANGES_WHEN_TITLECASED},
    {"Changes_When_Casefolded", N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED},
    {"CWCF",                    N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED},
    {"Changes_When_Casemapped", N00B_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED},
    {"CWCM",                    N00B_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED},

    {"ID_Start",     N00B_UNICODE_PROP_ID_START},
    {"IDS",          N00B_UNICODE_PROP_ID_START},
    {"ID_Continue",  N00B_UNICODE_PROP_ID_CONTINUE},
    {"IDC",          N00B_UNICODE_PROP_ID_CONTINUE},
    {"XID_Start",    N00B_UNICODE_PROP_XID_START},
    {"XIDS",         N00B_UNICODE_PROP_XID_START},
    {"XID_Continue", N00B_UNICODE_PROP_XID_CONTINUE},
    {"XIDC",         N00B_UNICODE_PROP_XID_CONTINUE},

    {"Pattern_Syntax",      N00B_UNICODE_PROP_PATTERN_SYNTAX},
    {"Pat_Syn",             N00B_UNICODE_PROP_PATTERN_SYNTAX},
    {"Pattern_White_Space", N00B_UNICODE_PROP_PATTERN_WHITE_SPACE},
    {"Pat_WS",              N00B_UNICODE_PROP_PATTERN_WHITE_SPACE},

    {"Dash",                 N00B_UNICODE_PROP_DASH},
    {"Quotation_Mark",       N00B_UNICODE_PROP_QUOTATION_MARK},
    {"QMark",                N00B_UNICODE_PROP_QUOTATION_MARK},
    {"Terminal_Punctuation", N00B_UNICODE_PROP_TERMINAL_PUNCTUATION},
    {"Term",                 N00B_UNICODE_PROP_TERMINAL_PUNCTUATION},
    {"Sentence_Terminal",    N00B_UNICODE_PROP_SENTENCE_TERMINAL},
    {"STerm",                N00B_UNICODE_PROP_SENTENCE_TERMINAL},
    {"Diacritic",            N00B_UNICODE_PROP_DIACRITIC},
    {"Dia",                  N00B_UNICODE_PROP_DIACRITIC},
    {"Extender",             N00B_UNICODE_PROP_EXTENDER},
    {"Ext",                  N00B_UNICODE_PROP_EXTENDER},

    {"Grapheme_Base",   N00B_UNICODE_PROP_GRAPHEME_BASE},
    {"Gr_Base",         N00B_UNICODE_PROP_GRAPHEME_BASE},
    {"Grapheme_Extend", N00B_UNICODE_PROP_GRAPHEME_EXTEND},
    {"Gr_Ext",          N00B_UNICODE_PROP_GRAPHEME_EXTEND},
    {"Grapheme_Link",   N00B_UNICODE_PROP_GRAPHEME_LINK},
    {"Gr_Link",         N00B_UNICODE_PROP_GRAPHEME_LINK},

    {"Math",            N00B_UNICODE_PROP_MATH},
    {"Hex_Digit",       N00B_UNICODE_PROP_HEX_DIGIT},
    {"Hex",             N00B_UNICODE_PROP_HEX_DIGIT},
    {"ASCII_Hex_Digit", N00B_UNICODE_PROP_ASCII_HEX_DIGIT},
    {"AHex",            N00B_UNICODE_PROP_ASCII_HEX_DIGIT},

    {"Ideographic",          N00B_UNICODE_PROP_IDEOGRAPHIC},
    {"Ideo",                 N00B_UNICODE_PROP_IDEOGRAPHIC},
    {"Unified_Ideograph",    N00B_UNICODE_PROP_UNIFIED_IDEOGRAPH},
    {"UIdeo",                N00B_UNICODE_PROP_UNIFIED_IDEOGRAPH},
    {"Radical",              N00B_UNICODE_PROP_RADICAL},
    {"IDS_Binary_Operator",  N00B_UNICODE_PROP_IDS_BINARY_OPERATOR},
    {"IDSB",                 N00B_UNICODE_PROP_IDS_BINARY_OPERATOR},
    {"IDS_Trinary_Operator", N00B_UNICODE_PROP_IDS_TRINARY_OPERATOR},
    {"IDST",                 N00B_UNICODE_PROP_IDS_TRINARY_OPERATOR},
    {"Join_Control",         N00B_UNICODE_PROP_JOIN_CONTROL},
    {"Join_C",               N00B_UNICODE_PROP_JOIN_CONTROL},

    {"Emoji",                 N00B_UNICODE_PROP_EMOJI},
    {"Emoji_Presentation",    N00B_UNICODE_PROP_EMOJI_PRESENTATION},
    {"EPres",                 N00B_UNICODE_PROP_EMOJI_PRESENTATION},
    {"Emoji_Modifier",        N00B_UNICODE_PROP_EMOJI_MODIFIER},
    {"EMod",                  N00B_UNICODE_PROP_EMOJI_MODIFIER},
    {"Emoji_Modifier_Base",   N00B_UNICODE_PROP_EMOJI_MODIFIER_BASE},
    {"EBase",                 N00B_UNICODE_PROP_EMOJI_MODIFIER_BASE},
    {"Emoji_Component",       N00B_UNICODE_PROP_EMOJI_COMPONENT},
    {"EComp",                 N00B_UNICODE_PROP_EMOJI_COMPONENT},
    {"Extended_Pictographic", N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC},
    {"ExtPict",               N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC},
};

bool
n00b_unicode_property_by_name(const char *name, n00b_unicode_property_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_prop_names) / sizeof(_prop_names[0]); i++) {
        if (loose_eq(_prop_names[i].name, name)) {
            if (out) {
                *out = _prop_names[i].value;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bidi_Class lookup.
// ---------------------------------------------------------------------------

typedef struct {
    const char               *name;
    n00b_unicode_bidi_class_t value;
} _bidi_name_t;

static const _bidi_name_t _bidi_names[] = {
    {"L",   N00B_UNICODE_BIDI_L},   {"Left_To_Right",       N00B_UNICODE_BIDI_L},
    {"R",   N00B_UNICODE_BIDI_R},   {"Right_To_Left",       N00B_UNICODE_BIDI_R},
    {"AL",  N00B_UNICODE_BIDI_AL},  {"Arabic_Letter",       N00B_UNICODE_BIDI_AL},
    {"EN",  N00B_UNICODE_BIDI_EN},  {"European_Number",     N00B_UNICODE_BIDI_EN},
    {"ES",  N00B_UNICODE_BIDI_ES},  {"European_Separator",  N00B_UNICODE_BIDI_ES},
    {"ET",  N00B_UNICODE_BIDI_ET},  {"European_Terminator", N00B_UNICODE_BIDI_ET},
    {"AN",  N00B_UNICODE_BIDI_AN},  {"Arabic_Number",       N00B_UNICODE_BIDI_AN},
    {"CS",  N00B_UNICODE_BIDI_CS},  {"Common_Separator",    N00B_UNICODE_BIDI_CS},
    {"NSM", N00B_UNICODE_BIDI_NSM}, {"Nonspacing_Mark",     N00B_UNICODE_BIDI_NSM},
    {"BN",  N00B_UNICODE_BIDI_BN},  {"Boundary_Neutral",    N00B_UNICODE_BIDI_BN},
    {"B",   N00B_UNICODE_BIDI_B},   {"Paragraph_Separator", N00B_UNICODE_BIDI_B},
    {"S",   N00B_UNICODE_BIDI_S},   {"Segment_Separator",   N00B_UNICODE_BIDI_S},
    {"WS",  N00B_UNICODE_BIDI_WS},  {"White_Space",         N00B_UNICODE_BIDI_WS},
    {"ON",  N00B_UNICODE_BIDI_ON},  {"Other_Neutral",       N00B_UNICODE_BIDI_ON},
    {"LRE", N00B_UNICODE_BIDI_LRE},
    {"Left_To_Right_Embedding",     N00B_UNICODE_BIDI_LRE},
    {"LRO", N00B_UNICODE_BIDI_LRO},
    {"Left_To_Right_Override",      N00B_UNICODE_BIDI_LRO},
    {"RLE", N00B_UNICODE_BIDI_RLE},
    {"Right_To_Left_Embedding",     N00B_UNICODE_BIDI_RLE},
    {"RLO", N00B_UNICODE_BIDI_RLO},
    {"Right_To_Left_Override",      N00B_UNICODE_BIDI_RLO},
    {"PDF", N00B_UNICODE_BIDI_PDF},
    {"Pop_Directional_Format",      N00B_UNICODE_BIDI_PDF},
    {"LRI", N00B_UNICODE_BIDI_LRI},
    {"Left_To_Right_Isolate",       N00B_UNICODE_BIDI_LRI},
    {"RLI", N00B_UNICODE_BIDI_RLI},
    {"Right_To_Left_Isolate",       N00B_UNICODE_BIDI_RLI},
    {"FSI", N00B_UNICODE_BIDI_FSI},
    {"First_Strong_Isolate",        N00B_UNICODE_BIDI_FSI},
    {"PDI", N00B_UNICODE_BIDI_PDI},
    {"Pop_Directional_Isolate",     N00B_UNICODE_BIDI_PDI},
};

bool
n00b_unicode_bidi_class_by_name(const char                *name,
                                n00b_unicode_bidi_class_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_bidi_names) / sizeof(_bidi_names[0]); i++) {
        if (loose_eq(_bidi_names[i].name, name)) {
            if (out) {
                *out = _bidi_names[i].value;
            }
            return true;
        }
    }
    return false;
}
