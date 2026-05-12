#include "text/unicode/identifiers.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "internal/text/unicode/tables.h"

extern const uint16_t n00b_unicode_id_status_stage1[];
extern const uint8_t  n00b_unicode_id_status_stage2[];

/* UAX #31 recommends XID_Start / XID_Continue over plain ID_Start /
 * ID_Continue for new language designs — XID_* is closed under NFKC
 * normalization, so `is_id_start(x)` and `is_id_start(NFKC(x))` agree
 * for any character that would normalize to a sequence headed by an
 * identifier start.  The two property sets differ by a tiny handful
 * of NFKC-unstable letters; XID_* is the version you want.
 *
 * We route the canonical predicates through the XID variants and
 * keep `is_xid_*` as explicit synonyms for callers that want to be
 * loud about the choice. */
bool
n00b_unicode_is_id_start(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_XID_START);
}

bool
n00b_unicode_is_id_continue(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_XID_CONTINUE);
}

bool
n00b_unicode_is_xid_start(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_XID_START);
}

bool
n00b_unicode_is_xid_continue(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_XID_CONTINUE);
}

bool
n00b_unicode_is_pattern_syntax(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_PATTERN_SYNTAX);
}

bool
n00b_unicode_is_pattern_white_space(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_PATTERN_WHITE_SPACE);
}

bool
n00b_unicode_is_identifier_allowed(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return false;
    return N00B_UNICODE_LOOKUP(n00b_unicode_id_status_stage1, n00b_unicode_id_status_stage2, cp)
        == 1;
}

bool
n00b_unicode_is_valid_identifier_raw(const char *data, int64_t len)
{
    if (len <= 0)
        return false;

    uint32_t pos   = 0;
    int32_t  first = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (first < 0 || !n00b_unicode_is_xid_start((n00b_codepoint_t)first))
        return false;

    while (pos < (uint32_t)len) {
        int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
        if (cp < 0 || !n00b_unicode_is_xid_continue((n00b_codepoint_t)cp))
            return false;
    }

    return true;
}

bool
n00b_unicode_is_valid_identifier(n00b_string_t *s)
{
    return n00b_unicode_is_valid_identifier_raw(s->data, s->u8_bytes);
}
