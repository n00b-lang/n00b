// type_normalize.c — Type tree normalization + mangle + hash.
//
// Walks parse tree leaves, collects token text, applies normalization
// (drop restrict/_Atomic, sort qualifiers, strip attributes, canonical
// spacing), then hashes via SHA256 for typeid/typehash.

#include "xform/type_normalize.h"
#include "core/sha256.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Leaf collection
// ============================================================================

typedef struct {
    const char **texts;
    size_t       count;
    size_t       cap;
} leaf_buf_t;

static void
leaf_buf_init(leaf_buf_t *lb)
{
    lb->cap   = 32;
    lb->count = 0;
    lb->texts = malloc(lb->cap * sizeof(char *));
}

static void
leaf_buf_push(leaf_buf_t *lb, const char *text)
{
    if (lb->count >= lb->cap) {
        lb->cap *= 2;
        lb->texts = realloc(lb->texts, lb->cap * sizeof(char *));
    }
    lb->texts[lb->count++] = text;
}

static void
leaf_buf_free(leaf_buf_t *lb)
{
    free(lb->texts);
}

static void
collect_leaves(n00b_parse_tree_t *node, leaf_buf_t *lb)
{
    if (!node) {
        return;
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);
        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t s = n00b_option_get(tok->value);
            if (s.data && s.u8_bytes > 0) {
                leaf_buf_push(lb, s.data);
            }
        }
        return;
    }

    size_t nc = n00b_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_leaves(n00b_tree_child(node, i), lb);
    }
}

// ============================================================================
// Normalization helpers
// ============================================================================

static bool
is_dropped_qualifier(const char *s)
{
    return strcmp(s, "restrict") == 0 || strcmp(s, "_Atomic") == 0
        || strcmp(s, "__restrict") == 0 || strcmp(s, "__restrict__") == 0;
}

static bool
is_kept_qualifier(const char *s)
{
    return strcmp(s, "const") == 0 || strcmp(s, "volatile") == 0
        || strcmp(s, "__const") == 0 || strcmp(s, "__const__") == 0
        || strcmp(s, "__volatile") == 0 || strcmp(s, "__volatile__") == 0;
}

// Normalize qualifier name to its canonical form.
static const char *
canonical_qualifier(const char *s)
{
    if (strcmp(s, "__const") == 0 || strcmp(s, "__const__") == 0) {
        return "const";
    }
    if (strcmp(s, "__volatile") == 0 || strcmp(s, "__volatile__") == 0) {
        return "volatile";
    }
    return s;
}

static bool
is_alnum_or_underscore(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool
is_attribute_start(const char *s)
{
    return strcmp(s, "__attribute__") == 0 || strcmp(s, "__attribute") == 0;
}

// ============================================================================
// String builder
// ============================================================================

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static void
strbuf_init(strbuf_t *sb)
{
    sb->cap = 128;
    sb->len = 0;
    sb->buf = malloc(sb->cap);
}

static void
strbuf_append(strbuf_t *sb, const char *s)
{
    size_t slen = strlen(s);
    if (sb->len + slen + 1 > sb->cap) {
        while (sb->len + slen + 1 > sb->cap) {
            sb->cap *= 2;
        }
        sb->buf = realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void
strbuf_append_char(strbuf_t *sb, char c)
{
    if (sb->len + 2 > sb->cap) {
        sb->cap *= 2;
        sb->buf = realloc(sb->buf, sb->cap);
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
}

static char *
strbuf_finish(strbuf_t *sb)
{
    return sb->buf;
}

// ============================================================================
// Core normalization
// ============================================================================

char *
n00b_normalize_type_tree(n00b_parse_tree_t *subtree)
{
    if (!subtree) {
        return strdup("");
    }

    leaf_buf_t lb;
    leaf_buf_init(&lb);
    collect_leaves(subtree, &lb);

    // Normalization pass over collected token texts.
    strbuf_t out;
    strbuf_init(&out);

    // Qualifier accumulator (at most a few qualifiers).
    const char *quals[8];
    int         nquals     = 0;
    bool        last_alnum = false;

    // Skip attribute blocks: track depth for [[...]] and __attribute__((...)).
    int  attr_bracket_depth = 0;
    int  attr_paren_depth   = 0;
    bool in_attribute        = false;

    for (size_t i = 0; i < lb.count; i++) {
        const char *tok = lb.texts[i];

        // Handle [[...]] attribute blocks.
        if (attr_bracket_depth > 0) {
            if (strcmp(tok, "[") == 0) {
                attr_bracket_depth++;
            }
            else if (strcmp(tok, "]") == 0) {
                attr_bracket_depth--;
            }
            continue;
        }

        if (strcmp(tok, "[") == 0 && i + 1 < lb.count
            && strcmp(lb.texts[i + 1], "[") == 0) {
            attr_bracket_depth = 2;
            i++; // skip second [
            continue;
        }

        // Handle __attribute__((...)) blocks.
        if (in_attribute) {
            if (strcmp(tok, "(") == 0) {
                attr_paren_depth++;
            }
            else if (strcmp(tok, ")") == 0) {
                attr_paren_depth--;
                if (attr_paren_depth == 0) {
                    in_attribute = false;
                }
            }
            continue;
        }

        if (is_attribute_start(tok)) {
            in_attribute    = true;
            attr_paren_depth = 0;
            continue;
        }

        // Drop certain qualifiers.
        if (is_dropped_qualifier(tok)) {
            continue;
        }

        // Accumulate kept qualifiers for sorted output.
        if (is_kept_qualifier(tok)) {
            if (nquals < 8) {
                quals[nquals++] = canonical_qualifier(tok);
            }
            continue;
        }

        // Non-qualifier token: flush sorted qualifiers first.
        if (nquals > 0) {
            // Simple insertion sort for tiny array.
            for (int a = 1; a < nquals; a++) {
                const char *key = quals[a];
                int         b   = a - 1;
                while (b >= 0 && strcmp(quals[b], key) > 0) {
                    quals[b + 1] = quals[b];
                    b--;
                }
                quals[b + 1] = key;
            }

            for (int q = 0; q < nquals; q++) {
                if (out.len > 0 && last_alnum) {
                    strbuf_append_char(&out, ' ');
                }
                strbuf_append(&out, quals[q]);
                last_alnum = true;
            }
            nquals = 0;
        }

        // Emit token with canonical spacing.
        bool tok_is_alnum = tok[0] != '\0' && is_alnum_or_underscore(tok[0]);

        if (out.len > 0 && tok_is_alnum && last_alnum) {
            strbuf_append_char(&out, ' ');
        }

        strbuf_append(&out, tok);
        last_alnum = tok_is_alnum;
    }

    // Flush trailing qualifiers.
    if (nquals > 0) {
        for (int a = 1; a < nquals; a++) {
            const char *key = quals[a];
            int         b   = a - 1;
            while (b >= 0 && strcmp(quals[b], key) > 0) {
                quals[b + 1] = quals[b];
                b--;
            }
            quals[b + 1] = key;
        }

        for (int q = 0; q < nquals; q++) {
            if (out.len > 0 && last_alnum) {
                strbuf_append_char(&out, ' ');
            }
            strbuf_append(&out, quals[q]);
            last_alnum = true;
        }
    }

    leaf_buf_free(&lb);
    return strbuf_finish(&out);
}

// ============================================================================
// Mangle: SHA256 -> base64-like identifier
// ============================================================================

// clang-format off
static const signed char b64_map[] = {
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',
    'z', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', -1, -2
};
// clang-format on

static char *
map_one(int bits, char *p)
{
    assert(bits < 64 && bits >= 0);

    int c = b64_map[bits];

    if (c > 0) {
        *p++ = (char)c;
    }
    else {
        *p++ = '_';
        *p++ = (char)('c' + c);
    }

    return p;
}

static void
digest_to_bytes(n00b_sha256_digest_t digest, uint8_t *out)
{
    for (int i = 0; i < N00B_SHA256_DIGEST_WORDS; i++) {
        uint32_t w     = digest[i];
        out[i * 4 + 0] = (uint8_t)(w >> 24);
        out[i * 4 + 1] = (uint8_t)(w >> 16);
        out[i * 4 + 2] = (uint8_t)(w >> 8);
        out[i * 4 + 3] = (uint8_t)(w);
    }
}

char *
n00b_type_mangle(const char *normalized)
{
    n00b_sha256_digest_t digest;
    n00b_sha256_hash(normalized, strlen(normalized), digest);

    uint8_t dbytes[32];
    digest_to_bytes(digest, dbytes);

    // 30 bytes -> 40 base64 chars + 2 prefix + 1 NUL.
    // Some chars expand to 2 bytes (_a or _b), allocate extra.
    char *res = malloc(96);
    char *p   = res;
    *p++      = '_';
    *p++      = '_';

    for (int i = 0; i < 30;) {
        int c = dbytes[i++];
        int d = dbytes[i++];
        int e = dbytes[i++];

        p = map_one(c >> 2, p);
        p = map_one(((c & 0x3) << 4) | (d >> 4), p);
        p = map_one(((d & 0x0f) << 2) | (e >> 6), p);
        p = map_one(e & 0x3f, p);
    }

    *p = '\0';
    return res;
}

// ============================================================================
// Hash: SHA256 -> uint64
// ============================================================================

uint64_t
n00b_type_hash_u64(const char *normalized)
{
    n00b_sha256_digest_t digest;
    n00b_sha256_hash(normalized, strlen(normalized), digest);

    uint8_t dbytes[32];
    digest_to_bytes(digest, dbytes);

    uint64_t h = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | dbytes[i];
    }
    return h;
}
