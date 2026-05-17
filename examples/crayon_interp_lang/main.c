/**
 * @file examples/crayon_interp_lang/main.c
 * @brief Online binary classifier: "is this actor a `<lang>` interpreter?"
 *
 * Same machinery as the dev-activity demo (warehouse subscriber, n00b
 * worker thread + futex-based SPSC ring, n00b ML facade with PII-style
 * subspaces, save / load / drift / delta).  Different filter and label:
 *
 *   - Filter: only events whose actor classifies as INTERPRETER.
 *   - Label: positive iff `actor.classification.metadata.interpreter.language`
 *            == --lang (default `python`).
 *
 * The model learns to recognize the language from features Crayon's
 * rules don't directly look at: argv-token morphology, ancestry,
 * signing identity, path bucket.  Useful for catching invocations the
 * exact-name rule misses (custom python launchers, stripped binaries,
 * etc.).
 */

#ifdef __APPLE__

#include <xpc/xpc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "adt/dict_untyped.h"

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

static struct {
    n00b_ml_trainer_t *trainer;
    n00b_ml_correctable_t *layered;
    n00b_ml_monitor_t *monitor;
    crayon_features_t  ids;
    n00b_ml_input_t  *sample;

    _Atomic uint64_t   pos_seen;
    _Atomic uint64_t   neg_seen;
    _Atomic uint64_t   skipped;
    _Atomic uint64_t   total;
} g_app;

static const char  *g_target_lang   = "python";
static const char  *g_save_path;
static const char  *g_load_path;
static uint64_t     g_print_every   = 200;
static double       g_print_every_s = 5.0;
static double       g_last_print    = 0.0;
static _Atomic bool g_done;

// ----------------------------------------------------------------------------
// SPSC ring queue (same shape as the dev-activity demo)
// ----------------------------------------------------------------------------

#define EVENT_QUEUE_CAP 4096

static struct {
    xpc_object_t      slots[EVENT_QUEUE_CAP];
    _Atomic uint64_t  head;
    _Atomic uint64_t  tail;
    _Atomic uint64_t  dropped;
    n00b_futex_t      wake;
    _Atomic bool      shutdown;
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
    char *bytes = malloc((size_t)sz);
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

// Walk the parsed event for `actor.classification.metadata.interpreter.language`.
// Returns NULL if any link in the chain is missing.
static const char *
event_interp_language(n00b_json_node_t *event)
{
    if (!n00b_json_is_object(event)) return NULL;
    bool   found;
    void  *v;

    v = _n00b_dict_untyped_get(event->object, (void *)"actor", &found);
    n00b_json_node_t *actor = found ? v : NULL;
    if (!n00b_json_is_object(actor)) return NULL;

    v = _n00b_dict_untyped_get(actor->object, (void *)"classification", &found);
    n00b_json_node_t *cls = found ? v : NULL;
    if (!n00b_json_is_object(cls)) return NULL;

    v = _n00b_dict_untyped_get(cls->object, (void *)"metadata", &found);
    n00b_json_node_t *md = found ? v : NULL;
    if (!n00b_json_is_object(md)) return NULL;

    v = _n00b_dict_untyped_get(md->object, (void *)"interpreter", &found);
    n00b_json_node_t *interp = found ? v : NULL;
    if (!n00b_json_is_object(interp)) return NULL;

    v = _n00b_dict_untyped_get(interp->object, (void *)"language", &found);
    n00b_json_node_t *lang = found ? v : NULL;
    if (!lang || !n00b_json_is_string(lang)) return NULL;
    return lang->string;
}

// ----------------------------------------------------------------------------
// Status
// ----------------------------------------------------------------------------

static void
print_top_k(void)
{
    n00b_list_t(n00b_ml_learned_rule_t) ranked
        = g_app.layered
              ? n00b_ml_correctable_strongest_rules(g_app.layered, 8)
              : n00b_ml_strongest_rules(g_app.trainer->model, g_app.trainer->rules, 8);
    size_t n = n00b_list_len(ranked);
    if (n == 0) { printf("  (no learned features yet)\n"); return; }
    for (size_t i = 0; i < n; i++) {
        n00b_ml_learned_rule_t s = n00b_list_get(ranked, i);
        const char *sub = (s.group_name && s.group_name->u8_bytes)
                              ? (const char *)s.group_name->data : "?";
        const char *ex  = (s.most_common_match  && s.most_common_match->u8_bytes)
                              ? (const char *)s.most_common_match->data : "(no debug)";
        printf("  %+8.4f  %-4s  rule=%-5u  %s (n=%u)\n",
               (double)s.weight, sub, s.rule_id, ex, s.match_count);
    }
}

static void
print_status(void)
{
    uint64_t pos   = atomic_load(&g_app.pos_seen);
    uint64_t neg   = atomic_load(&g_app.neg_seen);
    uint64_t skip  = atomic_load(&g_app.skipped);
    uint64_t total = atomic_load(&g_app.total);
    uint64_t drop  = atomic_load(&g_q.dropped);

    printf("\n=== %llu interpreter events  pos(%s)=%llu neg=%llu  skip=%llu drop=%llu ===\n",
           (unsigned long long)total, g_target_lang,
           (unsigned long long)pos, (unsigned long long)neg,
           (unsigned long long)skip, (unsigned long long)drop);
    if (g_app.monitor) {
        printf("  residual mean = %.4g   ewma = %.4g   drift = %.4g\n",
               n00b_ml_monitor_residual_mean(g_app.monitor),
               n00b_ml_monitor_residual_ewma(g_app.monitor),
               n00b_ml_monitor_weight_drift(g_app.monitor));
    }
    print_top_k();
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// Subscriber callback (foreign thread): just enqueue
// ----------------------------------------------------------------------------

static void
on_event(xpc_object_t evt, void *user_data)
{
    (void)user_data;
    if (xpc_get_type(evt) != XPC_TYPE_DICTIONARY) return;
    if (xpc_dictionary_get_uint64(evt, CRAYON_SVC_KEY_EVENT_TYPE)
        != CRAYON_WH_EVENT_NORMALIZED) return;
    xpc_object_t copy = xpc_copy(evt);
    if (!copy) return;
    queue_push(copy);
}

// Worker (n00b-registered).
static void *
worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        xpc_object_t evt = queue_pop_blocking();
        if (!evt) break;
        n00b_json_node_t *event_json = crayon_xpc_to_json(evt);
        xpc_release(evt);

        // Filter: actor must classify as INTERPRETER.
        uint64_t cls = 0;
        if (!crayon_features_classification(event_json, &cls)
            || !(cls & CRAYON_CLASSIFY_BIT(CRAYON_CLASSIFY_CATEGORY_INTERPRETER))) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }

        const char *lang = event_interp_language(event_json);
        // We need the rule to have stamped a language so we have ground
        // truth.  Without it we can't auto-label, so skip.
        if (!lang) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }
        bool label = (strcmp(lang, g_target_lang) == 0);

        n00b_ml_input_reset(g_app.sample);
        if (!crayon_features_project(g_app.sample, &g_app.ids, event_json)) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }

        if (g_app.layered) {
            n00b_ml_monitor_observe(g_app.monitor, g_app.sample,
                                    label ? 1.0f : 0.0f);
            float p = n00b_ml_correctable_predict(g_app.layered, g_app.sample);
            if ((p > 0.5f) != label) {
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
        bool   t_due  = (now - g_last_print) >= g_print_every_s;
        if (ev_due || t_due) {
            g_last_print = now;
            print_status();
        }
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// CLI
// ----------------------------------------------------------------------------

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--lang LANG] [--save PATH] [--load PATH] [--every N]\n"
            "  --lang LANG   Target language for the positive class (default: python).\n"
            "                Common values: python, javascript, ruby, lua, php.\n"
            "  --save PATH   Write the trainer blob to PATH on Ctrl-C.\n"
            "  --load PATH   Refine an existing model from PATH.\n"
            "  --every N     Print status every N events (default 200).\n",
            argv0);
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lang")  && i + 1 < argc) g_target_lang = argv[++i];
        else if (!strcmp(argv[i], "--save")  && i + 1 < argc) g_save_path = argv[++i];
        else if (!strcmp(argv[i], "--load")  && i + 1 < argc) g_load_path = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
            g_print_every = (uint64_t)strtoull(argv[++i], NULL, 10);
            if (g_print_every == 0) g_print_every = 1;
        } else { usage(argv[0]); return 1; }
    }

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // The trainer / layered / monitor / sample pointers live in our
    // global struct.  Without registering it as a GC root the next
    // collection reaps them and the worker faults on the next event.
    n00b_gc_register_root(g_app);

    if (g_load_path) {
        n00b_buffer_t *blob = read_file_to_buffer(g_load_path);
        if (!blob) { fprintf(stderr, "load: cannot open %s\n", g_load_path); return 1; }
        n00b_result_t(n00b_ml_scorer_t *) sr = n00b_ml_scorer_load(blob);
        if (n00b_result_is_err(sr)) {
            fprintf(stderr, "load: %s\n", n00b_ml_err_str(n00b_result_get_err(sr)));
            return 1;
        }
        n00b_ml_scorer_t *base = n00b_result_get(sr);
        g_app.layered = n00b_ml_correctable_new(base, .feedback_learning_rate = 0.005f);
        g_app.monitor = n00b_ml_monitor_new(base, .shadow_learning_rate = 0.05f);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("LEX"),  &g_app.ids.lex);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("FLOW"), &g_app.ids.flow);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("GEOM"), &g_app.ids.geom);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("ENV"),  &g_app.ids.env);
        n00b_ml_rules_track_matches(base->rules);
        g_app.sample = n00b_ml_input_new(base->rules);
        fprintf(stderr,
                "crayon_interp_lang: refining %s-classifier from %s\n",
                g_target_lang, g_load_path);
    } else {
        g_app.trainer = n00b_ml_trainer_new(.learning_rate = 0.1f, .weight_decay = 1e-4f);
        g_app.ids     = crayon_features_register_rule_groups(g_app.trainer);
        n00b_ml_rules_track_matches(g_app.trainer->rules);
        g_app.sample  = n00b_ml_input_new(g_app.trainer->rules);
        fprintf(stderr,
                "crayon_interp_lang: bootstrapping %s-classifier — save with --save PATH\n",
                g_target_lang);
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    n00b_result_t(n00b_thread_t *) tr = n00b_thread_spawn(worker_main, NULL);
    if (n00b_result_is_err(tr)) {
        fprintf(stderr, "crayon_interp_lang: failed to spawn worker\n");
        return 1;
    }
    n00b_thread_t *worker = n00b_result_get(tr);

    crayon_subscriber_t *sub = crayon_subscriber_open(on_event, NULL);
    if (!sub) {
        fprintf(stderr, "crayon_interp_lang: cannot open warehouse subscription\n");
        queue_shutdown();
        n00b_thread_join(worker);
        return 1;
    }

    g_last_print = mono_seconds();
    while (!atomic_load(&g_done)) {
        usleep(100 * 1000);
    }

    crayon_subscriber_close(sub);
    queue_shutdown();
    n00b_thread_join(worker);
    print_status();

    if (g_save_path) {
        n00b_buffer_t *blob = g_app.layered
            ? n00b_ml_correctable_save(g_app.layered)
            : n00b_ml_trainer_save(g_app.trainer);
        if (!write_buffer_to_file(g_save_path, blob)) {
            fprintf(stderr, "save: failed to write %s\n", g_save_path);
            return 1;
        }
        fprintf(stderr, "crayon_interp_lang: wrote %zu bytes to %s\n",
                (size_t)blob->byte_len, g_save_path);
    }
    return 0;
}

#else

#include <stdio.h>
int main(void) {
    fprintf(stderr, "crayon_interp_lang is macOS-only.\n");
    return 1;
}

#endif // __APPLE__
