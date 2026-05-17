/**
 * @file examples/crayon_api_crud/main.c
 * @brief Online binary classifier: "is this API call a write?"
 *
 * Subscribes to the warehouse, filters for `attribution.api_call_*`
 * events (Crayon's per-API-call rollups), auto-labels positive iff the
 * heuristic's `crud` ∈ {create, update, delete}, and trains an n00b ML
 * binary classifier from the structured fields the heuristic itself
 * sees: HTTP method, provider, service, operation, path-template
 * tokens, channel, outcome, count, and duration.
 *
 * Same SPSC ring + n00b worker thread architecture as the dev-activity
 * and interp-lang demos.  Save / load / drift / delta all reused.
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

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/futex.h"
#include "core/gc.h"
#include "ml/ml.h"
#include "adt/dict_untyped.h"

// ----------------------------------------------------------------------------
// Subspace IDs and feature engineering specific to attribution events.
// ----------------------------------------------------------------------------

typedef struct {
    n00b_ml_rule_group_id_t lex;   // method / provider / service / operation
                                 // / path-template segments
    n00b_ml_rule_group_id_t geom;  // request-count and duration buckets
    n00b_ml_rule_group_id_t env;   // channel + outcome + signing identity
} api_features_t;

static api_features_t
api_register_subspaces(n00b_ml_trainer_t *t)
{
    api_features_t s = {
        .lex  = n00b_ml_trainer_define_rule_group_cstr(t, "LEX",  1u << 14),
        .geom = n00b_ml_trainer_define_rule_group_cstr(t, "GEOM", 256),
        .env  = n00b_ml_trainer_define_rule_group_cstr(t, "ENV",  1u << 10),
    };
    return s;
}

static n00b_json_node_t *
json_obj_get(n00b_json_node_t *n, const char *key)
{
    if (!n00b_json_is_object(n)) return NULL;
    bool  found;
    void *v = _n00b_dict_untyped_get(n->object, (void *)key, &found);
    return found ? (n00b_json_node_t *)v : NULL;
}

static const char *
json_str(n00b_json_node_t *n)
{
    return (n && n00b_json_is_string(n)) ? n->string : NULL;
}

static int64_t
json_int(n00b_json_node_t *n)
{
    return (n && n00b_json_is_int(n)) ? n->integer : 0;
}

static const char *
count_bucket(int64_t c)
{
    if (c <= 1)   return "count:1";
    if (c <= 5)   return "count:2-5";
    if (c <= 20)  return "count:6-20";
    if (c <= 100) return "count:21-100";
    return                "count:100+";
}

static const char *
duration_bucket(int64_t ns)
{
    if (ns <= 0)              return "dur:none";
    if (ns < 1000000)         return "dur:<1ms";
    if (ns < 10000000)        return "dur:1-10ms";
    if (ns < 100000000)       return "dur:10-100ms";
    if (ns < 1000000000)      return "dur:0.1-1s";
    if (ns < 10000000000LL)   return "dur:1-10s";
    return                          "dur:10s+";
}

// Project an attribution event into the trainer's feature vector.
// Returns false when the event has no `method` (couldn't extract any
// meaningful signal — typically the rollup variants we don't care about).
static bool
project_api_event(n00b_ml_input_t       *sample,
                  const api_features_t   *ids,
                  n00b_json_node_t       *event)
{
    const char *method   = json_str(json_obj_get(event, "method"));
    const char *provider = json_str(json_obj_get(event, "provider"));
    const char *service  = json_str(json_obj_get(event, "service"));
    const char *op       = json_str(json_obj_get(event, "operation"));
    const char *path     = json_str(json_obj_get(event, "path_template"));
    const char *channel  = json_str(json_obj_get(event, "channel"));
    const char *outcome  = json_str(json_obj_get(event, "outcome"));
    int64_t     count    = json_int(json_obj_get(event, "count"));
    int64_t     dur      = json_int(json_obj_get(event, "duration_ns"));

    // We require *some* identifying signal — method or provider — so
    // we don't train on totally-empty events.  Many heuristic rollups
    // arrive with provider populated but method absent (Crayon collapses
    // bursts of same-template calls into one rollup).
    if (!method && !provider) return false;

    // LEX features — strong signals: HTTP method, provider, service,
    // operation, and each path-template segment.
    if (method)   n00b_ml_input_match_cstr(sample, ids->lex, method);
    if (provider) n00b_ml_input_match_cstr(sample, ids->lex, provider);
    if (service)  n00b_ml_input_match_cstr(sample, ids->lex, service);
    if (op)       n00b_ml_input_match_cstr(sample, ids->lex, op);

    if (path) {
        // Walk the path template, splitting on '/' and projecting each
        // non-empty segment.  Cap at 6 segments per event so a deep
        // template doesn't dominate any one sample.
        char buf[256];
        size_t plen = strnlen(path, sizeof(buf) - 1);
        memcpy(buf, path, plen);
        buf[plen] = '\0';
        int    segs = 0;
        char  *save = NULL;
        for (char *t = strtok_r(buf, "/", &save); t && segs < 6;
             t = strtok_r(NULL, "/", &save), segs++) {
            if (*t) n00b_ml_input_match_cstr(sample, ids->lex, t);
        }
    }

    // GEOM — coarse count + duration buckets.
    n00b_ml_input_match_cstr(sample, ids->geom, count_bucket(count));
    n00b_ml_input_match_cstr(sample, ids->geom, duration_bucket(dur));

    // ENV — channel, outcome, actor signing.
    if (channel) n00b_ml_input_match_cstr(sample, ids->env, channel);
    if (outcome) n00b_ml_input_match_cstr(sample, ids->env, outcome);

    n00b_json_node_t *actor = json_obj_get(event, "actor");
    if (actor) {
        const char *sid = json_str(json_obj_get(actor, "signing_id"));
        if (sid) n00b_ml_input_match_cstr(sample, ids->env, sid);
    }
    return true;
}

// True if the rule's verdict is a write (CREATE / UPDATE / DELETE).
static bool
is_write_crud(const char *crud)
{
    if (!crud) return false;
    return strcmp(crud, "create") == 0
        || strcmp(crud, "update") == 0
        || strcmp(crud, "delete") == 0;
}

// ----------------------------------------------------------------------------
// State + queue (mirrors interp_lang)
// ----------------------------------------------------------------------------

static struct {
    n00b_ml_trainer_t *trainer;
    n00b_ml_correctable_t *layered;
    n00b_ml_monitor_t *monitor;
    api_features_t     ids;
    n00b_ml_input_t  *sample;

    _Atomic uint64_t   pos_seen;
    _Atomic uint64_t   neg_seen;
    _Atomic uint64_t   skipped;
    _Atomic uint64_t   total;
} g_app;

static const char  *g_save_path;
static const char  *g_load_path;
static uint64_t     g_print_every   = 100;
static double       g_print_every_s = 5.0;
static double       g_last_print    = 0.0;
static _Atomic bool g_done;

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
// Persistence
// ----------------------------------------------------------------------------

static n00b_buffer_t *
read_file_to_buffer(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
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

// ----------------------------------------------------------------------------
// Status / signal / mono_seconds (same shapes as the other demos)
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

static void
print_top_k(void)
{
    n00b_list_t(n00b_ml_learned_rule_t) ranked
        = g_app.layered
              ? n00b_ml_correctable_strongest_rules(g_app.layered, 10)
              : n00b_ml_strongest_rules(g_app.trainer->model, g_app.trainer->rules, 10);
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

    printf("\n=== %llu api calls  write=%llu read-like=%llu  skip=%llu drop=%llu ===\n",
           (unsigned long long)total,
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
// Subscriber + worker
// ----------------------------------------------------------------------------

static void
on_event(xpc_object_t evt, void *user_data)
{
    (void)user_data;
    if (xpc_get_type(evt) != XPC_TYPE_DICTIONARY) return;
    if (xpc_dictionary_get_uint64(evt, CRAYON_SVC_KEY_EVENT_TYPE)
        != CRAYON_WH_EVENT_NORMALIZED) return;
    const char *kind = xpc_dictionary_get_string(evt, "kind");
    // Filter at the cheap-string-compare level on the foreign thread so
    // we don't waste a queue slot or a JSON walk on irrelevant events.
    if (!kind || strncmp(kind, "attribution.api_call", 20) != 0) return;
    xpc_object_t copy = xpc_copy(evt);
    if (!copy) return;
    queue_push(copy);
}

static void *
worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        xpc_object_t evt = queue_pop_blocking();
        if (!evt) break;
        n00b_json_node_t *event_json = crayon_xpc_to_json(evt);
        xpc_release(evt);

        const char *crud = json_str(json_obj_get(event_json, "crud"));
        if (!crud) {
            atomic_fetch_add(&g_app.skipped, 1);
            continue;
        }
        bool label = is_write_crud(crud);

        n00b_ml_input_reset(g_app.sample);
        if (!project_api_event(g_app.sample, &g_app.ids, event_json)) {
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
            "Usage: %s [--save PATH] [--load PATH] [--every N]\n"
            "  --save PATH   Write the trainer blob to PATH on Ctrl-C.\n"
            "  --load PATH   Refine an existing model from PATH.\n"
            "  --every N     Print status every N events (default 100).\n",
            argv0);
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--save")  && i + 1 < argc) g_save_path = argv[++i];
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
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("GEOM"), &g_app.ids.geom);
        n00b_ml_lookup_rule_group(base->rules, n00b_string_from_cstr("ENV"),  &g_app.ids.env);
        n00b_ml_rules_track_matches(base->rules);
        g_app.sample = n00b_ml_input_new(base->rules);
        fprintf(stderr,
                "crayon_api_crud: refining write-classifier from %s\n",
                g_load_path);
    } else {
        g_app.trainer = n00b_ml_trainer_new(.learning_rate = 0.1f, .weight_decay = 1e-4f);
        g_app.ids     = api_register_subspaces(g_app.trainer);
        n00b_ml_rules_track_matches(g_app.trainer->rules);
        g_app.sample  = n00b_ml_input_new(g_app.trainer->rules);
        fprintf(stderr,
                "crayon_api_crud: bootstrapping write-classifier — save with --save PATH\n");
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    n00b_result_t(n00b_thread_t *) tr = n00b_thread_spawn(worker_main, NULL);
    if (n00b_result_is_err(tr)) {
        fprintf(stderr, "crayon_api_crud: failed to spawn worker\n");
        return 1;
    }
    n00b_thread_t *worker = n00b_result_get(tr);

    crayon_subscriber_t *sub = crayon_subscriber_open(on_event, NULL);
    if (!sub) {
        fprintf(stderr, "crayon_api_crud: cannot open warehouse subscription\n");
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
        fprintf(stderr, "crayon_api_crud: wrote %zu bytes to %s\n",
                (size_t)blob->byte_len, g_save_path);
    }
    return 0;
}

#else

#include <stdio.h>
int main(void) {
    fprintf(stderr, "crayon_api_crud is macOS-only.\n");
    return 1;
}

#endif // __APPLE__
