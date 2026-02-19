#include "unicode/string_ops.h"
#include "unicode/casemap.h"
#include "unicode/encoding.h"
#include "unicode/normalization.h"
#include "unicode/properties.h"
#include "unicode/segmentation.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Concatenation
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_str_cat_raw(n00b_allocator_t *allocator, const char *a,
                                int64_t a_len, const char *b, int64_t b_len) {
  uint32_t total = (uint32_t)a_len + (uint32_t)b_len;
  char *buf = n00b_alloc_array(char, total + 1);
  memcpy(buf, a, (size_t)a_len);
  memcpy(buf + a_len, b, (size_t)b_len);
  buf[total] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, total);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_cat(n00b_string_t a, n00b_string_t b) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_str_cat_raw(allocator, a.data, a.u8_bytes, b.data, b.u8_bytes);
}

n00b_string_t n00b_unicode_str_cat_many(n00b_string_t *parts, uint32_t count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  if (count == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  uint32_t total_bytes = 0;
  int64_t total_cps = 0;
  for (uint32_t i = 0; i < count; i++) {
    total_bytes += (uint32_t)parts[i].u8_bytes;
    total_cps += parts[i].codepoints;
  }

  char *buf = n00b_alloc_array(char, total_bytes + 1);
  uint32_t pos = 0;
  for (uint32_t i = 0; i < count; i++) {
    memcpy(buf + pos, parts[i].data, (size_t)parts[i].u8_bytes);
    pos += (uint32_t)parts[i].u8_bytes;
  }
  buf[total_bytes] = '\0';

  n00b_string_t result = n00b_string_from_raw(allocator, buf, total_bytes, total_cps);
  n00b_free(buf);
  return result;
}

// Internal helper: concatenate multiple raw segments.
static n00b_string_t cat_many_raw(n00b_allocator_t *allocator, const char **parts,
                                int64_t *lengths, uint32_t count) {
  uint32_t total_bytes = 0;
  for (uint32_t i = 0; i < count; i++) {
    total_bytes += (uint32_t)lengths[i];
  }

  char *buf = n00b_alloc_array(char, total_bytes + 1);
  uint32_t pos = 0;
  for (uint32_t i = 0; i < count; i++) {
    memcpy(buf + pos, parts[i], (size_t)lengths[i]);
    pos += (uint32_t)lengths[i];
  }
  buf[total_bytes] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, total_bytes);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total_bytes, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_join(n00b_string_t sep, n00b_string_t *parts,
                             uint32_t count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  if (count == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  const char *sep_data = sep.data;
  int64_t sep_len = sep.u8_bytes;

  uint32_t total_bytes = 0;
  for (uint32_t i = 0; i < count; i++) {
    total_bytes += (uint32_t)parts[i].u8_bytes;
    if (i > 0) {
      total_bytes += (uint32_t)sep_len;
    }
  }

  char *buf = n00b_alloc_array(char, total_bytes + 1);
  uint32_t pos = 0;

  for (uint32_t i = 0; i < count; i++) {
    if (i > 0) {
      memcpy(buf + pos, sep_data, (size_t)sep_len);
      pos += (uint32_t)sep_len;
    }
    memcpy(buf + pos, parts[i].data, (size_t)parts[i].u8_bytes);
    pos += (uint32_t)parts[i].u8_bytes;
  }
  buf[total_bytes] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, total_bytes);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total_bytes, cps);
  n00b_free(buf);
  return result;
}

// ---------------------------------------------------------------------------
// Grapheme-aware slicing helpers
// ---------------------------------------------------------------------------

// Build a byte-offset array for grapheme boundaries.
// Returns count of grapheme clusters. offsets[i] = byte start of grapheme i.
// offsets[count] = num_bytes (sentinel).
static uint32_t get_grapheme_offsets(const char *data, int64_t len,
                                     uint32_t **offsets_out) {
  uint32_t num_bytes = (uint32_t)len;
  uint32_t cap = num_bytes > 0 ? num_bytes + 1 : 16;
  uint32_t *offs = n00b_alloc_array(char, cap * sizeof(uint32_t));
  uint32_t count = 0;

  offs[count++] = 0;

  n00b_unicode_break_iter_t *it = n00b_unicode_grapheme_iter_raw(data, len);
  int32_t b;
  while ((b = n00b_unicode_break_next(it)) >= 0) {
    if (count >= cap) {
      cap *= 2;
      {
        uint32_t *new_offs = n00b_alloc_array(char, cap * sizeof(uint32_t));
        memcpy(new_offs, offs, count * sizeof(uint32_t));
        n00b_free(offs);
        offs = new_offs;
      }
    }
    offs[count++] = (uint32_t)b;
  }

  // Ensure sentinel
  if (count == 0 || offs[count - 1] != num_bytes) {
    if (count >= cap) {
      cap++;
      {
        uint32_t *new_offs = n00b_alloc_array(char, cap * sizeof(uint32_t));
        memcpy(new_offs, offs, count * sizeof(uint32_t));
        n00b_free(offs);
        offs = new_offs;
      }
    }
    offs[count++] = num_bytes;
  }

  n00b_unicode_break_iter_free(it);
  *offsets_out = offs;
  return count - 1; // number of grapheme clusters
}

// Resolve negative index (Python-style: -1 = last)
static int32_t resolve_index(int32_t idx, uint32_t n_graphemes) {
  if (idx < 0)
    idx += (int32_t)n_graphemes;
  if (idx < 0)
    idx = 0;
  if ((uint32_t)idx > n_graphemes)
    idx = (int32_t)n_graphemes;
  return idx;
}

n00b_string_t n00b_unicode_str_slice_raw(n00b_allocator_t *allocator, const char *data,
                                  int64_t len, int32_t start, int32_t end) {
  if (len == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  uint32_t *offsets;
  uint32_t n = get_grapheme_offsets(data, len, &offsets);

  start = resolve_index(start, n);
  end = resolve_index(end, n);

  n00b_string_t result;
  if (start >= end) {
    result = n00b_string_from_raw(allocator, "", 0, 0);
  } else {
    uint32_t byte_start = offsets[start];
    uint32_t byte_end = offsets[end];
    uint32_t slice_len = byte_end - byte_start;
    int64_t cps =
        n00b_unicode_utf8_count_codepoints_raw(data + byte_start, slice_len);
    result = n00b_string_from_raw(allocator, data + byte_start, slice_len, cps);
  }

  n00b_free(offsets);
  return result;
}

n00b_string_t n00b_unicode_str_slice(n00b_string_t s, int32_t start,
                              int32_t end) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_str_slice_raw(allocator, s.data, s.u8_bytes, start, end);
}

n00b_string_t n00b_unicode_str_grapheme_at(n00b_string_t s, int32_t index) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  if (len == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  uint32_t *offsets;
  uint32_t n = get_grapheme_offsets(data, len, &offsets);

  index = resolve_index(index, n);

  n00b_string_t result;
  if ((uint32_t)index >= n) {
    result = n00b_string_from_raw(allocator, "", 0, 0);
  } else {
    uint32_t byte_start = offsets[index];
    uint32_t byte_end = offsets[index + 1];
    uint32_t seg_len = byte_end - byte_start;
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + byte_start, seg_len);
    result = n00b_string_from_raw(allocator, data + byte_start, seg_len, cps);
  }

  n00b_free(offsets);
  return result;
}

// ---------------------------------------------------------------------------
// Byte-level slicing
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_str_slice_bytes(n00b_string_t s, uint32_t byte_start,
                                    uint32_t byte_end) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  if (byte_start > (uint32_t)len)
    byte_start = (uint32_t)len;
  if (byte_end > (uint32_t)len)
    byte_end = (uint32_t)len;
  if (byte_start >= byte_end)
    return n00b_string_from_raw(allocator, "", 0, 0);

  uint32_t slice_len = byte_end - byte_start;
  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + byte_start, slice_len);
  return n00b_string_from_raw(allocator, data + byte_start, slice_len, cps);
}

// ---------------------------------------------------------------------------
// Search (byte-level, using memmem-style)
// ---------------------------------------------------------------------------

static const char *find_bytes(const char *haystack, uint32_t hlen,
                              const char *needle, uint32_t nlen) {
  if (nlen == 0)
    return haystack;
  if (nlen > hlen)
    return NULL;

  for (uint32_t i = 0; i <= hlen - nlen; i++) {
    if (memcmp(haystack + i, needle, nlen) == 0) {
      return haystack + i;
    }
  }
  return NULL;
}

static const char *rfind_bytes(const char *haystack, uint32_t hlen,
                               const char *needle, uint32_t nlen) {
  if (nlen == 0)
    return haystack + hlen;
  if (nlen > hlen)
    return NULL;

  for (uint32_t i = hlen - nlen;; i--) {
    if (memcmp(haystack + i, needle, nlen) == 0) {
      return haystack + i;
    }
    if (i == 0)
      break;
  }
  return NULL;
}

int32_t n00b_unicode_str_find_raw(const char *haystack, int64_t h_len,
                             const char *needle, int64_t n_len) {
  const char *p =
      find_bytes(haystack, (uint32_t)h_len, needle, (uint32_t)n_len);
  return p ? (int32_t)(p - haystack) : -1;
}

n00b_unicode_opt_i32_t n00b_unicode_str_find(n00b_string_t haystack, n00b_string_t needle) {
  int32_t r = n00b_unicode_str_find_raw(haystack.data, haystack.u8_bytes, needle.data,
                                   needle.u8_bytes);
  if (r < 0)
    return (n00b_unicode_opt_i32_t){ .has_value = false };
  return (n00b_unicode_opt_i32_t){ .has_value = true, .value = r };
}

int32_t n00b_unicode_str_rfind_raw(const char *haystack, int64_t h_len,
                              const char *needle, int64_t n_len) {
  const char *p =
      rfind_bytes(haystack, (uint32_t)h_len, needle, (uint32_t)n_len);
  return p ? (int32_t)(p - haystack) : -1;
}

n00b_unicode_opt_i32_t n00b_unicode_str_rfind(n00b_string_t haystack, n00b_string_t needle) {
  int32_t r = n00b_unicode_str_rfind_raw(haystack.data, haystack.u8_bytes, needle.data,
                                    needle.u8_bytes);
  if (r < 0)
    return (n00b_unicode_opt_i32_t){ .has_value = false };
  return (n00b_unicode_opt_i32_t){ .has_value = true, .value = r };
}

bool n00b_unicode_str_contains_raw(const char *haystack, int64_t h_len,
                              const char *needle, int64_t n_len) {
  return n00b_unicode_str_find_raw(haystack, h_len, needle, n_len) >= 0;
}

bool n00b_unicode_str_contains(n00b_string_t haystack, n00b_string_t needle) {
  return n00b_unicode_str_contains_raw(haystack.data, haystack.u8_bytes, needle.data,
                                  needle.u8_bytes);
}

bool n00b_unicode_str_starts_with_raw(const char *data, int64_t len,
                                 const char *prefix, int64_t prefix_len) {
  if (prefix_len > len)
    return false;
  return memcmp(data, prefix, (size_t)prefix_len) == 0;
}

bool n00b_unicode_str_starts_with(n00b_string_t s, n00b_string_t prefix) {
  return n00b_unicode_str_starts_with_raw(s.data, s.u8_bytes, prefix.data,
                                     prefix.u8_bytes);
}

bool n00b_unicode_str_ends_with_raw(const char *data, int64_t len,
                               const char *suffix, int64_t suffix_len) {
  if (suffix_len > len)
    return false;
  return memcmp(data + len - suffix_len, suffix, (size_t)suffix_len) == 0;
}

bool n00b_unicode_str_ends_with(n00b_string_t s, n00b_string_t suffix) {
  return n00b_unicode_str_ends_with_raw(s.data, s.u8_bytes, suffix.data, suffix.u8_bytes);
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_str_replace_raw(n00b_allocator_t *allocator, const char *data,
                                    int64_t len, const char *old_s,
                                    int64_t old_len, const char *new_s,
                                    int64_t new_len) {
  int32_t pos = n00b_unicode_str_find_raw(data, len, old_s, old_len);
  if (pos < 0) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  uint32_t result_len = (uint32_t)len - (uint32_t)old_len + (uint32_t)new_len;
  char *buf = n00b_alloc_array(char, result_len + 1);
  memcpy(buf, data, (uint32_t)pos);
  memcpy(buf + pos, new_s, (size_t)new_len);
  memcpy(buf + pos + new_len, data + pos + old_len,
         (size_t)(len - pos - old_len));
  buf[result_len] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, result_len);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, result_len, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_replace(n00b_string_t s, n00b_string_t old_s,
                                n00b_string_t new_s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_str_replace_raw(allocator, s.data, s.u8_bytes, old_s.data,
                                 old_s.u8_bytes, new_s.data, new_s.u8_bytes);
}

n00b_string_t n00b_unicode_str_replace_all_raw(n00b_allocator_t *allocator, const char *data,
                                        int64_t len, const char *old_s,
                                        int64_t old_len, const char *new_s,
                                        int64_t new_len) {
  if (old_len == 0) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  // Count occurrences
  uint32_t count = 0;
  const char *p = data;
  uint32_t remaining = (uint32_t)len;
  while (remaining >= (uint32_t)old_len) {
    const char *found = find_bytes(p, remaining, old_s, (uint32_t)old_len);
    if (!found)
      break;
    count++;
    uint32_t skip = (uint32_t)(found - p) + (uint32_t)old_len;
    p += skip;
    remaining -= skip;
  }

  if (count == 0) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  uint32_t result_len;
  if ((uint32_t)new_len >= (uint32_t)old_len) {
    result_len =
        (uint32_t)len + count * ((uint32_t)new_len - (uint32_t)old_len);
  } else {
    result_len =
        (uint32_t)len - count * ((uint32_t)old_len - (uint32_t)new_len);
  }

  char *buf = n00b_alloc_array(char, result_len + 1);
  uint32_t out_pos = 0;

  p = data;
  remaining = (uint32_t)len;
  while (remaining >= (uint32_t)old_len) {
    const char *found = find_bytes(p, remaining, old_s, (uint32_t)old_len);
    if (!found)
      break;
    uint32_t before = (uint32_t)(found - p);
    memcpy(buf + out_pos, p, before);
    out_pos += before;
    memcpy(buf + out_pos, new_s, (size_t)new_len);
    out_pos += (uint32_t)new_len;
    p = found + old_len;
    remaining = (uint32_t)len - (uint32_t)(p - data);
  }
  // Copy remainder
  memcpy(buf + out_pos, p, remaining);
  out_pos += remaining;
  buf[out_pos] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, out_pos);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, out_pos, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_replace_all(n00b_string_t s, n00b_string_t old_s,
                                    n00b_string_t new_s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_str_replace_all_raw(allocator, s.data, s.u8_bytes, old_s.data,
                                     old_s.u8_bytes, new_s.data, new_s.u8_bytes);
}

// ---------------------------------------------------------------------------
// Split
// ---------------------------------------------------------------------------

n00b_string_t *n00b_unicode_str_split_raw(n00b_allocator_t *allocator, const char *data,
                                   int64_t len, const char *sep,
                                   int64_t sep_len, uint32_t *count) {
  if (sep_len == 0) {
    // Split into individual codepoints
    int64_t num_cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    if (num_cps <= 0) {
      *count = 0;
      return n00b_alloc_array(char, sizeof(n00b_string_t));
    }
    n00b_string_t *arr =
        n00b_alloc_array(char, (size_t)num_cps * sizeof(n00b_string_t));
    uint32_t pos = 0;
    uint32_t idx = 0;
    while (pos < (uint32_t)len) {
      uint32_t start = pos;
      n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
      uint32_t cp_len = pos - start;
      int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + start, cp_len);
      arr[idx++] = n00b_string_from_raw(allocator, data + start, cp_len, cps);
    }
    *count = idx;
    return arr;
  }

  // Count splits
  uint32_t n = 1;
  const char *p = data;
  uint32_t remaining = (uint32_t)len;
  while (remaining >= (uint32_t)sep_len) {
    const char *found = find_bytes(p, remaining, sep, (uint32_t)sep_len);
    if (!found)
      break;
    n++;
    uint32_t skip = (uint32_t)(found - p) + (uint32_t)sep_len;
    p += skip;
    remaining -= skip;
  }

  n00b_string_t *arr = n00b_alloc_array(char, n * sizeof(n00b_string_t));
  uint32_t idx = 0;

  p = data;
  remaining = (uint32_t)len;
  while (remaining >= (uint32_t)sep_len && idx < n - 1) {
    const char *found = find_bytes(p, remaining, sep, (uint32_t)sep_len);
    if (!found)
      break;
    uint32_t seg_len = (uint32_t)(found - p);
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(p, seg_len);
    arr[idx++] = n00b_string_from_raw(allocator, p, seg_len, cps);
    p = found + sep_len;
    remaining = (uint32_t)len - (uint32_t)(p - data);
  }
  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(p, remaining);
  arr[idx++] = n00b_string_from_raw(allocator, p, remaining, cps);
  *count = idx;
  return arr;
}

n00b_string_t *n00b_unicode_str_split(n00b_string_t s, n00b_string_t sep,
                               uint32_t *count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  return n00b_unicode_str_split_raw(allocator, s.data, s.u8_bytes, sep.data, sep.u8_bytes,
                               count);
}

n00b_string_t *n00b_unicode_str_split_words(n00b_string_t s, uint32_t *count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  const char *data = s.data;
  int64_t len = s.u8_bytes;

  // First pass: collect into a temporary malloc'd array
  uint32_t cap = 16;
  uint32_t n = 0;

  // Store byte offset pairs (start, length) in a temp array
  uint32_t *ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));

  n00b_unicode_break_iter_t *it = n00b_unicode_word_iter_raw(data, len);
  uint32_t prev = 0;
  int32_t b;

  while ((b = n00b_unicode_break_next(it)) >= 0) {
    if ((uint32_t)b > prev) {
      if (n >= cap) {
        cap *= 2;
        {
          uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
          memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
          n00b_free(ranges);
          ranges = new_ranges;
        }
      }
      ranges[n * 2] = prev;
      ranges[n * 2 + 1] = (uint32_t)b - prev;
      n++;
    }
    prev = (uint32_t)b;
  }
  // Last segment
  if (prev < (uint32_t)len) {
    if (n >= cap) {
      cap++;
      {
        uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
        memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
        n00b_free(ranges);
        ranges = new_ranges;
      }
    }
    ranges[n * 2] = prev;
    ranges[n * 2 + 1] = (uint32_t)len - prev;
    n++;
  }

  n00b_unicode_break_iter_free(it);

  // Now allocate the allocator array and populate
  n00b_string_t *arr = n00b_alloc_array(char, n * sizeof(n00b_string_t));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t start = ranges[i * 2];
    uint32_t seg_len = ranges[i * 2 + 1];
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + start, seg_len);
    arr[i] = n00b_string_from_raw(allocator, data + start, seg_len, cps);
  }

  n00b_free(ranges);
  *count = n;
  return arr;
}

n00b_string_t *n00b_unicode_str_split_graphemes(n00b_string_t s,
                                         uint32_t *count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  const char *data = s.data;
  int64_t len = s.u8_bytes;

  uint32_t cap = 16;
  uint32_t n = 0;
  uint32_t *ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));

  n00b_unicode_break_iter_t *it = n00b_unicode_grapheme_iter_raw(data, len);
  uint32_t prev = 0;
  int32_t b;

  while ((b = n00b_unicode_break_next(it)) >= 0) {
    if ((uint32_t)b > prev) {
      if (n >= cap) {
        cap *= 2;
        {
          uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
          memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
          n00b_free(ranges);
          ranges = new_ranges;
        }
      }
      ranges[n * 2] = prev;
      ranges[n * 2 + 1] = (uint32_t)b - prev;
      n++;
    }
    prev = (uint32_t)b;
  }
  if (prev < (uint32_t)len) {
    if (n >= cap) {
      cap++;
      {
        uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
        memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
        n00b_free(ranges);
        ranges = new_ranges;
      }
    }
    ranges[n * 2] = prev;
    ranges[n * 2 + 1] = (uint32_t)len - prev;
    n++;
  }

  n00b_unicode_break_iter_free(it);

  n00b_string_t *arr = n00b_alloc_array(char, n * sizeof(n00b_string_t));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t start = ranges[i * 2];
    uint32_t seg_len = ranges[i * 2 + 1];
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + start, seg_len);
    arr[i] = n00b_string_from_raw(allocator, data + start, seg_len, cps);
  }

  n00b_free(ranges);
  *count = n;
  return arr;
}

n00b_string_t *n00b_unicode_str_split_lines(n00b_string_t s, uint32_t *count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  const char *data = s.data;
  int64_t len = s.u8_bytes;

  uint32_t cap = 16;
  uint32_t n = 0;
  uint32_t *ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));

  uint32_t start = 0;
  for (uint32_t i = 0; i < (uint32_t)len; i++) {
    if (data[i] == '\n' || data[i] == '\r') {
      if (n >= cap) {
        cap *= 2;
        {
          uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
          memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
          n00b_free(ranges);
          ranges = new_ranges;
        }
      }
      ranges[n * 2] = start;
      ranges[n * 2 + 1] = i - start;
      n++;
      // Skip CRLF as one terminator
      if (data[i] == '\r' && i + 1 < (uint32_t)len && data[i + 1] == '\n') {
        i++;
      }
      start = i + 1;
    }
  }
  // Last line
  if (n >= cap) {
    cap++;
    {
      uint32_t *new_ranges = n00b_alloc_array(char, cap * 2 * sizeof(uint32_t));
      memcpy(new_ranges, ranges, n * 2 * sizeof(uint32_t));
      n00b_free(ranges);
      ranges = new_ranges;
    }
  }
  ranges[n * 2] = start;
  ranges[n * 2 + 1] = (uint32_t)len - start;
  n++;

  n00b_string_t *arr = n00b_alloc_array(char, n * sizeof(n00b_string_t));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t seg_start = ranges[i * 2];
    uint32_t seg_len = ranges[i * 2 + 1];
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + seg_start, seg_len);
    arr[i] = n00b_string_from_raw(allocator, data + seg_start, seg_len, cps);
  }

  n00b_free(ranges);
  *count = n;
  return arr;
}

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------

static bool is_whitespace_cp(n00b_codepoint_t cp) {
  return n00b_unicode_has_property(cp, N00B_UNICODE_PROP_WHITE_SPACE);
}

n00b_string_t n00b_unicode_str_trim_left(n00b_string_t s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  uint32_t pos = 0;
  while (pos < (uint32_t)len) {
    uint32_t save = pos;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (cp < 0 || !is_whitespace_cp((n00b_codepoint_t)cp)) {
      pos = save;
      break;
    }
  }
  uint32_t result_len = (uint32_t)len - pos;
  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + pos, result_len);
  return n00b_string_from_raw(allocator, data + pos, result_len, cps);
}

n00b_string_t n00b_unicode_str_trim_right(n00b_string_t s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  // Scan from end: find last non-whitespace byte boundary
  uint32_t end = (uint32_t)len;

  while (end > 0) {
    // Find start of last codepoint
    uint32_t prev = end - 1;
    while (prev > 0 && ((uint8_t)data[prev] & 0xC0) == 0x80) {
      prev--;
    }
    uint32_t tmp = prev;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &tmp);
    if (cp < 0 || !is_whitespace_cp((n00b_codepoint_t)cp))
      break;
    end = prev;
  }
  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, end);
  return n00b_string_from_raw(allocator, data, end, cps);
}

n00b_string_t n00b_unicode_str_trim(n00b_string_t s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  // Trim left
  uint32_t lpos = 0;
  while (lpos < (uint32_t)len) {
    uint32_t save = lpos;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &lpos);
    if (cp < 0 || !is_whitespace_cp((n00b_codepoint_t)cp)) {
      lpos = save;
      break;
    }
  }

  // Trim right
  uint32_t end = (uint32_t)len;
  while (end > lpos) {
    uint32_t prev = end - 1;
    while (prev > lpos && ((uint8_t)data[prev] & 0xC0) == 0x80) {
      prev--;
    }
    uint32_t tmp = prev;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &tmp);
    if (cp < 0 || !is_whitespace_cp((n00b_codepoint_t)cp))
      break;
    end = prev;
  }

  uint32_t result_len = end - lpos;
  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data + lpos, result_len);
  return n00b_string_from_raw(allocator, data + lpos, result_len, cps);
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

int n00b_unicode_str_cmp_raw(const char *a, int64_t a_len, const char *b,
                        int64_t b_len) {
  int64_t min_len = a_len < b_len ? a_len : b_len;
  int c = memcmp(a, b, (size_t)min_len);
  if (c != 0)
    return c;
  if (a_len < b_len)
    return -1;
  if (a_len > b_len)
    return 1;
  return 0;
}

int n00b_unicode_str_cmp(n00b_string_t a, n00b_string_t b) {
  return n00b_unicode_str_cmp_raw(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

bool n00b_unicode_str_eq_raw(const char *a, int64_t a_len, const char *b,
                        int64_t b_len) {
  return a_len == b_len && memcmp(a, b, (size_t)a_len) == 0;
}

bool n00b_unicode_str_eq(n00b_string_t a, n00b_string_t b) {
  return n00b_unicode_str_eq_raw(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

bool n00b_unicode_str_eq_nfc_raw(const char *a, int64_t a_len, const char *b,
                            int64_t b_len) {
  n00b_string_t na = n00b_unicode_nfc_raw(NULL, a, a_len);
  n00b_string_t nb = n00b_unicode_nfc_raw(NULL, b, b_len);
  bool eq = n00b_unicode_str_eq_raw(na.data, na.u8_bytes, nb.data, nb.u8_bytes);
  n00b_free(na.data);
  n00b_free(nb.data);
  return eq;
}

bool n00b_unicode_str_eq_nfc(n00b_string_t a, n00b_string_t b) {
  return n00b_unicode_str_eq_nfc_raw(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

bool n00b_unicode_str_eq_casefold_raw(const char *a, int64_t a_len, const char *b,
                                 int64_t b_len) {
  n00b_string_t fa = n00b_unicode_casefold_raw(NULL, a, a_len);
  n00b_string_t fb = n00b_unicode_casefold_raw(NULL, b, b_len);
  bool eq = n00b_unicode_str_eq_raw(fa.data, fa.u8_bytes, fb.data, fb.u8_bytes);
  n00b_free(fa.data);
  n00b_free(fb.data);
  return eq;
}

bool n00b_unicode_str_eq_casefold(n00b_string_t a, n00b_string_t b) {
  return n00b_unicode_str_eq_casefold_raw(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

// ---------------------------------------------------------------------------
// Width-aware padding
// ---------------------------------------------------------------------------

static void encode_fill(n00b_codepoint_t fill_cp, int32_t n, char **out_buf,
                        uint32_t *out_bytes, uint32_t *out_cps) {
  if (n <= 0) {
    *out_buf = n00b_alloc_array(char, 1);
    (*out_buf)[0] = '\0';
    *out_bytes = 0;
    *out_cps = 0;
    return;
  }

  char enc[4];
  uint32_t enc_len = n00b_unicode_utf8_encode(fill_cp, enc);
  uint32_t total = enc_len * (uint32_t)n;
  char *buf = n00b_alloc_array(char, total + 1);

  for (int32_t i = 0; i < n; i++) {
    memcpy(buf + i * enc_len, enc, enc_len);
  }
  buf[total] = '\0';

  *out_buf = buf;
  *out_bytes = total;
  *out_cps = (uint32_t)n;
}

n00b_string_t n00b_unicode_str_pad_right(n00b_string_t s, int32_t width) _kargs {
  n00b_allocator_t *allocator = NULL;
  n00b_codepoint_t fill = ' ';
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  int32_t cur = n00b_unicode_display_width_raw(data, len);
  if (cur >= width) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  int32_t fill_w = n00b_unicode_char_width(fill);
  if (fill_w <= 0)
    fill_w = 1;
  int32_t need = (width - cur) / fill_w;

  char *pad_buf;
  uint32_t pad_bytes, pad_cps;
  encode_fill(fill, need, &pad_buf, &pad_bytes, &pad_cps);

  uint32_t total = (uint32_t)len + pad_bytes;
  char *buf = n00b_alloc_array(char, total + 1);
  memcpy(buf, data, (size_t)len);
  memcpy(buf + len, pad_buf, pad_bytes);
  buf[total] = '\0';
  n00b_free(pad_buf);

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, total);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_pad_left(n00b_string_t s, int32_t width) _kargs {
  n00b_allocator_t *allocator = NULL;
  n00b_codepoint_t fill = ' ';
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  int32_t cur = n00b_unicode_display_width_raw(data, len);
  if (cur >= width) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  int32_t fill_w = n00b_unicode_char_width(fill);
  if (fill_w <= 0)
    fill_w = 1;
  int32_t need = (width - cur) / fill_w;

  char *pad_buf;
  uint32_t pad_bytes, pad_cps;
  encode_fill(fill, need, &pad_buf, &pad_bytes, &pad_cps);

  uint32_t total = pad_bytes + (uint32_t)len;
  char *buf = n00b_alloc_array(char, total + 1);
  memcpy(buf, pad_buf, pad_bytes);
  memcpy(buf + pad_bytes, data, (size_t)len);
  buf[total] = '\0';
  n00b_free(pad_buf);

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, total);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total, cps);
  n00b_free(buf);
  return result;
}

n00b_string_t n00b_unicode_str_center(n00b_string_t s, int32_t width) _kargs {
  n00b_allocator_t *allocator = NULL;
  n00b_codepoint_t fill = ' ';
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  int32_t cur = n00b_unicode_display_width_raw(data, len);
  if (cur >= width) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  int32_t fill_w = n00b_unicode_char_width(fill);
  if (fill_w <= 0)
    fill_w = 1;
  int32_t total_fill = (width - cur) / fill_w;
  int32_t left = total_fill / 2;
  int32_t right = total_fill - left;

  char *lpad_buf, *rpad_buf;
  uint32_t lpad_bytes, lpad_cps, rpad_bytes, rpad_cps;
  encode_fill(fill, left, &lpad_buf, &lpad_bytes, &lpad_cps);
  encode_fill(fill, right, &rpad_buf, &rpad_bytes, &rpad_cps);

  const char *parts[3] = {lpad_buf, data, rpad_buf};
  int64_t lengths[3] = {lpad_bytes, len, rpad_bytes};
  n00b_string_t result = cat_many_raw(allocator, parts, lengths, 3);

  n00b_free(lpad_buf);
  n00b_free(rpad_buf);
  return result;
}

n00b_string_t n00b_unicode_str_truncate(n00b_string_t s, int32_t max_width) _kargs {
  n00b_allocator_t *allocator = NULL;
  const char *ellipsis = "...";
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;
  int64_t ellipsis_len = (int64_t)strlen(ellipsis);

  int32_t cur = n00b_unicode_display_width_raw(data, len);
  if (cur <= max_width) {
    int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len);
    return n00b_string_from_raw(allocator, data, len, cps);
  }

  int32_t ellip_w = n00b_unicode_display_width_raw(ellipsis, ellipsis_len);
  int32_t target = max_width - ellip_w;
  if (target < 0)
    target = 0;

  // Walk codepoints, accumulate width until we exceed target
  uint32_t pos = 0;
  int32_t w = 0;
  uint32_t last_pos = 0;

  while (pos < (uint32_t)len) {
    last_pos = pos;
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (cp < 0)
      break;
    w += n00b_unicode_char_width((n00b_codepoint_t)cp);
    if (w > target) {
      pos = last_pos;
      break;
    }
  }

  return n00b_unicode_str_cat_raw(allocator, data, (int64_t)pos, ellipsis, ellipsis_len);
}

// ---------------------------------------------------------------------------
// Repeat
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_str_repeat(n00b_string_t s, uint32_t count) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  if (count == 0 || len == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  uint32_t total = (uint32_t)len * count;
  char *buf = n00b_alloc_array(char, total + 1);

  for (uint32_t i = 0; i < count; i++) {
    memcpy(buf + i * (uint32_t)len, data, (size_t)len);
  }
  buf[total] = '\0';

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(data, (uint32_t)len) * count;
  n00b_string_t result = n00b_string_from_raw(allocator, buf, total, cps);
  n00b_free(buf);
  return result;
}

// ---------------------------------------------------------------------------
// Reverse (grapheme-aware)
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_str_reverse(n00b_string_t s) _kargs {
  n00b_allocator_t *allocator = NULL;
}
{
  if (!allocator)
    allocator = nullptr;

  const char *data = s.data;
  int64_t len = s.u8_bytes;

  if (len == 0) {
    return n00b_string_from_raw(allocator, "", 0, 0);
  }

  // Get grapheme cluster boundaries
  uint32_t *offsets;
  uint32_t n_graphemes = get_grapheme_offsets(data, len, &offsets);

  char *buf = n00b_alloc_array(char, (size_t)len + 1);
  uint32_t out_pos = 0;

  // Write grapheme clusters in reverse order
  for (uint32_t i = n_graphemes; i > 0; i--) {
    uint32_t start = offsets[i - 1];
    uint32_t end = offsets[i];
    memcpy(buf + out_pos, data + start, end - start);
    out_pos += end - start;
  }
  buf[out_pos] = '\0';

  n00b_free(offsets);

  int64_t cps = n00b_unicode_utf8_count_codepoints_raw(buf, out_pos);
  n00b_string_t result = n00b_string_from_raw(allocator, buf, out_pos, cps);
  n00b_free(buf);
  return result;
}
