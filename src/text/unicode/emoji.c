#include "text/unicode/emoji.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"

bool
n00b_unicode_is_emoji(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_EMOJI);
}

bool
n00b_unicode_is_emoji_presentation(n00b_codepoint_t cp)
{
    return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_EMOJI_PRESENTATION);
}

// Scan an emoji sequence starting at byte_pos.
// Returns the type and sets *seq_bytes to the number of bytes consumed.
n00b_unicode_emoji_type_t
n00b_unicode_emoji_scan_raw(const char *data,
                            int64_t     len,
                            uint32_t    byte_pos,
                            uint32_t   *seq_bytes)
{
    uint32_t num_bytes = (uint32_t)len;

    if (byte_pos >= num_bytes) {
        if (seq_bytes)
            *seq_bytes = 0;
        return N00B_UNICODE_EMOJI_NONE;
    }

    uint32_t pos = byte_pos;
    int32_t  cp  = n00b_unicode_utf8_decode(data, num_bytes, &pos);
    if (cp < 0) {
        if (seq_bytes)
            *seq_bytes = 0;
        return N00B_UNICODE_EMOJI_NONE;
    }

    // Must start with an emoji character
    if (!n00b_unicode_has_property((n00b_codepoint_t)cp, N00B_UNICODE_PROP_EMOJI)
        && !n00b_unicode_has_property((n00b_codepoint_t)cp,
                                      N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC)) {
        if (seq_bytes)
            *seq_bytes = 0;
        return N00B_UNICODE_EMOJI_NONE;
    }

    uint32_t                  start_pos = byte_pos;
    n00b_unicode_emoji_type_t result    = N00B_UNICODE_EMOJI_BASIC;

    // Check for emoji presentation (has Emoji_Presentation or followed by VS16)
    if (n00b_unicode_has_property((n00b_codepoint_t)cp, N00B_UNICODE_PROP_EMOJI_PRESENTATION)) {
        result = N00B_UNICODE_EMOJI_PRESENTATION_SEQ;
    }

    // Check for VS16 (U+FE0F)
    if (pos < num_bytes) {
        uint32_t save = pos;
        int32_t  next = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        if (next == 0xFE0F) {
            result = N00B_UNICODE_EMOJI_PRESENTATION_SEQ;
        }
        else {
            pos = save;
        }
    }

    // Check for keycap sequence: [0-9#*] + VS16 + U+20E3
    if ((cp >= '0' && cp <= '9') || cp == '#' || cp == '*') {
        uint32_t save = pos;
        int32_t  n1   = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        if (n1 == 0x20E3) {
            result = N00B_UNICODE_EMOJI_KEYCAP;
        }
        else if (n1 == 0xFE0F && pos < num_bytes) {
            int32_t n2 = n00b_unicode_utf8_decode(data, num_bytes, &pos);
            if (n2 == 0x20E3) {
                result = N00B_UNICODE_EMOJI_KEYCAP;
            }
            else {
                pos = save;
            }
        }
        else {
            pos = save;
        }
    }

    // Check for emoji modifier sequence
    if (n00b_unicode_has_property((n00b_codepoint_t)cp,
                                  N00B_UNICODE_PROP_EMOJI_MODIFIER_BASE)) {
        uint32_t save = pos;
        if (pos < num_bytes) {
            int32_t mod = n00b_unicode_utf8_decode(data, num_bytes, &pos);
            if (mod >= 0
                && n00b_unicode_has_property((n00b_codepoint_t)mod,
                                             N00B_UNICODE_PROP_EMOJI_MODIFIER)) {
                result = N00B_UNICODE_EMOJI_MODIFIER_SEQ;
            }
            else {
                pos = save;
            }
        }
    }

    // Check for flag sequence (Regional_Indicator × Regional_Indicator)
    if (n00b_unicode_has_property((n00b_codepoint_t)cp, N00B_UNICODE_PROP_EMOJI)
        && cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        uint32_t save = pos;
        if (pos < num_bytes) {
            int32_t ri2 = n00b_unicode_utf8_decode(data, num_bytes, &pos);
            if (ri2 >= 0x1F1E6 && ri2 <= 0x1F1FF) {
                result = N00B_UNICODE_EMOJI_FLAG;
            }
            else {
                pos = save;
            }
        }
    }

    // Check for ZWJ sequence: consume (ZWJ + emoji)+ chain
    bool had_zwj = false;
    while (pos < num_bytes) {
        uint32_t save = pos;
        int32_t  next = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        if (next == 0x200D) { // ZWJ
            if (pos < num_bytes) {
                int32_t after_zwj = n00b_unicode_utf8_decode(data, num_bytes, &pos);
                if (after_zwj >= 0
                    && (n00b_unicode_has_property((n00b_codepoint_t)after_zwj,
                                                  N00B_UNICODE_PROP_EMOJI)
                        || n00b_unicode_has_property(
                            (n00b_codepoint_t)after_zwj,
                            N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC))) {
                    had_zwj = true;
                    // Consume possible VS16 after
                    if (pos < num_bytes) {
                        uint32_t vs_save = pos;
                        int32_t  vs      = n00b_unicode_utf8_decode(data, num_bytes, &pos);
                        if (vs != 0xFE0F)
                            pos = vs_save;
                    }
                    // Consume possible modifier after
                    if (pos < num_bytes) {
                        uint32_t mod_save = pos;
                        int32_t  mod      = n00b_unicode_utf8_decode(data, num_bytes, &pos);
                        if (mod < 0
                            || !n00b_unicode_has_property((n00b_codepoint_t)mod,
                                                          N00B_UNICODE_PROP_EMOJI_MODIFIER)) {
                            pos = mod_save;
                        }
                    }
                    continue;
                }
            }
            pos = save;
            break;
        }
        else if (next == 0xFE0F) {
            // VS16 within sequence — continue
            continue;
        }
        else if (next >= 0xE0020 && next <= 0xE007E) {
            // Tag characters (for tag sequences like flag subdivisions)
            while (pos < num_bytes) {
                uint32_t tag_save = pos;
                int32_t  tag      = n00b_unicode_utf8_decode(data, num_bytes, &pos);
                if (tag == 0xE007F) {
                    result = N00B_UNICODE_EMOJI_TAG_SEQ;
                    break;
                }
                if (tag < 0xE0020 || tag > 0xE007E) {
                    pos = tag_save;
                    break;
                }
            }
            break;
        }
        else {
            pos = save;
            break;
        }
    }

    if (had_zwj)
        result = N00B_UNICODE_EMOJI_ZWJ_SEQ;

    if (seq_bytes)
        *seq_bytes = pos - start_pos;
    return result;
}

n00b_unicode_emoji_scan_result_t
n00b_unicode_emoji_scan(n00b_string_t s, uint32_t byte_pos)
{
    uint32_t                  seq_bytes = 0;
    n00b_unicode_emoji_type_t type
        = n00b_unicode_emoji_scan_raw(s.data, s.u8_bytes, byte_pos, &seq_bytes);
    return (n00b_unicode_emoji_scan_result_t){.type = type, .seq_bytes = seq_bytes};
}
