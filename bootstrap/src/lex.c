/**
 * @file lex.c
 * @brief C tokenizer for the NCC bootstrap compiler.
 *
 * Tokenizes preprocessed C source into a `tok_t[]` array, handling
 * identifiers, keywords, numbers, strings, character literals,
 * punctuation, whitespace, comments, and preprocessor directives.
 * Tracks `#line` directives for accurate source locations and
 * manages `ncc off`/`ncc on` pragma ranges.
 */
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <unistd.h>

#include "types.h"
#include "xform.h"

#define finish_token(t, state, p)              \
    do {                                       \
        (t)->len = (p) - (state)->cur;         \
        (state)->offset += (p) - (state)->cur; \
        (state)->cur = (p);                    \
    } while (0)

static inline void
tok_punct(lex_t *state, int len)
{
    state->line_start = false;
    lex_ensure_tok_space(state);
    state->toks[state->num_toks++] = (tok_t){
        .type     = TT_PUNCT,
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .len      = len,
        .line_no  = state->line_no,
            };

    state->offset += len;
    state->cur += len;
}

#define tok_unk(state) tok_punct(state, 1)

static inline void
tok_literal(lex_t *state, char terminator, ttype_t type)
{
    state->line_start = false;

    // Check if previous token is a literal prefix (identifier with no gap)
    tok_t *t;
    int    prefix_len = 0;

    if (state->num_toks > 0) {
        tok_t *prev = &state->toks[state->num_toks - 1];
        // Previous token is identifier AND ends exactly at current position
        if (prev->type == TT_ID && prev->offset + prev->len == state->offset) {
            // Reuse the previous token - it becomes the literal with a prefix
            t          = prev;
            prefix_len = prev->len;
            // Don't increment num_toks - we're reusing this slot
        }
        else {
            lex_ensure_tok_space(state);
            t = &state->toks[state->num_toks++];
        }
    }
    else {
        lex_ensure_tok_space(state);
        t = &state->toks[state->num_toks++];
    }

    int start_offset = state->offset - prefix_len;

    *t = (tok_t){
        .type       = type,
        .src_file   = state->cur_src_file,
        .offset     = start_offset,
        .line_no    = state->line_no,
        .prefix_len = prefix_len,
    };

    char *p = state->cur + 1;

    while (p < state->end) {
        char c = *p;
        if (c == terminator) {
            p++;
            t->len = (p - state->cur) + prefix_len;
            state->offset += p - state->cur;
            state->cur = p;
            return;
        }
        if (c == '\\') {
            p++;
            if (*p++ == '\n') {
                state->line_no++;
            }
            continue;
        }
        if (c == '\n') {
            t->type = TT_ERR;
            state->line_no++;
            t->len = (p - state->cur) + prefix_len;
            state->offset += p - state->cur;
            state->cur = p;
            return;
        }
        p++;
    }

    t->type = TT_ERR;
    t->len  = (p - state->cur) + prefix_len;
    state->offset += p - state->cur;
    state->cur = p;
}

#define tok_char(state)   tok_literal(state, '\'', TT_CHR)
#define tok_string(state) tok_literal(state, '"', TT_STR)

static inline bool
is_id_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static const char *c_keywords[] = {
    "_Alignas",
    "_Alignof",
    "_Atomic",
    "_BitInt",
    "_Bool",
    "_Complex",
    "_Countof",
    "_Decimal128",
    "_Decimal32",
    "_Decimal64",
    "_Float128",
    "_Float16",
    "_Float32",
    "_Float32x",
    "_Float64",
    "_Float64x",
    "_Generic",
    "_Imaginary",
    "_Nonnull",
    "_Noreturn",
    "_Null_unspecified",
    "_Nullable",
    "_Static_assert",
    "_Thread_local",
    "__Float32x4_t", // ARM NEON 4xfloat vector
    "__Float64x2_t", // ARM NEON 2xdouble vector
    "__SVBool_t",   // ARM SVE predicate type
    "__SVFloat32_t", // ARM SVE scalable float vector
    "__SVFloat64_t", // ARM SVE scalable double vector
    "__asm",
    "__asm__",
    "__attribute",
    "__attribute__",
    "__bf16",       // ARM/x86 bfloat16 (brain float, ML-optimized 16-bit float)
    "__builtin_types_compatible_p",
    "__builtin_va_arg",
    "__builtin_va_list",
    "__const",
    "__const__",
    "__extension__",
    "__fp16",       // ARM/Clang 16-bit half-precision float (IEEE 754 binary16)
    "__inline",
    "__inline__",
    "__int128",
    "__int128_t",
    "__mfp8",       // ARM 8-bit minifloat (for ML inference, AArch64)
    "__nonnull",
    "__null_unspecified",
    "__nullable",
    "__restrict",
    "__restrict__",
    "__signed",
    "__signed__",
    "__thread",
    "__typeof",
    "__typeof__",
    "__uint128_t",
    "__volatile",
    "__volatile__",
    "_kargs",
    "alignas",
    "alignof",
    "asm",
    "auto",
    "bool",
    "break",
    "case",
    "char",
    "const",
    "constexpr",
    "continue",
    "default",
    "do",
    "double",
    "else",
    "enum",
    "extern",
    "false",
    "float",
    "for",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "nullptr",
    "register",
    "restrict",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "struct",
    "switch",
    "thread_local",
    "true",
    "typedef",
    "typehash",
    "typeof",
    "typeof_unqual",
    "union",
    "unsigned",
    "void",
    "volatile",
    "while",
};

#define NUM_KEYWORDS (sizeof(c_keywords) / sizeof(c_keywords[0]))

static int
keyword_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void
ensure_keywords_sorted(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    for (size_t i = 1; i < NUM_KEYWORDS; i++) {
        if (strcmp(c_keywords[i - 1], c_keywords[i]) >= 0) {
            fprintf(stderr,
                    "FATAL: c_keywords out of order: \"%s\" >= \"%s\" at [%zu]\n",
                    c_keywords[i - 1],
                    c_keywords[i],
                    i);
            abort();
        }
    }
}

static inline bool
is_keyword(lex_t *state, tok_t *tok)
{
    char *s = extract(state->input, tok);
    return bsearch(&s, c_keywords, NUM_KEYWORDS, sizeof(char *), keyword_cmp) != nullptr;
}

/**
 * @brief Check if character can continue a numeric literal (pp-number).
 *
 * C numeric literals (pp-numbers) can contain:
 * - digits and letters (hex, exponents, suffixes)
 * - '.' for decimal points
 * - '\'' for C23 digit separators
 * - '+' or '-' after e/E/p/P for exponent signs
 */
static inline bool
is_num_char(char c, char prev)
{
    if (isalnum((unsigned char)c) || c == '_' || c == '.') {
        return true;
    }
    // C23 digit separator
    if (c == '\'') {
        return true;
    }
    // Exponent sign: +/- after e, E, p, or P
    if ((c == '+' || c == '-') && (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
        return true;
    }
    return false;
}

static inline void
tok_id_or_num(lex_t *state, bool num)
{
    state->line_start = false;
    lex_ensure_tok_space(state);
    tok_t *t          = &state->toks[state->num_toks++];
    char  *p          = state->cur;

    *t = (tok_t){
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .line_no  = state->line_no,
            };

    if (num) {
        // Numeric literal: use extended character set
        char prev = *p;
        while (++p < state->end) {
            if (!is_num_char(*p, prev)) {
                break;
            }
            prev = *p;
        }
        t->len  = p - state->cur;
        t->type = TT_NUM;
    }
    else {
        // Identifier: use standard identifier characters
        while (++p < state->end) {
            if (!is_id_char(*p)) {
                break;
            }
        }
        // Set len before keyword check (is_keyword needs t->len)
        t->len = p - state->cur;
        if (is_keyword(state, t)) {
            t->type = TT_KEYWORD;
        }
        else {
            t->type = TT_ID;
        }
    }

    state->offset += p - state->cur;
    state->cur = p;
}

static inline void
tok_ws(lex_t *state)
{
    lex_ensure_tok_space(state);
    tok_t *t = &state->toks[state->num_toks++];
    char  *p = state->cur;

    *t = (tok_t){
        .type     = TT_WS,
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .line_no  = state->line_no,
            };

    if (*p == '\n') {
        state->line_no++;
        state->line_start = true;
    }

    while (++p < state->end) {
        switch (*p) {
        case ' ':
        case '\t':
        case '\r':
            continue;
        case '\n':
            state->line_start = true;
            state->line_no++;
            continue;
        default:
            break;
        }
        break;
    }

    finish_token(t, state, p);
}

// Add a new ncc_off range starting at current token index
static void
add_ncc_off_range(lex_t *state)
{
    state->num_ranges++;
    state->ncc_off_ranges = base_realloc(state->ncc_off_ranges,
                                    state->num_ranges * sizeof(ncc_off_range_t));
    state->ncc_off_ranges[state->num_ranges - 1] = (ncc_off_range_t){
        .start = state->num_toks,
        .end   = -1, // Will be set when we see #pragma ncc on
    };
}

// Close the most recent open ncc_off range
static void
close_ncc_off_range(lex_t *state)
{
    // Find the most recent unclosed range
    for (int i = state->num_ranges - 1; i >= 0; i--) {
        if (state->ncc_off_ranges[i].end == -1) {
            state->ncc_off_ranges[i].end = state->num_toks;
            return;
        }
    }
}

// Parse a line directive and update line number and file tracking
// Handles: # <line> "<file>" [flags] and #line <line> ["<file>"]
// Flag 3 indicates a system header file
static void
parse_line_directive(lex_t *state, char *start, char *end)
{
    char *p = start + 1; // Skip '#'

    // Skip whitespace after #
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Check for "line" keyword (optional in GCC-style directives)
    if (p + 4 <= end && strncmp(p, "line", 4) == 0 &&
        (p[4] == ' ' || p[4] == '\t')) {
        p += 4;
        // Skip whitespace after "line"
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
    }

    // Parse line number
    if (p < end && *p >= '0' && *p <= '9') {
        int line_num = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            line_num = line_num * 10 + (*p - '0');
            p++;
        }
        // Set line_no to one less than the directive value, because
        // the newline at the end of the directive will increment it
        if (line_num > 0) {
            state->line_no = line_num - 1;
        }

        // Skip whitespace before filename
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        // Parse quoted filename if present
        if (p < end && *p == '"') {
            p++; // Skip opening quote
            char *filename_start = p;
            while (p < end && *p != '"') {
                p++;
            }
            int filename_len = p - filename_start;
            if (filename_len > 0) {
                // Allocate and store filename (these are interned, so duplicates share memory)
                char *filename = base_alloc(filename_len + 1);
                memcpy(filename, filename_start, filename_len);
                filename[filename_len]  = '\0';
                state->cur_src_file = filename;
            }
            p++; // Skip closing quote
        }

        // Parse flags after filename (space-separated integers)
        // Flag 3 = system header - use to create ncc_off ranges
        bool has_flag_3 = false;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (p < end && *p >= '0' && *p <= '9') {
                int flag = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    flag = flag * 10 + (*p - '0');
                    p++;
                }
                if (flag == 3) {
                    has_flag_3 = true;
                }
            }
            else {
                break;
            }
        }

        // Track system header state for ncc_off ranges.
        // Only transition INTO system header if not already guarded by
        // #pragma ncc off (which already created a range).
        if (has_flag_3 && !state->in_system_header) {
            // Check if we're already inside a pragma ncc off range
            bool already_off = false;
            for (int i = state->num_ranges - 1; i >= 0; i--) {
                if (state->ncc_off_ranges[i].end == -1) {
                    already_off = true;
                    break;
                }
            }
            state->in_system_header = true;
            if (!already_off) {
                add_ncc_off_range(state);
            }
        }
        else if (!has_flag_3 && state->in_system_header) {
            state->in_system_header = false;
            // Only close a range if we opened one (not if pragma did)
            // Check if the most recent unclosed range was opened at/after
            // where we started tracking this system header
            for (int i = state->num_ranges - 1; i >= 0; i--) {
                if (state->ncc_off_ranges[i].end == -1) {
                    // Close it - we're leaving the system header
                    state->ncc_off_ranges[i].end = state->num_toks;
                    break;
                }
            }
        }
    }
}

// Check for #pragma ncc off/on and update ranges
// Returns true if this was a #pragma ncc directive
static bool
parse_pragma_ncc(lex_t *state, char *start, char *end)
{
    char *p = start + 1; // Skip '#'

    // Skip whitespace after #
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Check for "pragma"
    if (p + 6 > end || strncmp(p, "pragma", 6) != 0) {
        return false;
    }
    p += 6;

    // Must have whitespace after pragma
    if (p >= end || (*p != ' ' && *p != '\t')) {
        return false;
    }

    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Check for "ncc"
    if (p + 3 > end || strncmp(p, "ncc", 3) != 0) {
        return false;
    }
    p += 3;

    // Must have whitespace after ncc
    if (p >= end || (*p != ' ' && *p != '\t')) {
        return false;
    }

    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Check for "off" or "on"
    if (p + 3 <= end && strncmp(p, "off", 3) == 0) {
        char c = (p + 3 < end) ? p[3] : '\0';
        if (c == '\0' || c == ' ' || c == '\t' || c == '\n') {
            add_ncc_off_range(state);
            return true;
        }
    }
    else if (p + 2 <= end && strncmp(p, "on", 2) == 0) {
        char c = (p + 2 < end) ? p[2] : '\0';
        if (c == '\0' || c == ' ' || c == '\t' || c == '\n') {
            close_ncc_off_range(state);
            return true;
        }
    }

    return false;
}

static inline void
tok_preproc(lex_t *state)
{
    lex_ensure_tok_space(state);
    tok_t *t     = &state->toks[state->num_toks++];
    char  *p     = state->cur;
    char  *start = p;

    *t = (tok_t){
        .type     = TT_PREPROC,
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .line_no  = state->line_no,
            };

    while (++p < state->end) {
        switch (*p) {
        case '\\':
            if (*++p == '\n') {
                state->line_no++;
            }
            continue;
        case '\n':
            // Check for #pragma ncc first
            if (parse_pragma_ncc(state, start, p)) {
                // Mark the pragma to be skipped in output
                t->skip_emit = 1;
            }
            // Then check if this is a line directive
            parse_line_directive(state, start, p);
            state->line_no++;
            state->line_start = true;
            ++p;
            break;
        default:
            continue;
        }
        break;
    }

    finish_token(t, state, p);
}

static inline void
line_comment(lex_t *state)
{
    lex_ensure_tok_space(state);
    tok_t *t = &state->toks[state->num_toks++];
    char  *p = state->cur;

    *t = (tok_t){
        .type     = TT_COMMENT,
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .line_no  = state->line_no,
            };

    while (++p < state->end) {
        if (*p == '\n') {
            state->line_no++;
            state->line_start = true;
            p++;
            break;
        }
    }
    finish_token(t, state, p);
}

static inline void
match_comment(lex_t *state)
{
    lex_ensure_tok_space(state);
    tok_t *t = &state->toks[state->num_toks++];
    char  *p = state->cur;

    *t = (tok_t){
        .type     = TT_COMMENT,
        .src_file = state->cur_src_file,
        .offset   = state->offset,
        .line_no  = state->line_no,
            };

    while (++p < state->end - 1) {
        if (*p == '*' && p[1] == '/') {
            p += 2;
            finish_token(t, state, p);
            return;
        }
    }

    t->type = TT_ERR;
    finish_token(t, state, p);
}

static inline void
tok_comment_or_punct(lex_t *state)
{
    if (state->cur + 1 >= state->end) {
        return;
    }

    switch (state->cur[1]) {
    case '/':
        line_comment(state);
        break;
    case '*':
        match_comment(state);
        break;
    case '=':
        tok_punct(state, 2);
        break;
    default:
        tok_punct(state, 1);
        break;
    }
}

void
lex(lex_t *state)
{
    while (state->cur < state->end) {
        switch (*state->cur) {
        case ' ':
        case '\t':
        case '\n':
            tok_ws(state);
            continue;
        case '#':
            if (state->line_start) {
                tok_preproc(state);
            }
            else {
                // Not valid but just in case.
                tok_punct(state, 1);
            }
            continue;
        case '/':
            tok_comment_or_punct(state);
            continue;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            tok_id_or_num(state, true);
            continue;
        case '_':
        case 'a':
        case 'b':
        case 'c':
        case 'd':
        case 'e':
        case 'f':
        case 'g':
        case 'h':
        case 'i':
        case 'j':
        case 'k':
        case 'l':
        case 'm':
        case 'n':
        case 'o':
        case 'p':
        case 'q':
        case 'r':
        case 's':
        case 't':
        case 'u':
        case 'v':
        case 'w':
        case 'x':
        case 'y':
        case 'z':
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case 'I':
        case 'J':
        case 'K':
        case 'L':
        case 'M':
        case 'N':
        case 'O':
        case 'P':
        case 'Q':
        case 'R':
        case 'S':
        case 'T':
        case 'U':
        case 'V':
        case 'W':
        case 'X':
        case 'Y':
        case 'Z':
            tok_id_or_num(state, false);
            continue;
        case '"':
            tok_string(state);
            continue;
        case '\'':
            tok_char(state);
            continue;
        case '.':
            if (state->cur + 2 < state->end
                && state->cur[1] == '.'
                && state->cur[2] == '.') {
                tok_punct(state, 3);
            }
            else if (state->cur + 1 < state->end
                     && isdigit((unsigned char)state->cur[1])) {
                // Float starting with dot: .5, .123, etc.
                tok_id_or_num(state, true);
            }
            else {
                tok_punct(state, 1);
            }
            continue;
        case ':':
            if (state->cur + 1 < state->end && state->cur[1] == ':') {
                tok_punct(state, 2);
            }
            else {
                tok_punct(state, 1);
            }
            continue;
        case '+':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '+' || state->cur[1] == '=') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '-':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '-'
                    || state->cur[1] == '=' || state->cur[1] == '>') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '*':
            if (state->cur + 1 < state->end && state->cur[1] == '=') {
                tok_punct(state, 2);
                continue;
            }
            tok_punct(state, 1);
            continue;
        case '%':
            if (state->cur + 1 < state->end && state->cur[1] == '=') {
                tok_punct(state, 2);
                continue;
            }
            tok_punct(state, 1);
            continue;
        case '&':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '&' || state->cur[1] == '=') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '|':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '|' || state->cur[1] == '=') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '^':
            if (state->cur + 1 < state->end && state->cur[1] == '=') {
                tok_punct(state, 2);
                continue;
            }
            tok_punct(state, 1);
            continue;
        case '=':
            if (state->cur + 1 < state->end && state->cur[1] == '=') {
                tok_punct(state, 2);
                continue;
            }
            tok_punct(state, 1);
            continue;
        case '!':
            if (state->cur + 1 < state->end && state->cur[1] == '=') {
                tok_punct(state, 2);
                continue;
            }
            tok_punct(state, 1);
            continue;
        case '<':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '<') {
                    if (state->cur + 2 < state->end && state->cur[2] == '=') {
                        tok_punct(state, 3);
                        continue;
                    }
                    tok_punct(state, 2);
                    continue;
                }
                if (state->cur[1] == '=') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '>':
            if (state->cur + 1 < state->end) {
                if (state->cur[1] == '>') {
                    if (state->cur + 2 < state->end && state->cur[2] == '=') {
                        tok_punct(state, 3);
                        continue;
                    }
                    tok_punct(state, 2);
                    continue;
                }
                if (state->cur[1] == '=') {
                    tok_punct(state, 2);
                    continue;
                }
            }
            tok_punct(state, 1);
            continue;
        case '{':
            // Check if previous token is identifier prefix (no gap)
            if (state->num_toks > 0) {
                tok_t *prev = &state->toks[state->num_toks - 1];
                if (prev->type == TT_ID
                    && prev->offset + prev->len == state->offset) {
                    // Don't merge if preceded by struct/union/enum —
                    // the identifier is a tag name, not a brace prefix.
                    bool after_struct = false;
                    for (int si = state->num_toks - 2; si >= 0; si--) {
                        tok_t *pt = &state->toks[si];
                        if (pt->type == TT_WS || pt->type == TT_COMMENT) {
                            continue;
                        }
                        if (pt->type == TT_KEYWORD) {
                            int         kl;
                            const char *kt = tok_text_ptr(state->input,
                                                          pt, &kl);
                            if ((kl == 6 && !memcmp(kt, "struct", 6))
                                || (kl == 5 && !memcmp(kt, "union", 5))
                                || (kl == 4 && !memcmp(kt, "enum", 4))) {
                                after_struct = true;
                            }
                        }
                        break;
                    }
                    if (!after_struct) {
                        // Merge: identifier becomes prefix, token becomes '{'
                        prev->prefix_len = prev->len;
                        prev->type       = TT_PUNCT;
                        prev->len += 1; // Include the '{'
                        state->offset++;
                        state->cur++;
                        state->line_start = false;
                        continue;
                    }
                }
            }
            tok_punct(state, 1);
            continue;
        case '~':
        case '(':
        case ')':
        case '[':
        case '}':
        case ']':
        case ';':
        case ',':
        case '?':
            tok_punct(state, 1);
            continue;
        default:
            tok_unk(state);
            break;
        }
    }
}

void
lex_init(lex_t *ctx, ncc_buf_t *input, char *in_file)
{
    assert(input);
    ensure_keywords_sorted();

    int toks_cap = 0;

    *ctx = (lex_t){
        .input          = input,
        .toks           = alloc_tokens(input, &toks_cap),
        .ncc_off_ranges = nullptr,
        .cur            = input->data,
        .end            = input->data + input->len,
        .in_file        = in_file,
        .out_file       = nullptr,
        .cur_src_file   = in_file,
        .num_toks       = 0,
        .toks_cap       = toks_cap,
        .num_ranges     = 0,
        .offset         = 0,
        .line_no        = 1,
        .line_start     = true,
    };
}

bool
id_check(char *to_match, ncc_buf_t *input, int offset, int len)
{
    if (strlen(to_match) != (size_t)len) {
        return false;
    }

    return !(strncmp(to_match, input->data + offset, len));
}

char *
extract(ncc_buf_t *input, tok_t *tok)
{
    if (tok->type == TT_ERR) {
        return "«eof»";
    }

    // Handle synthetic tokens (use replacement buffer)
    const char *p;
    int         len;
    if (tok->replacement) {
        p   = tok->replacement->data;
        len = tok->replacement->len;
    }
    else {
        p   = input->data + tok->offset;
        len = tok->len;
    }

    char *result = base_calloc(1, len + 1);
    memcpy(result, p, len);

    return result;
}

tok_t *
advance(tok_xform_t *ctx, bool skip_ws)
{
    tok_t *t;

    while (true) {
        if (ctx->ix >= ctx->max) {
            return nullptr;
        }

        t = &ctx->toks[++ctx->ix];

        if (t->type == TT_COMMENT) {
            continue;
        }
        if (skip_ws && t->type == TT_WS) {
            continue;
        }

        return t;
    }
}

tok_t *
backup(tok_xform_t *ctx, bool skip_ws)
{
    tok_t *t;

    while (true) {
        if (ctx->ix == 0) {
            return nullptr;
        }
        t = &ctx->toks[--ctx->ix];

        if (t->type == TT_COMMENT) {
            continue;
        }
        if (skip_ws && t->type == TT_WS) {
            continue;
        }
        return t;
    }
}

/* ============================================================================
 * NCC-off range checking
 * ============================================================================ */

bool
lex_is_ncc_off(lex_t *state, int tok_ix)
{
    for (int i = 0; i < state->num_ranges; i++) {
        ncc_off_range_t *r = &state->ncc_off_ranges[i];
        int              end = (r->end == -1) ? state->num_toks : r->end;
        if (tok_ix >= r->start && tok_ix < end) {
            return true;
        }
    }
    return false;
}

int
lex_find_token_index(lex_t *state, tok_t *tok)
{
    if (!state || !tok) {
        return -1;
    }
    for (int i = 0; i < state->num_toks; i++) {
        if (&state->toks[i] == tok) {
            return i;
        }
    }
    return -1;
}

bool
lex_tok_is_ncc_off(lex_t *state, tok_t *tok)
{
    if (!state || !tok) {
        return false;
    }
    int ix = lex_find_token_index(state, tok);
    if (ix < 0) {
        return false;
    }
    return lex_is_ncc_off(state, ix);
}
