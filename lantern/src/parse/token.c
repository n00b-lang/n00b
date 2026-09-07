#include "parse/token.h"
#include "lib/alloc.h"
#include <assert.h>

void
parse_trivia_free(parse_trivia_t *t)
{
    while (t) {
        parse_trivia_t *next = t->next;

        core_free(t);
        t = next;
    }
}

int32_t
parse_token_list_build_ptrs(core_list_t(parse_token_info_t) *tl, parse_token_info_ptr_t **out)
{
    assert(tl);
    assert(out);

    int32_t n = (int32_t)tl->len;

    parse_token_info_ptr_t *ptrs = core_alloc_array(parse_token_info_ptr_t, n);

    for (int32_t i = 0; i < n; i++) {
        ptrs[i] = &tl->data[i];
    }

    *out = ptrs;
    return n;
}
