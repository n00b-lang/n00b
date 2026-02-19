#include "unicode/encoding.h"
#include <string.h>
#include "internal/unicode/raw.h"
#include "core/alloc.h"

// ---------------------------------------------------------------------------
// UTF-8 decode: returns codepoint at src[*pos], advances *pos.
// Returns -1 on invalid sequence or if *pos >= len.
// ---------------------------------------------------------------------------

int32_t n00b_unicode_utf8_decode(const char *src, uint32_t len, uint32_t *pos) {
  if (*pos >= len) {
    return -1;
  }

  uint8_t b0 = (uint8_t)src[*pos];

  if (b0 < 0x80) {
    (*pos)++;
    return b0;
  }

  n00b_codepoint_t cp;
  uint32_t need;

  if ((b0 & 0xE0) == 0xC0) {
    cp = b0 & 0x1F;
    need = 2;
  } else if ((b0 & 0xF0) == 0xE0) {
    cp = b0 & 0x0F;
    need = 3;
  } else if ((b0 & 0xF8) == 0xF0) {
    cp = b0 & 0x07;
    need = 4;
  } else {
    (*pos)++;
    return -1; // invalid lead byte
  }

  if (*pos + need > len) {
    (*pos)++;
    return -1; // truncated
  }

  for (uint32_t i = 1; i < need; i++) {
    uint8_t b = (uint8_t)src[*pos + i];
    if ((b & 0xC0) != 0x80) {
      (*pos)++;
      return -1; // bad continuation
    }
    cp = (cp << 6) | (b & 0x3F);
  }

  *pos += need;

  // Overlong check
  if (need == 2 && cp < 0x80)
    return -1;
  if (need == 3 && cp < 0x800)
    return -1;
  if (need == 4 && cp < 0x10000)
    return -1;

  // Surrogates and out-of-range
  if (cp >= 0xD800 && cp <= 0xDFFF)
    return -1;
  if (cp > 0x10FFFF)
    return -1;

  return (int32_t)cp;
}

// ---------------------------------------------------------------------------
// UTF-8 encode: write codepoint to dst, return bytes written (1–4).
// dst must have at least 4 bytes available.
// ---------------------------------------------------------------------------

uint32_t n00b_unicode_utf8_encode(n00b_codepoint_t cp, char *dst) {
  if (cp < 0x80) {
    dst[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = (char)(0xF0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// ---------------------------------------------------------------------------
// UTF-8 validation
// ---------------------------------------------------------------------------

bool n00b_unicode_utf8_validate(const char *src, uint32_t len) {
  uint32_t pos = 0;
  while (pos < len) {
    if (n00b_unicode_utf8_decode(src, len, &pos) < 0) {
      return false;
    }
  }
  return true;
}

bool n00b_unicode_str_validate(n00b_string_t s) {
  return n00b_unicode_utf8_validate(s.data, (uint32_t)s.u8_bytes);
}

// ---------------------------------------------------------------------------
// Count codepoints
// ---------------------------------------------------------------------------

int64_t n00b_unicode_utf8_count_codepoints_raw(const char *src, uint32_t len) {
  uint32_t pos = 0;
  int64_t count = 0;

  while (pos < len) {
    if (n00b_unicode_utf8_decode(src, len, &pos) < 0) {
      return -1;
    }
    count++;
  }
  return count;
}

int64_t n00b_unicode_utf8_count_codepoints(n00b_string_t s) {
  return n00b_unicode_utf8_count_codepoints_raw(s.data, (uint32_t)s.u8_bytes);
}

// ---------------------------------------------------------------------------
// UTF-16 conversion
// ---------------------------------------------------------------------------

static uint16_t *n00b_unicode_to_utf16_raw(const char *data, int64_t len,
                                      uint32_t *out_len,
                                      n00b_allocator_t *allocator) {
  // First, count codepoints so we can size the output buffer.
  // Worst case: each codepoint becomes a surrogate pair (2 uint16_t).
  int64_t cp_count = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
  if (cp_count < 0) {
    cp_count = len; // fallback upper bound
  }

  uint16_t *buf =
      n00b_alloc_array(char, (size_t)(cp_count * 2 + 1) * sizeof(uint16_t),
                       .allocator = allocator);
  uint32_t pos = 0;
  uint32_t idx = 0;
  uint32_t byte_len = (uint32_t)len;

  while (pos < byte_len) {
    int32_t cp = n00b_unicode_utf8_decode(data, byte_len, &pos);
    if (cp < 0)
      break;

    if (cp < 0x10000) {
      buf[idx++] = (uint16_t)cp;
    } else {
      cp -= 0x10000;
      buf[idx++] = (uint16_t)(0xD800 + (cp >> 10));
      buf[idx++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
    }
  }

  if (out_len)
    *out_len = idx;
  return buf;
}

uint16_t *
n00b_unicode_to_utf16(n00b_string_t s, uint32_t *out_len)
_kargs { n00b_allocator_t *allocator = nullptr; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_to_utf16_raw(s.data, s.u8_bytes, out_len, allocator);
}

static n00b_codepoint_t *
n00b_unicode_to_utf32_raw(const char *data, int64_t len, uint32_t *out_len,
                     n00b_allocator_t *allocator) {
  int64_t cp_count = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
  if (cp_count < 0) {
    cp_count = len; // fallback upper bound
  }

  n00b_codepoint_t *buf =
      n00b_alloc_array(char, (size_t)(cp_count + 1) * sizeof(n00b_codepoint_t),
                       .allocator = allocator);
  uint32_t pos = 0;
  uint32_t idx = 0;
  uint32_t byte_len = (uint32_t)len;

  while (pos < byte_len) {
    int32_t cp = n00b_unicode_utf8_decode(data, byte_len, &pos);
    if (cp < 0)
      break;
    buf[idx++] = (n00b_codepoint_t)cp;
  }

  if (out_len)
    *out_len = idx;
  return buf;
}

n00b_codepoint_t *
n00b_unicode_to_utf32(n00b_string_t s, uint32_t *out_len)
_kargs { n00b_allocator_t *allocator = nullptr; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_to_utf32_raw(s.data, s.u8_bytes, out_len, allocator);
}

n00b_string_t
n00b_unicode_from_utf16(const uint16_t *src, uint32_t len)
_kargs { n00b_allocator_t *allocator = nullptr; }
{
  if (!allocator)
    allocator = nullptr;

  // Worst case: each code unit -> 4 UTF-8 bytes
  char *buf = n00b_alloc_array(char, (size_t)len * 4 + 1);
  uint32_t pos = 0;
  uint32_t cp_count = 0;

  for (uint32_t i = 0; i < len; i++) {
    n00b_codepoint_t cp;
    if (src[i] >= 0xD800 && src[i] <= 0xDBFF && i + 1 < len &&
        src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((n00b_codepoint_t)(src[i] - 0xD800) << 10) +
           (src[i + 1] - 0xDC00);
      i++;
    } else {
      cp = src[i];
    }
    pos += n00b_unicode_utf8_encode(cp, buf + pos);
    cp_count++;
  }

  buf[pos] = '\0';

  n00b_string_t result = n00b_string_from_raw(allocator, buf, pos, cp_count);
  n00b_free(buf);
  return result;
}

n00b_string_t
n00b_unicode_from_utf32(const n00b_codepoint_t *src, uint32_t len)
_kargs { n00b_allocator_t *allocator = nullptr; }
{
  if (!allocator)
    allocator = nullptr;

  char *buf = n00b_alloc_array(char, (size_t)len * 4 + 1);
  uint32_t pos = 0;

  for (uint32_t i = 0; i < len; i++) {
    pos += n00b_unicode_utf8_encode(src[i], buf + pos);
  }

  buf[pos] = '\0';

  n00b_string_t result = n00b_string_from_raw(allocator, buf, pos, len);
  n00b_free(buf);
  return result;
}

// ---------------------------------------------------------------------------
// BOM detection
// ---------------------------------------------------------------------------

n00b_unicode_bom_t n00b_unicode_detect_bom(const char *data, uint32_t len,
                                 uint32_t *bom_len) {
  const uint8_t *d = (const uint8_t *)data;

  if (len >= 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0xFE &&
      d[3] == 0xFF) {
    if (bom_len)
      *bom_len = 4;
    return N00B_UNICODE_BOM_UTF32_BE;
  }
  if (len >= 4 && d[0] == 0xFF && d[1] == 0xFE && d[2] == 0x00 &&
      d[3] == 0x00) {
    if (bom_len)
      *bom_len = 4;
    return N00B_UNICODE_BOM_UTF32_LE;
  }
  if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
    if (bom_len)
      *bom_len = 3;
    return N00B_UNICODE_BOM_UTF8;
  }
  if (len >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
    if (bom_len)
      *bom_len = 2;
    return N00B_UNICODE_BOM_UTF16_BE;
  }
  if (len >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
    if (bom_len)
      *bom_len = 2;
    return N00B_UNICODE_BOM_UTF16_LE;
  }

  if (bom_len)
    *bom_len = 0;
  return N00B_UNICODE_BOM_NONE;
}
