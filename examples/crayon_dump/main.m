/**
 * @file examples/crayon_dump/main.m
 * @brief Verify the n00b → Crayon warehouse subscriber wiring.
 *
 * Streams every CRAYON_WH_EVENT_NORMALIZED dictionary from a running
 * Crayon daemon and prints one summary line per event.  Used to
 * confirm the wire-level subscriber works before the binary-classifier
 * demo pulls in n00b types and the TUI.
 *
 * Run: ./crayon_dump   (Ctrl-C to stop)
 */

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpc/xpc.h>

#include "crayon_protocol.h"
#include "crayon_subscriber.h"

static _Atomic bool g_done = false;
static _Atomic uint64_t g_count = 0;

static void
on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_done, true);
}

static const char *
xpc_str(xpc_object_t d, const char *k)
{
    if (!d || xpc_get_type(d) != XPC_TYPE_DICTIONARY) return NULL;
    return xpc_dictionary_get_string(d, k);
}

static int64_t
xpc_i64(xpc_object_t d, const char *k)
{
    if (!d || xpc_get_type(d) != XPC_TYPE_DICTIONARY) return 0;
    return xpc_dictionary_get_int64(d, k);
}

static uint64_t
xpc_u64(xpc_object_t d, const char *k)
{
    if (!d || xpc_get_type(d) != XPC_TYPE_DICTIONARY) return 0;
    return xpc_dictionary_get_uint64(d, k);
}

static void
on_event(xpc_object_t evt, void *user_data)
{
    (void)user_data;
    xpc_type_t tt = xpc_get_type(evt);
    if (tt == XPC_TYPE_ERROR) {
        const char *desc = xpc_dictionary_get_string(evt, XPC_ERROR_KEY_DESCRIPTION);
        fprintf(stderr, "crayon_dump: error event: %s\n", desc ? desc : "?");
        return;
    }
    if (tt != XPC_TYPE_DICTIONARY) return;

    uint64_t event_type = xpc_u64(evt, CRAYON_SVC_KEY_EVENT_TYPE);
    if (event_type != CRAYON_WH_EVENT_NORMALIZED) {
        // Operational rollup; skip in dump mode.
        return;
    }

    const char  *kind = xpc_str(evt, "kind");
    uint64_t     ts   = xpc_u64(evt, "ts_ns");
    xpc_object_t actor = xpc_dictionary_get_value(evt, "actor");
    const char  *exe  = xpc_str(actor, "exe_path");
    int64_t      pid  = xpc_i64(actor, "pid");

    uint64_t n = atomic_fetch_add(&g_count, 1) + 1;
    printf("[%llu] ts=%llu kind=%s pid=%lld exe=%s\n",
           (unsigned long long)n,
           (unsigned long long)ts,
           kind ? kind : "?",
           (long long)pid,
           exe ? exe : "-");
    fflush(stdout);
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
            "crayon_dump: subscribing to %s — Ctrl-C to stop\n",
            CRAYON_WAREHOUSE_MACH_SERVICE_NAME);

    crayon_subscriber_t *s = crayon_subscriber_open(on_event, NULL);
    if (!s) {
        fprintf(stderr,
                "crayon_dump: failed to open warehouse subscription "
                "(is the Crayon daemon running?)\n");
        return 1;
    }

    while (!atomic_load(&g_done)) {
        usleep(100 * 1000);
    }

    fprintf(stderr,
            "\ncrayon_dump: stopping after %llu events, %llu reconnects\n",
            (unsigned long long)atomic_load(&g_count),
            (unsigned long long)crayon_subscriber_interrupt_count(s));

    crayon_subscriber_close(s);
    return 0;
}

#else

#include <stdio.h>
int main(void) {
    fprintf(stderr, "crayon_dump is macOS-only.\n");
    return 1;
}

#endif // __APPLE__
