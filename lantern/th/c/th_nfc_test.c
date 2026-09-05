#include <stdio.h>
#include <string.h>

#include "typehash.h"

void th_install_nfc(th_ctx *ctx);

int main(void)
{
    const char *forms[] = {"caf\xc3\xa9", "cafe\xcc\x81", "int"};

    for (int i = 0; i < 3; i++) {
        th_ctx *ctx = th_ctx_new();

        th_install_nfc(ctx);

        th_error err = {0};
        th_type *t   = th_parse(ctx, forms[i], &err);
        uint64_t h   = t ? th_hash(ctx, t) : 0;

        printf("%-14s %llu%s\n", i == 0 ? "NFC" : i == 1 ? "NFD" : "ascii",
               (unsigned long long)h,
               (t && h) ? "" : " <-- FAILED");
        th_ctx_free(ctx);
    }

    return 0;
}
