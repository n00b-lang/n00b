/**
 * @file mod_token_xforms.c
 * @brief Phase A: Pre-CPP token-level transforms for --modernize.
 *
 * Operates on the original source token array before preprocessing.
 * Uses tok->replacement for substitutions and tok->skip_emit for deletions.
 */
#include <stdbool.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <ctype.h>

#include "lex.h"
#include "token.h"
#include "buf.h"
#include "modernize.h"

// Track which headers need to be inserted
typedef struct {
    bool need_stddef;    // for unreachable()
    bool need_stdbit;    // for stdc_leading_zeros etc.
    bool need_stdckdint; // for ckd_add etc.
    bool has_stddef;
    bool has_stdbit;
    bool has_stdckdint;
    int  insert_after_ix; // Token index after which to insert headers (-1 = at start)
} header_state_t;

/**
 * @brief Check if token text matches a string (works for both normal and
 *        replacement tokens).
 */
static bool
tok_text_eq(ncc_buf_t *input, tok_t *tok, const char *str)
{
    int         len;
    const char *text = tok_text_ptr(input, tok, &len);
    int         slen = strlen(str);

    return len == slen && memcmp(text, str, slen) == 0;
}

/**
 * @brief Set replacement text on a token.
 */
static void
set_replacement(tok_t *tok, const char *text)
{
    int        len = strlen(text);
    ncc_buf_t *buf = ncc_buf_alloc(len);
    memcpy(buf->data, text, len);
    buf->data[len] = '\0';
    buf->len       = len;
    tok->replacement = buf;
}

/**
 * @brief Check if preproc token is an include of a specific header.
 *        Matches both <header> and "header" forms.
 */
static bool
preproc_is_include(ncc_buf_t *input, tok_t *tok, const char *header)
{
    int         len;
    const char *text = tok_text_ptr(input, tok, &len);

    // Must start with #
    if (len < 1 || text[0] != '#') {
        return false;
    }

    const char *p   = text + 1;
    const char *end = text + len;

    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Must have "include"
    if (p + 7 > end || strncmp(p, "include", 7) != 0) {
        return false;
    }
    p += 7;

    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Match <header> or "header"
    if (p < end && (*p == '<' || *p == '"')) {
        char closer = (*p == '<') ? '>' : '"';
        p++;
        const char *name_start = p;

        while (p < end && *p != closer && *p != '\n') {
            p++;
        }

        int hlen = strlen(header);
        if (p - name_start == hlen && memcmp(name_start, header, hlen) == 0) {
            return true;
        }
    }

    return false;
}

// ============================================================
// Tier 1: Keyword replacements
// ============================================================

static void
xform_keyword_replacements(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type == TT_KEYWORD) {
            if (tok_text_eq(input, tok, "_Bool")) {
                set_replacement(tok, "bool");
            }
            else if (tok_text_eq(input, tok, "_Static_assert")) {
                set_replacement(tok, "static_assert");
            }
            else if (tok_text_eq(input, tok, "_Alignas")) {
                set_replacement(tok, "alignas");
            }
            else if (tok_text_eq(input, tok, "_Alignof")) {
                set_replacement(tok, "alignof");
            }
            else if (tok_text_eq(input, tok, "_Thread_local")) {
                set_replacement(tok, "thread_local");
            }
            else if (tok_text_eq(input, tok, "_Noreturn")) {
                set_replacement(tok, "[[noreturn]]");
            }
            else if (tok_text_eq(input, tok, "__typeof__")) {
                set_replacement(tok, "typeof");
            }
        }
        else if (tok->type == TT_ID) {
            if (tok_text_eq(input, tok, "__typeof__")) {
                set_replacement(tok, "typeof");
            }
            else if (tok_text_eq(input, tok, "__auto_type")) {
                set_replacement(tok, "auto");
            }
        }
    }
}

// ============================================================
// Tier 1: Include removal (#include <stdbool.h> etc.)
// ============================================================

static void
xform_include_removal(lex_t *state, header_state_t *headers, bool skip_removal)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_PREPROC) {
            continue;
        }

        if (!skip_removal) {
            if (preproc_is_include(input, tok, "stdbool.h")) {
                tok->skip_emit = 1;
                // Also skip trailing whitespace (the newline)
                if (i + 1 < state->num_toks && state->toks[i + 1].type == TT_WS) {
                    state->toks[i + 1].skip_emit = 1;
                }
            }
            else if (preproc_is_include(input, tok, "stdalign.h")) {
                tok->skip_emit = 1;
                if (i + 1 < state->num_toks && state->toks[i + 1].type == TT_WS) {
                    state->toks[i + 1].skip_emit = 1;
                }
            }
            else if (preproc_is_include(input, tok, "stdnoreturn.h")) {
                tok->skip_emit = 1;
                if (i + 1 < state->num_toks && state->toks[i + 1].type == TT_WS) {
                    state->toks[i + 1].skip_emit = 1;
                }
            }
        }

        // Track existing headers for later insertion decisions
        if (preproc_is_include(input, tok, "stddef.h")) {
            headers->has_stddef = true;
        }
        else if (preproc_is_include(input, tok, "stdbit.h")) {
            headers->has_stdbit = true;
        }
        else if (preproc_is_include(input, tok, "stdckdint.h")) {
            headers->has_stdckdint = true;
        }
    }
}

// ============================================================
// Tier 1: Preprocessor modernization (#elifdef / #elifndef)
// ============================================================

static void
xform_elifdef(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_PREPROC) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(input, tok, &len);

        // Look for #elif followed by defined(X) or !defined(X)
        const char *p   = text;
        const char *end = text + len;

        if (*p != '#') {
            continue;
        }
        p++;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p + 4 > end || strncmp(p, "elif", 4) != 0) {
            continue;
        }
        p += 4;

        if (p >= end || (*p != ' ' && *p != '\t')) {
            continue;
        }

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool negated = false;
        if (p < end && *p == '!') {
            negated = true;
            p++;
        }

        if (p + 7 > end || strncmp(p, "defined", 7) != 0) {
            continue;
        }
        p += 7;

        // Skip whitespace
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p >= end || *p != '(') {
            continue;
        }
        p++;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        const char *name_start = p;
        while (p < end && (isalnum(*p) || *p == '_')) {
            p++;
        }
        int name_len = p - name_start;

        if (name_len == 0) {
            continue;
        }

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p >= end || *p != ')') {
            continue;
        }
        p++;

        // Make sure nothing else follows (except whitespace/newline)
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p != '\n' && *p != '\0') {
            continue;
        }

        // Build replacement: #elifdef X\n or #elifndef X\n
        // Include trailing newline since the original preproc token includes it
        char  buf[256];
        int   rlen;
        if (negated) {
            rlen = snprintf(buf, sizeof(buf), "#elifndef %.*s\n", name_len, name_start);
        }
        else {
            rlen = snprintf(buf, sizeof(buf), "#elifdef %.*s\n", name_len, name_start);
        }

        if (rlen > 0 && rlen < (int)sizeof(buf)) {
            set_replacement(tok, buf);
        }
    }
}

// ============================================================
// Tier 2: __attribute__ replacements
// ============================================================

/**
 * @brief Skip whitespace/comment tokens forward.
 * @return Index of next non-WS/comment token, or num_toks if end.
 */
static int
skip_ws_fwd(lex_t *state, int ix)
{
    while (ix < state->num_toks) {
        ttype_t t = state->toks[ix].type;
        if (t != TT_WS && t != TT_COMMENT) {
            return ix;
        }
        ix++;
    }
    return ix;
}

static void
xform_attributes(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if ((tok->type != TT_ID && tok->type != TT_KEYWORD)
            || !tok_text_eq(input, tok, "__attribute__")) {
            continue;
        }

        // Match: __attribute__ ( ( <name> [( <args> )] ) )
        int j = skip_ws_fwd(state, i + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], "(")) {
            continue;
        }
        int outer_open = j;

        j = skip_ws_fwd(state, j + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], "(")) {
            continue;
        }
        int inner_open = j;

        j = skip_ws_fwd(state, j + 1);
        if (j >= state->num_toks
            || (state->toks[j].type != TT_ID && state->toks[j].type != TT_KEYWORD)) {
            continue;
        }
        int attr_name_ix = j;

        // Check which attribute this is
        const char *replacement = nullptr;

        if (tok_text_eq(input, &state->toks[j], "noreturn")) {
            replacement = "[[noreturn]]";
        }
        else if (tok_text_eq(input, &state->toks[j], "unused")) {
            replacement = "[[maybe_unused]]";
        }
        else if (tok_text_eq(input, &state->toks[j], "warn_unused_result")) {
            replacement = "[[nodiscard]]";
        }
        else if (tok_text_eq(input, &state->toks[j], "fallthrough")) {
            replacement = "[[fallthrough]]";
        }
        else if (tok_text_eq(input, &state->toks[j], "deprecated")) {
            // Could be deprecated or deprecated("msg")
            int k = skip_ws_fwd(state, j + 1);

            if (k < state->num_toks && tok_text_eq(input, &state->toks[k], "(")) {
                // deprecated("msg") — find the string arg
                int str_ix = skip_ws_fwd(state, k + 1);
                if (str_ix < state->num_toks && state->toks[str_ix].type == TT_STR) {
                    int close_inner_arg = skip_ws_fwd(state, str_ix + 1);
                    if (close_inner_arg < state->num_toks
                        && tok_text_eq(input, &state->toks[close_inner_arg], ")")) {
                        int close1 = skip_ws_fwd(state, close_inner_arg + 1);
                        if (close1 < state->num_toks
                            && tok_text_eq(input, &state->toks[close1], ")")) {
                            int close2 = skip_ws_fwd(state, close1 + 1);
                            if (close2 < state->num_toks
                                && tok_text_eq(input, &state->toks[close2], ")")) {
                                // Build [[deprecated("msg")]]
                                int         slen;
                                const char *stext = tok_text_ptr(input, &state->toks[str_ix], &slen);
                                char        buf[512];
                                int rlen = snprintf(buf, sizeof(buf),
                                                    "[[deprecated(%.*s)]]", slen, stext);
                                if (rlen > 0 && rlen < (int)sizeof(buf)) {
                                    set_replacement(tok, buf);
                                    // Skip all tokens from outer_open to close2
                                    for (int x = outer_open; x <= close2; x++) {
                                        state->toks[x].skip_emit = 1;
                                    }
                                }
                                continue;
                            }
                        }
                    }
                }
            }
            else {
                // Plain deprecated (no args)
                replacement = "[[deprecated]]";
            }
        }

        if (!replacement) {
            continue;
        }

        // Find closing )) after the attribute name
        j = skip_ws_fwd(state, attr_name_ix + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], ")")) {
            continue;
        }
        int inner_close = j;

        j = skip_ws_fwd(state, j + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], ")")) {
            continue;
        }
        int outer_close = j;

        (void)inner_open;
        (void)outer_open;
        (void)inner_close;

        set_replacement(tok, replacement);

        // Mark all remaining tokens in the pattern as skip_emit
        for (int x = outer_open; x <= outer_close; x++) {
            state->toks[x].skip_emit = 1;
        }
    }
}

// ============================================================
// Tier 2: __builtin_* replacements
// ============================================================

/**
 * @brief Collect tokens for a single argument expression (respecting paren nesting).
 *        Returns the index of the delimiter (',' or ')') after the arg.
 */
static int
collect_arg(lex_t *state, int start, int *arg_start, int *arg_end)
{
    int depth = 0;
    int first = -1;
    int last  = -1;

    for (int i = start; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type == TT_WS || tok->type == TT_COMMENT) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(state->input, tok, &len);

        if (len == 1 && text[0] == '(') {
            depth++;
            if (first < 0) {
                first = i;
            }
            last = i;
        }
        else if (len == 1 && text[0] == ')') {
            if (depth == 0) {
                *arg_start = first;
                *arg_end   = last;
                return i;
            }
            depth--;
            last = i;
        }
        else if (len == 1 && text[0] == ',' && depth == 0) {
            *arg_start = first;
            *arg_end   = last;
            return i;
        }
        else {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }

    *arg_start = first;
    *arg_end   = last;
    return state->num_toks;
}

/**
 * @brief Extract text for a range of tokens (including intervening WS).
 */
static char *
extract_token_range(lex_t *state, int first, int last)
{
    ncc_buf_t *result = ncc_buf_alloc(64);

    for (int i = first; i <= last; i++) {
        tok_t      *tok = &state->toks[i];
        int         len;
        const char *text = tok_text_ptr(state->input, tok, &len);
        result = ncc_buf_concat(result, (char *)text, len);
    }

    // Null-terminate
    char *s = base_alloc(result->len + 1);
    memcpy(s, result->data, result->len);
    s[result->len] = '\0';
    base_dealloc(result);
    return s;
}

static void
xform_builtins(lex_t *state, header_state_t *headers)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_ID) {
            continue;
        }

        // Check for __builtin_unreachable()
        if (tok_text_eq(input, tok, "__builtin_unreachable")) {
            int j = skip_ws_fwd(state, i + 1);
            if (j < state->num_toks && tok_text_eq(input, &state->toks[j], "(")) {
                int k = skip_ws_fwd(state, j + 1);
                if (k < state->num_toks && tok_text_eq(input, &state->toks[k], ")")) {
                    set_replacement(tok, "unreachable");
                    headers->need_stddef = true;
                }
            }
            continue;
        }

        // __builtin_clz variants -> stdc_leading_zeros
        bool is_clz = tok_text_eq(input, tok, "__builtin_clz")
                    || tok_text_eq(input, tok, "__builtin_clzl")
                    || tok_text_eq(input, tok, "__builtin_clzll");
        if (is_clz) {
            set_replacement(tok, "stdc_leading_zeros");
            headers->need_stdbit = true;
            continue;
        }

        // __builtin_ctz variants -> stdc_trailing_zeros
        bool is_ctz = tok_text_eq(input, tok, "__builtin_ctz")
                    || tok_text_eq(input, tok, "__builtin_ctzl")
                    || tok_text_eq(input, tok, "__builtin_ctzll");
        if (is_ctz) {
            set_replacement(tok, "stdc_trailing_zeros");
            headers->need_stdbit = true;
            continue;
        }

        // __builtin_popcount variants -> stdc_count_ones
        bool is_popc = tok_text_eq(input, tok, "__builtin_popcount")
                     || tok_text_eq(input, tok, "__builtin_popcountl")
                     || tok_text_eq(input, tok, "__builtin_popcountll");
        if (is_popc) {
            set_replacement(tok, "stdc_count_ones");
            headers->need_stdbit = true;
            continue;
        }

        // __builtin_{add,sub,mul}_overflow(a, b, &r) -> ckd_{add,sub,mul}(&r, a, b)
        const char *ckd_name = nullptr;
        if (tok_text_eq(input, tok, "__builtin_add_overflow")) {
            ckd_name = "ckd_add";
        }
        else if (tok_text_eq(input, tok, "__builtin_sub_overflow")) {
            ckd_name = "ckd_sub";
        }
        else if (tok_text_eq(input, tok, "__builtin_mul_overflow")) {
            ckd_name = "ckd_mul";
        }

        if (ckd_name) {
            // Parse: __builtin_xxx_overflow(a, b, &r)
            int j = skip_ws_fwd(state, i + 1);
            if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], "(")) {
                continue;
            }

            int a_start, a_end;
            int comma1 = collect_arg(state, j + 1, &a_start, &a_end);
            if (comma1 >= state->num_toks || !tok_text_eq(input, &state->toks[comma1], ",")) {
                continue;
            }

            int b_start, b_end;
            int comma2 = collect_arg(state, comma1 + 1, &b_start, &b_end);
            if (comma2 >= state->num_toks || !tok_text_eq(input, &state->toks[comma2], ",")) {
                continue;
            }

            int r_start, r_end;
            int close = collect_arg(state, comma2 + 1, &r_start, &r_end);
            if (close >= state->num_toks || !tok_text_eq(input, &state->toks[close], ")")) {
                continue;
            }

            if (a_start < 0 || b_start < 0 || r_start < 0) {
                continue;
            }

            // Extract text for each argument
            char *a_text = extract_token_range(state, a_start, a_end);
            char *b_text = extract_token_range(state, b_start, b_end);
            char *r_text = extract_token_range(state, r_start, r_end);

            // Build replacement: ckd_xxx(&r, a, b)
            char buf[1024];
            int  rlen = snprintf(buf, sizeof(buf), "%s(%s, %s, %s)",
                                 ckd_name, r_text, a_text, b_text);

            base_dealloc(a_text);
            base_dealloc(b_text);
            base_dealloc(r_text);

            if (rlen > 0 && rlen < (int)sizeof(buf)) {
                set_replacement(tok, buf);
                // Skip everything from the ( to the )
                for (int x = j; x <= close; x++) {
                    state->toks[x].skip_emit = 1;
                }
                headers->need_stdckdint = true;
            }
            continue;
        }
    }
}

// ============================================================
// Tier 2: = {0} -> = {}
// ============================================================

static void
xform_empty_init(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_PUNCT || !tok_text_eq(input, tok, "=")) {
            continue;
        }

        int j = skip_ws_fwd(state, i + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], "{")) {
            continue;
        }

        int k = skip_ws_fwd(state, j + 1);
        if (k >= state->num_toks || state->toks[k].type != TT_NUM
            || !tok_text_eq(input, &state->toks[k], "0")) {
            continue;
        }

        int m = skip_ws_fwd(state, k + 1);
        if (m >= state->num_toks || !tok_text_eq(input, &state->toks[m], "}")) {
            continue;
        }

        // Remove the 0 token
        state->toks[k].skip_emit = 1;
    }
}

// ============================================================
// Tier 2: ##__VA_ARGS__ -> __VA_OPT__(,) __VA_ARGS__
// ============================================================

static void
xform_va_args_paste(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_PREPROC) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(input, tok, &len);

        // Check if this preprocessor directive contains ##__VA_ARGS__
        const char *match = nullptr;
        for (int j = 0; j <= len - 14; j++) {
            if (memcmp(text + j, "##__VA_ARGS__", 13) == 0) {
                // Check it's actually ## followed by __VA_ARGS__
                // (not inside a string)
                match = text + j;
                break;
            }
        }

        if (!match) {
            continue;
        }

        // Build replacement text with the substitution
        ncc_buf_t *result     = ncc_buf_alloc(len + 32);
        int        prefix_len = match - text;

        // Copy up to the ## (but look for comma before ##)
        // The typical pattern is: , ##__VA_ARGS__
        // We want: __VA_OPT__(,) __VA_ARGS__
        // Find if there's a comma before ##
        int comma_pos = prefix_len - 1;
        while (comma_pos >= 0 && (text[comma_pos] == ' ' || text[comma_pos] == '\t')) {
            comma_pos--;
        }

        if (comma_pos >= 0 && text[comma_pos] == ',') {
            // Replace ", ##__VA_ARGS__" with "__VA_OPT__(,) __VA_ARGS__"
            result = ncc_buf_concat(result, (char *)text, comma_pos);
            result = ncc_buf_concat(result, " __VA_OPT__(,) __VA_ARGS__", 26);

            // Copy everything after ##__VA_ARGS__
            int after = (match - text) + 13;
            if (after < len) {
                result = ncc_buf_concat(result, (char *)text + after, len - after);
            }

            // Null-terminate the replacement
            result = ncc_buf_concat(result, "\0", 1);
            result->len--; // Don't count the null terminator

            tok->replacement = result;
        }
        else {
            // No comma before ## — this is a standard token paste
            // (e.g. x##__VA_ARGS__), not the GCC comma-elision extension.
            // Leave it alone.
            base_dealloc(result);
        }
    }
}

// ============================================================
// Tier 2: va_start(ap, last) -> va_start(ap)
// ============================================================

static void
xform_va_start(lex_t *state)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_ID || !tok_text_eq(input, tok, "va_start")) {
            continue;
        }

        // Match: va_start ( ap , last )
        int j = skip_ws_fwd(state, i + 1);
        if (j >= state->num_toks || !tok_text_eq(input, &state->toks[j], "(")) {
            continue;
        }

        // Find the first argument (ap)
        int ap_start, ap_end;
        int comma = collect_arg(state, j + 1, &ap_start, &ap_end);
        if (comma >= state->num_toks || !tok_text_eq(input, &state->toks[comma], ",")) {
            continue;
        }

        // Find the second argument (last) and closing paren
        int last_start, last_end;
        int close = collect_arg(state, comma + 1, &last_start, &last_end);
        if (close >= state->num_toks || !tok_text_eq(input, &state->toks[close], ")")) {
            continue;
        }

        // Remove from comma through the last argument (keep the closing paren)
        for (int x = comma; x < close; x++) {
            state->toks[x].skip_emit = 1;
        }
    }
}

// ============================================================
// Header insertion point computation
// ============================================================

/**
 * @brief Find insertion point and compute header text to add.
 *
 * Instead of modifying the mmap'd token array (which can't be realloc'd),
 * we compute:
 * - insert_after_ix: the token index after which to insert header text
 * - The caller reconstructs the source and inserts text at the right spot
 */
static void
compute_needed_headers(lex_t *state, header_state_t *headers)
{
    bool need_any = (headers->need_stddef && !headers->has_stddef)
                 || (headers->need_stdbit && !headers->has_stdbit)
                 || (headers->need_stdckdint && !headers->has_stdckdint);

    if (!need_any) {
        headers->insert_after_ix = -1;
        return;
    }

    // Find the last #include directive
    int last_include_ix = -1;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];
        if (tok->type != TT_PREPROC) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(state->input, tok, &len);
        const char *p    = text + 1;
        const char *end  = text + len;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p + 7 <= end && strncmp(p, "include", 7) == 0) {
            last_include_ix = i;
        }
    }

    if (last_include_ix < 0) {
        // No includes — insert before everything
        headers->insert_after_ix = -1;
    }
    else {
        // Insert after the newline following the last include
        if (last_include_ix + 1 < state->num_toks
            && state->toks[last_include_ix + 1].type == TT_WS) {
            headers->insert_after_ix = last_include_ix + 1;
        }
        else {
            headers->insert_after_ix = last_include_ix;
        }
    }
}

/**
 * @brief Build the header text to insert.
 * @return Allocated string with the header lines, or nullptr if nothing needed.
 */
static char *
build_header_text(header_state_t *headers)
{
    if (headers->insert_after_ix == -1
        && !(headers->need_stddef && !headers->has_stddef)
        && !(headers->need_stdbit && !headers->has_stdbit)
        && !(headers->need_stdckdint && !headers->has_stdckdint)) {
        return nullptr;
    }

    ncc_buf_t *result = ncc_buf_alloc(128);

    if (headers->need_stddef && !headers->has_stddef) {
        result = ncc_buf_concat(result, "#include <stddef.h>\n", 20);
    }
    if (headers->need_stdbit && !headers->has_stdbit) {
        result = ncc_buf_concat(result, "#include <stdbit.h>\n", 20);
    }
    if (headers->need_stdckdint && !headers->has_stdckdint) {
        result = ncc_buf_concat(result, "#include <stdckdint.h>\n", 23);
    }

    if (result->len == 0) {
        base_dealloc(result);
        return nullptr;
    }

    char *s = base_alloc(result->len + 1);
    memcpy(s, result->data, result->len);
    s[result->len] = '\0';
    base_dealloc(result);
    return s;
}

// ============================================================
// Overflow check -> ckd_* rewrite
// ============================================================

typedef enum {
    OVF_ADD,
    OVF_SUB,
    OVF_MUL,
} ovf_op_t;

static bool
is_type_max(ncc_buf_t *input, tok_t *tok)
{
    static const char *maxes[] = {
        "INT_MAX",    "UINT_MAX",   "LONG_MAX",   "ULONG_MAX",
        "LLONG_MAX",  "ULLONG_MAX", "SIZE_MAX",   "INT8_MAX",
        "INT16_MAX",  "INT32_MAX",  "INT64_MAX",  "UINT8_MAX",
        "UINT16_MAX", "UINT32_MAX", "UINT64_MAX",
    };

    for (int i = 0; i < (int)(sizeof(maxes) / sizeof(maxes[0])); i++) {
        if (tok_text_eq(input, tok, maxes[i])) {
            return true;
        }
    }
    return false;
}

static bool
is_type_min(ncc_buf_t *input, tok_t *tok)
{
    static const char *mins[] = {
        "INT_MIN",   "LONG_MIN",  "LLONG_MIN", "INT8_MIN",
        "INT16_MIN", "INT32_MIN", "INT64_MIN",
    };

    for (int i = 0; i < (int)(sizeof(mins) / sizeof(mins[0])); i++) {
        if (tok_text_eq(input, tok, mins[i])) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Collect a "simple expression" — an identifier, member access chain,
 *        array subscript, or parenthesized sub-expression.
 *
 * Starts at index `start` (should skip WS first). Stops at comparison
 * operators, arithmetic ops, &&, ||, comma, semicolon, or closing paren/brace
 * at depth 0.
 *
 * @return Index one past the last token of the expression, or -1 on failure.
 *         Sets *first_ix and *last_ix to the token range.
 */
static int
collect_simple_expr(lex_t *state, int start, int *first_ix, int *last_ix)
{
    int depth = 0;
    int first = -1;
    int last  = -1;

    for (int i = start; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type == TT_WS || tok->type == TT_COMMENT) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(state->input, tok, &len);

        if (len == 1) {
            char c = text[0];

            if (c == '(' || c == '[') {
                if (first < 0) {
                    first = i;
                }
                depth++;
                last = i;
                continue;
            }
            if (c == ')' || c == ']') {
                if (depth == 0) {
                    // End of expression
                    *first_ix = first;
                    *last_ix  = last;
                    return i;
                }
                depth--;
                last = i;
                continue;
            }

            if (depth == 0) {
                // Stop at operators and delimiters
                if (c == '>' || c == '<' || c == '+' || c == '-' || c == '*'
                    || c == '/' || c == ',' || c == ';' || c == '{' || c == '}') {
                    *first_ix = first;
                    *last_ix  = last;
                    return i;
                }
            }
        }

        if (depth == 0 && len == 2) {
            if (memcmp(text, "&&", 2) == 0 || memcmp(text, "||", 2) == 0
                || memcmp(text, "!=", 2) == 0 || memcmp(text, "==", 2) == 0
                || memcmp(text, ">=", 2) == 0 || memcmp(text, "<=", 2) == 0) {
                *first_ix = first;
                *last_ix  = last;
                return i;
            }
        }

        // Part of the expression
        if (first < 0) {
            first = i;
        }
        last = i;
    }

    *first_ix = first;
    *last_ix  = last;
    return state->num_toks;
}

/**
 * @brief Compare two expression ranges token-by-token (ignoring whitespace).
 */
static bool
exprs_match(lex_t *state, int a_first, int a_last, int b_first, int b_last)
{
    if (a_first < 0 || b_first < 0) {
        return false;
    }

    ncc_buf_t *input = state->input;
    int    ai    = a_first;
    int    bi    = b_first;

    while (true) {
        // Skip WS in a
        while (ai <= a_last && (state->toks[ai].type == TT_WS
                                || state->toks[ai].type == TT_COMMENT)) {
            ai++;
        }
        // Skip WS in b
        while (bi <= b_last && (state->toks[bi].type == TT_WS
                                || state->toks[bi].type == TT_COMMENT)) {
            bi++;
        }

        bool a_done = (ai > a_last);
        bool b_done = (bi > b_last);

        if (a_done && b_done) {
            return true;
        }
        if (a_done != b_done) {
            return false;
        }

        // Compare tokens
        int         a_len, b_len;
        const char *a_text = tok_text_ptr(input, &state->toks[ai], &a_len);
        const char *b_text = tok_text_ptr(input, &state->toks[bi], &b_len);

        if (a_len != b_len || memcmp(a_text, b_text, a_len) != 0) {
            return false;
        }

        ai++;
        bi++;
    }
}

/**
 * @brief Match addition overflow guard: `expr_a > TYPE_MAX - expr_b`
 *        or reversed: `TYPE_MAX - expr_b < expr_a`.
 *
 * @param state    Lexer state
 * @param cond_start  Index of first token after `if (`
 * @param cond_end    Index of `)` closing the if-condition
 * @param a_first/a_last  Output: expr_a token range
 * @param b_first/b_last  Output: expr_b token range
 * @return true if matched
 */
static bool
match_add_guard(lex_t *state, int cond_start, int cond_end,
                int *a_first, int *a_last, int *b_first, int *b_last)
{
    ncc_buf_t *input = state->input;

    // Try Pattern A: expr_a > TYPE_MAX - expr_b
    {
        int lhs_first, lhs_last;
        int pos = collect_simple_expr(state, cond_start, &lhs_first, &lhs_last);

        if (pos < cond_end && lhs_first >= 0) {
            tok_t *op_tok = &state->toks[pos];
            if (tok_text_eq(input, op_tok, ">")) {
                int next = skip_ws_fwd(state, pos + 1);
                if (next < cond_end && state->toks[next].type == TT_ID
                    && is_type_max(input, &state->toks[next])) {
                    int minus = skip_ws_fwd(state, next + 1);
                    if (minus < cond_end && tok_text_eq(input, &state->toks[minus], "-")) {
                        int rhs_first, rhs_last;
                        int end = collect_simple_expr(state, minus + 1,
                                                     &rhs_first, &rhs_last);
                        if (end == cond_end && rhs_first >= 0) {
                            *a_first = lhs_first;
                            *a_last  = lhs_last;
                            *b_first = rhs_first;
                            *b_last  = rhs_last;
                            return true;
                        }
                    }
                }
            }
        }
    }

    // Try Pattern B (reversed): TYPE_MAX - expr_b < expr_a
    {
        int next = skip_ws_fwd(state, cond_start);
        if (next < cond_end && state->toks[next].type == TT_ID
            && is_type_max(input, &state->toks[next])) {
            int minus = skip_ws_fwd(state, next + 1);
            if (minus < cond_end && tok_text_eq(input, &state->toks[minus], "-")) {
                int rhs_first, rhs_last;
                int pos = collect_simple_expr(state, minus + 1, &rhs_first, &rhs_last);
                if (pos < cond_end && rhs_first >= 0
                    && tok_text_eq(input, &state->toks[pos], "<")) {
                    int lhs_first, lhs_last;
                    int end = collect_simple_expr(state, pos + 1, &lhs_first, &lhs_last);
                    if (end == cond_end && lhs_first >= 0) {
                        *a_first = lhs_first;
                        *a_last  = lhs_last;
                        *b_first = rhs_first;
                        *b_last  = rhs_last;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

/**
 * @brief Match subtraction overflow guard: `expr_a < TYPE_MIN + expr_b`
 */
static bool
match_sub_guard(lex_t *state, int cond_start, int cond_end,
                int *a_first, int *a_last, int *b_first, int *b_last)
{
    ncc_buf_t *input = state->input;

    int lhs_first, lhs_last;
    int pos = collect_simple_expr(state, cond_start, &lhs_first, &lhs_last);

    if (pos >= cond_end || lhs_first < 0) {
        return false;
    }

    if (!tok_text_eq(input, &state->toks[pos], "<")) {
        return false;
    }

    int next = skip_ws_fwd(state, pos + 1);
    if (next >= cond_end || state->toks[next].type != TT_ID
        || !is_type_min(input, &state->toks[next])) {
        return false;
    }

    int plus = skip_ws_fwd(state, next + 1);
    if (plus >= cond_end || !tok_text_eq(input, &state->toks[plus], "+")) {
        return false;
    }

    int rhs_first, rhs_last;
    int end = collect_simple_expr(state, plus + 1, &rhs_first, &rhs_last);

    if (end != cond_end || rhs_first < 0) {
        return false;
    }

    *a_first = lhs_first;
    *a_last  = lhs_last;
    *b_first = rhs_first;
    *b_last  = rhs_last;
    return true;
}

/**
 * @brief Match multiplication overflow guard:
 *        `expr_b != 0 && expr_a > TYPE_MAX / expr_b2`
 *        Verifies expr_b matches expr_b2.
 */
static bool
match_mul_guard(lex_t *state, int cond_start, int cond_end,
                int *a_first, int *a_last, int *b_first, int *b_last)
{
    ncc_buf_t *input = state->input;

    // Match: expr_b != 0 &&
    int b1_first, b1_last;
    int pos = collect_simple_expr(state, cond_start, &b1_first, &b1_last);

    if (pos >= cond_end || b1_first < 0) {
        return false;
    }

    int         ne_len;
    const char *ne_text = tok_text_ptr(input, &state->toks[pos], &ne_len);
    if (ne_len != 2 || memcmp(ne_text, "!=", 2) != 0) {
        return false;
    }

    int zero = skip_ws_fwd(state, pos + 1);
    if (zero >= cond_end || state->toks[zero].type != TT_NUM
        || !tok_text_eq(input, &state->toks[zero], "0")) {
        return false;
    }

    int ampamp = skip_ws_fwd(state, zero + 1);
    if (ampamp >= cond_end) {
        return false;
    }

    int         aa_len;
    const char *aa_text = tok_text_ptr(input, &state->toks[ampamp], &aa_len);
    if (aa_len != 2 || memcmp(aa_text, "&&", 2) != 0) {
        return false;
    }

    // Match: expr_a > TYPE_MAX / expr_b2
    int lhs_first, lhs_last;
    pos = collect_simple_expr(state, ampamp + 1, &lhs_first, &lhs_last);

    if (pos >= cond_end || lhs_first < 0) {
        return false;
    }

    if (!tok_text_eq(input, &state->toks[pos], ">")) {
        return false;
    }

    int max_ix = skip_ws_fwd(state, pos + 1);
    if (max_ix >= cond_end || state->toks[max_ix].type != TT_ID
        || !is_type_max(input, &state->toks[max_ix])) {
        return false;
    }

    int slash = skip_ws_fwd(state, max_ix + 1);
    if (slash >= cond_end || !tok_text_eq(input, &state->toks[slash], "/")) {
        return false;
    }

    int b2_first, b2_last;
    int end = collect_simple_expr(state, slash + 1, &b2_first, &b2_last);

    if (end != cond_end || b2_first < 0) {
        return false;
    }

    // Verify expr_b matches expr_b2
    if (!exprs_match(state, b1_first, b1_last, b2_first, b2_last)) {
        return false;
    }

    *a_first = lhs_first;
    *a_last  = lhs_last;
    *b_first = b1_first;
    *b_last  = b1_last;
    return true;
}

/**
 * @brief Skip past an if-body (either a braced block or a single statement).
 *
 * @param state  Lexer state
 * @param start  Index of first token after the `)` closing the if-condition
 * @return Index of first token after the body, or -1 on failure
 */
static int
skip_if_body(lex_t *state, int start)
{
    int ix = skip_ws_fwd(state, start);
    if (ix >= state->num_toks) {
        return -1;
    }

    if (tok_text_eq(state->input, &state->toks[ix], "{")) {
        // Braced block — skip to matching }
        int depth = 1;
        ix++;
        while (ix < state->num_toks && depth > 0) {
            if (tok_text_eq(state->input, &state->toks[ix], "{")) {
                depth++;
            }
            else if (tok_text_eq(state->input, &state->toks[ix], "}")) {
                depth--;
            }
            ix++;
        }
        return ix;
    }
    else {
        // Single statement — find the ;
        while (ix < state->num_toks) {
            if (tok_text_eq(state->input, &state->toks[ix], ";")) {
                return ix + 1;
            }
            ix++;
        }
        return -1;
    }
}

/**
 * @brief Look for `[type] result = expr_a OP expr_b ;` starting at `start`.
 *
 * Skips optional `else` clause first. The assignment may optionally have a
 * type specifier (e.g., `int result = a + b;`).
 *
 * @param state    Lexer state
 * @param start    Index to start searching from
 * @param op       Expected operator ('+', '-', '*')
 * @param a_first/a_last  Expected LHS operand token range
 * @param b_first/b_last  Expected RHS operand token range
 * @param assign_first  Output: first token of the assignment statement
 * @param assign_semi   Output: index of the semicolon
 * @param result_first  Output: first token of the result variable name
 * @param result_last   Output: last token of the result variable name
 * @param has_type      Output: true if the assignment has a type specifier
 * @param type_first    Output: first token of the type specifier
 * @return true if a matching assignment was found
 */
static bool
find_matching_assignment(lex_t *state, int start, char op,
                         int a_first, int a_last, int b_first, int b_last,
                         int *assign_first, int *assign_semi,
                         int *result_first, int *result_last,
                         bool *has_type, int *type_first)
{
    ncc_buf_t *input = state->input;
    int    ix    = skip_ws_fwd(state, start);

    if (ix >= state->num_toks) {
        return false;
    }

    // Skip optional else clause
    if (state->toks[ix].type == TT_KEYWORD
        && tok_text_eq(input, &state->toks[ix], "else")) {
        int after_else = skip_if_body(state, ix + 1);
        if (after_else < 0) {
            return false;
        }
        ix = skip_ws_fwd(state, after_else);
    }

    if (ix >= state->num_toks) {
        return false;
    }

    *assign_first = ix;
    *has_type     = false;

    // Check if this looks like a type + variable declaration
    // We look for patterns like: `int result =`, `unsigned long result =`, etc.
    // Simple heuristic: scan forward for `=`, check if there's a type before the var name.
    //
    // First, try to find `=` within a reasonable distance
    int eq_ix = -1;
    for (int scan = ix; scan < state->num_toks && scan < ix + 20; scan++) {
        if (state->toks[scan].type == TT_PUNCT
            && tok_text_eq(input, &state->toks[scan], "=")) {
            // Make sure it's not ==
            eq_ix = scan;
            break;
        }
        if (tok_text_eq(input, &state->toks[scan], ";")
            || tok_text_eq(input, &state->toks[scan], "{")
            || tok_text_eq(input, &state->toks[scan], "}")) {
            break;
        }
    }

    if (eq_ix < 0) {
        return false;
    }

    // Find the identifier just before `=` (the result variable)
    int var_ix = eq_ix - 1;
    while (var_ix >= ix
           && (state->toks[var_ix].type == TT_WS
               || state->toks[var_ix].type == TT_COMMENT)) {
        var_ix--;
    }

    if (var_ix < ix || state->toks[var_ix].type != TT_ID) {
        return false;
    }

    *result_first = var_ix;
    *result_last  = var_ix;

    // Check if there's a type before the variable name
    if (var_ix > ix) {
        int before_var = var_ix - 1;
        while (before_var >= ix
               && (state->toks[before_var].type == TT_WS
                   || state->toks[before_var].type == TT_COMMENT)) {
            before_var--;
        }
        if (before_var >= ix) {
            ttype_t t = state->toks[before_var].type;
            if (t == TT_KEYWORD || t == TT_ID) {
                *has_type   = true;
                *type_first = ix;
            }
        }
    }

    // Now check: after = we should have expr_a OP expr_b ;
    int rhs_start = skip_ws_fwd(state, eq_ix + 1);

    int ra_first, ra_last;
    int op_pos = collect_simple_expr(state, rhs_start, &ra_first, &ra_last);

    if (op_pos >= state->num_toks || ra_first < 0) {
        return false;
    }

    // Check operator
    char    op_str[2] = {op, '\0'};
    int     op_len;
    const char *op_text = tok_text_ptr(input, &state->toks[op_pos], &op_len);
    if (op_len != 1 || op_text[0] != op) {
        (void)op_str;
        return false;
    }

    int rb_first, rb_last;
    int semi = collect_simple_expr(state, op_pos + 1, &rb_first, &rb_last);

    if (semi >= state->num_toks || rb_first < 0) {
        return false;
    }

    if (!tok_text_eq(input, &state->toks[semi], ";")) {
        return false;
    }

    *assign_semi = semi;

    // Verify operands match (in either order for commutative ops)
    if (exprs_match(state, a_first, a_last, ra_first, ra_last)
        && exprs_match(state, b_first, b_last, rb_first, rb_last)) {
        return true;
    }

    // For addition and multiplication (commutative), try swapped order
    if (op == '+' || op == '*') {
        if (exprs_match(state, a_first, a_last, rb_first, rb_last)
            && exprs_match(state, b_first, b_last, ra_first, ra_last)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Find the closing `)` of an if-condition, starting after `if (`.
 *
 * @param state  Lexer state
 * @param open_paren  Index of the `(` token
 * @return Index of the matching `)`, or -1 if not found
 */
static int
find_cond_close(lex_t *state, int open_paren)
{
    int depth = 1;

    for (int i = open_paren + 1; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];
        if (tok->type == TT_WS || tok->type == TT_COMMENT) {
            continue;
        }

        if (tok_text_eq(state->input, tok, "(")) {
            depth++;
        }
        else if (tok_text_eq(state->input, tok, ")")) {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }

    return -1;
}

static void
xform_overflow_checks(lex_t *state, header_state_t *headers, bool conservative)
{
    ncc_buf_t *input = state->input;

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->type != TT_KEYWORD || !tok_text_eq(input, tok, "if")) {
            continue;
        }

        int if_ix    = i;
        int paren_ix = skip_ws_fwd(state, i + 1);
        if (paren_ix >= state->num_toks
            || !tok_text_eq(input, &state->toks[paren_ix], "(")) {
            continue;
        }

        int close_paren = find_cond_close(state, paren_ix);
        if (close_paren < 0) {
            continue;
        }

        int cond_start = skip_ws_fwd(state, paren_ix + 1);
        int cond_end   = close_paren; // collect_simple_expr stops at this )

        int     a_first, a_last, b_first, b_last;
        ovf_op_t op_type;
        char     op_char;
        const char *ckd_name;
        bool    matched = false;

        if (match_add_guard(state, cond_start, cond_end,
                            &a_first, &a_last, &b_first, &b_last)) {
            op_type  = OVF_ADD;
            op_char  = '+';
            ckd_name = "ckd_add";
            matched  = true;
        }
        else if (match_sub_guard(state, cond_start, cond_end,
                                 &a_first, &a_last, &b_first, &b_last)) {
            op_type  = OVF_SUB;
            op_char  = '-';
            ckd_name = "ckd_sub";
            matched  = true;
        }
        else if (match_mul_guard(state, cond_start, cond_end,
                                 &a_first, &a_last, &b_first, &b_last)) {
            op_type  = OVF_MUL;
            op_char  = '*';
            ckd_name = "ckd_mul";
            matched  = true;
        }

        if (!matched) {
            continue;
        }
        (void)op_type;

        // Skip past if-body to find the assignment
        int after_body = skip_if_body(state, close_paren + 1);
        if (after_body < 0) {
            continue;
        }

        int  assign_first, assign_semi;
        int  result_first, result_last;
        bool has_decl_type;
        int  decl_type_first;

        if (!find_matching_assignment(state, after_body, op_char,
                                      a_first, a_last, b_first, b_last,
                                      &assign_first, &assign_semi,
                                      &result_first, &result_last,
                                      &has_decl_type, &decl_type_first)) {
            continue;
        }

        // We have a match! Now rewrite.
        char *a_text = extract_token_range(state, a_first, a_last);
        char *b_text = extract_token_range(state, b_first, b_last);
        char *r_text = extract_token_range(state, result_first, result_last);

        if (conservative) {
            // Insert a comment before the if statement
            char comment[512];
            snprintf(comment, sizeof(comment),
                     "/* modernize: consider %s(&%s, %s, %s) */\n",
                     ckd_name, r_text, a_text, b_text);

            // Find the whitespace token just before the `if`
            int ws_ix = if_ix - 1;
            while (ws_ix >= 0 && state->toks[ws_ix].type == TT_COMMENT) {
                ws_ix--;
            }

            if (ws_ix >= 0 && state->toks[ws_ix].type == TT_WS) {
                // Prepend comment to existing whitespace
                int         ws_len;
                const char *ws_text = tok_text_ptr(input, &state->toks[ws_ix], &ws_len);
                int         clen    = strlen(comment);
                ncc_buf_t  *rep     = ncc_buf_alloc(ws_len + clen);
                memcpy(rep->data, ws_text, ws_len);
                memcpy(rep->data + ws_len, comment, clen);
                rep->data[ws_len + clen] = '\0';
                rep->len                 = ws_len + clen;
                state->toks[ws_ix].replacement = rep;
            }
            else {
                // No preceding whitespace — prepend comment to the if token
                int       clen = strlen(comment);
                int       klen = 2; // "if"
                ncc_buf_t *rep = ncc_buf_alloc(clen + klen);
                memcpy(rep->data, comment, clen);
                memcpy(rep->data + clen, "if", klen);
                rep->data[clen + klen] = '\0';
                rep->len               = clen + klen;
                tok->replacement       = rep;
            }
        }
        else {
            // Auto-rewrite mode

            if (has_decl_type) {
                // `int result = a + b;` after the if-body
                // -> emit `type result;\n` before the if, then ckd_* in the condition
                // Extract declaration text (type + variable name)
                char *decl_text = extract_token_range(state, decl_type_first,
                                                      result_last);

                // Build: "type result;\nif (ckd_OP(&result, a, b))"
                char new_cond[1024];
                int  nclen = snprintf(new_cond, sizeof(new_cond),
                                      "%s;\n%sif (%s(&%s, %s, %s))",
                                      decl_text,
                                      "", // indentation handled by clang-format
                                      ckd_name, r_text, a_text, b_text);
                base_dealloc(decl_text);

                if (nclen > 0 && nclen < (int)sizeof(new_cond)) {
                    set_replacement(tok, new_cond);
                    for (int x = paren_ix; x <= close_paren; x++) {
                        state->toks[x].skip_emit = 1;
                    }
                }

                // Skip entire assignment statement (including trailing WS)
                for (int x = assign_first; x <= assign_semi; x++) {
                    state->toks[x].skip_emit = 1;
                }
                if (assign_semi + 1 < state->num_toks
                    && state->toks[assign_semi + 1].type == TT_WS) {
                    state->toks[assign_semi + 1].skip_emit = 1;
                }
            }
            else {
                // Bare `result = a + b;` -> replace if condition, remove assignment
                char new_cond[1024];
                int  nclen = snprintf(new_cond, sizeof(new_cond),
                                      "if (%s(&%s, %s, %s))",
                                      ckd_name, r_text, a_text, b_text);

                if (nclen > 0 && nclen < (int)sizeof(new_cond)) {
                    set_replacement(tok, new_cond);
                    for (int x = paren_ix; x <= close_paren; x++) {
                        state->toks[x].skip_emit = 1;
                    }
                }

                // Remove entire assignment statement
                for (int x = assign_first; x <= assign_semi; x++) {
                    state->toks[x].skip_emit = 1;
                }
                if (assign_semi + 1 < state->num_toks
                    && state->toks[assign_semi + 1].type == TT_WS) {
                    state->toks[assign_semi + 1].skip_emit = 1;
                }
            }

            headers->need_stdckdint = true;
        }

        base_dealloc(a_text);
        base_dealloc(b_text);
        base_dealloc(r_text);
    }
}

// ============================================================
// #pragma once: replace header guards
// ============================================================

/**
 * @brief Extract guard name from #ifndef GUARD or #if !defined(GUARD)
 *        or #if !defined GUARD.
 *
 * @return true if matched, with name_out filled (null-terminated, up to name_sz-1 chars)
 */
static bool
preproc_extract_guard(ncc_buf_t *input, tok_t *tok, char *name_out, int name_sz)
{
    int         len;
    const char *text = tok_text_ptr(input, tok, &len);
    const char *p    = text;
    const char *end  = text + len;

    if (p >= end || *p != '#') {
        return false;
    }
    p++;

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Try #ifndef GUARD
    if (p + 6 <= end && strncmp(p, "ifndef", 6) == 0) {
        p += 6;
        if (p >= end || (*p != ' ' && *p != '\t')) {
            return false;
        }
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        const char *name_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        int name_len = p - name_start;
        if (name_len == 0 || name_len >= name_sz) {
            return false;
        }
        // Nothing significant after the name (just WS/newline)
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p != '\n' && *p != '\0') {
            return false;
        }
        memcpy(name_out, name_start, name_len);
        name_out[name_len] = '\0';
        return true;
    }

    // Try #if !defined(GUARD) or #if !defined GUARD
    if (p + 2 <= end && strncmp(p, "if", 2) == 0
        && (p[2] == ' ' || p[2] == '\t')) {
        p += 2;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= end || *p != '!') {
            return false;
        }
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p + 7 > end || strncmp(p, "defined", 7) != 0) {
            return false;
        }
        p += 7;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool has_paren = false;
        if (p < end && *p == '(') {
            has_paren = true;
            p++;
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
        }

        const char *name_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        int name_len = p - name_start;
        if (name_len == 0 || name_len >= name_sz) {
            return false;
        }

        if (has_paren) {
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (p >= end || *p != ')') {
                return false;
            }
            p++;
        }

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p != '\n' && *p != '\0') {
            return false;
        }
        memcpy(name_out, name_start, name_len);
        name_out[name_len] = '\0';
        return true;
    }

    return false;
}

/**
 * @brief Check if token is #define NAME with no value (just the guard define).
 */
static bool
preproc_is_define_only(ncc_buf_t *input, tok_t *tok, const char *name, int name_len)
{
    int         len;
    const char *text = tok_text_ptr(input, tok, &len);
    const char *p    = text;
    const char *end  = text + len;

    if (p >= end || *p != '#') {
        return false;
    }
    p++;

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (p + 6 > end || strncmp(p, "define", 6) != 0) {
        return false;
    }
    p += 6;

    if (p >= end || (*p != ' ' && *p != '\t')) {
        return false;
    }
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (p + name_len > end || memcmp(p, name, name_len) != 0) {
        return false;
    }
    p += name_len;

    // After the name, only whitespace/newline allowed (no value)
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p < end && *p != '\n' && *p != '\0') {
        return false;
    }

    return true;
}

/**
 * @brief Check if token is #endif (with optional trailing comment).
 */
static bool
preproc_is_endif(ncc_buf_t *input, tok_t *tok)
{
    int         len;
    const char *text = tok_text_ptr(input, tok, &len);
    const char *p    = text;
    const char *end  = text + len;

    if (p >= end || *p != '#') {
        return false;
    }
    p++;

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (p + 5 > end || strncmp(p, "endif", 5) != 0) {
        return false;
    }
    p += 5;

    // Allow optional whitespace, comment, newline after endif
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Optional comment: /* ... */ or // ...
    if (p < end && *p == '/') {
        // That's fine — rest of line is a comment
        return true;
    }

    if (p < end && *p != '\n' && *p != '\0') {
        return false;
    }

    return true;
}

/**
 * @brief Replace #ifndef/#define/#endif header guard with #pragma once.
 */
static void
xform_pragma_once(lex_t *state)
{
    ncc_buf_t *input = state->input;

    // Collect all preprocessor token indices
    int first_preproc = -1;
    int second_preproc = -1;
    int last_preproc = -1;

    for (int i = 0; i < state->num_toks; i++) {
        if (state->toks[i].type != TT_PREPROC) {
            continue;
        }
        if (first_preproc < 0) {
            first_preproc = i;
        }
        else if (second_preproc < 0) {
            second_preproc = i;
        }
        last_preproc = i;
    }

    // Need at least 3 preproc directives, and #define must be distinct from #endif
    if (first_preproc < 0 || second_preproc < 0 || last_preproc < 0
        || second_preproc == last_preproc) {
        return;
    }

    // 1. First preproc must be #ifndef GUARD or #if !defined(GUARD)
    char guard_name[256];
    if (!preproc_extract_guard(input, &state->toks[first_preproc],
                               guard_name, sizeof(guard_name))) {
        return;
    }

    int guard_len = strlen(guard_name);

    // 2. Second preproc must be #define GUARD (same name, no value)
    if (!preproc_is_define_only(input, &state->toks[second_preproc],
                                guard_name, guard_len)) {
        return;
    }

    // 3. Last preproc must be #endif
    if (!preproc_is_endif(input, &state->toks[last_preproc])) {
        return;
    }

    // 4. There must be content between the #define and the #endif
    bool has_content = false;
    for (int i = second_preproc + 1; i < last_preproc; i++) {
        ttype_t t = state->toks[i].type;
        if (t != TT_WS && t != TT_COMMENT) {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return;
    }

    // Replace first preproc (#ifndef) with #pragma once\n
    set_replacement(&state->toks[first_preproc], "#pragma once\n");

    // Skip the second preproc (#define GUARD) and its trailing whitespace
    state->toks[second_preproc].skip_emit = 1;
    if (second_preproc + 1 < state->num_toks
        && state->toks[second_preproc + 1].type == TT_WS) {
        state->toks[second_preproc + 1].skip_emit = 1;
    }

    // Skip the last preproc (#endif) and its trailing whitespace
    state->toks[last_preproc].skip_emit = 1;
    if (last_preproc + 1 < state->num_toks
        && state->toks[last_preproc + 1].type == TT_WS) {
        state->toks[last_preproc + 1].skip_emit = 1;
    }

    // Also skip the trailing whitespace after the first preproc since
    // our replacement already includes the newline
    if (first_preproc + 1 < state->num_toks
        && state->toks[first_preproc + 1].type == TT_WS) {
        state->toks[first_preproc + 1].skip_emit = 1;
    }
}

// ============================================================
// Public entry point
// ============================================================

void
mod_token_xforms(lex_t *state, int *insert_after_ix, char **header_text,
                 bool conservative_overflow, const modernize_skip_t *skip)
{
    header_state_t headers = {};

    // Tier 1
    if (!skip->skip_keywords) {
        xform_keyword_replacements(state);
    }
    xform_include_removal(state, &headers, skip->skip_includes);
    if (!skip->skip_elifdef) {
        xform_elifdef(state);
    }

    // Tier 2
    if (!skip->skip_attributes) {
        xform_attributes(state);
    }
    if (!skip->skip_builtins) {
        xform_builtins(state, &headers);
    }
    if (!skip->skip_empty_init) {
        xform_empty_init(state);
    }
    if (!skip->skip_va_paste) {
        xform_va_args_paste(state);
    }
    if (!skip->skip_va_start) {
        xform_va_start(state);
    }

    // Overflow check -> ckd_* rewrite
    if (!skip->skip_overflow) {
        xform_overflow_checks(state, &headers, conservative_overflow);
    }

    // #pragma once
    if (!skip->skip_pragma_once) {
        xform_pragma_once(state);
    }

    // Compute header insertion info
    // Always run include_removal for header tracking even when skipping removals,
    // but skip the removal part. The function already ran above; headers are tracked.
    compute_needed_headers(state, &headers);

    *insert_after_ix = headers.insert_after_ix;
    *header_text     = build_header_text(&headers);
}
