#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xform.h"
#include "types.h"

#define MMAP_PROTS (PROT_READ | PROT_WRITE)
#define MMAP_FLAGS (MAP_PRIVATE | MAP_ANON)

tok_t *
alloc_tokens(ncc_buf_t *b)
{
    int len = sizeof(tok_t) * b->len;
    int ps  = getpagesize();

    if (len % ps) {
        int pages = (len / ps) + 1;
        len       = pages * ps;
    }

    return mmap(nullptr, len, MMAP_PROTS, MMAP_FLAGS, -1, 0);
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

list_t *
get_wrapper_actuals(xform_t *state, int lparen_ix, int rparen_ix)
{
    int     arg_count = 0;
    bool    found_arg = false;
    list_t *result;
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

    result    = list_alloc(arg_count);
    found_arg = false;

    while (arg_count && i > lparen_ix) {
        t = &state->toks[--i];
        switch (t->type) {
        case TT_ID:
        case TT_KEYWORD:
            if (!found_arg) {
                found_arg                  = true;
                result->items[--arg_count] = extract(state->input, t);
                // Special case `foo(void) {`
                if (result->nitems == 1 && !strcmp(result->items[0], "void")) {
                    base_dealloc(result->items[0]);
                    result->items[0] = nullptr;
                    result->nitems   = 0;
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
join(list_t *str_list, char *joiner)
{
    if (!str_list->nitems) {
        return base_calloc(1, 1);
    }

    int jlen   = strlen(joiner);
    int reslen = 0;

    for (int i = 0; i < str_list->nitems; i++) {
        reslen += strlen(str_list->items[i]);
    }

    reslen += jlen * (str_list->nitems - 1);

    char *result = base_calloc(1, reslen + 1);
    char *p      = result;

    for (int i = 0; i < str_list->nitems; i++) {
        // Add joiner before all items except the first
        if (i > 0 && jlen) {
            memcpy(p, joiner, jlen);
            p += jlen;
        }
        int l = strlen(str_list->items[i]);
        if (l) {
            memcpy(p, str_list->items[i], l);
            p += l;
        }
    }

    return result;
}
