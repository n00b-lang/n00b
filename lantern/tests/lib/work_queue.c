#include "core.h"
#include "lib/diagnostic.h"
#include "lib/work_queue.h"
#include "harness.h"

static void *
worker(void *arg)
{
    core_work_queue_t *q = arg;
    core_work_job_t *job;
    while (core_work_take(q, &job))
        core_work_complete(q, job, true, nullptr);
    return nullptr;
}

typedef struct {
    core_work_queue_t *queue;
    uint32_t kind;
} kind_worker_t;

static void *
worker_kind(void *arg)
{
    kind_worker_t *worker = arg;
    core_work_job_t *job;
    while (core_work_take_kind(worker->queue, worker->kind, &job))
        core_work_complete(worker->queue, job, true, nullptr);
    return nullptr;
}

int
main(void)
{
    core_work_queue_t *q = core_work_queue_new();
    core_work_job_t *lex = nullptr;
    core_work_job_t *pp = nullptr;
    core_work_job_t *analysis = nullptr;
    core_module_spec_t module = {0};
    core_module_spec_init(&module, CORE_STRING_STATIC("pkg.module"));

    check("the first request is submitted",
          core_work_submit(q, 1, &module, nullptr, 0, &lex)
              == CORE_WORK_SUBMITTED);
    core_work_job_t *same = nullptr;
    check("an identical request reuses the in-flight job",
          core_work_submit(q, 1, &module, nullptr, 0, &same)
                  == CORE_WORK_CACHED
              && same == lex);
    core_work_job_t *deps[] = {lex};
    check("a dependent phase is submitted",
          core_work_submit(q, 2, &module, deps, 1, &pp)
              == CORE_WORK_SUBMITTED);
    core_work_job_t *pp_deps[] = {pp};
    check("analysis waits for preprocessing",
          core_work_submit(q, 3, &module, pp_deps, 1, &analysis)
              == CORE_WORK_SUBMITTED);

    core_work_job_t *taken = nullptr;
    check("only the prerequisite is initially ready",
          core_work_take(q, &taken) && taken == lex);
    int value = 42;
    check("the prerequisite completes", core_work_complete(q, lex, true, &value));
    check("preprocessing becomes ready",
          core_work_take(q, &taken) && taken == pp);
    check("preprocessing completes", core_work_complete(q, pp, true, nullptr));
    check("analysis becomes ready", core_work_take(q, &taken) && taken == analysis);
    check("analysis completes",
          core_work_complete(q, analysis, true, nullptr));
    check("wait observes success", core_work_wait(q, analysis));
    check("the cached result is visible", core_work_job_result(q, lex) == &value);

    core_work_job_t *bad = nullptr;
    core_work_job_t *child = nullptr;
    core_work_job_t *grandchild = nullptr;
    check("a separate root can be queued",
          core_work_submit(q, 4, &module, nullptr, 0, &bad)
              == CORE_WORK_SUBMITTED);
    core_work_job_t *bad_deps[] = {bad};
    check("a child can depend on it",
          core_work_submit(q, 5, &module, bad_deps, 1,
                           &child) == CORE_WORK_SUBMITTED);
    core_work_job_t *child_deps[] = {child};
    check("a grandchild can depend on the child",
          core_work_submit(q, 6, &module, child_deps, 1,
                           &grandchild) == CORE_WORK_SUBMITTED);
    check("failed work is taken", core_work_take(q, &taken) && taken == bad);
    check("failure completes", core_work_complete(q, bad, false, nullptr));
    check("failure propagates through dependent phases",
          core_work_job_state(q, child) == CORE_WORK_FAILED
              && core_work_job_state(q, grandchild) == CORE_WORK_FAILED
              && !core_work_wait(q, grandchild));
    core_work_queue_close(q);
    check("no runnable jobs remain", !core_work_take(q, &taken));

    core_work_queue_free(q);
    core_module_spec_release(&module);

    q = core_work_queue_new();
    core_module_spec_init(&module, CORE_STRING_STATIC("pkg.phases"));
    core_work_job_t *lex_phase = nullptr;
    core_work_job_t *parse_phase = nullptr;
    check("phase-filtered workers can share one dependency queue",
          core_work_submit(q, 10, &module, nullptr, 0, &lex_phase) ==
                  CORE_WORK_SUBMITTED &&
              core_work_submit(q, 20, &module, (core_work_job_t *[]){lex_phase},
                               1, &parse_phase) == CORE_WORK_SUBMITTED);
    kind_worker_t lexical_worker = {.queue = q, .kind = 10};
    pthread_t lexical_thread;
    bool worker_started = pthread_create(&lexical_thread, nullptr,
                                         worker_kind, &lexical_worker) == 0;
    if (!worker_started && core_work_take(q, &taken))
        core_work_complete(q, taken, true, nullptr);
    core_work_job_t *taken_parse = nullptr;
    bool parse_taken = core_work_take_kind(q, 20, &taken_parse) &&
                       taken_parse == parse_phase;
    check("the matching phase wakes after its dependency completes",
          parse_taken && core_work_complete(q, parse_phase, true, nullptr));
    core_work_queue_close(q);
    if (worker_started) pthread_join(lexical_thread, nullptr);
    check("filtered workers complete their assigned phase",
          worker_started && core_work_wait(q, lex_phase) &&
              core_work_wait(q, parse_phase));
    core_work_queue_free(q);
    core_module_spec_release(&module);

    q = core_work_queue_new();
    pthread_t workers[2];
    pthread_create(&workers[0], nullptr, worker, q);
    pthread_create(&workers[1], nullptr, worker, q);
    lex = nullptr;
    pp = nullptr;
    analysis = nullptr;
    core_module_spec_init(&module, CORE_STRING_STATIC("pkg.module"));
    core_work_submit(q, 1, &module, nullptr, 0, &lex);
    check("a running worker finishes an early phase", core_work_wait(q, lex));
    core_work_job_t *late_deps[] = {lex};
    core_work_submit(q, 2, &module, late_deps, 1, &pp);
    core_work_job_t *late_analysis_deps[] = {pp};
    core_work_submit(q, 3, &module, late_analysis_deps, 1, &analysis);
    core_work_queue_close(q);
    pthread_join(workers[0], nullptr);
    pthread_join(workers[1], nullptr);
    check("workers remain available for later discovered phases",
          core_work_wait(q, analysis));
    core_work_queue_free(q);
    core_module_spec_release(&module);

    q = core_work_queue_new();
    core_module_spec_t fatal_module = {0};
    core_source_file_t source = {0};
    core_translation_unit_t unit = {0};
    core_module_spec_init(&fatal_module,
                          CORE_STRING_STATIC("pkg.fatal_diagnostic"));
    core_source_file_init(&source, CORE_STRING_STATIC("fatal.n00b"), "", 0);
    bool unit_initialized = core_translation_unit_init(&unit, &fatal_module,
                                                       &source);
    core_work_job_t *phase = nullptr;
    core_work_job_t *dependent = nullptr;
    core_work_job_t *phase_deps[] = {phase};
    check("fatal-diagnostic test TU initializes", unit_initialized);
    check("phase with TU is submitted",
          core_work_submit(q, 30, &fatal_module, nullptr, 0, &phase) ==
              CORE_WORK_SUBMITTED);
    phase_deps[0] = phase;
    check("dependent TU phase is submitted",
          core_work_submit(q, 31, &fatal_module, phase_deps, 1, &dependent) ==
              CORE_WORK_SUBMITTED);
    bool phase_taken = core_work_take(q, &taken) && taken == phase;
    check("fatal-diagnostic phase starts", phase_taken);
    core_diagnostics_report(core_translation_unit_diagnostics(&unit), &unit,
        CORE_DIAG_FATAL, CORE_STRING_STATIC("TEST-F0001"),
        CORE_STRING_STATIC("fatal compilation diagnostic"));
    check("TU-aware completion propagates its fatal diagnostic",
          unit_initialized && core_work_complete_tu(q, phase, &unit, true)
              && core_work_job_state(q, dependent) == CORE_WORK_FAILED
              && !core_work_wait(q, dependent));
    core_work_queue_close(q);
    check("fatal dependency chain has no runnable work",
          !core_work_take(q, &taken));
    core_work_queue_free(q);
    core_translation_unit_release(&unit);
    core_module_spec_release(&fatal_module);

    return report("lib-work-queue");
}
