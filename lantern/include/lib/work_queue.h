#pragma once

#include <pthread.h>

#include "core.h"
#include "lib/module.h"

typedef struct core_work_queue_t core_work_queue_t;
typedef struct core_work_job_t core_work_job_t;

typedef enum {
    CORE_WORK_PENDING,
    CORE_WORK_READY,
    CORE_WORK_RUNNING,
    CORE_WORK_SUCCEEDED,
    CORE_WORK_FAILED,
} core_work_state_t;

typedef enum {
    CORE_WORK_SUBMITTED,
    CORE_WORK_CACHED,
    CORE_WORK_INVALID,
    CORE_WORK_DEPENDENCY_FAILED,
} core_work_submit_result_t;

core_work_queue_t *core_work_queue_new(void);
void core_work_queue_free(core_work_queue_t *q);
void core_work_queue_close(core_work_queue_t *q);

core_work_submit_result_t core_work_submit(
    core_work_queue_t *q, uint32_t kind, const core_module_spec_t *module,
    uint64_t input_revision,
    core_work_job_t *const *dependencies, size_t dependency_count,
    core_work_job_t **out);

bool core_work_take(core_work_queue_t *q, core_work_job_t **out);
bool core_work_take_kind(core_work_queue_t *q, uint32_t kind,
                         core_work_job_t **out);
bool core_work_complete(core_work_queue_t *q, core_work_job_t *job,
                        bool succeeded, void *result);
bool core_work_complete_tu(core_work_queue_t *q, core_work_job_t *job,
                          core_translation_unit_t *unit, bool succeeded);
bool core_work_wait(core_work_queue_t *q, core_work_job_t *job);

uint32_t core_work_job_kind(const core_work_job_t *job);
const core_module_spec_t *core_work_job_module(const core_work_job_t *job);
core_work_state_t core_work_job_state(core_work_queue_t *q,
                                      const core_work_job_t *job);
void *core_work_job_result(core_work_queue_t *q, const core_work_job_t *job);
