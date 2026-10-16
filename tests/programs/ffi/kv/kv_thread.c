#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "kv.h"

void kv_free(void *data);

static void *
kv_thread_body(void *result)
{
    const uint8_t key[]   = {'t', 'k'};
    const uint8_t value[] = {'v'};
    kv_db        *db      = NULL;
    uint8_t      *found   = NULL;
    size_t        length  = 0;
    int          *status  = result;
    *status               = 1;
    if (kv_open("thread", &db) == KV_OK && kv_put(db, key, 2, value, 1) == KV_OK
        && kv_get(db, key, 2, &found, &length) == KV_OK && length == 1 && found[0] == 'v') {
        kv_free(found);
        kv_close(db);
        *status = 0;
    }
    return NULL;
}

int
kv_thread_check(void)
{
    pthread_t thread;
    int       status = 2;
    if (pthread_create(&thread, NULL, kv_thread_body, &status) != 0 || pthread_join(thread, NULL) != 0) {
        return 3;
    }
    return status;
}
