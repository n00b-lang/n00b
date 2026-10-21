#ifndef IR_RACES_H
#define IR_RACES_H

#include "ir/summary.h"

typedef enum : int8_t {
    IR_LOCK_RELEASE = -1,
    IR_LOCK_NONE    = 0,
    IR_LOCK_ACQUIRE = 1,
    IR_LOCK_SYNC    = 2,
} ir_lock_role_t;

typedef struct {
    void *context;
    bool (*atomic)(void *context, const ir_instr_t *in,
                   const ir_splace_t *place);
    ir_lock_role_t (*lock_role)(void *context, const ir_instr_t *in);
    ir_func_t *(*direct_target)(void *context, const ir_instr_t *in);
} ir_race_hooks_t;

typedef struct {
    ir_func_t         *entry;
    uint32_t           starts;
    const ir_splace_t *arguments;
    uint32_t           argument_count;
} ir_race_entry_t;

typedef struct {
    const ir_func_t  *entry;
    const ir_instr_t *site;
    ir_splace_t       place;
    bool              self;
} ir_race_t;

uint32_t ir_races_find(ir_summaries_t *summaries,
                       ir_func_t *const *main_roots, size_t main_count,
                       const ir_race_entry_t *entries, size_t entry_count,
                       const ir_race_hooks_t *hooks, ir_race_t *out,
                       uint32_t capacity);

#endif
