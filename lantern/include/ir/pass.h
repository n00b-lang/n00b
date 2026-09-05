#ifndef IR_PASS_H
#define IR_PASS_H

#include "ir/diag.h"
#include "ir/ir.h"

ir_instr_t *ir_instr_insert(ir_block_t *b, ir_op_t op, uint32_t at);
ir_span_t   ir_span_cover(ir_span_t a, ir_span_t b);
ir_instr_t *ir_instr_derived(ir_block_t *b, ir_op_t op, const ir_instr_t *from,
                             uint32_t at);
ir_instr_t *ir_instr_generated(ir_block_t *b, ir_op_t op, ir_span_t near,
                               const char *pass, uint32_t at);
bool        ir_instr_move(ir_instr_t *in, ir_block_t *to, uint32_t at);
bool        ir_instr_inlined_from(ir_instr_t *in, ir_span_t call_site,
                                  const char *agent);

typedef const char *(*ir_rename_fn)(ir_ctx_t *ctx, const char *local,
                                    uint32_t nth, void *state);

typedef struct {
    ir_rename_fn rename;
    void        *state;
    ir_diags_t  *diags;
} ir_promote_opts_t;

bool ir_promote(ir_module_t *m, const ir_promote_opts_t *opts);
bool ir_split_edges(ir_module_t *m, ir_diags_t *d);

extern const ir_pass_t ir_pass_promote;
extern const ir_pass_t ir_pass_split_edges;

typedef enum : uint8_t {
    IR_REGION_SEQ,
    IR_REGION_IF,
    IR_REGION_LOOP,
    IR_REGION_SWITCH,
    IR_REGION_BLOCK,
} ir_region_kind_t;

struct ir_region_t {
    ir_region_kind_t kind;
    ir_block_t      *head;
    ir_region_t    **kids;
    uint32_t         n_kids;
    uint32_t         cap_kids;
    ir_region_t     *parent;
    ir_span_t        span;
    bool             dispatch;
};

const char  *ir_region_kind_name(ir_region_kind_t k);
bool         ir_structure(ir_module_t *m, ir_diags_t *d);
ir_region_t *ir_region_root(ir_func_t *f);
ir_region_t *ir_region_of_block(ir_func_t *f, ir_block_t *b);
bool         ir_region_needs_dispatch(const ir_region_t *r);
ir_region_t *ir_region_dispatch_site(ir_func_t *f);

char *ir_module_render_nested(ir_module_t *m);

extern const ir_pass_t ir_pass_structure;

#endif
