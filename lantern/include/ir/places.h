#ifndef IR_PLACES_H
#define IR_PLACES_H

#include "ir/ir.h"

typedef enum : uint8_t {
    IR_PLACE_LOCAL,
    IR_PLACE_PARAM,
    IR_PLACE_GLOBAL,
    IR_PLACE_THREAD,
    IR_PLACE_HEAP,
    IR_PLACE_UNKNOWN,
} ir_place_kind_t;

typedef struct {
    ir_place_kind_t   kind;
    const ir_value_t *root;
    th_type          *type;
    bool              exposed;
} ir_place_t;

typedef enum : uint8_t {
    IR_ACCESS_LOAD,
    IR_ACCESS_STORE,
} ir_access_kind_t;

typedef struct {
    ir_instr_t      *instr;
    ir_access_kind_t kind;
    bool             whole;
    bool             direct;
    uint32_t         first;
    uint32_t         count;
} ir_access_t;

typedef struct ir_places_t ir_places_t;

ir_places_t       *ir_places_build(ir_func_t *f);
ir_places_t       *ir_func_places(ir_func_t *f);
uint32_t           ir_places_count(const ir_places_t *places);
const ir_place_t  *ir_place_at(const ir_places_t *places, uint32_t index);
bool               ir_places_of(ir_places_t *places, const ir_value_t *address,
                                const uint32_t **ids, uint32_t *count);
bool               ir_places_may_alias(const ir_places_t *places, uint32_t a,
                                       uint32_t b);
uint32_t           ir_places_access_count(const ir_places_t *places);
const ir_access_t *ir_places_access_at(const ir_places_t *places,
                                       uint32_t index);
uint32_t           ir_places_accesses_of(const ir_places_t *places,
                                         const ir_instr_t *in,
                                         uint32_t *first);
const uint32_t    *ir_places_access_places(const ir_places_t *places,
                                           const ir_access_t *access);
bool               ir_places_access_private(const ir_places_t *places,
                                            const ir_access_t *access);
void               ir_places_free(ir_places_t *places);

void               ir_reinterpretations_open(ir_ctx_t *ctx);
bool               ir_reinterpretations_note_function(ir_ctx_t *ctx,
                                                      const ir_func_t *f);
void               ir_reinterpretations_note_unseen(ir_ctx_t *ctx);
void               ir_reinterpretations_seal(ir_ctx_t *ctx);
bool               ir_places_types_overlap(ir_module_t *module, th_type *a,
                                           th_type *b);
#endif
