
#include "private/parse/unicode_class.h"

static bool
is_ascii_upper(int32_t cp)
{
    return cp >= 'A' && cp <= 'Z';
}

static bool
is_ascii_lower(int32_t cp)
{
    return cp >= 'a' && cp <= 'z';
}

static bool
is_ascii_alpha(int32_t cp)
{
    return is_ascii_upper(cp) || is_ascii_lower(cp);
}

static bool
is_hex_digit(int32_t cp)
{
    return unicode_cp_is_ascii_digit(cp) || (cp >= 'a' && cp <= 'f')
        || (cp >= 'A' && cp <= 'F');
}

static bool
is_nonzero_digit(int32_t cp)
{
    return cp >= '1' && cp <= '9';
}

static bool
is_whitespace(int32_t cp)
{
    return unicode_has_property((core_codepoint_t)cp,
                                     UNICODE_PROP_WHITE_SPACE);
}

static bool
is_printable(int32_t cp)
{
    if (cp < 0) {
        return false;
    }

    unicode_gc_t gc = unicode_general_category((core_codepoint_t)cp);

    if (gc == UNICODE_GC_CC) {
        return false;
    }

    if (gc == UNICODE_GC_CN || gc == UNICODE_GC_CS) {
        return false;
    }

    return true;
}

static bool
is_non_ws_printable(int32_t cp)
{
    return is_printable(cp) && !is_whitespace(cp);
}

static bool
is_non_nl_ws(int32_t cp)
{
    return is_whitespace(cp) && cp != '\n' && cp != '\r';
}

static bool
is_non_nl_printable(int32_t cp)
{
    return is_printable(cp) && cp != '\n' && cp != '\r';
}

static bool
is_json_string_char(int32_t cp)
{
    if (cp < 0x20) {
        return false;
    }

    if (cp == '"' || cp == '\\') {
        return false;
    }

    return true;
}

bool
parse_codepoint_matches_class(int32_t cp, parse_char_class_t cc)
{
    switch (cc) {
    case PARSE_CC_ID_START:
        return unicode_is_id_start((core_codepoint_t)cp);
    case PARSE_CC_ID_CONTINUE:
        return unicode_is_id_continue((core_codepoint_t)cp);
    case PARSE_CC_ASCII_DIGIT:
        return unicode_cp_is_ascii_digit(cp);
    case PARSE_CC_UNICODE_DIGIT:
        return unicode_general_category((core_codepoint_t)cp) == UNICODE_GC_ND;
    case PARSE_CC_ASCII_UPPER:
        return is_ascii_upper(cp);
    case PARSE_CC_ASCII_LOWER:
        return is_ascii_lower(cp);
    case PARSE_CC_ASCII_ALPHA:
        return is_ascii_alpha(cp);
    case PARSE_CC_WHITESPACE:
        return is_whitespace(cp);
    case PARSE_CC_HEX_DIGIT:
        return is_hex_digit(cp);
    case PARSE_CC_NONZERO_DIGIT:
        return is_nonzero_digit(cp);
    case PARSE_CC_PRINTABLE:
        return is_printable(cp);
    case PARSE_CC_NON_WS_PRINTABLE:
        return is_non_ws_printable(cp);
    case PARSE_CC_NON_NL_WS:
        return is_non_nl_ws(cp);
    case PARSE_CC_NON_NL_PRINTABLE:
        return is_non_nl_printable(cp);
    case PARSE_CC_JSON_STRING_CHAR:
        return is_json_string_char(cp);
    case PARSE_CC_REGEX_BODY_CHAR:
        return is_printable(cp) && cp != '/';
    }

    return false;
}
