/*
 * io_wsa.c - WSAPoll I/O backend for Windows
 *
 * Uses WSAPoll for socket event notification, with software timers,
 * process monitoring (RegisterWaitForSingleObject), directory watching
 * (ReadDirectoryChangesW), and user events (CreateEvent).
 *
 * A self-connected loopback socket pair provides cross-thread wakeup
 * since WSAPoll has no equivalent of eventfd.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "conduit/io.h"
#include "conduit/timer.h"
#include "conduit/user_event.h"
#include "core/stw.h"

#include <io.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Timer tracking (software timers, same approach as io_poll.c)
// ============================================================================

typedef struct wsa_timer {
    n00b_conduit_timer_t *timer;
    uint64_t              next_fire;  // Monotonic ms
    struct wsa_timer     *next;
} wsa_timer_t;

// ============================================================================
// Process monitoring
// ============================================================================

typedef struct wsa_proc {
    n00b_conduit_proc_watch_t *watch;
    HANDLE                     proc_handle;
    HANDLE                     wait_handle;
    volatile LONG              fired;
    struct wsa_proc           *next;
} wsa_proc_t;

// ============================================================================
// Vnode (directory change) monitoring
// ============================================================================

typedef struct wsa_vnode {
    n00b_conduit_vnode_watch_t *watch;
    HANDLE                      dir_handle;
    OVERLAPPED                  overlapped;
    uint8_t                     buf[4096];
    volatile LONG               fired;
    struct wsa_vnode           *next;
} wsa_vnode_t;

// ============================================================================
// User events
// ============================================================================

typedef struct wsa_user_event {
    n00b_conduit_user_event_t  *event;
    HANDLE                      win_event;
    struct wsa_user_event      *next;
} wsa_user_event_t;

// ============================================================================
// WSA backend context
// ============================================================================

#define WSA_INITIAL_CAPACITY 64

typedef struct {
    n00b_conduit_t   *conduit;
    WSAPOLLFD        *pollfds;
    n00b_conduit_io_target_t **targets;
    int               nfds;
    int               cap;
    wsa_timer_t      *timers;
    wsa_proc_t       *procs;
    wsa_vnode_t      *vnodes;
    wsa_user_event_t *user_events;
    SOCKET            wakeup_rd;
    SOCKET            wakeup_wr;
} wsa_ctx_t;

// ============================================================================
// Monotonic time helper
// ============================================================================

static uint64_t
wsa_monotonic_ms(void)
{
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
}

// ============================================================================
// Wakeup socket pair (self-connected TCP loopback)
// ============================================================================

static bool
wsa_create_wakeup_pair(SOCKET *rd_out, SOCKET *wr_out)
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listener);
        return false;
    }

    int addrlen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR) {
        closesocket(listener);
        return false;
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return false;
    }

    SOCKET writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer == INVALID_SOCKET) {
        closesocket(listener);
        return false;
    }

    if (connect(writer, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(writer);
        closesocket(listener);
        return false;
    }

    SOCKET reader = accept(listener, NULL, NULL);
    closesocket(listener);
    if (reader == INVALID_SOCKET) {
        closesocket(writer);
        return false;
    }

    // Make reader non-blocking
    u_long mode = 1;
    if (ioctlsocket(reader, FIONBIO, &mode) == SOCKET_ERROR) {
        closesocket(reader);
        closesocket(writer);
        return false;
    }

    *rd_out = reader;
    *wr_out = writer;
    return true;
}

static void
wsa_wakeup(wsa_ctx_t *ctx)
{
    // Best-effort wakeup -- ignore return value
    char c = 1;
    (void)send(ctx->wakeup_wr, &c, 1, 0);
}

static void
wsa_drain_wakeup(wsa_ctx_t *ctx)
{
    // wakeup_rd is non-blocking; recv returns WSAEWOULDBLOCK when drained
    char buf[64];
    while (recv(ctx->wakeup_rd, buf, sizeof(buf), 0) > 0) {}
}

// ============================================================================
// FD set helpers
// ============================================================================

static int
wsa_find_fd(wsa_ctx_t *ctx, int fd)
{
    for (int i = 0; i < ctx->nfds; i++) {
        if (ctx->pollfds[i].fd == (SOCKET)fd) {
            return i;
        }
    }
    return -1;
}

static bool
wsa_grow(wsa_ctx_t *ctx)
{
    int new_cap = ctx->cap * 2;
    if (new_cap < WSA_INITIAL_CAPACITY) {
        new_cap = WSA_INITIAL_CAPACITY;
    }

    WSAPOLLFD *new_fds = calloc(new_cap, sizeof(WSAPOLLFD));
    if (!new_fds) return false;

    n00b_conduit_io_target_t **new_tgt = calloc(new_cap,
                                                sizeof(n00b_conduit_io_target_t *));
    if (!new_tgt) {
        free(new_fds);
        return false;
    }

    if (ctx->pollfds && ctx->nfds > 0) {
        memcpy(new_fds, ctx->pollfds, ctx->nfds * sizeof(WSAPOLLFD));
        memcpy(new_tgt, ctx->targets,
               ctx->nfds * sizeof(n00b_conduit_io_target_t *));
    }

    free(ctx->pollfds);
    free(ctx->targets);

    ctx->pollfds = new_fds;
    ctx->targets = new_tgt;
    ctx->cap      = new_cap;
    return true;
}

static short
ops_to_wsa_events(n00b_conduit_io_op_t ops)
{
    short events = 0;
    if (ops & N00B_CONDUIT_IO_READ)  events |= POLLIN;
    if (ops & N00B_CONDUIT_IO_WRITE) events |= POLLOUT;
    return events;
}

static n00b_conduit_io_op_t
wsa_events_to_ops(short revents)
{
    n00b_conduit_io_op_t ops = 0;
    if (revents & POLLIN)   ops |= N00B_CONDUIT_IO_READ;
    if (revents & POLLOUT)  ops |= N00B_CONDUIT_IO_WRITE;
    if (revents & POLLERR)  ops |= N00B_CONDUIT_IO_ERROR;
    if (revents & POLLHUP)  ops |= N00B_CONDUIT_IO_HUP;
    if (revents & POLLNVAL) ops |= N00B_CONDUIT_IO_ERROR;
    return ops;
}

// ============================================================================
// WSA startup (called from io.c before any backend init)
// ============================================================================

bool
n00b_wsa_ensure_init(void)
{
    static atomic_int done = 0;
    if (atomic_load(&done)) {
        return true;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        atomic_store(&done, 1);
        return true;
    }
    return false;
}

// ============================================================================
// Backend operations
// ============================================================================

static void *
wsa_init(n00b_conduit_t *c)
{
    wsa_ctx_t *ctx = n00b_alloc(wsa_ctx_t);
    if (!ctx) return nullptr;

    ctx->conduit     = c;
    ctx->pollfds     = nullptr;
    ctx->targets     = nullptr;
    ctx->nfds        = 0;
    ctx->cap         = 0;
    ctx->timers      = nullptr;
    ctx->procs       = nullptr;
    ctx->vnodes      = nullptr;
    ctx->user_events = nullptr;
    ctx->wakeup_rd   = INVALID_SOCKET;
    ctx->wakeup_wr   = INVALID_SOCKET;

    // Create wakeup socket pair
    if (!wsa_create_wakeup_pair(&ctx->wakeup_rd, &ctx->wakeup_wr)) {
        return nullptr;
    }

    // Add wakeup reader to poll set
    if (!wsa_grow(ctx)) return nullptr;

    int idx = ctx->nfds++;
    ctx->pollfds[idx].fd      = ctx->wakeup_rd;
    ctx->pollfds[idx].events  = POLLIN;
    ctx->pollfds[idx].revents = 0;
    ctx->targets[idx]         = nullptr;

    return ctx;
}

static void
wsa_cleanup(void *vctx)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx) return;

    if (ctx->wakeup_rd != INVALID_SOCKET) closesocket(ctx->wakeup_rd);
    if (ctx->wakeup_wr != INVALID_SOCKET) closesocket(ctx->wakeup_wr);

    // Clean up process watches
    for (wsa_proc_t *pp = ctx->procs; pp; pp = pp->next) {
        if (pp->wait_handle) {
            UnregisterWaitEx(pp->wait_handle, INVALID_HANDLE_VALUE);
        }
        if (pp->proc_handle) {
            CloseHandle(pp->proc_handle);
        }
    }

    // Clean up vnode watches
    for (wsa_vnode_t *vn = ctx->vnodes; vn; vn = vn->next) {
        if (vn->dir_handle != INVALID_HANDLE_VALUE) {
            CancelIo(vn->dir_handle);
            CloseHandle(vn->dir_handle);
        }
    }

    // Clean up user events
    for (wsa_user_event_t *ue = ctx->user_events; ue; ue = ue->next) {
        if (ue->win_event) CloseHandle(ue->win_event);
    }

    free(ctx->pollfds);
    ctx->pollfds = nullptr;
    free(ctx->targets);
    ctx->targets = nullptr;
}

static bool
wsa_add(void *vctx, int fd, n00b_conduit_io_op_t ops,
        n00b_conduit_io_target_t *target)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx) return false;

    int idx = wsa_find_fd(ctx, fd);
    if (idx >= 0) {
        ctx->pollfds[idx].events = ops_to_wsa_events(ops);
        ctx->targets[idx]        = target;
        return true;
    }

    if (ctx->nfds >= ctx->cap) {
        if (!wsa_grow(ctx)) return false;
    }

    idx = ctx->nfds++;
    ctx->pollfds[idx].fd      = (SOCKET)fd;
    ctx->pollfds[idx].events  = ops_to_wsa_events(ops);
    ctx->pollfds[idx].revents = 0;
    ctx->targets[idx]         = target;
    return true;
}

static bool
wsa_modify(void *vctx, int fd, n00b_conduit_io_op_t ops,
           n00b_conduit_io_target_t *target)
{
    (void)target; // WSA preserves target via ctx->targets array
    wsa_ctx_t *ctx = vctx;
    if (!ctx) return false;

    int idx = wsa_find_fd(ctx, fd);
    if (idx < 0) return false;

    ctx->pollfds[idx].events = ops_to_wsa_events(ops);
    return true;
}

static bool
wsa_remove(void *vctx, int fd)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx) return false;

    int idx = wsa_find_fd(ctx, fd);
    if (idx < 0) return true; // Idempotent removal (matches poll backend)

    int last = ctx->nfds - 1;
    if (idx != last) {
        ctx->pollfds[idx]  = ctx->pollfds[last];
        ctx->targets[idx] = ctx->targets[last];
    }
    ctx->nfds--;
    return true;
}

static n00b_string_t *
wsa_name(void)
{
    return r"wsa";
}

// ============================================================================
// Timer support (software timers)
// ============================================================================

static bool
wsa_timer_add(void *vctx, n00b_conduit_timer_t *timer)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !timer) return false;

    wsa_timer_t *wt = n00b_alloc(wsa_timer_t);
    if (!wt) return false;

    wt->timer     = timer;
    wt->next_fire = wsa_monotonic_ms() + timer->interval_ms;
    wt->next      = ctx->timers;
    ctx->timers   = wt;
    return true;
}

static void
wsa_timer_remove(void *vctx, n00b_conduit_timer_t *timer)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !timer) return;

    wsa_timer_t **pp = &ctx->timers;
    while (*pp) {
        if ((*pp)->timer == timer) {
            wsa_timer_t *wt = *pp;
            *pp = wt->next;
            timer->cancelled = true;
            return;
        }
        pp = &(*pp)->next;
    }
}

static int
wsa_process_timers(wsa_ctx_t *ctx, int timeout_ms)
{
    if (!ctx->timers) return timeout_ms;

    uint64_t now = wsa_monotonic_ms();
    int min_timeout = timeout_ms;

    wsa_timer_t **pp = &ctx->timers;
    while (*pp) {
        wsa_timer_t          *wt    = *pp;
        n00b_conduit_timer_t *timer = wt->timer;

        if (timer->cancelled) {
            *pp = wt->next;
            continue;
        }

        if (now >= wt->next_fire) {
            n00b_conduit_timer_fire(timer);
            if (timer->repeating && !timer->cancelled) {
                wt->next_fire = now + timer->interval_ms;
                pp = &wt->next;
            } else {
                *pp = wt->next;
                continue;
            }
        } else {
            int ms_left = (int)(wt->next_fire - now);
            if (min_timeout < 0 || ms_left < min_timeout) {
                min_timeout = ms_left;
            }
            pp = &wt->next;
        }
    }

    return min_timeout;
}

// ============================================================================
// Process monitoring
// ============================================================================

static VOID CALLBACK
wsa_proc_wait_callback(PVOID param, BOOLEAN timed_out)
{
    (void)timed_out;
    wsa_proc_t *wp = param;
    InterlockedExchange(&wp->fired, 1);
}

static bool
wsa_proc_add(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !watch) return false;

    HANDLE ph = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, (DWORD)watch->pid);
    if (!ph) return false;

    wsa_proc_t *wp = n00b_alloc(wsa_proc_t);
    if (!wp) {
        CloseHandle(ph);
        return false;
    }

    wp->watch       = watch;
    wp->proc_handle = ph;
    wp->wait_handle = NULL;
    wp->fired       = 0;
    wp->next        = ctx->procs;
    ctx->procs      = wp;

    if (!RegisterWaitForSingleObject(&wp->wait_handle, ph,
                                     wsa_proc_wait_callback, wp,
                                     INFINITE, WT_EXECUTEONLYONCE)) {
        CloseHandle(ph);
        return false;
    }

    return true;
}

static void
wsa_proc_remove(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !watch) return;

    wsa_proc_t **pp = &ctx->procs;
    while (*pp) {
        if ((*pp)->watch == watch) {
            wsa_proc_t *wp = *pp;
            *pp = wp->next;
            if (wp->wait_handle) {
                UnregisterWaitEx(wp->wait_handle, INVALID_HANDLE_VALUE);
            }
            CloseHandle(wp->proc_handle);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void
wsa_process_procs(wsa_ctx_t *ctx)
{
    for (wsa_proc_t *wp = ctx->procs; wp; wp = wp->next) {
        if (!InterlockedCompareExchange(&wp->fired, 0, 1)) {
            continue;
        }
        DWORD exit_code = 0;
        GetExitCodeProcess(wp->proc_handle, &exit_code);
        n00b_conduit_proc_fire(wp->watch, N00B_CONDUIT_PROC_EXIT,
                               (int)exit_code);
    }
}

// ============================================================================
// Vnode monitoring (ReadDirectoryChangesW)
// ============================================================================

static void
wsa_start_directory_watch(wsa_vnode_t *vn);

static bool
wsa_vnode_add(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !watch) return false;

    // Recover path from FD via GetFinalPathNameByHandle
    HANDLE fh = (HANDLE)_get_osfhandle(watch->fd);
    if (fh == INVALID_HANDLE_VALUE) return false;

    wchar_t wpath[MAX_PATH];
    DWORD len = GetFinalPathNameByHandleW(fh, wpath, MAX_PATH,
                                          FILE_NAME_NORMALIZED);
    if (len == 0 || len >= MAX_PATH) return false;

    // Open directory handle with FILE_FLAG_OVERLAPPED
    // First check if the path is a directory; if not, get its parent
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    wchar_t dir_path[MAX_PATH];
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        wcscpy_s(dir_path, MAX_PATH, wpath);
    } else {
        // Get parent directory
        wcscpy_s(dir_path, MAX_PATH, wpath);
        wchar_t *last_sep = wcsrchr(dir_path, L'\\');
        if (!last_sep) last_sep = wcsrchr(dir_path, L'/');
        if (last_sep) *last_sep = L'\0';
        else return false;
    }

    HANDLE dir = CreateFileW(dir_path, FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             NULL);
    if (dir == INVALID_HANDLE_VALUE) return false;

    wsa_vnode_t *vn = n00b_alloc(wsa_vnode_t);
    if (!vn) {
        CloseHandle(dir);
        return false;
    }

    vn->watch      = watch;
    vn->dir_handle = dir;
    vn->fired      = 0;
    vn->next       = ctx->vnodes;
    ctx->vnodes    = vn;
    memset(&vn->overlapped, 0, sizeof(vn->overlapped));

    wsa_start_directory_watch(vn);
    return true;
}

static void
wsa_start_directory_watch(wsa_vnode_t *vn)
{
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME
                 | FILE_NOTIFY_CHANGE_DIR_NAME
                 | FILE_NOTIFY_CHANGE_ATTRIBUTES
                 | FILE_NOTIFY_CHANGE_SIZE
                 | FILE_NOTIFY_CHANGE_LAST_WRITE;

    ReadDirectoryChangesW(vn->dir_handle, vn->buf, sizeof(vn->buf),
                          FALSE, filter, NULL, &vn->overlapped, NULL);
}

static void
wsa_vnode_remove(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !watch) return;

    wsa_vnode_t **pp = &ctx->vnodes;
    while (*pp) {
        if ((*pp)->watch == watch) {
            wsa_vnode_t *vn = *pp;
            *pp = vn->next;
            CancelIo(vn->dir_handle);
            CloseHandle(vn->dir_handle);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void
wsa_process_vnodes(wsa_ctx_t *ctx)
{
    for (wsa_vnode_t *vn = ctx->vnodes; vn; vn = vn->next) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(vn->dir_handle, &vn->overlapped, &bytes,
                                 FALSE)) {
            continue;
        }
        if (bytes == 0) continue;

        // Parse FILE_NOTIFY_INFORMATION entries
        FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)vn->buf;
        uint32_t ops = 0;

        for (;;) {
            switch (info->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME:
                ops |= N00B_CONDUIT_VNODE_WRITE;
                break;
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME:
                ops |= N00B_CONDUIT_VNODE_DELETE;
                break;
            case FILE_ACTION_MODIFIED:
                ops |= N00B_CONDUIT_VNODE_WRITE;
                break;
            }

            if (info->NextEntryOffset == 0) break;
            info = (FILE_NOTIFY_INFORMATION *)((uint8_t *)info
                                               + info->NextEntryOffset);
        }

        if (ops) {
            n00b_conduit_vnode_fire(vn->watch, ops);
        }

        // Restart the watch
        wsa_start_directory_watch(vn);
    }
}

// ============================================================================
// User events
// ============================================================================

static bool
wsa_user_event_add(void *vctx, n00b_conduit_user_event_t *event)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !event) return false;

    HANDLE evt = CreateEventW(NULL, FALSE, FALSE, NULL); // Auto-reset
    if (!evt) return false;

    wsa_user_event_t *we = n00b_alloc(wsa_user_event_t);
    if (!we) {
        CloseHandle(evt);
        return false;
    }

    we->event        = event;
    we->win_event    = evt;
    we->next         = ctx->user_events;
    ctx->user_events = we;
    return true;
}

static void
wsa_user_event_remove(void *vctx, n00b_conduit_user_event_t *event)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !event) return;

    wsa_user_event_t **pp = &ctx->user_events;
    while (*pp) {
        if ((*pp)->event == event) {
            wsa_user_event_t *we = *pp;
            *pp = we->next;
            CloseHandle(we->win_event);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void
wsa_user_event_trigger(void *vctx, n00b_conduit_user_event_t *event)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !event) return;

    for (wsa_user_event_t *we = ctx->user_events; we; we = we->next) {
        if (we->event->event_id == event->event_id) {
            SetEvent(we->win_event);
            wsa_wakeup(ctx);
            return;
        }
    }
}

static void
wsa_process_user_events(wsa_ctx_t *ctx)
{
    for (wsa_user_event_t *we = ctx->user_events; we; we = we->next) {
        if (WaitForSingleObject(we->win_event, 0) == WAIT_OBJECT_0) {
            n00b_conduit_user_event_fire(we->event);
        }
    }
}

// ============================================================================
// Wait
// ============================================================================

static int
wsa_wait(void *vctx, n00b_conduit_io_event_t *events, int max_events,
         int timeout_ms)
{
    wsa_ctx_t *ctx = vctx;
    if (!ctx || !events || max_events <= 0) return -1;

    // Process timers and adjust timeout
    int adjusted = wsa_process_timers(ctx, timeout_ms);

    if (ctx->nfds == 0 && !ctx->timers) return 0;

    if (ctx->nfds == 0) {
        if (adjusted > 0) Sleep((DWORD)adjusted);
        wsa_process_timers(ctx, 0);
        return 0;
    }

    int n;
    n00b_run_blocking(n = WSAPoll(ctx->pollfds, ctx->nfds, adjusted));
    if (n == SOCKET_ERROR) return -1;

    // Post-poll processing
    wsa_process_timers(ctx, 0);
    wsa_process_procs(ctx);
    wsa_process_vnodes(ctx);
    wsa_process_user_events(ctx);

    if (n == 0) return 0;

    // Drain wakeup FD if it fired
    int wakeup_idx = wsa_find_fd(ctx, (int)ctx->wakeup_rd);
    if (wakeup_idx >= 0 && (ctx->pollfds[wakeup_idx].revents & POLLIN)) {
        wsa_drain_wakeup(ctx);
        n--;
    }

    // Convert to conduit events
    int num_events = 0;
    int total_seen = 0;
    for (int i = 0; i < ctx->nfds && num_events < max_events && total_seen < n;
         i++) {
        if (ctx->pollfds[i].revents == 0) continue;
        if (ctx->pollfds[i].fd == ctx->wakeup_rd) continue;
        total_seen++;

        n00b_conduit_io_event_t *ev = &events[num_events];
        ev->fd        = (int)ctx->pollfds[i].fd;
        ev->ops       = wsa_events_to_ops(ctx->pollfds[i].revents);
        ev->topic     = nullptr;
        ev->target    = ctx->targets[i];
        num_events++;
    }

    return num_events;
}

// ============================================================================
// Vtable
// ============================================================================

static const n00b_conduit_io_ops_t wsa_ops = {
    .init               = wsa_init,
    .cleanup            = wsa_cleanup,
    .add                = wsa_add,
    .modify             = wsa_modify,
    .remove             = wsa_remove,
    .wait               = wsa_wait,
    .name               = wsa_name,
    .timer_add          = wsa_timer_add,
    .timer_remove       = wsa_timer_remove,
    .signal_add         = nullptr,
    .signal_remove      = nullptr,
    .proc_add           = wsa_proc_add,
    .proc_remove        = wsa_proc_remove,
    .vnode_add          = wsa_vnode_add,
    .vnode_remove       = wsa_vnode_remove,
    .user_event_add     = wsa_user_event_add,
    .user_event_remove  = wsa_user_event_remove,
    .user_event_trigger = wsa_user_event_trigger,
};

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_wsa_ops(void)
{
    return n00b_result_ok(const n00b_conduit_io_ops_t *, &wsa_ops);
}

#else // !_WIN32

#include "conduit/io.h"

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_wsa_ops(void)
{
    return n00b_result_err(const n00b_conduit_io_ops_t *, N00B_CONDUIT_ERR_NOT_SUPPORTED);
}

#endif // _WIN32
