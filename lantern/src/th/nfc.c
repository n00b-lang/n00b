#include "private/unicode/ucd.h"
#include "lib/alloc.h"
#include "th/typehash.h"

static bool nfc(void *state, core_string_t in, core_string_t *out)
{
    (void)state;
    bool ascii = true;
    for (size_t i = 0; i < in.u8_bytes && ascii; i++) {
        ascii = (unsigned char)in.data[i] < 0x80;
    }
    if (!ascii) {
        core_string_t normalized = ucd_normalize(in, UCD_NFC);
        bool composed = normalized.data && core_string_eq(normalized, in);
        core_free(normalized.data);
        if (!composed) {
            return false;
        }
    }
    *out = in;
    return true;
}

void th_install_nfc(th_ctx *ctx)
{
    th_ctx_set_normalizer(ctx, nfc, nullptr);
}
