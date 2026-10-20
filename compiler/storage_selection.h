#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lib/string.h"

typedef enum {
    N00BB_STORAGE_KEEP,
    N00BB_STORAGE_EXPLICIT,
    N00BB_STORAGE_RUNTIME_DYNAMIC,
    N00BB_STORAGE_INVALID_TARGET,
    N00BB_STORAGE_MISSING_CLASS,
    N00BB_STORAGE_CONFLICT,
} n00bb_storage_decision_t;

typedef struct {
    bool is_typed_variable;
    bool has_lexical_lifetime;
    bool is_reference;
    bool required;
    core_string_t requested;
    core_string_t selected;
} n00bb_storage_facts_t;

n00bb_storage_decision_t n00bb_storage_decide(
    const n00bb_storage_facts_t *facts, bool *dynamic_duration);

typedef struct {
    bool has_lexical_lifetime;
    bool is_reference;
    bool size_known;
    uint64_t size_bytes;
    uint64_t stack_limit_bytes;
} n00bb_storage_placement_facts_t;

n00bb_storage_decision_t n00bb_storage_choose_placement(
    const n00bb_storage_placement_facts_t *facts,
    bool *dynamic_duration);
