// token.c - Token list operations.
#include "slay/token.h"
#include <assert.h>

int32_t
n00b_token_list_build_ptrs(n00b_list_t(n00b_token_info_t) *tl, n00b_token_info_ptr_t **out)
{
    assert(tl);
    assert(out);

    int32_t n = (int32_t)tl->len;

    n00b_token_info_ptr_t *ptrs = n00b_alloc_array(n00b_token_info_ptr_t, n);

    for (int32_t i = 0; i < n; i++) {
        ptrs[i] = &tl->data[i];
    }

    *out = ptrs;
    return n;
}
