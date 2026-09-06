/** @file src/hostmeta/util.c — string / JSON / file helpers shared by
 *        the hostmeta collectors.
 *
 *  These are the pieces chalk's plugins got from Nim's stdlib and
 *  `utils/`: `tryToLoadFile`, `splitLinesAnd`, `toLowerAscii().contains`,
 *  `strip(chars = {'/'})`, and `lookupByPath` over a JSON tree.
 */

#define N00B_USE_INTERNAL_API

#include "internal/hostmeta/hostmeta_internal.h"

#include "adt/dict.h"
#include "core/buffer.h"
#include "core/env.h"
#include "core/file.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "util/ascii_ci.h"
#include "util/path.h"

#include <string.h>

n00b_string_t *
n00b_hostmeta_str(const char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    return n00b_string_from_cstr(s);
}

n00b_string_t *
n00b_hostmeta_env(const char *name)
{
    if (name == nullptr) {
        return nullptr;
    }
    n00b_string_t *v = n00b_getenv(n00b_string_from_cstr(name));
    if (v == nullptr || v->u8_bytes == 0) {
        return nullptr;
    }
    return v;
}

n00b_string_t *
n00b_hostmeta_read_file(n00b_string_t *path)
{
    if (path == nullptr || path->u8_bytes == 0) {
        return nullptr;
    }

    n00b_string_t *canonical = n00b_path_canonical(path);
    if (canonical == nullptr) {
        return nullptr;
    }

    auto fr = n00b_file_open(canonical, .kind = N00B_FILE_KIND_MMAP);
    if (n00b_result_is_err(fr)) {
        return nullptr;
    }
    n00b_file_t *f  = n00b_result_get(fr);
    auto         br = n00b_file_as_buffer(f);
    n00b_file_close(f);
    if (n00b_result_is_err(br)) {
        return nullptr;
    }
    n00b_buffer_t *b = n00b_result_get(br);
    if (b == nullptr) {
        return nullptr;
    }
    return n00b_string_from_raw(b->data, (int64_t)b->byte_len);
}

n00b_list_t(n00b_string_t *) *
    n00b_hostmeta_split_lines(n00b_string_t *s)
{
    n00b_list_t(n00b_string_t *) *out = n00b_alloc(n00b_list_t(n00b_string_t *));
    *out = n00b_list_new(n00b_string_t *);

    if (s == nullptr || s->u8_bytes == 0) {
        return out;
    }

    n00b_array_t(n00b_string_t *) lines = n00b_unicode_str_split_lines(s);
    for (int64_t i = 0; i < (int64_t)lines.len; i++) {
        n00b_string_t *line = lines.data[i];
        if (line == nullptr) {
            continue;
        }
        n00b_string_t *trimmed = n00b_unicode_str_trim(line);
        if (trimmed == nullptr || trimmed->u8_bytes == 0) {
            continue;
        }
        n00b_list_push(*out, trimmed);
    }
    return out;
}

static char
ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool
n00b_hostmeta_icontains(n00b_string_t *haystack, const char *needle)
{
    if (haystack == nullptr || needle == nullptr) {
        return false;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return true;
    }
    if (haystack->u8_bytes < nlen) {
        return false;
    }
    size_t limit = haystack->u8_bytes - nlen;
    for (size_t i = 0; i <= limit; i++) {
        size_t j = 0;
        while (j < nlen
               && ascii_lower(haystack->data[i + j]) == ascii_lower(needle[j])) {
            j++;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

bool
n00b_hostmeta_istarts_with(n00b_string_t *s, const char *prefix)
{
    if (s == nullptr || prefix == nullptr) {
        return false;
    }
    size_t plen = strlen(prefix);
    if (s->u8_bytes < plen) {
        return false;
    }
    return n00b_ascii_ci_eq_n(s->data, prefix, plen);
}

n00b_string_t *
n00b_hostmeta_strip_slashes(n00b_string_t *s, bool leading, bool trailing)
{
    if (s == nullptr || s->u8_bytes == 0) {
        return s;
    }
    size_t start = 0;
    size_t end   = s->u8_bytes;
    if (leading) {
        while (start < end && s->data[start] == '/') {
            start++;
        }
    }
    if (trailing) {
        while (end > start && s->data[end - 1] == '/') {
            end--;
        }
    }
    if (start == 0 && end == s->u8_bytes) {
        return s;
    }
    return n00b_string_from_raw(s->data + start, (int64_t)(end - start));
}

n00b_string_t *
n00b_hostmeta_branch_to_ref(n00b_string_t *branch)
{
    if (branch == nullptr || branch->u8_bytes == 0) {
        return nullptr;
    }
    n00b_string_t *b = branch;

    // Jenkins reports the remote-tracking name; the remote prefix is
    // not part of the ref.
    if (n00b_unicode_str_starts_with(b, r"origin/")) {
        b = n00b_string_from_raw(b->data + 7, (int64_t)(b->u8_bytes - 7));
    }
    if (b->u8_bytes == 0) {
        return nullptr;
    }
    if (n00b_unicode_str_starts_with(b, r"refs/")) {
        return b;
    }
    return n00b_unicode_str_cat(r"refs/heads/", b);
}

// ----------------------------------------------------------------------
// JSON helpers
// ----------------------------------------------------------------------

n00b_json_node_t *
n00b_hostmeta_parse_json(n00b_string_t *body, n00b_string_t **err_out)
{
    if (err_out != nullptr) {
        *err_out = nullptr;
    }
    // A 200 with no body means "this attribute exists but is unset";
    // an empty object keeps that distinguishable from a parse failure.
    if (body == nullptr || body->u8_bytes == 0) {
        return n00b_json_object_new();
    }

    const char       *err  = nullptr;
    n00b_json_node_t *node = n00b_json_parse(body->data, body->u8_bytes, &err);
    if (node == nullptr && err_out != nullptr) {
        *err_out = n00b_hostmeta_str(err ? err : "invalid JSON");
    }
    return node;
}

n00b_json_node_t *
n00b_hostmeta_json_copy(n00b_json_node_t *node)
{
    if (node == nullptr) {
        return nullptr;
    }

    switch (n00b_json_type(node)) {
    case N00B_JSON_NULL:
        return n00b_json_null_new();
    case N00B_JSON_BOOL:
        return n00b_json_bool_new(n00b_json_as_bool(node));
    case N00B_JSON_INT:
        return n00b_json_int_new(n00b_json_as_i64(node));
    case N00B_JSON_DOUBLE:
        return n00b_json_double_new(n00b_json_as_f64(node));
    case N00B_JSON_STRING:
        return n00b_json_string_new_from_n00b(n00b_json_as_string(node));
    case N00B_JSON_ARRAY: {
        n00b_json_node_t *out = n00b_json_array_new();
        size_t            n   = n00b_json_array_len(node);
        for (size_t i = 0; i < n; i++) {
            n00b_json_array_push(out,
                                 n00b_hostmeta_json_copy(
                                     n00b_json_array_get(node, i)));
        }
        return out;
    }
    case N00B_JSON_OBJECT: {
        n00b_json_node_t   *out = n00b_json_object_new();
        n00b_json_object_t *obj = n00b_json_as_object(node);
        if (obj != nullptr) {
            n00b_dict_foreach(obj, key, child, {
                n00b_json_object_put_n00b(out,
                                          key,
                                          n00b_hostmeta_json_copy(child));
            });
        }
        return out;
    }
    }
    return nullptr;
}

n00b_json_node_t *
n00b_hostmeta_json_path(n00b_json_node_t *root, const char *dotted)
{
    if (root == nullptr || dotted == nullptr) {
        return nullptr;
    }

    n00b_json_node_t *cur   = root;
    const char       *start = dotted;

    while (*start != '\0') {
        const char *dot = strchr(start, '.');
        size_t      len = dot ? (size_t)(dot - start) : strlen(start);
        if (len == 0 || n00b_json_type(cur) != N00B_JSON_OBJECT) {
            return nullptr;
        }
        n00b_string_t *seg = n00b_string_from_raw(start, (int64_t)len);
        cur                = n00b_json_object_get(cur, seg);
        if (cur == nullptr) {
            return nullptr;
        }
        start = dot ? dot + 1 : start + len;
    }
    return cur;
}
