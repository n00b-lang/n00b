#ifndef IR_PDG_H
#define IR_PDG_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "lib/string.h"

typedef enum : uint8_t {
    IR_DEP_CONTROL,
    IR_DEP_DATA,
} ir_dep_kind_t;

typedef enum : uint8_t {
    IR_DEP_EXACT,
    IR_DEP_MAY,
} ir_dep_certainty_t;

typedef struct {
    ir_instr_t        *from;
    ir_instr_t        *to;
    ir_dep_kind_t      kind;
    ir_dep_certainty_t certainty;
    ir_local_t        *via;
} ir_dep_edge_t;

typedef struct ir_pdg_t ir_pdg_t;

ir_pdg_t *ir_pdg_of(ir_func_t *f);
void      ir_pdg_free(ir_pdg_t *g);

uint32_t             ir_pdg_node_count(const ir_pdg_t *g);
ir_instr_t          *ir_pdg_node_at(const ir_pdg_t *g, uint32_t i);
uint32_t             ir_pdg_edge_count(const ir_pdg_t *g);
const ir_dep_edge_t *ir_pdg_edge_at(const ir_pdg_t *g, uint32_t i);

uint32_t             ir_pdg_out_count(const ir_pdg_t *g, ir_instr_t *in);
const ir_dep_edge_t *ir_pdg_out_at(const ir_pdg_t *g, ir_instr_t *in,
                                   uint32_t i);
uint32_t             ir_pdg_in_count(const ir_pdg_t *g, ir_instr_t *in);
const ir_dep_edge_t *ir_pdg_in_at(const ir_pdg_t *g, ir_instr_t *in,
                                  uint32_t i);

uint32_t    ir_controllers_of(const ir_pdg_t *g, ir_block_t *b);
ir_block_t *ir_controller_at(const ir_pdg_t *g, ir_block_t *b, uint32_t i);
uint32_t    ir_controller_slot_at(const ir_pdg_t *g, ir_block_t *b,
                                  uint32_t i);

bool ir_pdg_is_exact(const ir_pdg_t *g);

typedef enum : uint8_t {
    IR_EDGE_FALLTHROUGH,
    IR_EDGE_BRANCH_TRUE,
    IR_EDGE_BRANCH_FALSE,
    IR_EDGE_LOOP_BACK,
    IR_EDGE_LOOP_EXIT,
    IR_EDGE_JUMP,
    IR_EDGE_CASE,
} ir_edge_kind_t;

core_string_t  ir_edge_kind_name(ir_edge_kind_t k);
ir_edge_kind_t ir_edge_kind(const ir_pdg_t *g, ir_block_t *from,
                            uint32_t slot);

#endif
