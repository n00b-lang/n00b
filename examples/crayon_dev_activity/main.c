/**
 * @file examples/crayon_dev_activity/main.m
 * @brief Online binary classifier: "is this actor producing source code?"
 *
 * Subscribes to the Crayon warehouse, projects each event into the four
 * PII-style subspaces, auto-labels positive when actor.classification.
 * categories has AI or EDITOR set (and negative when any *other*
 * "interesting" bit is set), and trains an n00b ML logistic-regression
 * model online.
 *
 * Modes:
 *   - **No flags**: train from scratch.  Prints periodic stats + top-K.
 *   - **--save PATH**: write the trainer blob on Ctrl-C so you can ship it.
 *   - **--load PATH**: load that blob, wrap in a layered model with a
 *     small-learning-rate delta + a drift monitor, refine from new data.
 */

#ifdef __APPLE__

#include <xpc/xpc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "crayon_protocol.h"
#include "crayon_subscriber.h"
#include "crayon_xpc_to_json.h"
#include "crayon_features.h"

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/futex.h"
#include "core/gc.h"
#include "ml/ml.h"

#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/box.h"
#include "display/widgets/label.h"
#include "display/event_loop.h"

// ANSI is a plain-terminal renderer — fast cell-level escape codes.
// notcurses' pixel mode runs every codepoint through FreeType's
// autofitter on every redraw, which is too slow for our update rate.

// ----------------------------------------------------------------------------
// Process-wide state — guarded by g_mu.
// ----------------------------------------------------------------------------

typedef struct {
    n00b_ml_trainer_t *trainer;       // live training; non-null whether
                                      // bootstrapping or refining a delta
                                      // (in refine mode this is the shadow)
    n00b_ml_correctable_t *layered;       // null in bootstrap mode
    n00b_ml_monitor_t *monitor;       // null in bootstrap mode
    crayon_features_t  ids;
    n00b_ml_input_t  *sample;        // reused; reset between events

    _Atomic uint64_t   pos_seen;
    _Atomic uint64_t   neg_seen;
    _Atomic uint64_t   skipped;       // events without classification
    _Atomic uint64_t   total;
} app_state_t;

static app_state_t  g_app;
static _Atomic bool g_done;
static const char      *g_save_path;
static const char      *g_load_path;
static uint64_t         g_print_every  = 200;
static double           g_print_every_sec = 5.0;
static double           g_last_print_sec  = 0.0;
static bool             g_tui_mode;
static n00b_canvas_t   *g_canvas;
static n00b_plane_t    *g_status_label;
static n00b_plane_t    *g_features_label;

// ----------------------------------------------------------------------------
// Cross-thread event queue.
//
// libxpc delivers events on dispatch pool threads that the n00b runtime
// hasn't registered, so the producer side only uses C atomics + an
// n00b_futex_t for wakeup — no n00b allocation, no n00b locking.  The
// consumer is an n00b-spawned worker thread (GC-registered) that
// processes events with full n00b primitives.
//
// Single-producer / single-consumer ring buffer; ordering between the
// two ends is via the head / tail atomic pair.  Wakeup uses a separate
// counter that the consumer waits on with `n00b_futex_wait`.
// ----------------------------------------------------------------------------

#define EVENT_QUEUE_CAP 4096

static struct {
    xpc_object_t      slots[EVENT_QUEUE_CAP];
    _Atomic uint64_t  head;     // producer-only writes
    _Atomic uint64_t  tail;     // consumer-only writes
    _Atomic uint64_t  dropped;
    n00b_futex_t      wake;     // bumped on each push, waited on when empty
    _Atomic bool      shutdown; // consumer exits once the queue drains
} g_q;

static void
queue_push(xpc_object_t evt)
{
    uint64_t h = atomic_load_explicit(&g_q.head, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&g_q.tail, memory_order_acquire);
    if (h - t >= EVENT_QUEUE_CAP) {
        xpc_release(evt);
        atomic_fetch_add(&g_q.dropped, 1);
        return;
    }
    g_q.slots[h % EVENT_QUEUE_CAP] = evt;
    atomic_store_explicit(&g_q.head, h + 1, memory_order_release);
    atomic_fetch_add_explicit(&g_q.wake, 1, memory_order_release);
    n00b_futex_wake_one(&g_q.wake);
}

// Block until the queue has an event or shutdown was signalled.
// Returns NULL on shutdown-with-empty-queue.
static xpc_object_t
queue_pop_blocking(void)
{
    for (;;) {
        uint64_t h = atomic_load_explicit(&g_q.head, memory_order_acquire);
        uint64_t t = atomic_load_explicit(&g_q.tail, memory_order_relaxed);
        if (h != t) {
            xpc_object_t evt = g_q.slots[t % EVENT_QUEUE_CAP];
            atomic_store_explicit(&g_q.tail, t + 1, memory_order_release);
            return evt;
        }
        if (atomic_load(&g_q.shutdown)) return NULL;
        // Empty — sample wake counter, sleep until it changes (or 100 ms).
        uint32_t w = atomic_load_explicit(&g_q.wake, memory_order_acquire);
        n00b_futex_wait(&g_q.wake, w, 100ULL * 1000 * 1000);
    }
}

static void
queue_shutdown(void)
{
    atomic_store(&g_q.shutdown, true);
    atomic_fetch_add(&g_q.wake, 1);
    n00b_futex_wake_all(&g_q.wake);
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static double
mono_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_done, true);
    // In TUI mode `n00b_canvas_run` runs the main thread's event loop;
    // it won't notice g_done unless we explicitly stop it.  This call
    // is async-signal-safe (single atomic store inside n00b's
    // implementation) so calling it from a signal handler is fine.
    if (g_tui_mode) n00b_canvas_run_stop();
}

static n00b_buffer_t *
read_file_to_buffer(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *bytes = (char *)malloc((size_t)sz);
    if (fread(bytes, 1, (size_t)sz, f) != (size_t)sz) {
        free(bytes); fclose(f); return NULL;
    }
    fclose(f);
    n00b_buffer_t *buf = n00b_buffer_from_bytes(bytes, sz);
    free(bytes);
    return buf;
}

static bool
write_buffer_to_file(const char *path, n00b_buffer_t *buf)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(buf->data, 1, buf->byte_len, f) == buf->byte_len;
    fclose(f);
    return ok;
}

// ----------------------------------------------------------------------------
// Periodic status print
// ----------------------------------------------------------------------------

// Format the status pane (counters, monitor metrics) into a buffer.
static int
format_status(char *buf, size_t cap)
{
    uint64_t pos   = atomic_load(&g_app.pos_seen);
    uint64_t neg   = atomic_load(&g_app.neg_seen);
    uint64_t skip  = atomic_load(&g_app.skipped);
    uint64_t total = atomic_load(&g_app.total);
    uint64_t drop  = atomic_load(&g_q.dropped);

    int n = snprintf(buf, cap,
                     "events: %llu   pos: %llu   neg: %llu\n"
                     "skipped: %llu   dropped: %llu",
                     (unsigned long long)total,
                     (unsigned long long)pos,
                     (unsigned long long)neg,
                     (unsigned long long)skip,
                     (unsigned long long)drop);
    if (g_app.monitor && n < (int)cap) {
        n += snprintf(buf + n, cap - (size_t)n,
                      "\nresidual mean: %.4g   ewma: %.4g\n"
                      "weight drift: %.4g",
                      n00b_ml_monitor_residual_mean(g_app.monitor),
                      n00b_ml_monitor_residual_ewma(g_app.monitor),
                      n00b_ml_monitor_weight_drift(g_app.monitor));
    }
    return n;
}

// Format the top-K feature pane into a buffer.
static int
format_features(char *buf, size_t cap)
{
    n00b_list_t(n00b_ml_learned_rule_t) ranked
        = g_app.layered
              ? n00b_ml_correctable_strongest_rules(g_app.layered, 8)
              : n00b_ml_strongest_rules(g_app.trainer->model, g_app.trainer->rules, 8);
    size_t k   = n00b_list_len(ranked);
    int    off = 0;
    if (k == 0) {
        return snprintf(buf, cap, "(no learned features yet)");
    }
    for (size_t i = 0; i < k && off < (int)cap; i++) {
        n00b_ml_learned_rule_t s = n00b_list_get(ranked, i);
        const char *sub = (s.group_name && s.group_name->u8_bytes)
                              ? (const char *)s.group_name->data : "?";
        const char *ex  = (s.most_common_match  && s.most_common_match->u8_bytes)
                              ? (const char *)s.most_common_match->data : "(no debug)";
        off += snprintf(buf + off, cap - (size_t)off,
                        "%s%+7.3f  %-4s  rule=%-5u  %s (n=%u)",
                        i == 0 ? "" : "\n",
                        (double)s.weight, sub, s.rule_id,
                        ex, s.match_count);
    }
    return off;
}

static void
print_status(void)
{
    char status_buf[1024];
    char features_buf[2048];
    format_status(status_buf, sizeof(status_buf));
    format_features(features_buf, sizeof(features_buf));

    if (g_tui_mode) {
        if (g_status_label) {
            n00b_label_set_text(g_status_label,
                                n00b_string_from_cstr(status_buf));
        }
        if (g_features_label) {
            n00b_label_set_text(g_features_label,
                                n00b_string_from_cstr(features_buf));
        }
    } else {
        printf("\n=== %s ===\n%s\n", status_buf, features_buf);
        fflush(stdout);
    }
}

// ----------------------------------------------------------------------------
// libxpc subscriber callback — runs on a foreign dispatch pool thread
// (no n00b registration), so it must NOT call into n00b.  Just retain
// the event and hand it to the worker via the cross-thread queue.
// ----------------------------------------------------------------------------

static void
on_event(xpc_object_t evt, void *user_data)
{
    (void)user_data;
    xpc_type_t tt = xpc_get_type(evt);
    if (tt == XPC_TYPE_ERROR) {
        const char *desc = xpc_dictionary_get_string(evt, XPC_ERROR_KEY_DESCRIPTION);
        fprintf(stderr, "crayon_dev_activity: connection error: %s\n",
                desc ? desc : "?");
        return;
    }
    if (tt != XPC_TYPE_DICTIONARY) return;
    if (xpc_dictionary_get_uint64(evt, CRAYON_SVC_KEY_EVENT_TYPE)
        != CRAYON_WH_EVENT_NORMALIZED) {
        return;
    }
    // Deep-copy the event onto our own xpc allocation so the worker
    // walks a tree that doesn't share lazy-deserialization state with
    // libxpc's connection delivery thread.  xpc_retain alone leaves
    // wire-data + lazy-decode shared, which crashed under cross-thread
    // xpc_dictionary_apply.
    xpc_object_t copy = xpc_copy(evt);
    if (!copy) return;
    queue_push(copy);
}

// Worker thread body — runs on an n00b-registered thread (via
// n00b_thread_spawn) so n00b allocators / locks are safe to call.
static void *
worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        xpc_object_t evt = queue_pop_blocking();
        if (!evt) break;
        n00b_json_node_t *event_json = crayon_xpc_to_json(evt);
        xpc_release(evt);

        uint64_t cls = 0;
        if (!crayon_features_classification(event_json, &cls)) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }
        bool label = crayon_features_dev_activity_label(cls);
        n00b_ml_input_reset(g_app.sample);
        if (!crayon_features_project(g_app.sample, &g_app.ids, event_json)) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }

        if (g_app.layered) {
            n00b_ml_monitor_observe(g_app.monitor, g_app.sample,
                                    label ? 1.0f : 0.0f);
            float p = n00b_ml_correctable_predict(g_app.layered, g_app.sample);
            bool  layered_says_pos = (p > 0.5f);
            if (layered_says_pos != label) {
                n00b_ml_correctable_correct(g_app.layered, g_app.sample,
                                         label ? N00B_ML_FB_FALSE_NEGATIVE
                                               : N00B_ML_FB_FALSE_POSITIVE);
            }
        } else {
            n00b_ml_trainer_observe(g_app.trainer, g_app.sample,
                                    label ? 1.0f : 0.0f);
        }

        if (label) atomic_fetch_add(&g_app.pos_seen, 1);
        else       atomic_fetch_add(&g_app.neg_seen, 1);
        uint64_t total = atomic_fetch_add(&g_app.total, 1) + 1;

        bool   ev_due = (total % g_print_every == 0);
        double now    = mono_seconds();
        bool   t_due  = (now - g_last_print_sec) >= g_print_every_sec;
        if (ev_due || t_due) {
            g_last_print_sec = now;
            print_status();
        }
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// TUI
// ----------------------------------------------------------------------------

static bool
tui_init(void)
{
    g_canvas = n00b_alloc(n00b_canvas_t);
    n00b_runtime_t *rt = n00b_get_runtime();
    n00b_canvas_init(g_canvas,
                     .vtable = &n00b_renderer_ansi,
                     .output = (n00b_conduit_topic_t(n00b_buffer_t *) *)
                                   rt->stdout_topic);

    int32_t cph     = (int32_t)g_canvas->cell_px_h;
    int32_t frame_w = (int32_t)g_canvas->frame_cols;
    int32_t frame_h = (int32_t)g_canvas->frame_rows;

    n00b_plane_t *root = n00b_box_new(.canvas    = g_canvas,
                                       .direction = N00B_FLEX_COLUMN,
                                       .gap       = cph);
    root->width  = frame_w;
    root->height = frame_h;
    n00b_canvas_add_plane(g_canvas, root);

    n00b_plane_t *title = n00b_label_new(
        n00b_string_from_cstr("crayon dev-activity classifier  "
                              "(Ctrl-C to quit)"),
        .canvas    = g_canvas,
        .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, title, 0, 0);

    // Labels default to a single visible line ("height=0 means auto, 1
    // unless wrap").  Our status/features text is multi-line so we
    // give each label enough vertical space — measured in pixels via
    // the canvas's cell height — to show every line.
    g_status_label = n00b_label_new(
        n00b_string_from_cstr("waiting for events…"),
        .canvas = g_canvas,
        .height = 5 * cph);          // up to 5 status lines
    n00b_plane_add_child(root, g_status_label, 0, 0);

    g_features_label = n00b_label_new(
        n00b_string_from_cstr("(no learned features yet)"),
        .canvas = g_canvas,
        .height = 9 * cph);          // up to 9 ranked-feature lines
    n00b_plane_add_child(root, g_features_label, 0, 0);

    n00b_canvas_alt_screen_enter(g_canvas);
    return true;
}

static void
tui_run(void)
{
    n00b_canvas_run(g_canvas, .tick_ms = 100);
}

static void
tui_destroy(void)
{
    if (g_canvas) {
        n00b_canvas_alt_screen_leave(g_canvas);
        n00b_canvas_destroy(g_canvas);
        g_canvas         = NULL;
        g_status_label   = NULL;
        g_features_label = NULL;
    }
}

// ----------------------------------------------------------------------------
// CLI
// ----------------------------------------------------------------------------

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--save PATH] [--load PATH] [--every N] [--tui]\n"
            "  --save PATH   Write the trainer blob to PATH on Ctrl-C.\n"
            "  --load PATH   Load a base model from PATH and refine via a\n"
            "                delta + drift monitor (otherwise: train fresh).\n"
            "  --every N     Refresh status every N events (default 200).\n"
            "  --tui         Run the notcurses TUI (default: stdout).\n",
            argv0);
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--save") && i + 1 < argc) {
            g_save_path = argv[++i];
        } else if (!strcmp(argv[i], "--load") && i + 1 < argc) {
            g_load_path = argv[++i];
        } else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
            g_print_every = (uint64_t)strtoull(argv[++i], NULL, 10);
            if (g_print_every == 0) g_print_every = 1;
        } else if (!strcmp(argv[i], "--tui")) {
            g_tui_mode = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // The trainer / layered / monitor / sample pointers live in our
    // global struct.  Without registering it as a GC root the next
    // collection reaps them and the worker faults on the next event.
    n00b_gc_register_root(g_app);

    if (g_load_path) {
        // Refine mode: load the shipped scorer, wrap it as a layered
        // model with a small delta, and run a drift monitor in parallel.
        n00b_buffer_t *blob = read_file_to_buffer(g_load_path);
        if (!blob) {
            fprintf(stderr, "load: cannot open %s\n", g_load_path);
            return 1;
        }
        n00b_result_t(n00b_ml_scorer_t *) sr = n00b_ml_scorer_load(blob);
        if (n00b_result_is_err(sr)) {
            fprintf(stderr, "load: %s\n",
                    n00b_ml_err_str(n00b_result_get_err(sr)));
            return 1;
        }
        n00b_ml_scorer_t *base = n00b_result_get(sr);

        g_app.layered = n00b_ml_correctable_new(base, .feedback_learning_rate = 0.005f);
        g_app.monitor = n00b_ml_monitor_new(base, .shadow_learning_rate = 0.05f);

        // The monitor owns its shadow trainer, but the layered model
        // shares the base's config.  Re-derive our subspace IDs by name
        // off the loaded config.
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("LEX"),
                                &g_app.ids.lex);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("FLOW"),
                                &g_app.ids.flow);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("GEOM"),
                                &g_app.ids.geom);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("ENV"),
                                &g_app.ids.env);

        n00b_ml_rules_track_matches(base->rules);
        g_app.sample = n00b_ml_input_new(base->rules);

        fprintf(stderr,
                "crayon_dev_activity: loaded base from %s — refining with "
                "delta + drift monitor\n", g_load_path);
    } else {
        // Bootstrap mode: fresh trainer.
        g_app.trainer = n00b_ml_trainer_new(.learning_rate = 0.1f,
                                            .weight_decay            = 1e-4f);
        g_app.ids     = crayon_features_register_rule_groups(g_app.trainer);
        n00b_ml_rules_track_matches(g_app.trainer->rules);
        g_app.sample  = n00b_ml_input_new(g_app.trainer->rules);

        fprintf(stderr,
                "crayon_dev_activity: bootstrapping a fresh model — "
                "save with --save PATH\n");
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (g_tui_mode && !tui_init()) {
        fprintf(stderr, "crayon_dev_activity: TUI unavailable on this build\n");
        return 1;
    }

    // Spawn the worker BEFORE opening the subscriber so the queue has
    // a consumer ready when events start flowing.
    n00b_result_t(n00b_thread_t *) tr = n00b_thread_spawn(worker_main, NULL);
    if (n00b_result_is_err(tr)) {
        fprintf(stderr, "crayon_dev_activity: failed to spawn worker\n");
        tui_destroy();
        return 1;
    }
    n00b_thread_t *worker = n00b_result_get(tr);

    crayon_subscriber_t *sub = crayon_subscriber_open(on_event, NULL);
    if (!sub) {
        fprintf(stderr,
                "crayon_dev_activity: cannot open warehouse subscription "
                "(is the Crayon daemon running?)\n");
        queue_shutdown();
        n00b_thread_join(worker);
        tui_destroy();
        return 1;
    }

    g_last_print_sec = mono_seconds();
    if (g_tui_mode) {
        // The event loop blocks on the main thread until Ctrl-C.  The
        // worker keeps refreshing the labels in the background.
        tui_run();
        atomic_store(&g_done, true);
    } else {
        while (!atomic_load(&g_done)) {
            usleep(100 * 1000);
        }
    }

    crayon_subscriber_close(sub);
    queue_shutdown();
    n00b_thread_join(worker);
    tui_destroy();
    if (!g_tui_mode) print_status();

    if (g_save_path) {
        n00b_buffer_t *blob = g_app.layered
            ? n00b_ml_correctable_save(g_app.layered)
            : n00b_ml_trainer_save(g_app.trainer);
        if (!write_buffer_to_file(g_save_path, blob)) {
            fprintf(stderr, "save: failed to write %s\n", g_save_path);
            return 1;
        }
        fprintf(stderr, "crayon_dev_activity: wrote %zu bytes to %s\n",
                (size_t)blob->byte_len, g_save_path);
    }

    return 0;
}

#else

#include <stdio.h>
int main(void) {
    fprintf(stderr, "crayon_dev_activity is macOS-only.\n");
    return 1;
}

#endif // __APPLE__
