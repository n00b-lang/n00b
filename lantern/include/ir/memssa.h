#ifndef IR_MEMSSA_H
#define IR_MEMSSA_H

#include "ir/ir.h"
#include "ir/places.h"

typedef enum : uint8_t {
    IR_MEMDEF_ENTRY,
    IR_MEMDEF_STORE,
    IR_MEMDEF_CALL,
    IR_MEMDEF_PHI,
} ir_memdef_kind_t;

typedef struct {
    ir_memdef_kind_t   kind;
    ir_instr_t        *instr;
    const ir_access_t *access;
    ir_block_t        *block;
} ir_memdef_t;

typedef struct ir_memssa_t ir_memssa_t;

typedef bool (*ir_memdef_visit_fn)(void *context, const ir_memdef_t *def);

ir_memssa_t *ir_memssa_build(ir_func_t *f);
ir_memssa_t *ir_func_memssa(ir_func_t *f);
void         ir_memssa_free(ir_memssa_t *memssa);

bool ir_memssa_visit_clobbers(ir_memssa_t *memssa, uint32_t load_access,
                              ir_memdef_visit_fn visit, void *context);
bool ir_memssa_reads_entry(ir_memssa_t *memssa, uint32_t load_access);

typedef enum : uint8_t {
    IR_ELEMENT_DISJOINT,
    IR_ELEMENT_SAME,
    IR_ELEMENT_MAYBE,
} ir_element_relation_t;

typedef ir_element_relation_t (*ir_element_relation_fn)(
    void *context, const ir_instr_t *store, const ir_instr_t *load);

bool ir_memssa_visit_element_defs(ir_memssa_t *memssa, uint32_t load_access,
                                  ir_element_relation_fn relate,
                                  ir_memdef_visit_fn visit, void *context);

uint32_t           ir_memssa_def_before(const ir_memssa_t *memssa,
                                        const ir_instr_t *in);
uint32_t           ir_memssa_load_def(const ir_memssa_t *memssa,
                                      uint32_t load_access);
uint32_t           ir_memssa_def_count(const ir_memssa_t *memssa);
const ir_memdef_t *ir_memssa_def(const ir_memssa_t *memssa, uint32_t id);
uint32_t           ir_memssa_def_prev(const ir_memssa_t *memssa, uint32_t id);
uint32_t           ir_memssa_phi_count(const ir_memssa_t *memssa,
                                       uint32_t id);
uint32_t           ir_memssa_phi_incoming(const ir_memssa_t *memssa,
                                          uint32_t id, uint32_t index);

#endif
