/**
 * @file utils.c
 * @brief Shared utilities: token allocation, compiler discovery, diagnostics.
 *
 * Provides token array management, `ncc_find_compiler()`
 * (searches `NCC_COMPILER` / `CC` / fallback), newline counting, and
 * diagnostic helpers (`ncc_error`, `ncc_warning`).
 */
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <unistd.h>

#include "xform.h"
#include "types.h"

#define INITIAL_TOK_CAP 8192

tok_t *
alloc_tokens(ncc_buf_t *b, int *out_cap)
{
    (void)b;
    *out_cap = INITIAL_TOK_CAP;
    return base_calloc(INITIAL_TOK_CAP, sizeof(tok_t));
}

void
lex_ensure_tok_space(lex_t *state)
{
    if (state->num_toks < state->toks_cap) {
        return;
    }
    int new_cap = state->toks_cap * 2;
    tok_t *new_toks = base_realloc(state->toks, new_cap * sizeof(tok_t));
    memset(new_toks + state->toks_cap, 0,
           (new_cap - state->toks_cap) * sizeof(tok_t));
    state->toks     = new_toks;
    state->toks_cap = new_cap;
}

int
count_newlines(lex_t *state, tok_t *t)
{
    int   result = 0;
    char *p;
    char *end;

    if (t->replacement) {
        p   = t->replacement->data;
        end = p + t->replacement->len;
    }
    else {
        p   = &state->input->data[t->offset];
        end = p + t->len;
    }

    while (p < end) {
        if (*p++ == '\n') {
            result++;
        }
    }

    return result;
}

// This requires being handed the index of the two bounding
// parentheses of a formal declaration (and NOT a prototype).
// Its goal is to extract the *names* of each variable, so that
// we can use them to have a wrapper invoke the ACTUAL implementation.
//
// Currently, we use this for the 'once' function only, but I know I'm
// eventually going to want to use it to support automatic compile
// time 'detours', so that we can add code to look at inputs / outputs
// for debugging purposes.
//
// There will probably be other uses too.
//
// This does a quick scan front-to-back, counting commas to determine
// how big of a list to allocate. Then, it actually works by starting
// at the end and working backword, since, for each argument, the name
// is the last thing we see.
//
// Note that this function requires each param slot to have a name,
// and so will 100% fail on functions declared varargs. This is
// intentional, since we cannot statically proxy this, even with our
// preprocessor.
//
// If we see a preproc directive in here, we also bail.
//
// We also bail if there isn't an ID where we expect to see it.
//
// We otherwise ignore stuff we don't understand.

ncc_list_t *
get_wrapper_actuals(tok_xform_t *state, int lparen_ix, int rparen_ix)
{
    int     arg_count = 0;
    bool    found_arg = false;
    ncc_list_t *result;
    int     i;
    tok_t  *t;

    for (i = lparen_ix + 1; i < rparen_ix; i++) {
        t = &state->toks[i];

        switch (t->type) {
        case TT_ID:
        case TT_KEYWORD:
            if (!found_arg) {
                found_arg = true;
                arg_count++;
            }
            continue;
        case TT_PREPROC:
            ncc_error(
                "%s:%d: Cannot have a preprocessor directive inside a function "
                "declaration for something being transformed.\n",
                state->in_file ? state->in_file : "<unknown>",
                t->line_no);
            exit(-1);
        case TT_PUNCT:
            if (state->input->data[t->offset] == ',') {
                if (!found_arg) {
                    ncc_error("%s:%d: Could not find an argument before ','\n",
                              state->in_file ? state->in_file : "<unknown>",
                              t->line_no);
                    exit(-1);
                }
                found_arg = false;
                continue;
            }
        default:
            continue;
        }
    }

    result    = ncc_list_alloc(arg_count);
    found_arg = false;

    while (arg_count && i > lparen_ix) {
        t = &state->toks[--i];
        switch (t->type) {
        case TT_ID:
        case TT_KEYWORD:
            if (!found_arg) {
                found_arg                  = true;
                result->data[--arg_count] = extract(state->input, t);
                // Special case `foo(void) {`
                if (result->len == 1 && !strcmp(result->data[0], "void")) {
                    base_dealloc(result->data[0]);
                    result->data[0] = nullptr;
                    result->len     = 0;
                }
            }
            continue;
        case TT_WS:
        case TT_COMMENT:
            continue;
        default:
            if (!found_arg) {
missing_name:
                ncc_error("%s:%d: Could not find a parameter name.\n",
                          state->in_file ? state->in_file : "<unknown>",
                          t->line_no);
                exit(-1);
            }

            if (state->input->data[t->offset] == ',') {
                found_arg = false;
            }
            continue;
        }
    }
    if (arg_count) {
        goto missing_name;
    }

    return result;
}

char *
join(ncc_list_t *str_list, char *joiner)
{
    if (!str_list->len) {
        return base_calloc(1, 1);
    }

    int jlen   = strlen(joiner);
    int reslen = 0;

    for (int i = 0; i < str_list->len; i++) {
        reslen += strlen(str_list->data[i]);
    }

    reslen += jlen * (str_list->len - 1);

    char *result = base_calloc(1, reslen + 1);
    char *p      = result;

    for (int i = 0; i < str_list->len; i++) {
        // Add joiner before all items except the first
        if (i > 0 && jlen) {
            memcpy(p, joiner, jlen);
            p += jlen;
        }
        int l = strlen(str_list->data[i]);
        if (l) {
            memcpy(p, str_list->data[i], l);
            p += l;
        }
    }

    return result;
}
