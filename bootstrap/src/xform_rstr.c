/**
 * @file xform_rstr.c
 * @brief Transform `__ncc_rstr("...")` calls into static compound literal
 *        `n00b_string_t` expressions with pre-compiled styling data.
 *
 * The pre-CPP scan (in compile_packrat.c) rewrites `r"..."` into
 * `__ncc_rstr("...")`.  This transform parses the rich markup at compile
 * time and emits a static compound literal that constructs the string
 * with all styling baked in.
 *
 * ## Supported tags
 *
 * | Syntax | Meaning |
 * |--------|---------|
 * | `«b»` / `«bold»` | Bold on |
 * | `«i»` / `«italic»` | Italic on |
 * | `«u»` / `«underline»` | Underline on |
 * | `«uu»` / `«2u»` | Double underline on |
 * | `«st»` / `«strike»` / `«strikethrough»` | Strikethrough on |
 * | `«r»` / `«reverse»` | Reverse on |
 * | `«dim»` / `«faint»` | Dim on |
 * | `«blink»` | Blink on |
 * | `«upper»` / `«up»` | Uppercase |
 * | `«lower»` / `«l»` | Lowercase |
 * | `«title»` / `«t»` | Title case |
 * | `«caps»` / `«allcaps»` | All-caps |
 * | `«/tag»` | Close the named tag |
 * | `«/»` | Reset all styles |
 * | `«name»` | Named style (deferred lookup) |
 * | `«@role»` | Role (deferred lookup) |
 * | `\«` | Literal `«` |
 * | `\\` | Literal `\` |
 *
 * Substitutions (`#`, `#N`, `#N:spec`) are rejected with a compile error
 * since they require runtime arguments.
 */

#include "branch_symbols.h"
#include "transform.h"
#include "xform_helpers.h"
#include "types.h"
#include "nt_types.h"
#include "ncc_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <stddef.h>

// =========================================================================
// UTF-8 codepoint counter
// =========================================================================

static int64_t
count_utf8_codepoints(const char *data, int64_t len)
{
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if ((data[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

// =========================================================================
// Rich markup property / case tables (mirrors rich_desc.c)
// =========================================================================

static const struct {
    const char *name;
    const char *field_name; // C struct field name in n00b_text_style_t
} prop_table[] = {
    {"b",             "bold"},
    {"bold",          "bold"},
    {"i",             "italic"},
    {"italic",        "italic"},
    {"u",             "underline"},
    {"underline",     "underline"},
    {"uu",            "double_underline"},
    {"2u",            "double_underline"},
    {"st",            "strikethrough"},
    {"strike",        "strikethrough"},
    {"strikethrough", "strikethrough"},
    {"r",             "reverse"},
    {"reverse",       "reverse"},
    {"dim",           "dim"},
    {"faint",         "dim"},
    {"blink",         "blink"},
    {NULL,            NULL},
};

typedef enum {
    RSTR_CASE_NONE  = 0,
    RSTR_CASE_UPPER = 1,
    RSTR_CASE_LOWER = 2,
    RSTR_CASE_TITLE = 3,
    RSTR_CASE_CAPS  = 4,
} rstr_case_t;

static const struct {
    const char *name;
    rstr_case_t value;
} case_table[] = {
    {"upper",   RSTR_CASE_UPPER},
    {"up",      RSTR_CASE_UPPER},
    {"lower",   RSTR_CASE_LOWER},
    {"l",       RSTR_CASE_LOWER},
    {"caps",    RSTR_CASE_CAPS},
    {"allcaps", RSTR_CASE_CAPS},
    {"t",       RSTR_CASE_TITLE},
    {"title",   RSTR_CASE_TITLE},
    {NULL,      0},
};

// Return the field name for a known inline property, or NULL.
static const char *
lookup_prop_field(const char *name, int name_len)
{
    for (int i = 0; prop_table[i].name; i++) {
        if ((int)strlen(prop_table[i].name) == name_len
            && memcmp(prop_table[i].name, name, name_len) == 0) {
            return prop_table[i].field_name;
        }
    }
    return NULL;
}

// Return the case value for a known case tag, or -1.
static int
lookup_case_val(const char *name, int name_len)
{
    for (int i = 0; case_table[i].name; i++) {
        if ((int)strlen(case_table[i].name) == name_len
            && memcmp(case_table[i].name, name, name_len) == 0) {
            return (int)case_table[i].value;
        }
    }
    return -1;
}

// =========================================================================
// Segment list (compile-time parse result)
// =========================================================================

typedef enum {
    SEG_TEXT,
    SEG_PROP_ON,
    SEG_PROP_OFF,
    SEG_CASE_ON,
    SEG_CASE_OFF,
    SEG_STYLE_ON,
    SEG_STYLE_OFF,
    SEG_ROLE_ON,
    SEG_ROLE_OFF,
    SEG_RESET,
} seg_kind_t;

typedef struct {
    seg_kind_t  kind;
    int         text_offset; // byte offset into stripped text
    int         text_length; // byte count
    const char *field_name;  // prop field name, or style/role name
    int         case_val;    // rstr_case_t for case tags
} rstr_segment_t;

typedef struct {
    rstr_segment_t *segs;
    int             count;
    int             cap;
} rstr_seg_list_t;

static void
rseg_push(rstr_seg_list_t *sl, rstr_segment_t seg)
{
    if (sl->count >= sl->cap) {
        int new_cap          = sl->cap ? sl->cap * 2 : 16;
        rstr_segment_t *new_s = base_alloc(new_cap * sizeof(rstr_segment_t));
        if (sl->segs) {
            memcpy(new_s, sl->segs, sl->count * sizeof(rstr_segment_t));
            base_dealloc(sl->segs);
        }
        sl->segs = new_s;
        sl->cap  = new_cap;
    }
    sl->segs[sl->count++] = seg;
}

// =========================================================================
// Stripped text buffer (plain text with markup removed)
// =========================================================================

typedef struct {
    char *data;
    int   len;
    int   cap;
} rstr_text_buf_t;

static void
rstr_text_append(rstr_text_buf_t *tb, const char *src, int src_len)
{
    if (tb->len + src_len > tb->cap) {
        int new_cap = tb->cap ? tb->cap * 2 : 256;
        while (new_cap < tb->len + src_len) {
            new_cap *= 2;
        }
        char *new_data = base_alloc(new_cap);
        if (tb->data) {
            memcpy(new_data, tb->data, tb->len);
            base_dealloc(tb->data);
        }
        tb->data = new_data;
        tb->cap  = new_cap;
    }
    memcpy(tb->data + tb->len, src, src_len);
    tb->len += src_len;
}

// =========================================================================
// Markup parser
// =========================================================================

// Classify a tag body and emit the appropriate segment.
static void
classify_tag(rstr_seg_list_t *sl, const char *tag_body, int tag_len,
             const char *file, int line)
{
    if (tag_len == 0) {
        return;
    }

    // Reset: "/" alone
    if (tag_len == 1 && tag_body[0] == '/') {
        rseg_push(sl, (rstr_segment_t){.kind = SEG_RESET});
        return;
    }

    // Substitution: starts with '#' — compile error
    if (tag_body[0] == '#') {
        ncc_error("%s:%d: r\"...\" string literals cannot contain "
                  "substitutions (#)\n",
                  file ? file : "<unknown>",
                  line);
        exit(1);
    }

    // Close tag: starts with '/'
    bool        is_close = (tag_body[0] == '/');
    const char *name     = is_close ? tag_body + 1 : tag_body;
    int         name_len = is_close ? tag_len - 1 : tag_len;

    if (name_len == 0) {
        return;
    }

    // Role: starts with '@'
    if (name[0] == '@') {
        char *tag_name = base_alloc(name_len + 1);
        memcpy(tag_name, name, name_len);
        tag_name[name_len] = '\0';
        rseg_push(sl,
                  (rstr_segment_t){
                      .kind       = is_close ? SEG_ROLE_OFF : SEG_ROLE_ON,
                      .field_name = tag_name,
                  });
        return;
    }

    // Inline property?
    const char *field = lookup_prop_field(name, name_len);
    if (field) {
        rseg_push(sl,
                  (rstr_segment_t){
                      .kind       = is_close ? SEG_PROP_OFF : SEG_PROP_ON,
                      .field_name = field,
                  });
        return;
    }

    // Text case?
    int cval = lookup_case_val(name, name_len);
    if (cval >= 0) {
        rseg_push(sl,
                  (rstr_segment_t){
                      .kind     = is_close ? SEG_CASE_OFF : SEG_CASE_ON,
                      .case_val = cval,
                  });
        return;
    }

    // Named style (default)
    char *tag_name = base_alloc(name_len + 1);
    memcpy(tag_name, name, name_len);
    tag_name[name_len] = '\0';
    rseg_push(sl,
              (rstr_segment_t){
                  .kind       = is_close ? SEG_STYLE_OFF : SEG_STYLE_ON,
                  .field_name = tag_name,
              });
}

/**
 * @brief Parse a rich markup string, producing a segment list and
 *        stripped text buffer.
 *
 * The input is the raw content between quotes of the string literal
 * (C string escapes have already been processed by the preprocessor,
 * but guillemet-delimited tags are intact).
 */
static void
parse_rich_markup(const char *desc, int desc_len, rstr_seg_list_t *sl,
                  rstr_text_buf_t *tb, const char *file, int line)
{
    int i          = 0;
    int text_start = 0;

    while (i < desc_len) {
        // Escape: backslash
        if (desc[i] == '\\' && i + 1 < desc_len) {
            char next = desc[i + 1];
            // \xC2\xAB = « , \xC2\xBB = »
            if ((unsigned char)next == 0xC2 && i + 2 < desc_len) {
                unsigned char after = (unsigned char)desc[i + 2];
                if (after == 0xAB || after == 0xBB) {
                    // Flush text before backslash
                    if (i > text_start) {
                        int off = tb->len;
                        rstr_text_append(tb, desc + text_start, i - text_start);
                        rseg_push(sl,
                                  (rstr_segment_t){
                                      .kind        = SEG_TEXT,
                                      .text_offset = off,
                                      .text_length = i - text_start,
                                  });
                    }
                    // Emit the escaped guillemet
                    int off = tb->len;
                    rstr_text_append(tb, desc + i + 1, 2);
                    rseg_push(sl,
                              (rstr_segment_t){
                                  .kind        = SEG_TEXT,
                                  .text_offset = off,
                                  .text_length = 2,
                              });
                    i += 3;
                    text_start = i;
                    continue;
                }
            }
            // Normal C escapes: pass through as-is (they're part of the text)
            i += 2;
            continue;
        }

        // Guillemet tag: « ... »
        // « is U+00AB = C2 AB in UTF-8
        // » is U+00BB = C2 BB in UTF-8
        if (i + 1 < desc_len && (unsigned char)desc[i] == 0xC2
            && (unsigned char)desc[i + 1] == 0xAB) {
            // Flush text before tag
            if (i > text_start) {
                int off = tb->len;
                rstr_text_append(tb, desc + text_start, i - text_start);
                rseg_push(sl,
                          (rstr_segment_t){
                              .kind        = SEG_TEXT,
                              .text_offset = off,
                              .text_length = i - text_start,
                          });
            }
            int tag_start = i + 2;
            int j         = tag_start;
            // Find closing »
            while (j + 1 < desc_len
                   && !((unsigned char)desc[j] == 0xC2
                        && (unsigned char)desc[j + 1] == 0xBB)) {
                j++;
            }
            if (j + 1 < desc_len) {
                classify_tag(sl, desc + tag_start, j - tag_start, file, line);
                i          = j + 2;
                text_start = i;
            }
            else {
                // No closing » — treat as literal text
                i += 2;
            }
            continue;
        }

        // Bracket tag: [| ... |]
        if (desc[i] == '[' && i + 1 < desc_len && desc[i + 1] == '|') {
            if (i > text_start) {
                int off = tb->len;
                rstr_text_append(tb, desc + text_start, i - text_start);
                rseg_push(sl,
                          (rstr_segment_t){
                              .kind        = SEG_TEXT,
                              .text_offset = off,
                              .text_length = i - text_start,
                          });
            }
            int tag_start = i + 2;
            int j         = tag_start;
            while (j + 1 < desc_len && !(desc[j] == '|' && desc[j + 1] == ']')) {
                j++;
            }
            if (j + 1 < desc_len) {
                classify_tag(sl, desc + tag_start, j - tag_start, file, line);
                i          = j + 2;
                text_start = i;
            }
            else {
                i += 2;
            }
            continue;
        }

        i++;
    }

    // Trailing text
    if (i > text_start) {
        int off = tb->len;
        rstr_text_append(tb, desc + text_start, i - text_start);
        rseg_push(sl,
                  (rstr_segment_t){
                      .kind        = SEG_TEXT,
                      .text_offset = off,
                      .text_length = i - text_start,
                  });
    }
}

// =========================================================================
// Style record builder
// =========================================================================

// A pending style that has been opened but not yet closed.
typedef struct {
    enum {
        PSTYLE_PROP,   // inline property (tristate field)
        PSTYLE_CASE,   // text case
        PSTYLE_NAMED,  // named style (deferred)
        PSTYLE_ROLE,   // role (deferred)
    } kind;
    const char *field_name; // prop field name, or tag name
    int         case_val;   // for PSTYLE_CASE
    int         start_byte; // byte offset in stripped text where it opened
} pending_style_t;

typedef struct {
    pending_style_t *items;
    int              count;
    int              cap;
} pending_stack_t;

static void
pending_push(pending_stack_t *ps, pending_style_t item)
{
    if (ps->count >= ps->cap) {
        int new_cap         = ps->cap ? ps->cap * 2 : 16;
        pending_style_t *np = base_alloc(new_cap * sizeof(pending_style_t));
        if (ps->items) {
            memcpy(np, ps->items, ps->count * sizeof(pending_style_t));
            base_dealloc(ps->items);
        }
        ps->items = np;
        ps->cap   = new_cap;
    }
    ps->items[ps->count++] = item;
}

// Output style record: represents a final (start, end, style_or_tag) tuple.
typedef struct {
    int         start;      // byte offset in stripped text
    int         end;        // byte offset (exclusive), or -1 for open-ended
    const char *field_name; // for prop: field name; for named/role: tag
    int         kind;       // same as pending_style_t.kind
    int         case_val;   // for case
} out_style_t;

typedef struct {
    out_style_t *items;
    int          count;
    int          cap;
} out_style_list_t;

static void
out_push(out_style_list_t *ol, out_style_t item)
{
    if (ol->count >= ol->cap) {
        int new_cap     = ol->cap ? ol->cap * 2 : 16;
        out_style_t *np = base_alloc(new_cap * sizeof(out_style_t));
        if (ol->items) {
            memcpy(np, ol->items, ol->count * sizeof(out_style_t));
            base_dealloc(ol->items);
        }
        ol->items = np;
        ol->cap   = new_cap;
    }
    ol->items[ol->count++] = item;
}

/**
 * @brief Walk segments and produce output style records.
 *
 * Each open tag records a start offset; closing tags finalize the record.
 * Reset clears all pending styles. Unmatched opens extend to end-of-string.
 */
static void
build_style_records(rstr_seg_list_t *sl, out_style_list_t *out)
{
    pending_stack_t ps = {0};
    int             text_pos = 0; // current byte position in stripped text

    for (int i = 0; i < sl->count; i++) {
        rstr_segment_t *seg = &sl->segs[i];

        switch (seg->kind) {
        case SEG_TEXT:
            text_pos = seg->text_offset + seg->text_length;
            break;

        case SEG_PROP_ON:
            pending_push(&ps,
                         (pending_style_t){
                             .kind       = PSTYLE_PROP,
                             .field_name = seg->field_name,
                             .start_byte = text_pos,
                         });
            break;

        case SEG_PROP_OFF:
            for (int j = ps.count - 1; j >= 0; j--) {
                if (ps.items[j].kind == PSTYLE_PROP
                    && strcmp(ps.items[j].field_name, seg->field_name) == 0) {
                    out_push(out,
                             (out_style_t){
                                 .start      = ps.items[j].start_byte,
                                 .end        = text_pos,
                                 .field_name = ps.items[j].field_name,
                                 .kind       = PSTYLE_PROP,
                             });
                    for (int k = j; k < ps.count - 1; k++) {
                        ps.items[k] = ps.items[k + 1];
                    }
                    ps.count--;
                    break;
                }
            }
            break;

        case SEG_CASE_ON:
            pending_push(&ps,
                         (pending_style_t){
                             .kind       = PSTYLE_CASE,
                             .case_val   = seg->case_val,
                             .start_byte = text_pos,
                         });
            break;

        case SEG_CASE_OFF:
            for (int j = ps.count - 1; j >= 0; j--) {
                if (ps.items[j].kind == PSTYLE_CASE) {
                    out_push(out,
                             (out_style_t){
                                 .start    = ps.items[j].start_byte,
                                 .end      = text_pos,
                                 .kind     = PSTYLE_CASE,
                                 .case_val = ps.items[j].case_val,
                             });
                    for (int k = j; k < ps.count - 1; k++) {
                        ps.items[k] = ps.items[k + 1];
                    }
                    ps.count--;
                    break;
                }
            }
            break;

        case SEG_STYLE_ON:
            pending_push(&ps,
                         (pending_style_t){
                             .kind       = PSTYLE_NAMED,
                             .field_name = seg->field_name,
                             .start_byte = text_pos,
                         });
            break;

        case SEG_STYLE_OFF:
            for (int j = ps.count - 1; j >= 0; j--) {
                if (ps.items[j].kind == PSTYLE_NAMED
                    && strcmp(ps.items[j].field_name, seg->field_name) == 0) {
                    out_push(out,
                             (out_style_t){
                                 .start      = ps.items[j].start_byte,
                                 .end        = text_pos,
                                 .field_name = ps.items[j].field_name,
                                 .kind       = PSTYLE_NAMED,
                             });
                    for (int k = j; k < ps.count - 1; k++) {
                        ps.items[k] = ps.items[k + 1];
                    }
                    ps.count--;
                    break;
                }
            }
            break;

        case SEG_ROLE_ON:
            pending_push(&ps,
                         (pending_style_t){
                             .kind       = PSTYLE_ROLE,
                             .field_name = seg->field_name,
                             .start_byte = text_pos,
                         });
            break;

        case SEG_ROLE_OFF:
            for (int j = ps.count - 1; j >= 0; j--) {
                if (ps.items[j].kind == PSTYLE_ROLE
                    && strcmp(ps.items[j].field_name, seg->field_name) == 0) {
                    out_push(out,
                             (out_style_t){
                                 .start      = ps.items[j].start_byte,
                                 .end        = text_pos,
                                 .field_name = ps.items[j].field_name,
                                 .kind       = PSTYLE_ROLE,
                             });
                    for (int k = j; k < ps.count - 1; k++) {
                        ps.items[k] = ps.items[k + 1];
                    }
                    ps.count--;
                    break;
                }
            }
            break;

        case SEG_RESET:
            // Close all pending styles at current position
            for (int j = 0; j < ps.count; j++) {
                out_push(out,
                         (out_style_t){
                             .start      = ps.items[j].start_byte,
                             .end        = text_pos,
                             .field_name = ps.items[j].field_name,
                             .kind       = ps.items[j].kind,
                             .case_val   = ps.items[j].case_val,
                         });
            }
            ps.count = 0;
            break;
        }
    }

    // Close any remaining open styles (extend to end of string)
    for (int j = 0; j < ps.count; j++) {
        out_push(out,
                 (out_style_t){
                     .start      = ps.items[j].start_byte,
                     .end        = -1, // open-ended
                     .field_name = ps.items[j].field_name,
                     .kind       = ps.items[j].kind,
                     .case_val   = ps.items[j].case_val,
                 });
    }

    if (ps.items) {
        base_dealloc(ps.items);
    }
}

// =========================================================================
// Code emitter: produce the replacement C expression
// =========================================================================

// Append a C string literal's contents (escaped for inclusion in a string).
static void
emit_escaped_string(FILE *f, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\0': fputs("\\0", f);  break;
        default:
            if (c < 0x20 || c == 0x7f) {
                fprintf(f, "\\x%02x", c);
            }
            else {
                fputc(c, f);
            }
            break;
        }
    }
}

// Monotonic counter for unique variable names in statement expressions.
static int rstr_counter = 0;

// Emit one style record's `.info` initializer as a static variable
// declaration, writing the variable name to `var_name_out`.
static void
emit_style_var(FILE *f, out_style_t *os, int idx, int uid)
{
    if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
        // Deferred: info = NULL, tag = "name"
        // No separate style variable needed — info is NULL.
    }
    else if (os->kind == PSTYLE_CASE) {
        fprintf(f, "static n00b_text_style_t _ncc_rs_%d_ts_%d="
                   "{.text_case=%d};",
                uid, idx, os->case_val);
    }
    else {
        // Inline prop
        fprintf(f, "static n00b_text_style_t _ncc_rs_%d_ts_%d="
                   "{.%s=2};", // N00B_TRI_YES==2
                uid, idx, os->field_name);
    }
}

/**
 * @brief Generate the full statement expression for a rich string.
 *
 * Uses a GCC statement expression `({...})` with local static variables
 * to avoid reliance on C23 static compound literals.
 *
 * @param tb     Stripped text buffer
 * @param out    Output style records
 * @return Dynamically allocated C expression string
 */
static char *
emit_rstr_expression(rstr_text_buf_t *tb, out_style_list_t *out)
{
    char  *result = NULL;
    size_t size;
    FILE  *f = open_memstream(&result, &size);

    int     uid      = rstr_counter++;
    int64_t cp_count = count_utf8_codepoints(tb->data, tb->len);

    // Statement expression: ({ static declarations...; &_ncc_rs_N; })
    fprintf(f, "({");

    if (out->count > 0) {
        // Emit static text style variables for inline props/case
        for (int i = 0; i < out->count; i++) {
            emit_style_var(f, &out->items[i], i, uid);
        }

        // Emit the styling info struct (with fixed-size array)
        fprintf(f, "static struct{"
                   "int64_t num_styles;"
                   "n00b_text_style_t*base_style;"
                   "n00b_style_record_t styles[%d];"
                   "}_ncc_rs_%d_si={",
                out->count, uid);
        fprintf(f, ".num_styles=%d,.base_style=((n00b_text_style_t*)0),",
                out->count);
        fprintf(f, ".styles={");

        for (int i = 0; i < out->count; i++) {
            if (i > 0) {
                fputc(',', f);
            }
            out_style_t *os = &out->items[i];
            fprintf(f, "{");

            // .info and .tag
            if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
                // field_name already includes '@' for roles
                fprintf(f, ".info=((n00b_text_style_t*)0),.tag=\"");
                fputs(os->field_name, f);
                fprintf(f, "\"");
            }
            else {
                fprintf(f, ".info=&_ncc_rs_%d_ts_%d,.tag=((void*)0)",
                        uid, i);
            }

            // .start
            fprintf(f, ",.start=%d", os->start);

            // .end
            if (os->end >= 0) {
                fprintf(f, ",.end={.has_value=1,.value=(size_t)%d}", os->end);
            }
            else {
                fprintf(f, ",.end={.has_value=0}");
            }
            fprintf(f, "}");
        }

        fprintf(f, "}};");
    }

    // Emit the string struct
    fprintf(f, "static struct n00b_string_t _ncc_rs_%d={"
               ".u8_bytes=%d,.data=\"",
            uid, tb->len);
    emit_escaped_string(f, tb->data, tb->len);
    fprintf(f, "\",.codepoints=%lld,", (long long)cp_count);

    if (out->count == 0) {
        fprintf(f, ".styling=((void*)0)");
    }
    else {
        fprintf(f, ".styling=&_ncc_rs_%d_si", uid);
    }

    fprintf(f, "};");

    // Yield address of the static string
    fprintf(f, "&_ncc_rs_%d;})", uid);

    fclose(f);
    return result;
}

// =========================================================================
// String literal extraction
// =========================================================================

/**
 * @brief Extract the content of a C string literal from its token text.
 *
 * Given a token whose text is `"hello «b»world«/b»"`, returns
 * `hello «b»world«/b»` (stripping the outer quotes).
 *
 * Also handles adjacent string concatenation by concatenating the
 * contents of multiple adjacent string tokens.
 */
static char *
extract_string_content(tree_xform_t *ctx, tnode_t *arglist, int *out_len)
{
    // The argument to __ncc_rstr() is one or more string literal tokens.
    // In the parse tree, they appear as the argument_expression_list's
    // children or as a single string_literal/primary_expression.
    //
    // We collect all string tokens from the arglist subtree.

    // Use a simple DFS to find all TT_STR tokens.
    char  *result = NULL;
    size_t size;
    FILE  *f = open_memstream(&result, &size);

    typedef struct {
        tnode_t **items;
        int       count;
        int       cap;
    } node_stack_t;

    node_stack_t stack = {0};

    // Push initial node
    if (stack.cap == 0) {
        stack.cap   = 32;
        stack.items = base_alloc(stack.cap * sizeof(tnode_t *));
    }
    stack.items[stack.count++] = arglist;

    while (stack.count > 0) {
        tnode_t *node = stack.items[--stack.count];
        if (!node) {
            continue;
        }
        if (node->tptr && node->tptr->type == TT_STR) {
            // Extract string content (strip quotes)
            int         tlen;
            const char *text = tok_text_ptr(ctx->input, node->tptr, &tlen);
            if (tlen >= 2 && text[0] == '"' && text[tlen - 1] == '"') {
                fwrite(text + 1, 1, tlen - 2, f);
            }
        }
        // Push children in reverse for in-order traversal
        for (int i = node->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(node, i);
            if (!kid) {
                continue;
            }
            if (stack.count >= stack.cap) {
                stack.cap *= 2;
                stack.items = base_realloc(stack.items,
                                           stack.cap * sizeof(tnode_t *));
            }
            stack.items[stack.count++] = kid;
        }
    }

    if (stack.items) {
        base_dealloc(stack.items);
    }

    fclose(f);
    *out_len = (int)size;
    return result;
}

// =========================================================================
// Find callee name (shared pattern with constexpr)
// =========================================================================

static tok_t *
rstr_find_id_tok(tnode_t *node)
{
    if (!node) {
        return NULL;
    }
    if (node->tptr) {
        return node->tptr;
    }
    if (node->nt_id == NT_postfix_expression
        && node->branch != BRANCH(postfix_expression, PRIMARY)) {
        return NULL;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tok_t *tok = rstr_find_id_tok(tnode_get_kid(node, i));
        if (tok) {
            return tok;
        }
    }
    return NULL;
}

static char *
rstr_get_callee_name(tree_xform_t *ctx, tnode_t *node)
{
    tnode_t *callee = tnode_get_kid(node, 0);
    if (!callee) {
        return NULL;
    }
    tok_t *tok = rstr_find_id_tok(callee);
    if (!tok) {
        return NULL;
    }
    return extract(ctx->input, tok);
}

// =========================================================================
// Main transform
// =========================================================================

static tnode_t *
xform_rstr(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(postfix_expression, CALL)) {
        return NULL;
    }

    char *name = rstr_get_callee_name(ctx, node);
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "__ncc_rstr") != 0) {
        base_dealloc(name);
        return NULL;
    }
    base_dealloc(name);

    int         line = get_node_line(node);
    const char *file = ctx->lex ? ctx->lex->in_file : "<unknown>";

    // Get the argument subtree.
    // CALL: kid[0]=callee, kid[1]="(", kid[2]=arglist or ")", kid[3]=")"
    tnode_t *kid2 = tnode_get_kid(node, 2);
    if (!kid2) {
        ncc_error("%s:%d: __ncc_rstr() requires a string argument\n",
                  file, line);
        exit(1);
    }

    // kid2 might be the argument_expression_list or just ")"
    tnode_t *arglist = kid2;
    if (kid2->tptr && kid2->tptr->type == TT_PUNCT) {
        // No arguments — just ")"
        ncc_error("%s:%d: __ncc_rstr() requires a string argument\n",
                  file, line);
        exit(1);
    }

    // Extract string content from the argument
    int   content_len;
    char *content = extract_string_content(ctx, arglist, &content_len);
    if (!content || content_len == 0) {
        ncc_error("%s:%d: __ncc_rstr() argument must be a string literal\n",
                  file, line);
        exit(1);
    }

    // Parse rich markup
    rstr_seg_list_t sl = {0};
    rstr_text_buf_t tb = {0};
    parse_rich_markup(content, content_len, &sl, &tb, file, line);

    // Build style records
    out_style_list_t out = {0};
    build_style_records(&sl, &out);

    // Generate the compound literal expression
    char *expr = emit_rstr_expression(&tb, &out);

    // Build replacement node: a primary_expression with a synthetic token
    // containing the entire generated expression.
    tnode_t *replacement = synth_nonterminal("postfix_expression_9");
    replacement->nt_id   = NT_postfix_expression;
    replacement->branch  = BRANCH(postfix_expression, PRIMARY);

    tnode_t *primary = synth_nonterminal("primary_expression_1");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 1;

    // Create a synthetic terminal with the entire expression as replacement text.
    // synth_terminal() internally creates its own token.
    tnode_t *leaf = synth_terminal(expr, TT_ID, line);

    add_child(primary, leaf);
    add_child(replacement, primary);

    // Cleanup
    base_dealloc(content);
    base_dealloc(expr);
    if (sl.segs) {
        base_dealloc(sl.segs);
    }
    if (tb.data) {
        base_dealloc(tb.data);
    }
    if (out.items) {
        base_dealloc(out.items);
    }

    replace_node(node, replacement, "rstr");
    return replacement;
}

// =========================================================================
// Registration
// =========================================================================

void
register_rstr_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_postfix_expression, xform_rstr, "rstr");
}
