#ifndef IR_PDG_MEMORY_H
#define IR_PDG_MEMORY_H

#include "ir/ir.h"
#include "ir/doms.h"
#include "ir/places.h"

typedef enum : uint8_t {
    IR_MEMDEF_ENTRY,
    IR_MEMDEF_STORE,
    IR_MEMDEF_CALL,
    IR_MEMDEF_PHI,
    IR_MEMDEF_ELEMENT_PHI,
} ir_memdef_kind_t;

typedef struct {
    ir_memdef_kind_t   kind;
    ir_instr_t        *instr;
    const ir_access_t *access;
    ir_block_t        *block;
    uint32_t           element_base;
    const ir_value_t  *element_index;
    uint32_t           memory_phi;
} ir_memdef_t;

typedef struct ir_pdg_memory_t ir_pdg_memory_t;

typedef bool (*ir_memdef_visit_fn)(void *context, const ir_memdef_t *def);

ir_pdg_memory_t *ir_pdg_memory_build(ir_func_t *f, ir_doms_t *doms);
void         ir_pdg_memory_free(ir_pdg_memory_t *memory);

bool ir_pdg_memory_visit_clobbers(ir_pdg_memory_t *memory, uint32_t load_access,
                              ir_memdef_visit_fn visit, void *context);
bool ir_pdg_memory_reads_entry(ir_pdg_memory_t *memory, uint32_t load_access);

typedef enum : uint8_t {
    IR_ELEMENT_DISJOINT,
    IR_ELEMENT_SAME,
    IR_ELEMENT_MAYBE,
} ir_element_relation_t;

typedef ir_element_relation_t (*ir_element_relation_fn)(
    void *context, const ir_instr_t *store, const ir_instr_t *load);

bool ir_pdg_memory_visit_element_defs(ir_pdg_memory_t *memory, uint32_t load_access,
                                  ir_element_relation_fn relate,
                                  ir_memdef_visit_fn visit, void *context);

bool               ir_pdg_memory_defines(const ir_pdg_memory_t *memory,
                                         const ir_instr_t *in);
uint32_t           ir_pdg_memory_def_before(const ir_pdg_memory_t *memory,
                                        const ir_instr_t *in);
uint32_t           ir_pdg_memory_load_def(const ir_pdg_memory_t *memory,
                                      uint32_t load_access);
uint32_t           ir_pdg_memory_def_count(const ir_pdg_memory_t *memory);
const ir_memdef_t *ir_pdg_memory_def(const ir_pdg_memory_t *memory, uint32_t id);
uint32_t           ir_pdg_memory_def_prev(const ir_pdg_memory_t *memory, uint32_t id);
uint32_t           ir_pdg_memory_phi_count(const ir_pdg_memory_t *memory,
                                       uint32_t id);
uint32_t           ir_pdg_memory_phi_incoming(const ir_pdg_memory_t *memory,
                                          uint32_t id, uint32_t index);

bool     ir_pdg_memory_element_of(const ir_instr_t *in, const ir_value_t **base,
                              const ir_value_t **index);
uint32_t ir_pdg_memory_access_of(const ir_pdg_memory_t *memory, const ir_instr_t *in,
                             ir_access_kind_t kind);
uint32_t ir_pdg_memory_element_link_count(const ir_pdg_memory_t *memory,
                                      uint32_t load_access);
uint32_t ir_pdg_memory_element_link(const ir_pdg_memory_t *memory,
                                uint32_t load_access, uint32_t index);
ir_element_relation_t ir_pdg_memory_element_relation(const ir_pdg_memory_t *memory,
                                                 uint32_t def,
                                                 uint32_t load_access);
bool     ir_pdg_memory_verify(ir_pdg_memory_t *memory);

typedef bool (*ir_pdg_memory_edge_fn)(void *context, ir_instr_t *from,
                                      bool exact);

bool ir_pdg_memory_load_sources(ir_pdg_memory_t *memory, uint32_t load_access,
                                ir_pdg_memory_edge_fn emit, void *context);
uint32_t ir_pdg_memory_slot_def(ir_pdg_memory_t *memory, uint32_t load_access);
bool     ir_pdg_memory_slot_stores(ir_pdg_memory_t *memory, uint32_t load_access,
                                   ir_memdef_visit_fn visit, void *context);
bool ir_pdg_memory_call_sources(ir_pdg_memory_t *memory,
                                const ir_instr_t *call,
                                ir_pdg_memory_edge_fn emit, void *context);
bool ir_pdg_memory_store_observed(ir_pdg_memory_t *memory,
                                  const ir_instr_t *store, uint32_t place);

#endif
