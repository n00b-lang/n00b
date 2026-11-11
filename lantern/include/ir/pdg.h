#ifndef IR_PDG_H
#define IR_PDG_H

#include "ir/diag.h"
#include "ir/doms.h"
#include "ir/ir.h"
#include "ir/pdg_memory.h"
#include "lib/string.h"

#define IR_COMPTIME_GLOBAL "n00b_comptime"
#define IR_COMPTIME_FROZEN "n00b_comptime_frozen"

typedef enum : uint8_t {
    IR_DEP_CONTROL,
    IR_DEP_DATA,
    IR_DEP_ARGUMENT,
    IR_DEP_RETURN,
    IR_DEP_FAILURE,
    IR_DEP_MEMORY,
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
    ir_value_t        *param;
} ir_dep_edge_t;

typedef struct ir_pdg_t ir_pdg_t;

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
uint32_t             ir_pdg_param_out_count(const ir_pdg_t *g,
                                            const ir_value_t *param);
const ir_dep_edge_t *ir_pdg_param_out_at(const ir_pdg_t *g,
                                         const ir_value_t *param,
                                         uint32_t i);

uint32_t    ir_controllers_of(const ir_pdg_t *g, ir_block_t *b);
ir_block_t *ir_controller_at(const ir_pdg_t *g, ir_block_t *b, uint32_t i);
uint32_t    ir_controller_slot_at(const ir_pdg_t *g, ir_block_t *b,
                                  uint32_t i);

bool ir_pdg_is_exact(const ir_pdg_t *g);
ir_doms_t *ir_pdg_doms(const ir_pdg_t *g);
ir_pdg_memory_t *ir_pdg_memory(const ir_pdg_t *g);
uint32_t ir_pdg_local_version(const ir_pdg_t *g, const ir_instr_t *read,
                              const ir_local_t *local);

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

ir_value_t *ir_call_param(ir_func_t *target, const ir_operand_t *o,
                          uint32_t *position, uint32_t *type_index);

typedef void (*ir_pdg_target_fn)(void *sink, ir_func_t *target);

typedef struct {
    void *context;
    bool (*call_targets)(void *context, const ir_instr_t *call,
                         ir_pdg_target_fn add, void *sink);
    ir_func_t *comptime_root;
} ir_pdg_hooks_t;

typedef struct ir_group_pdg_t ir_group_pdg_t;

ir_group_pdg_t *ir_group_pdg_of(ir_func_t *const *functions, size_t count,
                                const ir_pdg_hooks_t *hooks);
void            ir_group_pdg_free(ir_group_pdg_t *g);

ir_pdg_t *ir_group_pdg_func(const ir_group_pdg_t *g, const ir_func_t *f);

uint32_t             ir_group_pdg_edge_count(const ir_group_pdg_t *g);
const ir_dep_edge_t *ir_group_pdg_edge_at(const ir_group_pdg_t *g,
                                          uint32_t i);
uint32_t             ir_group_pdg_out_count(const ir_group_pdg_t *g,
                                            const ir_instr_t *in);
const ir_dep_edge_t *ir_group_pdg_out_at(const ir_group_pdg_t *g,
                                         const ir_instr_t *in, uint32_t i);
uint32_t             ir_group_pdg_in_count(const ir_group_pdg_t *g,
                                           const ir_instr_t *in);
const ir_dep_edge_t *ir_group_pdg_in_at(const ir_group_pdg_t *g,
                                        const ir_instr_t *in, uint32_t i);
uint32_t             ir_group_pdg_param_in_count(const ir_group_pdg_t *g,
                                                 const ir_value_t *param);
const ir_dep_edge_t *ir_group_pdg_param_in_at(const ir_group_pdg_t *g,
                                              const ir_value_t *param,
                                              uint32_t i);

uint32_t   ir_group_pdg_callee_count(const ir_group_pdg_t *g,
                                     const ir_func_t *f);
ir_func_t *ir_group_pdg_callee_at(const ir_group_pdg_t *g,
                                  const ir_func_t *f, uint32_t i);

uint32_t   ir_group_pdg_component_count(const ir_group_pdg_t *g);
uint32_t   ir_group_pdg_component_size(const ir_group_pdg_t *g, uint32_t c);
ir_func_t *ir_group_pdg_component_at(const ir_group_pdg_t *g, uint32_t c,
                                     uint32_t i);
bool       ir_group_pdg_component_recursive(const ir_group_pdg_t *g,
                                            uint32_t c);
uint32_t   ir_group_pdg_component_of(const ir_group_pdg_t *g,
                                     const ir_func_t *f);

bool ir_pdg_reads_comptime(const ir_instr_t *in);
bool ir_pdg_asks_frozen(const ir_instr_t *in);

#endif
