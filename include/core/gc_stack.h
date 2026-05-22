/**
 * @file gc_stack.h
 * @brief Compiler-published exact stack roots for the GC.
 *
 * The default collector path remains conservative.  Code generated or
 * instrumented with stack maps can publish a per-thread frame chain so the
 * collector can scan only declared root slots at STW safepoints.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"

typedef enum {
    N00B_GC_STACK_CONSERVATIVE = 0,
    N00B_GC_STACK_EXACT_WITH_FALLBACK,
    N00B_GC_STACK_EXACT_ONLY,
} n00b_gc_stack_policy_t;

typedef struct {
    uint32_t root_index;
    uint32_t num_words;
} n00b_gc_stack_slot_t;

typedef struct {
    uint32_t                    num_roots;
    uint32_t                    num_slots;
    uint32_t                    flags;
    const n00b_gc_stack_slot_t *slots;
    const char                 *function_name;
    const char                 *file_name;
    uint32_t                    line;
} n00b_gc_stack_map_t;

typedef struct n00b_gc_stack_frame_t {
    struct n00b_gc_stack_frame_t *prev;
    const n00b_gc_stack_map_t    *map;
    void                        **roots;
} n00b_gc_stack_frame_t;

extern n00b_gc_stack_policy_t n00b_gc_stack_get_policy(void);
extern n00b_gc_stack_policy_t n00b_gc_stack_set_policy(n00b_gc_stack_policy_t policy);
extern void
n00b_gc_stack_push(n00b_gc_stack_frame_t *frame, const n00b_gc_stack_map_t *map, void **roots);
extern void n00b_gc_stack_pop(n00b_gc_stack_frame_t *frame);
