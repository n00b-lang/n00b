#define N00B_USE_INTERNAL_API
// n00b_thread_attach_foreign() (the bounds-less foreign attach the gateway
// uses) is behind the __N00B_THREAD_INTERNAL gate, exactly as the gateway's
// ingest.c enables it.
#define __N00B_THREAD_INTERNAL 1

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"

// ============================================================================
// Regression: a BOUNDS-LESS foreign thread must be RESOLVABLE by
// n00b_thread_self().
//
// A raw pthread (the Crayon gateway's libdispatch/XPC pattern) attaches via
// n00b_thread_attach_foreign() with NO stack bounds, so its slot's stack_lo
// stays null and it is deliberately absent from the GC stack scan.  Pre-fix
// the n00b_thread_self() foreign slow path SKIPPED any null-stack_lo slot, so
// self() never resolved this thread; the read-hold adopt inside
// n00b_thread_init then passed that null self into find_read_lock_record,
// dereferencing null->record (SIGSEGV at offsetof(n00b_thread_t, record)) —
// the crayon-gw crash on the crayon-es subscription path.
//
// Post-fix the slot is published with its os_tid before its live bit, and the
// foreign slow path resolves a null-stack_lo slot by os_tid identity.  This
// test attaches such a thread and asserts self() resolves to it, then runs a
// gc-framed allocation (which re-acquires the STW gate read-hold — the path
// that would otherwise deadlock for an unresolvable thread).
// ============================================================================

static void *
foreign_fn(void *raw)
{
    // Unattached foreign thread: brackets no live slot, so self() must resolve
    // to nullptr WITHOUT faulting.
    assert(n00b_thread_self() == nullptr);

    // Bounds-less foreign attach (no stack range passed).  Pre-fix this
    // SIGSEGV'd inside the read-hold adopt; it must now return a usable thread.
    n00b_thread_t *attached = n00b_thread_attach_foreign();
    assert(attached != nullptr);

    // The fix: a bounds-less foreign thread is now resolvable by identity, so
    // n00b_thread_self() returns the SAME thread the attach registered.
    n00b_thread_t *self = n00b_thread_self();
    assert(self == attached);
    printf("  bounds-less foreign self() = %p (resolved; null/crash pre-fix)\n",
           (void *)self);

    // Exercise the gate reentrancy that previously crashed/deadlocked: a
    // gc-framed allocation re-acquires the STW gate read-hold.
    n00b_string_t *s = n00b_string_from_cstr("bounds-less foreign attach ok");
    assert(s != nullptr);
    printf("  gc-framed call after attach ok: \"%s\"\n", s->data);

    return raw;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running thread_self_boundsless_foreign test...\n");

    pthread_t t;
    int       rc = pthread_create(&t, nullptr, foreign_fn, nullptr);
    assert(rc == 0);
    pthread_join(t, nullptr);

    printf("All thread_self_boundsless_foreign tests passed.\n");
    n00b_shutdown();
    return 0;
}
