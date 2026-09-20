#include <stdio.h>
#include "th/typehash.h"

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    th_member im = {.name = "y", .type = th_name(ctx, "int")};
    th_type  *in = th_record(ctx, &im, 1);
    th_member om = {.name = "x", .type = in};
    th_type  *ou = th_record(ctx, &om, 1);

    const uint8_t *p; size_t n;
    th_encode(ctx, ou, &p, &n);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
    return 0;
}
