// The global lock only needs to run at times where it's important
// that all threads are stopped / synchronized.
//
// This is mainly (but not exclusively) when we need to do garbage
// collection; we don't want threads to be mucking w/ memory while we
// are copying data.
//
// We generally would like to avoid contention and cache
// synchronization, so instead of one global lock, the STW works like
// this:
//
// There is one 'stop the world' mutex that must be acquired before
// you can actually begin the process. Then, when you have the mutex,
// you iterate through the global list of threads, and set the STW
// bit in their self-lock futex. Every thread that checks in or.

// 'self-lock' futex to the value 0xffffffff.
//
// Every time a thread is about to call a function that will suspend
// it, any time it uses memory management routines, and every loop
// through the interpreter, the thread will store ~0 into
// thread->wait_futex to indicate it's in the 'self-futex'. If the
// sentinel has been added to the futex by the STW, then that thread
// will block until the STW finishes the stop the world process.
//
// After the STW runs through all threads once, it effectively
// busy-waits.  It keeps iterating through all threads, looking to see
// if they're suspended on their own futex, or otherwise waiting.
//
// For waiting threads, they also ensure wait on their self-lock when
// they resume, so once the STW thread has placed the lock, as long as
// the thread is still in a wait state, all is good.
//
// At that point, the STW thread proceeds, until it is time to restart
// the world.
//
// When it comes time to do that, it goes back through and resets
// the self-lock for each thread to its expected value.
//
// For any thread that was blocked on their self-lock, they then
// ensure that the STW is unlocked before proceeding.
//
// We could use a RW lock for the STW instead; that may end up just as
// good; yes, there'd be a lot more reading of the same memory
// address, but it wouldn't often be modified (on stop-the-world,
// suspend and resume), so probably not much of an issue.
//
// Still, this is simple enough, and doesn't allow nesting of suspends
// / resumes, which I prefer. I might not change it.

#define N00B_USE_INTERNAL_API

#include <time.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"
#include "core/futex.h"

static inline int32_t
get_tid()
{
    return __n00b_thread_self.id_info.parts.id;
}

void
_n00b_stop_the_world(char *loc)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    _n00b_thread_suspend(loc);

    int32_t tid   = get_tid();
    int32_t owner = n00b_atomic_load(&rt->stw);

    if (owner == tid) {
        rt->stw_nesting++;
        return;
    }

    int32_t expected;

    do {
        int32_t cur;

        while ((cur = (int32_t)n00b_atomic_load(&rt->stw)) != N00B_NO_OWNER) {
            uint32_t generation = n00b_atomic_load(&rt->stw_generation);
            n00b_futex_wait(&rt->stw_generation, generation, 1000 * 1000);
        }

        expected = N00B_NO_OWNER;
    } while (!n00b_cas(&rt->stw, (uint32_t *)&expected, tid));

    assert(n00b_atomic_load(&rt->stw) == (uint32_t)tid);

    int n = (int)rt->max_threads;

    // Loop through every single thread and add the STW bit to their
    // state, which will alert them to go wait on the STW.

    n00b_thread_t *t;

    while (n--) {
        t = n00b_atomic_load(&rt->threads[n].thread);
        if (!t || t == n00b_thread_self()) {
            continue;
        }

        n00b_atomic_or(&t->self_lock, N00B_STW);
    }

    // Wait for every other live thread to enter a state where its
    // callee-saved registers are spilled to its scanned stack: either
    // BLOCKING (called wait_for_stw_release, which checkpointed them) or
    // SUSPEND (cooperatively suspended at a known-safe point and
    // checkpointed them).  Without this barrier the world isn't actually
    // stopped — the initiator returns and the GC proceeds while other
    // threads are still running with live heap pointers in registers
    // the GC can't see.  Plain busy-wait: threads flip their bit at
    // their next checkin / safepoint, which is generally microseconds.
    n = (int)rt->max_threads;

    while (n--) {
        while (true) {
            t = n00b_atomic_load(&rt->threads[n].thread);
            if (!t || t == n00b_thread_self()) {
                break;
            }

            uint32_t state = n00b_atomic_load(&t->self_lock);
            if (state & (N00B_SUSPEND | N00B_BLOCKING)) {
                break;
            }

            // A thread can exit between the first STW marking pass and this
            // barrier.  Reload the runtime slot each turn so we do not spin
            // forever on a stale TLS pointer after the exiting thread clears
            // its registration.
            n00b_atomic_or(&t->self_lock, N00B_STW);
        }
    }

    rt->stw_nesting = 1;

    _n00b_thread_resume(loc);
}

void
_n00b_restart_the_world(char *loc)
{
    int32_t         tid   = get_tid();
    n00b_runtime_t *rt    = n00b_get_runtime();
    int32_t         owner = n00b_atomic_load(&rt->stw);

    if (owner != tid) {
        abort();
    }

    assert(rt->stw_nesting > 0);

    if (--rt->stw_nesting) {
        return;
    }

    n00b_barrier();
    int n = (int)rt->max_threads;

    n00b_thread_t *t;

    while (n--) {
        t = n00b_atomic_load(&rt->threads[n].thread);
        if (!t || t == n00b_thread_self()) {
            continue;
        }

        n00b_atomic_and(&t->self_lock, ~N00B_STW);
    }

    atomic_store(&rt->stw, N00B_NO_OWNER);

    n00b_atomic_add(&rt->stw_generation, 1);
    n00b_futex_wake(&rt->stw_generation, true);
    n00b_futex_wake(&rt->stw, true);
}

const struct timespec stw_check_timeout = {
    .tv_sec  = 0,
    .tv_nsec = 10000,
};

void
n00b_wait_for_stw_release(void)
{
    n00b_runtime_t *rt         = n00b_get_runtime();
    n00b_jmp_buf_t  save_state = {};

    n00b_capture_stack_top(n00b_thread_self());
    int32_t  cur        = n00b_atomic_load(&rt->stw);
    uint32_t generation = n00b_atomic_load(&rt->stw_generation);

    if (cur == get_tid()) {
        return;
    }

    if (!n00b_setjmp(&save_state)) {
        // n00b_setjmp() must capture callee-saved registers BEFORE we
        // announce N00B_BLOCKING — the STW initiator uses that bit
        // to decide save_state is ready to scan.  Swapping the order
        // races: initiator sees the bit, scans an uninitialised
        // save_state.
        n00b_atomic_or(&__n00b_thread_self.self_lock, N00B_BLOCKING);

        while (cur != N00B_NO_OWNER) {
            // Use the version that doesn't check the STW when it's
            // signaled.  Wait on a generation futex instead of the owner
            // word: the same owner can run consecutive STW cycles, so the
            // owner value has an ABA shape (tid -> no-owner -> same tid).
            n00b_futex_wait_timespec(&rt->stw_generation,
                                     generation,
                                     (void *)&stw_check_timeout);
            generation = n00b_atomic_load(&rt->stw_generation);
            cur        = n00b_atomic_load(&rt->stw);
        }

        n00b_longjmp(&save_state, 1);
    }
    uint32_t old_state = n00b_atomic_and(&__n00b_thread_self.self_lock,
                                         ~N00B_BLOCKING);
    uint32_t new_state = n00b_atomic_load(&__n00b_thread_self.self_lock);
    int32_t  owner     = n00b_atomic_load(&rt->stw);

    if (((old_state | new_state) & N00B_STW)
        && owner != N00B_NO_OWNER
        && owner != get_tid()) {
        n00b_thread_checkin();
    }
}

void
n00b_thread_checkin(void)
{
    int val = n00b_atomic_load(&__n00b_thread_self.self_lock);

    if (!val) {
        return;
    }

    if (n00b_atomic_load(&__n00b_thread_self.self_lock) & N00B_STW) {
        // n00b_wait_for_stw_release sets N00B_BLOCKING itself, after
        // n00b_setjmp() has captured callee-saved registers.  Setting it here
        // first races: the STW initiator may see the bit and start
        // scanning before save_state is initialised.
        n00b_wait_for_stw_release();
    }
}

// 'suspend' means we are about to go into some kind of state where we
// might not check in (at least not soon). We actually can still keep
// running, but we promise that we are not doing ANYTHING in scope for
// a STW. So we cannot be using heap memory or locks, etc.
//
// The intent here is we call this, then call sleep(), poll() or
// any other potentially blocking call we need to make, then call
// n00b_thread_resume() immediately on getting woken.
void
_n00b_thread_suspend(char *loc)
{
    n00b_atomic_or(&__n00b_thread_self.self_lock, N00B_SUSPEND);
}

void
_n00b_thread_resume(char *loc)
{
    (void)loc;

    // Threads can try to resume during a STW. We should go ahead and
    // turn off 'SUSPEND', but then we will hit the STW when we do the
    // check-in at the end.
    //
    // However, the STW thread should quick-return; it shouldn't have
    // any contention, unless it is trying to access a resource that's
    // already locked at the time of the STW, which should be a no-no.
    //
    // If that happens, the system will deadlock.
    n00b_runtime_t *rt = n00b_get_runtime();

    n00b_atomic_and(&__n00b_thread_self.self_lock, ~N00B_SUSPEND);

    int val = n00b_atomic_load(&rt->stw);
    if (val == get_tid()) {
        return;
    }

    n00b_thread_checkin();
}
