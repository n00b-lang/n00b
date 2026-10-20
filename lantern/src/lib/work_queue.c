#include "lib/work_queue.h"

#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef struct {
    uint32_t kind;
    core_module_spec_t module;
    uint64_t input_revision;
    uint32_t variant;
} work_key_t;

typedef core_work_job_t *core_work_job_ptr_t;
core_list_decl(core_work_job_ptr_t);

typedef struct ready_kind_t {
    core_work_job_t *head;
    core_work_job_t *tail;
} ready_kind_t;

struct core_work_job_t {
    work_key_t key;
    core_work_state_t state;
    void *result;
    size_t remaining;
    core_list_t(core_work_job_ptr_t) dependents;
    core_list_t(core_work_job_ptr_t) dependencies;
    core_dict_t dependency_index;
    ready_kind_t *ready_kind;
    core_work_job_t *ready_prev;
    core_work_job_t *ready_next;
    core_work_job_t *kind_prev;
    core_work_job_t *kind_next;
};

struct core_work_queue_t {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    core_dict_t jobs;
    core_dict_t ready_by_kind;
    core_work_job_t *ready_head;
    core_work_job_t *ready_tail;
    size_t terminal;
    bool closed;
};

static core_hash_value_t
work_key_hash(void *p)
{
    work_key_t *key = p;
    core_hash_value_t h = core_hash_raw(&key->kind, sizeof(key->kind));
    h ^= core_hash_raw(key->module.identifier.data,
                       key->module.identifier.u8_bytes);
    h ^= core_hash_raw(&key->input_revision, sizeof(key->input_revision));
    return h ^ core_hash_raw(&key->variant, sizeof(key->variant));
}

static bool
work_key_eq(const void *a, const void *b)
{
    const work_key_t *ka = a;
    const work_key_t *kb = b;
    return ka->kind == kb->kind
        && ka->input_revision == kb->input_revision
        && ka->variant == kb->variant
        && core_module_spec_eq(&ka->module, &kb->module);
}

core_work_queue_t *
core_work_queue_new(void)
{
    core_work_queue_t *q = core_alloc(core_work_queue_t);
    pthread_mutex_init(&q->lock, nullptr);
    pthread_cond_init(&q->changed, nullptr);
    core_dict_init(&q->jobs, work_key_hash, work_key_eq);
    core_dict_init(&q->ready_by_kind, nullptr, nullptr);
    return q;
}

static void
ready_push(core_work_queue_t *q, core_work_job_t *job)
{
    bool found = false;
    ready_kind_t *kind = core_dict_get(&q->ready_by_kind,
                                      (void *)(uintptr_t)job->key.kind,
                                      &found);
    if (!found) {
        kind = core_alloc(ready_kind_t);
        core_dict_add(&q->ready_by_kind,
                      (void *)(uintptr_t)job->key.kind, kind);
    }
    job->ready_kind = kind;
    job->ready_prev = q->ready_tail;
    if (q->ready_tail) q->ready_tail->ready_next = job;
    else q->ready_head = job;
    q->ready_tail = job;
    job->kind_prev = kind->tail;
    if (kind->tail) kind->tail->kind_next = job;
    else kind->head = job;
    kind->tail = job;
}

static void
ready_remove(core_work_queue_t *q, core_work_job_t *job)
{
    ready_kind_t *kind = job->ready_kind;
    if (job->ready_prev) job->ready_prev->ready_next = job->ready_next;
    else q->ready_head = job->ready_next;
    if (job->ready_next) job->ready_next->ready_prev = job->ready_prev;
    else q->ready_tail = job->ready_prev;
    if (job->kind_prev) job->kind_prev->kind_next = job->kind_next;
    else kind->head = job->kind_next;
    if (job->kind_next) job->kind_next->kind_prev = job->kind_prev;
    else kind->tail = job->kind_prev;
    job->ready_kind = nullptr;
    job->ready_prev = job->ready_next = nullptr;
    job->kind_prev = job->kind_next = nullptr;
}

static bool
terminal(core_work_state_t state)
{
    return state == CORE_WORK_SUCCEEDED || state == CORE_WORK_FAILED;
}

static void
fail_dependents(core_work_queue_t *q, core_work_job_t *job)
{
    for (size_t i = 0; i < job->dependents.len; i++) {
        core_work_job_t *next = job->dependents.data[i];
        if (next->state != CORE_WORK_PENDING) continue;
        next->state = CORE_WORK_FAILED;
        q->terminal++;
        fail_dependents(q, next);
    }
}

void
core_work_queue_free(core_work_queue_t *q)
{
    if (!q) return;
    core_dict_foreach (&q->jobs, entry) {
        core_work_job_t *job = entry->value;
        core_list_free(job->dependencies);
        core_list_free(job->dependents);
        core_dict_free(&job->dependency_index);
        core_module_spec_release(&job->key.module);
        core_free(job);
    }
    core_dict_foreach (&q->ready_by_kind, entry)
        core_free(entry->value);
    core_dict_free(&q->ready_by_kind);
    core_dict_free(&q->jobs);
    pthread_cond_destroy(&q->changed);
    pthread_mutex_destroy(&q->lock);
    core_free(q);
}

void
core_work_queue_close(core_work_queue_t *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
}

static bool
same_dependencies(core_work_job_t *job, core_work_job_t *const *deps,
                  size_t count)
{
    if (job->dependencies.len != count) return false;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool same = true;
    for (size_t i = 0; i < count; i++) {
        if (!core_dict_contains(&job->dependency_index, deps[i])
            || !core_dict_add(&seen, deps[i], deps[i])) {
            same = false;
            break;
        }
    }
    core_dict_free(&seen);
    return same;
}

core_work_submit_result_t
core_work_submit(core_work_queue_t *q, uint32_t kind,
                 const core_module_spec_t *module,
                 uint64_t input_revision,
                 core_work_job_t *const *dependencies, size_t dependency_count,
                 core_work_job_t **out)
{
    if (out) *out = nullptr;
    if (!q || !module || !module->identifier.data
        || (!dependencies && dependency_count))
        return CORE_WORK_INVALID;

    pthread_mutex_lock(&q->lock);
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        return CORE_WORK_INVALID;
    }
    work_key_t probe = {
        .kind = kind,
        .module = *module,
        .input_revision = input_revision,
    };
    bool found = false;
    core_work_job_t *existing = nullptr;
    for (;;) {
        existing = _core_dict_get(&q->jobs, &probe, &found);
        if (!found) break;
        if (same_dependencies(existing, dependencies, dependency_count)) {
            if (out) *out = existing;
            pthread_mutex_unlock(&q->lock);
            return CORE_WORK_CACHED;
        }
        if (probe.variant == UINT32_MAX) {
            pthread_mutex_unlock(&q->lock);
            return CORE_WORK_INVALID;
        }
        probe.variant++;
    }

    core_work_job_t *job = core_alloc(core_work_job_t);
    job->key = probe;
    if (!core_module_spec_init(&job->key.module, module->identifier)) {
        core_free(job);
        pthread_mutex_unlock(&q->lock);
        return CORE_WORK_INVALID;
    }
    job->state = CORE_WORK_PENDING;
    job->dependencies = core_list_new(core_work_job_ptr_t);
    job->dependents = core_list_new(core_work_job_ptr_t);
    core_dict_init(&job->dependency_index, nullptr, nullptr);
    for (size_t i = 0; i < dependency_count; i++) {
        core_work_job_t *dep = dependencies[i];
        if (!dep || _core_dict_get(&q->jobs, &dep->key, &found) != dep || !found
            || dep == job) {
            core_list_free(job->dependencies);
            core_list_free(job->dependents);
            core_dict_free(&job->dependency_index);
            core_module_spec_release(&job->key.module);
            core_free(job);
            pthread_mutex_unlock(&q->lock);
            return CORE_WORK_INVALID;
        }
        if (!core_dict_add(&job->dependency_index, dep, dep)) continue;
        core_list_push(job->dependencies, dep);
        if (dep->state == CORE_WORK_FAILED) {
            job->state = CORE_WORK_FAILED;
        }
        else if (dep->state != CORE_WORK_SUCCEEDED) {
            job->remaining++;
        }
    }
    _core_dict_put(&q->jobs, &job->key, job);
    q->terminal += job->state == CORE_WORK_FAILED;
    if (job->state != CORE_WORK_FAILED) {
        for (size_t i = 0; i < job->dependencies.len; i++) {
            core_work_job_t *dep = job->dependencies.data[i];
            if (!terminal(dep->state)) core_list_push(dep->dependents, job);
        }
        if (!job->remaining) {
            job->state = CORE_WORK_READY;
            ready_push(q, job);
        }
    }
    if (out) *out = job;
    pthread_cond_broadcast(&q->changed);
    core_work_submit_result_t result =
        job->state == CORE_WORK_FAILED ? CORE_WORK_DEPENDENCY_FAILED
                                       : CORE_WORK_SUBMITTED;
    pthread_mutex_unlock(&q->lock);
    return result;
}

static bool
work_take(core_work_queue_t *q, bool filter_kind, uint32_t kind,
         core_work_job_t **out)
{
    if (!q || !out) return false;
    pthread_mutex_lock(&q->lock);
    core_work_job_t *job = nullptr;
    for (;;) {
        if (!filter_kind) job = q->ready_head;
        else {
            ready_kind_t *bucket = core_dict_get(&q->ready_by_kind,
                                                (void *)(uintptr_t)kind,
                                                nullptr);
            job = bucket ? bucket->head : nullptr;
        }
        if (job || (q->closed && q->terminal == q->jobs.count))
            break;
        pthread_cond_wait(&q->changed, &q->lock);
    }

    if (!job) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    ready_remove(q, job);
    job->state = CORE_WORK_RUNNING;
    *out = job;
    pthread_mutex_unlock(&q->lock);
    return true;
}

bool
core_work_take(core_work_queue_t *q, core_work_job_t **out)
{
    return work_take(q, false, 0, out);
}

bool
core_work_take_kind(core_work_queue_t *q, uint32_t kind,
                    core_work_job_t **out)
{
    return work_take(q, true, kind, out);
}

bool
core_work_complete(core_work_queue_t *q, core_work_job_t *job, bool succeeded,
                   void *result)
{
    if (!q || !job) return false;
    pthread_mutex_lock(&q->lock);
    bool found = false;
    if (_core_dict_get(&q->jobs, &job->key, &found) != job || !found
        || job->state != CORE_WORK_RUNNING) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    job->state = succeeded ? CORE_WORK_SUCCEEDED : CORE_WORK_FAILED;
    job->result = result;
    q->terminal++;
    for (size_t i = 0; i < job->dependents.len; i++) {
        core_work_job_t *next = job->dependents.data[i];
        if (next->state != CORE_WORK_PENDING) continue;
        if (!succeeded) {
            next->state = CORE_WORK_FAILED;
            q->terminal++;
            fail_dependents(q, next);
        }
        else if (--next->remaining == 0) {
            next->state = CORE_WORK_READY;
            ready_push(q, next);
        }
    }
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
    return true;
}

bool
core_work_complete_tu(core_work_queue_t *q, core_work_job_t *job,
                      core_translation_unit_t *unit, bool succeeded)
{
    return core_work_complete(q, job,
        succeeded && !core_translation_unit_has_fatal_diagnostics(unit), unit);
}

bool
core_work_wait(core_work_queue_t *q, core_work_job_t *job)
{
    if (!q || !job) return false;
    pthread_mutex_lock(&q->lock);
    bool found = false;
    if (_core_dict_get(&q->jobs, &job->key, &found) != job || !found) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    while (!terminal(job->state)) pthread_cond_wait(&q->changed, &q->lock);
    bool ok = job->state == CORE_WORK_SUCCEEDED;
    pthread_mutex_unlock(&q->lock);
    return ok;
}

uint32_t core_work_job_kind(const core_work_job_t *job) { return job ? job->key.kind : 0; }
const core_module_spec_t *core_work_job_module(const core_work_job_t *job) { return job ? &job->key.module : nullptr; }

core_work_state_t
core_work_job_state(core_work_queue_t *q, const core_work_job_t *job)
{
    if (!q || !job) return CORE_WORK_FAILED;
    pthread_mutex_lock(&q->lock);
    bool found = false;
    core_work_state_t state = _core_dict_get(&q->jobs, &((core_work_job_t *)job)->key,
                                              &found) == job && found
                                  ? job->state : CORE_WORK_FAILED;
    pthread_mutex_unlock(&q->lock);
    return state;
}

void *
core_work_job_result(core_work_queue_t *q, const core_work_job_t *job)
{
    if (!q || !job) return nullptr;
    pthread_mutex_lock(&q->lock);
    bool found = false;
    void *result = _core_dict_get(&q->jobs, &((core_work_job_t *)job)->key,
                                  &found) == job && found ? job->result : nullptr;
    pthread_mutex_unlock(&q->lock);
    return result;
}
