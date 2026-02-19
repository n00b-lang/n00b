#include "unicode/security.h"
#include "unicode/encoding.h"
#include "unicode/identifiers.h"
#include "unicode/normalization.h"
#include "unicode/properties.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include "internal/unicode/tables.h"
#include <string.h>

extern const uint32_t n00b_unicode_confusable_index[][2];
extern const uint32_t n00b_unicode_confusable_index_len;
extern const uint32_t n00b_unicode_confusable_data[];

// From gen_script_extensions.c
#define N00B_UNICODE_SCRIPT_COMMON 0
#define N00B_UNICODE_SCRIPT_INHERITED 57

// Script indices for UTS #39 highly restrictive combos
#define SCRIPT_LATIN 73
#define SCRIPT_HAN 49
#define SCRIPT_HIRAGANA 55
#define SCRIPT_KATAKANA 63
#define SCRIPT_HANGUL 50
#define SCRIPT_BOPOMOFO 13

// "Recommended" scripts per UTS #39 §5.2
static const n00b_unicode_script_t recommended_scripts[] = {
    0,   // Common
    57,  // Inherited
    4,   // Arabic
    5,   // Armenian
    11,  // Bengali
    13,  // Bopomofo
    29,  // Cyrillic
    31,  // Devanagari
    38,  // Ethiopic
    40,  // Georgian
    44,  // Greek
    45,  // Gujarati
    47,  // Gurmukhi
    49,  // Han
    50,  // Hangul
    54,  // Hebrew
    55,  // Hiragana
    62,  // Kannada
    63,  // Katakana
    68,  // Khmer
    72,  // Lao
    73,  // Latin
    83,  // Malayalam
    98,  // Myanmar
    118, // Oriya
    136, // Sinhala
    148, // Tamil
    150, // Telugu
    152, // Thaana
    153, // Thai
    154, // Tibetan
};
#define NUM_RECOMMENDED                                                        \
  (sizeof(recommended_scripts) / sizeof(recommended_scripts[0]))

// UTS #39 skeleton algorithm: NFD -> confusable map -> NFD
n00b_string_t n00b_unicode_skeleton_raw(n00b_allocator_t *allocator, const char *data,
                                int64_t len) {
  // Step 1: NFD
  n00b_string_t nfd = n00b_unicode_nfd_raw(allocator, data, len);

  // Step 2: Apply confusable mappings
  char *buf = n00b_alloc_array(char, nfd.u8_bytes * 12 + 1);
  uint32_t buf_pos = 0;
  uint32_t cp_count = 0;
  uint32_t pos = 0;

  while (pos < (uint32_t)nfd.u8_bytes) {
    int32_t cp = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    if (cp < 0)
      break;

    const uint32_t *entry = n00b_unicode_sparse_lookup(
        n00b_unicode_confusable_index, n00b_unicode_confusable_index_len,
        n00b_unicode_confusable_data, (n00b_codepoint_t)cp);

    if (entry) {
      uint32_t count = entry[0];
      for (uint32_t i = 0; i < count; i++) {
        buf_pos += n00b_unicode_utf8_encode(entry[1 + i], buf + buf_pos);
        cp_count++;
      }
    } else {
      buf_pos += n00b_unicode_utf8_encode((n00b_codepoint_t)cp, buf + buf_pos);
      cp_count++;
    }
  }

  buf[buf_pos] = '\0';

  // Step 3: NFD again
  n00b_string_t result = n00b_unicode_nfd_raw(allocator, buf, buf_pos);

  return result;
}

n00b_string_t n00b_unicode_skeleton(n00b_string_t s) _kargs { n00b_allocator_t *allocator = NULL; }
{
    if (!allocator) allocator = nullptr;
    return n00b_unicode_skeleton_raw(allocator, s.data, s.u8_bytes);
}

bool n00b_unicode_is_confusable_raw(const char *a, int64_t a_len, const char *b,
                               int64_t b_len) {
  n00b_string_t sa = n00b_unicode_skeleton_raw(nullptr, a, a_len);
  n00b_string_t sb = n00b_unicode_skeleton_raw(nullptr, b, b_len);

  bool result =
      (sa.u8_bytes == sb.u8_bytes && memcmp(sa.data, sb.data, sa.u8_bytes) == 0);

  n00b_free(sa.data);
  n00b_free(sb.data);
  return result;
}

bool n00b_unicode_is_confusable(n00b_string_t a, n00b_string_t b) {
  return n00b_unicode_is_confusable_raw(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

static bool is_recommended(n00b_unicode_script_t sc) {
  for (uint32_t i = 0; i < NUM_RECOMMENDED; i++) {
    if (recommended_scripts[i] == sc)
      return true;
  }
  return false;
}

// Check if a set of scripts is a subset of an allowed combination.
// `scripts` is a bitset (bool array indexed by script id, max 170).
static bool is_highly_restrictive(const bool *scripts) {
  // Allowed combos (all implicitly include Common+Inherited):
  // Japanese: {Latin, Han, Hiragana, Katakana}
  // Korean:   {Latin, Han, Hangul}
  // Chinese:  {Latin, Han, Bopomofo}
  static const n00b_unicode_script_t japanese[] = {SCRIPT_LATIN, SCRIPT_HAN,
                                              SCRIPT_HIRAGANA, SCRIPT_KATAKANA};
  static const n00b_unicode_script_t korean[] = {SCRIPT_LATIN, SCRIPT_HAN,
                                            SCRIPT_HANGUL};
  static const n00b_unicode_script_t chinese[] = {SCRIPT_LATIN, SCRIPT_HAN,
                                             SCRIPT_BOPOMOFO};

  struct {
    const n00b_unicode_script_t *set;
    int count;
  } combos[] = {
      {japanese, 4},
      {korean, 3},
      {chinese, 3},
  };

  for (int c = 0; c < 3; c++) {
    bool ok = true;
    for (int s = 0; s < 170 && ok; s++) {
      if (!scripts[s])
        continue;
      if ((n00b_unicode_script_t)s == N00B_UNICODE_SCRIPT_COMMON ||
          (n00b_unicode_script_t)s == N00B_UNICODE_SCRIPT_INHERITED)
        continue;
      bool found = false;
      for (int j = 0; j < combos[c].count; j++) {
        if ((n00b_unicode_script_t)s == combos[c].set[j]) {
          found = true;
          break;
        }
      }
      if (!found)
        ok = false;
    }
    if (ok)
      return true;
  }
  return false;
}

// Compute "augmented script set" for a codepoint per UTS #39.
// Returns count of non-Common/Inherited scripts, or 0 if only Common/Inherited.
// When only Common/Inherited, the character is compatible with all scripts.
static int augmented_script_set(n00b_codepoint_t cp, n00b_unicode_script_t *out,
                                int max) {
  n00b_unicode_script_t ext[32];
  int ext_count = n00b_unicode_script_extensions(cp, ext, 32);

  int n = 0;
  for (int i = 0; i < ext_count && n < max; i++) {
    if (ext[i] != N00B_UNICODE_SCRIPT_COMMON && ext[i] != N00B_UNICODE_SCRIPT_INHERITED) {
      out[n++] = ext[i];
    }
  }
  return n; // 0 means "compatible with everything"
}

n00b_unicode_restriction_level_t n00b_unicode_script_restriction_raw(const char *data,
                                                           int64_t len) {
  if (len == 0)
    return N00B_UNICODE_RESTRICTION_ASCII_ONLY;

  // UTS #39: maintain running intersection of augmented script sets.
  // Start with "all scripts" (represented as all_scripts=true).
  // For each codepoint with a non-empty augmented set, intersect.
  bool resolved[170] = {0};
  bool all_scripts = true; // True = haven't seen any restricting char yet
  bool ascii_only = true;
  bool all_id_allowed = true;
  bool has_any_char = false;

  // Also track the union of all non-Common/Inherited scripts (for
  // highly/moderately checks)
  bool script_union[170] = {0};

  uint32_t pos = 0;
  while (pos < (uint32_t)len) {
    int32_t cp = n00b_unicode_utf8_decode(data, len, &pos);
    if (cp < 0)
      break;
    has_any_char = true;

    if ((n00b_codepoint_t)cp > 0x7F)
      ascii_only = false;
    if (all_id_allowed &&
        !n00b_unicode_is_identifier_allowed((n00b_codepoint_t)cp))
      all_id_allowed = false;

    n00b_unicode_script_t aug[32];
    int aug_count = augmented_script_set((n00b_codepoint_t)cp, aug, 32);

    // Add to union
    for (int i = 0; i < aug_count; i++) {
      if (aug[i] < 170)
        script_union[aug[i]] = true;
    }

    if (aug_count == 0)
      continue; // Common/Inherited only — compatible with everything

    if (all_scripts) {
      // First restricting character: resolved = its augmented set
      for (int i = 0; i < aug_count; i++) {
        if (aug[i] < 170)
          resolved[aug[i]] = true;
      }
      all_scripts = false;
    } else {
      // Intersect: keep only scripts in both resolved and this augmented set
      bool aug_set[170] = {0};
      for (int i = 0; i < aug_count; i++) {
        if (aug[i] < 170)
          aug_set[aug[i]] = true;
      }
      for (int s = 0; s < 170; s++) {
        if (resolved[s] && !aug_set[s])
          resolved[s] = false;
      }
    }
  }

  if (!has_any_char)
    return N00B_UNICODE_RESTRICTION_ASCII_ONLY;
  if (ascii_only)
    return N00B_UNICODE_RESTRICTION_ASCII_ONLY;

  // Count scripts in the resolved intersection
  int resolved_count = 0;
  if (all_scripts) {
    // Only Common/Inherited characters → no restricting scripts
    resolved_count = 0;
  } else {
    for (int s = 0; s < 170; s++) {
      if (resolved[s])
        resolved_count++;
    }
  }

  // Non-empty intersection → all characters share at least one script
  if (all_scripts || resolved_count > 0)
    return N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT;

  // Multi-script from here on. Use the union to classify.
  // Highly Restrictive: union is subset of an allowed multi-script combo
  if (is_highly_restrictive(script_union))
    return N00B_UNICODE_RESTRICTION_HIGHLY_RESTRICTIVE;

  // Moderately Restrictive: Latin + exactly one other recommended script
  if (script_union[SCRIPT_LATIN]) {
    int other_count = 0;
    bool all_recommended_others = true;
    for (int s = 0; s < 170; s++) {
      if (!script_union[s])
        continue;
      if ((n00b_unicode_script_t)s == N00B_UNICODE_SCRIPT_COMMON ||
          (n00b_unicode_script_t)s == N00B_UNICODE_SCRIPT_INHERITED ||
          (n00b_unicode_script_t)s == SCRIPT_LATIN)
        continue;
      other_count++;
      if (!is_recommended((n00b_unicode_script_t)s))
        all_recommended_others = false;
    }
    if (other_count == 1 && all_recommended_others)
      return N00B_UNICODE_RESTRICTION_MODERATELY_RESTRICTIVE;
  }

  // Minimally Restrictive: all codepoints have Identifier_Status=Allowed
  if (all_id_allowed)
    return N00B_UNICODE_RESTRICTION_MINIMALLY_RESTRICTIVE;

  return N00B_UNICODE_RESTRICTION_UNRESTRICTED;
}

n00b_unicode_restriction_level_t n00b_unicode_script_restriction(n00b_string_t s) {
  return n00b_unicode_script_restriction_raw(s.data, s.u8_bytes);
}

bool n00b_unicode_has_mixed_scripts_raw(const char *data, int64_t len) {
  return n00b_unicode_script_restriction_raw(data, len) >
         N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT;
}

bool n00b_unicode_has_mixed_scripts(n00b_string_t s) {
  return n00b_unicode_has_mixed_scripts_raw(s.data, s.u8_bytes);
}
