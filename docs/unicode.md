# n00b Unicode Library

## Overview

The n00b Unicode library is a comprehensive, type-safe Unicode implementation for C23.
It covers the full scope of the Unicode Standard and its annexes: character properties,
normalization, case mapping, collation, text segmentation, bidirectional layout, line
breaking, emoji detection, identifier validation, IDNA/Punycode, and security analysis.

### Design principles

- **Value-type strings.** `n00b_string_t` is a 40-byte struct passed by value &mdash; no
  heap indirection for the descriptor itself, no reference counting.
- **Keyword arguments.** Most functions that allocate accept an optional `.allocator`
  parameter (and sometimes `.locale`, `.fill`, `.width`, etc.) via ncc's `_kargs`
  extension.
- **Option/Result types.** Fallible lookups return `n00b_option_t(T)` rather than
  sentinel values.
- **Two-tier API.** Many public functions have an internal `_raw` counterpart that
  operates on `(const char *data, int64_t len)` pairs. The public API wraps these
  with `n00b_string_t`.

---

## String primer: `n00b_string_t`

Defined in `include/core/string.h`:

```c
typedef struct {
    int64_t  u8_bytes;   // byte length of UTF-8 data
    char    *data;       // UTF-8 bytes (not guaranteed NUL-terminated)
    int64_t  codepoints; // codepoint count (0 if not yet computed)
    void    *styling;    // reserved for rich-text metadata
} n00b_string_t;
```

Construction:

```c
// From a raw C string (copies src, appends NUL internally):
n00b_string_t s = n00b_string_from_raw(allocator, "Hello", 5, 0);

// Empty string:
n00b_string_t e = n00b_string_empty(allocator);
```

Because `n00b_string_t` is a value type, you pass it directly &mdash; no pointer
required for read-only access. Functions that produce new strings return them by
value as well.

---

## Modules

### 1. Core types &mdash; `unicode/types.h`

Pure type definitions, no functions. Enumerations for every Unicode property used
by the library:

| Type | Property |
|------|----------|
| `n00b_codepoint_t` | `uint32_t` codepoint (U+0000 .. U+10FFFF) |
| `n00b_unicode_gc_t` | General_Category (30 values) |
| `n00b_unicode_bidi_class_t` | Bidi_Class (23 values, UAX #9) |
| `n00b_unicode_eaw_t` | East_Asian_Width (6 values) |
| `n00b_unicode_script_t` | Script index (UAX #24) |
| `n00b_unicode_block_t` | Block index |
| `n00b_unicode_lb_t` | Line_Break (50 values, UAX #14) |
| `n00b_unicode_gcb_t` | Grapheme_Cluster_Break (17 values, UAX #29) |
| `n00b_unicode_wb_t` | Word_Break (19 values, UAX #29) |
| `n00b_unicode_sb_t` | Sentence_Break (15 values, UAX #29) |
| `n00b_unicode_jt_t` | Joining_Type (6 values) |
| `n00b_unicode_property_t` | Binary properties (47 bit positions in a `uint64_t`) |
| `n00b_unicode_numeric_type_t` | NONE / DECIMAL / DIGIT / NUMERIC |
| `n00b_unicode_numeric_value_t` | Rational value `{ type, numerator, denominator }` |
| `n00b_unicode_norm_form_t` | NFC / NFD / NFKC / NFKD |
| `n00b_unicode_bom_t` | BOM detection result |
| `n00b_unicode_lb_action_t` | NONE / ALLOWED / MANDATORY break actions |
| `n00b_unicode_emoji_type_t` | Emoji sequence classification (8 values) |
| `n00b_unicode_idna_error_t` | IDNA error codes (12 values, UTS #46) |
| `n00b_unicode_restriction_level_t` | Script restriction levels (UTS #39) |
| `n00b_unicode_break_type_t` | GRAPHEME / WORD / SENTENCE iterator selector |

### 2. Extended types &mdash; `unicode/types_ext.h`

Builds on `types.h` with ncc-specific option types and composite result structs:

```c
typedef n00b_option_t(int32_t) n00b_unicode_opt_i32_t;

typedef struct {
    n00b_string_t             value;  // converted domain (empty on error)
    n00b_unicode_idna_error_t error;  // N00B_UNICODE_IDNA_OK on success
} n00b_unicode_idna_result_t;

typedef struct {
    n00b_unicode_emoji_type_t type;       // kind of emoji sequence
    uint32_t                  seq_bytes;  // bytes consumed
} n00b_unicode_emoji_scan_result_t;
```

### 3. Character properties &mdash; `unicode/properties.h`

Per-codepoint property lookups backed by two-stage tables and binary search.

```c
n00b_unicode_gc_t         n00b_unicode_general_category(n00b_codepoint_t cp);
uint8_t                   n00b_unicode_combining_class(n00b_codepoint_t cp);
n00b_unicode_script_t     n00b_unicode_script(n00b_codepoint_t cp);
const char               *n00b_unicode_script_name(n00b_unicode_script_t s);
n00b_unicode_block_t      n00b_unicode_block(n00b_codepoint_t cp);
const char               *n00b_unicode_block_name(n00b_unicode_block_t b);
n00b_unicode_bidi_class_t n00b_unicode_bidi_class(n00b_codepoint_t cp);
n00b_unicode_eaw_t        n00b_unicode_east_asian_width(n00b_codepoint_t cp);
int                       n00b_unicode_char_width(n00b_codepoint_t cp);
n00b_unicode_jt_t         n00b_unicode_joining_type(n00b_codepoint_t cp);
bool                      n00b_unicode_has_property(n00b_codepoint_t cp,
                                                    n00b_unicode_property_t prop);
int                       n00b_unicode_script_extensions(n00b_codepoint_t cp,
                                                         n00b_unicode_script_t *out,
                                                         int max);
```

Numeric properties:

```c
n00b_unicode_numeric_type_t  n00b_unicode_numeric_type(n00b_codepoint_t cp);
n00b_unicode_numeric_value_t n00b_unicode_numeric_value(n00b_codepoint_t cp);
n00b_unicode_opt_i32_t       n00b_unicode_digit_value(n00b_codepoint_t cp);
```

String-level aggregate:

```c
int32_t n00b_unicode_display_width(n00b_string_t s);
```

**Example:**

```c
n00b_codepoint_t cp = 0x00E9;  // LATIN SMALL LETTER E WITH ACUTE

n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
// gc == N00B_UNICODE_GC_LL  (Lowercase_Letter)

const char *script = n00b_unicode_script_name(n00b_unicode_script(cp));
// script == "Latin"

int w = n00b_unicode_char_width(cp);
// w == 1  (single column)

bool alpha = n00b_unicode_has_property(cp, N00B_UNICODE_PROP_ALPHABETIC);
// alpha == true
```

### 4. Encoding &mdash; `unicode/encoding.h`

UTF-8 validation, decoding, encoding, BOM detection, and transcoding to/from
UTF-16 and UTF-32.

```c
// Low-level UTF-8:
int32_t  n00b_unicode_utf8_decode(const char *src, uint32_t len, uint32_t *pos);
uint32_t n00b_unicode_utf8_encode(n00b_codepoint_t cp, char *dst);
bool     n00b_unicode_utf8_validate(const char *src, uint32_t len);

// BOM detection:
n00b_unicode_bom_t n00b_unicode_detect_bom(const char *data, uint32_t len,
                                            uint32_t *bom_len);

// String-level:
int64_t n00b_unicode_utf8_count_codepoints(n00b_string_t s);
bool    n00b_unicode_str_validate(n00b_string_t s);

// Transcoding (all accept .allocator):
uint16_t         *n00b_unicode_to_utf16(n00b_string_t s, uint32_t *out_len, ...);
n00b_codepoint_t *n00b_unicode_to_utf32(n00b_string_t s, uint32_t *out_len, ...);
n00b_string_t     n00b_unicode_from_utf16(const uint16_t *src, uint32_t len, ...);
n00b_string_t     n00b_unicode_from_utf32(const n00b_codepoint_t *src, uint32_t len, ...);
```

**Example:**

```c
const char *raw = "caf\xC3\xA9";
bool valid = n00b_unicode_utf8_validate(raw, 5);  // true

// Transcode to UTF-32:
uint32_t u32_len;
n00b_codepoint_t *u32 = n00b_unicode_to_utf32(s, &u32_len);
// u32[3] == 0x00E9, u32_len == 4
```

### 5. Normalization &mdash; `unicode/normalization.h`

Few people really understand normalization or how to use it. So here's
a reminder:

- NFC — The default choice for display, file names, and
         interchange. It's not lossy.
  
- NFKC — Lossy; it's meant for searching text. It works by turning
         characters that are "the same" from a meaning perspective,
         but visually different, into the same thing. So you don't
         want to print it. But unicode identifiers for languages, and
         things like that should use it.
          
- NFD — Not lossy, but decomposed; the accent gets separated from the
         letter, and composed emojis split apart. Generally though,
         this is an expert move only-- if you want to strip accents
         for search matching, you probably want to use NFKD. It's
         really just a power move.

- NFKD — It's a good choice for search matching, because it lets you
         strip out combining characters easily AND it collapses
         variants of a character, so you get better matching.


NFC, NFD, NFKC, NFKD normalization with quick-check predicates and a streaming
normalizer for incremental processing.

```c
// One-shot (all accept .allocator):
n00b_string_t n00b_unicode_nfc(n00b_string_t s, ...);
n00b_string_t n00b_unicode_nfd(n00b_string_t s, ...);
n00b_string_t n00b_unicode_nfkc(n00b_string_t s, ...);
n00b_string_t n00b_unicode_nfkd(n00b_string_t s, ...);

// Quick-check:
bool n00b_unicode_is_nfc(n00b_string_t s);
bool n00b_unicode_is_nfd(n00b_string_t s);

// Streaming normalizer:
n00b_unicode_normalizer_t *n00b_unicode_normalizer_new(n00b_unicode_norm_form_t form, ...);
void   n00b_unicode_normalizer_feed(n00b_unicode_normalizer_t *n, n00b_codepoint_t cp);
size_t n00b_unicode_normalizer_read(n00b_unicode_normalizer_t *n,
                                    n00b_codepoint_t *out, size_t max);
size_t n00b_unicode_normalizer_flush(n00b_unicode_normalizer_t *n,
                                     n00b_codepoint_t *out, size_t max);
void   n00b_unicode_normalizer_free(n00b_unicode_normalizer_t *n);
```

**Example:**

```c
n00b_string_t composed = n00b_unicode_nfc(s);

if (!n00b_unicode_is_nfc(s)) {
    s = n00b_unicode_nfc(s);
}
```

### 6. Case mapping &mdash; `unicode/casemap.h`

Simple per-codepoint mappings and full string-level case operations with optional
locale support.

```c
// Per-codepoint:
n00b_codepoint_t n00b_unicode_toupper_cp(n00b_codepoint_t cp);
n00b_codepoint_t n00b_unicode_tolower_cp(n00b_codepoint_t cp);
n00b_codepoint_t n00b_unicode_totitle_cp(n00b_codepoint_t cp);
n00b_codepoint_t n00b_unicode_casefold_cp(n00b_codepoint_t cp);

// String-level (accept .allocator and .locale):
n00b_string_t n00b_unicode_toupper(n00b_string_t s, ...);
n00b_string_t n00b_unicode_tolower(n00b_string_t s, ...);
n00b_string_t n00b_unicode_totitle(n00b_string_t s, ...);
n00b_string_t n00b_unicode_casefold(n00b_string_t s, ...);

// Case-insensitive comparison:
int n00b_unicode_casecmp(n00b_string_t a, n00b_string_t b);
```

**Example:**

```c
n00b_string_t upper = n00b_unicode_toupper(s);

// Turkish locale: dotted/dotless I distinction:
n00b_string_t tr = n00b_unicode_tolower(s, .locale = "tr");

// Case-insensitive equality:
if (n00b_unicode_casecmp(a, b) == 0) { /* match */ }
```

### 7. Collation &mdash; `unicode/collation.h`

Unicode Collation Algorithm (UCA) for locale-independent sort ordering.

```c
int  n00b_unicode_collate(n00b_string_t a, n00b_string_t b);

n00b_unicode_sort_key_t n00b_unicode_sort_key(n00b_string_t s, ...);
void n00b_unicode_sort_key_free(n00b_unicode_sort_key_t *key);
```

The `n00b_unicode_sort_key_t` struct holds a binary sort key (`uint8_t *data`,
`uint32_t len`) that can be compared with `memcmp` for fast batch sorting.

**Example:**

```c
// Direct comparison:
int order = n00b_unicode_collate(a, b);

// Precompute sort keys for repeated comparisons:
n00b_unicode_sort_key_t ka = n00b_unicode_sort_key(a);
n00b_unicode_sort_key_t kb = n00b_unicode_sort_key(b);
int result = memcmp(ka.data, kb.data, ka.len < kb.len ? ka.len : kb.len);
n00b_unicode_sort_key_free(&ka);
n00b_unicode_sort_key_free(&kb);
```

### 8. Text segmentation &mdash; `unicode/segmentation.h`

Grapheme cluster, word, and sentence boundary detection per UAX #29.

```c
// Iterator constructors (accept .allocator):
n00b_unicode_break_iter_t *n00b_unicode_grapheme_iter(n00b_string_t s, ...);
n00b_unicode_break_iter_t *n00b_unicode_word_iter(n00b_string_t s, ...);
n00b_unicode_break_iter_t *n00b_unicode_sentence_iter(n00b_string_t s, ...);

// Navigation (returns byte offset, or -1 at boundary):
int32_t n00b_unicode_break_next(n00b_unicode_break_iter_t *it);
int32_t n00b_unicode_break_prev(n00b_unicode_break_iter_t *it);
void    n00b_unicode_break_iter_free(n00b_unicode_break_iter_t *it);

// Convenience:
uint32_t n00b_unicode_grapheme_count(n00b_string_t s);
```

**Example:**

```c
uint32_t gc = n00b_unicode_grapheme_count(s);

n00b_unicode_break_iter_t *it = n00b_unicode_word_iter(s);
int32_t pos;
while ((pos = n00b_unicode_break_next(it)) >= 0) {
    // pos is the byte offset of the next word boundary
}
n00b_unicode_break_iter_free(it);
```

### 9. Iteration macros &mdash; `unicode/iter.h`

High-level `foreach` macros built on the segmentation iterators. Each declares
the loop variable in scope and uses `__attribute__((cleanup))` to free iterator
state automatically &mdash; safe to `break` or `return` from.

```c
n00b_unicode_foreach_cp(s, cp)          // cp: int32_t, each codepoint
n00b_unicode_foreach_grapheme(s, g)     // g:  n00b_string_t, each grapheme cluster
n00b_unicode_foreach_word(s, w)         // w:  n00b_string_t, each word
n00b_unicode_foreach_sentence(s, sent)  // sent: n00b_string_t, each sentence
n00b_unicode_foreach_line(s, line)      // line: n00b_string_t, each line
```

All macros take `n00b_string_t *s` (a pointer).

**Example:**

```c
n00b_string_t text = n00b_string_from_raw(alloc, "Hello, world!", 13, 0);

n00b_unicode_foreach_word(&text, w) {
    // w is a n00b_string_t view of each word segment
    printf("word: %.*s\n", (int)w.u8_bytes, w.data);
}
```

### 10. Bidirectional algorithm &mdash; `unicode/bidi.h`

Full implementation of the Unicode Bidirectional Algorithm (UAX #9) for
mixed left-to-right / right-to-left text.

```c
n00b_unicode_bidi_para_t *n00b_unicode_bidi_open(n00b_string_t s, ...);

uint8_t        n00b_unicode_bidi_paragraph_level(const n00b_unicode_bidi_para_t *p);
const uint8_t *n00b_unicode_bidi_levels(const n00b_unicode_bidi_para_t *p,
                                        uint32_t *len);
void           n00b_unicode_bidi_reorder_visual(const n00b_unicode_bidi_para_t *p,
                                                int32_t *visual_map);
void           n00b_unicode_bidi_free(n00b_unicode_bidi_para_t *p);
```

**Example:**

```c
n00b_unicode_bidi_para_t *para = n00b_unicode_bidi_open(s);

uint8_t level = n00b_unicode_bidi_paragraph_level(para);
// 0 = LTR, 1 = RTL

uint32_t len;
const uint8_t *levels = n00b_unicode_bidi_levels(para, &len);
// levels[i] = resolved embedding level for codepoint i

int32_t visual_map[len];
n00b_unicode_bidi_reorder_visual(para, visual_map);
// visual_map[visual_pos] = logical_pos

n00b_unicode_bidi_free(para);
```

### 11. Line breaking &mdash; `unicode/linebreak.h`

Line break opportunity analysis per UAX #14 and width-aware wrapping.

```c
void n00b_unicode_linebreaks(n00b_string_t s, n00b_unicode_lb_action_t *out);

uint32_t *n00b_unicode_linebreak_wrap(n00b_string_t s, uint32_t *num_breaks, ...);
    // keyword args: .width = 80, .allocator = nullptr
```

**Example:**

```c
// Find all break opportunities:
n00b_unicode_lb_action_t breaks[cp_count];
n00b_unicode_linebreaks(s, breaks);
// breaks[i] == N00B_UNICODE_LB_ALLOWED  -> may break before codepoint i
// breaks[i] == N00B_UNICODE_LB_MANDATORY -> must break

// Wrap to 72 columns:
uint32_t num;
uint32_t *offsets = n00b_unicode_linebreak_wrap(s, &num, .width = 72);
```

### 12. Emoji detection &mdash; `unicode/emoji.h`

Identifies emoji codepoints and scans multi-codepoint emoji sequences
(ZWJ sequences, flag sequences, keycap sequences, etc.).

```c
bool n00b_unicode_is_emoji(n00b_codepoint_t cp);
bool n00b_unicode_is_emoji_presentation(n00b_codepoint_t cp);

n00b_unicode_emoji_scan_result_t n00b_unicode_emoji_scan(n00b_string_t s,
                                                         uint32_t byte_pos);
```

**Example:**

```c
n00b_unicode_emoji_scan_result_t r = n00b_unicode_emoji_scan(s, 0);
if (r.type != N00B_UNICODE_EMOJI_NONE) {
    // r.seq_bytes bytes were consumed by the emoji sequence
}
```

### 13. Identifier validation &mdash; `unicode/identifiers.h`

UAX #31 identifier classification and UTS #39 allowed-identifier checks.

```c
bool n00b_unicode_is_id_start(n00b_codepoint_t cp);
bool n00b_unicode_is_id_continue(n00b_codepoint_t cp);
bool n00b_unicode_is_xid_start(n00b_codepoint_t cp);
bool n00b_unicode_is_xid_continue(n00b_codepoint_t cp);
bool n00b_unicode_is_pattern_syntax(n00b_codepoint_t cp);
bool n00b_unicode_is_pattern_white_space(n00b_codepoint_t cp);
bool n00b_unicode_is_identifier_allowed(n00b_codepoint_t cp);
bool n00b_unicode_is_valid_identifier(n00b_string_t s);
```

**Example:**

```c
// Validate a user-supplied identifier:
if (n00b_unicode_is_valid_identifier(name)) {
    // XID_Start followed by XID_Continue*
}
```

### 14. IDNA / Punycode &mdash; `unicode/idna.h`

Internationalized domain name conversion per UTS #46.

```c
n00b_unicode_idna_result_t n00b_unicode_idna_to_ascii(n00b_string_t domain, ...);
n00b_unicode_idna_result_t n00b_unicode_idna_to_unicode(n00b_string_t domain, ...);
```

**Example:**

```c
n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(domain);
if (r.error == N00B_UNICODE_IDNA_OK) {
    // r.value contains the ACE-encoded domain
}
```

### 15. Security analysis &mdash; `unicode/security.h`

Confusable detection and script restriction analysis per UTS #39.

```c
n00b_string_t n00b_unicode_skeleton(n00b_string_t s, ...);
bool          n00b_unicode_is_confusable(n00b_string_t a, n00b_string_t b);

n00b_unicode_restriction_level_t n00b_unicode_script_restriction(n00b_string_t s);
bool n00b_unicode_has_mixed_scripts(n00b_string_t s);
```

**Example:**

```c
// Detect homograph attacks:
if (n00b_unicode_is_confusable(user_input, known_label)) {
    // potential spoofing
}

// Enforce script purity:
n00b_unicode_restriction_level_t level = n00b_unicode_script_restriction(s);
if (level > N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT) {
    // mixed scripts detected
}
```

### 16. Composable queries &mdash; `unicode/query.h`

Filter-based codepoint search across the entire Unicode range. Filters are
composable predicates; queries combine them with AND or OR logic.

```c
// Filter constructors:
n00b_cp_filter_t n00b_filter_gc(n00b_unicode_gc_t gc);
n00b_cp_filter_t n00b_filter_script(n00b_unicode_script_t script);
n00b_cp_filter_t n00b_filter_bidi(n00b_unicode_bidi_class_t bidi);
n00b_cp_filter_t n00b_filter_property(n00b_unicode_property_t prop);
n00b_cp_filter_t n00b_filter_range(n00b_codepoint_t lo, n00b_codepoint_t hi);
n00b_cp_filter_t n00b_filter_block(n00b_unicode_block_t block);
n00b_cp_filter_t n00b_filter_eaw(n00b_unicode_eaw_t eaw);

// Name lookup:
const char *n00b_unicode_cp_name(n00b_codepoint_t cp);
n00b_option_t(n00b_codepoint_t) n00b_unicode_cp_from_name(const char *name);
```

#### Query macros

```c
// AND query (all filters must match):
n00b_cp_query(filter1, filter2, ..., .max_results = N, .out_count = &cnt);

// OR query (any filter may match):
n00b_cp_query_any(filter1, filter2, ...);
```

Both accept keyword arguments: `.range_start`, `.range_end`, `.max_results`,
`.out_count`, `.allocator`.

**Example:**

```c
// Find all uppercase Latin letters:
uint32_t count;
n00b_codepoint_t *results = n00b_cp_query(
    n00b_filter_gc(N00B_UNICODE_GC_LU),
    n00b_filter_script(N00B_UNICODE_SCRIPT_LATIN),
    .out_count = &count
);

// Find codepoints that are either emoji or math symbols:
n00b_codepoint_t *either = n00b_cp_query_any(
    n00b_filter_property(N00B_UNICODE_PROP_EMOJI),
    n00b_filter_gc(N00B_UNICODE_GC_SM),
    .max_results = 500,
    .out_count = &count
);

// Reverse lookup by name:
n00b_option_t(n00b_codepoint_t) cp = n00b_unicode_cp_from_name("SNOWMAN");
```

---

## Cross-cutting patterns

### Keyword arguments (`_kargs`)

Most functions that allocate accept optional keyword arguments via ncc's `_kargs`
extension. At the call site, pass them with designated-initializer syntax after
the positional arguments:

```c
n00b_unicode_toupper(s, .locale = "tr", .allocator = my_alloc)
n00b_unicode_linebreak_wrap(s, &num, .width = 72)
n00b_cp_query(n00b_filter_gc(N00B_UNICODE_GC_LU), .max_results = 100)
```

When no keyword arguments are needed, simply omit them &mdash; defaults apply
automatically. The most common keyword argument is `.allocator`; when `nullptr`
(the default), the library uses the system allocator.

### Memory ownership

- Functions returning `n00b_string_t` return a new string by value.
  The caller owns `s.data` and must eventually free it.
- Functions returning arrays (query, transcoding) return caller-owned
  heap allocations that must be freed.
- Sort keys must be freed with `n00b_unicode_sort_key_free()`.
- Opaque handles (normalizer, break iterator, bidi paragraph) must be freed
  with their corresponding `*_free()` function.
- The `cleanup` attribute on iteration macros handles iterator deallocation
  automatically.

### Internal `_raw` API

Many public functions have an internal counterpart in
`include/internal/unicode/raw.h` that operates on `(const char *data, int64_t len)`
pairs instead of `n00b_string_t`. These are prefixed `n00b_unicode_*_raw` and are
intended for use within the library itself, not by application code.

---

## Quick reference

| Task | Function |
|------|----------|
| Validate UTF-8 | `n00b_unicode_str_validate(s)` |
| Count graphemes | `n00b_unicode_grapheme_count(s)` |
| Terminal display width | `n00b_unicode_display_width(s)` |
| Normalize to NFC | `n00b_unicode_nfc(s)` |
| Uppercase | `n00b_unicode_toupper(s)` |
| Case-insensitive compare | `n00b_unicode_casecmp(a, b)` |
| Locale-aware sort | `n00b_unicode_collate(a, b)` |
| Iterate graphemes | `n00b_unicode_foreach_grapheme(&s, g) { ... }` |
| Iterate words | `n00b_unicode_foreach_word(&s, w) { ... }` |
| Wrap to width | `n00b_unicode_linebreak_wrap(s, &num, .width = 80)` |
| Detect emoji sequence | `n00b_unicode_emoji_scan(s, byte_pos)` |
| Validate identifier | `n00b_unicode_is_valid_identifier(s)` |
| Domain to Punycode | `n00b_unicode_idna_to_ascii(domain)` |
| Confusable check | `n00b_unicode_is_confusable(a, b)` |
| Script restriction | `n00b_unicode_script_restriction(s)` |
| Bidi reorder | `n00b_unicode_bidi_open(s)` / `bidi_reorder_visual` |
| Codepoint name | `n00b_unicode_cp_name(cp)` |
| Query by property | `n00b_cp_query(n00b_filter_gc(...), ...)` |
