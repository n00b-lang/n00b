/** @file src/chalk/source.c — source-code codec.
 *
 *  Ports chalk/plugins/codecSource.nim. The mark is a single-line
 *  comment at the end of the file using the language's comment
 *  syntax. Language detection: shebang line (`#!/usr/bin/env <name>`)
 *  takes precedence, else file extension. The supported language
 *  table lives in lang_table.c.
 *
 *  When a mark is removed from a file that had it on its own comment
 *  line ending with "\\n# " (or other comment prefix + space), the
 *  whole line is excised. Otherwise the mark is replaced with the
 *  canonical empty placeholder `{ "MAGIC" : "dadfedabbadabbed" }` —
 *  this keeps the hash invariant under remarking. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/sha256.h"
#include "core/alloc.h"
#include "parsers/json.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/mark_internal.h"
#include "internal/chalk/lang_table.h"
#include "internal/chalk/sidecar_internal.h"
#include "internal/chalk/file_io.h"

#include <string.h>

#define MAGIC          N00B_CHALK_MAGIC_STRING
#define MAGIC_LEN      16
#define EMPTY_MARK     "{ \"MAGIC\" : \"" N00B_CHALK_MAGIC_STRING "\" }"
#define EMPTY_MARK_LEN 33

// -----------------------------------------------------------------------
// Language detection
// -----------------------------------------------------------------------

// Parse the basename of the interpreter from a shebang line. Accepts
// "#!/usr/bin/env python" → "python", "#!/usr/bin/python3" → "python3"
// (which then needs an extra step — we only return the chunk after
// the last '/' or " ").
static const char *
detect_lang_from_shebang(const char *data, size_t len)
{
    if (len < 2 || data[0] != '#' || data[1] != '!') return nullptr;

    // Find end of shebang line.
    size_t i = 2;
    while (i < len && data[i] != '\n' && data[i] != '\r') i++;
    size_t line_end = i;

    // Find the LAST token on the line.
    size_t last_start = 2;
    for (size_t j = 2; j < line_end; j++) {
        if (data[j] == ' ' || data[j] == '\t' || data[j] == '/') {
            last_start = j + 1;
        }
    }
    if (last_start >= line_end) return nullptr;

    // Strip trailing version digits ("python3" → "python").
    size_t name_end = line_end;
    while (name_end > last_start && data[name_end - 1] >= '0'
           && data[name_end - 1] <= '9') {
        name_end--;
    }
    if (name_end == last_start) return nullptr;

    return n00b_chalk_lang_lookup_ext(data + last_start,
                                       name_end - last_start);
}

static const char *
extension_from_path(n00b_string_t *path, size_t *out_len)
{
    if (!path || !path->data) return nullptr;
    const char *d = path->data;
    size_t      n = path->u8_bytes;
    for (size_t i = n; i > 0; i--) {
        if (d[i - 1] == '.') {
            *out_len = n - i;
            return d + i;
        }
        if (d[i - 1] == '/') break;
    }
    return nullptr;
}

// Find a chalk mark within the buffer: locate MAGIC, walk back to
// the opening `{`, walk forward to the matching `}`. Returns true and
// fills *out_start / *out_end if found.
static bool
find_mark(const char *data, size_t len, size_t *out_start, size_t *out_end)
{
    if (len < MAGIC_LEN) return false;
    int64_t magic = -1;
    for (size_t i = 0; i + MAGIC_LEN <= len; i++) {
        if (memcmp(data + i, MAGIC, MAGIC_LEN) == 0) {
            magic = (int64_t)i;
            break;
        }
    }
    if (magic < 0) return false;

    // Walk back for the opening `{`.
    int64_t bs = -1;
    for (int64_t i = magic; i >= 0; i--) {
        if (data[i] == '{') { bs = i; break; }
    }
    if (bs < 0) return false;

    // Walk forward for the matching `}`, string-aware.
    int  depth  = 0;
    bool in_str = false;
    bool escape = false;
    for (size_t i = (size_t)bs; i < len; i++) {
        char c = data[i];
        if (in_str) {
            if (escape) { escape = false; }
            else if (c == '\\') { escape = true; }
            else if (c == '"') { in_str = false; }
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_start = (size_t)bs;
                *out_end   = i + 1;
                return true;
            }
        }
    }
    return false;
}

// -----------------------------------------------------------------------
// "Unmarked" content production (matches getUnmarkedScriptContent in
// chalk/plugins/codecSource.nim).
//
// If we find a mark at [cs, r) preceded by "\n<comment> " AND
// terminated by '\n' or EOF, excise the WHOLE line. Else replace the
// mark JSON with EMPTY_MARK (the placeholder).
// -----------------------------------------------------------------------

static n00b_buffer_t *
unmarked_content(const char *data, size_t len, const char *comment)
{
    size_t cs, r;
    if (!find_mark(data, len, &cs, &r)) {
        // No mark — input is the output.
        return n00b_buffer_from_bytes((char *)data, (int64_t)len);
    }

    size_t comment_len   = strlen(comment);
    size_t base_prefix   = 2;            // "\n" + " "
    size_t pre_excise    = base_prefix + comment_len;
    bool   add_empty     = true;

    if (cs >= pre_excise) {
        // Check "\n<comment> "
        bool ok = data[cs - pre_excise] == '\n'
               && memcmp(data + cs - pre_excise + 1, comment, comment_len) == 0
               && data[cs - 1] == ' ';
        bool trail_ok = (r == len) || (data[r] == '\n');
        if (ok && trail_ok) {
            add_empty = false;
        }
    }

    if (!add_empty) {
        size_t out_start_end = cs - pre_excise;
        size_t out_total     = out_start_end + (len - r);
        char  *out           = n00b_alloc_array(char, out_total > 0 ? out_total : 1);
        memcpy(out, data, out_start_end);
        if (len > r) memcpy(out + out_start_end, data + r, len - r);
        return n00b_buffer_from_bytes(out, (int64_t)out_total);
    }
    else {
        size_t out_total = cs + EMPTY_MARK_LEN + (len - r);
        char  *out       = n00b_alloc_array(char, out_total);
        memcpy(out, data, cs);
        memcpy(out + cs, EMPTY_MARK, EMPTY_MARK_LEN);
        if (len > r) memcpy(out + cs + EMPTY_MARK_LEN, data + r, len - r);
        return n00b_buffer_from_bytes(out, (int64_t)out_total);
    }
}

// -----------------------------------------------------------------------
// Marked content production (mirrors getMarkedScriptContents).
// -----------------------------------------------------------------------

static n00b_buffer_t *
marked_content(const char *data, size_t len,
               const char *comment, const char *mark_json, size_t mark_len)
{
    size_t cs, r;
    if (find_mark(data, len, &cs, &r)) {
        // Replace existing mark in place.
        size_t out_total = cs + mark_len + (len - r);
        char  *out       = n00b_alloc_array(char, out_total);
        memcpy(out, data, cs);
        memcpy(out + cs, mark_json, mark_len);
        if (len > r) memcpy(out + cs + mark_len, data + r, len - r);
        return n00b_buffer_from_bytes(out, (int64_t)out_total);
    }

    // No mark — append.
    bool   trailing_nl = (len > 0 && data[len - 1] == '\n');
    size_t clen        = strlen(comment);

    // trailing newline preserved: file + "<comment> <mark>\n"
    // no trailing newline:        file + "\n<comment> <mark>"
    size_t extra = clen + 1 + mark_len + 1; // comment + " " + mark + "\n"
    if (!trailing_nl) extra++;              // leading "\n"
    size_t out_total = len + extra;
    char  *out       = n00b_alloc_array(char, out_total);
    size_t op        = 0;
    memcpy(out + op, data, len); op += len;
    if (!trailing_nl) out[op++] = '\n';
    memcpy(out + op, comment, clen); op += clen;
    out[op++] = ' ';
    memcpy(out + op, mark_json, mark_len); op += mark_len;
    if (trailing_nl) out[op++] = '\n';
    return n00b_buffer_from_bytes(out, (int64_t)op);
}

// -----------------------------------------------------------------------
// Comment selection: use shebang's interpreter if present, else NULL
// (caller falls back to extension lookup).
// -----------------------------------------------------------------------

static const char *
pick_comment(n00b_buffer_t *bytes, n00b_string_t *path_hint)
{
    const char *lang = detect_lang_from_shebang(bytes->data, bytes->byte_len);
    if (!lang && path_hint) {
        size_t exlen = 0;
        const char *ext = extension_from_path(path_hint, &exlen);
        if (ext) lang = n00b_chalk_lang_lookup_ext(ext, exlen);
    }
    if (!lang) return nullptr;
    return n00b_chalk_lang_lookup_comment(lang);
}

// -----------------------------------------------------------------------
// SHA-256 of the unmarked content.
// -----------------------------------------------------------------------

static n00b_buffer_t *
sha256_of_buffer(n00b_buffer_t *b)
{
    return n00b_chalk_sha256_buffer(b);
}

// -----------------------------------------------------------------------
// Codec entry points
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_source_hash_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_buffer_t *, 1);
    const char *comment = pick_comment(bytes, nullptr);
    if (!comment) comment = "#";  // fallback
    n00b_buffer_t *unmarked = unmarked_content(bytes->data, bytes->byte_len,
                                                comment);
    return n00b_result_ok(n00b_buffer_t *, sha256_of_buffer(unmarked));
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_source_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    if (!bytes || !mark) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    const char *comment = pick_comment(bytes, nullptr);
    if (!comment) comment = "#";

    n00b_buffer_t *unmarked = unmarked_content(bytes->data, bytes->byte_len,
                                                comment);
    n00b_buffer_t *hash_buf = sha256_of_buffer(unmarked);
    auto fin = n00b_chalk_mark_finalize(mark, hash_buf);
    if (n00b_result_is_err(fin)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 2);
    }
    n00b_buffer_t *encoded = n00b_result_get(fin);

    n00b_buffer_t *out = marked_content(bytes->data, bytes->byte_len,
                                         comment, encoded->data,
                                         (size_t)encoded->byte_len);

    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = out;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_source_delete_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_io_result_t *, 1);
    const char *comment = pick_comment(bytes, nullptr);
    if (!comment) comment = "#";
    n00b_buffer_t *unmarked = unmarked_content(bytes->data, bytes->byte_len,
                                                comment);
    auto r = (n00b_chalk_io_result_t *)n00b_alloc(n00b_chalk_io_result_t);
    r->kind           = N00B_CHALK_OUT_IN_BAND;
    r->bytes          = unmarked;
    r->sidecar_suffix = nullptr;
    return n00b_result_ok(n00b_chalk_io_result_t *, r);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_source_extract_buffer(n00b_buffer_t *bytes)
{
    if (!bytes) return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    size_t cs, r;
    if (!find_mark(bytes->data, bytes->byte_len, &cs, &r)) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 2);
    }
    auto payload_buf = n00b_buffer_from_bytes(bytes->data + cs,
                                               (int64_t)(r - cs));
    return n00b_chalk_sidecar_parse_bytes(payload_buf,
                                          N00B_CHALK_CODEC_SOURCE);
}

// File-mode entry points use the standard helpers.
n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_source_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark,
                                      n00b_chalk_source_insert_buffer);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_source_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_source_delete_buffer);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_source_extract_file(n00b_string_t *path)
{
    return n00b_chalk_file_extract_via(path, n00b_chalk_source_extract_buffer);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_source_hash_file(n00b_string_t *path)
{
    return n00b_chalk_file_hash_via(path, n00b_chalk_source_hash_buffer);
}
