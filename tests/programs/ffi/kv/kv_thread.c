#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <stddef.h>
#include <stdint.h>

#include "kv.h"

void kv_free(void *data);

static void
kv_thread_run(int *status)
{
    const uint8_t key[]   = {'t', 'k'};
    const uint8_t value[] = {'v'};
    kv_db        *db      = NULL;
    uint8_t      *found   = NULL;
    size_t        length  = 0;
    *status               = 1;
    if (kv_open("thread", &db) == KV_OK && kv_put(db, key, 2, value, 1) == KV_OK
        && kv_get(db, key, 2, &found, &length) == KV_OK && length == 1 && found[0] == 'v') {
        kv_free(found);
        kv_close(db);
        *status = 0;
    }
}

#if defined(_WIN32)
static DWORD WINAPI
kv_thread_body(void *result)
{
    kv_thread_run(result);
    return 0;
}

int
kv_thread_check(void)
{
    int    status = 2;
    HANDLE thread = CreateThread(NULL, 0, kv_thread_body, &status, 0, NULL);
    if (!thread) {
        return 3;
    }
    DWORD waited = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return waited == WAIT_OBJECT_0 ? status : 3;
}
#else
static void *
kv_thread_body(void *result)
{
    kv_thread_run(result);
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
#endif
