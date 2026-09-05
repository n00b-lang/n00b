#pragma once

#include "ncc.h"

static inline int32_t
ncc_unicode_utf8_decode(const char *src, uint32_t len, uint32_t *pos)
{
    if (*pos >= len) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)src + *pos;
    uint32_t       remaining = len - *pos;
    uint32_t       cp;
    uint32_t       bytes;

    uint8_t b0 = p[0];

    if (b0 < 0x80) {
        cp    = b0;
        bytes = 1;
    }
    else if ((b0 & 0xE0) == 0xC0) {
        if (remaining < 2) {
            return -1;
        }
        if ((p[1] & 0xC0) != 0x80) {
            return -1;
        }
        cp    = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
        bytes = 2;
        if (cp < 0x80) {
            return -1;
        }
    }
    else if ((b0 & 0xF0) == 0xE0) {
        if (remaining < 3) {
            return -1;
        }
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
            return -1;
        }
        cp = ((uint32_t)(b0 & 0x0F) << 12)
           | ((uint32_t)(p[1] & 0x3F) << 6)
           | (p[2] & 0x3F);
        bytes = 3;
        if (cp < 0x800) {
            return -1;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return -1;
        }
    }
    else if ((b0 & 0xF8) == 0xF0) {
        if (remaining < 4) {
            return -1;
        }
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80
            || (p[3] & 0xC0) != 0x80) {
            return -1;
        }
        cp = ((uint32_t)(b0 & 0x07) << 18)
           | ((uint32_t)(p[1] & 0x3F) << 12)
           | ((uint32_t)(p[2] & 0x3F) << 6)
           | (p[3] & 0x3F);
        bytes = 4;
        if (cp < 0x10000 || cp > 0x10FFFF) {
            return -1;
        }
    }
    else {
        return -1;
    }

    *pos += bytes;
    return (int32_t)cp;
}

static inline uint32_t
ncc_unicode_utf8_encode(ncc_codepoint_t cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    else if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    else if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    else {
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static inline bool
ncc_unicode_utf8_validate(const char *src, uint32_t len)
{
    uint32_t pos = 0;

    while (pos < len) {
        if (ncc_unicode_utf8_decode(src, len, &pos) < 0) {
            return false;
        }
    }
    return true;
}
