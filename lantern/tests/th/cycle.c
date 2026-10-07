#include <stdio.h>
#include <pthread.h>

#include "th/typehash.h"

typedef struct {
    th_ctx *ctx;
    th_type *type;
    uint64_t hash;
} nfc_worker_t;

static void *hash_on_worker(void *data)
{
    nfc_worker_t *worker = data;
    worker->hash = th_hash(worker->ctx, worker->type);
    return nullptr;
}

static bool cross_thread_nfc_hash(void)
{
    th_ctx *ctx = th_ctx_new();
    th_install_nfc(ctx);
    nfc_worker_t worker = {.ctx = ctx, .type = th_name(ctx, "cafe\xcc\x81")};
    pthread_t thread;
    bool ok = pthread_create(&thread, nullptr, hash_on_worker, &worker) == 0;
    if (ok) ok = pthread_join(thread, nullptr) == 0;
    if (ok) ok = worker.hash == th_hash(ctx, th_name(ctx, "caf\xc3\xa9"));
    th_ctx_free(ctx);
    return ok;
}

int main(void)
{
    if (!cross_thread_nfc_hash()) return 1;
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
