// xform_rstr.c — Transform: r"..." rich string literals.
//
// Matches __ncc_rstr("...") calls (produced by the prescan in ncc.c) and
// replaces them with static compound literals containing pre-compiled
// styling data.
//
// Registered as post-order on "postfix_expression".

#include "slay/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// Rich markup property / case tables
// =========================================================================

static const struct {
    const char *name;
    const char *field_name;
} prop_table[] = {
    {"b",             "bold"            },
    {"bold",          "bold"            },
    {"i",             "italic"          },
    {"italic",        "italic"          },
    {"u",             "underline"       },
    {"underline",     "underline"       },
    {"uu",            "double_underline"},
    {"2u",            "double_underline"},
    {"st",            "strikethrough"   },
    {"strike",        "strikethrough"   },
    {"strikethrough", "strikethrough"   },
    {"r",             "reverse"         },
    {"reverse",       "reverse"         },
    {"dim",           "dim"             },
    {"faint",         "dim"             },
    {"blink",         "blink"           },
    {NULL,            NULL              },
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
    {"caps",    RSTR_CASE_CAPS },
    {"allcaps", RSTR_CASE_CAPS },
    {"t",       RSTR_CASE_TITLE},
    {"title",   RSTR_CASE_TITLE},
    {NULL,      0              },
};

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
    int         text_offset;
    int         text_length;
    const char *field_name;
    int         case_val;
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
        int             new_cap = sl->cap ? sl->cap * 2 : 16;
        rstr_segment_t *new_s   = malloc((size_t)new_cap * sizeof(rstr_segment_t));

        if (sl->segs) {
            memcpy(new_s, sl->segs, (size_t)sl->count * sizeof(rstr_segment_t));
            free(sl->segs);
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

        char *new_data = malloc((size_t)new_cap);

        if (tb->data) {
            memcpy(new_data, tb->data, (size_t)tb->len);
            free(tb->data);
        }

        tb->data = new_data;
        tb->cap  = new_cap;
    }

    memcpy(tb->data + tb->len, src, (size_t)src_len);
    tb->len += src_len;
}

// =========================================================================
// Markup parser
// =========================================================================

static void
classify_tag(rstr_seg_list_t *sl, const char *tag_body, int tag_len)
{
    if (tag_len == 0) {
        return;
    }

    // Reset: "/" alone.
    if (tag_len == 1 && tag_body[0] == '/') {
        rseg_push(sl, (rstr_segment_t){.kind = SEG_RESET});
        return;
    }

    // Substitution: starts with '#' — compile error.
    if (tag_body[0] == '#') {
        fprintf(stderr,
                "ncc: error: r\"...\" string literals cannot contain "
                "substitutions (#)\n");
        exit(1);
    }

    bool        is_close = (tag_body[0] == '/');
    const char *name     = is_close ? tag_body + 1 : tag_body;
    int         name_len = is_close ? tag_len - 1 : tag_len;

    if (name_len == 0) {
        return;
    }

    // Role: starts with '@'.
    if (name[0] == '@') {
        char *tag_name = malloc((size_t)(name_len + 1));

        memcpy(tag_name, name, (size_t)name_len);
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

    // Named style (default).
    char *tag_name = malloc((size_t)(name_len + 1));

    memcpy(tag_name, name, (size_t)name_len);
    tag_name[name_len] = '\0';
    rseg_push(sl,
              (rstr_segment_t){
                  .kind       = is_close ? SEG_STYLE_OFF : SEG_STYLE_ON,
                  .field_name = tag_name,
              });
}

static void
parse_rich_markup(const char *desc, int desc_len, rstr_seg_list_t *sl,
                  rstr_text_buf_t *tb)
{
    int i          = 0;
    int text_start = 0;

    while (i < desc_len) {
        // Escape: backslash.
        if (desc[i] == '\\' && i + 1 < desc_len) {
            char next = desc[i + 1];

            // \xC2\xAB = «, \xC2\xBB = »
            if ((unsigned char)next == 0xC2 && i + 2 < desc_len) {
                unsigned char after = (unsigned char)desc[i + 2];

                if (after == 0xAB || after == 0xBB) {
                    // Flush text before backslash.
                    if (i > text_start) {
                        int off = tb->len;

                        rstr_text_append(tb, desc + text_start,
                                         i - text_start);
                        rseg_push(sl,
                                  (rstr_segment_t){
                                      .kind        = SEG_TEXT,
                                      .text_offset = off,
                                      .text_length = i - text_start,
                                  });
                    }

                    // Emit the escaped guillemet.
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

            // Normal C escapes: pass through as-is.
            i += 2;
            continue;
        }

        // Guillemet tag: « ... »
        if (i + 1 < desc_len && (unsigned char)desc[i] == 0xC2
            && (unsigned char)desc[i + 1] == 0xAB) {
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

            while (j + 1 < desc_len
                   && !((unsigned char)desc[j] == 0xC2
                        && (unsigned char)desc[j + 1] == 0xBB)) {
                j++;
            }

            if (j + 1 < desc_len) {
                classify_tag(sl, desc + tag_start, j - tag_start);
                i          = j + 2;
                text_start = i;
            }
            else {
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

            while (j + 1 < desc_len
                   && !(desc[j] == '|' && desc[j + 1] == ']')) {
                j++;
            }

            if (j + 1 < desc_len) {
                classify_tag(sl, desc + tag_start, j - tag_start);
                i          = j + 2;
                text_start = i;
            }
            else {
                fprintf(stderr,
                        "ncc: error: unclosed '[|' tag in r-string\n");
                exit(1);
            }
            continue;
        }

        i++;
    }

    // Trailing text.
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

typedef struct {
    enum {
        PSTYLE_PROP,
        PSTYLE_CASE,
        PSTYLE_NAMED,
        PSTYLE_ROLE,
    } kind;
    const char *field_name;
    int         case_val;
    int         start_byte;
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
        int              new_cap = ps->cap ? ps->cap * 2 : 16;
        pending_style_t *np      = malloc((size_t)new_cap
                                          * sizeof(pending_style_t));

        if (ps->items) {
            memcpy(np, ps->items,
                   (size_t)ps->count * sizeof(pending_style_t));
            free(ps->items);
        }

        ps->items = np;
        ps->cap   = new_cap;
    }

    ps->items[ps->count++] = item;
}

typedef struct {
    int         start;
    int         end;
    const char *field_name;
    int         kind;
    int         case_val;
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
        int          new_cap = ol->cap ? ol->cap * 2 : 16;
        out_style_t *np      = malloc((size_t)new_cap * sizeof(out_style_t));

        if (ol->items) {
            memcpy(np, ol->items,
                   (size_t)ol->count * sizeof(out_style_t));
            free(ol->items);
        }

        ol->items = np;
        ol->cap   = new_cap;
    }

    ol->items[ol->count++] = item;
}

static void
build_style_records(rstr_seg_list_t *sl, out_style_list_t *out)
{
    pending_stack_t ps       = {0};
    int             text_pos = 0;

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

    // Close any remaining open styles (extend to end of string).
    for (int j = 0; j < ps.count; j++) {
        out_push(out,
                 (out_style_t){
                     .start      = ps.items[j].start_byte,
                     .end        = -1,
                     .field_name = ps.items[j].field_name,
                     .kind       = ps.items[j].kind,
                     .case_val   = ps.items[j].case_val,
                 });
    }

    free(ps.items);
}

// =========================================================================
// Code emitter
// =========================================================================

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

static void
emit_style_var(FILE *f, out_style_t *os, int idx, int uid)
{
    if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
        // Deferred: info = NULL, tag = "name". No style variable needed.
    }
    else if (os->kind == PSTYLE_CASE) {
        fprintf(f,
                "static n00b_text_style_t _ncc_rs_%d_ts_%d="
                "{.text_case=%d};",
                uid, idx, os->case_val);
    }
    else {
        fprintf(f,
                "static n00b_text_style_t _ncc_rs_%d_ts_%d="
                "{.%s=2};",
                uid, idx, os->field_name);
    }
}

static char *
emit_rstr_expression(rstr_text_buf_t *tb, out_style_list_t *out, int uid)
{
    char  *result = NULL;
    size_t size;
    FILE  *f = open_memstream(&result, &size);

    int64_t cp_count = count_utf8_codepoints(tb->data, tb->len);

    fprintf(f, "({");

    if (out->count > 0) {
        for (int i = 0; i < out->count; i++) {
            emit_style_var(f, &out->items[i], i, uid);
        }

        fprintf(f,
                "static struct{"
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

            if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
                fprintf(f, ".info=((n00b_text_style_t*)0),.tag=\"");
                fputs(os->field_name, f);
                fprintf(f, "\"");
            }
            else {
                fprintf(f, ".info=&_ncc_rs_%d_ts_%d,.tag=((void*)0)",
                        uid, i);
            }

            fprintf(f, ",.start=%d", os->start);

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

    fprintf(f,
            "static struct n00b_string_t _ncc_rs_%d={"
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
    fprintf(f, "&_ncc_rs_%d;})", uid);

    fclose(f);
    return result;
}

// =========================================================================
// String literal extraction
// =========================================================================

static char *
extract_rstr_content(n00b_parse_tree_t *arglist, int *out_len)
{
    char  *result = NULL;
    size_t size;
    FILE  *f = open_memstream(&result, &size);

    // DFS to find all STRING token leaves.
    typedef struct {
        n00b_parse_tree_t **items;
        int                 count;
        int                 cap;
    } node_stack_t;

    node_stack_t stack = {0};

    stack.cap   = 32;
    stack.items = malloc((size_t)stack.cap * sizeof(n00b_parse_tree_t *));
    stack.items[stack.count++] = arglist;

    while (stack.count > 0) {
        n00b_parse_tree_t *node = stack.items[--stack.count];

        if (!node) {
            continue;
        }

        if (n00b_tree_is_leaf(node)) {
            const char *text = n00b_xform_leaf_text(node);

            if (!text) {
                continue;
            }

            size_t tlen = strlen(text);

            if (tlen >= 2 && text[0] == '"' && text[tlen - 1] == '"') {
                const char *p   = text + 1;
                const char *end = text + tlen - 1;

                while (p < end) {
                    if (*p == '\\' && p + 1 < end) {
                        p++;

                        switch (*p) {
                        case 'n':  fputc('\n', f); p++; break;
                        case 'r':  fputc('\r', f); p++; break;
                        case 't':  fputc('\t', f); p++; break;
                        case '0':  fputc('\0', f); p++; break;
                        case '\\': fputc('\\', f); p++; break;
                        case '"':  fputc('"', f);  p++; break;
                        case 'a':  fputc('\a', f); p++; break;
                        case 'b':  fputc('\b', f); p++; break;
                        case 'f':  fputc('\f', f); p++; break;
                        case 'v':  fputc('\v', f); p++; break;
                        case 'x': {
                            p++;
                            unsigned int val = 0;

                            for (int d = 0; d < 2 && p < end; d++, p++) {
                                unsigned char c = (unsigned char)*p;

                                if (c >= '0' && c <= '9') {
                                    val = val * 16 + (c - '0');
                                }
                                else if (c >= 'a' && c <= 'f') {
                                    val = val * 16 + (c - 'a' + 10);
                                }
                                else if (c >= 'A' && c <= 'F') {
                                    val = val * 16 + (c - 'A' + 10);
                                }
                                else {
                                    break;
                                }
                            }

                            fputc((char)val, f);
                            break;
                        }
                        default:
                            fputc('\\', f);
                            fputc(*p, f);
                            p++;
                            break;
                        }
                    }
                    else {
                        fputc(*p, f);
                        p++;
                    }
                }
            }
        }
        else {
            // Push children in reverse for in-order traversal.
            size_t nc = n00b_tree_num_children(node);

            for (int i = (int)nc - 1; i >= 0; i--) {
                n00b_parse_tree_t *kid = n00b_tree_child(node, (size_t)i);

                if (!kid) {
                    continue;
                }

                if (stack.count >= stack.cap) {
                    stack.cap *= 2;
                    stack.items = realloc(
                        stack.items,
                        (size_t)stack.cap * sizeof(n00b_parse_tree_t *));
                }

                stack.items[stack.count++] = kid;
            }
        }
    }

    free(stack.items);
    fclose(f);
    *out_len = (int)size;
    return result;
}

// =========================================================================
// Callee name helper
// =========================================================================

static const char *
rstr_get_callee_name(n00b_parse_tree_t *node)
{
    // child[0] is the callee.
    if (n00b_tree_num_children(node) == 0) {
        return NULL;
    }

    n00b_parse_tree_t *callee = n00b_tree_child(node, 0);

    if (!callee) {
        return NULL;
    }

    // Walk down to find the first leaf token.
    while (callee && !n00b_tree_is_leaf(callee)) {
        if (n00b_tree_num_children(callee) == 0) {
            return NULL;
        }
        callee = n00b_tree_child(callee, 0);
    }

    return n00b_xform_leaf_text(callee);
}

// =========================================================================
// Main transform
// =========================================================================

static n00b_parse_tree_t *
xform_rstr(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    // Match: postfix_expression that looks like a function call.
    // A call has children: callee ( arglist ) or callee ( )
    size_t nc = n00b_tree_num_children(node);

    if (nc < 3) {
        return NULL;
    }

    // Check that child[1] is "(".
    n00b_parse_tree_t *paren = n00b_tree_child(node, 1);

    if (!paren || !n00b_xform_leaf_text_eq(paren, "(")) {
        return NULL;
    }

    // Check callee is __ncc_rstr.
    const char *name = rstr_get_callee_name(node);

    if (!name || strcmp(name, "__ncc_rstr") != 0) {
        return NULL;
    }

    // Get the argument subtree.
    // CALL: kid[0]=callee, kid[1]="(", kid[2]=arglist or ")", kid[3]=")"
    n00b_parse_tree_t *kid2 = n00b_tree_child(node, 2);

    if (!kid2) {
        fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
        exit(1);
    }

    // kid2 might be the ")" if no arguments.
    if (n00b_xform_leaf_text_eq(kid2, ")")) {
        fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
        exit(1);
    }

    n00b_parse_tree_t *arglist = kid2;

    // Extract string content from the argument.
    int   content_len;
    char *content = extract_rstr_content(arglist, &content_len);

    if (!content) {
        fprintf(stderr,
                "ncc: error: __ncc_rstr() argument must be a string literal\n");
        exit(1);
    }

    // Parse rich markup.
    rstr_seg_list_t sl = {0};
    rstr_text_buf_t tb = {0};

    parse_rich_markup(content, content_len, &sl, &tb);

    // Build style records.
    out_style_list_t out = {0};

    build_style_records(&sl, &out);

    // Generate the compound literal expression.
    int   uid  = ctx->unique_id++;
    char *expr = emit_rstr_expression(&tb, &out, uid);

    // Parse as primary_expression (statement expression is a primary expr).
    n00b_result_t(n00b_parse_tree_ptr_t) r =
        n00b_xform_parse_template(ctx->grammar, "primary_expression",
                                  expr, NULL);

    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "ncc: error: failed to parse rstr expansion template\n");
        exit(1);
    }

    n00b_parse_tree_t *replacement = n00b_result_get(r);

    // Cleanup.
    free(content);
    free(expr);
    free(sl.segs);
    free(tb.data);
    free(out.items);

    return replacement;
}

// =========================================================================
// Registration
// =========================================================================

void
n00b_register_rstr_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "postfix_expression", xform_rstr, "rstr");
}
