/**
 * @file mod_tree_xforms.c
 * @brief Phase B: Context-aware transforms for --modernize.
 *
 * Currently implements NULL -> nullptr replacement using token-level
 * analysis. Preprocessor directives (#define bodies) are skipped to
 * avoid breaking macros that rely on NULL being a macro.
 *
 * Returns the number of changes made, and writes the result to *result.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "argv_parse.h"
#include "buf.h"
#include "lex.h"
#include "token.h"

/**
 * @brief Skip whitespace/comment tokens backward.
 * @return Index of previous non-WS/comment token, or -1 if beginning.
 */
static int
skip_ws_back(lex_t *state, int ix)
{
    ix--;
    while (ix >= 0) {
        ttype_t t = state->toks[ix].type;
        if (t != TT_WS && t != TT_COMMENT) {
            return ix;
        }
        ix--;
    }
    return -1;
}

/**
 * @brief Check if replacing NULL at null_ix would be unsafe.
 *
 * Returns true if the surrounding context suggests NULL is used in a
 * non-pointer context (arithmetic, bitwise ops, sizeof, integer cast,
 * numeric comparison).
 */
static bool
is_unsafe_null_context(lex_t *state, ncc_buf_t *input, int null_ix)
{
    int prev_ix = skip_ws_back(state, null_ix);
    int next_ix = null_ix + 1;

    // skip_ws_fwd equivalent inline (no dependency on mod_token_xforms.c)
    while (next_ix < state->num_toks) {
        ttype_t t = state->toks[next_ix].type;
        if (t != TT_WS && t != TT_COMMENT) {
            break;
        }
        next_ix++;
    }
    if (next_ix >= state->num_toks) {
        next_ix = -1;
    }

    // Check previous token
    if (prev_ix >= 0) {
        int         plen;
        const char *ptext = tok_text_ptr(input, &state->toks[prev_ix], &plen);
        ttype_t     ptype = state->toks[prev_ix].type;

        // sizeof before NULL (direct: sizeof NULL)
        if (ptype == TT_KEYWORD && plen == 6 && memcmp(ptext, "sizeof", 6) == 0) {
            return true;
        }

        // sizeof(NULL) — prev is '(', check if '(' is preceded by sizeof
        if (ptype == TT_PUNCT && plen == 1 && ptext[0] == '(') {
            int before_paren = skip_ws_back(state, prev_ix);
            if (before_paren >= 0) {
                int         blen;
                const char *btext = tok_text_ptr(input, &state->toks[before_paren], &blen);
                if (state->toks[before_paren].type == TT_KEYWORD
                    && blen == 6 && memcmp(btext, "sizeof", 6) == 0) {
                    return true;
                }
            }
        }

        if (ptype == TT_PUNCT) {
            // Arithmetic: + - * / %
            if (plen == 1) {
                char c = ptext[0];
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
                    return true;
                }
                // Bitwise: & | ^ ~
                if (c == '&' || c == '|' || c == '^' || c == '~') {
                    return true;
                }
            }
            // Shift: << >>
            if (plen == 2 && ((ptext[0] == '<' && ptext[1] == '<')
                           || (ptext[0] == '>' && ptext[1] == '>'))) {
                return true;
            }
            // Compound assignment: += -= *= /= %= &= |= ^=
            if (plen == 2 && ptext[1] == '=') {
                char c = ptext[0];
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%'
                    || c == '&' || c == '|' || c == '^') {
                    return true;
                }
            }
            // Shift-assign: <<= >>=
            if (plen == 3 && ptext[2] == '='
                && ((ptext[0] == '<' && ptext[1] == '<')
                 || (ptext[0] == '>' && ptext[1] == '>'))) {
                return true;
            }

            // Integer cast: (int)NULL, (unsigned long)NULL, etc.
            if (plen == 1 && ptext[0] == ')') {
                // Scan backwards to find matching (
                int depth = 1;
                int k     = prev_ix - 1;
                while (k >= 0 && depth > 0) {
                    int         klen;
                    const char *ktext = tok_text_ptr(input, &state->toks[k], &klen);
                    if (state->toks[k].type == TT_PUNCT) {
                        if (klen == 1 && ktext[0] == ')') {
                            depth++;
                        }
                        else if (klen == 1 && ktext[0] == '(') {
                            depth--;
                        }
                    }
                    if (depth > 0) {
                        k--;
                    }
                }

                if (k >= 0 && depth == 0) {
                    // Check if tokens between ( and ) are integer type keywords
                    bool all_int_types = true;
                    bool found_type    = false;
                    for (int m = k + 1; m < prev_ix; m++) {
                        ttype_t mt = state->toks[m].type;
                        if (mt == TT_WS || mt == TT_COMMENT) {
                            continue;
                        }
                        if (mt == TT_PUNCT) {
                            // Pointer star means it's a pointer cast — safe
                            int         mlen;
                            const char *mtext = tok_text_ptr(input, &state->toks[m], &mlen);
                            if (mlen == 1 && mtext[0] == '*') {
                                all_int_types = false;
                                break;
                            }
                        }
                        if (mt == TT_KEYWORD) {
                            int         mlen;
                            const char *mtext = tok_text_ptr(input, &state->toks[m], &mlen);
                            if ((mlen == 3 && memcmp(mtext, "int", 3) == 0)
                                || (mlen == 4 && memcmp(mtext, "long", 4) == 0)
                                || (mlen == 5 && memcmp(mtext, "short", 5) == 0)
                                || (mlen == 8 && memcmp(mtext, "unsigned", 8) == 0)
                                || (mlen == 6 && memcmp(mtext, "signed", 6) == 0)
                                || (mlen == 4 && memcmp(mtext, "char", 4) == 0)) {
                                found_type = true;
                            }
                            else {
                                all_int_types = false;
                                break;
                            }
                        }
                        else if (mt == TT_ID) {
                            int         mlen;
                            const char *mtext = tok_text_ptr(input, &state->toks[m], &mlen);
                            if ((mlen == 9 && memcmp(mtext, "uintptr_t", 9) == 0)
                                || (mlen == 8 && memcmp(mtext, "intptr_t", 8) == 0)
                                || (mlen == 9 && memcmp(mtext, "ptrdiff_t", 9) == 0)
                                || (mlen == 6 && memcmp(mtext, "size_t", 6) == 0)) {
                                found_type = true;
                            }
                            else {
                                all_int_types = false;
                                break;
                            }
                        }
                        else {
                            all_int_types = false;
                            break;
                        }
                    }
                    if (all_int_types && found_type) {
                        return true;
                    }
                }
            }

            // Comparison with numeric literal: 0 == NULL or 0 != NULL
            if (plen == 2 && (memcmp(ptext, "==", 2) == 0
                           || memcmp(ptext, "!=", 2) == 0)) {
                int before_cmp = skip_ws_back(state, prev_ix);
                if (before_cmp >= 0 && state->toks[before_cmp].type == TT_NUM) {
                    return true;
                }
            }
        }
    }

    // Check next token
    if (next_ix >= 0) {
        int         nlen;
        const char *ntext = tok_text_ptr(input, &state->toks[next_ix], &nlen);
        ttype_t     ntype = state->toks[next_ix].type;

        if (ntype == TT_PUNCT) {
            // Arithmetic: + - * / %
            if (nlen == 1) {
                char c = ntext[0];
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
                    return true;
                }
                // Bitwise: & | ^ ~
                if (c == '&' || c == '|' || c == '^') {
                    return true;
                }
            }
            // Shift: << >>
            if (nlen == 2 && ((ntext[0] == '<' && ntext[1] == '<')
                           || (ntext[0] == '>' && ntext[1] == '>'))) {
                return true;
            }

            // Comparison with numeric literal: NULL == 0 or NULL != 0
            if (nlen == 2 && (memcmp(ntext, "==", 2) == 0
                           || memcmp(ntext, "!=", 2) == 0)) {
                int after_cmp = next_ix + 1;
                while (after_cmp < state->num_toks) {
                    ttype_t t = state->toks[after_cmp].type;
                    if (t != TT_WS && t != TT_COMMENT) {
                        break;
                    }
                    after_cmp++;
                }
                if (after_cmp < state->num_toks
                    && state->toks[after_cmp].type == TT_NUM) {
                    return true;
                }
            }
        }
    }

    return false;
}

/**
 * @brief Replace NULL with nullptr in a source buffer.
 *
 * Re-tokenizes the Phase A output, scans for TT_ID tokens with text "NULL"
 * that are not inside preprocessor directives, replaces them, and
 * reconstructs the source. Skips replacement when surrounding context
 * indicates a non-pointer use (arithmetic, bitwise, sizeof, integer cast,
 * numeric comparison).
 *
 * Also replaces the pattern (void *)0 and (void*)0 with nullptr.
 *
 * @param ctx   Parsed command-line arguments
 * @param source Phase A output buffer
 * @param result Output: modified source buffer (caller must free)
 * @return Number of NULL->nullptr replacements made
 */
int
mod_tree_xforms(ncc_argv_t *ctx, ncc_buf_t *source, ncc_buf_t **result)
{
    (void)ctx;

    lex_t lex_state;
    lex_init(&lex_state, source, "«modernize»");
    lex(&lex_state);

    int changes = 0;

    for (int i = 0; i < lex_state.num_toks; i++) {
        tok_t *tok = &lex_state.toks[i];

        // Skip tokens inside preprocessor directives
        if (tok->type == TT_PREPROC) {
            continue;
        }

        if (tok->type != TT_ID) {
            continue;
        }

        int         len;
        const char *text = tok_text_ptr(source, tok, &len);

        if (len == 4 && memcmp(text, "NULL", 4) == 0) {
            if (is_unsafe_null_context(&lex_state, source, i)) {
                continue;
            }
            int        rlen = 7;
            ncc_buf_t *rep  = ncc_buf_alloc(rlen);
            memcpy(rep->data, "nullptr", rlen);
            rep->data[rlen] = '\0';
            rep->len        = rlen;
            tok->replacement = rep;
            changes++;
        }
    }

    // Also look for (void *)0 / (void*)0 pattern
    for (int i = 0; i < lex_state.num_toks; i++) {
        tok_t *tok = &lex_state.toks[i];

        if (tok->type != TT_PUNCT) {
            continue;
        }

        int         plen;
        const char *ptext = tok_text_ptr(source, tok, &plen);
        if (plen != 1 || ptext[0] != '(') {
            continue;
        }

        // Match: ( void * ) 0
        int j = i + 1;
        while (j < lex_state.num_toks
               && (lex_state.toks[j].type == TT_WS
                   || lex_state.toks[j].type == TT_COMMENT)) {
            j++;
        }
        if (j >= lex_state.num_toks) {
            continue;
        }

        int         vlen;
        const char *vtext = tok_text_ptr(source, &lex_state.toks[j], &vlen);
        if (lex_state.toks[j].type != TT_KEYWORD || vlen != 4
            || memcmp(vtext, "void", 4) != 0) {
            continue;
        }
        int void_ix = j;

        j++;
        while (j < lex_state.num_toks
               && (lex_state.toks[j].type == TT_WS
                   || lex_state.toks[j].type == TT_COMMENT)) {
            j++;
        }
        if (j >= lex_state.num_toks) {
            continue;
        }

        int         slen;
        const char *stext = tok_text_ptr(source, &lex_state.toks[j], &slen);
        if (slen != 1 || stext[0] != '*') {
            continue;
        }
        int star_ix = j;

        j++;
        while (j < lex_state.num_toks
               && (lex_state.toks[j].type == TT_WS
                   || lex_state.toks[j].type == TT_COMMENT)) {
            j++;
        }
        if (j >= lex_state.num_toks) {
            continue;
        }

        int         clen;
        const char *ctext = tok_text_ptr(source, &lex_state.toks[j], &clen);
        if (clen != 1 || ctext[0] != ')') {
            continue;
        }
        int close_ix = j;

        j++;
        while (j < lex_state.num_toks
               && (lex_state.toks[j].type == TT_WS
                   || lex_state.toks[j].type == TT_COMMENT)) {
            j++;
        }
        if (j >= lex_state.num_toks) {
            continue;
        }

        int         zlen;
        const char *ztext = tok_text_ptr(source, &lex_state.toks[j], &zlen);
        if (lex_state.toks[j].type != TT_NUM || zlen != 1 || ztext[0] != '0') {
            continue;
        }
        int zero_ix = j;

        // Replace the opening ( with nullptr, skip the rest
        int        rlen = 7;
        ncc_buf_t *rep  = ncc_buf_alloc(rlen);
        memcpy(rep->data, "nullptr", rlen);
        rep->data[rlen] = '\0';
        rep->len        = rlen;
        tok->replacement = rep;

        // Skip void, *, ), and 0
        lex_state.toks[void_ix].skip_emit  = 1;
        lex_state.toks[star_ix].skip_emit  = 1;
        lex_state.toks[close_ix].skip_emit = 1;
        lex_state.toks[zero_ix].skip_emit  = 1;

        // Also skip whitespace between them
        for (int x = i + 1; x < zero_ix; x++) {
            if (lex_state.toks[x].type == TT_WS) {
                lex_state.toks[x].skip_emit = 1;
            }
        }

        changes++;
    }

    if (changes == 0) {
        *result = nullptr;
        return 0;
    }

    // Reconstruct
    ncc_buf_t *out = ncc_buf_alloc(source->len + 256);

    for (int i = 0; i < lex_state.num_toks; i++) {
        tok_t *t = &lex_state.toks[i];

        if (t->skip_emit) {
            continue;
        }

        if (t->replacement) {
            out = ncc_buf_concat(out, t->replacement->data, t->replacement->len);
        }
        else {
            out = ncc_buf_concat(out, source->data + t->offset, t->len);
        }
    }

    *result = out;
    return changes;
}
