# n00b String Library

## Overview

The n00b string library builds on `n00b_string_t` (defined in `core/string.h`) to
provide Unicode-aware string manipulation, rich text styling, formatted output,
markdown parsing, and ANSI terminal sequence handling.

The library is split into three tiers:

1. **String operations** &mdash; grapheme-aware slicing, search, replace, split,
   pad, wrap, and more.
2. **Rich text** &mdash; an abstract styling system with named styles, text roles,
   a rich markup language (`[|b|]bold[|/b|]` / `«b»bold«/b»`), compile-time
   rich string literals (`r"..."`), and `n00b_format()` for runtime formatting
   with substitutions.
3. **Markdown &amp; ANSI** &mdash; markdown parsing into styled strings, and an
   incremental ANSI/VT escape sequence parser.

### Design principles

- **Grapheme-aware by default.** String operations like slicing, indexing,
  padding, and reversal work on grapheme clusters, not bytes or codepoints.
- **Abstract styling.** The style system describes *what* styling is requested
  (bold, italic, font hint, palette index) without committing to *how* it is
  rendered.  No ANSI codes appear until the presentation layer resolves styles.
- **Compile-time rich text.** The `r"..."` string literal syntax lets ncc
  parse rich markup at compile time, producing static `n00b_string_t` values
  with styling baked in &mdash; zero runtime parsing cost.
- **Keyword arguments.** Functions that allocate accept an optional `.allocator`
  keyword argument via ncc's `_kargs` extension.

---

## Modules

### 1. String operations &mdash; `strings/string_ops.h`

The largest module.  Full suite of Unicode-aware string manipulation, all
grapheme-cluster-aware where relevant.

#### Concatenation and joining

```c
n00b_string_t n00b_unicode_str_cat(n00b_string_t a, n00b_string_t b, ...);
n00b_string_t n00b_unicode_str_cat_many(n00b_array_t(n00b_string_t) parts, ...);
n00b_string_t n00b_unicode_str_join(n00b_string_t sep,
                                     n00b_array_t(n00b_string_t) parts, ...);
```

#### Slicing (grapheme-indexed, negative indices supported)

```c
n00b_string_t n00b_unicode_str_slice(n00b_string_t s, int32_t start, int32_t end, ...);
n00b_string_t n00b_unicode_str_grapheme_at(n00b_string_t s, int32_t index, ...);
n00b_string_t n00b_unicode_str_slice_bytes(n00b_string_t s,
                                            uint32_t byte_start, uint32_t byte_end, ...);
```

#### Search

```c
// All search functions accept these keyword options:
//   .reverse         = false   Search from the right (find only)
//   .normalize       = true    NFC-normalize before comparison
//   .case_sensitive  = true    When false, Unicode case-fold
//   .strip_marks     = false   When true, strip accents/diacritics

n00b_unicode_opt_i32_t n00b_unicode_str_find(n00b_string_t haystack,
                                              n00b_string_t needle, ...);
bool n00b_unicode_str_contains(n00b_string_t haystack,
                                n00b_string_t needle, ...);
bool n00b_unicode_str_starts_with(n00b_string_t s, n00b_string_t prefix);
bool n00b_unicode_str_ends_with(n00b_string_t s, n00b_string_t suffix);
```

#### Replace

```c
n00b_string_t n00b_unicode_str_replace(n00b_string_t s, n00b_string_t old_s,
                                        n00b_string_t new_s, ...);
n00b_string_t n00b_unicode_str_replace_all(n00b_string_t s, n00b_string_t old_s,
                                            n00b_string_t new_s, ...);
```

#### Split (returns `n00b_array_t(n00b_string_t)`)

```c
n00b_array_t(n00b_string_t) n00b_unicode_str_split(n00b_string_t s,
                                                     n00b_string_t sep, ...);
n00b_array_t(n00b_string_t) n00b_unicode_str_split_words(n00b_string_t s, ...);
n00b_array_t(n00b_string_t) n00b_unicode_str_split_graphemes(n00b_string_t s, ...);
n00b_array_t(n00b_string_t) n00b_unicode_str_split_lines(n00b_string_t s, ...);
```

#### Trim

```c
n00b_string_t n00b_unicode_str_trim(n00b_string_t s, ...);
    // keyword args: .left = true, .right = true, .allocator
```

Compat macros `n00b_unicode_str_trim_left` (`.right = false`) and
`n00b_unicode_str_trim_right` (`.left = false`) are still available.

#### Comparison

```c
int  n00b_unicode_str_cmp(n00b_string_t a, n00b_string_t b);
bool n00b_unicode_str_eq(n00b_string_t a, n00b_string_t b, ...);
    // keyword args: .normalize = false, .case_sensitive = true, .strip_marks = false
```

Compat macros `n00b_unicode_str_eq_nfc` (`.normalize = true`) and
`n00b_unicode_str_eq_casefold` (`.case_sensitive = false`) are still available.

#### Width-aware padding and truncation

```c
n00b_string_t n00b_unicode_str_pad(n00b_string_t s, int32_t width, ...);
    // keyword args: .align = N00B_STR_ALIGN_LEFT, .fill = ' ', .allocator
n00b_string_t n00b_unicode_str_truncate(n00b_string_t s, int32_t max_width, ...);
    // keyword args: .allocator, .ellipsis = "..."
```

Compat macros `n00b_unicode_str_pad_left` (`.align = N00B_STR_ALIGN_RIGHT`),
`n00b_unicode_str_pad_right` (`.align = N00B_STR_ALIGN_LEFT`), and
`n00b_unicode_str_center` (`.align = N00B_STR_ALIGN_CENTER`) are still available.

#### Repeat, reverse, and wrap

```c
n00b_string_t n00b_unicode_str_repeat(n00b_string_t s, uint32_t count, ...);
n00b_string_t n00b_unicode_str_reverse(n00b_string_t s, ...);

n00b_array_t(n00b_string_t) n00b_unicode_str_wrap(n00b_string_t s, ...);
    // keyword args: .width = 80, .hang = 0, .no_hard_wrap = false, .allocator
```

#### Escape / unescape

```c
n00b_string_t n00b_unicode_str_escape(n00b_string_t s, ...);
n00b_result_t(n00b_string_t) n00b_unicode_str_unescape(n00b_string_t s, ...);
```

**Example:**

```c
n00b_string_t hello = n00b_string_from_raw(alloc, "Hello", 5, 0);
n00b_string_t world = n00b_string_from_raw(alloc, "World", 5, 0);

// Concatenate:
n00b_string_t hw = n00b_unicode_str_cat(hello, world);

// Grapheme-aware slice (negative index = from end):
n00b_string_t last3 = n00b_unicode_str_slice(s, -3, -1);

// Width-aware truncation with custom ellipsis:
n00b_string_t t = n00b_unicode_str_truncate(s, 20, .ellipsis = "\xe2\x80\xa6");

// Grapheme-aware reverse (keeps combining marks with base):
n00b_string_t r = n00b_unicode_str_reverse(s);

// Wrap to 72 columns with 4-column hanging indent:
n00b_array_t(n00b_string_t) lines = n00b_unicode_str_wrap(s, .width = 72, .hang = 4);
```

### 2. String conversions &mdash; `strings/string_convert.h`

Conversions between `n00b_string_t` and other representations: integers,
hexadecimal, C strings, string literals, codepoints, and files.

```c
n00b_string_t n00b_unicode_str_from_int(int64_t n, ...);
n00b_string_t n00b_unicode_str_to_hex(n00b_string_t s, bool upper, ...);
char         *n00b_unicode_str_to_cstr(n00b_string_t s, ...);
n00b_string_t n00b_unicode_str_to_literal(n00b_string_t s, ...);
n00b_string_t n00b_unicode_str_from_codepoint(n00b_codepoint_t cp, ...);

n00b_result_t(n00b_string_t) n00b_unicode_str_from_file(const char *path, ...);
```

**Example:**

```c
n00b_string_t dec = n00b_unicode_str_from_int(42);
// dec.data == "42"

n00b_result_t(n00b_string_t) r = n00b_unicode_str_from_file("/etc/hostname");
n00b_string_t contents = n00b_result_unwrap(r);
```

### 3. Numeric formatting &mdash; `strings/fmt_numbers.h`

Higher-level numeric formatting with commas, zero-padding, configurable
bool output, codepoint display, and pointer formatting.

```c
n00b_string_t n00b_fmt_hex(uint64_t value, bool caps, ...);
n00b_string_t n00b_fmt_int(int64_t value, bool commas, ...);
n00b_string_t n00b_fmt_uint(uint64_t value, bool commas, ...);
n00b_string_t n00b_fmt_float(double value, int width, bool fill, ...);
n00b_string_t n00b_fmt_bool(bool value, bool upper, bool word, bool yn, ...);
n00b_string_t n00b_fmt_codepoint(n00b_codepoint_t cp, ...);
n00b_string_t n00b_fmt_pointer(void *ptr, bool caps, ...);
```

Float formatting uses the Grisu2 algorithm (via `strings/fptostr.h`) for
shortest-representation output.

**Example:**

```c
n00b_string_t s = n00b_fmt_int(1234567, true);
// s.data == "1,234,567"

n00b_string_t h = n00b_fmt_hex(0xDEAD, true);
// h.data == "DEAD"

n00b_string_t b = n00b_fmt_bool(true, false, true, false);
// b.data == "true"
```

---

## Rich text

The rich text system adds abstract styling to `n00b_string_t` values.
Styles describe decorations (bold, italic, underline, strikethrough),
font hints (mono, serif, sans), text case transformations, palette indices,
and direct RGB colors.  No rendering decisions are made &mdash; the
presentation layer (ANSI output, themes, GUI) interprets the abstract
metadata later.

### 4. Style types &mdash; `strings/text_style.h`

Pure type definitions for the style system:

| Type | Purpose |
|------|---------|
| `n00b_tristate_t` | `UNSPECIFIED` / `NO` / `YES` for style inheritance |
| `n00b_text_case_t` | `NONE` / `UPPER` / `LOWER` / `TITLE` / `CAPS` |
| `n00b_font_hint_t` | `DEFAULT` / `MONO` / `SERIF` / `SANS` |
| `n00b_text_style_t` | Full style descriptor (decorations, font, color) |
| `n00b_style_record_t` | Style + byte range within a string |
| `n00b_string_style_info_t` | Aggregate styling metadata (flexible array) |

`n00b_text_style_t` uses tristate fields so that "unspecified" is always
distinguishable from an explicit setting.  This allows styles to be merged:
an overlay's unspecified fields inherit from the base.

Color fields use the high bit (`1 << 31`) as a validity flag:

```c
n00b_color_t red = n00b_color_make(0xFF0000);    // valid red
bool valid = n00b_color_is_set(red);              // true
uint32_t rgb = n00b_color_rgb(red);               // 0xFF0000
```

### 5. Style operations &mdash; `strings/style_ops.h`

Constructor, merge/overlay, comparison, and copy for `n00b_text_style_t`.

```c
n00b_text_style_t *n00b_str_style_new(...);
n00b_text_style_t *n00b_str_style_merge(const n00b_text_style_t *base,
                                         const n00b_text_style_t *overlay, ...);
n00b_text_style_t *n00b_str_style_copy(const n00b_text_style_t *src, ...);
bool               n00b_str_style_eq(const n00b_text_style_t *a,
                                      const n00b_text_style_t *b);
bool               n00b_str_style_is_empty(const n00b_text_style_t *s);
```

Merge follows inheritance rules: for each field, the overlay value is
used unless it is unspecified/sentinel, in which case the base value is kept.

**Example:**

```c
n00b_text_style_t *base = n00b_str_style_new();
base->bold = N00B_TRI_YES;

n00b_text_style_t *overlay = n00b_str_style_new();
overlay->italic = N00B_TRI_YES;

n00b_text_style_t *merged = n00b_str_style_merge(base, overlay);
// merged->bold == N00B_TRI_YES  (inherited from base)
// merged->italic == N00B_TRI_YES  (from overlay)
```

### 6. String styling &mdash; `strings/string_style.h`

Attach, query, and manipulate styling metadata on `n00b_string_t`.  All
functions return `n00b_string_t` by value &mdash; the original string is
never mutated.

```c
n00b_string_t             n00b_str_set_base_style(n00b_string_t s,
                                                    const n00b_text_style_t *style, ...);
n00b_string_t             n00b_str_add_style(n00b_string_t s,
                                              const n00b_text_style_t *style,
                                              size_t start,
                                              n00b_option_t(size_t) end_opt, ...);
n00b_string_style_info_t *n00b_str_get_style_info(n00b_string_t s);
n00b_text_style_t        *n00b_str_resolve_style_at(n00b_string_t s,
                                                      size_t byte_pos, ...);
n00b_string_t             n00b_str_strip_styles(n00b_string_t s);
```

Style ranges use byte offsets.  Pass `n00b_option_none(size_t)` for the end
to make a style extend to the end of the string.

**Example:**

```c
n00b_text_style_t *bold = n00b_str_style_new();
bold->bold = N00B_TRI_YES;

// Apply bold to bytes [0, 5):
n00b_string_t styled = n00b_str_add_style(s, bold, 0,
                                            n00b_option_set(size_t, 5));

// Query the effective style at byte 2:
n00b_text_style_t *at2 = n00b_str_resolve_style_at(styled, 2);
// at2->bold == N00B_TRI_YES
```

### 7. Style &amp; role registry &mdash; `strings/style_registry.h`

A global registry of named styles and text roles, initialized with
defaults by `n00b_init()`.

```c
void n00b_str_registry_init(void);

void               n00b_str_style_register(const char *name,
                                            const n00b_text_style_t *style);
n00b_text_style_t *n00b_str_style_lookup(const char *name);

void               n00b_str_role_register(const char *name,
                                           const n00b_text_style_t *style);
n00b_text_style_t *n00b_str_role_lookup(const char *name);
```

**Built-in named styles:**

| Name | Effect |
|------|--------|
| `em` | italic |
| `em1` / `em2` / `em3` | italic, bold+italic, bold+underline |
| `h1` / `h2` / `h3` | bold (+ case variants) |

**Built-in text roles:**

| Role | Effect |
|------|--------|
| `@code` / `@mono` | font_hint = MONO |
| `@heading` | bold |
| `@error` | bold + (color via theme) |
| `@success` / `@muted` / `@link` / `@label` / `@button` / `@input` | appropriate defaults |

### 8. Rich markup descriptors &mdash; `strings/rich_desc.h`

Parses rich markup format strings into segment arrays.  Two delimiter
forms are supported:

| Bracket form | Guillemet form | Meaning |
|-------------|----------------|---------|
| `[|b|]` | `«b»` | Turn on property (bold) |
| `[|/b|]` | `«/b»` | Turn off property |
| `[|name|]` | `«name»` | Push named style |
| `[|/name|]` | `«/name»` | Pop named style |
| `[|@role|]` | `«@role»` | Push text role |
| `[|/@role|]` | `«/@role»` | Pop text role |
| `[|/|]` | `«/»` | Reset all styles |
| `[|#|]` | | Auto-indexed substitution |
| `[|#N|]` | | Explicit-indexed substitution |
| `[|#N:spec|]` | | Substitution with format spec |
| `[|#!|]` | | Substitution, strip styling |

Inline property tags: `b`/`bold`, `i`/`italic`, `u`/`underline`,
`uu`/`2u`, `st`/`strike`/`strikethrough`, `r`/`reverse`, `dim`/`faint`,
`blink`, `upper`/`up`, `lower`/`l`, `caps`/`allcaps`, `t`/`title`.

Escaping: `\[`, `\\`, `\«` produce literal characters.

```c
n00b_rich_desc_t *n00b_rich_desc_parse(const char *desc, int32_t desc_len);
void              n00b_rich_desc_cache_init(void);
```

Parsed descriptors are cached by XXH3 hash of the input bytes.

### 9. Format specifiers &mdash; `strings/format_spec.h`

Printf-like format specifiers for substitution tags.  The spec string
(the part after `:` in `[|#N:spec|]`) is parsed into an
`n00b_format_spec_t`.

Syntax: `[flags][width][.precision]type`

| Flags | Meaning |
|-------|---------|
| `-` | Left-align |
| `0` | Zero-pad |
| `+` | Force sign |
| ` ` | Space for positive |
| `,` | Thousands separator |

| Types | Meaning |
|-------|---------|
| `d`/`i` | Signed decimal |
| `u` | Unsigned decimal |
| `x`/`X` | Hex |
| `o` | Octal |
| `f` | Fixed float |
| `e`/`E` | Scientific |
| `g`/`G` | Shortest |
| `s` | String |
| `b`/`B` | Bool (true/false) |
| `y`/`Y` | Bool (yes/no) |
| `p`/`P` | Pointer |

```c
n00b_format_spec_t n00b_format_spec_parse(const char *spec, int spec_len);

n00b_string_t n00b_str_fmt_int_ex(int64_t value,
                                    const n00b_format_spec_t *spec, ...);
n00b_string_t n00b_str_fmt_float_ex(double value,
                                      const n00b_format_spec_t *spec, ...);
n00b_string_t n00b_str_fmt_string_ex(n00b_string_t value,
                                       const n00b_format_spec_t *spec, ...);
```

### 10. Rich string formatting &mdash; `strings/format.h`

The capstone API.  Takes a rich markup descriptor and variadic arguments,
returns a styled `n00b_string_t` with abstract styling metadata.

```c
n00b_string_t n00b_format(n00b_string_t desc, +);
n00b_string_t n00b_cformat(const char *desc, +);
```

The `+` in the signature is ncc's checked-variadic syntax.  Integer args
are passed as `int64_t` (via cast to `void *`), string args as
`n00b_string_t *`, double args as `double *`.

**Processing pipeline:**

1. Parse descriptor via `n00b_rich_desc_parse()` (cached by XXH3 hash)
2. Walk segments, maintaining a style stack (bounded depth ~32):
   - `TEXT` &rarr; append literal text with current merged style
   - `STYLE_ON/OFF` &rarr; push/pop named style from registry
   - `PROP_ON/OFF` &rarr; push/pop inline property style
   - `ROLE_ON/OFF` &rarr; push/pop role from registry
   - `RESET` &rarr; clear style stack
   - `SUBST` &rarr; format argument via spec dispatch, append result
3. Build `n00b_string_style_info_t`, attach to result string

**Example:**

```c
// Simple bold text:
n00b_string_t s = n00b_cformat("[|b|]Hello[|/b|] world");

// Substitution with comma-separated integer:
int64_t n = 1234567;
n00b_string_t s = n00b_cformat("Count: [|#:,d|]", &n);
// s.data == "Count: 1,234,567"

// Guillemet syntax with nested styles:
n00b_string_t s = n00b_cformat("«b»bold «i»and italic«/i»«/b»");

// Text role:
n00b_string_t s = n00b_cformat("[|@code|]x = 42[|/@code|]");
// "x = 42" has font_hint = MONO

// Reset all styles mid-string:
n00b_string_t s = n00b_cformat("[|b|][|i|]styled[|/|] plain");

// Float with precision:
double pi = 3.14159;
n00b_string_t s = n00b_cformat("pi = [|#:.2f|]", &pi);
// s.data == "pi = 3.14"
```

### 11. Rich text literals &mdash; ncc `r"..."` syntax

For strings whose markup is known at compile time, ncc provides the
`r"..."` prefix. The compiler parses the rich markup during compilation
and emits a static `n00b_string_t` with styling baked in &mdash; no
runtime parsing, no cache lookup, zero overhead.

```c
n00b_string_t *greeting = r"«b»Hello«/b» world";
n00b_string_t *msg      = r"[|i|]italic[|/i|] and [|b|]bold[|/b|]";
n00b_string_t *code     = r"«@code»x = 42«/@code»";
```

**Markup supported:** all inline property tags (`b`, `i`, `u`, `uu`, `st`,
`r`, `dim`, `blink`), text case tags (`upper`, `lower`, `title`, `caps`),
named styles, text roles (`@code`, `@heading`, etc.), and reset (`«/»` /
`[|/|]`). Escape `\«` for a literal guillemet, `\\` for a literal
backslash. Substitutions (`[|#|]`, `[|#N:spec|]`) are NOT supported in
`r"..."` &mdash; use `n00b_format()` / `n00b_cformat()` for those.

**When to use which:**

| Scenario | Use |
|----------|-----|
| Static styled text, no substitutions | `r"«b»hello«/b»"` |
| Styled text with runtime values | `n00b_cformat("[|b|]count:[|/b|] [|#:,d|]", &n)` |
| Plain text, no styling | `STR("hello")` or `n00b_string_from_raw(...)` |

**Full reference:** `r"..."` is one of several ncc compile-time literal
forms (alongside `b"..."` buffers, `[...]` / `a{...}` arrays, `l{...}`
lists, and `d{...}` dicts). For the comprehensive user manual covering
all forms, the build-time helper, the cached_hash perf path, the lock
model, and the libn00b migration recipe, see
[`ncc_static_objects.md`](ncc_static_objects.md) in this same directory.
Dict literals also have their own focused reference at
[`dict_literals.md`](dict_literals.md).

---

## Markdown

### 12. Markdown parsing &mdash; `strings/markdown.h`

Parses markdown into an N-ary typed tree using md4c as the backend.
Supports GitHub-Flavored Markdown extensions (tables, strikethrough,
task lists, autolinks).

```c
n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *
n00b_parse_markdown(n00b_string_t s, ...);
```

The tree uses `n00b_tree_t(N, L)` with the same type for internal nodes
and leaves.  Block nodes (paragraph, heading, list, code block, HR) are
internal nodes; text nodes are leaves.

Node kinds cover three categories:

| Category | Examples |
|----------|---------|
| Block | `N00B_MD_BLOCK_P`, `N00B_MD_BLOCK_H`, `N00B_MD_BLOCK_CODE`, `N00B_MD_BLOCK_UL`, `N00B_MD_BLOCK_LI`, `N00B_MD_BLOCK_HR` |
| Span | `N00B_MD_SPAN_STRONG`, `N00B_MD_SPAN_EM`, `N00B_MD_SPAN_CODE`, `N00B_MD_SPAN_STRIKETHRU`, `N00B_MD_SPAN_U` |
| Text | `N00B_MD_TEXT_NORMAL`, `N00B_MD_TEXT_CODE`, `N00B_MD_TEXT_BR` |

**Example:**

```c
n00b_string_t src = STR("# Hello\n\nSome **bold** text.");
auto tree = n00b_parse_markdown(src);
// tree is rooted at N00B_MD_DOCUMENT with block children
```

### 13. Markdown rendering &mdash; `strings/md_render.h`

Walks a markdown AST and produces a single styled `n00b_string_t` with
style records for each inline span.

```c
n00b_string_t
n00b_str_md_render(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree);
```

| Markdown element | Style applied |
|------------------|---------------|
| `**strong**` | bold |
| `*emphasis*` | italic |
| `` `code` `` | font_hint = MONO |
| `~~strikethrough~~` | strikethrough |
| `<u>underline</u>` | underline |
| `# heading` | bold |
| ```` ``` ```` (code block) | font_hint = MONO |

**Example:**

```c
n00b_string_t src = STR("**bold** and *italic*");
auto tree = n00b_parse_markdown(src);

n00b_string_t styled = n00b_str_md_render(tree);
// styled.data == "bold and italic"
// styled has style records: bold on [0,4), italic on [9,15)
```

### 14. Markdown line array &mdash; `strings/md_lines.h`

Converts a markdown AST into an array of styled lines &mdash; one entry
per renderable unit.  No layout or line-wrapping is performed; the caller
is responsible for reflowing to a target width.

```c
n00b_array_t(n00b_string_t)
n00b_str_md_to_lines(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree);
```

| AST node | Array entry |
|----------|-------------|
| Paragraph | One entry with inline styles |
| Heading | One entry with bold style |
| List item | One entry per item (bullet prefix) |
| Code block | One entry per source line (mono) |
| Horizontal rule | One entry containing `"---"` |

**Example:**

```c
n00b_string_t src = STR("# Title\n\nSome text.\n\n- one\n- two\n\n---\n\n```\ncode\n```");
auto tree = n00b_parse_markdown(src);

n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
// lines[0].data == "Title"       (bold)
// lines[1].data == "Some text."
// lines[2].data == "- one"
// lines[3].data == "- two"
// lines[4].data == "---"
// lines[5].data == "code"        (mono)

n00b_array_free(lines);
```

---

## ANSI terminal

### 15. ANSI parser &mdash; `strings/ansi.h`

Incremental state-machine parser that classifies raw terminal output into
text spans, C0/C1 control codes, CSI sequences, nF/Fp/Fe/Fs sequences,
and control strings (DCS, OSC, PM, APC, SOS).

```c
n00b_ansi_ctx *n00b_ansi_parser_create(...);
void           n00b_ansi_parse(n00b_ansi_ctx *ctx, n00b_buffer_t *buf);

n00b_list_t(n00b_ansi_node_t *) n00b_ansi_parser_results(n00b_ansi_ctx *ctx);

n00b_string_t n00b_ansi_nodes_to_string(
    n00b_list_t(n00b_ansi_node_t *) nodes, bool keep_control, ...);
```

Node classifications:

| Kind | Meaning |
|------|---------|
| `N00B_ANSI_TEXT` | Printable text run |
| `N00B_ANSI_C0_CODE` / `N00B_ANSI_C1_CODE` | Control codes |
| `N00B_ANSI_CONTROL_SEQUENCE` | CSI sequence (e.g., SGR colors) |
| `N00B_ANSI_PRIVATE_CONTROL` | CSI with private parameter indicator |
| `N00B_ANSI_NF_SEQUENCE` / `N00B_ANSI_FP_SEQUENCE` / `N00B_ANSI_FE_SEQUENCE` / `N00B_ANSI_FS_SEQUENCE` | Escape sequences |
| `N00B_ANSI_CTL_STR_CHAR` / `N00B_ANSI_CRL_STR_CMD` | Control strings |
| `N00B_ANSI_PARTIAL` | Incomplete (awaiting more data) |

The parser is designed for incremental use: feed successive buffers with
`n00b_ansi_parse()` and retrieve completed nodes with
`n00b_ansi_parser_results()`.  Partial sequences are carried over
automatically.

Convenience macros for common ANSI escape strings are also provided
(`n00b_ansi_dim`, `n00b_ansi_reset`, `n00b_erase_screen`,
cursor movement format strings, etc.).

**Example:**

```c
n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
n00b_ansi_parse(ctx, buf);

n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);

// Strip control sequences, keep only text:
n00b_string_t plain = n00b_ansi_nodes_to_string(nodes, false);

n00b_free(ctx);
```

---

## Cross-cutting patterns

### Keyword arguments (`_kargs`)

Most functions that allocate accept optional keyword arguments via ncc's
`_kargs` extension:

```c
n00b_unicode_str_pad(s, 40, .fill = '.', .allocator = my_alloc)
n00b_unicode_str_wrap(s, .width = 72, .hang = 4)
n00b_parse_markdown(src, .allocator = arena)
```

When no keyword arguments are needed, simply omit them &mdash; defaults apply.

### Memory ownership

- Functions returning `n00b_string_t` return a new string by value.
  The caller owns `s.data` and must eventually free it.
- Functions returning `n00b_array_t(T)` return caller-owned arrays;
  free with `n00b_array_free(arr)`.
- Functions returning `n00b_list_t(T)` return caller-owned lists;
  free with `n00b_list_free(list)`.
- Opaque handles (`n00b_ansi_ctx *`) must be freed with `n00b_free()`.
- Rich text literals (`r"..."`) return pointers to static data &mdash;
  do not free.
- Style pointers from `n00b_str_style_new()`, `n00b_str_style_merge()`,
  and `n00b_str_style_copy()` are caller-owned.

### Styling inspection

To examine the styling on any `n00b_string_t`:

```c
n00b_string_style_info_t *info = n00b_str_get_style_info(s);
if (info) {
    // info->base_style: default style for the whole string (may be NULL)
    // info->num_styles: number of range records
    // info->styles[i]: { .info, .start, .end }
    for (int64_t i = 0; i < info->num_styles; i++) {
        n00b_style_record_t *rec = &info->styles[i];
        // rec->info->bold, rec->info->italic, etc.
    }
}

// Or resolve the effective merged style at a specific position:
n00b_text_style_t *at = n00b_str_resolve_style_at(s, byte_offset);
```

---

## Quick reference

| Task | Function / syntax |
|------|-------------------|
| Grapheme-aware slice | `n00b_unicode_str_slice(s, start, end)` |
| Search | `n00b_unicode_str_find(haystack, needle)` |
| Split on separator | `n00b_unicode_str_split(s, sep)` |
| Pad to width | `n00b_unicode_str_pad(s, width)` |
| Wrap to width | `n00b_unicode_str_wrap(s, .width = 72)` |
| Reverse (grapheme-safe) | `n00b_unicode_str_reverse(s)` |
| Integer to string | `n00b_fmt_int(value, commas)` |
| Float to string | `n00b_fmt_float(value, width, fill)` |
| Format with styles | `n00b_cformat("[|b|]hello[|/b|] [|#|]", &name)` |
| Compile-time rich text | `r"«b»hello«/b»"` |
| Set base style | `n00b_str_set_base_style(s, style)` |
| Add ranged style | `n00b_str_add_style(s, style, start, end_opt)` |
| Query style at position | `n00b_str_resolve_style_at(s, byte_pos)` |
| Register named style | `n00b_str_style_register(name, style)` |
| Parse markdown | `n00b_parse_markdown(src)` |
| Markdown to styled string | `n00b_str_md_render(tree)` |
| Markdown to line array | `n00b_str_md_to_lines(tree)` |
| Parse ANSI sequences | `n00b_ansi_parse(ctx, buf)` |
| Strip ANSI to plain text | `n00b_ansi_nodes_to_string(nodes, false)` |
