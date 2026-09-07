#include <stdio.h>

#include "th/typehash.h"

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type  *A  = th_record_stub(ctx);
    th_type  *pA = th_ind(ctx, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, A);
    th_member ma = {.name = "next", .type = pA};

    th_record_fill(ctx, A, &ma, 1);

    th_type  *B  = th_record_stub(ctx);
    th_type  *C  = th_record_stub(ctx);
    th_type  *pC = th_ind(ctx, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, C);
    th_type  *pB = th_ind(ctx, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, B);
    th_member mb = {.name = "next", .type = pC};
    th_member mc = {.name = "next", .type = pB};

    th_record_fill(ctx, B, &mb, 1);
    th_record_fill(ctx, C, &mc, 1);

    const uint8_t *p;
    size_t         n;

    th_encode(ctx, A, &p, &n);
    printf("A = (next: ptr[A])        %llu\t", (unsigned long long)th_hash(ctx, A));

    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");

    th_encode(ctx, B, &p, &n);
    printf("B = (next: ptr[C]), C...  %llu\t", (unsigned long long)th_hash(ctx, B));

    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");

    th_ctx_free(ctx);

    return 0;
}
