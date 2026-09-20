#include <stdlib.h>
#include <utf8proc.h>

#include "th/typehash.h"

typedef struct {
    utf8proc_uint8_t *buf;
} nfc_state;

static bool nfc(void *state, const char *in, size_t inlen, const char **out,
                size_t *outlen)
{
    nfc_state        *st = state;
    utf8proc_uint8_t *r  = nullptr;
    utf8proc_ssize_t  n  = utf8proc_map((const utf8proc_uint8_t *)in,
                                        (utf8proc_ssize_t)inlen, &r,
                                        UTF8PROC_STABLE | UTF8PROC_COMPOSE);

    if (n < 0) {
        return false;
    }

    free(st->buf);
    st->buf = r;
    *out    = (const char *)r;
    *outlen = (size_t)n;

    return true;
}

static _Thread_local nfc_state g_nfc;

void th_install_nfc(th_ctx *ctx)
{
    th_ctx_set_normalizer(ctx, nfc, &g_nfc);
}
